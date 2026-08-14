/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Cloud/StravaTokenRefresh.h"
#include "FileIO/AnchoredFileSystem.h"
#include "FileIO/GpxParser.h"
#include "Planning/PlanBundleImportJournal.h"
#include "Train/ErgFile.h"
#include "Train/StravaRoutesClient.h"
#include "Train/StravaRoutesDownload.h"
#include "Train/StravaRoutesDownloadPipeline.h"
#include "Train/TrainDB.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using StravaRoutesDownloadPipeline::DownloadBatchResult;
using StravaRoutesDownloadPipeline::ImportBatchResult;
using StravaRoutesDownloadPipeline::ImportCallbacks;
using StravaRoutesDownloadPipeline::Runner;
using StravaRoutesDownloadPipeline::StagedRoute;

extern std::atomic_bool gpxSettingsReadBlocked;
extern std::atomic_bool gpxSettingsReadEntered;
extern std::atomic_int gpxSettingsReadCalls;

namespace {
std::atomic_int routeChunkDelayMs{0};
std::atomic_int gpxParserDelayMs{0};
std::atomic_int routeChunkVisits{0};
std::atomic_int gpxParserVisits{0};
std::atomic_int gpxInterpolationVisits{0};
std::atomic<qsizetype> gpxPointLimitOverride{0};
std::mutex backingPathMutex;
QString lastErgFileBackingPath;
std::mutex publicationMutationActionMutex;
std::function<void()> publicationMutationAction;

template<typename Value>
class SynchronizedResult
{
public:
    void store(Value next)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::move(next);
    }

    Value load() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

private:
    mutable std::mutex mutex_;
    Value value_{};
};
}

void stravaRoutesPipelineChunkTestHook()
{
    routeChunkVisits.fetch_add(1, std::memory_order_release);
    const int delay = routeChunkDelayMs.load(std::memory_order_acquire);
    if (delay > 0) QThread::msleep(static_cast<unsigned long>(delay));
}

void ergFileGpxParserProgressTestHook()
{
    gpxParserVisits.fetch_add(1, std::memory_order_release);
    const int delay = gpxParserDelayMs.load(std::memory_order_acquire);
    if (delay > 0) QThread::msleep(static_cast<unsigned long>(delay));
}

void ergFileGpxParserInterpolationTestHook()
{
    gpxInterpolationVisits.fetch_add(1, std::memory_order_release);
}

qsizetype ergFileGpxParserPointLimitTestOverride()
{
    return gpxPointLimitOverride.load(std::memory_order_acquire);
}

void planBundleImportPublicationChunkTestHook()
{
}

void planBundleImportPublicationMutationTestHook()
{
    std::function<void()> action;
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        action = publicationMutationAction;
    }
    if (action) action();
}

void ergFileByteBackingPathTestHook(const QString &path)
{
    const std::lock_guard<std::mutex> lock(backingPathMutex);
    lastErgFileBackingPath = path;
}

namespace {

std::atomic_int dialogFinishStagingCleanupVisits{0};

using namespace std::chrono_literals;

struct Gate
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct DestructionState
{
    std::atomic_bool destroyed{false};
    std::atomic_bool destroyedOffGuiThread{false};
};

struct ServiceTreeState
{
    std::atomic_bool rootDestroyed{false};
    std::atomic_bool managerDestroyed{false};
    std::atomic_bool replyDestroyed{false};
    std::atomic_bool teardownStayedOnWorker{true};
};

class DestructionProbe final : public QObject
{
public:
    explicit DestructionProbe(
        std::shared_ptr<DestructionState> state)
        : state(std::move(state))
    {
    }

    ~DestructionProbe() override
    {
        state->destroyedOffGuiThread.store(
            QThread::currentThread() != qApp->thread(),
            std::memory_order_release);
        state->destroyed.store(true, std::memory_order_release);
    }

private:
    std::shared_ptr<DestructionState> state;
};

class TrackedNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit TrackedNetworkAccessManager(
        const std::shared_ptr<ServiceTreeState> &state,
        QObject *parent)
        : QNetworkAccessManager(parent)
        , state(state)
    {
    }

    ~TrackedNetworkAccessManager() override
    {
        state->teardownStayedOnWorker.store(
            state->teardownStayedOnWorker.load(
                std::memory_order_acquire)
                && QThread::currentThread() != qApp->thread(),
            std::memory_order_release);
        state->managerDestroyed.store(true, std::memory_order_release);
    }

private:
    std::shared_ptr<ServiceTreeState> state;
};

class NetworkServiceTree final : public QObject
{
public:
    explicit NetworkServiceTree(
        std::shared_ptr<ServiceTreeState> state)
        : state(std::move(state))
        , manager(new TrackedNetworkAccessManager(this->state, this))
    {
    }

    ~NetworkServiceTree() override
    {
        state->teardownStayedOnWorker.store(
            state->teardownStayedOnWorker.load(
                std::memory_order_acquire)
                && QThread::currentThread() != qApp->thread(),
            std::memory_order_release);
        state->rootDestroyed.store(true, std::memory_order_release);
    }

    std::shared_ptr<ServiceTreeState> state;
    TrackedNetworkAccessManager *manager;
};

QString uniqueValue(const QString &prefix)
{
    return prefix + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
        ? file.readAll() : QByteArray();
}

bool waitForLeader(Gate &gate)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.condition.wait_for(lock, 1s, [&gate] {
        return gate.entered;
    });
}

void releaseLeader(Gate &gate)
{
    {
        const std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();
}

StravaRoutesClient::PayloadResult validPayload(
    const QString &routeId)
{
    StravaRoutesClient client(
        [routeId](
            const QUrl &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            StravaAuthenticatedSession::Result response;
            response.payload = QByteArrayLiteral(
                "<?xml version=\"1.0\"?><gpx version=\"1.1\"><rte><name>")
                + routeId.toUtf8()
                + QByteArrayLiteral("</name></rte></gpx>");
            response.contentType =
                QStringLiteral("application/gpx+xml");
            return response;
        });
    return client.downloadGpx(routeId);
}

QList<StagedRoute> routes(int count)
{
    QList<StagedRoute> result;
    for (int index = 1; index <= count; ++index) {
        result.append({
            QString::number(index),
            QStringLiteral("/staged/%1.gpx").arg(index),
            128
        });
    }
    return result;
}

QByteArray routeGpx(int pointCount = 12, int intervalSeconds = 1)
{
    QByteArray contents(
        "<?xml version=\"1.0\"?><gpx version=\"1.1\" "
        "xmlns=\"http://www.topografix.com/GPX/1/1\"><trk><trkseg>");
    const QDateTime start(
        QDate(2026, 1, 1), QTime(12, 0), Qt::UTC);
    for (int index = 0; index < pointCount; ++index) {
        contents += QStringLiteral(
            "<trkpt lat=\"60.%1\" lon=\"24.%2\"><ele>%3</ele>"
            "<time>%4</time></trkpt>")
            .arg(100000 + index)
            .arg(900000 + index)
            .arg(100 + index)
            .arg(start.addSecs(
                qint64(index) * intervalSeconds).toString(Qt::ISODate))
            .toUtf8();
    }
    contents += "</trkseg></trk></gpx>";
    return contents;
}

QStringList decisionMarkers(const QString &workoutRoot)
{
    return QDir(workoutRoot).entryList(
        QStringList{QStringLiteral(".gc-plan-import-decision-*")},
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDir::Name);
}

StravaRoutesDownloadProductionTest::ImportOperationPtr
prepareCommittedPublishedOperation(
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QString &stagingRoot,
    TrainDB &database,
    QString &error)
{
    const DownloadBatchResult downloadResult =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("42")}, stagingRoot,
            [](const QString &,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                StravaRoutesClient::PayloadResult result;
                result.payload = routeGpx();
                return result;
            });
    if (!downloadResult.isValid() || downloadResult.staged.size() != 1) {
        error = downloadResult.error;
        return {};
    }
    auto operation = StravaRoutesDownloadProductionTest::prepareImport(
        downloadResult, athleteRoot, workoutRoot, false,
        GpxParser::captureOptions(), {}, error);
    bool committed = false;
    if (!operation
        || !StravaRoutesDownloadProductionTest::commitDecision(
            operation, database.databaseFilePath(), {},
            committed, error)
        || !committed
        || !StravaRoutesDownloadProductionTest::publish(
            operation, {}, error)) {
        return {};
    }
    return operation;
}

} // namespace

void stravaRoutesDownloadFinishStagingCleanupTestHook()
{
    dialogFinishStagingCleanupVisits.fetch_add(
        1, std::memory_order_relaxed);
}

class TestStravaRoutesDownloadPipeline : public QObject
{
    Q_OBJECT

private slots:
    void routesFollowerPostedCancellationIsPrompt();
    void completionReturnsToGuiThread();
    void cancellationStopsWorker();
    void runnerTeardownCancelsCooperativeWorkerPromptly();
    void runnerTeardownDoesNotJoinNonCooperativeOperation();
    void workerObjectTreeHasWorkerAffinity();
    void workerOwnedObjectIsDestroyedOnWorkerThread();
    void productionNetworkServiceTreeTearsDownOnWorkerThread();
    void thrownOperationIsReported();
    void downloadsAreStagedInBoundedBatches();
    void downloadsRespectAggregateByteBudget();
    void oversizedPayloadIsRejectedWithoutRetry();
    void stagingCleanupRejectsForeignDirectorySwap();
    void stagingCleanupRejectsSymlinkSwap();
    void pinnedRouteBytesSurviveStagingParentSwap();
    void stagedRouteRejectsSameSizedPrevalidationReplacement();
    void downloadCancellationKeepsCompletedPrefix();
    void cancelledBatchCommitsCompletedPrefixOnce();
    void preparationCancellationCommitsTakenPrefixOnce();
    void downloadErrorsDoNotDiscardValidRoutes();
    void multiRouteImportUsesOneShortTransaction();
    void moreThanEightRoutesUseOneTransactionPerBoundedBatch();
    void prepareFailuresAreSkipped();
    void cancellationBeforeTransactionRollsBackFiles();
    void beginFailureRollsBackFiles();
    void importFailureRollsBackTransactionAndFiles();
    void commitFailureRollsBackTransactionAndFiles();
    void delayedPinnedReadCancellationKeepsGuiResponsive();
    void delayedGpxParserCancellationKeepsGuiResponsive();
    void excessiveGpxHighWaterMarkIsRejected();
    void generatedGpxPointBudgetIsEnforced();
    void interpolationCancellationIsPrompt();
    void gpxValidationCancellationIsCooperative();
    void capturedGpxOptionsOutliveSettingsOwner();
    void oversizedPredecessorAbortIsPrompt();
    void byteBackedErgFileSurvivesParserTemporaryCleanup();
    void multiRouteTrainDbLuwCommitsAsOneUnit();
    void multiRouteTrainDbLuwRollsBackAsOneUnit();
    void productionMultiRouteCommitUsesOneDatabaseLuw();
    void databaseReplacementBeforePublicationRetainsDecisionMarker();
    void productionCompositionAbortCloseRecovers();
    void productionDialogAbortAndCloseUseActualSlots();
    void queuedCancellationBlocksLaterAsyncStages();
    void workerFinalizationKeepsOwnerEventLoopResponsive();
    void reentrantDataChangedCanReleaseCallerOperation();
    void productionDialogFinalizationSurvivesReentrantDeletion();
};

void TestStravaRoutesDownloadPipeline::
queuedCancellationBlocksLaterAsyncStages()
{
    StravaRoutesDownloadPipeline::ImportLifecycle lifecycle;
    lifecycle.begin();
    const auto generation = lifecycle.generation();
    int preparationStarts = 0;
    int decisionStarts = 0;
    bool callbacksDelivered = false;
    QTimer::singleShot(0, [&] {
        if (lifecycle.accepts(generation))
            ++preparationStarts;
        QTimer::singleShot(0, [&] {
            if (lifecycle.accepts(generation))
                ++decisionStarts;
            callbacksDelivered = true;
        });
    });

    lifecycle.requestAbort(false);
    QTRY_VERIFY_WITH_TIMEOUT(callbacksDelivered, 1000);
    QCOMPARE(preparationStarts, 0);
    QCOMPARE(decisionStarts, 0);
}

void TestStravaRoutesDownloadPipeline::
workerFinalizationKeepsOwnerEventLoopResponsive()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QString error;
    auto operation = prepareCommittedPublishedOperation(
        athleteRoot, workoutRoot, stagingRoot, database, error);
    QVERIFY2(operation, qPrintable(error));

    auto result = std::make_shared<SynchronizedResult<bool>>();
    auto cleaned = std::make_shared<SynchronizedResult<bool>>();
    auto workerError = std::make_shared<SynchronizedResult<QString>>();
    bool completed = false;
    Runner runner;
    QVERIFY(runner.start(
        [operation, result, cleaned, workerError](
            const StravaRoutesDownloadPipeline::CancellationCheck &) {
            bool stagingCleaned = false;
            QString error;
            result->store(StravaRoutesDownloadProductionTest::finalize(
                operation, stagingCleaned, error));
            cleaned->store(stagingCleaned);
            workerError->store(error);
        },
        [&](bool) { completed = true; }));

    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QVERIFY(heartbeats > 0);
    QVERIFY2(result->load(), qPrintable(workerError->load()));
    QVERIFY(cleaned->load());
    const QString target =
        QDir(QFileInfo(workoutRoot).canonicalFilePath()).filePath(
            StravaRoutesClient::workoutFileName(QStringLiteral("42")));
    QVERIFY(database.hasWorkout(target));
}

void TestStravaRoutesDownloadPipeline::
reentrantDataChangedCanReleaseCallerOperation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QString error;
    auto operation = prepareCommittedPublishedOperation(
        athleteRoot, workoutRoot, stagingRoot, database, error);
    QVERIFY2(operation, qPrintable(error));
    bool stagingCleaned = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::finalize(
        operation, stagingCleaned, error), qPrintable(error));
    QVERIFY(stagingCleaned);

    std::weak_ptr<StravaRoutesDownloadProductionTest::ImportOperation>
        weakOperation = operation;
    QPointer<QObject> owner = new QObject;
    bool leaseAliveInNotification = false;
    connect(&database, &TrainDB::dataChanged, [&] {
        operation.reset();
        leaseAliveInNotification = !weakOperation.expired();
        delete owner.data();
    });
    QVERIFY(StravaRoutesDownloadProductionTest::notifyFinalization(
        operation, [&] { database.dataChanged(); }));

    QVERIFY(!operation);
    QVERIFY(owner.isNull());
    QVERIFY(leaseAliveInNotification);
    QVERIFY(weakOperation.expired());
}

void TestStravaRoutesDownloadPipeline::
productionDialogFinalizationSurvivesReentrantDeletion()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QString error;
    auto operation = prepareCommittedPublishedOperation(
        athleteRoot, workoutRoot, stagingRoot, database, error);
    QVERIFY2(operation, qPrintable(error));
    bool stagingCleaned = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::finalize(
        operation, stagingCleaned, error), qPrintable(error));
    QVERIFY(stagingCleaned);

    QPointer<StravaRoutesDownload> dialog =
        StravaRoutesDownloadProductionTest::createDialog();
    QVERIFY(dialog);
    StravaRoutesDownloadProductionTest::attachOperation(
        dialog, operation);
    bool destroyedReentrantly = false;
    connect(&database, &TrainDB::dataChanged, [&] {
        destroyedReentrantly = true;
        delete dialog.data();
    });
    TrainDB *const previousDatabase = trainDB;
    trainDB = &database;
    dialogFinishStagingCleanupVisits.store(
        0, std::memory_order_relaxed);
    StravaRoutesDownloadProductionTest::finishDialogFinalization(
        dialog, true, true, QString(), false);
    trainDB = previousDatabase;

    QVERIFY(destroyedReentrantly);
    QVERIFY(dialog.isNull());
    QCOMPARE(
        dialogFinishStagingCleanupVisits.load(
            std::memory_order_relaxed),
        0);
}

void TestStravaRoutesDownloadPipeline::
productionDialogAbortAndCloseUseActualSlots()
{
    QPointer<StravaRoutesDownload> dialog =
        StravaRoutesDownloadProductionTest::createDialog();
    QVERIFY(dialog);

    StravaRoutesDownloadProductionTest::configureBusy(dialog, true);
    StravaRoutesDownloadProductionTest::clickAbort(dialog);
    QVERIFY(StravaRoutesDownloadProductionTest::aborted(dialog));
    QVERIFY(!StravaRoutesDownloadProductionTest::closeWhenIdle(dialog));

    StravaRoutesDownloadProductionTest::configureBusy(dialog, true);
    StravaRoutesDownloadProductionTest::clickClose(dialog);
    QVERIFY(StravaRoutesDownloadProductionTest::aborted(dialog));
    QVERIFY(StravaRoutesDownloadProductionTest::closeWhenIdle(dialog));

    StravaRoutesDownloadProductionTest::configureBusy(dialog, true);
    QCloseEvent event;
    QApplication::sendEvent(dialog, &event);
    QVERIFY(!event.isAccepted());
    QVERIFY(StravaRoutesDownloadProductionTest::aborted(dialog));
    QVERIFY(StravaRoutesDownloadProductionTest::closeWhenIdle(dialog));
    delete dialog.data();
}

void TestStravaRoutesDownloadPipeline::
routesFollowerPostedCancellationIsPrompt()
{
    const QString account = uniqueValue(QStringLiteral("routes-account"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("routes-refresh"));
    Gate gate;

    auto leader = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            refreshToken,
            [&](const QString &effectiveToken) {
                {
                    const std::lock_guard<std::mutex> lock(gate.mutex);
                    gate.entered = true;
                }
                gate.condition.notify_all();
                std::unique_lock<std::mutex> lock(gate.mutex);
                gate.condition.wait_for(lock, 2s, [&gate] {
                    return gate.released;
                });
                return StravaTokenRefreshResult{
                    true,
                    QStringLiteral("access"),
                    QStringLiteral("rotated-refresh"),
                    QString(),
                    effectiveToken,
                    QString()
                };
            },
            {},
            std::chrono::milliseconds(0));
    });
    QVERIFY2(waitForLeader(gate), "The refresh leader did not start.");

    std::atomic_bool followerEntered{false};
    std::mutex resultMutex;
    StravaRoutesClient::RoutesResult routesResult;
    bool finished = false;
    bool completedOnGuiThread = false;
    Runner runner;
    QVERIFY(runner.start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            StravaRoutesClient client(
                [&](const QUrl &,
                    qsizetype,
                    const StravaRoutesClient::CancellationCheck &check) {
                    followerEntered.store(true, std::memory_order_release);
                    const StravaTokenRefreshResult result =
                        StravaTokenRefreshCoordinator::refresh(
                            account,
                            refreshToken,
                            [](const QString &) {
                                StravaTokenRefreshResult unexpected;
                                unexpected.error = QStringLiteral(
                                    "A follower started a second refresh.");
                                return unexpected;
                            },
                            check,
                            std::chrono::milliseconds(0));
                    StravaAuthenticatedSession::Result response;
                    response.error = result.error;
                    return response;
                });
            const auto value = client.listRoutes(cancelled);
            const std::lock_guard<std::mutex> lock(resultMutex);
            routesResult = value;
        },
        [&](bool operationFailed) {
            QVERIFY(!operationFailed);
            completedOnGuiThread =
                QThread::currentThread() == qApp->thread();
            finished = true;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(
        followerEntered.load(std::memory_order_acquire), 500);

    QElapsedTimer elapsed;
    elapsed.start();
    QTimer::singleShot(0, &runner, [&runner] {
        runner.cancel();
    });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);

    QVERIFY2(
        elapsed.elapsed() < 400,
        qPrintable(QStringLiteral(
            "The asynchronous Routes follower cancelled in %1 ms")
            .arg(elapsed.elapsed())));
    QVERIFY(completedOnGuiThread);
    QCOMPARE(
        leader.wait_for(0ms),
        std::future_status::timeout);
    {
        const std::lock_guard<std::mutex> lock(resultMutex);
        QVERIFY(!routesResult.isValid());
        QVERIFY(routesResult.error.contains(
            QStringLiteral("cancel"), Qt::CaseInsensitive));
    }

    releaseLeader(gate);
    QVERIFY(leader.get().isValid());
}

void TestStravaRoutesDownloadPipeline::completionReturnsToGuiThread()
{
    bool finished = false;
    bool onGuiThread = false;
    std::atomic_bool ran{false};
    Runner runner;

    QVERIFY(runner.start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &) {
            ran.store(true, std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            onGuiThread = QThread::currentThread() == qApp->thread();
            finished = true;
        }));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    QVERIFY(ran.load(std::memory_order_acquire));
    QVERIFY(onGuiThread);
    QVERIFY(!runner.isRunning());
}

void TestStravaRoutesDownloadPipeline::cancellationStopsWorker()
{
    std::atomic_bool entered{false};
    std::atomic_bool observedCancellation{false};
    bool finished = false;
    Runner runner;
    QVERIFY(runner.start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            entered.store(true, std::memory_order_release);
            while (!cancelled()) QThread::msleep(1);
            observedCancellation.store(true, std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            finished = true;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(
        entered.load(std::memory_order_acquire), 500);

    runner.cancel();

    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    QVERIFY(observedCancellation.load(std::memory_order_acquire));
}

void TestStravaRoutesDownloadPipeline::
runnerTeardownCancelsCooperativeWorkerPromptly()
{
    std::atomic_bool entered{false};
    std::atomic_bool exited{false};
    bool callbackCalled = false;
    auto runner = std::make_unique<Runner>();
    QVERIFY(runner->start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            entered.store(true, std::memory_order_release);
            while (!cancelled()) QThread::msleep(1);
            exited.store(true, std::memory_order_release);
        },
        [&](bool) { callbackCalled = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(
        entered.load(std::memory_order_acquire), 500);

    QElapsedTimer elapsed;
    elapsed.start();
    runner.reset();

    QVERIFY(elapsed.elapsed() < 500);
    QTRY_VERIFY_WITH_TIMEOUT(
        exited.load(std::memory_order_acquire), 500);
    QCoreApplication::processEvents();
    QVERIFY(!callbackCalled);
}

void TestStravaRoutesDownloadPipeline::
runnerTeardownDoesNotJoinNonCooperativeOperation()
{
    std::atomic_bool entered{false};
    std::atomic_bool exited{false};
    bool callbackCalled = false;
    const auto destruction = std::make_shared<DestructionState>();
    auto runner = std::make_unique<Runner>();
    QVERIFY(runner->start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &) {
            entered.store(true, std::memory_order_release);
            QThread::msleep(250);
            exited.store(true, std::memory_order_release);
        },
        [&](bool) { callbackCalled = true; },
        new DestructionProbe(destruction)));
    QTRY_VERIFY_WITH_TIMEOUT(
        entered.load(std::memory_order_acquire), 500);

    QElapsedTimer elapsed;
    elapsed.start();
    runner.reset();

    QVERIFY2(elapsed.elapsed() < 100,
             "Runner destruction joined a noncooperative worker");
    QTRY_VERIFY_WITH_TIMEOUT(
        exited.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        destruction->destroyed.load(std::memory_order_acquire), 1000);
    QVERIFY(destruction->destroyedOffGuiThread.load(
        std::memory_order_acquire));
    QCoreApplication::processEvents();
    QVERIFY(!callbackCalled);
}

void TestStravaRoutesDownloadPipeline::workerObjectTreeHasWorkerAffinity()
{
    auto *owner = new QObject;
    auto *child = new QObject(owner);
    std::atomic_bool ownerOnWorker{false};
    std::atomic_bool childOnWorker{false};
    bool finished = false;
    Runner runner;
    QVERIFY(runner.start(
        [owner, child, &ownerOnWorker, &childOnWorker](
            const StravaRoutesDownloadPipeline::CancellationCheck &) {
            ownerOnWorker.store(
                owner->thread() == QThread::currentThread(),
                std::memory_order_release);
            childOnWorker.store(
                child->thread() == QThread::currentThread(),
                std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            finished = true;
        },
        owner));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    QVERIFY(ownerOnWorker.load(std::memory_order_acquire));
    QVERIFY(childOnWorker.load(std::memory_order_acquire));
}

void TestStravaRoutesDownloadPipeline::
workerOwnedObjectIsDestroyedOnWorkerThread()
{
    const auto state = std::make_shared<DestructionState>();
    bool finished = false;
    {
        Runner runner;
        QVERIFY(runner.start(
            [](const StravaRoutesDownloadPipeline::CancellationCheck &) {},
            [&](bool failed) {
                QVERIFY(!failed);
                finished = true;
            },
            new DestructionProbe(state)));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    }

    QTRY_VERIFY_WITH_TIMEOUT(
        state->destroyed.load(std::memory_order_acquire), 500);
    QVERIFY(state->destroyedOffGuiThread.load(
        std::memory_order_acquire));
}

void TestStravaRoutesDownloadPipeline::
productionNetworkServiceTreeTearsDownOnWorkerThread()
{
    const auto state = std::make_shared<ServiceTreeState>();
    auto *service = new NetworkServiceTree(state);
    std::atomic_bool requestCreated{false};
    std::atomic_bool cancellationObserved{false};
    bool callbackCalled = false;
    auto runner = std::make_unique<Runner>();
    QVERIFY(runner->start(
        [service, state, &requestCreated, &cancellationObserved](
            const StravaRoutesDownloadPipeline::CancellationCheck
                &cancelled) {
            QNetworkReply *reply = service->manager->get(
                QNetworkRequest(QUrl(
                    QStringLiteral("data:text/plain,route"))));
            QObject::connect(
                reply, &QObject::destroyed,
                [state] {
                    state->teardownStayedOnWorker.store(
                        state->teardownStayedOnWorker.load(
                            std::memory_order_acquire)
                            && QThread::currentThread()
                                != qApp->thread(),
                        std::memory_order_release);
                    state->replyDestroyed.store(
                        true, std::memory_order_release);
                });
            requestCreated.store(true, std::memory_order_release);
            while (!cancelled()) QThread::msleep(1);
            cancellationObserved.store(
                true, std::memory_order_release);
        },
        [&](bool) { callbackCalled = true; },
        service));
    QTRY_VERIFY_WITH_TIMEOUT(
        requestCreated.load(std::memory_order_acquire), 1000);

    runner.reset();

    QTRY_VERIFY_WITH_TIMEOUT(
        cancellationObserved.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->rootDestroyed.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->managerDestroyed.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->replyDestroyed.load(std::memory_order_acquire), 1000);
    QVERIFY(state->teardownStayedOnWorker.load(
        std::memory_order_acquire));
    QCoreApplication::processEvents();
    QVERIFY(!callbackCalled);
}

void TestStravaRoutesDownloadPipeline::thrownOperationIsReported()
{
    bool finished = false;
    bool failed = false;
    Runner runner;
    QVERIFY(runner.start(
        [](const StravaRoutesDownloadPipeline::CancellationCheck &) {
            throw std::runtime_error("synthetic failure");
        },
        [&](bool operationFailed) {
            failed = operationFailed;
            finished = true;
        }));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    QVERIFY(failed);
}

void TestStravaRoutesDownloadPipeline::
downloadsAreStagedInBoundedBatches()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QStringList routeIds;
    const int requested =
        StravaRoutesDownloadPipeline::maximumFilesPerBatch() + 2;
    for (int index = 1; index <= requested; ++index)
        routeIds.append(QString::number(index));
    int calls = 0;

    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            routeIds,
            parent.path(),
            [&](const QString &routeId,
                const StravaRoutesDownloadPipeline::CancellationCheck &) {
                ++calls;
                return validPayload(routeId);
            });

    QVERIFY(result.isValid());
    QVERIFY(!result.cancelled);
    QCOMPARE(
        result.staged.size(),
        StravaRoutesDownloadPipeline::maximumFilesPerBatch());
    QCOMPARE(result.remainingRouteIds.size(), 2);
    QCOMPARE(calls, result.staged.size());
    for (const StagedRoute &route : result.staged) {
        QVERIFY(QFileInfo::exists(route.path));
        QVERIFY(route.bytes > 0);
        QVERIFY(route.pin);
        QCOMPARE(
            route.digest.size(),
            QCryptographicHash::hashLength(
                QCryptographicHash::Sha256));
        QFile stagedFile(route.path);
        QVERIFY(stagedFile.open(QIODevice::ReadOnly));
        QCOMPARE(
            QCryptographicHash::hash(
                stagedFile.readAll(), QCryptographicHash::Sha256),
            route.digest);
        QCOMPARE(QFileInfo(route.path).absolutePath(),
                 result.stagingDirectory);
    }
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
        result));
}

void TestStravaRoutesDownloadPipeline::downloadsRespectAggregateByteBudget()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const qint64 budget =
        StravaRoutesDownloadPipeline::maximumBytesPerBatch();
    QVERIFY(budget > 2);
    int calls = 0;

    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1"), QStringLiteral("2"),
             QStringLiteral("3")},
            parent.path(),
            [&](const QString &,
                const StravaRoutesDownloadPipeline::CancellationCheck &) {
                ++calls;
                StravaRoutesClient::PayloadResult payload;
                payload.payload = QByteArray(
                    qsizetype(budget / 2 + 1), 'x');
                payload.contentType = QStringLiteral("application/gpx+xml");
                return payload;
            });

    QVERIFY(result.isValid());
    QCOMPARE(result.staged.size(), 1);
    QCOMPARE(result.staged.first().bytes, budget / 2 + 1);
    QCOMPARE(
        result.remainingRouteIds,
        QStringList({QStringLiteral("2"), QStringLiteral("3")}));
    QCOMPARE(calls, 2);
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
        result));
}

void TestStravaRoutesDownloadPipeline::
oversizedPayloadIsRejectedWithoutRetry()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const qint64 budget =
        StravaRoutesDownloadPipeline::maximumBytesPerBatch();
    int calls = 0;
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1")}, parent.path(),
            [&](const QString &,
                const StravaRoutesDownloadPipeline::CancellationCheck &) {
                ++calls;
                StravaRoutesClient::PayloadResult payload;
                payload.payload = QByteArray(qsizetype(budget + 1), 'x');
                payload.contentType = QStringLiteral("application/gpx+xml");
                return payload;
            });

    QVERIFY(result.isValid());
    QCOMPARE(calls, 1);
    QVERIFY(result.staged.isEmpty());
    QCOMPARE(result.failures.size(), 1);
    QVERIFY(result.remainingRouteIds.isEmpty());
    QVERIFY(result.stagingDirectory.isEmpty());
}

void TestStravaRoutesDownloadPipeline::
stagingCleanupRejectsForeignDirectorySwap()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1")}, parent.path(),
            [](const QString &routeId,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                return validPayload(routeId);
            });
    QVERIFY(result.isValid());
    QCOMPARE(result.staged.size(), 1);

    const QString displaced = result.stagingDirectory
        + QStringLiteral("-owned");
    if (!QDir().rename(result.stagingDirectory, displaced)) {
        QSKIP("The platform did not permit the staging-directory swap");
    }
    QVERIFY(QDir().mkdir(result.stagingDirectory));
    const QString foreign = QDir(result.stagingDirectory).filePath(
        QStringLiteral("foreign.txt"));
    QVERIFY(QFile(foreign).open(QIODevice::WriteOnly));

    QVERIFY(!StravaRoutesDownloadPipeline::removeStagingDirectory(result));
    QVERIFY(QFileInfo::exists(foreign));
    QVERIFY(QFile::remove(foreign));
    QVERIFY(QDir().rmdir(result.stagingDirectory));
    QVERIFY(QDir().rename(displaced, result.stagingDirectory));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(result));
}

void TestStravaRoutesDownloadPipeline::
stagingCleanupRejectsSymlinkSwap()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates shortcuts rather than symlinks on Windows");
#else
    QTemporaryDir parent;
    QTemporaryDir foreignDirectory;
    QVERIFY(parent.isValid());
    QVERIFY(foreignDirectory.isValid());
    const QString sentinel = foreignDirectory.filePath(
        QStringLiteral("sentinel"));
    QVERIFY(QFile(sentinel).open(QIODevice::WriteOnly));
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1")}, parent.path(),
            [](const QString &routeId,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                return validPayload(routeId);
            });
    QVERIFY(result.isValid());
    const QString displaced = result.stagingDirectory
        + QStringLiteral("-owned");
    QVERIFY(QDir().rename(result.stagingDirectory, displaced));
    if (!QFile::link(
            foreignDirectory.path(), result.stagingDirectory)) {
        QVERIFY(QDir().rename(displaced, result.stagingDirectory));
        QSKIP("Symbolic links are unavailable in this test environment");
    }

    QVERIFY(!StravaRoutesDownloadPipeline::removeStagingDirectory(result));
    QVERIFY(QFileInfo::exists(sentinel));
    QVERIFY(QFile::remove(result.stagingDirectory));
    QVERIFY(QDir().rename(displaced, result.stagingDirectory));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(result));
#endif
}

void TestStravaRoutesDownloadPipeline::
pinnedRouteBytesSurviveStagingParentSwap()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1")}, parent.path(),
            [](const QString &routeId,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                return validPayload(routeId);
            });
    QVERIFY(result.isValid());
    QCOMPARE(result.staged.size(), 1);

    const QByteArray expected = validPayload(QStringLiteral("1")).payload;
    const QByteArray foreign = validPayload(QStringLiteral("99")).payload;
    const QString displaced = result.stagingDirectory
        + QStringLiteral("-owned");
    QByteArray parsed;
    QString error;
    bool renameDenied = false;
    QVERIFY2(StravaRoutesDownloadPipeline::withPinnedRouteBytes(
        result.staged.first(),
        [&](const QByteArray &contents, QString &) {
            parsed = contents;
            if (!QDir().rename(result.stagingDirectory, displaced)) {
                renameDenied = true;
                return true;
            }
            if (!QDir().mkdir(result.stagingDirectory)) {
                return false;
            }
            QFile replacement(result.staged.first().path);
            return replacement.open(QIODevice::WriteOnly)
                && replacement.write(foreign) == foreign.size()
                && replacement.flush();
        },
        error), qPrintable(error));
    QCOMPARE(parsed, expected);

#ifdef Q_OS_WIN
    if (renameDenied) {
        QVERIFY(QFileInfo::exists(result.staged.first().path));
        QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(result));
        QSKIP("The pinned staging anchor denies directory renames on Windows");
    }
#endif
    QVERIFY(!renameDenied);

    QVERIFY(QDir(result.stagingDirectory).removeRecursively());
    QVERIFY(QDir().rename(displaced, result.stagingDirectory));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(result));
}

void TestStravaRoutesDownloadPipeline::
stagedRouteRejectsSameSizedPrevalidationReplacement()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1")}, parent.path(),
            [](const QString &routeId,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                return validPayload(routeId);
            });
    QVERIFY(result.isValid());
    QCOMPARE(result.staged.size(), 1);

    const QByteArray original = validPayload(QStringLiteral("1")).payload;
    const QByteArray replacement = validPayload(QStringLiteral("9")).payload;
    QCOMPARE(replacement.size(), original.size());
    const QString displaced = result.stagingDirectory
        + QStringLiteral("-owned");
    if (!QDir().rename(result.stagingDirectory, displaced)) {
        QSKIP("The platform did not permit the staging-directory swap");
    }
    QVERIFY(QDir().mkdir(result.stagingDirectory));
    QFile replacementFile(result.staged.first().path);
    QVERIFY(replacementFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(replacementFile.write(replacement), replacement.size());
    QVERIFY(replacementFile.flush());
    replacementFile.close();

    bool parserCalled = false;
    QString error;
    QVERIFY(!StravaRoutesDownloadPipeline::withPinnedRouteBytes(
        result.staged.first(),
        [&](const QByteArray &, QString &) {
            parserCalled = true;
            return true;
        },
        error));
    QVERIFY(!parserCalled);
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(result.stagingDirectory).removeRecursively());
    QVERIFY(QDir().rename(displaced, result.stagingDirectory));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(result));
}

void TestStravaRoutesDownloadPipeline::
downloadCancellationKeepsCompletedPrefix()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    std::atomic_bool cancelled{false};
    int calls = 0;

    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1"), QStringLiteral("2"),
             QStringLiteral("3"), QStringLiteral("4")},
            parent.path(),
            [&](const QString &routeId,
                const StravaRoutesDownloadPipeline::CancellationCheck &) {
                ++calls;
                const auto payload = validPayload(routeId);
                if (calls == 2) {
                    cancelled.store(true, std::memory_order_release);
                }
                return payload;
            },
            [&] {
                return cancelled.load(std::memory_order_acquire);
            });

    QVERIFY(result.isValid());
    QVERIFY(result.cancelled);
    QCOMPARE(result.staged.size(), 1);
    QCOMPARE(
        result.remainingRouteIds,
        QStringList({QStringLiteral("2"), QStringLiteral("3"),
                     QStringLiteral("4")}));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
        result));
}

void TestStravaRoutesDownloadPipeline::
cancelledBatchCommitsCompletedPrefixOnce()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    std::atomic_bool cancelled{false};
    int calls = 0;
    const DownloadBatchResult downloaded =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1"), QStringLiteral("2"),
             QStringLiteral("3")},
            parent.path(),
            [&](const QString &routeId,
                const StravaRoutesDownloadPipeline::CancellationCheck &) {
                ++calls;
                const auto payload = validPayload(routeId);
                if (calls == 2)
                    cancelled.store(true, std::memory_order_release);
                return payload;
            },
            [&] { return cancelled.load(std::memory_order_acquire); });
    QVERIFY(downloaded.cancelled);

    int transactions = 0;
    QStringList imported;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) { return true; };
    callbacks.beginTransaction = [&] {
        ++transactions;
        return true;
    };
    callbacks.importRoute = [&](const StagedRoute &route, QString &) {
        imported.append(route.routeId);
        return true;
    };
    callbacks.commitTransaction = [] { return true; };
    callbacks.rollbackTransaction = [] {};
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult committed =
        StravaRoutesDownloadPipeline::importCompletedPrefix(
            downloaded, callbacks);
    QVERIFY(committed.isValid());
    QCOMPARE(imported, QStringList({QStringLiteral("1")}));
    QCOMPARE(committed.importedRouteIds, imported);
    QCOMPARE(transactions, 1);
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(downloaded));
}

void TestStravaRoutesDownloadPipeline::
preparationCancellationCommitsTakenPrefixOnce()
{
    const QList<StagedRoute> allRoutes = routes(4);
    StravaRoutesDownloadPipeline::PreparationCursor cursor(
        allRoutes, 7);
    QList<StagedRoute> prepared;
    StagedRoute route;
    QVERIFY(cursor.takeNext(7, route));
    prepared.append(route);
    QVERIFY(cursor.takeNext(7, route));
    prepared.append(route);

    QVERIFY(!cursor.takeNext(8, route));
    QVERIFY(cursor.stopped());
    QCOMPARE(
        cursor.remainingRouteIds(),
        QStringList({QStringLiteral("3"), QStringLiteral("4")}));
    QVERIFY(!cursor.takeNext(8, route));

    int transactions = 0;
    int commits = 0;
    QStringList imported;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) { return true; };
    callbacks.beginTransaction = [&] {
        ++transactions;
        return true;
    };
    callbacks.importRoute = [&](const StagedRoute &item, QString &) {
        imported.append(item.routeId);
        return true;
    };
    callbacks.commitTransaction = [&] {
        ++commits;
        return true;
    };
    callbacks.rollbackTransaction = [] {};
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult committed =
        StravaRoutesDownloadPipeline::importBatch(prepared, callbacks);
    QVERIFY(committed.isValid());
    QCOMPARE(
        committed.importedRouteIds,
        QStringList({QStringLiteral("1"), QStringLiteral("2")}));
    QCOMPARE(imported, committed.importedRouteIds);
    QCOMPARE(transactions, 1);
    QCOMPARE(commits, 1);
}

void TestStravaRoutesDownloadPipeline::
downloadErrorsDoNotDiscardValidRoutes()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const DownloadBatchResult result =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("1"), QStringLiteral("2"),
             QStringLiteral("3")},
            parent.path(),
            [](const QString &routeId,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                if (routeId == QStringLiteral("2")) {
                    StravaRoutesClient::PayloadResult failure;
                    failure.error = QStringLiteral("synthetic error");
                    return failure;
                }
                return validPayload(routeId);
            });

    QVERIFY(result.isValid());
    QCOMPARE(result.staged.size(), 2);
    QCOMPARE(result.failures.size(), 1);
    QCOMPARE(result.failures.first().routeId, QStringLiteral("2"));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
        result));
}

void TestStravaRoutesDownloadPipeline::
multiRouteImportUsesOneShortTransaction()
{
    QStringList events;
    bool inTransaction = false;
    int beginCount = 0;
    int commitCount = 0;
    int rollbackCount = 0;
    int networkCalls = 0;
    bool transactionInvariant = true;
    const QList<StagedRoute> staged = routes(5);
    for (const StagedRoute &route : staged) {
        QVERIFY(!inTransaction);
        events.append(QStringLiteral("network-%1").arg(route.routeId));
        ++networkCalls;
    }

    ImportCallbacks callbacks;
    callbacks.prepare = [&](const StagedRoute &route, QString &) {
        transactionInvariant = transactionInvariant && !inTransaction;
        events.append(QStringLiteral("prepare-%1").arg(route.routeId));
        return true;
    };
    callbacks.beginTransaction = [&] {
        transactionInvariant = transactionInvariant && !inTransaction;
        ++beginCount;
        inTransaction = true;
        events.append(QStringLiteral("begin"));
        return true;
    };
    callbacks.importRoute = [&](const StagedRoute &route, QString &) {
        transactionInvariant = transactionInvariant && inTransaction;
        events.append(QStringLiteral("import-%1").arg(route.routeId));
        return true;
    };
    callbacks.commitTransaction = [&] {
        transactionInvariant = transactionInvariant && inTransaction;
        ++commitCount;
        inTransaction = false;
        events.append(QStringLiteral("commit"));
        return true;
    };
    callbacks.rollbackTransaction = [&] {
        ++rollbackCount;
        inTransaction = false;
    };
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [&] {
        transactionInvariant = transactionInvariant && !inTransaction;
        events.append(QStringLiteral("finalize"));
    };

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(staged, callbacks);

    QVERIFY(result.isValid());
    QCOMPARE(result.importedRouteIds.size(), staged.size());
    QCOMPARE(networkCalls, staged.size());
    QCOMPARE(beginCount, 1);
    QCOMPARE(commitCount, 1);
    QCOMPARE(rollbackCount, 0);
    QVERIFY(transactionInvariant);
    QVERIFY(events.indexOf(QStringLiteral("begin"))
            > events.lastIndexOf(QStringLiteral("network-5")));
    QVERIFY(events.indexOf(QStringLiteral("begin"))
            > events.lastIndexOf(QStringLiteral("prepare-5")));
}

void TestStravaRoutesDownloadPipeline::
moreThanEightRoutesUseOneTransactionPerBoundedBatch()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    QStringList remaining;
    for (int index = 1; index <= 18; ++index)
        remaining.append(QString::number(index));

    bool inTransaction = false;
    bool transactionInvariant = true;
    int networkCalls = 0;
    int beginCount = 0;
    int commitCount = 0;
    while (!remaining.isEmpty()) {
        const DownloadBatchResult downloaded =
            StravaRoutesDownloadPipeline::stageDownloadBatch(
                remaining, parent.path(),
                [&](const QString &routeId,
                    const StravaRoutesDownloadPipeline::CancellationCheck &) {
                    transactionInvariant =
                        transactionInvariant && !inTransaction;
                    ++networkCalls;
                    return validPayload(routeId);
                });
        QVERIFY(downloaded.isValid());
        QVERIFY(!downloaded.staged.isEmpty());

        ImportCallbacks callbacks;
        callbacks.prepare = [&](const StagedRoute &, QString &) {
            transactionInvariant = transactionInvariant && !inTransaction;
            return true;
        };
        callbacks.beginTransaction = [&] {
            transactionInvariant = transactionInvariant && !inTransaction;
            inTransaction = true;
            ++beginCount;
            return true;
        };
        callbacks.importRoute = [&](const StagedRoute &, QString &) {
            transactionInvariant = transactionInvariant && inTransaction;
            return true;
        };
        callbacks.commitTransaction = [&] {
            transactionInvariant = transactionInvariant && inTransaction;
            inTransaction = false;
            ++commitCount;
            return true;
        };
        callbacks.rollbackTransaction = [&] {
            inTransaction = false;
        };
        callbacks.rollbackFiles = [] {};
        callbacks.finalizeFiles = [] {};

        const ImportBatchResult imported =
            StravaRoutesDownloadPipeline::importBatch(
                downloaded.staged, callbacks);
        QVERIFY(imported.isValid());
        QCOMPARE(imported.importedRouteIds.size(),
                 downloaded.staged.size());
        QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
            downloaded));
        remaining = downloaded.remainingRouteIds;
    }

    QCOMPARE(networkCalls, 18);
    QCOMPARE(beginCount, 3);
    QCOMPARE(commitCount, 3);
    QVERIFY(transactionInvariant);
    QVERIFY(!inTransaction);
}

void TestStravaRoutesDownloadPipeline::prepareFailuresAreSkipped()
{
    int imported = 0;
    int transactions = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &route, QString &error) {
        if (route.routeId == QStringLiteral("2")) {
            error = QStringLiteral("invalid route");
            return false;
        }
        return true;
    };
    callbacks.beginTransaction = [&] {
        ++transactions;
        return true;
    };
    callbacks.importRoute = [&](const StagedRoute &, QString &) {
        ++imported;
        return true;
    };
    callbacks.commitTransaction = [] { return true; };
    callbacks.rollbackTransaction = [] {};
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(3), callbacks);

    QVERIFY(result.isValid());
    QCOMPARE(result.failures.size(), 1);
    QCOMPARE(result.failures.first().routeId, QStringLiteral("2"));
    QCOMPARE(result.importedRouteIds.size(), 2);
    QCOMPARE(imported, 2);
    QCOMPARE(transactions, 1);
}

void TestStravaRoutesDownloadPipeline::
cancellationBeforeTransactionRollsBackFiles()
{
    int cancellationChecks = 0;
    int beginCount = 0;
    int rollbackFiles = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.beginTransaction = [&] {
        ++beginCount;
        return true;
    };
    callbacks.importRoute = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.commitTransaction = [] { return true; };
    callbacks.rollbackTransaction = [] {};
    callbacks.rollbackFiles = [&] { ++rollbackFiles; };
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(
            routes(3), callbacks,
            [&] { return ++cancellationChecks >= 2; });

    QVERIFY(result.isValid());
    QVERIFY(result.cancelled);
    QCOMPARE(beginCount, 0);
    QCOMPARE(rollbackFiles, 1);
    QVERIFY(result.importedRouteIds.isEmpty());
}

void TestStravaRoutesDownloadPipeline::beginFailureRollsBackFiles()
{
    int rollbackFiles = 0;
    int rollbackDatabase = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.beginTransaction = [] { return false; };
    callbacks.importRoute = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.commitTransaction = [] { return true; };
    callbacks.rollbackTransaction = [&] { ++rollbackDatabase; };
    callbacks.rollbackFiles = [&] { ++rollbackFiles; };
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(2), callbacks);

    QVERIFY(!result.isValid());
    QCOMPARE(rollbackDatabase, 0);
    QCOMPARE(rollbackFiles, 1);
    QVERIFY(result.importedRouteIds.isEmpty());
}

void TestStravaRoutesDownloadPipeline::
importFailureRollsBackTransactionAndFiles()
{
    int imports = 0;
    int commits = 0;
    int rollbackDatabase = 0;
    int rollbackFiles = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.beginTransaction = [] { return true; };
    callbacks.importRoute = [&](const StagedRoute &, QString &error) {
        if (++imports == 2) {
            error = QStringLiteral("synthetic import failure");
            return false;
        }
        return true;
    };
    callbacks.commitTransaction = [&] {
        ++commits;
        return true;
    };
    callbacks.rollbackTransaction = [&] { ++rollbackDatabase; };
    callbacks.rollbackFiles = [&] { ++rollbackFiles; };
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(3), callbacks);

    QVERIFY(!result.isValid());
    QCOMPARE(imports, 2);
    QCOMPARE(commits, 0);
    QCOMPARE(rollbackDatabase, 1);
    QCOMPARE(rollbackFiles, 1);
    QVERIFY(result.importedRouteIds.isEmpty());
}

void TestStravaRoutesDownloadPipeline::
commitFailureRollsBackTransactionAndFiles()
{
    int rollbackDatabase = 0;
    int rollbackFiles = 0;
    int finalized = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.beginTransaction = [] { return true; };
    callbacks.importRoute = [](const StagedRoute &, QString &) {
        return true;
    };
    callbacks.commitTransaction = [] { return false; };
    callbacks.rollbackTransaction = [&] { ++rollbackDatabase; };
    callbacks.rollbackFiles = [&] { ++rollbackFiles; };
    callbacks.finalizeFiles = [&] { ++finalized; };

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(3), callbacks);

    QVERIFY(!result.isValid());
    QCOMPARE(rollbackDatabase, 1);
    QCOMPARE(rollbackFiles, 1);
    QCOMPARE(finalized, 0);
    QVERIFY(result.importedRouteIds.isEmpty());
}

void TestStravaRoutesDownloadPipeline::
delayedPinnedReadCancellationKeepsGuiResponsive()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QByteArray payload(16 * 1024 * 1024, 'x');
    const DownloadBatchResult staged =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("42")}, parent.path(),
            [&payload](const QString &,
                       const StravaRoutesDownloadPipeline::CancellationCheck &) {
                StravaRoutesClient::PayloadResult result;
                result.payload = payload;
                return result;
            });
    QVERIFY(staged.isValid());
    QCOMPARE(staged.staged.size(), 1);

    routeChunkVisits.store(0, std::memory_order_release);
    routeChunkDelayMs.store(20, std::memory_order_release);
    std::atomic_bool cancelledRead{false};
    bool finished = false;
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();

    Runner runner;
    QVERIFY(runner.start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            QString error;
            const bool read =
                StravaRoutesDownloadPipeline::withPinnedRouteBytes(
                    staged.staged.first(),
                    [](const QByteArray &, QString &) { return true; },
                    error, cancelled);
            cancelledRead.store(!read, std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            finished = true;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(
        routeChunkVisits.load(std::memory_order_acquire) > 0, 500);

    QElapsedTimer elapsed;
    elapsed.start();
    runner.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(finished, 500);
    QVERIFY(elapsed.elapsed() < 400);
    QVERIFY(heartbeats > 0);
    QVERIFY(cancelledRead.load(std::memory_order_acquire));
    routeChunkDelayMs.store(0, std::memory_order_release);
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(staged));
}

void TestStravaRoutesDownloadPipeline::
delayedGpxParserCancellationKeepsGuiResponsive()
{
    gpxParserVisits.store(0, std::memory_order_release);
    gpxParserDelayMs.store(10, std::memory_order_release);
    const QByteArray contents = routeGpx(2000);
    std::atomic_bool parserCancelled{false};
    std::atomic_bool parserExited{false};
    bool callbackCalled = false;
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();

    auto runner = std::make_unique<Runner>();
    QVERIFY(runner->start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            QString error;
            std::unique_ptr<ErgFile> workout(ErgFile::fromGpxContentBytes(
                contents,
                QStringLiteral("/library/Strava-Route-42.gpx"),
                ErgFileFormat::crs,
                nullptr,
                error,
                cancelled));
            parserCancelled.store(!workout, std::memory_order_release);
            parserExited.store(true, std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            callbackCalled = true;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(
        gpxParserVisits.load(std::memory_order_acquire) > 0, 500);

    QElapsedTimer elapsed;
    elapsed.start();
    runner.reset();
    QVERIFY(elapsed.elapsed() < 100);
    QTRY_VERIFY_WITH_TIMEOUT(
        parserExited.load(std::memory_order_acquire), 300);
    QVERIFY(heartbeats > 0);
    QVERIFY(parserCancelled.load(std::memory_order_acquire));
    QVERIFY(!callbackCalled);
    gpxParserDelayMs.store(0, std::memory_order_release);
}

void TestStravaRoutesDownloadPipeline::
excessiveGpxHighWaterMarkIsRejected()
{
    GpxParserOptions options;
    options.garminSmartRecording = true;
    options.garminHighWaterMark = std::numeric_limits<int>::max();
    QString error;
    std::unique_ptr<ErgFile> workout(ErgFile::fromGpxContentBytes(
        routeGpx(12, 2),
        QStringLiteral("/library/high-water-limit.gpx"),
        ErgFileFormat::crs, nullptr, options, error));

    QVERIFY(!workout);
    QVERIFY(error.contains(QStringLiteral("limit"), Qt::CaseInsensitive));
}

void TestStravaRoutesDownloadPipeline::
generatedGpxPointBudgetIsEnforced()
{
    GpxParserOptions options;
    options.garminSmartRecording = true;
    options.garminHighWaterMark = 100;
    gpxPointLimitOverride.store(8, std::memory_order_release);
    QString error;
    std::unique_ptr<ErgFile> workout(ErgFile::fromGpxContentBytes(
        routeGpx(12, 2),
        QStringLiteral("/library/generated-point-limit.gpx"),
        ErgFileFormat::crs, nullptr, options, error));
    gpxPointLimitOverride.store(0, std::memory_order_release);

    QVERIFY(!workout);
    QVERIFY(error.contains(QStringLiteral("limit"), Qt::CaseInsensitive));
}

void TestStravaRoutesDownloadPipeline::
interpolationCancellationIsPrompt()
{
    GpxParserOptions options;
    options.garminSmartRecording = true;
    options.garminHighWaterMark = 100;
    gpxInterpolationVisits.store(0, std::memory_order_release);
    QString error;
    std::unique_ptr<ErgFile> workout(ErgFile::fromGpxContentBytes(
        routeGpx(12, 10),
        QStringLiteral("/library/interpolation-cancel.gpx"),
        ErgFileFormat::crs, nullptr, options, error,
        [] {
            return gpxInterpolationVisits.load(
                       std::memory_order_acquire) > 0;
        }));
    const int visits = gpxInterpolationVisits.load(
        std::memory_order_acquire);

    QVERIFY(!workout);
    QVERIFY(visits > 0);
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestStravaRoutesDownloadPipeline::
gpxValidationCancellationIsCooperative()
{
    const QByteArray payload = routeGpx(500);
    StravaRoutesClient client(
        [payload](
            const QUrl &, qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            StravaAuthenticatedSession::Result response;
            response.payload = payload;
            response.contentType = QStringLiteral("application/gpx+xml");
            return response;
        });
    std::atomic_int cancellationChecks{0};
    const auto result = client.downloadGpx(
        QStringLiteral("42"), [&] {
            return cancellationChecks.fetch_add(
                       1, std::memory_order_acq_rel) >= 1;
        });

    QVERIFY(!result.isValid());
    QVERIFY(cancellationChecks.load(std::memory_order_acquire) > 1);
    QVERIFY(result.error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestStravaRoutesDownloadPipeline::
capturedGpxOptionsOutliveSettingsOwner()
{
    GSettings *const originalSettings = appsettings;
    struct SettingsRestore
    {
        GSettings *settings = nullptr;
        ~SettingsRestore()
        {
            gpxSettingsReadBlocked.store(
                false, std::memory_order_release);
            appsettings = settings;
        }
    } restore{originalSettings};
    auto settingsOwner = std::make_unique<GSettings>(
        QStringLiteral("GoldenCheetah"),
        QStringLiteral("CapturedGpxOptionsTest"));
    appsettings = settingsOwner.get();
    gpxSettingsReadCalls.store(0, std::memory_order_release);
    gpxSettingsReadEntered.store(false, std::memory_order_release);
    gpxSettingsReadBlocked.store(true, std::memory_order_release);

    std::thread releaseSettingsRead([] {
        while (!gpxSettingsReadEntered.load(std::memory_order_acquire))
            QThread::msleep(1);
        QThread::msleep(20);
        gpxSettingsReadBlocked.store(false, std::memory_order_release);
    });
    const GpxParserOptions options = GpxParser::captureOptions();
    releaseSettingsRead.join();
    const int capturedReads = gpxSettingsReadCalls.load(
        std::memory_order_acquire);
    settingsOwner.reset();
    appsettings = nullptr;

    std::atomic_bool parsed{false};
    bool finished = false;
    Runner runner;
    const bool started = runner.start(
        [&](const StravaRoutesDownloadPipeline::CancellationCheck &cancelled) {
            QString error;
            std::unique_ptr<ErgFile> workout(
                ErgFile::fromGpxContentBytes(
                    routeGpx(),
                    QStringLiteral("/library/captured-options.gpx"),
                    ErgFileFormat::crs,
                    nullptr,
                    options,
                    error,
                    cancelled));
            parsed.store(bool(workout), std::memory_order_release);
        },
        [&](bool failed) {
            QVERIFY(!failed);
            finished = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 1000);
    const int finalReads = gpxSettingsReadCalls.load(
        std::memory_order_acquire);

    QVERIFY(started);
    QVERIFY(parsed.load(std::memory_order_acquire));
    QCOMPARE(capturedReads, 2);
    QCOMPARE(finalReads, capturedReads);
}

void TestStravaRoutesDownloadPipeline::
oversizedPredecessorAbortIsPrompt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(
        QStringLiteral("Strava-Route-42.gpx"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QVERIFY(file.resize(
        StravaRoutesDownloadPipeline::maximumBytesPerBatch() + 1));
    file.close();

    AnchoredFileSystem::DirectoryAnchor anchor;
    QString error;
    QVERIFY2(AnchoredFileSystem::DirectoryAnchor::open(
        directory.path(), anchor, error), qPrintable(error));
    const AnchoredFileSystem::EntryRef entry = anchor.entry(
        QFileInfo(path).fileName(), error);
    QVERIFY2(entry.isValid(), qPrintable(error));
    AnchoredFileSystem::PinnedFile pinned;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!StravaRoutesDownloadPipeline::pinRoutePredecessor(
        entry, pinned, error, [] { return false; }));
    QVERIFY(elapsed.elapsed() < 100);
    QVERIFY(error.contains(
        QStringLiteral("large"), Qt::CaseInsensitive));

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.resize(8 * 1024 * 1024));
    file.close();
    int cancellationChecks = 0;
    error.clear();
    elapsed.restart();
    QVERIFY(!StravaRoutesDownloadPipeline::pinRoutePredecessor(
        entry, pinned, error, [&] { return ++cancellationChecks >= 2; }));
    QVERIFY(elapsed.elapsed() < 250);
    QVERIFY(cancellationChecks >= 2);
    QVERIFY(error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestStravaRoutesDownloadPipeline::
byteBackedErgFileSurvivesParserTemporaryCleanup()
{
    const QByteArray contents = routeGpx();
    const QString target = QStringLiteral("/library/strava-42.gpx");
    {
        const std::lock_guard<std::mutex> lock(backingPathMutex);
        lastErgFileBackingPath.clear();
    }
    QString error;
    std::unique_ptr<ErgFile> workout(ErgFile::fromGpxContentBytes(
        contents, target, ErgFileFormat::crs, nullptr, error));

    QVERIFY2(workout, qPrintable(error));
    QVERIFY(workout->isValid());
    QCOMPARE(workout->filename(), target);
    QVERIFY(workout->Points.size() >= 3);
    QString backingPath;
    {
        const std::lock_guard<std::mutex> lock(backingPathMutex);
        backingPath = lastErgFileBackingPath;
    }
    QVERIFY(!backingPath.isEmpty());
    QVERIFY(!QFileInfo::exists(backingPath));

    ErgFileQueryAdapter query(workout.get());
    int lap = 0;
    const double distance = workout->Points.at(1).x;
    QVERIFY(query.altitudeAt(distance, lap) >= 100.0);
    geolocation location;
    double slope = 0.0;
    QVERIFY(query.locationAt(distance, lap, location, slope));

    QTemporaryDir databaseHome;
    QVERIFY(databaseHome.isValid());
    TrainDB database{QDir(databaseHome.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);
    TrainDB::ScopedLUW transaction(database);
    QVERIFY(transaction.isActive());
    QVERIFY(database.importWorkout(
        target, *workout, ImportMode::insertOrUpdate));
    QVERIFY(transaction.commit());
    QVERIFY(database.hasWorkout(target));
    std::unique_ptr<QAbstractTableModel> model(database.getWorkoutModel());
    bool found = false;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, TdbWorkoutModelIdx::filepath).data().toString()
            == target) {
            found = true;
            QCOMPARE(
                model->index(row, TdbWorkoutModelIdx::type).data().toString(),
                QStringLiteral("slp"));
            break;
        }
    }
    QVERIFY(found);
}

void TestStravaRoutesDownloadPipeline::
multiRouteTrainDbLuwCommitsAsOneUnit()
{
    QTemporaryDir databaseHome;
    QVERIFY(databaseHome.isValid());
    TrainDB database{QDir(databaseHome.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);
    QSignalSpy changed(&database, &TrainDB::dataChanged);

    std::unique_ptr<TrainDB::ScopedLUW> transaction;
    int beginCount = 0;
    int commitCount = 0;
    int rollbackCount = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) { return true; };
    callbacks.beginTransaction = [&] {
        ++beginCount;
        transaction = std::make_unique<TrainDB::ScopedLUW>(database);
        return transaction->isActive();
    };
    callbacks.importRoute = [&](const StagedRoute &route, QString &) {
        ErgFileBase workout;
        workout.format(ErgFileFormat::crs);
        workout.name(QStringLiteral("Route %1").arg(route.routeId));
        return database.importWorkout(
            QStringLiteral("/strava-%1.crs").arg(route.routeId),
            workout);
    };
    callbacks.commitTransaction = [&] {
        ++commitCount;
        const bool committed = transaction && transaction->commit();
        transaction.reset();
        return committed;
    };
    callbacks.rollbackTransaction = [&] {
        ++rollbackCount;
        if (transaction) transaction->rollback();
        transaction.reset();
    };
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(3), callbacks);

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.importedRouteIds.size(), 3);
    QCOMPARE(beginCount, 1);
    QCOMPARE(commitCount, 1);
    QCOMPARE(rollbackCount, 0);
    QCOMPARE(changed.count(), 1);
    for (int index = 1; index <= 3; ++index) {
        QVERIFY(database.hasWorkout(
            QStringLiteral("/strava-%1.crs").arg(index)));
    }
}

void TestStravaRoutesDownloadPipeline::
multiRouteTrainDbLuwRollsBackAsOneUnit()
{
    QTemporaryDir databaseHome;
    QVERIFY(databaseHome.isValid());
    TrainDB database{QDir(databaseHome.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);
    QSignalSpy changed(&database, &TrainDB::dataChanged);

    std::unique_ptr<TrainDB::ScopedLUW> transaction;
    int beginCount = 0;
    int commitCount = 0;
    int rollbackCount = 0;
    ImportCallbacks callbacks;
    callbacks.prepare = [](const StagedRoute &, QString &) { return true; };
    callbacks.beginTransaction = [&] {
        ++beginCount;
        transaction = std::make_unique<TrainDB::ScopedLUW>(database);
        return transaction->isActive();
    };
    callbacks.importRoute = [&](const StagedRoute &route, QString &error) {
        ErgFileBase workout;
        workout.format(ErgFileFormat::crs);
        workout.name(QStringLiteral("Route %1").arg(route.routeId));
        const QString path = route.routeId == QStringLiteral("2")
            ? QStringLiteral("/strava-1.crs")
            : QStringLiteral("/strava-%1.crs").arg(route.routeId);
        if (database.importWorkout(path, workout)) return true;
        error = QStringLiteral("injected duplicate route");
        return false;
    };
    callbacks.commitTransaction = [&] {
        ++commitCount;
        const bool committed = transaction && transaction->commit();
        transaction.reset();
        return committed;
    };
    callbacks.rollbackTransaction = [&] {
        ++rollbackCount;
        if (transaction) transaction->rollback();
        transaction.reset();
    };
    callbacks.rollbackFiles = [] {};
    callbacks.finalizeFiles = [] {};

    const ImportBatchResult result =
        StravaRoutesDownloadPipeline::importBatch(routes(3), callbacks);

    QVERIFY(!result.isValid());
    QCOMPARE(beginCount, 1);
    QCOMPARE(commitCount, 0);
    QCOMPARE(rollbackCount, 1);
    QCOMPARE(changed.count(), 0);
    QVERIFY(!database.hasWorkout(QStringLiteral("/strava-1.crs")));
    QVERIFY(!database.hasWorkout(QStringLiteral("/strava-3.crs")));
}

void TestStravaRoutesDownloadPipeline::
productionMultiRouteCommitUsesOneDatabaseLuw()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);

    const QStringList routeIds{
        QStringLiteral("41"), QStringLiteral("42"), QStringLiteral("43")};
    const DownloadBatchResult downloadResult =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            routeIds, stagingRoot,
            [](const QString &,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                StravaRoutesClient::PayloadResult result;
                result.payload = routeGpx();
                return result;
            });
    QVERIFY2(downloadResult.isValid(), qPrintable(downloadResult.error));
    QCOMPARE(downloadResult.staged.size(), routeIds.size());

    QString error;
    auto operation = StravaRoutesDownloadProductionTest::prepareImport(
        downloadResult, athleteRoot, workoutRoot, false,
        GpxParser::captureOptions(), {}, error);
    QVERIFY2(operation, qPrintable(error));
    bool decisionCommitted = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::commitDecision(
        operation, database.databaseFilePath(), {},
        decisionCommitted, error), qPrintable(error));
    QVERIFY(decisionCommitted);
    QVERIFY2(StravaRoutesDownloadProductionTest::publish(
        operation, {}, error), qPrintable(error));

    QSignalSpy changed(&database, &TrainDB::dataChanged);
    bool stagingCleaned = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::finalizeWithDatabase(
        operation, &database, stagingCleaned, error), qPrintable(error));
    QVERIFY(stagingCleaned);
    QCOMPARE(changed.count(), 1);
    for (const QString &routeId : routeIds) {
        QVERIFY(database.hasWorkout(
            QDir(QFileInfo(workoutRoot).canonicalFilePath()).filePath(
            StravaRoutesClient::workoutFileName(routeId))));
    }
}

void TestStravaRoutesDownloadPipeline::
databaseReplacementBeforePublicationRetainsDecisionMarker()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString replacementRoot = root.filePath(
        QStringLiteral("replacement-database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(replacementRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);
    QString replacementSource;
    {
        TrainDB replacement{QDir(replacementRoot)};
        QCOMPARE(replacement.schemaStatus(), TrainDB::SchemaStatus::current);
        replacementSource = replacement.databaseFilePath();
    }

    const DownloadBatchResult downloadResult =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("42")}, stagingRoot,
            [](const QString &,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                StravaRoutesClient::PayloadResult result;
                result.payload = routeGpx();
                return result;
            });
    QVERIFY2(downloadResult.isValid(), qPrintable(downloadResult.error));
    QString error;
    auto operation = StravaRoutesDownloadProductionTest::prepareImport(
        downloadResult, athleteRoot, workoutRoot, false,
        GpxParser::captureOptions(), {}, error);
    QVERIFY2(operation, qPrintable(error));
    bool committed = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::commitDecision(
        operation, database.databaseFilePath(), {}, committed, error),
        qPrintable(error));
    QVERIFY(committed);

    const QString databasePath = database.databaseFilePath();
    const QString displacedPath = databasePath
        + QStringLiteral(".decision-generation");
    bool swapped = false;
    bool replacementDenied = false;
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        publicationMutationAction = [&] {
            if (swapped || replacementDenied) return;
            if (!QFile::rename(databasePath, displacedPath)) {
                replacementDenied = true;
                return;
            }
            if (!QFile::copy(replacementSource, databasePath)) {
                QFile::rename(displacedPath, databasePath);
                replacementDenied = true;
                return;
            }
            swapped = true;
        };
    }
    const bool published = StravaRoutesDownloadProductionTest::publish(
        operation, {}, error);
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        publicationMutationAction = {};
    }
#ifdef Q_OS_WIN
    if (replacementDenied)
        QSKIP("The open SQLite generation denies replacement on Windows");
#endif
    QVERIFY(swapped);
    QVERIFY(!published);
    const QString target =
        QDir(QFileInfo(workoutRoot).canonicalFilePath()).filePath(
            StravaRoutesClient::workoutFileName(QStringLiteral("42")));
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(decisionMarkers(workoutRoot).size(), 1);
    QCOMPARE(readFile(databasePath), readFile(replacementSource));

    bool recoveryEntered = false;
    {
        TrainDB replacementDatabase(
            QDir(databaseRoot),
            uniqueValue(QStringLiteral("replacement-recovery")), false);
        QCOMPARE(
            replacementDatabase.schemaStatus(),
            TrainDB::SchemaStatus::current);
        const PlanBundleImport::BoundDatabaseCompletion completion =
            [&recoveryEntered](
                const TrainDB::PlanImportJournal &,
                const PlanBundleImport::PublishedValidation &,
                QString &) {
                recoveryEntered = true;
                return false;
            };
        error.clear();
        QVERIFY(!PlanBundleImport::Journal::reconcileAll(
            &replacementDatabase, athleteRoot, workoutRoot,
            completion, error));
    }
    QVERIFY(!recoveryEntered);
    QCOMPARE(decisionMarkers(workoutRoot).size(), 1);
    QCOMPARE(readFile(databasePath), readFile(replacementSource));
}

void TestStravaRoutesDownloadPipeline::
productionCompositionAbortCloseRecovers()
{
    StravaRoutesDownloadPipeline::ImportLifecycle lifecycle;
    lifecycle.begin();
    const auto generation = lifecycle.generation();
    lifecycle.requestAbort(true);
    QVERIFY(lifecycle.aborted());
    QVERIFY(lifecycle.closeWhenIdle());
    QVERIFY(!lifecycle.accepts(generation));

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString databaseRoot = root.filePath(QStringLiteral("database"));
    const QString athleteRoot = root.filePath(QStringLiteral("athlete"));
    const QString workoutRoot = root.filePath(QStringLiteral("workouts"));
    const QString stagingRoot = root.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(databaseRoot));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(stagingRoot));
    TrainDB database{QDir(databaseRoot)};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);

    const DownloadBatchResult downloadResult =
        StravaRoutesDownloadPipeline::stageDownloadBatch(
            {QStringLiteral("42")}, stagingRoot,
            [](const QString &,
               const StravaRoutesDownloadPipeline::CancellationCheck &) {
                StravaRoutesClient::PayloadResult result;
                result.payload = routeGpx();
                return result;
            });
    QVERIFY2(downloadResult.isValid(), qPrintable(downloadResult.error));
    QCOMPARE(downloadResult.staged.size(), 1);
    const GpxParserOptions options = GpxParser::captureOptions();

    QString error;
    auto operation = StravaRoutesDownloadProductionTest::prepareImport(
        downloadResult, athleteRoot, workoutRoot, false,
        options, {}, error);
    QVERIFY2(operation, qPrintable(error));
    bool committed = false;
    QVERIFY2(StravaRoutesDownloadProductionTest::commitDecision(
        operation, database.databaseFilePath(), {}, committed, error),
        qPrintable(error));
    QVERIFY(committed);

    QVERIFY(!StravaRoutesDownloadProductionTest::publish(
        operation, [] { return true; }, error));
    QVERIFY(error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    operation.reset();

    const QString target =
        QDir(QFileInfo(workoutRoot).canonicalFilePath()).filePath(
            StravaRoutesClient::workoutFileName(QStringLiteral("42")));
    const PlanBundleImport::BoundDatabaseCompletion recoverDatabase =
        [&database, &options](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validatePublished,
            QString &completionError) {
            TrainDB::ScopedLUW transaction(database);
            if (!transaction.isActive()) return false;
            for (const TrainDB::PlanImportWorkout &record : journal.workouts) {
                const QString path = QDir(journal.workoutRoot).filePath(
                    record.targetFileName);
                std::unique_ptr<ErgFile> workout(
                    ErgFile::fromGpxContentBytes(
                        record.contents, path, ErgFileFormat::crs,
                        nullptr, options, completionError));
                if (!workout
                    || !database.importWorkout(
                        path, *workout, ImportMode::insertOrUpdate)) {
                    return false;
                }
            }
            if (!validatePublished(completionError)
                || !database.removePlanImportJournal(
                    journal.id, completionError)) {
                return false;
            }
            return transaction.commit();
        };
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
        &database, athleteRoot, workoutRoot,
        recoverDatabase, error), qPrintable(error));
    QVERIFY(QFileInfo::exists(target));
    QVERIFY(database.hasWorkout(target));
    QVERIFY(StravaRoutesDownloadPipeline::removeStagingDirectory(
        downloadResult));
}

QTEST_MAIN(TestStravaRoutesDownloadPipeline)
#include "testStravaRoutesDownloadPipeline.moc"
