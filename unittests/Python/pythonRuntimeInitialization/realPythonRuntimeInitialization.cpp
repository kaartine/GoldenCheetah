/*
 * Real embedded-CPython initialization child. Run each mode in a separate
 * process:
 *   tst_realPythonRuntimeInitialization ready /absolute/python/path
 *   tst_realPythonRuntimeInitialization invalid-home /absolute/python/path
 */

#include "Python/PythonRuntimeInitializer.h"

#include <Python.h>

#include <csignal>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

PyObject *initializeTestModule()
{
    static PyModuleDef definition{
        PyModuleDef_HEAD_INIT,
        "gc_initialization_test",
        nullptr,
        -1,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    };
    return PyModule_Create(&definition);
}

void testSignalHandler(int)
{
}

class ConfigGuard final
{
public:
    ConfigGuard() { PyConfig_InitPythonConfig(&config); }
    ~ConfigGuard() { PyConfig_Clear(&config); }

    PyConfig config;
};

int run(const bool invalidHome, const wchar_t *executable)
{
    using Initializer = PythonRuntimeInitializer;
    Initializer::State state = Initializer::State::NotStarted;
    ConfigGuard guard;
    PyStatus status = PyStatus_Ok();
    const auto previousSignalHandler = std::signal(SIGINT, testSignalHandler);

    const auto result = Initializer::run(
        state,
        {
            [&]() {
                guard.config.parse_argv = 0;
                guard.config.use_environment = 1;
                guard.config.install_signal_handlers = 0;
                guard.config.site_import = 1;
                guard.config.module_search_paths_set = 0;
                status = PyConfig_SetString(
                    &guard.config, &guard.config.program_name, executable);
                if (PyStatus_Exception(status)) return false;
                status = PyConfig_SetString(
                    &guard.config, &guard.config.executable, executable);
                if (PyStatus_Exception(status)) return false;
                if (!invalidHome) return true;
                status = PyConfig_SetString(
                    &guard.config, &guard.config.home,
                    L"/goldencheetah/nonexistent-python-home");
                return !PyStatus_Exception(status);
            },
            []() {
                return PyImport_AppendInittab(
                    "gc_initialization_test", initializeTestModule) != -1;
            },
            [&]() {
                status = Py_InitializeFromConfig(&guard.config);
                return !PyStatus_Exception(status);
            },
            []() { return Py_IsInitialized() != 0; }
        });

    if (invalidHome) {
        if (result != Initializer::Result::Failed
            || state != Initializer::State::Preinitialized
            || Py_IsInitialized()) {
            return 20;
        }
        return 0;
    }

    if (result != Initializer::Result::Initialized
        || state != Initializer::State::InterpreterInitialized) {
        return 10;
    }
    if (std::wcscmp(Py_GetProgramName(), executable) != 0) return 11;

    const auto installed = std::signal(SIGINT, previousSignalHandler);
    if (installed != testSignalHandler) return 12;

    if (PyRun_SimpleString(
            "import encodings, site, gc_initialization_test, sys\n") != 0) {
        return 13;
    }

    PyObject *sys = PyImport_ImportModule("sys");
    if (!sys) return 14;
    PyObject *value = PyObject_GetAttrString(sys, "executable");
    Py_DECREF(sys);
    if (!value) return 15;
    const wchar_t *configuredExecutable = PyUnicode_AsWideCharString(
        value, nullptr);
    Py_DECREF(value);
    if (!configuredExecutable) return 16;
    const bool matches = std::wcscmp(configuredExecutable, executable) == 0;
    PyMem_Free(const_cast<wchar_t *>(configuredExecutable));
    if (!matches) return 17;

    return Py_FinalizeEx() == 0 ? 0 : 18;
}

}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const bool invalidHome = std::strcmp(argv[1], "invalid-home") == 0;
    if (!invalidHome && std::strcmp(argv[1], "ready") != 0) return 3;

    const std::string encodedExecutable(argv[2]);
    const std::wstring executable(
        encodedExecutable.begin(), encodedExecutable.end());
    return run(invalidHome, executable.c_str());
}
