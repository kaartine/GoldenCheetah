/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFileCacheWriteError.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct ThrowingNotifyCopyState {
    std::atomic<int> copies {0};
    std::atomic<int> throwOnCopy {0};
};

class ThrowingNotifyCopy
{
public:
    explicit ThrowingNotifyCopy(
        std::shared_ptr<ThrowingNotifyCopyState> state)
        : state_(std::move(state))
    {
    }

    ThrowingNotifyCopy(const ThrowingNotifyCopy &other)
        : state_(other.state_)
    {
        const int copy = state_->copies.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (copy == state_->throwOnCopy.load(std::memory_order_relaxed))
            throw std::runtime_error("injected notify copy failure");
    }

    void operator()(const QString &) const
    {
    }

private:
    std::shared_ptr<ThrowingNotifyCopyState> state_;
};

} // namespace

class TestRideFileCacheWriteError : public QObject
{
    Q_OBJECT

private slots:
    void concurrentReportsScheduleOneOwnerDelivery();
    void reportDuringFailedDispatchRetriesInsteadOfBeingLost();
    void ownerBoundQueueDropsDeliveryAfterOwnerDestruction();
    void staleFailedDispatchDeliveryCannotReplaceRetry();
    void synchronousDeliveryWinsDispatchResult_data();
    void synchronousDeliveryWinsDispatchResult();
    void throwingDispatchCanBeRetried();
    void throwAfterSynchronousDeliveryReleasesPendingCallbacks();
    void deliveryConstructionFailureCanBeRetried();
    void dispatchFailureRetryIsBounded();
    void reentrantReportDoesNotDeadlockDispatcher();
    void failedDispatchCanBeRetried();
    void firstScheduledFailureIsRetained();
};

void
TestRideFileCacheWriteError::concurrentReportsScheduleOneOwnerDelivery()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    QMutex deliveryMutex;
    QVector<RideFileCacheWriteErrorCoordinator::Delivery> deliveries;
    const auto dispatch =
        [&](RideFileCacheWriteErrorCoordinator::Delivery delivery) {
            QMutexLocker locker(&deliveryMutex);
            deliveries.append(std::move(delivery));
            return true;
        };

    int notificationCount = 0;
    QThread *notificationThread = nullptr;
    QString deliveredMessage;
    const auto notify = [&](const QString &message) {
        ++notificationCount;
        notificationThread = QThread::currentThread();
        deliveredMessage = message;
    };

    constexpr int WorkerCount = 24;
    std::atomic<int> ready {0};
    std::atomic<bool> start {false};
    std::atomic<int> scheduled {0};
    std::atomic<int> coalesced {0};
    std::vector<std::thread> workers;
    workers.reserve(WorkerCount);
    for (int index = 0; index < WorkerCount; ++index) {
        workers.emplace_back([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            const auto result = coordinator.report(
                QStringLiteral("/cache/%1.cpx").arg(index),
                QStringLiteral("injected failure %1").arg(index),
                dispatch,
                notify);
            if (result == RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled)
                scheduled.fetch_add(1, std::memory_order_relaxed);
            else if (result == RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced)
                coalesced.fetch_add(1, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) != WorkerCount)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers)
        worker.join();

    QCOMPARE(scheduled.load(), 1);
    QCOMPARE(coalesced.load(), WorkerCount - 1);
    QCOMPARE(notificationCount, 0);

    RideFileCacheWriteErrorCoordinator::Delivery delivery;
    {
        QMutexLocker locker(&deliveryMutex);
        QCOMPARE(deliveries.size(), 1);
        delivery = std::move(deliveries.front());
        deliveries.clear();
    }
    const auto duplicateDelivery = delivery;
    delivery();
    duplicateDelivery();

    QCOMPARE(notificationCount, 1);
    QCOMPARE(notificationThread, QThread::currentThread());
    QVERIFY(deliveredMessage.startsWith(
        QStringLiteral("Cannot create cache file /cache/")));
    QVERIFY(deliveredMessage.contains(
        QStringLiteral("injected failure")));

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/later.cpx"),
            QStringLiteral("later failure"),
            dispatch,
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);
    QCOMPARE(notificationCount, 1);
}

void TestRideFileCacheWriteError::failedDispatchCanBeRetried()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    int notificationCount = 0;
    RideFileCacheWriteErrorCoordinator::Delivery delivery;
    const auto notify = [&](const QString &) {
        ++notificationCount;
    };

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/first.cpx"),
            QStringLiteral("first failure"),
            [](RideFileCacheWriteErrorCoordinator::Delivery) {
                return false;
            },
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed);

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/retry.cpx"),
            QStringLiteral("retry failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                delivery = std::move(pending);
                return true;
            },
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QVERIFY(static_cast<bool>(delivery));
    QCOMPARE(notificationCount, 0);

    delivery();
    QCOMPARE(notificationCount, 1);
}

void
TestRideFileCacheWriteError::
reportDuringFailedDispatchRetriesInsteadOfBeingLost()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool firstDispatchEntered = false;
    bool releaseFirstDispatch = false;
    RideFileCacheWriteErrorCoordinator::Delivery secondDelivery;
    std::atomic<int> notificationCount {0};
    int activeDispatchCalls = 0;
    bool coalescedDispatchCalled = false;
    QString deliveredMessage;
    RideFileCacheWriteErrorCoordinator::ReportResult firstResult =
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed;
    const auto notify = [&](const QString &message) {
        notificationCount.fetch_add(1, std::memory_order_relaxed);
        deliveredMessage = message;
    };

    std::thread first([&]() {
        firstResult = coordinator.report(
            QStringLiteral("/cache/first.cpx"),
            QStringLiteral("first failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                ++activeDispatchCalls;
                if (activeDispatchCalls == 1) {
                    std::unique_lock<std::mutex> lock(gateMutex);
                    firstDispatchEntered = true;
                    gateChanged.notify_all();
                    gateChanged.wait_for(
                        lock,
                        std::chrono::seconds(5),
                        [&]() { return releaseFirstDispatch; });
                    return false;
                }
                secondDelivery = std::move(pending);
                return true;
            },
            notify);
    });

    bool firstDispatchEnteredInTime;
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        firstDispatchEnteredInTime = gateChanged.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return firstDispatchEntered; });
    }
    if (!firstDispatchEnteredInTime) {
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            releaseFirstDispatch = true;
        }
        gateChanged.notify_all();
        first.join();
        QFAIL("first dispatch did not start");
    }

    bool safetyReleasedDispatch = false;
    std::thread safetyRelease([&]() {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gateChanged.wait_for(
                lock,
                std::chrono::seconds(5),
                [&]() { return releaseFirstDispatch; })) {
            safetyReleasedDispatch = true;
            releaseFirstDispatch = true;
            lock.unlock();
            gateChanged.notify_all();
        }
    });

    const auto secondResult = coordinator.report(
        QStringLiteral("/cache/second.cpx"),
        QStringLiteral("second failure"),
        [&](RideFileCacheWriteErrorCoordinator::Delivery) {
            coalescedDispatchCalled = true;
            return true;
        },
        notify);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseFirstDispatch = true;
    }
    gateChanged.notify_all();
    safetyRelease.join();
    first.join();

    QVERIFY(!safetyReleasedDispatch);
    QCOMPARE(
        firstResult,
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QCOMPARE(
        secondResult,
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);
    QCOMPARE(activeDispatchCalls, 2);
    QVERIFY(!coalescedDispatchCalled);
    QVERIFY(static_cast<bool>(secondDelivery));
    QCOMPARE(notificationCount.load(), 0);

    secondDelivery();
    QCOMPARE(notificationCount.load(), 1);
    QVERIFY(deliveredMessage.contains(QStringLiteral("second failure")));
}

void
TestRideFileCacheWriteError::
ownerBoundQueueDropsDeliveryAfterOwnerDestruction()
{
    int deliveryCount = 0;
    QThread *deliveryThread = nullptr;
    QObject liveOwner;
    bool liveQueued = false;
    std::thread liveWorker([&]() {
        liveQueued = RideFileCacheWriteErrorCoordinator::queueForOwner(
            &liveOwner,
            [&]() {
                ++deliveryCount;
                deliveryThread = QThread::currentThread();
            });
    });
    liveWorker.join();
    QVERIFY(liveQueued);
    QCoreApplication::sendPostedEvents(&liveOwner, QEvent::MetaCall);
    QCOMPARE(deliveryCount, 1);
    QCOMPARE(deliveryThread, QThread::currentThread());

    QObject *doomedOwner = new QObject;
    bool doomedQueued = false;
    std::thread doomedWorker([&]() {
        doomedQueued = RideFileCacheWriteErrorCoordinator::queueForOwner(
            doomedOwner,
            [&]() { ++deliveryCount; });
    });
    doomedWorker.join();
    QVERIFY(doomedQueued);
    delete doomedOwner;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(deliveryCount, 1);
}

void
TestRideFileCacheWriteError::
staleFailedDispatchDeliveryCannotReplaceRetry()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    RideFileCacheWriteErrorCoordinator::Delivery staleDelivery;
    RideFileCacheWriteErrorCoordinator::Delivery retryDelivery;
    int notificationCount = 0;
    QString deliveredMessage;
    const auto notify = [&](const QString &message) {
        ++notificationCount;
        deliveredMessage = message;
    };

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/stale.cpx"),
            QStringLiteral("stale failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                staleDelivery = std::move(pending);
                return false;
            },
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed);
    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/retry.cpx"),
            QStringLiteral("retry failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                retryDelivery = std::move(pending);
                return true;
            },
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);

    QVERIFY(static_cast<bool>(staleDelivery));
    QVERIFY(static_cast<bool>(retryDelivery));
    staleDelivery();
    QCOMPARE(notificationCount, 0);
    retryDelivery();
    QCOMPARE(notificationCount, 1);
    QVERIFY(deliveredMessage.contains(QStringLiteral("retry failure")));
}

void
TestRideFileCacheWriteError::synchronousDeliveryWinsDispatchResult_data()
{
    QTest::addColumn<bool>("accepted");
    QTest::newRow("accepted") << true;
    QTest::newRow("reported-failed") << false;
}

void
TestRideFileCacheWriteError::synchronousDeliveryWinsDispatchResult()
{
    QFETCH(bool, accepted);
    RideFileCacheWriteErrorCoordinator coordinator;
    int notificationCount = 0;

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/synchronous.cpx"),
            QStringLiteral("synchronous failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                pending();
                return accepted;
            },
            [&](const QString &) { ++notificationCount; }),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QCOMPARE(notificationCount, 1);
}

void TestRideFileCacheWriteError::throwingDispatchCanBeRetried()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    bool threw = false;
    bool nestedDispatchCalled = false;
    RideFileCacheWriteErrorCoordinator::ReportResult nestedResult =
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed;
    try {
        coordinator.report(
            QStringLiteral("/cache/throw.cpx"),
            QStringLiteral("throwing failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery) -> bool {
                nestedResult = coordinator.report(
                    QStringLiteral("/cache/pending.cpx"),
                    QStringLiteral("pending failure"),
                    [&](RideFileCacheWriteErrorCoordinator::Delivery) {
                        nestedDispatchCalled = true;
                        return true;
                    },
                    [](const QString &) {});
                throw std::runtime_error("injected dispatch exception");
            },
            [](const QString &) {});
    } catch (const std::runtime_error &) {
        threw = true;
    }
    QVERIFY(threw);
    QCOMPARE(
        nestedResult,
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);
    QVERIFY(!nestedDispatchCalled);

    RideFileCacheWriteErrorCoordinator::Delivery retryDelivery;
    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/retry.cpx"),
            QStringLiteral("retry failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                retryDelivery = std::move(pending);
                return true;
            },
            [](const QString &) {}),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QVERIFY(static_cast<bool>(retryDelivery));
}

void TestRideFileCacheWriteError::reentrantReportDoesNotDeadlockDispatcher()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    RideFileCacheWriteErrorCoordinator::ReportResult nestedResult =
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed;
    RideFileCacheWriteErrorCoordinator::Delivery outerDelivery;
    bool nestedDispatchCalled = false;
    std::atomic<int> notificationCount {0};
    const auto notify = [&](const QString &) {
        notificationCount.fetch_add(1, std::memory_order_relaxed);
    };

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/outer.cpx"),
            QStringLiteral("outer failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                outerDelivery = std::move(pending);
                nestedResult = coordinator.report(
                    QStringLiteral("/cache/nested.cpx"),
                    QStringLiteral("nested failure"),
                    [&](RideFileCacheWriteErrorCoordinator::Delivery) {
                        nestedDispatchCalled = true;
                        return true;
                    },
                    notify);
                return true;
            },
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);

    QCOMPARE(
        nestedResult,
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);
    QVERIFY(!nestedDispatchCalled);
    QVERIFY(static_cast<bool>(outerDelivery));
    QCOMPARE(notificationCount.load(), 0);
    outerDelivery();
    QCOMPARE(notificationCount.load(), 1);
}

void
TestRideFileCacheWriteError::
throwAfterSynchronousDeliveryReleasesPendingCallbacks()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    auto callbackLifetime = std::make_shared<int>(1);
    const std::weak_ptr<int> callbackLifetimeObserver =
        callbackLifetime;
    RideFileCacheWriteErrorCoordinator::ReportResult nestedResult =
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed;
    int notificationCount = 0;
    bool threw = false;

    try {
        coordinator.report(
            QStringLiteral("/cache/outer.cpx"),
            QStringLiteral("outer failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery delivery)
                -> bool {
                nestedResult = coordinator.report(
                    QStringLiteral("/cache/pending.cpx"),
                    QStringLiteral("pending failure"),
                    [callbackLifetime](
                        RideFileCacheWriteErrorCoordinator::Delivery) {
                        return true;
                    },
                    [](const QString &) {});
                delivery();
                throw std::runtime_error(
                    "injected post-delivery exception");
            },
            [&](const QString &) { ++notificationCount; });
    } catch (const std::runtime_error &) {
        threw = true;
    }
    callbackLifetime.reset();

    QVERIFY(threw);
    QCOMPARE(
        nestedResult,
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);
    QCOMPARE(notificationCount, 1);
    QVERIFY(callbackLifetimeObserver.expired());
}

void
TestRideFileCacheWriteError::deliveryConstructionFailureCanBeRetried()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    const auto copyState =
        std::make_shared<ThrowingNotifyCopyState>();
    RideFileCacheWriteErrorCoordinator::Notify throwingNotify =
        ThrowingNotifyCopy(copyState);
    copyState->throwOnCopy.store(
        copyState->copies.load(std::memory_order_relaxed) + 2,
        std::memory_order_relaxed);

    bool threw = false;
    try {
        coordinator.report(
            QStringLiteral("/cache/construction.cpx"),
            QStringLiteral("construction failure"),
            [](RideFileCacheWriteErrorCoordinator::Delivery) {
                return true;
            },
            throwingNotify);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    QVERIFY(threw);

    RideFileCacheWriteErrorCoordinator::Delivery retryDelivery;
    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/retry.cpx"),
            QStringLiteral("retry failure"),
            [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
                retryDelivery = std::move(pending);
                return true;
            },
            [](const QString &) {}),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QVERIFY(static_cast<bool>(retryDelivery));
}

void TestRideFileCacheWriteError::dispatchFailureRetryIsBounded()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    int dispatchCalls = 0;
    int coalescedReports = 0;
    std::function<bool(
        RideFileCacheWriteErrorCoordinator::Delivery)> failingDispatch;
    failingDispatch =
        [&](RideFileCacheWriteErrorCoordinator::Delivery) {
            ++dispatchCalls;
            if (dispatchCalls <= 3) {
                const auto nestedResult = coordinator.report(
                    QStringLiteral("/cache/pending.cpx"),
                    QStringLiteral("pending failure"),
                    failingDispatch,
                    [](const QString &) {});
                if (nestedResult
                    == RideFileCacheWriteErrorCoordinator::
                        ReportResult::Coalesced) {
                    ++coalescedReports;
                }
            }
            return false;
        };

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/outer.cpx"),
            QStringLiteral("outer failure"),
            failingDispatch,
            [](const QString &) {}),
        RideFileCacheWriteErrorCoordinator::ReportResult::DispatchFailed);
    QCOMPARE(dispatchCalls, 2);
    QCOMPARE(coalescedReports, 2);
}

void TestRideFileCacheWriteError::firstScheduledFailureIsRetained()
{
    RideFileCacheWriteErrorCoordinator coordinator;
    RideFileCacheWriteErrorCoordinator::Delivery delivery;
    QString deliveredMessage;
    const auto dispatch =
        [&](RideFileCacheWriteErrorCoordinator::Delivery pending) {
            delivery = std::move(pending);
            return true;
        };
    const auto notify = [&](const QString &message) {
        deliveredMessage = message;
    };

    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/first.cpx"),
            QStringLiteral("first failure"),
            dispatch,
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Scheduled);
    QCOMPARE(
        coordinator.report(
            QStringLiteral("/cache/second.cpx"),
            QStringLiteral("second failure"),
            dispatch,
            notify),
        RideFileCacheWriteErrorCoordinator::ReportResult::Coalesced);

    QVERIFY(static_cast<bool>(delivery));
    delivery();
    QCOMPARE(
        deliveredMessage,
        QStringLiteral(
            "Cannot create cache file /cache/first.cpx: first failure."));
}

QTEST_GUILESS_MAIN(TestRideFileCacheWriteError)

#include "testRideFileCacheWriteError.moc"
