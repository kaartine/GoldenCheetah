/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "R/RExecutionGate.h"
#include "Charts/RWidgetExecutionGuard.h"

#include <QFile>
#include <QTest>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

void returnEarly(RExecutionGate &gate, int &cleanupCount)
{
    RExecutionGate::Lease lease = gate.tryAcquire([&cleanupCount]() {
        ++cleanupCount;
    });
    if (lease) return;
    QFAIL("owner thread could not acquire an idle gate");
}

}

class TestRExecutionGate : public QObject
{
    Q_OBJECT

private slots:
    void acquiresCleansAndReleases();
    void rejectsNestedAcquireDuringEventPump();
    void rejectsWrongThread();
    void cleansOnExceptionAndEarlyReturn();
    void moveTransfersSingleCleanup();
    void runsCoalescedWorkAfterCleanupAndRelease();
    void guardsEvaluationContinuations();
    void guardsPumpedObjectLists();
    void productionEntrypointsUseGate();
};

void
TestRExecutionGate::acquiresCleansAndReleases()
{
    RExecutionGate gate;
    int cleanupCount = 0;
    bool gateStayedOccupiedDuringCleanup = false;

    {
        RExecutionGate::Lease first = gate.tryAcquire([&]() {
            ++cleanupCount;
            gateStayedOccupiedDuringCleanup =
                !gate.tryAcquire([]() {}).isValid();
        });
        QVERIFY(first.isValid());
        QCOMPARE(cleanupCount, 0);
    }
    QCOMPARE(cleanupCount, 1);
    QVERIFY(gateStayedOccupiedDuringCleanup);

    RExecutionGate::Lease second = gate.tryAcquire([&cleanupCount]() {
        ++cleanupCount;
    });
    QVERIFY(second);
}

void
TestRExecutionGate::rejectsNestedAcquireDuringEventPump()
{
    RExecutionGate gate;
    int outerCleanupCount = 0;
    bool nestedCleanupCalled = false;

    RExecutionGate::Lease outer = gate.tryAcquire([&outerCleanupCount]() {
        ++outerCleanupCount;
    });
    QVERIFY(outer);

    const auto simulateEventPump = [&]() {
        RExecutionGate::Lease nested = gate.tryAcquire([&nestedCleanupCalled]() {
            nestedCleanupCalled = true;
        });
        QVERIFY(!nested);
    };
    simulateEventPump();

    QVERIFY(!nestedCleanupCalled);
    QCOMPARE(outerCleanupCount, 0);
}

void
TestRExecutionGate::rejectsWrongThread()
{
    RExecutionGate gate;
    std::atomic_bool acquired{true};
    std::atomic_bool cleanupCalled{false};

    std::thread caller([&]() {
        RExecutionGate::Lease lease = gate.tryAcquire([&cleanupCalled]() {
            cleanupCalled.store(true, std::memory_order_release);
        });
        acquired.store(lease.isValid(), std::memory_order_release);
    });
    caller.join();

    QVERIFY(!acquired.load(std::memory_order_acquire));
    QVERIFY(!cleanupCalled.load(std::memory_order_acquire));
    QVERIFY(gate.tryAcquire([]() {}).isValid());
}

void
TestRExecutionGate::cleansOnExceptionAndEarlyReturn()
{
    RExecutionGate gate;
    int cleanupCount = 0;

    try {
        RExecutionGate::Lease lease = gate.tryAcquire([&cleanupCount]() {
            ++cleanupCount;
        });
        QVERIFY(lease);
        throw std::runtime_error("simulated R execution failure");
    } catch (const std::runtime_error &) {
    }
    QCOMPARE(cleanupCount, 1);

    returnEarly(gate, cleanupCount);
    QCOMPARE(cleanupCount, 2);

    RExecutionGate::Lease later = gate.tryAcquire([&cleanupCount]() {
        ++cleanupCount;
    });
    QVERIFY(later);
}

void
TestRExecutionGate::moveTransfersSingleCleanup()
{
    static_assert(!std::is_copy_constructible_v<RExecutionGate::Lease>);
    static_assert(!std::is_copy_assignable_v<RExecutionGate::Lease>);
    static_assert(std::is_nothrow_move_constructible_v<RExecutionGate::Lease>);
    static_assert(std::is_nothrow_move_assignable_v<RExecutionGate::Lease>);

    RExecutionGate gate;
    int cleanupCount = 0;
    {
        RExecutionGate::Lease original = gate.tryAcquire([&cleanupCount]() {
            ++cleanupCount;
        });
        QVERIFY(original);

        RExecutionGate::Lease moved(std::move(original));
        QVERIFY(!original);
        QVERIFY(moved);
        QVERIFY(!gate.tryAcquire([]() {}));
    }
    QCOMPARE(cleanupCount, 1);
    QVERIFY(gate.tryAcquire([]() {}));
}

void
TestRExecutionGate::runsCoalescedWorkAfterCleanupAndRelease()
{
    RExecutionGate gate;
    QStringList order;
    bool reacquiredAfterRelease = false;

    {
        RExecutionGate::Lease outer = gate.tryAcquire(
            [&order]() { order << "cleanup"; },
            [&]() {
                order << "released";
                RExecutionGate::Lease refresh = gate.tryAcquire([]() {});
                reacquiredAfterRelease = refresh.isValid();
            });
        QVERIFY(outer);
        QVERIFY(!gate.tryAcquire([]() {}));
    }

    QCOMPARE(order, QStringList({"cleanup", "released"}));
    QVERIFY(reacquiredAfterRelease);
    QVERIFY(gate.tryAcquire([]() {}));
}

void
TestRExecutionGate::guardsEvaluationContinuations()
{
    QObject *owner = new QObject;
    QObject *console = new QObject;
    QObject *canvas = new QObject;
    RWidgetExecutionGuard guard(owner, {console, canvas});
    QVERIFY(guard.isValid());

    int continuationCount = 0;
    QVERIFY(guard.runAndValidate([]() {}));
    QVERIFY(!guard.runAndValidate([&]() { delete canvas; }));
    if (guard.isValid()) ++continuationCount;
    QCOMPARE(continuationCount, 0);
    delete console;
    delete owner;

    QObject *secondCallOwner = new QObject;
    QObject *secondCallDependency = new QObject;
    RWidgetExecutionGuard secondCallGuard(
        secondCallOwner, {secondCallDependency});
    QVERIFY(secondCallGuard.runAndValidate([]() {}));
    QVERIFY(!secondCallGuard.runAndValidate(
        [&]() { delete secondCallDependency; }));
    if (secondCallGuard.isValid()) ++continuationCount;
    QCOMPARE(continuationCount, 0);
    delete secondCallOwner;

    QObject *printOwner = new QObject;
    RWidgetExecutionGuard printGuard(printOwner);
    QVERIFY(!printGuard.runAndValidate([&]() { delete printOwner; }));
    if (printGuard.isValid()) ++continuationCount;
    QCOMPARE(continuationCount, 0);

    int cleanupCount = 0;
    QObject *throwingOwner = new QObject;
    RWidgetExecutionGuard throwingGuard(throwingOwner);
    try {
        RExecutionGate gate;
        RExecutionGate::Lease lease = gate.tryAcquire([&]() { ++cleanupCount; });
        QVERIFY(lease);
        throwingGuard.runAndValidate([&]() {
            delete throwingOwner;
            throw std::runtime_error("deleted during evaluation");
        });
        QFAIL("evaluation exception was not propagated");
    } catch (const std::runtime_error &) {
        QVERIFY(!throwingGuard.isValid());
    }
    QCOMPARE(cleanupCount, 1);
    QCOMPARE(continuationCount, 0);
}

void
TestRExecutionGate::guardsPumpedObjectLists()
{
    QObject *contextObject = new QObject;
    QObject *athleteObject = new QObject;
    QObject *firstItem = new QObject;
    QObject *secondItem = new QObject;

    const QPointer<QObject> context(contextObject);
    const QPointer<QObject> athlete(athleteObject);
    const QList<QPointer<QObject>> guardedItems = {firstItem, secondItem};
    int dereferenceCount = 0;

    for (const QPointer<QObject> &guardedItem : guardedItems) {
        // Simulate athlete/cache teardown dispatched by processEvents().
        delete athleteObject;
        athleteObject = nullptr;
        delete firstItem;
        firstItem = nullptr;
        delete secondItem;
        secondItem = nullptr;

        if (!context || !athlete || !guardedItem) break;
        ++dereferenceCount;
    }

    QCOMPARE(dereferenceCount, 0);
    QVERIFY(context);
    QVERIFY(!athlete);
    QVERIFY(guardedItems[0].isNull());
    QVERIFY(guardedItems[1].isNull());
    delete contextObject;
}

void
TestRExecutionGate::productionEntrypointsUseGate()
{
    QFile chart(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/Charts/RChart.cpp"));
    QVERIFY2(chart.open(QIODevice::ReadOnly), qPrintable(chart.errorString()));
    const QByteArray source = chart.readAll();

    QCOMPARE(source.count("rtool->tryAcquireExecution("), 2);
    QCOMPARE(source.count("RWidgetExecutionGuard lifetimeGuard("), 2);
    QVERIFY(source.contains("lifetimeGuard.runAndValidate("));
    QVERIFY(source.contains("OverrideCursorGuard cursorGuard;"));
    QVERIFY(source.contains("WidgetUpdatesGuard updatesGuard(this);"));
    QCOMPARE(source.count("QApplication::setOverrideCursor("), 1);
    QCOMPARE(source.count("QApplication::restoreOverrideCursor("), 1);
    QCOMPARE(source.count("setUpdatesEnabled(false)"), 1);
    QCOMPARE(source.count("setUpdatesEnabled(true)"), 1);
    QVERIFY(!source.contains("rtool->context ="));
    QVERIFY(!source.contains("rtool->canvas ="));
    QVERIFY(!source.contains("rtool->perspective ="));
    QVERIFY(!source.contains("rtool->chart ="));

    QFile tool(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/RTool.cpp"));
    QVERIFY2(tool.open(QIODevice::ReadOnly), qPrintable(tool.errorString()));
    const QByteArray toolSource = tool.readAll();
    QVERIFY(toolSource.contains(
        "tryAcquireExecution(NULL, NULL, NULL, NULL)"));
    QVERIFY(toolSource.contains("appearanceRefreshPending.exchange("));
    QVERIFY(toolSource.contains("QList<QPointer<RideItem>> guardedActivities;"));
    QVERIFY(toolSource.contains(
        "executionContext->athlete != executionAthlete.data()"));
    QVERIFY(toolSource.contains("RideItem *item = guardedItem.data();"));
    QVERIFY(!toolSource.contains("foreach(RideItem *item, activities)"));

    QFile toolHeader(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/RTool.h"));
    QVERIFY2(
        toolHeader.open(QIODevice::ReadOnly),
        qPrintable(toolHeader.errorString()));
    const QByteArray toolHeaderSource = toolHeader.readAll();
    QVERIFY(toolHeaderSource.contains("QPointer<RCanvas> canvas;"));
    QVERIFY(toolHeaderSource.contains("QPointer<RChart> chart;"));
    QVERIFY(!toolHeaderSource.contains("RCanvas *canvas;"));
    QVERIFY(!toolHeaderSource.contains("RChart *chart;"));
}

QTEST_APPLESS_MAIN(TestRExecutionGate)
#include "testRExecutionGate.moc"
