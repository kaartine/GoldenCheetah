/*
 * Real embedded-CPython post-initialization child. Each mode is run in a
 * separate process with a marker path as its second argument.
 */

#include "Python/PythonPathAppender.h"
#include "Python/PythonRuntimeFinalizer.h"
#include "Python/PythonRuntimePostInitializer.h"

#include <Python.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

char markerPath[4096] = {};

void atExitMarker()
{
    FILE *marker = std::fopen(markerPath, "a");
    if (!marker) return;
    std::fputs("atexit\n", marker);
    std::fclose(marker);
}

bool runScript(const char *source)
{
    return PyRun_SimpleString(source) == 0;
}

PythonRuntimePostInitializer::Result expectedFailure(const char *mode)
{
    using Result = PythonRuntimePostInitializer::Result;
    if (std::strcmp(mode, "catcher-failure") == 0) {
        return Result::CatcherScriptFailed;
    }
    if (std::strcmp(mode, "deployment-failure") == 0) {
        return Result::DeploymentPathFailed;
    }
    if (std::strcmp(mode, "resource-failure") == 0) {
        return Result::LibraryResourceFailed;
    }
    if (std::strcmp(mode, "library-failure") == 0) {
        return Result::LibraryScriptFailed;
    }
    if (std::strcmp(mode, "catcher-attr-failure") == 0) {
        return Result::CatcherReferenceFailed;
    }
    if (std::strcmp(mode, "clear-attr-failure") == 0) {
        return Result::ClearReferenceFailed;
    }
    return Result::Prepared;
}

int run(const char *mode)
{
    using Finalizer = PythonRuntimeFinalizer;
    using PostInitializer = PythonRuntimePostInitializer;

    Py_Initialize();
    if (!Py_IsInitialized() || Py_AtExit(atExitMarker) != 0) return 10;

    Finalizer::State state = Finalizer::State::InterpreterInitialized;
    void *saved = nullptr;
    void *catcher = nullptr;
    void *clear = nullptr;
    const bool catcherFailure = std::strcmp(mode, "catcher-failure") == 0;
    const bool deploymentFailure =
        std::strcmp(mode, "deployment-failure") == 0;
    const bool resourceFailure = std::strcmp(mode, "resource-failure") == 0;
    const bool libraryFailure = std::strcmp(mode, "library-failure") == 0;
    const bool catcherAttrFailure =
        std::strcmp(mode, "catcher-attr-failure") == 0;
    const bool clearAttrFailure =
        std::strcmp(mode, "clear-attr-failure") == 0;

    const char *normalCatcher =
        "import sys\n"
        "class Catcher:\n"
        "    def __init__(self): self.value = ''\n"
        "    def write(self, text): self.value += text\n"
        "    def flush(self): pass\n"
        "catchOutErr = Catcher()\n"
        "sys.stdout = catchOutErr\n"
        "sys.stderr = catchOutErr\n";
    const char *failingCatcher =
        "import sys\n"
        "class Catcher:\n"
        "    def __init__(self): self.value = ''\n"
        "    def write(self, text): self.value += text\n"
        "    def flush(self): pass\n"
        "partialCatcher = Catcher()\n"
        "sys.stderr = partialCatcher\n"
        "raise RuntimeError('catcher failure after redirection')\n";

    const PostInitializer::Result postResult = PostInitializer::run(
        catcher,
        clear,
        {
            []() {
                return static_cast<void *>(PyImport_ImportModule("sys"));
            },
            [](void *sys) {
                return PythonPathAppender::append(
                    sys,
                    {
                        [](void *module) {
                            return static_cast<void *>(PyObject_GetAttrString(
                                static_cast<PyObject *>(module), "path"));
                        },
                        [](void *path) {
                            return PyList_Check(static_cast<PyObject *>(path));
                        },
                        []() {
                            return static_cast<void *>(PyUnicode_FromString("."));
                        },
                        [](void *path, void *entry) {
                            return PyList_Append(
                                static_cast<PyObject *>(path),
                                static_cast<PyObject *>(entry)) == 0;
                        },
                        [](void *reference) {
                            Py_DECREF(static_cast<PyObject *>(reference));
                        }
                    }) == PythonPathAppender::Result::Appended;
            },
            [&]() {
                return runScript(catcherFailure
                    ? failingCatcher : normalCatcher);
            },
            [&]() {
                return !deploymentFailure || runScript(
                    "deployment_side_effect = True\n"
                    "raise RuntimeError('deployment failure')\n");
            },
            [&]() { return !resourceFailure; },
            [&]() {
                return runScript(libraryFailure
                    ? "library_side_effect = True\n"
                      "raise RuntimeError('library failure')\n"
                    : "library_loaded = True\n");
            },
            []() {
                return static_cast<void *>(PyImport_AddModule("__main__"));
            },
            [&](void *mainModule) {
                return static_cast<void *>(PyObject_GetAttrString(
                    static_cast<PyObject *>(mainModule),
                    catcherAttrFailure ? "missingCatcher" : "catchOutErr"));
            },
            [&](void *catcherReference) {
                return static_cast<void *>(PyObject_GetAttrString(
                    static_cast<PyObject *>(catcherReference),
                    clearAttrFailure ? "missingClear" : "__init__"));
            },
            [](void *reference) {
                Py_DECREF(static_cast<PyObject *>(reference));
            }
        });

    const PostInitializer::Result expected = expectedFailure(mode);
    if (postResult != expected) return 11;
    if (postResult == PostInitializer::Result::Prepared) {
        if (!catcher || !clear) return 12;
        saved = PyEval_SaveThread();
        if (!saved) return 13;
        state = Finalizer::State::Ready;
    } else {
        if (catcher || clear || saved
            || state != Finalizer::State::InterpreterInitialized) {
            return 14;
        }
        PyErr_Clear();
    }

    std::vector<int> finalizerCalls;
    const Finalizer::Result finalResult = Finalizer::run(
        std::this_thread::get_id(), state, saved, clear, catcher,
        {
            [&](void *threadState) {
                finalizerCalls.push_back(1);
                PyEval_RestoreThread(
                    static_cast<PyThreadState *>(threadState));
            },
            [&](void *reference) {
                finalizerCalls.push_back(finalizerCalls.size() == 1 ? 2 : 3);
                Py_DECREF(static_cast<PyObject *>(reference));
            },
            [&]() {
                finalizerCalls.push_back(4);
                return Py_FinalizeEx();
            }
        });

    // No Python API is permitted below this point.
    if (finalResult != Finalizer::Result::Finalized
        || state != Finalizer::State::Finalized
        || saved || clear || catcher) {
        return 15;
    }
    if (expected == PostInitializer::Result::Prepared) {
        if (finalizerCalls != std::vector<int>({1, 2, 3, 4})) return 16;
    } else if (finalizerCalls != std::vector<int>({4})) {
        return 17;
    }

    FILE *marker = std::fopen(markerPath, "a");
    if (!marker) return 18;
    std::fputs("finalized\n", marker);
    std::fclose(marker);
    return 0;
}

}

int main(int argc, char **argv)
{
    if (argc != 3 || std::strlen(argv[2]) >= sizeof(markerPath)) return 2;
    const char *mode = argv[1];
    if (expectedFailure(mode) == PythonRuntimePostInitializer::Result::Prepared
        && std::strcmp(mode, "ready") != 0) {
        return 3;
    }
    std::strcpy(markerPath, argv[2]);
    return run(mode);
}
