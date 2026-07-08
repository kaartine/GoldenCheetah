/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PythonExecutionGate.h"

#include <utility>

PythonExecutionGate::Lease::~Lease()
{
    reset();
}

PythonExecutionGate::Lease::Lease(Lease &&other) noexcept
    : gate_(std::exchange(other.gate_, nullptr))
{
}

PythonExecutionGate::Lease &
PythonExecutionGate::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        reset();
        gate_ = std::exchange(other.gate_, nullptr);
    }
    return *this;
}

void
PythonExecutionGate::Lease::reset()
{
    if (!gate_) return;
    PythonExecutionGate *gate = std::exchange(gate_, nullptr);
    gate->release();
}

PythonExecutionGate::Admission
PythonExecutionGate::acquire(
    bool mustNotWait,
    const std::shared_ptr<std::atomic_bool> &cancelled,
    Lease &lease)
{
    const auto cancellationRequested = [&cancelled]() {
        return cancelled
                && cancelled->load(std::memory_order_acquire);
    };

    if (lease) return Admission::Busy;
    if (cancellationRequested()) return Admission::Cancelled;

    std::unique_lock<std::mutex> lock(mutex_);
    if (active_ && mustNotWait) return Admission::Busy;

    if (active_) {
        waiting_.fetch_add(1, std::memory_order_release);
        ready_.wait(lock, [this, &cancellationRequested]() {
            return !active_ || cancellationRequested();
        });
        waiting_.fetch_sub(1, std::memory_order_release);
    }
    if (cancellationRequested()) return Admission::Cancelled;

    active_ = true;
    lease = Lease(this);
    return Admission::Acquired;
}

void
PythonExecutionGate::wakeWaiters()
{
    // Pair with wait's mutex handoff so cancellation cannot notify before
    // a waiter has atomically entered the condition-variable wait.
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.notify_all();
}

unsigned int
PythonExecutionGate::waitingCount() const
{
    return waiting_.load(std::memory_order_acquire);
}

quint64
PythonExecutionGate::allocateToken()
{
    quint64 token = 0;
    while (token == 0) {
        token = nextToken_.fetch_add(1, std::memory_order_relaxed);
    }
    return token;
}

void
PythonExecutionGate::publishToken(quint64 token)
{
    publishedToken_.store(token, std::memory_order_release);
}

bool
PythonExecutionGate::isPublishedToken(quint64 token) const
{
    return token != 0
            && publishedToken_.load(std::memory_order_acquire) == token;
}

void
PythonExecutionGate::release()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
    }
    ready_.notify_all();
}
