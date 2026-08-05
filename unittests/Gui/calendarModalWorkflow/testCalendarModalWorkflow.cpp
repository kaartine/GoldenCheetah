#include <QtTest>

#include "CalendarSeasonWorkflow.h"
#include "ModalWorkflowGuard.h"


class FakeTab : public QObject
{
public:
    bool noSwitch() const { return value; }
    void setNoSwitch(bool next) { value = next; }

private:
    bool value = false;
};


class FakeMainWindow : public QObject
{
public:
    FakeTab *athleteTab() const { return activeTab; }

    FakeTab *activeTab = nullptr;
};


class FakeContext : public QObject
{
public:
    FakeMainWindow *mainWindow = nullptr;
    FakeTab *tab = nullptr;
};


class FakePointerOwner : public QObject
{
public:
    void *value = nullptr;
};


class MutationEmitter : public QObject
{
    Q_OBJECT

signals:
    void changed();
};


class TestCalendarModalWorkflow : public QObject
{
    Q_OBJECT

private slots:
    void ownerDestructionPreventsCommit_data();
    void ownerDestructionPreventsCommit();
    void ownerDestructionRejectsDialog();
    void ownerLossBeforeHookRejectsNestedDialog();
    void ownerDestructionEndsNestedDialog();
    void topologyReplacementPreventsCommit();
    void liveOwnerAllowsIdentityAdoption();
    void activeTabReplacementPreventsCommit();
    void exactPairRejectsSubstitution();
    void affectedOwnerSetMustRemainExact();
    void destroyedAffectedOwnerRetainsSurvivors();
    void ownerLossPreventsPostDialogContinuation();
    void mutationRejectsDialog();
    void noSwitchLeaseRestoresExactPriorValue_data();
    void noSwitchLeaseRestoresExactPriorValue();
    void noSwitchLeaseLeavesReplacementTabUntouched();
    void noSwitchLeaseToleratesDestroyedOriginalTab();
    void pointerOverrideRestoresOnlyOwnedState();
    void pointerOverrideDoesNotRestoreDetachedOwnerState();
    void pointerOverrideDoesNotRestoreExpiredOriginal();
    void committedCompletionCannotBecomeRetryable();
    void unchangedSeasonCanBeResolved();
    void currentSeasonIdentityIsRequired();
    void seasonMutationPreventsResolution_data();
    void seasonMutationPreventsResolution();
    void duplicateSeasonIdentityPreventsResolution();
    void duplicateRecordIdentityIsAmbiguous();
    void seasonEventsHaveStableUniqueIdentity();
};


void TestCalendarModalWorkflow::
ownerDestructionPreventsCommit_data()
{
    QTest::addColumn<int>("ownerIndex");

    QTest::newRow("window") << 0;
    QTest::newRow("context") << 1;
    QTest::newRow("athlete") << 2;
    QTest::newRow("cache") << 3;
    QTest::newRow("tab") << 4;
}


void TestCalendarModalWorkflow::
ownerDestructionPreventsCommit()
{
    QFETCH(int, ownerIndex);

    QList<QObject*> owners {
        new QObject, new QObject, new QObject,
        new QObject, new QObject
    };
    const ModalWorkflowGuard guard(owners);

    delete owners[ownerIndex];
    owners[ownerIndex] = nullptr;

    QVERIFY(!guard.canCommit());
    qDeleteAll(owners);
}


void TestCalendarModalWorkflow::
ownerDestructionRejectsDialog()
{
    QObject *owner = new QObject;
    QDialog dialog;
    dialog.setResult(QDialog::Accepted);
    const ModalWorkflowGuard guard({owner});
    guard.rejectOnOwnerLoss(&dialog);
    dialog.show();
    QVERIFY(dialog.isVisible());

    delete owner;

    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(!guard.canCommit());
}


void TestCalendarModalWorkflow::
ownerLossBeforeHookRejectsNestedDialog()
{
    QObject *owner = new QObject;
    const ModalWorkflowGuard guard({owner});
    delete owner;
    owner = nullptr;

    QDialog dialog;
    guard.rejectOnOwnerLoss(&dialog);
    QTimer::singleShot(1000, &dialog, &QDialog::accept);

    QCOMPARE(dialog.exec(), int(QDialog::Rejected));
    QVERIFY(!guard.canCommit());
}


void TestCalendarModalWorkflow::
ownerDestructionEndsNestedDialog()
{
    QObject *owner = new QObject;
    QDialog dialog;
    const ModalWorkflowGuard guard({owner});
    guard.rejectOnOwnerLoss(&dialog);

    QTimer::singleShot(0, &dialog, [&] {
        delete owner;
        owner = nullptr;
    });
    QTimer::singleShot(1000, &dialog, &QDialog::accept);

    QCOMPARE(dialog.exec(), int(QDialog::Rejected));
    QVERIFY(owner == nullptr);
    QVERIFY(!guard.canCommit());
}


void TestCalendarModalWorkflow::
topologyReplacementPreventsCommit()
{
    QObject context;
    QObject originalAthlete;
    QObject replacementAthlete;
    QObject *currentAthlete = &originalAthlete;
    const ModalWorkflowGuard guard(
        {&context, &originalAthlete},
        [&] { return currentAthlete == &originalAthlete; });

    QVERIFY(guard.canCommit());
    currentAthlete = &replacementAthlete;
    QVERIFY(!guard.canCommit());
}


void TestCalendarModalWorkflow::
liveOwnerAllowsIdentityAdoption()
{
    QObject owner;
    bool identityMatches = false;
    const ModalWorkflowGuard guard(
        {&owner}, [&] { return identityMatches; });

    QVERIFY(guard.ownersAlive());
    QVERIFY(!guard.canCommit());
    identityMatches = true;
    QVERIFY(guard.canCommit());
}


void TestCalendarModalWorkflow::
activeTabReplacementPreventsCommit()
{
    FakeTab original;
    FakeTab replacement;
    FakeMainWindow mainWindow;
    FakeContext context;
    mainWindow.activeTab = &original;
    context.mainWindow = &mainWindow;
    context.tab = &original;

    QVERIFY(modalWorkflowHasActiveTab(
        &mainWindow, &context, &original));

    mainWindow.activeTab = &replacement;
    QVERIFY(!modalWorkflowHasActiveTab(
        &mainWindow, &context, &original));
}


void TestCalendarModalWorkflow::
exactPairRejectsSubstitution()
{
    QObject first;
    QObject second;
    QObject replacement;

    QVERIFY(modalWorkflowHasExactPair(
        QList<QObject*>{&first, &second},
        &first, &second));
    QVERIFY(modalWorkflowHasExactPair(
        QList<QObject*>{&second, &first},
        &first, &second));
    QVERIFY(!modalWorkflowHasExactPair(
        QList<QObject*>{&first, &replacement},
        &first, &second));
    QVERIFY(!modalWorkflowHasExactPair(
        QList<QObject*>{&first, &second, &replacement},
        &first, &second));
    QVERIFY(!modalWorkflowHasExactPair(
        QList<QObject*>{&first, &first},
        &first, &second));
    QVERIFY(!modalWorkflowHasExactPair(
        QList<QObject*>{&first, &second},
        &first, &first));
}


void TestCalendarModalWorkflow::
affectedOwnerSetMustRemainExact()
{
    QObject first;
    QObject second;
    QObject replacement;
    const ModalOwnerSetSnapshot<QObject> snapshot(
        {&first, &second});

    QVERIFY(snapshot.matchesExactly({&second, &first}));
    QVERIFY(!snapshot.matchesExactly({&first, &replacement}));
    QVERIFY(!snapshot.matchesExactly(
        {&first, &second, &replacement}));
    QVERIFY(!snapshot.matchesExactly({&first, &first}));

    QObject *destroyed = new QObject;
    const ModalOwnerSetSnapshot<QObject> destroyedSnapshot(
        {&first, destroyed});
    delete destroyed;
    QVERIFY(!destroyedSnapshot.matchesExactly({&first}));
}


void TestCalendarModalWorkflow::
destroyedAffectedOwnerRetainsSurvivors()
{
    QObject first;
    QObject *second = new QObject;
    const QList<QPointer<QObject>> affected {
        QPointer<QObject>(&first), QPointer<QObject>(second)};
    delete second;
    second = nullptr;

    QList<QObject*> live;
    QVERIFY(!modalWorkflowCollectLiveOwners(affected, live));
    QCOMPARE(live, QList<QObject*>({&first}));
}


void TestCalendarModalWorkflow::
ownerLossPreventsPostDialogContinuation()
{
    QObject *owner = new QObject;
    const ModalWorkflowGuard guard({owner});
    bool called = false;
    delete owner;
    owner = nullptr;

    QVERIFY(!modalWorkflowRunContinuation(
        guard, [&] { called = true; }));
    QVERIFY(!called);
}


void TestCalendarModalWorkflow::
mutationRejectsDialog()
{
    MutationEmitter emitter;
    QDialog dialog;
    dialog.setResult(QDialog::Accepted);
    modalWorkflowRejectOnMutation(
        &emitter, &MutationEmitter::changed, &dialog);
    dialog.show();

    emit emitter.changed();

    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}


void TestCalendarModalWorkflow::
noSwitchLeaseRestoresExactPriorValue_data()
{
    QTest::addColumn<bool>("priorValue");

    QTest::newRow("unlocked") << false;
    QTest::newRow("outer-guard") << true;
}


void TestCalendarModalWorkflow::
noSwitchLeaseRestoresExactPriorValue()
{
    QFETCH(bool, priorValue);
    FakeTab tab;
    tab.setNoSwitch(priorValue);

    {
        ModalNoSwitchLease<FakeTab> lease(&tab);
        QVERIFY(tab.noSwitch());
    }

    QCOMPARE(tab.noSwitch(), priorValue);
}


void TestCalendarModalWorkflow::
noSwitchLeaseLeavesReplacementTabUntouched()
{
    FakeTab original;
    FakeTab replacement;
    replacement.setNoSwitch(true);

    {
        ModalNoSwitchLease<FakeTab> lease(&original);
        QVERIFY(original.noSwitch());
    }

    QVERIFY(!original.noSwitch());
    QVERIFY(replacement.noSwitch());
}


void TestCalendarModalWorkflow::
noSwitchLeaseToleratesDestroyedOriginalTab()
{
    FakeTab *original = new FakeTab;

    {
        ModalNoSwitchLease<FakeTab> lease(original);
        delete original;
        original = nullptr;
    }

    QVERIFY(original == nullptr);
}


void TestCalendarModalWorkflow::
pointerOverrideRestoresOnlyOwnedState()
{
    int originalValue = 1;
    int temporaryValue = 2;
    int externalValue = 3;
    FakePointerOwner owner;
    owner.value = &originalValue;

    {
        ModalPointerOverrideLease<FakePointerOwner, void*> lease(
            &owner, &FakePointerOwner::value, &temporaryValue);
        QCOMPARE(owner.value, static_cast<void*>(&temporaryValue));
        owner.value = &externalValue;
    }

    QCOMPARE(owner.value, static_cast<void*>(&externalValue));
}


void TestCalendarModalWorkflow::
pointerOverrideDoesNotRestoreDetachedOwnerState()
{
    int originalValue = 1;
    int temporaryValue = 2;
    FakePointerOwner owner;
    owner.value = &originalValue;
    bool ownerCurrent = true;

    {
        ModalPointerOverrideLease<FakePointerOwner, void*> lease(
            &owner, &FakePointerOwner::value, &temporaryValue,
            [&] { return ownerCurrent; });
        ownerCurrent = false;
    }

    QVERIFY(owner.value == nullptr);
}


void TestCalendarModalWorkflow::
pointerOverrideDoesNotRestoreExpiredOriginal()
{
    int originalValue = 1;
    int temporaryValue = 2;
    FakePointerOwner owner;
    owner.value = &originalValue;
    bool originalAlive = true;

    {
        ModalPointerOverrideLease<FakePointerOwner, void*> lease(
            &owner, &FakePointerOwner::value, &temporaryValue,
            {}, [&] { return originalAlive; });
        originalAlive = false;
    }

    QVERIFY(owner.value == nullptr);
}


void TestCalendarModalWorkflow::
committedCompletionCannotBecomeRetryable()
{
    QVERIFY(modalWorkflowCanAdvanceAfterCommit(true, false));
    QVERIFY(modalWorkflowCanAdvanceAfterCommit(false, true));
    QVERIFY(!modalWorkflowCanAdvanceAfterCommit(false, false));
}


namespace {

Season sampleSeason()
{
    Season season;
    season.setName(QStringLiteral("2026"));
    season.setAbsoluteStart(QDate(2026, 1, 1));
    season.setAbsoluteEnd(QDate(2026, 12, 31));
    season.events.append(SeasonEvent(
        QStringLiteral("Race"), QDate(2026, 8, 9), 2,
        QStringLiteral("A race")));
    season.phases.append(Phase(
        QStringLiteral("Build"),
        QDate(2026, 7, 1), QDate(2026, 8, 1)));
    return season;
}

} // namespace


void TestCalendarModalWorkflow::
unchangedSeasonCanBeResolved()
{
    QList<Season> seasons {sampleSeason()};
    const CalendarSeasonSnapshot snapshot(seasons.constFirst());

    QCOMPARE(snapshot.resolveUnchanged(seasons), &seasons.first());
}


void TestCalendarModalWorkflow::
currentSeasonIdentityIsRequired()
{
    QList<Season> seasons {sampleSeason()};
    const CalendarSeasonSnapshot snapshot(seasons.constFirst());
    Season other = sampleSeason();

    QVERIFY(snapshot.matchesCurrent(&seasons.first()));
    QVERIFY(!snapshot.matchesCurrent(&other));
    QVERIFY(!snapshot.matchesCurrent(nullptr));
}


void TestCalendarModalWorkflow::
seasonMutationPreventsResolution_data()
{
    QTest::addColumn<int>("mutation");

    QTest::newRow("season-field") << 0;
    QTest::newRow("event-field") << 1;
    QTest::newRow("event-added") << 2;
    QTest::newRow("phase-field") << 3;
    QTest::newRow("phase-added") << 4;
    QTest::newRow("load-profile") << 5;
}


void TestCalendarModalWorkflow::
seasonMutationPreventsResolution()
{
    QFETCH(int, mutation);
    QList<Season> seasons {sampleSeason()};
    const CalendarSeasonSnapshot snapshot(seasons.constFirst());

    switch (mutation) {
    case 0:
        seasons[0].setName(QStringLiteral("Changed"));
        break;
    case 1:
        seasons[0].events[0].description = QStringLiteral("Changed");
        break;
    case 2:
        seasons[0].events.append(SeasonEvent(
            QStringLiteral("Added"), QDate(2026, 9, 1)));
        break;
    case 3:
        seasons[0].phases[0].setLow(-70);
        break;
    case 4:
        seasons[0].phases.append(Phase(
            QStringLiteral("Peak"),
            QDate(2026, 8, 2), QDate(2026, 8, 8)));
        break;
    case 5:
        seasons[0].load().append(42);
        break;
    default:
        QFAIL("Unknown mutation");
    }

    QVERIFY(snapshot.resolveUnchanged(seasons) == nullptr);
}


void TestCalendarModalWorkflow::
duplicateSeasonIdentityPreventsResolution()
{
    Season original = sampleSeason();
    Season duplicate = original;
    QList<Season> seasons {original, duplicate};
    const CalendarSeasonSnapshot snapshot(original);

    QVERIFY(snapshot.resolveUnchanged(seasons) == nullptr);
}


void TestCalendarModalWorkflow::
duplicateRecordIdentityIsAmbiguous()
{
    Season season = sampleSeason();
    const QString eventId = season.events.first().id;
    const QUuid phaseId = season.phases.first().id();

    QCOMPARE(calendarUniqueSeasonEventIndex(season, eventId), 0);
    QCOMPARE(calendarUniqueSeasonPhaseIndex(season, phaseId), 0);

    season.events.append(season.events.first());
    season.phases.append(season.phases.first());

    QCOMPARE(calendarUniqueSeasonEventIndex(season, eventId),
             CalendarAmbiguousRecordIndex);
    QCOMPARE(calendarUniqueSeasonPhaseIndex(season, phaseId),
             CalendarAmbiguousRecordIndex);
    QCOMPARE(calendarUniqueSeasonEventIndex(season, QString()), -1);
    QCOMPARE(calendarUniqueSeasonPhaseIndex(season, QUuid()), -1);
}


void TestCalendarModalWorkflow::
seasonEventsHaveStableUniqueIdentity()
{
    const SeasonEvent first(
        QStringLiteral("First"), QDate(2026, 8, 1));
    const SeasonEvent second(
        QStringLiteral("Second"), QDate(2026, 8, 2));
    const QString supplied = QStringLiteral("external-id");
    const SeasonEvent imported(
        QStringLiteral("Imported"), QDate(2026, 8, 3),
        0, QString(), supplied);

    QVERIFY(!first.id.isEmpty());
    QVERIFY(!second.id.isEmpty());
    QVERIFY(first.id != second.id);
    QCOMPARE(imported.id, supplied);
    QCOMPARE(SeasonEvent(first).id, first.id);
}


QTEST_MAIN(TestCalendarModalWorkflow)

#include "testCalendarModalWorkflow.moc"
