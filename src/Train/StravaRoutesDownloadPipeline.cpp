/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaRoutesDownloadPipeline.h"

#include "FileIO/AnchoredFileSystem.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
extern void stravaRoutesPipelineChunkTestHook();
#endif

namespace StravaRoutesDownloadPipeline {

struct StagedRoutePin
{
    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile file;
};

struct StagingArea
{
    ~StagingArea()
    {
        cleanup();
    }

    bool cleanup()
    {
        if (removed) return true;
        bool succeeded = true;
        for (qsizetype index = qsizetype(files.size());
             index > 0; --index) {
            const std::shared_ptr<StagedRoutePin> &pin =
                files.at(size_t(index - 1));
            if (!pin || !pin->file.isValid()) {
                files.erase(files.begin() + (index - 1));
                continue;
            }
            const AnchoredFileSystem::MutationResult removal =
                AnchoredFileSystem::remove(pin->file);
            if (removal.applied()) {
                files.erase(files.begin() + (index - 1));
            } else {
                succeeded = false;
            }
        }
        if (files.empty() && directory.isValid()) {
            const AnchoredFileSystem::MutationResult removal =
                AnchoredFileSystem::removeEmptyDirectory(directory);
            if (removal.applied()) {
                removed = true;
            } else {
                succeeded = false;
            }
        }
        return succeeded;
    }

    QString path;
    AnchoredFileSystem::DirectoryAnchor directory;
    std::vector<std::shared_ptr<StagedRoutePin>> files;
    bool removed = false;
};

namespace {

constexpr int MaximumFiles = 8;
constexpr qint64 MaximumBytes = 32LL * 1024 * 1024;

bool cancellationRequested(const CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

QString boundedError(QString error)
{
    error = error.left(1024);
    for (QChar &character : error) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f)
            character = QLatin1Char(' ');
    }
    return error.simplified();
}

void appendFailure(QList<RouteFailure> &failures,
                   const QString &routeId,
                   const QString &error)
{
    failures.append({
        routeId,
        boundedError(error.isEmpty()
            ? QStringLiteral("The Strava route operation failed.")
            : error)
    });
}

void callWithoutThrowing(const std::function<void()> &operation)
{
    if (!operation) return;
    try {
        operation();
    } catch (...) {
    }
}

} // namespace

struct Runner::SharedState
{
    std::atomic_bool cancelled{false};
    std::atomic_bool operationFailed{false};
    std::atomic_bool complete{false};
    std::mutex mutex;
    std::condition_variable condition;
    Runner *completionTarget = nullptr;
    Completion completion;
};

Runner::Runner(QObject *parent)
    : QObject(parent)
{
}

Runner::~Runner()
{
    cancel();
    const std::shared_ptr<SharedState> state = state_;
    if (state) {
        const std::lock_guard<std::mutex> lock(state->mutex);
        state->completionTarget = nullptr;
        state->completion = {};
    }
    QThread *thread = thread_.data();
    if (!thread) return;
    thread->requestInterruption();
}

bool Runner::start(Operation operation,
                   Completion completion,
                   QObject *threadOwnedObject)
{
    if (!operation || thread_
        || QThread::currentThread() != QObject::thread()) {
        return false;
    }

    if (threadOwnedObject
        && (threadOwnedObject->parent()
            || threadOwnedObject->thread() != QObject::thread())) {
        return false;
    }

    const std::shared_ptr<SharedState> state =
        std::make_shared<SharedState>();
    state->completionTarget = this;
    state->completion = std::move(completion);
    QThread *thread = new QThread;
    QObject *workerContext = new QObject;
    if (threadOwnedObject)
        threadOwnedObject->setParent(workerContext);
    workerContext->moveToThread(thread);
    if (workerContext->thread() != thread) {
        if (threadOwnedObject)
            threadOwnedObject->setParent(nullptr);
        delete workerContext;
        delete thread;
        return false;
    }

    connect(
        thread, &QThread::started,
        workerContext,
        [state,
         operation = std::move(operation)] {
            try {
                operation([state] {
                    return state->cancelled.load(
                        std::memory_order_acquire)
                        || QThread::currentThread()
                            ->isInterruptionRequested();
                });
            } catch (...) {
                state->operationFailed.store(
                    true, std::memory_order_release);
            }
            state->complete.store(true, std::memory_order_release);
            state->condition.notify_all();
            {
                const std::lock_guard<std::mutex> lock(state->mutex);
                if (state->completionTarget) {
                    QMetaObject::invokeMethod(
                        state->completionTarget,
                        [state] {
                            Completion completion;
                            {
                                const std::lock_guard<std::mutex> lock(
                                    state->mutex);
                                completion = state->completion;
                            }
                            if (completion) {
                                completion(
                                    state->operationFailed.load(
                                        std::memory_order_acquire));
                            }
                        },
                        Qt::QueuedConnection);
                }
            }
            QThread::currentThread()->quit();
        });
    connect(
        thread, &QThread::finished,
        workerContext, &QObject::deleteLater);
    connect(
        thread, &QThread::finished,
        thread, &QObject::deleteLater);
    state_ = state;
    thread_ = thread;
    thread->start();
    return true;
}

void Runner::cancel()
{
    if (state_) {
        state_->cancelled.store(
            true, std::memory_order_release);
    }
    if (thread_) thread_->requestInterruption();
}

bool Runner::isRunning() const
{
    return state_
        && !state_->complete.load(std::memory_order_acquire);
}

bool Runner::waitForFinished(unsigned long timeoutMs)
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state
        || state->complete.load(std::memory_order_acquire)) {
        return true;
    }
    QThread *thread = thread_.data();
    if (thread && QThread::currentThread() == thread)
        return false;

    std::unique_lock<std::mutex> lock(state->mutex);
    return state->condition.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [&state] {
            return state->complete.load(std::memory_order_acquire);
        });
}

int maximumFilesPerBatch()
{
    return MaximumFiles;
}

qint64 maximumBytesPerBatch()
{
    return MaximumBytes;
}

void ImportLifecycle::begin()
{
    aborted_ = false;
    closeWhenIdle_ = false;
}

void ImportLifecycle::requestAbort(bool closeWhenIdle)
{
    aborted_ = true;
    closeWhenIdle_ = closeWhenIdle_ || closeWhenIdle;
    ++cancellationRevision_;
}

TransitionGeneration ImportLifecycle::generation() const
{
    return TransitionGeneration(cancellationRevision_);
}

bool ImportLifecycle::accepts(
    const TransitionGeneration &generation) const
{
    return generation.isCurrent(cancellationRevision_);
}

bool notifyFinalizationWithLease(
    std::shared_ptr<void> operationLease,
    const std::function<void()> &notification)
{
    if (!operationLease || !notification) return false;
    notification();
    return bool(operationLease);
}

PreparationCursor::PreparationCursor(
    QList<StagedRoute> routes,
    quint64 cancellationRevision)
    : routes_(std::move(routes))
    , cancellationRevision_(cancellationRevision)
{
}

bool PreparationCursor::takeNext(
    quint64 currentCancellationRevision,
    StagedRoute &route)
{
    if (stopped_ || next_ >= routes_.size()) return false;
    if (currentCancellationRevision != cancellationRevision_) {
        stopped_ = true;
        return false;
    }
    route = routes_.at(next_++);
    return true;
}

bool PreparationCursor::stopped() const
{
    return stopped_;
}

QStringList PreparationCursor::remainingRouteIds() const
{
    QStringList result;
    result.reserve(routes_.size() - next_);
    for (qsizetype index = next_; index < routes_.size(); ++index)
        result.append(routes_.at(index).routeId);
    return result;
}

DownloadBatchResult stageDownloadBatch(
    const QStringList &routeIds,
    const QString &stagingParent,
    const DownloadOperation &download,
    const CancellationCheck &cancelled)
{
    DownloadBatchResult result;
    if (!download || stagingParent.isEmpty()) {
        result.error = QStringLiteral(
            "The Strava route download stage is not configured.");
        return result;
    }

    AnchoredFileSystem::DirectoryAnchor parent;
    QString stagingError;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            stagingParent, parent, stagingError)
        || !parent.pathMatches(stagingError)
        || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
            parent, stagingError)) {
        result.error = QStringLiteral(
            "The Strava route staging directory is unavailable: %1")
                .arg(stagingError);
        return result;
    }

    auto staging = std::make_shared<StagingArea>();
    QString component;
    for (int attempt = 0; attempt < 16; ++attempt) {
        component = QStringLiteral("Strava-Routes-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const AnchoredFileSystem::MutationResult creation =
            AnchoredFileSystem::createPrivateFixedChildDirectory(
                parent, component, staging->directory);
        if (creation.effect
            == AnchoredFileSystem::MutationEffect::AppliedDurable) {
            break;
        }
        staging->directory = {};
        stagingError = creation.error;
    }
    if (!staging->directory.isValid()) {
        result.error = QStringLiteral(
            "The Strava route staging directory could not be created: %1")
                .arg(stagingError);
        return result;
    }
    staging->path = QDir(stagingParent).filePath(component);

    const int attempts = qMin(routeIds.size(), MaximumFiles);
    qint64 stagedBytes = 0;
    for (int index = 0; index < attempts; ++index) {
        if (cancellationRequested(cancelled)) {
            result.cancelled = true;
            result.remainingRouteIds = routeIds.mid(index);
            break;
        }

        const QString routeId = routeIds.at(index);
        const QString fileName =
            StravaRoutesClient::workoutFileName(routeId);
        if (fileName.isEmpty()) {
            appendFailure(
                result.failures, routeId,
                QStringLiteral("The Strava route ID is invalid."));
            continue;
        }

        StravaRoutesClient::PayloadResult payload;
        try {
            payload = download(routeId, cancelled);
        } catch (...) {
            payload.error = QStringLiteral(
                "The Strava route download failed.");
        }
        if (cancellationRequested(cancelled)) {
            result.cancelled = true;
            result.remainingRouteIds = routeIds.mid(index);
            break;
        }
        if (!payload.isValid()) {
            appendFailure(
                result.failures, routeId, payload.error);
            continue;
        }

        if (payload.payload.size() > MaximumBytes) {
            appendFailure(
                result.failures, routeId,
                QStringLiteral(
                    "The Strava route exceeds the batch byte limit."));
            continue;
        }
        if (stagedBytes > MaximumBytes - payload.payload.size()) {
            result.remainingRouteIds = routeIds.mid(index);
            break;
        }

        QString writeError;
        const AnchoredFileSystem::EntryRef destination =
            staging->directory.entry(fileName, writeError);
        auto pin = std::make_shared<StagedRoutePin>();
        pin->entry = destination;
        if (!destination.isValid()
            || !AnchoredFileSystem::writeNewFile(
                payload.payload, destination, pin->file, writeError)) {
            appendFailure(
                result.failures, routeId,
                writeError.isEmpty()
                    ? QStringLiteral(
                        "The Strava route could not be staged.")
                    : writeError);
            continue;
        }
        const QString path = QDir(staging->path).filePath(fileName);
        result.staged.append({
            routeId, path, payload.payload.size(),
            pin->file.sha256(), pin
        });
        staging->files.push_back(std::move(pin));
        stagedBytes += payload.payload.size();
    }

    if (!result.cancelled && result.remainingRouteIds.isEmpty()
        && routeIds.size() > attempts) {
        result.remainingRouteIds = routeIds.mid(attempts);
    }
    if (!result.staged.isEmpty()) {
        result.stagingDirectory = staging->path;
        result.stagingArea = std::move(staging);
    }
    return result;
}

bool removeStagingDirectory(const DownloadBatchResult &result)
{
    if (!result.stagingArea)
        return result.stagingDirectory.isEmpty();
    return result.stagingArea->cleanup();
}

bool pinRoutePredecessor(
    const AnchoredFileSystem::EntryRef &entry,
    AnchoredFileSystem::PinnedFile &file,
    QString &error,
    const CancellationCheck &cancelled)
{
    if (cancellationRequested(cancelled)) {
        error = QStringLiteral(
            "The Strava route predecessor validation was cancelled.");
        return false;
    }
    return AnchoredFileSystem::pinRegularFile(
        entry, file, error, MaximumBytes,
        [&cancelled](qint64, QString &controlError) {
            if (!cancellationRequested(cancelled)) return true;
            controlError = QStringLiteral(
                "The Strava route predecessor validation was cancelled.");
            return false;
        });
}

bool withPinnedRouteBytes(
    const StagedRoute &route,
    const PinnedRouteBytesOperation &operation,
    QString &error,
    const CancellationCheck &cancelled)
{
    error.clear();
    const QFileInfo sourceInfo(route.path);
    AnchoredFileSystem::DirectoryAnchor sourceDirectory;
    if (!operation || sourceInfo.fileName().isEmpty()
        || !route.pin || !route.pin->file.isValid()
        || sourceInfo.absoluteFilePath()
            != QFileInfo(route.pin->entry.displayPath())
                .absoluteFilePath()
        || route.bytes < 0
        || route.bytes > MaximumBytes
        || route.digest.size()
            != QCryptographicHash::hashLength(
                QCryptographicHash::Sha256)
        || route.pin->file.size() != route.bytes
        || route.pin->file.sha256() != route.digest
        || !AnchoredFileSystem::DirectoryAnchor::open(
            sourceInfo.absolutePath(), sourceDirectory, error)
        || !sourceDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The staged Strava route is unavailable.");
        }
        return false;
    }

    const AnchoredFileSystem::EntryRef sourceEntry =
        sourceDirectory.entry(sourceInfo.fileName(), error);
    bool sourceMatches = false;
    if (!sourceEntry.isValid()
        || !AnchoredFileSystem::entryMatches(
            sourceEntry, route.pin->file, sourceMatches, error)
        || !sourceMatches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The staged Strava route changed unexpectedly.");
        }
        return false;
    }

    QByteArray contents;
    contents.reserve(qsizetype(route.bytes));
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!AnchoredFileSystem::streamContents(
            route.pin->file,
            [&contents, &digest, &cancelled](
                const char *data, qsizetype size, QString &chunkError) {
#ifdef GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS
                ::stravaRoutesPipelineChunkTestHook();
#endif
                if (cancellationRequested(cancelled)) {
                    chunkError = QStringLiteral(
                        "The staged Strava route validation was cancelled.");
                    return false;
                }
                contents.append(data, size);
                digest.addData(QByteArrayView(data, size));
                return true;
            },
            error)
        || cancellationRequested(cancelled)
        || contents.size() != route.bytes
        || digest.result() != route.digest
        || !operation(contents, error)) {
        if (error.isEmpty() && cancellationRequested(cancelled)) {
            error = QStringLiteral(
                "The staged Strava route validation was cancelled.");
        }
        return false;
    }

    sourceMatches = false;
    if (!AnchoredFileSystem::entryMatches(
            sourceEntry, route.pin->file, sourceMatches, error)
        || !sourceMatches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The staged Strava route changed during validation.");
        }
        return false;
    }
    return true;
}

ImportBatchResult importBatch(
    const QList<StagedRoute> &routes,
    const ImportCallbacks &callbacks,
    const CancellationCheck &cancelled)
{
    ImportBatchResult result;
    if (!callbacks.prepare
        || !callbacks.beginTransaction
        || !callbacks.importRoute
        || !callbacks.commitTransaction
        || !callbacks.rollbackTransaction
        || !callbacks.rollbackFiles
        || !callbacks.finalizeFiles) {
        result.error = QStringLiteral(
            "The Strava route import stage is not configured.");
        return result;
    }

    QList<StagedRoute> prepared;
    for (const StagedRoute &route : routes) {
        if (cancellationRequested(cancelled)) {
            result.cancelled = true;
            callWithoutThrowing(callbacks.rollbackFiles);
            return result;
        }
        QString error;
        bool succeeded = false;
        try {
            succeeded = callbacks.prepare(route, error);
        } catch (...) {
            error = QStringLiteral(
                "The Strava route preparation failed.");
        }
        if (!succeeded) {
            appendFailure(result.failures, route.routeId, error);
            continue;
        }
        prepared.append(route);
    }

    if (prepared.isEmpty()) {
        callWithoutThrowing(callbacks.finalizeFiles);
        return result;
    }
    if (cancellationRequested(cancelled)) {
        result.cancelled = true;
        callWithoutThrowing(callbacks.rollbackFiles);
        return result;
    }

    bool transactionActive = false;
    try {
        transactionActive = callbacks.beginTransaction();
    } catch (...) {
        transactionActive = false;
    }
    if (!transactionActive) {
        result.error = QStringLiteral(
            "The Strava route import transaction could not be started.");
        callWithoutThrowing(callbacks.rollbackFiles);
        return result;
    }

    bool imported = true;
    QString importError;
    for (const StagedRoute &route : prepared) {
        if (cancellationRequested(cancelled)) {
            result.cancelled = true;
            imported = false;
            break;
        }
        try {
            if (!callbacks.importRoute(route, importError)) {
                imported = false;
                break;
            }
        } catch (...) {
            importError = QStringLiteral(
                "The Strava route database import failed.");
            imported = false;
            break;
        }
    }

    bool committed = false;
    if (imported) {
        try {
            committed = callbacks.commitTransaction();
        } catch (...) {
            committed = false;
        }
    }
    if (!imported || !committed) {
        callWithoutThrowing(callbacks.rollbackTransaction);
        callWithoutThrowing(callbacks.rollbackFiles);
        if (!result.cancelled) {
            result.error = boundedError(
                importError.isEmpty()
                    ? QStringLiteral(
                        "The Strava route import transaction failed.")
                    : importError);
        }
        return result;
    }

    callWithoutThrowing(callbacks.finalizeFiles);
    for (const StagedRoute &route : prepared)
        result.importedRouteIds.append(route.routeId);
    return result;
}

ImportBatchResult importCompletedPrefix(
    const DownloadBatchResult &download,
    const ImportCallbacks &callbacks)
{
    if (!download.isValid()) {
        ImportBatchResult result;
        result.error = download.error;
        return result;
    }
    return importBatch(download.staged, callbacks);
}

} // namespace StravaRoutesDownloadPipeline
