/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PYTHONRUNTIMEFINALIZER_H
#define GC_PYTHONRUNTIMEFINALIZER_H

#include <functional>
#include <thread>

class PythonRuntimeFinalizer final
{
public:
    enum class State {
        NotStarted,
        Preinitialized,
        InterpreterInitialized,
        Ready,
        Finalizing,
        Finalized
    };

    enum class Result {
        Rejected,
        Finalized,
        FinalizedWithErrors
    };

    struct Hooks {
        std::function<void(void *)> restoreThread;
        std::function<void(void *)> releaseReference;
        std::function<int()> finalize;
    };

    static Result run(
        const std::thread::id initializationThread,
        State &state,
        void *&savedThreadState,
        void *&clearReference,
        void *&catcherReference,
        const Hooks &hooks)
    {
        if (std::this_thread::get_id() != initializationThread
            || state == State::NotStarted || state == State::Finalizing
            || state == State::Finalized) {
            return Result::Rejected;
        }
        if (state == State::Preinitialized) {
            state = State::Finalized;
            return Result::Finalized;
        }
        if (!hooks.restoreThread || !hooks.releaseReference
            || !hooks.finalize) {
            return Result::Rejected;
        }
        state = State::Finalizing;

        if (savedThreadState) {
            void *saved = savedThreadState;
            savedThreadState = nullptr;
            hooks.restoreThread(saved);
        }

        if (clearReference) {
            void *reference = clearReference;
            clearReference = nullptr;
            hooks.releaseReference(reference);
        }
        if (catcherReference) {
            void *reference = catcherReference;
            catcherReference = nullptr;
            hooks.releaseReference(reference);
        }

        const int status = hooks.finalize();
        state = State::Finalized;
        return status < 0
            ? Result::FinalizedWithErrors
            : Result::Finalized;
    }
};

#endif
