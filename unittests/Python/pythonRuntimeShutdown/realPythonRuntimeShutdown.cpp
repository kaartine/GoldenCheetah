/*
 * Real embedded-CPython shutdown child. Run each mode in a separate process:
 *   tst_realPythonRuntimeShutdown ready marker.txt
 *   tst_realPythonRuntimeShutdown partial marker.txt
 */

#include "Python/PythonRuntimeFinalizer.h"

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

int run(const bool ready)
{
    using Finalizer = PythonRuntimeFinalizer;
    Py_InitializeEx(0);
    if (!Py_IsInitialized() || Py_AtExit(atExitMarker) != 0) return 10;

    Finalizer::State state = Finalizer::State::InterpreterInitialized;
    void *saved = nullptr;
    void *clear = nullptr;
    void *catcher = nullptr;
    std::vector<int> calls;
    bool hooksObservedFinalizing = true;

    if (ready) {
        if (PyRun_SimpleString(
                "class Catcher:\n"
                "    def __init__(self): self.value = ''\n"
                "catcher = Catcher()\n") != 0) {
            return 11;
        }
        PyObject *mainModule = PyImport_AddModule("__main__");
        catcher = PyObject_GetAttrString(mainModule, "catcher");
        clear = PyObject_GetAttrString(
            static_cast<PyObject *>(catcher), "__init__");
        if (!catcher || !clear) return 12;
        saved = PyEval_SaveThread();
        if (!saved) return 13;

        int workerStatus = -1;
        std::thread worker([&workerStatus]() {
            const PyGILState_STATE gil = PyGILState_Ensure();
            workerStatus = PyRun_SimpleString("assert 6 * 7 == 42");
            PyGILState_Release(gil);
        });
        worker.join();
        if (workerStatus != 0) return 14;
        state = Finalizer::State::Ready;
    }

    const Finalizer::Result result = Finalizer::run(
        std::this_thread::get_id(), state, saved, clear, catcher,
        {
            [&calls, &state, &hooksObservedFinalizing](void *threadState) {
                hooksObservedFinalizing = hooksObservedFinalizing
                    && state == Finalizer::State::Finalizing;
                calls.push_back(1);
                PyEval_RestoreThread(
                    static_cast<PyThreadState *>(threadState));
            },
            [&calls, &state, &hooksObservedFinalizing](void *reference) {
                hooksObservedFinalizing = hooksObservedFinalizing
                    && state == Finalizer::State::Finalizing;
                calls.push_back(calls.size() == 1 ? 2 : 3);
                Py_DECREF(static_cast<PyObject *>(reference));
            },
            [&calls, &state, &hooksObservedFinalizing]() {
                hooksObservedFinalizing = hooksObservedFinalizing
                    && state == Finalizer::State::Finalizing;
                calls.push_back(4);
                return Py_FinalizeEx();
            }
        });

    // No Python API is permitted below this point.
    if (result != Finalizer::Result::Finalized
        || state != Finalizer::State::Finalized
        || saved || clear || catcher || !hooksObservedFinalizing) {
        return 15;
    }
    if (ready && calls != std::vector<int>({1, 2, 3, 4})) return 16;
    if (!ready && calls != std::vector<int>({4})) return 17;
    if (Finalizer::run(
            std::this_thread::get_id(), state, saved, clear, catcher,
            {[](void *) {}, [](void *) {}, []() { return 0; }})
        != Finalizer::Result::Rejected) {
        return 18;
    }

    FILE *marker = std::fopen(markerPath, "a");
    if (!marker) return 19;
    std::fputs(ready ? "ready-finalized\n" : "partial-finalized\n", marker);
    std::fclose(marker);
    return 0;
}
}

int main(int argc, char **argv)
{
    if (argc != 3 || std::strlen(argv[2]) >= sizeof(markerPath)) return 2;
    const bool ready = std::strcmp(argv[1], "ready") == 0;
    if (!ready && std::strcmp(argv[1], "partial") != 0) return 3;
    std::strcpy(markerPath, argv[2]);
    return run(ready);
}
