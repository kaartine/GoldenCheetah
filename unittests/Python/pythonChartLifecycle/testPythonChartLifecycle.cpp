/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Python/PythonChartOwner.h"
#include "Python/PythonChartRunner.h"
#include "Python/PythonExecutionGate.h"
#include "Python/PythonRuntimeFinalizer.h"
#include "Core/ProcessLifetimeRuntimeOwner.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace {

class FakeEmbeddedRuntime
{
public:
    enum class InitializationState { NotStarted, InterpreterInitialized, Ready };

    FakeEmbeddedRuntime(InitializationState state, int *destructionCount)
        : state_(state), destructionCount_(destructionCount) {}
    ~FakeEmbeddedRuntime() { ++*destructionCount_; }

    InitializationState initializationState() const { return state_; }

private:
    InitializationState state_;
    int *destructionCount_;
};

template<typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

PythonChartRunInput input(
    quint64 token,
    const QString &source,
    const QString &filter = QString())
{
    ScriptContext context;
    context.runToken = token;
    context.contextFiltered = !filter.isEmpty();
    context.contextFilters = {filter};

    PythonChartRunInput runInput;
    runInput.context = std::make_shared<const ScriptContext>(context);
    runInput.source = source;
    runInput.token = token;
    return runInput;
}

}

class TestPythonChartLifecycle : public QObject
{
    Q_OBJECT

private slots:
    void runnerSnapshotsValuesAndCoalescesLatestRerun();
    void runnerDestructionCancelsAndJoinsActiveRun();
    void runnerDropsPendingRerun();
    void ownerSnapshotsAndCoalescesLatestRerun();
    void ownerClearDropsPendingAndSuppressesStaleResult();
    void ownerApplyMayDestroyOwner();
    void ownerReentrantApplyKeepsBusyUntilLatestRunFinishes();
    void ownerDestructionJoinsAndSuppressesUiCallbacks();
    void executionGateCancelsWaitingCaller();
    void executionGateSerializesWaitingCallers();
    void executionGateRejectsInterpreterLockHolderAndAllocatesTokens();
    void executionGateShutdownDrainsAndCloses();
    void executionGateShutdownTimeoutAndWrongThreadFailClosed();
    void runtimeFinalizerEnforcesOwnerStateAndOrdering();
    void runtimeFinalizerHandlesPartialAndFlushErrors();
    void runtimeInitializerTracksFailureOwnership();
    void pathAppenderOwnsReferencesOnEveryExit();
    void processLifetimeOwnerControlsPublication();
    void pythonConfigurationOwnsInitializationInputs();
    void pythonInitializationOwnershipWiring();
};

void
TestPythonChartLifecycle::runnerSnapshotsValuesAndCoalescesLatestRerun()
{
    QSemaphore firstStarted;
    QSemaphore firstCancelled;
    QList<quint64> cancelledTokens;
    QList<quint64> executionTokens;
    QStringList executionSources;
    QStringList executionFilters;
    std::mutex observationsMutex;
    std::atomic_int activeExecutions{0};
    std::atomic_int maximumExecutions{0};
    std::atomic_int completionCount{0};
    PythonRunResult completedResult;

    PythonChartRunner runner(
        [&](PythonChartRunInput runInput,
            std::shared_ptr<std::atomic_bool> cancellation) {
            const int active =
                    activeExecutions.fetch_add(
                        1, std::memory_order_acq_rel) + 1;
            int previousMaximum =
                    maximumExecutions.load(std::memory_order_acquire);
            while (active > previousMaximum
                   && !maximumExecutions.compare_exchange_weak(
                       previousMaximum, active,
                       std::memory_order_acq_rel)) {
            }

            {
                std::lock_guard<std::mutex> lock(observationsMutex);
                executionTokens.append(runInput.token);
                executionSources.append(runInput.source);
                executionFilters.append(
                    runInput.context
                        ? runInput.context->contextFilters.value(0)
                        : QString());
            }

            if (runInput.token == 11) {
                firstStarted.release();
                while (!cancellation->load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                firstCancelled.release();
            }

            PythonRunResult result;
            result.messages = {
                QStringLiteral("finished-%1").arg(runInput.token)};
            result.cancelled =
                    cancellation->load(std::memory_order_acquire);
            activeExecutions.fetch_sub(1, std::memory_order_acq_rel);
            return result;
        },
        [&](quint64 token) {
            cancelledTokens.append(token);
        },
        [&](PythonRunResult result) {
            completedResult = std::move(result);
            completionCount.fetch_add(1, std::memory_order_release);
        });

    PythonChartRunInput first =
            input(11, QStringLiteral("original-source"),
                  QStringLiteral("original-filter"));
    QCOMPARE(
        runner.request(first),
        PythonChartRunState::RequestDisposition::Started);
    first.source = QStringLiteral("edited-after-request");
    QVERIFY(firstStarted.tryAcquire(1, 2000));

    QCOMPARE(
        runner.request(
            input(12, QStringLiteral("superseded-source"),
                  QStringLiteral("superseded-filter"))),
        PythonChartRunState::RequestDisposition::Queued);
    QCOMPARE(
        runner.request(
            input(13, QStringLiteral("latest-source"),
                  QStringLiteral("latest-filter"))),
        PythonChartRunState::RequestDisposition::Queued);

    QVERIFY(firstCancelled.tryAcquire(1, 2000));
    QVERIFY(waitUntil([&]() {
        return completionCount.load(std::memory_order_acquire) == 1;
    }));

    {
        std::lock_guard<std::mutex> lock(observationsMutex);
        QCOMPARE(executionTokens, QList<quint64>({11, 13}));
        QCOMPARE(
            executionSources,
            QStringList({
                QStringLiteral("original-source"),
                QStringLiteral("latest-source")}));
        QCOMPARE(
            executionFilters,
            QStringList({
                QStringLiteral("original-filter"),
                QStringLiteral("latest-filter")}));
    }
    QCOMPARE(cancelledTokens, QList<quint64>({11}));
    QCOMPARE(maximumExecutions.load(), 1);
    QCOMPARE(completionCount.load(), 1);
    QCOMPARE(
        completedResult.messages,
        QStringList({QStringLiteral("finished-13")}));
    QVERIFY(!completedResult.cancelled);
    QVERIFY(!runner.active());
}

void
TestPythonChartLifecycle::ownerSnapshotsAndCoalescesLatestRerun()
{
    QSemaphore firstStarted;
    QList<quint64> cancelledTokens;
    QList<quint64> executionTokens;
    QStringList executionSources;
    QStringList executionFilters;
    QList<bool> busyTransitions;
    std::mutex observationsMutex;
    std::atomic_int activeExecutions{0};
    std::atomic_int maximumExecutions{0};
    std::atomic_int applyCount{0};
    std::atomic_bool callbacksOnOwnerThread{true};
    PythonRunResult appliedResult;
    PythonChartOwner::PreparedRun prepared;
    const Qt::HANDLE ownerThread = QThread::currentThreadId();

    const auto markCallbackThread = [&]() {
        if (QThread::currentThreadId() != ownerThread) {
            callbacksOnOwnerThread.store(false, std::memory_order_release);
        }
    };

    PythonChartOwner owner(
        {
            [&]() {
                markCallbackThread();
                return prepared;
            },
            [&](bool busy) {
                markCallbackThread();
                busyTransitions.append(busy);
            },
            [&](PythonRunResult result) {
                markCallbackThread();
                appliedResult = std::move(result);
                applyCount.fetch_add(1, std::memory_order_release);
            }
        },
        [&](PythonChartRunInput runInput,
            std::shared_ptr<std::atomic_bool> cancellation) {
            const int active =
                    activeExecutions.fetch_add(
                        1, std::memory_order_acq_rel) + 1;
            int previousMaximum =
                    maximumExecutions.load(std::memory_order_acquire);
            while (active > previousMaximum
                   && !maximumExecutions.compare_exchange_weak(
                       previousMaximum, active,
                       std::memory_order_acq_rel)) {
            }

            {
                std::lock_guard<std::mutex> lock(observationsMutex);
                executionTokens.append(runInput.token);
                executionSources.append(runInput.source);
                executionFilters.append(
                    runInput.context
                        ? runInput.context->contextFilters.value(0)
                        : QString());
            }

            if (runInput.token == 11) {
                firstStarted.release();
                while (!cancellation->load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
            }

            PythonRunResult result;
            result.messages = {
                QStringLiteral("owner-finished-%1").arg(runInput.token)};
            result.chartCommands.append([](PythonChart *) {});
            result.cancelled =
                    cancellation->load(std::memory_order_acquire);
            activeExecutions.fetch_sub(1, std::memory_order_acq_rel);
            return result;
        },
        [&](quint64 token) {
            cancelledTokens.append(token);
        });

    prepared.action = PythonChartOwner::Action::Run;
    prepared.input =
            input(11, QStringLiteral("owner-original-source"),
                  QStringLiteral("owner-original-filter"));
    owner.trigger();
    prepared.input.source = QStringLiteral("edited-after-trigger");
    QVERIFY(firstStarted.tryAcquire(1, 2000));

    prepared.input =
            input(12, QStringLiteral("owner-superseded-source"),
                  QStringLiteral("owner-superseded-filter"));
    owner.trigger();
    prepared.input =
            input(13, QStringLiteral("owner-latest-source"),
                  QStringLiteral("owner-latest-filter"));
    owner.trigger();

    QVERIFY(waitUntil([&]() {
        return applyCount.load(std::memory_order_acquire) == 1
                && busyTransitions.size() == 2
                && !owner.active();
    }));

    {
        std::lock_guard<std::mutex> lock(observationsMutex);
        QCOMPARE(executionTokens, QList<quint64>({11, 13}));
        QCOMPARE(
            executionSources,
            QStringList({
                QStringLiteral("owner-original-source"),
                QStringLiteral("owner-latest-source")}));
        QCOMPARE(
            executionFilters,
            QStringList({
                QStringLiteral("owner-original-filter"),
                QStringLiteral("owner-latest-filter")}));
    }
    QCOMPARE(cancelledTokens, QList<quint64>({11}));
    QCOMPARE(busyTransitions, QList<bool>({true, false}));
    QCOMPARE(maximumExecutions.load(std::memory_order_acquire), 1);
    QCOMPARE(applyCount.load(std::memory_order_acquire), 1);
    QCOMPARE(
        appliedResult.messages,
        QStringList({QStringLiteral("owner-finished-13")}));
    QCOMPARE(appliedResult.chartCommands.size(), 1);
    QVERIFY(!appliedResult.cancelled);
    QVERIFY(callbacksOnOwnerThread.load(std::memory_order_acquire));
}

void
TestPythonChartLifecycle::
ownerClearDropsPendingAndSuppressesStaleResult()
{
    QSemaphore firstStarted;
    QSemaphore allowFirstExit;
    QList<quint64> cancelledTokens;
    QList<quint64> executionTokens;
    QList<bool> busyTransitions;
    std::mutex executionMutex;
    std::atomic_int applyCount{0};
    PythonChartOwner::PreparedRun prepared;

    PythonChartOwner owner(
        {
            [&]() { return prepared; },
            [&](bool busy) { busyTransitions.append(busy); },
            [&](PythonRunResult) {
                applyCount.fetch_add(1, std::memory_order_release);
            }
        },
        [&](PythonChartRunInput runInput,
            std::shared_ptr<std::atomic_bool> cancellation) {
            {
                std::lock_guard<std::mutex> lock(executionMutex);
                executionTokens.append(runInput.token);
            }
            firstStarted.release();
            while (!cancellation->load(std::memory_order_acquire)) {
                QThread::msleep(1);
            }
            allowFirstExit.acquire();

            PythonRunResult result;
            result.messages = {QStringLiteral("stale-owner-result")};
            result.chartCommands.append([](PythonChart *) {});
            result.cancelled = false;
            return result;
        },
        [&](quint64 token) {
            cancelledTokens.append(token);
        });

    prepared.action = PythonChartOwner::Action::Run;
    prepared.input = input(31, QStringLiteral("owner-running"));
    owner.trigger();
    const bool started = firstStarted.tryAcquire(1, 2000);

    prepared.input = input(32, QStringLiteral("owner-must-not-run"));
    owner.trigger();
    prepared.action = PythonChartOwner::Action::Clear;
    owner.trigger();
    allowFirstExit.release();

    QVERIFY(waitUntil([&]() {
        return busyTransitions.size() == 2 && !owner.active();
    }));
    QVERIFY(started);
    {
        std::lock_guard<std::mutex> lock(executionMutex);
        QCOMPARE(executionTokens, QList<quint64>({31}));
    }
    QCOMPARE(cancelledTokens, QList<quint64>({31, 31}));
    QCOMPARE(busyTransitions, QList<bool>({true, false}));
    QCOMPARE(applyCount.load(std::memory_order_acquire), 0);
}

void
TestPythonChartLifecycle::ownerApplyMayDestroyOwner()
{
    QSemaphore workerStarted;
    QSemaphore allowWorkerExit;
    QList<bool> busyTransitions;
    std::atomic_int applyCount{0};
    bool applyReturned = false;
    PythonChartOwner::PreparedRun prepared;
    prepared.action = PythonChartOwner::Action::Run;
    prepared.input = input(41, QStringLiteral("delete-owner"));

    PythonChartOwner *owner = nullptr;
    owner = new PythonChartOwner(
        {
            [&]() { return prepared; },
            [&](bool busy) { busyTransitions.append(busy); },
            [&](PythonRunResult) {
                applyCount.fetch_add(1, std::memory_order_release);
                PythonChartOwner *doomed = owner;
                owner = nullptr;
                delete doomed;
                applyReturned = true;
            }
        },
        [&](PythonChartRunInput,
            std::shared_ptr<std::atomic_bool>) {
            workerStarted.release();
            allowWorkerExit.acquire();
            return PythonRunResult();
        },
        [](quint64) {});

    owner->trigger();
    const bool started = workerStarted.tryAcquire(1, 2000);
    allowWorkerExit.release();
    const bool destroyed = waitUntil([&]() {
        return owner == nullptr && applyReturned;
    });
    if (owner) {
        delete owner;
        owner = nullptr;
    }

    QVERIFY(started);
    QVERIFY(destroyed);
    QCOMPARE(applyCount.load(std::memory_order_acquire), 1);
    QCOMPARE(busyTransitions, QList<bool>({true}));
}

void
TestPythonChartLifecycle::
ownerReentrantApplyKeepsBusyUntilLatestRunFinishes()
{
    QSemaphore secondStarted;
    QSemaphore allowSecondExit;
    QList<bool> busyTransitions;
    QList<quint64> appliedTokens;
    std::atomic_int applyCount{0};
    PythonChartOwner::PreparedRun prepared;
    PythonChartOwner *owner = nullptr;

    owner = new PythonChartOwner(
        {
            [&]() { return prepared; },
            [&](bool busy) { busyTransitions.append(busy); },
            [&](PythonRunResult result) {
                const quint64 token =
                        static_cast<quint64>(result.value);
                appliedTokens.append(token);
                applyCount.fetch_add(1, std::memory_order_release);
                if (token == 51) {
                    prepared.input =
                            input(52, QStringLiteral("reentrant-latest"));
                    owner->trigger();
                }
            }
        },
        [&](PythonChartRunInput runInput,
            std::shared_ptr<std::atomic_bool>) {
            if (runInput.token == 52) {
                secondStarted.release();
                allowSecondExit.acquire();
            }
            PythonRunResult result;
            result.value = static_cast<double>(runInput.token);
            return result;
        },
        [](quint64) {});

    prepared.action = PythonChartOwner::Action::Run;
    prepared.input = input(51, QStringLiteral("reentrant-first"));
    owner->trigger();

    const bool latestStarted = waitUntil([&]() {
        return secondStarted.available() > 0;
    });
    if (latestStarted) secondStarted.acquire();
    const bool busyStayedOn =
            busyTransitions == QList<bool>({true}) && owner->active();

    allowSecondExit.release();
    const bool finished = waitUntil([&]() {
        return applyCount.load(std::memory_order_acquire) == 2
                && !owner->active();
    });

    QVERIFY(latestStarted);
    QVERIFY(busyStayedOn);
    QVERIFY(finished);
    QCOMPARE(appliedTokens, QList<quint64>({51, 52}));
    QCOMPARE(busyTransitions, QList<bool>({true, false}));

    delete owner;
}

void
TestPythonChartLifecycle::
ownerDestructionJoinsAndSuppressesUiCallbacks()
{
    QSemaphore ownerReady;
    QSemaphore beginDelete;
    QSemaphore workerStarted;
    QSemaphore cancelCalled;
    QSemaphore cancellationObserved;
    QSemaphore shutdownWaitStarted;
    QSemaphore allowWorkerExit;
    QList<quint64> cancelledTokens;
    std::atomic_int busyTrueCount{0};
    std::atomic_int busyFalseCount{0};
    std::atomic_int applyCount{0};
    std::atomic_int order{0};
    std::atomic_int workerExitOrder{0};
    std::atomic_int destructorReturnOrder{0};
    std::atomic_bool destructorReturned{false};

    std::thread ownerThread([&]() {
        PythonChartOwner::PreparedRun prepared;
        prepared.action = PythonChartOwner::Action::Run;
        prepared.input =
                input(41, QStringLiteral("owner-blocking-source"));

        auto *owner = new PythonChartOwner(
            {
                [&]() { return prepared; },
                [&](bool busy) {
                    if (busy) {
                        busyTrueCount.fetch_add(
                            1, std::memory_order_release);
                    } else {
                        busyFalseCount.fetch_add(
                            1, std::memory_order_release);
                    }
                },
                [&](PythonRunResult) {
                    applyCount.fetch_add(1, std::memory_order_release);
                }
            },
            [&](PythonChartRunInput,
                std::shared_ptr<std::atomic_bool> cancellation) {
                workerStarted.release();
                while (!cancellation->load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                cancellationObserved.release();
                allowWorkerExit.acquire();
                workerExitOrder.store(
                    order.fetch_add(1, std::memory_order_acq_rel) + 1,
                    std::memory_order_release);
                return PythonRunResult();
            },
            [&](quint64 token) {
                cancelledTokens.append(token);
                cancelCalled.release();
            },
            [&]() { shutdownWaitStarted.release(); });

        owner->trigger();
        ownerReady.release();
        beginDelete.acquire();
        delete owner;
        destructorReturnOrder.store(
            order.fetch_add(1, std::memory_order_acq_rel) + 1,
            std::memory_order_release);
        destructorReturned.store(true, std::memory_order_release);
    });

    const bool ready = ownerReady.tryAcquire(1, 2000);
    const bool started = workerStarted.tryAcquire(1, 2000);
    beginDelete.release();
    const bool cancelWasCalled = cancelCalled.tryAcquire(1, 2000);
    const bool cancellationWasObserved =
            cancellationObserved.tryAcquire(1, 2000);
    const bool waitDidStart =
            shutdownWaitStarted.tryAcquire(1, 2000);
    const bool destructorWaited =
            !destructorReturned.load(std::memory_order_acquire);
    allowWorkerExit.release();
    ownerThread.join();

    QVERIFY(ready);
    QVERIFY(started);
    QVERIFY(cancelWasCalled);
    QVERIFY(cancellationWasObserved);
    QVERIFY(waitDidStart);
    QVERIFY(destructorWaited);
    QCOMPARE(cancelledTokens, QList<quint64>({41}));
    QCOMPARE(busyTrueCount.load(std::memory_order_acquire), 1);
    QCOMPARE(busyFalseCount.load(std::memory_order_acquire), 0);
    QCOMPARE(applyCount.load(std::memory_order_acquire), 0);
    QVERIFY(workerExitOrder.load(std::memory_order_acquire) > 0);
    QVERIFY(
        workerExitOrder.load(std::memory_order_acquire)
        < destructorReturnOrder.load(std::memory_order_acquire));
}

void
TestPythonChartLifecycle::runnerDestructionCancelsAndJoinsActiveRun()
{
    QSemaphore runnerReady;
    QSemaphore beginDelete;
    QSemaphore workerStarted;
    QSemaphore cancelCalled;
    QSemaphore cancellationObserved;
    QSemaphore shutdownWaitStarted;
    QSemaphore allowWorkerExit;
    QList<quint64> cancelledTokens;
    std::atomic_int order{0};
    std::atomic_int workerExitOrder{0};
    std::atomic_int destructorReturnOrder{0};
    std::atomic_int completionCount{0};
    std::atomic_int disposition{
        static_cast<int>(
            PythonChartRunState::RequestDisposition::Queued)};
    std::atomic_bool destructorReturned{false};

    std::thread runnerThread([&]() {
        auto *runner = new PythonChartRunner(
            [&](PythonChartRunInput,
                std::shared_ptr<std::atomic_bool> cancellation) {
                workerStarted.release();
                while (!cancellation->load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                cancellationObserved.release();
                allowWorkerExit.acquire();
                workerExitOrder.store(
                    order.fetch_add(1, std::memory_order_acq_rel) + 1,
                    std::memory_order_release);
                return PythonRunResult();
            },
            [&](quint64 token) {
                cancelledTokens.append(token);
                cancelCalled.release();
            },
            [&](PythonRunResult) {
                completionCount.fetch_add(1, std::memory_order_release);
            },
            [&]() { shutdownWaitStarted.release(); });

        disposition.store(
            static_cast<int>(runner->request(
                input(21, QStringLiteral("blocking-source")))),
            std::memory_order_release);
        runnerReady.release();
        beginDelete.acquire();
        delete runner;
        destructorReturnOrder.store(
            order.fetch_add(1, std::memory_order_acq_rel) + 1,
            std::memory_order_release);
        destructorReturned.store(true, std::memory_order_release);
    });

    const bool ready = runnerReady.tryAcquire(1, 2000);
    const bool workerDidStart = workerStarted.tryAcquire(1, 2000);
    beginDelete.release();
    const bool helperSawCancel = cancelCalled.tryAcquire(1, 2000);
    const bool helperSawCancellation =
            cancellationObserved.tryAcquire(1, 2000);
    const bool waitDidStart =
            shutdownWaitStarted.tryAcquire(1, 2000);
    const bool destructorWaited =
            !destructorReturned.load(std::memory_order_acquire);
    allowWorkerExit.release();
    runnerThread.join();

    QCOMPARE(
        static_cast<PythonChartRunState::RequestDisposition>(
            disposition.load(std::memory_order_acquire)),
        PythonChartRunState::RequestDisposition::Started);
    QVERIFY(ready);
    QVERIFY(workerDidStart);
    QVERIFY(helperSawCancel);
    QVERIFY(helperSawCancellation);
    QVERIFY(waitDidStart);
    QVERIFY(destructorWaited);
    QVERIFY(workerExitOrder.load(std::memory_order_acquire) > 0);
    QVERIFY(
        workerExitOrder.load(std::memory_order_acquire)
        < destructorReturnOrder.load(std::memory_order_acquire));
    QCOMPARE(cancelledTokens, QList<quint64>({21}));
    QCOMPARE(completionCount.load(std::memory_order_acquire), 0);
}

void
TestPythonChartLifecycle::runnerDropsPendingRerun()
{
    QSemaphore workerStarted;
    QList<quint64> cancelledTokens;
    QList<quint64> executionTokens;
    std::mutex executionMutex;
    std::atomic_int completionCount{0};
    PythonRunResult completedResult;

    PythonChartRunner runner(
        [&](PythonChartRunInput runInput,
            std::shared_ptr<std::atomic_bool> cancellation) {
            {
                std::lock_guard<std::mutex> lock(executionMutex);
                executionTokens.append(runInput.token);
            }
            workerStarted.release();
            while (!cancellation->load(std::memory_order_acquire)) {
                QThread::msleep(1);
            }
            PythonRunResult result;
            result.cancelled = false;
            return result;
        },
        [&](quint64 token) {
            cancelledTokens.append(token);
        },
        [&](PythonRunResult result) {
            completedResult = std::move(result);
            completionCount.fetch_add(1, std::memory_order_release);
        });

    QCOMPARE(
        runner.request(input(31, QStringLiteral("running"))),
        PythonChartRunState::RequestDisposition::Started);
    QVERIFY(workerStarted.tryAcquire(1, 2000));
    QCOMPARE(
        runner.request(input(32, QStringLiteral("must-not-run"))),
        PythonChartRunState::RequestDisposition::Queued);
    runner.cancelCurrentAndDropPending();

    QVERIFY(waitUntil([&]() {
        return completionCount.load(std::memory_order_acquire) == 1;
    }));
    {
        std::lock_guard<std::mutex> lock(executionMutex);
        QCOMPARE(executionTokens, QList<quint64>({31}));
    }
    QCOMPARE(cancelledTokens, QList<quint64>({31, 31}));
    QVERIFY(completedResult.cancelled);
    QVERIFY(!runner.active());
}

void
TestPythonChartLifecycle::executionGateCancelsWaitingCaller()
{
    PythonExecutionGate gate;
    PythonExecutionGate::Lease activeLease;
    QCOMPARE(
        gate.acquire(false, {}, activeLease),
        PythonExecutionGate::Admission::Acquired);
    QVERIFY(activeLease);

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    std::atomic_bool waiterFinished{false};
    std::atomic_int admission{
        static_cast<int>(PythonExecutionGate::Admission::Acquired)};
    std::thread waiter(
        [&gate, cancellation, &waiterFinished, &admission]() {
            PythonExecutionGate::Lease lease;
            const PythonExecutionGate::Admission status =
                    gate.acquire(false, cancellation, lease);
            admission.store(
                static_cast<int>(status), std::memory_order_release);
            waiterFinished.store(true, std::memory_order_release);
        });

    const bool enteredWait =
            waitUntil([&gate]() { return gate.waitingCount() == 1; });
    cancellation->store(true, std::memory_order_release);
    gate.wakeWaiters();
    const bool cancelledBeforeLeaseRelease = waitUntil([&waiterFinished]() {
        return waiterFinished.load(std::memory_order_acquire);
    });
    if (!cancelledBeforeLeaseRelease) {
        activeLease = PythonExecutionGate::Lease();
    }
    waiter.join();

    QVERIFY(enteredWait);
    QVERIFY(cancelledBeforeLeaseRelease);
    QCOMPARE(
        static_cast<PythonExecutionGate::Admission>(
            admission.load(std::memory_order_acquire)),
        PythonExecutionGate::Admission::Cancelled);

    activeLease = PythonExecutionGate::Lease();
    PythonExecutionGate::Lease nextLease;
    QCOMPARE(
        gate.acquire(false, {}, nextLease),
        PythonExecutionGate::Admission::Acquired);
    QVERIFY(nextLease);
}

void
TestPythonChartLifecycle::executionGateSerializesWaitingCallers()
{
    static const char childEnvironment[] =
            "GC_PYTHON_GATE_SERIALIZATION_CHILD";
    if (!qEnvironmentVariableIsSet(childEnvironment)) {
        QProcess child;
        QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
        environment.insert(
            QString::fromLatin1(childEnvironment),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("executionGateSerializesWaitingCallers")});

        const bool started = child.waitForStarted(2000);
        const bool finished = started && child.waitForFinished(8000);
        if (!finished) {
            child.kill();
            child.waitForFinished(2000);
        }
        const QByteArray output = child.readAll();
        QVERIFY2(started, output.constData());
        QVERIFY2(finished, output.constData());
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit
                && child.exitCode() == 0,
            output.constData());
        return;
    }

    PythonExecutionGate gate;
    PythonExecutionGate::Lease activeLease;
    QCOMPARE(
        gate.acquire(false, {}, activeLease),
        PythonExecutionGate::Admission::Acquired);

    QSemaphore acquired;
    QSemaphore allowRelease;
    auto firstCancellation =
            std::make_shared<std::atomic_bool>(false);
    auto secondCancellation =
            std::make_shared<std::atomic_bool>(false);
    std::atomic_int activeCallers{0};
    std::atomic_int maximumCallers{0};
    std::atomic_int acquiredCallers{0};
    QList<int> acquisitionOrder;
    std::mutex orderMutex;

    const auto waitForGate =
            [&](int id,
                std::shared_ptr<std::atomic_bool> cancellation) {
                PythonExecutionGate::Lease lease;
                if (gate.acquire(false, cancellation, lease)
                    != PythonExecutionGate::Admission::Acquired) {
                    return;
                }

                const int active =
                        activeCallers.fetch_add(
                            1, std::memory_order_acq_rel) + 1;
                int previousMaximum =
                        maximumCallers.load(std::memory_order_acquire);
                while (active > previousMaximum
                       && !maximumCallers.compare_exchange_weak(
                           previousMaximum, active,
                           std::memory_order_acq_rel)) {
                }
                {
                    std::lock_guard<std::mutex> lock(orderMutex);
                    acquisitionOrder.append(id);
                }
                acquiredCallers.fetch_add(1, std::memory_order_release);
                acquired.release();
                allowRelease.acquire();
                activeCallers.fetch_sub(1, std::memory_order_acq_rel);
            };

    std::thread first(waitForGate, 1, firstCancellation);
    std::thread second(waitForGate, 2, secondCancellation);
    const bool bothWaiting =
            waitUntil([&gate]() { return gate.waitingCount() == 2; });

    bool firstAcquired = false;
    bool oneStillWaiting = false;
    bool secondAcquired = false;
    activeLease = PythonExecutionGate::Lease();
    if (bothWaiting) {
        firstAcquired = acquired.tryAcquire(1, 2000);
        oneStillWaiting = waitUntil(
            [&gate]() { return gate.waitingCount() == 1; });
        allowRelease.release();
        secondAcquired = acquired.tryAcquire(1, 2000);
        allowRelease.release();
    } else {
        allowRelease.release(2);
    }

    firstCancellation->store(true, std::memory_order_release);
    secondCancellation->store(true, std::memory_order_release);
    activeLease = PythonExecutionGate::Lease();
    gate.wakeWaiters();
    allowRelease.release(2);
    first.join();
    second.join();

    QVERIFY(bothWaiting);
    QVERIFY(firstAcquired);
    QVERIFY(oneStillWaiting);
    QVERIFY(secondAcquired);
    QCOMPARE(acquiredCallers.load(std::memory_order_acquire), 2);
    QCOMPARE(maximumCallers.load(std::memory_order_acquire), 1);
    std::lock_guard<std::mutex> lock(orderMutex);
    QCOMPARE(acquisitionOrder.size(), 2);
    QVERIFY(acquisitionOrder[0] != acquisitionOrder[1]);
}

void
TestPythonChartLifecycle::
executionGateRejectsInterpreterLockHolderAndAllocatesTokens()
{
    PythonExecutionGate gate;
    PythonExecutionGate::Lease activeLease;
    QCOMPARE(
        gate.acquire(false, {}, activeLease),
        PythonExecutionGate::Admission::Acquired);

    PythonExecutionGate::Lease rejectedLease;
    QCOMPARE(
        gate.acquire(true, {}, rejectedLease),
        PythonExecutionGate::Admission::Busy);
    QVERIFY(!rejectedLease);

    const quint64 firstToken = gate.allocateToken();
    const quint64 secondToken = gate.allocateToken();
    QVERIFY(firstToken != 0);
    QVERIFY(secondToken != 0);
    QVERIFY(firstToken != secondToken);

    gate.publishToken(firstToken);
    QVERIFY(gate.isPublishedToken(firstToken));
    QVERIFY(!gate.isPublishedToken(secondToken));
    QVERIFY(!gate.isPublishedToken(0));
    gate.publishToken(0);
    QVERIFY(!gate.isPublishedToken(firstToken));
}

void
TestPythonChartLifecycle::executionGateShutdownDrainsAndCloses()
{
    PythonExecutionGate gate;
    QSemaphore active;
    QSemaphore release;
    std::atomic_int waiterAdmission{
        static_cast<int>(PythonExecutionGate::Admission::Acquired)};

    std::thread worker([&]() {
        PythonExecutionGate::Lease lease;
        if (gate.acquire(false, {}, lease)
            == PythonExecutionGate::Admission::Acquired) {
            active.release();
            release.acquire();
        }
    });
    QVERIFY(active.tryAcquire(1, 2000));

    std::thread waiter([&]() {
        PythonExecutionGate::Lease lease;
        waiterAdmission.store(
            static_cast<int>(gate.acquire(false, {}, lease)),
            std::memory_order_release);
    });
    QVERIFY(waitUntil([&]() { return gate.waitingCount() == 1; }));

    std::thread releaser([&release]() {
        QThread::msleep(20);
        release.release();
    });
    const PythonExecutionGate::ShutdownResult shutdownResult =
        gate.beginShutdownAndWait(std::chrono::seconds(2));
    releaser.join();
    worker.join();
    waiter.join();

    QCOMPARE(
        shutdownResult,
        PythonExecutionGate::ShutdownResult::Drained);
    QCOMPARE(
        static_cast<PythonExecutionGate::Admission>(
            waiterAdmission.load(std::memory_order_acquire)),
        PythonExecutionGate::Admission::Closed);
    QVERIFY(gate.isPermanentlyClosed());
    QCOMPARE(gate.allocateToken(), quint64(0));
    PythonExecutionGate::Lease rejected;
    QCOMPARE(
        gate.acquire(false, {}, rejected),
        PythonExecutionGate::Admission::Closed);
    PythonExecutionGate::Entry rejectedEntry;
    QVERIFY(!gate.tryEnter(rejectedEntry));
}

void
TestPythonChartLifecycle::
executionGateShutdownTimeoutAndWrongThreadFailClosed()
{
    PythonExecutionGate wrongThreadGate;
    std::atomic_int wrongThreadResult{
        static_cast<int>(PythonExecutionGate::ShutdownResult::Drained)};
    std::thread wrongThread([&]() {
        wrongThreadResult.store(
            static_cast<int>(wrongThreadGate.beginShutdownAndWait(
                std::chrono::milliseconds(10))),
            std::memory_order_release);
    });
    wrongThread.join();
    QCOMPARE(
        static_cast<PythonExecutionGate::ShutdownResult>(
            wrongThreadResult.load(std::memory_order_acquire)),
        PythonExecutionGate::ShutdownResult::Rejected);
    QVERIFY(!wrongThreadGate.isPermanentlyClosed());

    PythonExecutionGate timeoutGate;
    PythonExecutionGate::Entry entry;
    QVERIFY(timeoutGate.tryEnter(entry));
    QCOMPARE(
        timeoutGate.beginShutdownAndWait(std::chrono::milliseconds(10)),
        PythonExecutionGate::ShutdownResult::TimedOut);
    QVERIFY(timeoutGate.isPermanentlyClosed());
    entry = PythonExecutionGate::Entry();
    QCOMPARE(
        timeoutGate.beginShutdownAndWait(std::chrono::milliseconds(10)),
        PythonExecutionGate::ShutdownResult::Rejected);
}

void
TestPythonChartLifecycle::runtimeFinalizerEnforcesOwnerStateAndOrdering()
{
    using Finalizer = PythonRuntimeFinalizer;
    Finalizer::State state = Finalizer::State::Ready;
    int savedValue = 1;
    int clearValue = 2;
    int catcherValue = 3;
    void *saved = &savedValue;
    void *clear = &clearValue;
    void *catcher = &catcherValue;
    QList<int> calls;
    bool finalizeSawFinalizing = false;
    const Finalizer::Hooks hooks{
        [&](void *value) {
            QCOMPARE(value, static_cast<void *>(&savedValue));
            QVERIFY(saved == nullptr);
            QCOMPARE(state, Finalizer::State::Finalizing);
            calls.append(1);
        },
        [&](void *value) {
            QCOMPARE(state, Finalizer::State::Finalizing);
            calls.append(value == &clearValue ? 2 : 3);
        },
        [&]() {
            finalizeSawFinalizing =
                state == Finalizer::State::Finalizing;
            calls.append(4);
            return 0;
        }
    };

    QCOMPARE(
        Finalizer::run(
            std::this_thread::get_id(), state, saved, clear, catcher, hooks),
        Finalizer::Result::Finalized);
    QCOMPARE(calls, QList<int>({1, 2, 3, 4}));
    QVERIFY(finalizeSawFinalizing);
    QCOMPARE(state, Finalizer::State::Finalized);
    QCOMPARE(saved, nullptr);
    QCOMPARE(clear, nullptr);
    QCOMPARE(catcher, nullptr);
    QCOMPARE(
        Finalizer::run(
            std::this_thread::get_id(), state, saved, clear, catcher, hooks),
        Finalizer::Result::Rejected);
    QCOMPARE(calls, QList<int>({1, 2, 3, 4}));
}

void
TestPythonChartLifecycle::runtimeFinalizerHandlesPartialAndFlushErrors()
{
    using Finalizer = PythonRuntimeFinalizer;
    Finalizer::State state = Finalizer::State::InterpreterInitialized;
    void *saved = nullptr;
    void *clear = nullptr;
    void *catcher = nullptr;
    int restoreCalls = 0;
    int releaseCalls = 0;
    int finalizeCalls = 0;
    Finalizer::Result reentrantResult = Finalizer::Result::Finalized;
    bool finalizeSawFinalizing = false;
    const Finalizer::Hooks hooks{
        [&](void *) { ++restoreCalls; },
        [&](void *) { ++releaseCalls; },
        [&]() {
            ++finalizeCalls;
            finalizeSawFinalizing =
                state == Finalizer::State::Finalizing;
            reentrantResult = Finalizer::run(
                std::this_thread::get_id(),
                state, saved, clear, catcher,
                {[](void *) {}, [](void *) {}, []() { return 0; }});
            return -1;
        }
    };

    QCOMPARE(
        Finalizer::run(
            std::this_thread::get_id(), state, saved, clear, catcher, hooks),
        Finalizer::Result::FinalizedWithErrors);
    QCOMPARE(state, Finalizer::State::Finalized);
    QCOMPARE(restoreCalls, 0);
    QCOMPARE(releaseCalls, 0);
    QCOMPARE(finalizeCalls, 1);
    QVERIFY(finalizeSawFinalizing);
    QCOMPARE(reentrantResult, Finalizer::Result::Rejected);

    state = Finalizer::State::InterpreterInitialized;
    std::atomic_int wrongThreadResult{
        static_cast<int>(Finalizer::Result::Finalized)};
    const std::thread::id ownerThread = std::this_thread::get_id();
    std::thread rejectedThread([&]() {
        wrongThreadResult.store(
            static_cast<int>(Finalizer::run(
                ownerThread, state, saved, clear, catcher, hooks)),
            std::memory_order_release);
    });
    rejectedThread.join();
    QCOMPARE(
        static_cast<Finalizer::Result>(
            wrongThreadResult.load(std::memory_order_acquire)),
        Finalizer::Result::Rejected);
    QCOMPARE(state, Finalizer::State::InterpreterInitialized);
    QCOMPARE(finalizeCalls, 1);

    state = Finalizer::State::Preinitialized;
    QCOMPARE(
        Finalizer::run(
            std::this_thread::get_id(), state, saved, clear, catcher, {}),
        Finalizer::Result::Finalized);
    QCOMPARE(state, Finalizer::State::Finalized);
    QCOMPARE(finalizeCalls, 1);
}

void
TestPythonChartLifecycle::runtimeInitializerTracksFailureOwnership()
{
    using Initializer = PythonRuntimeInitializer;
    using State = Initializer::State;

    const auto run = [](State &state, const int failAt,
                        const bool interpreterExists) {
        QList<int> calls;
        const auto result = Initializer::run(
            state,
            {
                [&]() { calls.append(1); return failAt != 1; },
                [&]() { calls.append(2); return failAt != 2; },
                [&]() { calls.append(3); return failAt != 3; },
                [&]() { calls.append(4); return interpreterExists; }
            });
        return qMakePair(result, calls);
    };

    State state = State::NotStarted;
    auto outcome = run(state, 1, false);
    QCOMPARE(outcome.first, Initializer::Result::Failed);
    QCOMPARE(outcome.second, QList<int>({1}));
    QCOMPARE(state, State::Preinitialized);
    QCOMPARE(run(state, 0, true).first, Initializer::Result::Rejected);

    state = State::NotStarted;
    outcome = run(state, 2, false);
    QCOMPARE(outcome.first, Initializer::Result::Failed);
    QCOMPARE(outcome.second, QList<int>({1, 2}));
    QCOMPARE(state, State::Preinitialized);

    state = State::NotStarted;
    outcome = run(state, 3, false);
    QCOMPARE(outcome.first, Initializer::Result::Failed);
    QCOMPARE(outcome.second, QList<int>({1, 2, 3, 4}));
    QCOMPARE(state, State::Preinitialized);

    state = State::NotStarted;
    outcome = run(state, 3, true);
    QCOMPARE(outcome.first, Initializer::Result::Failed);
    QCOMPARE(state, State::InterpreterInitialized);

    state = State::NotStarted;
    outcome = run(state, 0, true);
    QCOMPARE(outcome.first, Initializer::Result::Initialized);
    QCOMPARE(outcome.second, QList<int>({1, 2, 3, 4}));
    QCOMPARE(state, State::InterpreterInitialized);
}

void
TestPythonChartLifecycle::pathAppenderOwnsReferencesOnEveryExit()
{
    using Appender = PythonPathAppender;
    int sysValue = 1;
    int pathValue = 2;
    int entryValue = 3;

    const auto run = [&](const bool hasPath, const bool isList,
                         const bool hasEntry, const bool appendSucceeds) {
        QList<int> released;
        const auto result = Appender::append(
            &sysValue,
            {
                [&](void *) -> void * {
                    return hasPath ? &pathValue : nullptr;
                },
                [&](void *) -> bool { return isList; },
                [&]() -> void * { return hasEntry ? &entryValue : nullptr; },
                [&](void *, void *) -> bool { return appendSucceeds; },
                [&](void *reference) {
                    released.append(reference == &entryValue ? 3 : 2);
                }
            });
        return qMakePair(result, released);
    };

    auto outcome = run(false, true, true, true);
    QCOMPARE(outcome.first, Appender::Result::MissingPath);
    QVERIFY(outcome.second.isEmpty());

    outcome = run(true, false, true, true);
    QCOMPARE(outcome.first, Appender::Result::InvalidPath);
    QCOMPARE(outcome.second, QList<int>({2}));

    outcome = run(true, true, false, true);
    QCOMPARE(outcome.first, Appender::Result::AllocationFailed);
    QCOMPARE(outcome.second, QList<int>({2}));

    outcome = run(true, true, true, false);
    QCOMPARE(outcome.first, Appender::Result::AppendFailed);
    QCOMPARE(outcome.second, QList<int>({3, 2}));

    outcome = run(true, true, true, true);
    QCOMPARE(outcome.first, Appender::Result::Appended);
    QCOMPARE(outcome.second, QList<int>({3, 2}));

    QCOMPARE(Appender::append(nullptr, {}), Appender::Result::Rejected);
}

void
TestPythonChartLifecycle::processLifetimeOwnerControlsPublication()
{
    using State = FakeEmbeddedRuntime::InitializationState;

    FakeEmbeddedRuntime *alias = nullptr;
    int preInitDestructions = 0;
    {
        ProcessLifetimeRuntimeOwner<FakeEmbeddedRuntime> owner(alias);
        QCOMPARE(owner.initialize([&]() {
            return std::make_unique<FakeEmbeddedRuntime>(
                State::NotStarted, &preInitDestructions);
        }), nullptr);
        QCOMPARE(preInitDestructions, 1);
        QVERIFY(!owner.hasInitializedRuntime());
        QCOMPARE(alias, nullptr);
    }

    int partialDestructions = 0;
    int partialFactoryCalls = 0;
    FakeEmbeddedRuntime *partialStorage = nullptr;
    {
        ProcessLifetimeRuntimeOwner<FakeEmbeddedRuntime> owner(alias);
        QCOMPARE(owner.initialize([&]() {
            ++partialFactoryCalls;
            auto candidate = std::make_unique<FakeEmbeddedRuntime>(
                State::InterpreterInitialized, &partialDestructions);
            partialStorage = candidate.get();
            return candidate;
        }), nullptr);
        QCOMPARE(owner.initialize([&]() {
            ++partialFactoryCalls;
            return std::make_unique<FakeEmbeddedRuntime>(
                State::Ready, &partialDestructions);
        }), nullptr);
        QCOMPARE(partialFactoryCalls, 1);
        QCOMPARE(alias, nullptr);
        QVERIFY(owner.hasInitializedRuntime());
    }
    QCOMPARE(partialDestructions, 0);
    delete partialStorage;
    QCOMPARE(partialDestructions, 1);

    int readyDestructions = 0;
    int readyFactoryCalls = 0;
    FakeEmbeddedRuntime *readyStorage = nullptr;
    {
        ProcessLifetimeRuntimeOwner<FakeEmbeddedRuntime> owner(alias);
        FakeEmbeddedRuntime *published = owner.initialize([&]() {
            ++readyFactoryCalls;
            auto candidate = std::make_unique<FakeEmbeddedRuntime>(
                State::Ready, &readyDestructions);
            readyStorage = candidate.get();
            return candidate;
        });
        QCOMPARE(published, readyStorage);
        QCOMPARE(alias, readyStorage);
        QCOMPARE(owner.initialize([&]() {
            ++readyFactoryCalls;
            return std::unique_ptr<FakeEmbeddedRuntime>();
        }), readyStorage);
        QCOMPARE(readyFactoryCalls, 1);

        std::atomic_bool wrongThreadFactoryCalled{false};
        FakeEmbeddedRuntime *wrongThreadResult = readyStorage;
        std::thread wrongThread([&]() {
            wrongThreadResult = owner.initialize([&]() {
                wrongThreadFactoryCalled = true;
                return std::unique_ptr<FakeEmbeddedRuntime>();
            });
        });
        wrongThread.join();
        QCOMPARE(wrongThreadResult, nullptr);
        QVERIFY(!wrongThreadFactoryCalled);
    }
    QCOMPARE(alias, nullptr);
    QCOMPARE(readyDestructions, 0);
    delete readyStorage;
    QCOMPARE(readyDestructions, 1);

    int retainedDestructions = 0;
    FakeEmbeddedRuntime *retainedStorage = nullptr;
    {
        ProcessLifetimeRuntimeOwner<FakeEmbeddedRuntime> owner(alias);
        retainedStorage = owner.initialize([&]() {
            return std::make_unique<FakeEmbeddedRuntime>(
                State::Ready, &retainedDestructions);
        });
        QVERIFY(retainedStorage != nullptr);
        QVERIFY(owner.shutdownRetainingRuntime(
            [](FakeEmbeddedRuntime *) { return true; }));
        QCOMPARE(alias, retainedStorage);
        QCOMPARE(retainedDestructions, 0);
        QVERIFY(!owner.shutdownRetainingRuntime(
            [](FakeEmbeddedRuntime *) { return true; }));
    }
    QCOMPARE(retainedDestructions, 0);
    QCOMPARE(alias, retainedStorage);
    alias = nullptr;
    delete retainedStorage;
    QCOMPARE(retainedDestructions, 1);
}

void
TestPythonChartLifecycle::pythonConfigurationOwnsInitializationInputs()
{
    QFile header(QStringLiteral(
        GC_TEST_SOURCE_ROOT "/src/Python/PythonEmbed.h"));
    QVERIFY2(header.open(QIODevice::ReadOnly), qPrintable(header.errorString()));
    const QByteArray headerSource = header.readAll();

    QFile implementation(QStringLiteral(
        GC_TEST_SOURCE_ROOT "/src/Python/PythonEmbed.cpp"));
    QVERIFY2(
        implementation.open(QIODevice::ReadOnly),
        qPrintable(implementation.errorString()));
    const QByteArray implementationSource = implementation.readAll();

    QVERIFY(headerSource.contains("PythonRuntimeInitializer.h"));
    QVERIFY(implementationSource.contains(
        "PyConfig_InitPythonConfig(&config);"));
    QVERIFY(implementationSource.contains(
        "PyConfig_SetString("));
    QVERIFY(implementationSource.contains(
        "&guard.config, &guard.config.program_name,"));
    QVERIFY(implementationSource.contains(
        "&guard.config, &guard.config.executable,"));
    QVERIFY(implementationSource.contains(
        "&guard.config, &guard.config.home,"));
    QVERIFY(implementationSource.contains(
        "Py_InitializeFromConfig(&guard.config);"));
    QVERIFY(implementationSource.contains("PyConfig_Clear(&config);"));
    QVERIFY(implementationSource.contains("guard.config.parse_argv = 0;"));
    QVERIFY(implementationSource.contains("guard.config.use_environment = 1;"));
    QVERIFY(implementationSource.contains(
        "guard.config.install_signal_handlers = 0;"));
    QVERIFY(implementationSource.contains("guard.config.site_import = 1;"));
    QVERIFY(implementationSource.contains(
        "guard.config.module_search_paths_set = 0;"));
    QVERIFY(!implementationSource.contains("Py_SetProgramName("));
    QVERIFY(!implementationSource.contains("Py_InitializeEx("));
    QVERIFY(!implementationSource.contains("PyEval_InitThreads("));
    const qsizetype appendBuiltIn = implementationSource.indexOf(
        "PyImport_AppendInittab(");
    const qsizetype initialize = implementationSource.indexOf(
        "Py_InitializeFromConfig(&guard.config);");
    QVERIFY(appendBuiltIn >= 0);
    QVERIFY(initialize > appendBuiltIn);
}

void
TestPythonChartLifecycle::pythonInitializationOwnershipWiring()
{
    QFile header(QStringLiteral(
        GC_TEST_SOURCE_ROOT "/src/Python/PythonEmbed.h"));
    QVERIFY2(header.open(QIODevice::ReadOnly), qPrintable(header.errorString()));
    const QByteArray headerSource = header.readAll();

    QFile mainFile(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/Core/main.cpp"));
    QVERIFY2(mainFile.open(QIODevice::ReadOnly), qPrintable(mainFile.errorString()));
    const QByteArray mainSource = mainFile.readAll();

    QVERIFY(mainSource.contains(
        "ProcessLifetimeRuntimeOwner<PythonEmbed> pythonProcessLifetimeOwner(python);"));
    QVERIFY(mainSource.contains(
        "!pythonProcessLifetimeOwner.hasInitializedRuntime()"));
    QVERIFY(mainSource.contains("pythonProcessLifetimeOwner.initialize([]()"));
    QVERIFY(!mainSource.contains("python = new PythonEmbed()"));

    QFile implementation(QStringLiteral(
        GC_TEST_SOURCE_ROOT "/src/Python/PythonEmbed.cpp"));
    QVERIFY2(
        implementation.open(QIODevice::ReadOnly),
        qPrintable(implementation.errorString()));
    const QByteArray implementationSource = implementation.readAll();
    QVERIFY(implementationSource.contains(
        "initializationState_ = InitializationState::InterpreterInitialized;"));
    QVERIFY(implementationSource.contains(
        "initializationState_ = InitializationState::Ready;"));
    QVERIFY(implementationSource.contains("PyEval_RestoreThread("));
    QVERIFY(implementationSource.contains("Py_FinalizeEx();"));
    QVERIFY(!implementationSource.contains("PyGILState_Check()"));
    QVERIFY(headerSource.contains("std::atomic_bool loaded{false};"));
    QVERIFY(implementationSource.indexOf("executionGate.acquire(")
            < implementationSource.indexOf("PythonGilGuard gil;"));
    QVERIFY(implementationSource.indexOf("executionGate.tryEnter(entry)")
            < implementationSource.lastIndexOf("PythonGilGuard gil;"));
    QVERIFY(mainSource.contains(
        "pythonProcessLifetimeOwner.shutdownRetainingRuntime("));
    QVERIFY(mainSource.indexOf(
        "pythonProcessLifetimeOwner.shutdownRetainingRuntime(")
        < mainSource.indexOf("LocalFileStoreProcess::shutdownReaper()"));
    const qsizetype shutdownFailure = mainSource.indexOf(
        "Python runtime did not drain and finalize safely");
    const qsizetype failStop = mainSource.indexOf("_Exit(EXIT_FAILURE);", shutdownFailure);
    const qsizetype laterTeardown = mainSource.indexOf(
        "LocalFileStoreProcess::shutdownReaper()", shutdownFailure);
    QVERIFY(shutdownFailure >= 0);
    QVERIFY(failStop > shutdownFailure);
    QVERIFY(laterTeardown > failStop);
}

QTEST_GUILESS_MAIN(TestPythonChartLifecycle)
#include "testPythonChartLifecycle.moc"
