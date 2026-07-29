/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCacheSaveCapture.h"

#include "IntervalItem.h"
#include "RideItem.h"

#include <QDeadlineTimer>
#include <QThread>

#include <utility>

namespace RideCacheSave {

namespace {

bool waitForRefreshWorkersUntil(
    const QVector<QThread*> &workers,
    const QDeadlineTimer &deadline,
    QString &error)
{
    for (QThread *worker : workers) {
        if (!worker) continue;
        if (worker == QThread::currentThread()) {
            error = QStringLiteral(
                "Cannot wait for a refresh worker from itself");
            return false;
        }
        if (!worker->wait(deadline)) {
            error = QStringLiteral(
                "Timed out waiting for ride cache refresh");
            return false;
        }
    }
    return true;
}

} // namespace

RefreshBarrierAction refreshBarrierAction(
    qsizetype workerCount,
    bool hasActiveGeneration,
    bool hasPendingGeneration,
    bool stableCaptureBoundary)
{
    if (workerCount > 0) {
        return RefreshBarrierAction::WaitForWorkers;
    }
    if (hasActiveGeneration) {
        return RefreshBarrierAction::Invalid;
    }
    if (hasPendingGeneration && !stableCaptureBoundary) {
        return RefreshBarrierAction::StartPendingRefresh;
    }
    return RefreshBarrierAction::Capture;
}

bool waitForRefreshWorkers(
    const QVector<QThread*> &workers,
    QString &error,
    qint64 timeoutMs)
{
    error.clear();
    if (timeoutMs < 0) {
        error = QStringLiteral(
            "Invalid ride cache refresh timeout");
        return false;
    }
    return waitForRefreshWorkersUntil(
        workers, QDeadlineTimer(timeoutMs), error);
}

bool settleRefreshBarrier(
    const std::function<RefreshBarrierState()> &readState,
    const std::function<void(QThread*)> &completeWorker,
    const std::function<void()> &startPendingRefresh,
    QString &error,
    qint64 timeoutMs)
{
    error.clear();
    if (!readState || !completeWorker
        || !startPendingRefresh || timeoutMs < 0) {
        error = QStringLiteral(
            "Invalid ride cache refresh barrier");
        return false;
    }

    const QDeadlineTimer deadline(timeoutMs);
    for (;;) {
        const RefreshBarrierState state = readState();
        const RefreshBarrierAction action =
            refreshBarrierAction(
            state.workers.size(),
            state.hasActiveGeneration,
            state.hasPendingGeneration,
            state.stableCaptureBoundary);
        switch (action) {
        case RefreshBarrierAction::Capture:
            return true;

        case RefreshBarrierAction::StartPendingRefresh:
            if (deadline.hasExpired()) {
                error = QStringLiteral(
                    "Timed out settling ride cache refresh");
                return false;
            }
            startPendingRefresh();
            continue;

        case RefreshBarrierAction::WaitForWorkers:
            if (!waitForRefreshWorkersUntil(
                    state.workers, deadline, error)) {
                return false;
            }
            for (QThread *worker : state.workers) {
                if (worker) completeWorker(worker);
            }
            continue;

        case RefreshBarrierAction::Invalid:
            error = QStringLiteral(
                "Ride cache refresh has no active worker");
            return false;
        }
    }
}

Snapshot capture(
    const QString &version,
    const QString &targetPath,
    const QVector<MetricDefinition> &metrics,
    const QVector<RideItem*> &rides)
{
    Snapshot snapshot;
    snapshot.version = version;
    snapshot.targetPath = targetPath;
    snapshot.metrics = metrics;
    snapshot.rides.reserve(rides.size());

    for (RideItem *item : rides) {
        if (!item
            || item->metrics().isEmpty()
            || item->isstale
            || item->skipsave) {
            continue;
        }

        Ride ride;
        ride.dateTime = item->dateTime;
        ride.fileName = item->fileName;
        ride.fingerprint = item->fingerprint;
        ride.crc = item->crc;
        ride.metadataCrc = item->metacrc;
        ride.timestamp = item->timestamp;
        ride.databaseVersion = item->dbversion;
        ride.userDatabaseVersion = item->udbversion;
        ride.color = item->color.name();
        ride.present = item->present;
        ride.sport = item->sport;
        ride.aero = item->isAero;
        ride.weight = item->weight;
        ride.zoneRange = item->zoneRange;
        ride.hrZoneRange = item->hrZoneRange;
        ride.paceZoneRange = item->paceZoneRange;
        ride.overrides = item->overrides_;
        ride.samples = item->samples;
        ride.metricValues.values = item->metrics();
        ride.metricValues.counts = item->counts();
        ride.metricValues.stdMeans = item->stdmeans();
        ride.metricValues.stdVariances = item->stdvariances();
        ride.metadata = item->metadata();
        ride.xdata = item->xdata();

        ride.intervals.reserve(item->intervals().size());
        for (IntervalItem *itemInterval : item->intervals()) {
            if (!itemInterval) continue;

            Interval interval;
            interval.name = itemInterval->name;
            interval.start = itemInterval->start;
            interval.stop = itemInterval->stop;
            interval.startKm = itemInterval->startKM;
            interval.stopKm = itemInterval->stopKM;
            interval.type =
                static_cast<int>(itemInterval->type);
            interval.test = itemInterval->test;
            interval.color = itemInterval->color.name();
            if (itemInterval->type == RideFileInterval::ROUTE) {
                interval.route = itemInterval->route.toString();
            }
            interval.sequence = itemInterval->displaySequence;
            interval.metricValues.values =
                itemInterval->metrics();
            interval.metricValues.counts =
                itemInterval->counts();
            interval.metricValues.stdMeans =
                itemInterval->stdmeans();
            interval.metricValues.stdVariances =
                itemInterval->stdvariances();
            ride.intervals.append(std::move(interval));
        }
        snapshot.rides.append(std::move(ride));
    }
    return snapshot;
}

} // namespace RideCacheSave
