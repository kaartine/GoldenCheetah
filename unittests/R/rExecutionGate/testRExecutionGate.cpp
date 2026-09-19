/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "R/RExecutionGate.h"
#include "R/RDeferredUiWork.h"
#include "R/RRuntimeInitialization.h"
#include "Charts/RConsolePromptPolicy.h"
#include "Charts/RWidgetExecutionGuard.h"

#include <QCoreApplication>
#include <QFile>
#include <QTest>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

class DeferredUiTarget : public QObject
{
    Q_OBJECT

public:
    DeferredUiTarget(QString name, QStringList &order)
        : name_(std::move(name)), order_(order)
    {
    }

public slots:
    void runScript() { order_ << name_ + QStringLiteral(":chart"); }
    void ensurePrompt() { order_ << name_ + QStringLiteral(":prompt"); }

private:
    QString name_;
    QStringList &order_;
};

class RedeferPromptTarget : public QObject
{
    Q_OBJECT

public:
    RedeferPromptTarget(
        RExecutionGate &gate,
        RDeferredUiWork &pending,
        QStringList &order)
        : gate_(gate), pending_(pending), order_(order)
    {
    }

public slots:
    void ensurePrompt()
    {
        if (gate_.canDeferFromCurrentThread()) {
            pending_.requestConsolePrompt(this);
            return;
        }
        order_ << QStringLiteral("prompt");
    }

private:
    RExecutionGate &gate_;
    RDeferredUiWork &pending_;
    QStringList &order_;
};

class FaultingRuntime
{
public:
    enum class State { NotStarted, InterpreterInitialized };

    void initialize()
    {
        state = State::InterpreterInitialized;
        throw std::runtime_error("post-initialization setup failed");
    }

    State state = State::NotStarted;
};

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
    void exposesOnlyActiveOwnerForDeferral();
    void cleansOnExceptionAndEarlyReturn();
    void moveTransfersSingleCleanup();
    void runsCoalescedWorkAfterCleanupAndRelease();
    void guardsEvaluationContinuations();
    void guardsPumpedObjectLists();
    void guardsActiveExecutionBindings();
    void coalescesAndPostsGuardedUiWork();
    void redefersPromptAcrossNewLease();
    void drainsDeferredWorkOnException();
    void appliesConsolePromptPolicy();
    void preservesRuntimeStateAcrossInitializationException();
    void productionRuntimeInitializationIsTwoPhase();
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
TestRExecutionGate::exposesOnlyActiveOwnerForDeferral()
{
    RExecutionGate gate;
    QVERIFY(!gate.canDeferFromCurrentThread());

    RExecutionGate::Lease lease = gate.tryAcquire([]() {});
    QVERIFY(lease);
    QVERIFY(gate.canDeferFromCurrentThread());

    std::atomic_bool wrongThreadCouldDefer{true};
    std::thread caller([&]() {
        wrongThreadCouldDefer.store(
            gate.canDeferFromCurrentThread(), std::memory_order_release);
    });
    caller.join();
    QVERIFY(!wrongThreadCouldDefer.load(std::memory_order_acquire));

    lease = RExecutionGate::Lease();
    QVERIFY(!gate.canDeferFromCurrentThread());
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
TestRExecutionGate::guardsActiveExecutionBindings()
{
    QObject *contextObject = new QObject;
    QObject *athleteObject = new QObject;
    QObject *perspectiveObject = new QObject;
    QObject *replacementAthlete = new QObject;

    const QPointer<QObject> context(contextObject);
    const QPointer<QObject> boundAthlete(athleteObject);
    const QPointer<QObject> perspective(perspectiveObject);
    const auto bindingIsValid = [&](QObject *currentAthlete) {
        return context && boundAthlete && currentAthlete == boundAthlete.data();
    };

    QVERIFY(bindingIsValid(athleteObject));
    QVERIFY(perspective);
    QVERIFY(!bindingIsValid(replacementAthlete));

    delete athleteObject;
    athleteObject = nullptr;
    QVERIFY(!bindingIsValid(replacementAthlete));

    delete perspectiveObject;
    perspectiveObject = nullptr;
    QVERIFY(!perspective);

    delete contextObject;
    contextObject = nullptr;
    QVERIFY(!bindingIsValid(replacementAthlete));
    delete replacementAthlete;
}

void
TestRExecutionGate::coalescesAndPostsGuardedUiWork()
{
    QStringList order;
    auto *firstChart = new DeferredUiTarget(QStringLiteral("first"), order);
    auto *deletedBeforeTake =
        new DeferredUiTarget(QStringLiteral("before"), order);
    auto *secondChart = new DeferredUiTarget(QStringLiteral("second"), order);
    auto *firstPrompt = new DeferredUiTarget(QStringLiteral("first"), order);
    auto *deletedAfterPost =
        new DeferredUiTarget(QStringLiteral("after"), order);

    RDeferredUiWork pending;
    QVERIFY(pending.requestChartRerun(firstChart));
    QVERIFY(pending.requestChartRerun(firstChart));
    QVERIFY(pending.requestChartRerun(deletedBeforeTake));
    QVERIFY(pending.requestChartRerun(secondChart));
    QVERIFY(pending.requestConsolePrompt(firstPrompt));
    QVERIFY(pending.requestConsolePrompt(firstPrompt));
    QVERIFY(pending.requestConsolePrompt(deletedAfterPost));
    QVERIFY(!pending.requestChartRerun(nullptr));

    delete deletedBeforeTake;
    RDeferredUiWork::Batch batch = pending.take();
    QCOMPARE(batch.chartReruns.size(), 3);
    QCOMPARE(batch.consolePrompts.size(), 2);
    QVERIFY(pending.take().isEmpty());

    RDeferredUiWork::post(std::move(batch));
    delete deletedAfterPost;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();

    QCOMPARE(
        order,
        QStringList({
            QStringLiteral("first:chart"),
            QStringLiteral("second:chart"),
            QStringLiteral("first:prompt")}));

    // A later active lease may request one subsequent queued generation.
    QVERIFY(pending.requestChartRerun(firstChart));
    RDeferredUiWork::post(pending.take());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(order.count(QStringLiteral("first:chart")), 2);

    delete firstChart;
    delete secondChart;
    delete firstPrompt;
}

void
TestRExecutionGate::redefersPromptAcrossNewLease()
{
    RExecutionGate gate;
    RDeferredUiWork pending;
    QStringList order;
    RedeferPromptTarget prompt(gate, pending, order);
    bool releasedBeforePost = false;

    QVERIFY(pending.requestConsolePrompt(&prompt));
    RDeferredUiWork::post(pending.take());
    {
        RExecutionGate::Lease later = gate.tryAcquire(
            []() {},
            [&]() {
                releasedBeforePost = !gate.canDeferFromCurrentThread();
                RDeferredUiWork::post(pending.take());
            });
        QVERIFY(later);

        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(order.isEmpty());
        QCOMPARE(pending.take().consolePrompts.size(), 1);
        QVERIFY(pending.requestConsolePrompt(&prompt));
    }

    QVERIFY(releasedBeforePost);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(order, QStringList({QStringLiteral("prompt")}));
}

void
TestRExecutionGate::drainsDeferredWorkOnException()
{
    RExecutionGate gate;
    RDeferredUiWork pending;
    QStringList order;
    DeferredUiTarget chart(QStringLiteral("throw"), order);
    int drainCount = 0;

    try {
        RExecutionGate::Lease lease = gate.tryAcquire(
            []() {},
            [&]() {
                ++drainCount;
                RDeferredUiWork::post(pending.take());
            });
        QVERIFY(lease);
        QVERIFY(pending.requestChartRerun(&chart));
        throw std::runtime_error("simulated deferred execution failure");
    } catch (const std::runtime_error &) {
    }

    QCOMPARE(drainCount, 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
    QCOMPARE(order, QStringList({QStringLiteral("throw:chart")}));
    QVERIFY(pending.take().isEmpty());
}

void
TestRExecutionGate::appliesConsolePromptPolicy()
{
    QVERIFY(!RConsolePromptPolicy::hasPromptPrefix(QString()));
    QVERIFY(RConsolePromptPolicy::hasPromptPrefix(QStringLiteral("> ")));
    QVERIFY(RConsolePromptPolicy::hasPromptPrefix(QStringLiteral("> typed")));
    QVERIFY(RConsolePromptPolicy::hasPromptPrefix(QStringLiteral(">>")));
    QVERIFY(RConsolePromptPolicy::hasPromptPrefix(QStringLiteral(">>typed")));
    QVERIFY(!RConsolePromptPolicy::hasPromptPrefix(
        QStringLiteral("output > ")));
    QCOMPARE(RConsolePromptPolicy::prompt(false), QStringLiteral("> "));
    QCOMPARE(RConsolePromptPolicy::prompt(true), QStringLiteral(">>"));
    QVERIFY(!RConsolePromptPolicy::shouldAppendDeferred(
        QStringLiteral("> result")));

    QString ordinaryOutput = QStringLiteral("> result");
    ordinaryOutput += RConsolePromptPolicy::prompt(false);
    QCOMPARE(ordinaryOutput, QStringLiteral("> result> "));
}

void
TestRExecutionGate::preservesRuntimeStateAcrossInitializationException()
{
    FaultingRuntime runtime;
    FaultingRuntime::State ownerState = FaultingRuntime::State::NotStarted;
    bool caught = false;

    try {
        initializeRuntimeAndSynchronize(
            &runtime,
            [&ownerState](FaultingRuntime *candidate) {
                if (candidate->state
                    != FaultingRuntime::State::NotStarted) {
                    ownerState = FaultingRuntime::State::InterpreterInitialized;
                }
            });
    } catch (const std::runtime_error &) {
        caught = true;
    }

    QVERIFY(caught);
    QCOMPARE(ownerState, FaultingRuntime::State::InterpreterInitialized);
    const bool cleanupOrRetryAllowed =
        ownerState == FaultingRuntime::State::NotStarted;
    QVERIFY(!cleanupOrRetryAllowed);
}

void
TestRExecutionGate::productionRuntimeInitializationIsTwoPhase()
{
    QFile embed(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/REmbed.cpp"));
    QVERIFY2(embed.open(QIODevice::ReadOnly), qPrintable(embed.errorString()));
    const QByteArray embedSource = embed.readAll();
    const qsizetype initialize = embedSource.indexOf("REmbed::initialize()");
    const qsizetype initCall = embedSource.indexOf("Rf_initEmbeddedR(", initialize);
    const qsizetype initCheck = embedSource.indexOf("if (initResult < 0)", initCall);
    const qsizetype initializedState = embedSource.indexOf(
        "InitializationState::InterpreterInitialized", initCheck);
    const qsizetype replSetup = embedSource.indexOf("R_ReplDLLinit()", initializedState);
    const qsizetype readyState = embedSource.indexOf(
        "InitializationState::Ready", replSetup);
    QVERIFY(initialize >= 0);
    QVERIFY(initCall > initialize);
    QVERIFY(initCheck > initCall);
    QVERIFY(initializedState > initCheck);
    QVERIFY(replSetup > initializedState);
    QVERIFY(readyState > replSetup);
    QVERIFY(!embedSource.contains("Rf_endEmbeddedR("));
    QVERIFY(!embedSource.contains("R_RunExitFinalizers("));
    QVERIFY(!embedSource.contains("R_CleanTempDir("));

    QFile embedHeader(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/REmbed.h"));
    QVERIFY2(
        embedHeader.open(QIODevice::ReadOnly),
        qPrintable(embedHeader.errorString()));
    const QByteArray embedHeaderSource = embedHeader.readAll();
    QVERIFY(embedHeaderSource.contains("void initialize();"));
    QVERIFY(embedHeaderSource.contains("NotStarted"));
    QVERIFY(embedHeaderSource.contains("InterpreterInitialized"));
    QVERIFY(embedHeaderSource.contains("Ready"));

    QFile tool(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/RTool.cpp"));
    QVERIFY2(tool.open(QIODevice::ReadOnly), qPrintable(tool.errorString()));
    const QByteArray toolSource = tool.readAll();
    QVERIFY(toolSource.contains("R = new REmbed();"));
    QVERIFY(toolSource.contains("initializeRuntimeAndSynchronize(R"));
    QVERIFY(toolSource.contains(
        "initializationState_ = InitializationState::InterpreterInitialized;"));
    QVERIFY(toolSource.contains(
        "initializationState_ = InitializationState::Ready;"));
    QCOMPARE(toolSource.count("R = NULL;"), 1);
    QVERIFY(toolSource.contains(
        "if (initializationState_ != InitializationState::NotStarted) return;"));
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
    QVERIFY(source.contains("rtool->requestChartRerun(this);"));
    QCOMPARE(source.count("rtool->requestConsolePrompt(this)"), 2);
    QVERIFY(source.contains("void RConsole::ensurePrompt()"));
    QVERIFY(source.contains(
        "RConsolePromptPolicy::shouldAppendDeferred("));
    QCOMPARE(source.count("appendPrompt();"), 2);
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
    QVERIFY(toolSource.contains("RProtectionScope protectionScope;"));
    QVERIFY(toolSource.contains("protectionScope.protectValue(df);"));
    QVERIFY(toolSource.contains(
        "list = protectionScope.protectValue(Rf_allocVector(VECSXP, f.count()));"));
    QVERIFY(toolSource.contains(
        "rownames = protectionScope.protectValue(Rf_allocVector(STRSXP, f.count()));"));
    QVERIFY(!toolSource.contains("UNPROTECT(3); // list and names and rownames"));
    QCOMPARE(toolSource.count("RProtectionScope protectionScope;"), 3);
    QCOMPARE(toolSource.count("protectionScope.protectValue(df);"), 3);
    QVERIFY(toolSource.contains(
        "boundAthlete = executionContext ? executionContext->athlete : nullptr;"));
    QVERIFY(toolSource.contains(
        "boundContext->athlete == athlete"));
    QCOMPARE(
        toolSource.count("executionGate.canDeferFromCurrentThread()"),
        2);
    QVERIFY(toolSource.contains(
        "RDeferredUiWork::post(deferredUiWork.take());"));
    QCOMPARE(
        toolSource.count("!rtool->hasValidAthleteBinding()"),
        16);
    QCOMPARE(toolSource.count("if (rtool->perspective)"), 3);
    QVERIFY(!toolSource.contains(
        "fs.addFilter(rtool->perspective->isFiltered(),"));

    QFile toolHeader(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/R/RTool.h"));
    QVERIFY2(
        toolHeader.open(QIODevice::ReadOnly),
        qPrintable(toolHeader.errorString()));
    const QByteArray toolHeaderSource = toolHeader.readAll();
    QVERIFY(toolHeaderSource.contains("QPointer<RCanvas> canvas;"));
    QVERIFY(toolHeaderSource.contains("QPointer<Perspective> perspective;"));
    QVERIFY(toolHeaderSource.contains("QPointer<RChart> chart;"));
    QVERIFY(toolHeaderSource.contains("QPointer<Context> context;"));
    QVERIFY(toolHeaderSource.contains("QPointer<Athlete> boundAthlete;"));
    QVERIFY(!toolHeaderSource.contains("RCanvas *canvas;"));
    QVERIFY(!toolHeaderSource.contains("Perspective *perspective;"));
    QVERIFY(!toolHeaderSource.contains("RChart *chart;"));
    QVERIFY(!toolHeaderSource.contains("Context *context;"));

    QFile project(QStringLiteral(GC_TEST_SOURCE_ROOT "/src/src.pro"));
    QVERIFY2(project.open(QIODevice::ReadOnly), qPrintable(project.errorString()));
    const QByteArray projectSource = project.readAll();
    QVERIFY(projectSource.contains("R/RDeferredUiWork.h"));
    QVERIFY(projectSource.contains("Charts/RConsolePromptPolicy.h"));
}

QTEST_GUILESS_MAIN(TestRExecutionGate)
#include "testRExecutionGate.moc"
