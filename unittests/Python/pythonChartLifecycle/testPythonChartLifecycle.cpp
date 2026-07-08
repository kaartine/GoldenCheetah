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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace {

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

QTEST_GUILESS_MAIN(TestPythonChartLifecycle)
#include "testPythonChartLifecycle.moc"
