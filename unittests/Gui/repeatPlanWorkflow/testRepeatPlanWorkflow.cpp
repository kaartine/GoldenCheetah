#include <QtTest>

#include "PlanWizards.h"


class TestRepeatPlanWorkflow : public QObject
{
    Q_OBJECT

private slots:
    void ownerDestructionPreventsAcceptance_data();
    void ownerDestructionPreventsAcceptance();
    void incompleteReplacementPreventsAcceptance_data();
    void incompleteReplacementPreventsAcceptance();
    void completeReplacementCanBeAccepted();
    void emptyCopySetPreventsReplacement();
    void nonEmptyCopySetAllowsReplacement();
    void overlappingSourceAndDeletionAreAllowed();
    void replacementDisposition_data();
    void replacementDisposition();
    void destroyedContextPreventsPageAccess();
    void reentrantCompletionIsRejected();
    void importReaderLeaseSurvivesOwnerDestruction();
    void failedExportIsReportedAndWizardStaysOpen();
};


void
TestRepeatPlanWorkflow::ownerDestructionPreventsAcceptance_data
()
{
    QTest::addColumn<int>("ownerIndex");

    QTest::newRow("wizard") << 0;
    QTest::newRow("context") << 1;
    QTest::newRow("athlete") << 2;
    QTest::newRow("cache") << 3;
    QTest::newRow("tab") << 4;
}


void TestRepeatPlanWorkflow::
failedExportIsReportedAndWizardStaysOpen()
{
    int exportCalls = 0;
    int failureCalls = 0;

    const bool completed = runPlanExportCompletion(
        [&] {
            ++exportCalls;
            return false;
        },
        [&] { ++failureCalls; });

    QVERIFY(!completed);
    QCOMPARE(exportCalls, 1);
    QCOMPARE(failureCalls, 1);
}


void
TestRepeatPlanWorkflow::ownerDestructionPreventsAcceptance
()
{
    QFETCH(int, ownerIndex);

    QList<QObject*> owners {
        new QObject(), new QObject(), new QObject(),
        new QObject(), new QObject()
    };
    const RepeatPlanWorkflowGuard guard(
        owners[0], owners[1], owners[2],
        owners[3], owners[4]);
    RepeatPlanWorkflowState state;
    state.markInputsValid();
    state.markReplacementComplete();

    delete owners[ownerIndex];
    owners[ownerIndex] = nullptr;

    QVERIFY(!guard.allAlive());
    QVERIFY(!state.canAccept(guard));
    qDeleteAll(owners);
}


void
TestRepeatPlanWorkflow::incompleteReplacementPreventsAcceptance_data
()
{
    QTest::addColumn<bool>("inputsValid");
    QTest::addColumn<bool>("replacementComplete");

    QTest::newRow("source-changed") << false << false;
    QTest::newRow("replacement-failed") << true << false;
}


void
TestRepeatPlanWorkflow::incompleteReplacementPreventsAcceptance
()
{
    QFETCH(bool, inputsValid);
    QFETCH(bool, replacementComplete);

    QObject wizard;
    QObject context;
    QObject athlete;
    QObject cache;
    QObject tab;
    const RepeatPlanWorkflowGuard guard(
        &wizard, &context, &athlete, &cache, &tab);
    RepeatPlanWorkflowState state;
    if (inputsValid)
        state.markInputsValid();
    if (replacementComplete)
        state.markReplacementComplete();

    QVERIFY(!state.canAccept(guard));
}


void
TestRepeatPlanWorkflow::completeReplacementCanBeAccepted
()
{
    QObject wizard;
    QObject context;
    QObject athlete;
    QObject cache;
    QObject tab;
    const RepeatPlanWorkflowGuard guard(
        &wizard, &context, &athlete, &cache, &tab);
    RepeatPlanWorkflowState state;
    state.markInputsValid();
    state.markReplacementComplete();

    QVERIFY(guard.allAlive());
    QVERIFY(state.canAccept(guard));
}


void
TestRepeatPlanWorkflow::emptyCopySetPreventsReplacement
()
{
    RepeatPlanWorkflowState state;
    int replacementCalls = 0;

    if (state.markInputsValid(false)
        && state.canReplacePlan()) {
        ++replacementCalls;
    }

    QCOMPARE(replacementCalls, 0);
    QVERIFY(!state.canReplacePlan());
}


void
TestRepeatPlanWorkflow::nonEmptyCopySetAllowsReplacement
()
{
    RepeatPlanWorkflowState state;

    QVERIFY(state.markInputsValid(true));
    QVERIFY(state.canReplacePlan());
}


void TestRepeatPlanWorkflow::
overlappingSourceAndDeletionAreAllowed()
{
    RideItem *const sharedItem =
        reinterpret_cast<RideItem*>(quintptr(0x1));
    const QList<RideItem*> deletionItems {sharedItem};
    const QList<std::pair<RideItem*, QDate>> sourceItemsAndTargets {
        {sharedItem, QDate(2026, 8, 2)}};

    QVERIFY(repeatPlanReplacementInputsAreUsable(
        deletionItems, sourceItemsAndTargets));
}


void TestRepeatPlanWorkflow::replacementDisposition_data()
{
    QTest::addColumn<bool>("ownersAlive");
    QTest::addColumn<bool>("cleanlyCompleted");
    QTest::addColumn<int>("removedCount");
    QTest::addColumn<int>("addedCount");
    QTest::addColumn<RepeatPlanReplacementDisposition>("expected");

    QTest::newRow("owner-lost")
        << false << true << 2 << 3
        << RepeatPlanReplacementDisposition::OwnerLost;
    QTest::newRow("backend-failed")
        << true << false << 2 << 3
        << RepeatPlanReplacementDisposition::Failed;
    QTest::newRow("removal-count-mismatch")
        << true << true << 1 << 3
        << RepeatPlanReplacementDisposition::Failed;
    QTest::newRow("addition-count-mismatch")
        << true << true << 2 << 2
        << RepeatPlanReplacementDisposition::Failed;
    QTest::newRow("complete")
        << true << true << 2 << 3
        << RepeatPlanReplacementDisposition::Complete;
}


void TestRepeatPlanWorkflow::replacementDisposition()
{
    QFETCH(bool, ownersAlive);
    QFETCH(bool, cleanlyCompleted);
    QFETCH(int, removedCount);
    QFETCH(int, addedCount);
    QFETCH(RepeatPlanReplacementDisposition, expected);

    QCOMPARE(
        repeatPlanReplacementDisposition(
            ownersAlive, cleanlyCompleted,
            removedCount, 2, addedCount, 3),
        expected);
}


void
TestRepeatPlanWorkflow::destroyedContextPreventsPageAccess
()
{
    QObject wizard;
    QObject *context = new QObject;
    QObject athlete;
    QObject cache;
    QObject tab;
    const RepeatPlanWorkflowGuard guard(
        &wizard, context, &athlete, &cache, &tab);
    int pageReads = 0;
    int visibleItems = 3;

    delete context;
    context = nullptr;
    if (guard.canAccessPage())
        ++pageReads;
    else
        visibleItems = 0;

    QCOMPARE(pageReads, 0);
    QCOMPARE(visibleItems, 0);
    QVERIFY(!guard.canAccessPage());
}


void
TestRepeatPlanWorkflow::reentrantCompletionIsRejected
()
{
    QObject wizard;
    bool completionInProgress = false;

    {
        RepeatPlanWorkflowExecutionGuard first(
            &wizard, completionInProgress);
        QVERIFY(first.acquired());
        QVERIFY(completionInProgress);

        RepeatPlanWorkflowExecutionGuard reentrant(
            &wizard, completionInProgress);
        QVERIFY(!reentrant.acquired());
        QVERIFY(completionInProgress);
    }

    QVERIFY(!completionInProgress);
}


void
TestRepeatPlanWorkflow::importReaderLeaseSurvivesOwnerDestruction
()
{
    QObject *owner = new QObject;
    QSharedPointer<QObject> reader(new QObject);
    QWeakPointer<QObject> weakReader(reader);

    {
        PlanImportReaderLease<QObject> lease(owner, reader);
        QVERIFY(lease.available());
        QCOMPARE(lease.reader(), reader.data());

        reader.clear();
        delete owner;
        owner = nullptr;

        QVERIFY(!lease.ownerAlive());
        QVERIFY(!weakReader.isNull());
    }

    QVERIFY(weakReader.isNull());
}


QTEST_GUILESS_MAIN(TestRepeatPlanWorkflow)

#include "testRepeatPlanWorkflow.moc"
