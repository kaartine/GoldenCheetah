/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHESAVECAPTURE_H
#define GC_RIDECACHESAVECAPTURE_H

#include "RideCacheSaveSnapshot.h"

#include <functional>

class RideItem;
class QThread;

namespace RideCacheSave {

enum class RefreshBarrierAction {
    WaitForWorkers,
    StartPendingRefresh,
    Capture,
    Invalid
};

inline constexpr qint64 RefreshBarrierTimeoutMs = 30000;

struct RefreshBarrierState
{
    QVector<QThread*> workers;
    bool hasActiveGeneration = false;
    bool hasPendingGeneration = false;
    bool stableCaptureBoundary = false;
};

RefreshBarrierAction refreshBarrierAction(
    qsizetype workerCount,
    bool hasActiveGeneration,
    bool hasPendingGeneration,
    bool stableCaptureBoundary);

bool waitForRefreshWorkers(
    const QVector<QThread*> &workers,
    QString &error,
    qint64 timeoutMs = RefreshBarrierTimeoutMs);

bool settleRefreshBarrier(
    const std::function<RefreshBarrierState()> &readState,
    const std::function<void(QThread*)> &completeWorker,
    const std::function<void()> &startPendingRefresh,
    QString &error,
    qint64 timeoutMs = RefreshBarrierTimeoutMs);

Snapshot capture(
    const QString &version,
    const QString &targetPath,
    const QVector<MetricDefinition> &metrics,
    const QVector<RideItem*> &rides);

} // namespace RideCacheSave

#endif // GC_RIDECACHESAVECAPTURE_H
