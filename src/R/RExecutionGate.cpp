/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "RExecutionGate.h"

#include <atomic>
#include <exception>
#include <thread>
#include <utility>

struct RExecutionGate::State
{
    explicit State(std::thread::id ownerThread)
        : ownerThread(ownerThread)
    {
    }

    const std::thread::id ownerThread;
    std::atomic_bool active{false};
};

RExecutionGate::Lease::Lease(
    std::shared_ptr<State> state,
    Cleanup cleanup,
    Cleanup afterRelease) noexcept
    : state_(std::move(state))
    , cleanup_(std::move(cleanup))
    , afterRelease_(std::move(afterRelease))
{
}

RExecutionGate::Lease::~Lease()
{
    reset();
}

RExecutionGate::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_))
    , cleanup_(std::move(other.cleanup_))
    , afterRelease_(std::move(other.afterRelease_))
{
}

RExecutionGate::Lease &
RExecutionGate::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        cleanup_ = std::move(other.cleanup_);
        afterRelease_ = std::move(other.afterRelease_);
    }
    return *this;
}

void
RExecutionGate::Lease::reset() noexcept
{
    if (!state_) return;

    // Keep the state alive and the gate occupied until cleanup completes.
    std::shared_ptr<State> state = std::move(state_);
    Cleanup cleanup = std::move(cleanup_);
    Cleanup afterRelease = std::move(afterRelease_);
    if (std::this_thread::get_id() != state->ownerThread) {
        std::terminate();
    }
    try {
        if (cleanup) cleanup();
    } catch (...) {
        // A lease destructor cannot propagate. Releasing after cleanup was
        // attempted preserves the gate's teardown contract.
    }
    state->active.store(false, std::memory_order_release);
    try {
        if (afterRelease) afterRelease();
    } catch (...) {
        // Post-release work cannot make lease destruction throw.
    }
}

RExecutionGate::RExecutionGate()
    : state_(std::make_shared<State>(std::this_thread::get_id()))
{
}

RExecutionGate::Lease
RExecutionGate::tryAcquire(Cleanup cleanup, Cleanup afterRelease)
{
    if (std::this_thread::get_id() != state_->ownerThread) {
        return Lease();
    }

    bool expected = false;
    if (!state_->active.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return Lease();
    }

    return Lease(
        state_,
        std::move(cleanup),
        std::move(afterRelease));
}

bool
RExecutionGate::canDeferFromCurrentThread() const noexcept
{
    return std::this_thread::get_id() == state_->ownerThread
        && state_->active.load(std::memory_order_acquire);
}
