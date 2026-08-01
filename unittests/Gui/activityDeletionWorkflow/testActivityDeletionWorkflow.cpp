#include <QtTest>

#include "BatchProcessingDialog.h"

#include <array>
#include <memory>

using ActivityDeletionWorkflow::Identity;
using ActivityDeletionWorkflow::State;

class TestActivityDeletionWorkflow : public QObject
{
    Q_OBJECT

private slots:
    void exactSnapshotIsCurrent();
    void liveOwnersRunRelationshipCheck();
    void destroyedOwnerStopsRelationshipCheck_data();
    void destroyedOwnerStopsRelationshipCheck();
    void missingOwnerIsRejected_data();
    void missingOwnerIsRejected();
    void brokenRelationshipOrMembershipIsRejected_data();
    void brokenRelationshipOrMembershipIsRejected();
    void identityMutationIsRejected_data();
    void identityMutationIsRejected();
    void successfulSavedRenameCanBeAdopted();
    void savedRenameRejectsNamespaceChange();
    void savedRenameRejectsPathChange();
    void savedRenameRejectsDifferentCurrentIdentity();
    void untouchedBatchItemCannotBeAdopted();
    void invalidIdentityCannotBeAdopted_data();
    void invalidIdentityCannotBeAdopted();
};

void TestActivityDeletionWorkflow::liveOwnersRunRelationshipCheck()
{
    QObject workflow;
    QObject view;
    QObject context;
    QObject athlete;
    QObject cache;
    QPointer<QObject> guardedWorkflow(&workflow);
    QPointer<QObject> guardedView(&view);
    QPointer<QObject> guardedContext(&context);
    QPointer<QObject> guardedAthlete(&athlete);
    QPointer<QObject> guardedCache(&cache);
    int relationshipChecks = 0;

    const State state =
        ActivityDeletionWorkflow::guardedOwnerState(
            guardedWorkflow, guardedView,
            guardedContext, guardedAthlete,
            guardedCache,
            [&](QObject *, QObject *, QObject *,
                QObject *, QObject *) {
                ++relationshipChecks;
                return true;
            });

    QCOMPARE(relationshipChecks, 1);
    QVERIFY(ActivityDeletionWorkflow::ownersAreCurrent(state));
}

void TestActivityDeletionWorkflow::
destroyedOwnerStopsRelationshipCheck_data()
{
    QTest::addColumn<int>("ownerIndex");

    QTest::newRow("workflow") << 0;
    QTest::newRow("view") << 1;
    QTest::newRow("context") << 2;
    QTest::newRow("athlete") << 3;
    QTest::newRow("cache") << 4;
}

void TestActivityDeletionWorkflow::
destroyedOwnerStopsRelationshipCheck()
{
    QFETCH(int, ownerIndex);
    std::array<std::unique_ptr<QObject>, 5> owners;
    for (std::unique_ptr<QObject> &owner : owners) {
        owner = std::make_unique<QObject>();
    }
    QPointer<QObject> guardedWorkflow(owners[0].get());
    QPointer<QObject> guardedView(owners[1].get());
    QPointer<QObject> guardedContext(owners[2].get());
    QPointer<QObject> guardedAthlete(owners[3].get());
    QPointer<QObject> guardedCache(owners[4].get());
    owners[ownerIndex].reset();
    int relationshipChecks = 0;

    const State state =
        ActivityDeletionWorkflow::guardedOwnerState(
            guardedWorkflow, guardedView,
            guardedContext, guardedAthlete,
            guardedCache,
            [&](QObject *, QObject *, QObject *,
                QObject *, QObject *) {
                ++relationshipChecks;
                return true;
            });

    QCOMPARE(relationshipChecks, 0);
    QVERIFY(!ActivityDeletionWorkflow::ownersAreCurrent(state));
}

void TestActivityDeletionWorkflow::exactSnapshotIsCurrent()
{
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"),
        false};

    const State state{
        true, true, true, true, true, true,
        true, true};

    QVERIFY(ActivityDeletionWorkflow::isCurrent(
        state, identity, identity));
}

void TestActivityDeletionWorkflow::missingOwnerIsRejected_data()
{
    QTest::addColumn<bool>("workflowAvailable");
    QTest::addColumn<bool>("viewAvailable");
    QTest::addColumn<bool>("contextAvailable");
    QTest::addColumn<bool>("athleteAvailable");
    QTest::addColumn<bool>("cacheAvailable");
    QTest::addColumn<bool>("itemAvailable");

    QTest::newRow("workflow")
        << false << true << true << true << true << true;
    QTest::newRow("view")
        << true << false << true << true << true << true;
    QTest::newRow("context")
        << true << true << false << true << true << true;
    QTest::newRow("athlete")
        << true << true << true << false << true << true;
    QTest::newRow("cache")
        << true << true << true << true << false << true;
    QTest::newRow("item")
        << true << true << true << true << true << false;
}

void TestActivityDeletionWorkflow::missingOwnerIsRejected()
{
    QFETCH(bool, workflowAvailable);
    QFETCH(bool, viewAvailable);
    QFETCH(bool, contextAvailable);
    QFETCH(bool, athleteAvailable);
    QFETCH(bool, cacheAvailable);
    QFETCH(bool, itemAvailable);
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"),
        false};

    const State state{
        workflowAvailable, viewAvailable,
        contextAvailable, athleteAvailable,
        cacheAvailable, itemAvailable,
        true, true};

    QVERIFY(!ActivityDeletionWorkflow::isCurrent(
        state, identity, identity));
}

void TestActivityDeletionWorkflow::
brokenRelationshipOrMembershipIsRejected_data()
{
    QTest::addColumn<bool>("relationshipsMatch");
    QTest::addColumn<bool>("itemInCache");

    QTest::newRow("owner-relationship")
        << false << true;
    QTest::newRow("cache-membership")
        << true << false;
}

void TestActivityDeletionWorkflow::
brokenRelationshipOrMembershipIsRejected()
{
    QFETCH(bool, relationshipsMatch);
    QFETCH(bool, itemInCache);
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"),
        false};

    const State state{
        true, true, true, true, true, true,
        relationshipsMatch, itemInCache};

    QVERIFY(!ActivityDeletionWorkflow::isCurrent(
        state, identity, identity));
}

void TestActivityDeletionWorkflow::identityMutationIsRejected_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("planned");

    QTest::newRow("filename")
        << QStringLiteral("replacement.json")
        << QStringLiteral("/activities") << false;
    QTest::newRow("path")
        << QStringLiteral("activity.json")
        << QStringLiteral("/temporary") << false;
    QTest::newRow("namespace")
        << QStringLiteral("activity.json")
        << QStringLiteral("/activities") << true;
}

void TestActivityDeletionWorkflow::identityMutationIsRejected()
{
    QFETCH(QString, fileName);
    QFETCH(QString, path);
    QFETCH(bool, planned);
    const Identity expected{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"),
        false};
    const Identity current{fileName, path, planned};

    const State state{
        true, true, true, true, true, true,
        true, true};

    QVERIFY(!ActivityDeletionWorkflow::isCurrent(
        state, expected, current));
}

void TestActivityDeletionWorkflow::
successfulSavedRenameCanBeAdopted()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Identity expected{
        QStringLiteral("activity.fit"),
        directory.path(),
        false};
    const Identity current{
        QStringLiteral("2026_08_01_10_00_00.json"),
        directory.path(),
        false};
    QFile savedFile(directory.filePath(current.fileName));
    QVERIFY(savedFile.open(QIODevice::WriteOnly));
    QCOMPARE(savedFile.write("saved"), qint64(5));
    savedFile.close();
    const State state{
        true, true, true, true, true, true,
        true, true};
    const ActivityDeletionWorkflow::SavedIdentityEvidence evidence{
        true, current};

    QVERIFY(ActivityDeletionWorkflow::adoptSavedIdentity(
        state, current, evidence, directory.path(), expected));
    QVERIFY(ActivityDeletionWorkflow::isCurrent(
        state, current, expected));
}

void TestActivityDeletionWorkflow::
savedRenameRejectsNamespaceChange()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Identity expected{
        QStringLiteral("activity.fit"), directory.path(), false};
    const Identity current{
        QStringLiteral("activity.json"), directory.path(), true};
    const ActivityDeletionWorkflow::SavedIdentityEvidence evidence{
        true, current};

    QVERIFY(!ActivityDeletionWorkflow::adoptSavedIdentity(
        State{true, true, true, true, true, true, true, true},
        current, evidence, directory.path(), expected));
}

void TestActivityDeletionWorkflow::savedRenameRejectsPathChange()
{
    QTemporaryDir cacheDirectory;
    QTemporaryDir otherDirectory;
    QVERIFY(cacheDirectory.isValid());
    QVERIFY(otherDirectory.isValid());
    Identity expected{
        QStringLiteral("activity.fit"), cacheDirectory.path(), false};
    const Identity current{
        QStringLiteral("activity.json"), otherDirectory.path(), false};
    QFile savedFile(otherDirectory.filePath(current.fileName));
    QVERIFY(savedFile.open(QIODevice::WriteOnly));
    savedFile.close();
    const ActivityDeletionWorkflow::SavedIdentityEvidence evidence{
        true, current};

    QVERIFY(!ActivityDeletionWorkflow::adoptSavedIdentity(
        State{true, true, true, true, true, true, true, true},
        current, evidence, cacheDirectory.path(), expected));
}

void TestActivityDeletionWorkflow::
savedRenameRejectsDifferentCurrentIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Identity expected{
        QStringLiteral("activity.fit"), directory.path(), false};
    const Identity saved{
        QStringLiteral("activity.json"), directory.path(), false};
    const Identity current{
        QStringLiteral("different.json"), directory.path(), false};
    QFile savedFile(directory.filePath(current.fileName));
    QVERIFY(savedFile.open(QIODevice::WriteOnly));
    savedFile.close();
    const ActivityDeletionWorkflow::SavedIdentityEvidence evidence{
        true, saved};

    QVERIFY(!ActivityDeletionWorkflow::adoptSavedIdentity(
        State{true, true, true, true, true, true, true, true},
        current, evidence, directory.path(), expected));
}

void TestActivityDeletionWorkflow::
untouchedBatchItemCannotBeAdopted()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Identity expected{
        QStringLiteral("untouched.fit"), directory.path(), false};
    const Identity current{
        QStringLiteral("different.json"), directory.path(), false};
    QFile savedFile(directory.filePath(current.fileName));
    QVERIFY(savedFile.open(QIODevice::WriteOnly));
    savedFile.close();

    QVERIFY(!ActivityDeletionWorkflow::adoptSavedIdentity(
        State{true, true, true, true, true, true, true, true},
        current, ActivityDeletionWorkflow::SavedIdentityEvidence{},
        directory.path(), expected));
}

void TestActivityDeletionWorkflow::
invalidIdentityCannotBeAdopted_data()
{
    QTest::addColumn<bool>("itemAvailable");
    QTest::addColumn<bool>("itemInCache");
    QTest::addColumn<bool>("relationshipsMatch");

    QTest::newRow("item-destroyed")
        << false << true << true;
    QTest::newRow("item-detached")
        << true << false << true;
    QTest::newRow("owner-replaced")
        << true << true << false;
}

void TestActivityDeletionWorkflow::
invalidIdentityCannotBeAdopted()
{
    QFETCH(bool, itemAvailable);
    QFETCH(bool, itemInCache);
    QFETCH(bool, relationshipsMatch);
    Identity expected{
        QStringLiteral("activity.fit"),
        QStringLiteral("/activities"),
        false};
    const Identity original = expected;
    const Identity current{
        QStringLiteral("2026_08_01_10_00_00.json"),
        QStringLiteral("/activities"),
        false};
    const State state{
        true, true, true, true, true,
        itemAvailable, relationshipsMatch,
        itemInCache};

    QVERIFY(!ActivityDeletionWorkflow::adoptSavedIdentity(
        state, current,
        ActivityDeletionWorkflow::SavedIdentityEvidence{true, current},
        QStringLiteral("/activities"), expected));
    QCOMPARE(expected.fileName, original.fileName);
    QCOMPARE(expected.path, original.path);
    QCOMPARE(expected.planned, original.planned);
}

QTEST_MAIN(TestActivityDeletionWorkflow)

#include "testActivityDeletionWorkflow.moc"
