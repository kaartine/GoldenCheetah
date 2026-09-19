/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PYTHONRUNTIMEINITIALIZER_H
#define GC_PYTHONRUNTIMEINITIALIZER_H

#include "PythonRuntimeFinalizer.h"

#include <functional>

class PythonRuntimeInitializer final
{
public:
    using State = PythonRuntimeFinalizer::State;

    enum class Result {
        Rejected,
        Initialized,
        Failed
    };

    struct Hooks {
        std::function<bool()> configure;
        std::function<bool()> registerBuiltInModule;
        std::function<bool()> initialize;
        std::function<bool()> isInitialized;
    };

    static Result run(State &state, const Hooks &hooks)
    {
        if (state != State::NotStarted || !hooks.configure
            || !hooks.registerBuiltInModule || !hooks.initialize
            || !hooks.isInitialized) {
            return Result::Rejected;
        }

        // PyConfig setters may implicitly preinitialize CPython. Record that
        // process-lifetime ownership before making the first such call.
        state = State::Preinitialized;
        if (!hooks.configure() || !hooks.registerBuiltInModule()) {
            return Result::Failed;
        }

        const bool initializedCleanly = hooks.initialize();
        if (hooks.isInitialized()) state = State::InterpreterInitialized;
        return initializedCleanly && state == State::InterpreterInitialized
            ? Result::Initialized
            : Result::Failed;
    }
};

#endif
