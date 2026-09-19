/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PYTHONRUNTIMEPOSTINITIALIZER_H
#define GC_PYTHONRUNTIMEPOSTINITIALIZER_H

#include <functional>
#include <utility>

class PythonRuntimePostInitializer final
{
public:
    enum class Result {
        Rejected,
        Prepared,
        SysImportFailed,
        PathFailed,
        CatcherScriptFailed,
        DeploymentPathFailed,
        LibraryResourceFailed,
        LibraryScriptFailed,
        MainModuleFailed,
        CatcherReferenceFailed,
        ClearReferenceFailed,
        Exception
    };

    struct Hooks {
        std::function<void *()> importSys;
        std::function<bool(void *)> appendPath;
        std::function<bool()> installCatcher;
        std::function<bool()> prepareDeploymentPath;
        std::function<bool()> readLibraryResource;
        std::function<bool()> runLibraryScript;
        std::function<void *()> getMainModule;
        std::function<void *(void *)> getCatcher;
        std::function<void *(void *)> getClear;
        std::function<void(void *)> release;
    };

    template<typename Function>
    static Result contain(Function &&function) noexcept
    {
        try {
            return std::forward<Function>(function)();
        } catch (...) {
            return Result::Exception;
        }
    }

    static Result run(
        void *&catcherOutput,
        void *&clearOutput,
        const Hooks &hooks) noexcept
    {
        if (catcherOutput || clearOutput || !hooks.importSys
            || !hooks.appendPath || !hooks.installCatcher
            || !hooks.prepareDeploymentPath || !hooks.readLibraryResource
            || !hooks.runLibraryScript || !hooks.getMainModule
            || !hooks.getCatcher || !hooks.getClear || !hooks.release) {
            return Result::Rejected;
        }

        try {
            OwnedReference sys(hooks.importSys(), hooks.release);
            if (!sys.get()) return Result::SysImportFailed;
            if (!hooks.appendPath(sys.get())) return Result::PathFailed;
            if (!sys.reset()) return Result::Exception;
            if (!hooks.installCatcher()) return Result::CatcherScriptFailed;
            if (!hooks.prepareDeploymentPath()) {
                return Result::DeploymentPathFailed;
            }
            if (!hooks.readLibraryResource()) {
                return Result::LibraryResourceFailed;
            }
            if (!hooks.runLibraryScript()) {
                return Result::LibraryScriptFailed;
            }

            void *mainModule = hooks.getMainModule();
            if (!mainModule) return Result::MainModuleFailed;

            OwnedReference catcher(hooks.getCatcher(mainModule), hooks.release);
            if (!catcher.get()) return Result::CatcherReferenceFailed;
            OwnedReference clear(hooks.getClear(catcher.get()), hooks.release);
            if (!clear.get()) return Result::ClearReferenceFailed;

            catcherOutput = catcher.take();
            clearOutput = clear.take();
            return Result::Prepared;
        } catch (...) {
            return Result::Exception;
        }
    }

private:
    class OwnedReference final
    {
    public:
        OwnedReference(
            void *value,
            const std::function<void(void *)> &release) noexcept
            : value_(value), release_(release) {}

        ~OwnedReference() noexcept { reset(); }

        OwnedReference(const OwnedReference &) = delete;
        OwnedReference &operator=(const OwnedReference &) = delete;

        void *get() const noexcept { return value_; }

        void *take() noexcept
        {
            void *value = value_;
            value_ = nullptr;
            return value;
        }

        bool reset() noexcept
        {
            if (!value_) return true;
            void *value = value_;
            value_ = nullptr;
            try {
                release_(value);
                return true;
            } catch (...) {
                return false;
            }
        }

    private:
        void *value_;
        const std::function<void(void *)> &release_;
    };
};

#endif
