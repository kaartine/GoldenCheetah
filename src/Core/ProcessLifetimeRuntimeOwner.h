/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PROCESSLIFETIMERUNTIMEOWNER_H
#define GC_PROCESSLIFETIMERUNTIMEOWNER_H

#include <memory>
#include <thread>
#include <utility>

// Some embedded runtimes cannot be safely finalized until every caller has
// been drained on the initialization thread. This owner makes that policy
// explicit: pre-initialization failures are destroyed normally, while any
// initialized runtime storage is retained for the remaining OS-process
// lifetime. The compatibility alias is published only for a ready runtime.
template <typename Runtime>
class ProcessLifetimeRuntimeOwner
{
public:
    explicit ProcessLifetimeRuntimeOwner(Runtime *&compatibilityAlias)
        : compatibilityAlias_(compatibilityAlias), ownerThread_(std::this_thread::get_id())
    {
        compatibilityAlias_ = nullptr;
    }

    ~ProcessLifetimeRuntimeOwner()
    {
        compatibilityAlias_ = nullptr;
        if (runtime_) {
            // Deliberate OS-lifetime retention. Destroying this wrapper while
            // its embedded runtime remains initialized would invalidate state
            // still owned by that runtime.
            (void)runtime_.release();
        }
    }

    ProcessLifetimeRuntimeOwner(const ProcessLifetimeRuntimeOwner &) = delete;
    ProcessLifetimeRuntimeOwner &operator=(const ProcessLifetimeRuntimeOwner &) = delete;

    template <typename Factory>
    Runtime *initialize(Factory &&factory)
    {
        if (std::this_thread::get_id() != ownerThread_) return nullptr;
        if (runtime_) return compatibilityAlias_;

        std::unique_ptr<Runtime> candidate = std::forward<Factory>(factory)();
        if (!candidate) return nullptr;
        if (candidate->initializationState() == Runtime::InitializationState::NotStarted) {
            return nullptr;
        }

        runtime_ = std::move(candidate);
        if (runtime_->initializationState() == Runtime::InitializationState::Ready) {
            compatibilityAlias_ = runtime_.get();
        }
        return compatibilityAlias_;
    }

    bool hasInitializedRuntime() const { return runtime_ != nullptr; }
    bool isOwnerThread() const { return std::this_thread::get_id() == ownerThread_; }

private:
    Runtime *&compatibilityAlias_;
    std::thread::id ownerThread_;
    std::unique_ptr<Runtime> runtime_;
};

#endif
