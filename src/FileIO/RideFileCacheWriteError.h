/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_RideFileCacheWriteError_h
#define _GC_RideFileCacheWriteError_h

#include <QString>

#include <functional>
#include <memory>

class QObject;

class RideFileCacheWriteErrorCoordinator
{
public:
    using Delivery = std::function<void()>;
    using Dispatch = std::function<bool(Delivery)>;
    using Notify = std::function<void(const QString &)>;

    enum class ReportResult {
        Scheduled,
        Coalesced,
        DispatchFailed
    };

    RideFileCacheWriteErrorCoordinator();
    ~RideFileCacheWriteErrorCoordinator();

    RideFileCacheWriteErrorCoordinator(
        const RideFileCacheWriteErrorCoordinator &) = delete;
    RideFileCacheWriteErrorCoordinator &operator=(
        const RideFileCacheWriteErrorCoordinator &) = delete;

    // One coordinator belongs to one owner. Concurrent reports reuse the
    // active owner's dispatcher and notifier for a bounded pending retry.
    ReportResult report(
        const QString &cachePath,
        const QString &detail,
        const Dispatch &dispatch,
        const Notify &notify);

    static bool queueForOwner(
        QObject *owner,
        Delivery delivery);

private:
    struct SharedState;
    std::shared_ptr<SharedState> state_;
};

#endif // _GC_RideFileCacheWriteError_h
