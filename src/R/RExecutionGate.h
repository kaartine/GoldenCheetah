/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_REXECUTIONGATE_H
#define GC_REXECUTIONGATE_H

#include <functional>
#include <memory>

class RExecutionGate final
{
private:
    struct State;

public:
    using Cleanup = std::function<void()>;

    class Lease final
    {
    public:
        Lease() = default;
        ~Lease();

        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        explicit operator bool() const noexcept { return state_ != nullptr; }
        bool isValid() const noexcept { return state_ != nullptr; }

    private:
        friend class RExecutionGate;
        Lease(
            std::shared_ptr<State> state,
            Cleanup cleanup,
            Cleanup afterRelease) noexcept;
        void reset() noexcept;

        std::shared_ptr<State> state_;
        Cleanup cleanup_;
        Cleanup afterRelease_;
    };

    // Construction binds the gate to the current (GUI owner) thread.
    RExecutionGate();
    ~RExecutionGate() = default;

    RExecutionGate(const RExecutionGate &) = delete;
    RExecutionGate &operator=(const RExecutionGate &) = delete;
    RExecutionGate(RExecutionGate &&) = delete;
    RExecutionGate &operator=(RExecutionGate &&) = delete;

    // Returns an invalid lease for a nested call or a call from another
    // thread. A valid lease invokes cleanup before making the gate available.
    Lease tryAcquire(Cleanup cleanup, Cleanup afterRelease = {});

private:
    std::shared_ptr<State> state_;
};

#endif
