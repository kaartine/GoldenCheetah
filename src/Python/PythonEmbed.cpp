/*
 * Copyright (c) 2017 Mark Liversedge (liversedge@gmail.com)
 *
 * Additionally, for the original source used as a basis for this (RInside.cpp)
 * Released under the same GNU public license.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "PythonEmbed.h"
#include "Utils.h"
#include "Settings.h"
#include <stdexcept>
#include <chrono>
#include <type_traits>
#include <utility>

#include <QtGlobal>
#include <QCoreApplication>
#include <QMessageBox>
#include <QProcess>
#include <QThread>

#ifdef slots // clashes with python headers
#undef slots
#endif
#include <Python.h>

#if PY_VERSION_HEX < 0x03080000
#error "GoldenCheetah embedded Python requires CPython 3.8 or newer"
#endif

// we only really support Python 3, so lets only work on that basis
#if PY_MAJOR_VERSION >= 3
#define PYTHON3_VERSION PY_MINOR_VERSION
#endif

// global instance of embedded python
PythonEmbed *python;

namespace {

class PythonConfigGuard final
{
public:
    PythonConfigGuard() { PyConfig_InitPythonConfig(&config); }
    ~PythonConfigGuard() { PyConfig_Clear(&config); }

    PythonConfigGuard(const PythonConfigGuard &) = delete;
    PythonConfigGuard &operator=(const PythonConfigGuard &) = delete;

    PyConfig config;
};

bool pythonConfigFailed(const char *operation, const PyStatus &status)
{
    if (!PyStatus_Exception(status)) return false;
    fprintf(
        stderr,
        "Python configuration failed during %s%s%s%s (exit code %d).\n",
        operation,
        status.func ? " [" : "",
        status.func ? status.func : "",
        status.func ? "]" : "",
        status.exitcode);
    if (status.err_msg) fprintf(stderr, "%s\n", status.err_msg);
    return true;
}

const char *pythonPostInitializationStage(
    const PythonRuntimePostInitializer::Result result)
{
    using Result = PythonRuntimePostInitializer::Result;
    switch (result) {
    case Result::Rejected: return "transaction validation";
    case Result::Prepared: return "complete";
    case Result::SysImportFailed: return "sys import";
    case Result::PathFailed: return "sys.path setup";
    case Result::CatcherScriptFailed: return "stdio catcher script";
    case Result::DeploymentPathFailed: return "deployed site-packages setup";
    case Result::LibraryResourceFailed: return "library.py resource read";
    case Result::LibraryScriptFailed: return "library.py execution";
    case Result::MainModuleFailed: return "__main__ lookup";
    case Result::CatcherReferenceFailed: return "catchOutErr lookup";
    case Result::ClearReferenceFailed: return "catchOutErr.__init__ lookup";
    case Result::Exception: return "C++ exception containment";
    }
    return "unknown stage";
}

class PythonGilGuard final
{
public:
    PythonGilGuard() : state_(PyGILState_Ensure()) {}
    ~PythonGilGuard() { PyGILState_Release(state_); }

    PythonGilGuard(const PythonGilGuard &) = delete;
    PythonGilGuard &operator=(const PythonGilGuard &) = delete;

private:
    PyGILState_STATE state_;
};

template<typename Function>
class ScopeExit final
{
public:
    explicit ScopeExit(Function function)
        : function_(std::move(function)) {}
    ~ScopeExit() { function_(); }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    Function function_;
};

template<typename Function>
ScopeExit<typename std::decay<Function>::type>
scopeExit(Function &&function)
{
    return ScopeExit<typename std::decay<Function>::type>(
        std::forward<Function>(function));
}

thread_local bool pythonRunlineActive = false;

void clearCapturedOutput(PythonEmbed *embed)
{
    PyObject *cleared =
            PyObject_CallFunction(static_cast<PyObject *>(embed->clear), NULL);
    Py_XDECREF(cleared);
    PyErr_Clear();
}

QStringList takeCapturedOutput(PythonEmbed *embed)
{
    QStringList messages;
    PyObject *output = PyObject_GetAttrString(
        static_cast<PyObject *>(embed->catcher), "value");
    if (output) {
        Py_ssize_t size = 0;
        wchar_t *string = PyUnicode_AsWideCharString(output, &size);
        if (string) {
            if (size) messages = QString::fromWCharArray(string).split("\n");
            PyMem_Free(string);
            if (!messages.isEmpty()) messages << "\n";
        }
        Py_DECREF(output);
    } else {
        PyErr_Clear();
    }

    clearCapturedOutput(embed);
    return messages;
}

}

// SIP module with GoldenCheetah Bindings
extern "C" {
extern PyObject *PyInit_goldencheetah(void);
};

QString
PythonEmbed::buildVersion()
{
    return QString("%1.%2.%3").arg(PY_MAJOR_VERSION).arg(PY_MINOR_VERSION).arg(PY_MICRO_VERSION);
}

PythonEmbed::~PythonEmbed()
{
}

bool
PythonEmbed::shutdown()
{
    using ShutdownResult = PythonExecutionGate::ShutdownResult;
    const ShutdownResult drained =
            executionGate.beginShutdownAndWait(std::chrono::seconds(5));
    if (drained != ShutdownResult::Drained) return false;
    if (!contexts.isEmpty() || activeThreadId != 0 || activeRunToken != 0) {
        return false;
    }

    loaded = false;
    const PythonRuntimeFinalizer::Result result = PythonRuntimeFinalizer::run(
        initializationThread_,
        initializationState_,
        mainThreadState_,
        clear,
        catcher,
        {
            [](void *saved) {
                PyEval_RestoreThread(static_cast<PyThreadState *>(saved));
            },
            [](void *reference) {
                Py_DECREF(static_cast<PyObject *>(reference));
            },
            []() { return Py_FinalizeEx(); }
        });
    if (result == PythonRuntimeFinalizer::Result::FinalizedWithErrors) {
        qWarning() << "Python finalized with buffered-data flush errors";
    }
    return result != PythonRuntimeFinalizer::Result::Rejected;
}

bool PythonEmbed::pythonInstalled(QString &pybin, QString &pypath, QString PYTHONHOME)
{
    QStringList names; names << QString("python3.%1").arg(PYTHON3_VERSION) << QString("bin/python3.%1").arg(PYTHON3_VERSION) << "python3" << "bin/python3" << "python" << "bin/python";
    QString pythonbinary;
    if (PYTHONHOME=="") {

        // where to check
        QString path = QProcessEnvironment::systemEnvironment().value("PATH", "");
        printd("PATH=%s\n", path.toStdString().c_str());

        // what we found
        QStringList installnames;

        // lets search
        foreach(QString name, names) {
            installnames = Utils::searchPath(path, name, true);
            if (installnames.count() >0) break;
        }

        printd("Binary found:%d\n", (int)installnames.count());
        // if we failed, its not installed
        if (installnames.count()==0) return false;

        // lets just use the first one we found
        pythonbinary = installnames[0];
        pybin=pythonbinary;

    } else {

        // look for python3 or python in PYTHONHOME
#ifdef WIN32
        QString ext= QString(".exe");
#else
        QString ext= QString("");
#endif
        foreach(QString name, names) {
            QString filename = PYTHONHOME + QDir::separator() + name + ext;
            if (QFileInfo(filename).exists() && QFileInfo(filename).isExecutable()) {
                pythonbinary=filename;
                pybin=pythonbinary;
                printd("Binary found\n");
                break;
            }
        }
        // not found give up straight away
        if (pythonbinary == "") return false;
    }

#ifdef WIN32
        // ugh. QProcess doesn't like spaces or backslashes. POC.
        pythonbinary=pythonbinary.replace("\\", "/");
        pythonbinary="\"" + pythonbinary + "\"";
#endif

    // get the version and path via an interaction
    printd("Running: %s\n", pythonbinary.toStdString().c_str());
    QProcess py;
    py.setProgram(pythonbinary);

    // set the arguments
    QStringList args;
    args << "-c";
    args << QString("import sys\n"
                    "print('ZZ',sys.version_info.major,'ZZ')\n"
                    "print('ZZ',sys.version_info.minor,'ZZ')\n"
                    "print('ZZ', '%1'.join(sys.path), 'ZZ')\n"
                    "quit()\n").arg(PATHSEP);
    py.setArguments(args);
    py.setProcessChannelMode(QProcess::ForwardedErrorChannel);

    // If checking a specific PYTHONHOME (e.g. bundled), ensure the process uses it
    // and doesn't get confused by local user environment variables.
    if (!PYTHONHOME.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONHOME", PYTHONHOME);
        env.remove("PYTHONPATH"); // Ensure isolation from user's python libs
        py.setProcessEnvironment(env);
    }

    py.start();

    // failed to start python
    if (py.waitForStarted(500) == false) {
        fprintf(stderr, "Failed to start: %s\n", pythonbinary.toStdString().c_str());
        py.terminate();
        return false;
    }

    // wait for output, should be rapid
    if (py.waitForReadyRead(4000)==false) {
        fprintf(stderr, "Didn't get output: %s\n", pythonbinary.toStdString().c_str());
        py.terminate();
        return false;
    }

    // get output
    QString output = py.readAll();

    // close if it didn't already
    if (py.waitForFinished(500)==false) {
        fprintf(stderr, "forced terminate of %s\n", pythonbinary.toStdString().c_str());
        py.terminate();
    }

    // scan output
    QRegExp contents("^ZZ(.*)ZZ.*ZZ(.*)ZZ.*ZZ(.*)ZZ.*$");
    printd("Output: %s\n", output.toStdString().c_str());
    if (contents.exactMatch(output)) {
        QString vmajor=contents.cap(1);
        QString vminor=contents.cap(2);
        QString path=contents.cap(3);

        // check its Python 3 matching the version used for build
        if (vmajor.toInt() != 3 || vminor.toInt() != PYTHON3_VERSION) {
            fprintf(stderr, "Python version mismatch: GoldenCheetah was built with Python 3.%d, but found Python %d.%d at %s\n",
                    PYTHON3_VERSION, vmajor.toInt(), vminor.toInt(), pythonbinary.toStdString().c_str());
            return false;
        }

        // now get python path
#ifdef WIN32
        pypath = path.replace("\\", "/");
#else
        pypath = path;
#endif
        printd("Python path: %s\n", pypath.toStdString().c_str());
        return true;

    } else {

        // didn't understand !
        printd("Python output doesn't parse: %s\n", output.toStdString().c_str());
    }

    // by default we return false (pessimistic)
    return false;
}

PythonEmbed::PythonEmbed(const bool verbose, const bool interactive)
    : verbose(verbose), interactive(interactive)
{
    catcher = NULL;
    clear = NULL;
    loaded = false;
    activeThreadId = 0;
    activeRunToken = 0;
    activeCancellationRequested = false;
    name = QString("GoldenCheetah");

    // register metatypes used to pass between threads
    qRegisterMetaType<QVector<double> >();
    qRegisterMetaType<QStringList>();


    // Deployed Python location
    QString deployedPython = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MAC)
    deployedPython += "/../Frameworks/Python.framework/Versions/Current";
#elif defined(Q_OS_LINUX)
    deployedPython += QString("/opt/python3.%1").arg(PYTHON3_VERSION);
#endif

    // config, deployed or environment variable
    QString PYTHONHOME = appsettings->value(NULL, GC_PYTHON_HOME, "").toString().trimmed();
    if (PYTHONHOME == "") {
        if (pythonInstalled(pybin, pypath, deployedPython)) {
            PYTHONHOME = deployedPython;
            qputenv("PYTHONHOME",PYTHONHOME.toUtf8());
        } else {
            PYTHONHOME = QProcessEnvironment::systemEnvironment().value("PYTHONHOME", "");
        }
    } else {
        qputenv("PYTHONHOME",PYTHONHOME.toUtf8());
    }
    if (PYTHONHOME !="") printd("PYTHONHOME setting used: %s\n", PYTHONHOME.toStdString().c_str());

    // is python3 installed?
    if (pythonInstalled(pybin, pypath, PYTHONHOME)) {

        printd("Python is installed: %s\n", pybin.toStdString().c_str());

        const auto initializeConfiguredPython = [&]() {
            PythonConfigGuard guard;
            const std::wstring executable =
                QFileInfo(pybin).absoluteFilePath().toStdWString();
            const std::wstring home = PYTHONHOME.toStdWString();
            return PythonRuntimeInitializer::run(
                initializationState_,
                {
                    [&]() {
                        guard.config.parse_argv = 0;
                        guard.config.use_environment = 1;
                        guard.config.install_signal_handlers = 0;
                        guard.config.site_import = 1;
                        guard.config.module_search_paths_set = 0;

                        PyStatus status = PyConfig_SetString(
                            &guard.config, &guard.config.program_name,
                            executable.c_str());
                        if (pythonConfigFailed("program_name", status)) {
                            return false;
                        }
                        status = PyConfig_SetString(
                            &guard.config, &guard.config.executable,
                            executable.c_str());
                        if (pythonConfigFailed("executable", status)) {
                            return false;
                        }
                        if (!home.empty()) {
                            status = PyConfig_SetString(
                                &guard.config, &guard.config.home,
                                home.c_str());
                            if (pythonConfigFailed("home", status)) {
                                return false;
                            }
                        }
                        return true;
                    },
                    [&]() {
                        printd("PyImport_AppendInittab: goldencheetah\n");
                        if (PyImport_AppendInittab(
                                "goldencheetah", PyInit_goldencheetah) != -1) {
                            return true;
                        }
                        fprintf(
                            stderr,
                            "Python built-in module registration failed.\n");
                        return false;
                    },
                    [&]() {
                        printd("Py_InitializeFromConfig\n");
                        const PyStatus status =
                            Py_InitializeFromConfig(&guard.config);
                        return !pythonConfigFailed("initialization", status);
                    },
                    []() { return Py_IsInitialized() != 0; }
                }) == PythonRuntimeInitializer::Result::Initialized;
        };

        if (initializeConfiguredPython()) {
            initializationState_ = InitializationState::InterpreterInitialized;
            using PostInitializer = PythonRuntimePostInitializer;
            const PostInitializer::Result postResult =
                PostInitializer::contain([&]() {
                printd("Py_GetVersion()\n");
                version = QString(Py_GetVersion());
                version.replace("\n", " ");
                const std::string versionText = version.toStdString();

            const std::string stdOutErr = ("import sys\n"
 #ifdef Q_OS_LINUX
                                     "import os\n"
                                     "sys.setdlopenflags(os.RTLD_NOW | os.RTLD_DEEPBIND)\n"
 #endif
                                     "class CatchOutErr:\n"
                                     "    def __init__(self):\n"
                                     "        self.value = ''\n"
                                     "    def write(self, txt):\n"
                                     "        self.value += txt\n"
                                     "    def flush(self):\n"
                                     "        pass\n"
                                     "catchOutErr = CatchOutErr()\n"
                                     "sys.stdout = catchOutErr\n"
                                     "sys.stderr = catchOutErr\n"
                                     "import goldencheetah\n"
                                     "GC=goldencheetah.Bindings()\n");
            const std::string ensureSitePackages =
                "import sys\n"
                "sys.path.append(sys.prefix+'/lib/python3.'+"
                "str(sys.version_info.minor)+'/site-packages')\n";
            QString librarySource;

                const PostInitializer::Result result = PostInitializer::run(
                catcher,
                clear,
                {
                    []() {
                        printd("PyImportModule('sys')\n");
                        return static_cast<void *>(PyImport_ImportModule("sys"));
                    },
                    [](void *sys) {
                        printd("Add '.' to Path\n");
                        return PythonPathAppender::append(
                            sys,
                            {
                                [](void *module) {
                                    return static_cast<void *>(
                                        PyObject_GetAttrString(
                                            static_cast<PyObject *>(module),
                                            "path"));
                                },
                                [](void *path) {
                                    return PyList_Check(
                                        static_cast<PyObject *>(path));
                                },
                                []() {
                                    return static_cast<void *>(
                                        PyUnicode_FromString("."));
                                },
                                [](void *path, void *entry) {
                                    return PyList_Append(
                                        static_cast<PyObject *>(path),
                                        static_cast<PyObject *>(entry)) == 0;
                                },
                                [](void *reference) {
                                    Py_DECREF(
                                        static_cast<PyObject *>(reference));
                                }
                            }) == PythonPathAppender::Result::Appended;
                    },
                    [&stdOutErr]() {
                        printd("Install stdio catcher\n");
                        return PyRun_SimpleString(stdOutErr.c_str()) == 0;
                    },
                    [&]() {
 #ifdef Q_OS_LINUX
                        if (PYTHONHOME == deployedPython) {
                            return PyRun_SimpleString(
                                ensureSitePackages.c_str()) == 0;
                        }
 #endif
                        return true;
                    },
                    [&librarySource]() {
                        printd("Load library.py\n");
                        QFile library(":python/library.py");
                        if (!library.open(QFile::ReadOnly)) return false;
                        librarySource = library.readAll();
                        const bool readSuccessfully =
                            library.error() == QFileDevice::NoError;
                        library.close();
                        return readSuccessfully;
                    },
                    [&librarySource]() {
                        const QByteArray encoded = librarySource.toLatin1();
                        return PyRun_SimpleString(encoded.constData()) == 0;
                    },
                    []() {
                        printd("Get catcher refs\n");
                        return static_cast<void *>(
                            PyImport_AddModule("__main__"));
                    },
                    [](void *mainModule) {
                        return static_cast<void *>(PyObject_GetAttrString(
                            static_cast<PyObject *>(mainModule),
                            "catchOutErr"));
                    },
                    [](void *catcherReference) {
                        return static_cast<void *>(PyObject_GetAttrString(
                            static_cast<PyObject *>(catcherReference),
                            "__init__"));
                    },
                    [](void *reference) {
                        Py_DECREF(static_cast<PyObject *>(reference));
                    }
                });

                if (result == PostInitializer::Result::Prepared) {
                    fprintf(
                        stderr,
                        "Python loaded [%s]\n",
                        versionText.c_str());
                    fflush(stderr);
                }
                return result;
            });

            if (postResult == PostInitializer::Result::Prepared) {
                // No potentially throwing work is permitted after the GIL is
                // released: failure diagnostics use Python error APIs.
                mainThreadState_ = static_cast<void *>(PyEval_SaveThread());
                initializationState_ = InitializationState::Ready;
                loaded = true;
                return;
            }

            fprintf(
                stderr,
                "Python post-initialization failed during %s.\n",
                pythonPostInitializationStage(postResult));
            if (PyErr_Occurred()) PyErr_Print();
            PyErr_Clear();
        } // configured Python initialized
    } // pythonInstalled == true

    // if we get here loading failed
    fprintf(stderr, "Python embedding failed. GoldenCheetah requires Python 3.%d installed and in PATH.\n", PYTHON3_VERSION);
    // Notify user of the problem (they can disable Python in preferences if they don't want to see this)
    // Note: We don't permanently disable Python here - the user might fix the issue (install Python,
    // fix PYTHONHOME, etc.) and we should try again on next startup.
    try {
        QMessageBox msg(QMessageBox::Warning, QObject::tr("Python not available"),
                        QObject::tr("GoldenCheetah was built with Python 3.%1 but could not initialize Python.\n\n"
                                    "Please ensure Python 3.%1 is installed and in your PATH.\n"
                                    "You can disable Python in Options > General if you don't need it.").arg(PYTHON3_VERSION));
        msg.exec();
    } catch (...) {
        fprintf(stderr, "Unable to display the Python initialization warning.\n");
    }
    loaded=false;
    return;
}

// run on called thread
PythonRunResult
PythonEmbed::runline(
    ScriptContext scriptContext,
    QString line,
    std::shared_ptr<std::atomic_bool> cancelled)
{
    PythonRunResult result;
    const auto cancellationRequested = [&cancelled]() {
        return cancelled
                && cancelled->load(std::memory_order_acquire);
    };

    if (cancellationRequested()) {
        result.cancelled = true;
        return result;
    }

    if (pythonRunlineActive) {
        result.error =
                QStringLiteral("Nested Python execution is not supported.");
        result.messages << result.error;
        return result;
    }

    const bool guiCaller =
            QCoreApplication::instance()
            && QThread::currentThread()
                == QCoreApplication::instance()->thread();
    PythonExecutionGate::Lease executionLease;
    const PythonExecutionGate::Admission admission =
            executionGate.acquire(
                guiCaller, cancelled, executionLease);
    if (admission == PythonExecutionGate::Admission::Cancelled) {
        result.cancelled = true;
        return result;
    }
    if (admission == PythonExecutionGate::Admission::Busy) {
        result.error = QStringLiteral(
            "Python execution is already active on another thread.");
        result.messages << result.error;
        return result;
    }
    if (admission == PythonExecutionGate::Admission::Closed || !loaded) {
        result.error = QStringLiteral("Python is not available.");
        result.messages << result.error;
        return result;
    }

    pythonRunlineActive = true;
    auto releaseThreadGuard = scopeExit([]() {
        pythonRunlineActive = false;
    });

    PythonGilGuard gil;
    const unsigned long currentThreadId = PyThread_get_thread_ident();

    if (contexts.contains(currentThreadId)) {
        result.error =
                QStringLiteral("Python run context is already registered.");
        result.messages << result.error;
        return result;
    }

    scriptContext.runResult = &result;
    contexts.insert(currentThreadId, scriptContext);
    activeThreadId = currentThreadId;
    activeRunToken = scriptContext.runToken;
    activeCancellationRequested = false;
    executionGate.publishToken(activeRunToken);
    auto clearActiveRun = scopeExit([this, currentThreadId]() {
        contexts.remove(currentThreadId);
        if (activeThreadId == currentThreadId) {
            executionGate.publishToken(0);
            activeThreadId = 0;
            activeRunToken = 0;
            activeCancellationRequested = false;
        }
    });

    clearCapturedOutput(this);

    if (cancellationRequested()) {
        activeCancellationRequested = true;
        result.cancelled = true;
        return result;
    }

    if (scriptContext.interactiveShell) {
        PyObject *m, *d, *v;
        m = PyImport_AddModule("__main__");
        d = PyModule_GetDict(m);
        const QByteArray encodedLine = line.toUtf8();
        v = PyRun_StringFlags(
            encodedLine.constData(), Py_single_input, d, d, 0);
        if (v) Py_DECREF(v);
    } else {
        const QByteArray encodedLine = line.toUtf8();
        PyRun_SimpleString(encodedLine.constData());
    }

    if (activeCancellationRequested
        && PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
        PyErr_Clear();
    } else {
        PyErr_Print();
        PyErr_Clear();
    }

    result.messages = takeCapturedOutput(this);
    result.cancelled = activeCancellationRequested
            || cancellationRequested();
    return result;
}

quint64
PythonEmbed::allocateRunToken()
{
    return executionGate.allocateToken();
}

bool
PythonEmbed::cancel(quint64 runToken)
{
    PythonExecutionGate::Entry entry;
    if (!executionGate.tryEnter(entry)) return false;
    if (!loaded || runToken == 0) return false;

    executionGate.wakeWaiters();
    if (!executionGate.isPublishedToken(runToken)) {
        return false;
    }

    PythonGilGuard gil;
    if (activeThreadId == 0 || runToken != activeRunToken) {
        return false;
    }
    const int affected = PyThreadState_SetAsyncExc(
        activeThreadId, PyExc_KeyboardInterrupt);
    if (affected == 1) {
        activeCancellationRequested = true;
        return true;
    }
    if (affected > 1) {
        const int rolledBack =
                PyThreadState_SetAsyncExc(activeThreadId, NULL);
        if (rolledBack != affected) {
            qWarning()
                    << "Python cancellation rollback affected"
                    << rolledBack << "threads instead of" << affected;
        }
    } else {
        qWarning()
                << "Python cancellation could not find the active thread";
    }
    return false;
}
