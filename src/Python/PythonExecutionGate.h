/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_PYTHONEXECUTIONGATE_H
#define GC_PYTHONEXECUTIONGATE_H

#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class PythonExecutionGate final
{
public:
    enum class Admission { Acquired, Cancelled, Busy };

    class Lease final
    {
    public:
        Lease() = default;
        ~Lease();

        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        explicit operator bool() const { return gate_ != nullptr; }

    private:
        friend class PythonExecutionGate;
        explicit Lease(PythonExecutionGate *gate) : gate_(gate) {}
        void reset();

        PythonExecutionGate *gate_ = nullptr;
    };

    Admission acquire(
        bool mustNotWait,
        const std::shared_ptr<std::atomic_bool> &cancelled,
        Lease &lease);
    void wakeWaiters();
    unsigned int waitingCount() const;

    quint64 allocateToken();
    void publishToken(quint64 token);
    bool isPublishedToken(quint64 token) const;

private:
    void release();

    std::mutex mutex_;
    std::condition_variable ready_;
    bool active_ = false;
    std::atomic<unsigned int> waiting_{0};
    std::atomic<quint64> nextToken_{1};
    std::atomic<quint64> publishedToken_{0};
};

#endif
