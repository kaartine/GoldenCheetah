/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_ROUTES_DOWNLOAD_PIPELINE_H
#define GC_STRAVA_ROUTES_DOWNLOAD_PIPELINE_H

#include "StravaRoutesClient.h"
#include "FileIO/AnchoredFileSystem.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QThread;

namespace StravaRoutesDownloadPipeline {

using CancellationCheck = std::function<bool()>;

struct StagingArea;
struct StagedRoutePin;

class Runner final : public QObject
{
public:
    using Operation = std::function<void(const CancellationCheck &)>;
    using Completion = std::function<void(bool operationFailed)>;

    explicit Runner(QObject *parent = nullptr);
    ~Runner() override;

    bool start(Operation operation,
               Completion completion,
               QObject *threadOwnedObject = nullptr);
    void cancel();
    bool isRunning() const;
    bool waitForFinished(unsigned long timeoutMs);

private:
    struct SharedState;

    std::shared_ptr<SharedState> state_;
    QPointer<QThread> thread_;
};

struct StagedRoute
{
    QString routeId;
    QString path;
    qint64 bytes = 0;
    QByteArray digest;
    std::shared_ptr<StagedRoutePin> pin;
};

class PreparationCursor
{
public:
    // Only cancellation events newer than the completed download stop
    // preparation, so a downloaded prefix remains eligible for import.
    PreparationCursor(
        QList<StagedRoute> routes,
        quint64 cancellationRevision);

    bool takeNext(
        quint64 currentCancellationRevision,
        StagedRoute &route);
    bool stopped() const;
    QStringList remainingRouteIds() const;

private:
    QList<StagedRoute> routes_;
    qsizetype next_ = 0;
    quint64 cancellationRevision_ = 0;
    bool stopped_ = false;
};

struct RouteFailure
{
    QString routeId;
    QString error;
};

struct DownloadBatchResult
{
    QList<StagedRoute> staged;
    QList<RouteFailure> failures;
    QStringList remainingRouteIds;
    QString stagingDirectory;
    std::shared_ptr<StagingArea> stagingArea;
    QString error;
    bool cancelled = false;

    bool isValid() const { return error.isEmpty(); }
};

using DownloadOperation =
    std::function<StravaRoutesClient::PayloadResult(
        const QString &routeId,
        const CancellationCheck &cancelled)>;
using PinnedRouteBytesOperation =
    std::function<bool(const QByteArray &contents, QString &error)>;

int maximumFilesPerBatch();
qint64 maximumBytesPerBatch();

class TransitionGeneration
{
public:
    explicit TransitionGeneration(quint64 cancellationRevision)
        : cancellationRevision_(cancellationRevision)
    {
    }

    bool isCurrent(quint64 currentCancellationRevision) const
    {
        return currentCancellationRevision == cancellationRevision_;
    }

private:
    quint64 cancellationRevision_ = 0;
};

class ImportLifecycle
{
public:
    void begin();
    void requestAbort(bool closeWhenIdle);
    TransitionGeneration generation() const;
    bool accepts(const TransitionGeneration &generation) const;
    bool aborted() const { return aborted_; }
    bool closeWhenIdle() const { return closeWhenIdle_; }

private:
    quint64 cancellationRevision_ = 0;
    bool aborted_ = false;
    bool closeWhenIdle_ = false;
};

bool notifyFinalizationWithLease(
    std::shared_ptr<void> operationLease,
    const std::function<void()> &notification);

DownloadBatchResult stageDownloadBatch(
    const QStringList &routeIds,
    const QString &stagingParent,
    const DownloadOperation &download,
    const CancellationCheck &cancelled = {});
bool removeStagingDirectory(const DownloadBatchResult &result);
bool pinRoutePredecessor(
    const AnchoredFileSystem::EntryRef &entry,
    AnchoredFileSystem::PinnedFile &file,
    QString &error,
    const CancellationCheck &cancelled = {});
bool withPinnedRouteBytes(
    const StagedRoute &route,
    const PinnedRouteBytesOperation &operation,
    QString &error,
    const CancellationCheck &cancelled = {});

struct ImportCallbacks
{
    std::function<bool(const StagedRoute &, QString &error)> prepare;
    std::function<bool()> beginTransaction;
    std::function<bool(const StagedRoute &, QString &error)> importRoute;
    std::function<bool()> commitTransaction;
    std::function<void()> rollbackTransaction;
    std::function<void()> rollbackFiles;
    std::function<void()> finalizeFiles;
};

struct ImportBatchResult
{
    QStringList importedRouteIds;
    QList<RouteFailure> failures;
    QString error;
    bool cancelled = false;

    bool isValid() const { return error.isEmpty(); }
};

ImportBatchResult importBatch(
    const QList<StagedRoute> &routes,
    const ImportCallbacks &callbacks,
    const CancellationCheck &cancelled = {});
ImportBatchResult importCompletedPrefix(
    const DownloadBatchResult &download,
    const ImportCallbacks &callbacks);

} // namespace StravaRoutesDownloadPipeline

#endif
