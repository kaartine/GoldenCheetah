/*
 * Copyright (c) 2026 The GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_EstimatorThreadControl_h
#define GC_EstimatorThreadControl_h

#include <QThread>

#include <atomic>

class EstimatorThreadControl
{
public:
    bool prepareForStart(QThread &worker)
    {
        if (worker.isRunning()) return false;
        stopRequested_.store(false, std::memory_order_release);
        return true;
    }

    bool stopAndWait(QThread &worker)
    {
        stopRequested_.store(true, std::memory_order_release);
        if (worker.isRunning()) {
            if (QThread::currentThread() == &worker) return false;
            if (!worker.wait()) return false;
        }
        stopRequested_.store(false, std::memory_order_release);
        return true;
    }

    bool stopRequested() const
    {
        return stopRequested_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool stopRequested_{false};
};

#endif
