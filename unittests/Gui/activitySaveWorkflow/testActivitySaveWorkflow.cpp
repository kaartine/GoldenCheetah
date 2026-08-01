#include <QtTest>

#include "SaveDialogs.h"

using ActivitySaveWorkflow::Identity;

class TestActivitySaveWorkflow : public QObject
{
    Q_OBJECT

private slots:
    void exactIdentityAndOwnersAreAccepted();
    void optionalContextCanBeAbsent();
    void detachedDialogItemIsRejected();
    void requiredOwnerOrMembershipIsRejected_data();
    void requiredOwnerOrMembershipIsRejected();
    void identityMutationIsRejected_data();
    void identityMutationIsRejected();
};

void TestActivitySaveWorkflow::
exactIdentityAndOwnersAreAccepted()
{
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"), false};

    QVERIFY(ActivitySaveWorkflow::isCurrent(
        true, true, true, true, true, true,
        true, true, true, identity, identity));
}

void TestActivitySaveWorkflow::optionalContextCanBeAbsent()
{
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"), false};

    QVERIFY(ActivitySaveWorkflow::isCurrent(
        true, false, false, false, false, false,
        true, false, false, identity, identity));
}

void TestActivitySaveWorkflow::detachedDialogItemIsRejected()
{
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"), false};

    QVERIFY(!ActivitySaveWorkflow::isCurrent(
        true, true, true, true, true, true,
        true, true, false, identity, identity));
}

void TestActivitySaveWorkflow::
requiredOwnerOrMembershipIsRejected_data()
{
    QTest::addColumn<bool>("workflowAvailable");
    QTest::addColumn<bool>("contextAvailable");
    QTest::addColumn<bool>("athleteAvailable");
    QTest::addColumn<bool>("cacheAvailable");
    QTest::addColumn<bool>("relationshipsMatch");
    QTest::addColumn<bool>("itemAvailable");
    QTest::addColumn<bool>("itemInCache");

    QTest::newRow("workflow")
        << false << true << true << true
        << true << true << true;
    QTest::newRow("context")
        << true << false << true << true
        << true << true << true;
    QTest::newRow("athlete")
        << true << true << false << true
        << true << true << true;
    QTest::newRow("cache")
        << true << true << true << false
        << true << true << true;
    QTest::newRow("relationship")
        << true << true << true << true
        << false << true << true;
    QTest::newRow("item")
        << true << true << true << true
        << true << false << true;
    QTest::newRow("membership")
        << true << true << true << true
        << true << true << false;
}

void TestActivitySaveWorkflow::
requiredOwnerOrMembershipIsRejected()
{
    QFETCH(bool, workflowAvailable);
    QFETCH(bool, contextAvailable);
    QFETCH(bool, athleteAvailable);
    QFETCH(bool, cacheAvailable);
    QFETCH(bool, relationshipsMatch);
    QFETCH(bool, itemAvailable);
    QFETCH(bool, itemInCache);
    const Identity identity{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"), false};

    QVERIFY(!ActivitySaveWorkflow::isCurrent(
        workflowAvailable, true,
        contextAvailable, athleteAvailable,
        cacheAvailable, relationshipsMatch,
        itemAvailable, true, itemInCache,
        identity, identity));
}

void TestActivitySaveWorkflow::identityMutationIsRejected_data()
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

void TestActivitySaveWorkflow::identityMutationIsRejected()
{
    QFETCH(QString, fileName);
    QFETCH(QString, path);
    QFETCH(bool, planned);
    const Identity expected{
        QStringLiteral("activity.json"),
        QStringLiteral("/activities"), false};
    const Identity current{fileName, path, planned};

    QVERIFY(!ActivitySaveWorkflow::isCurrent(
        true, false, false, false, false, false,
        true, false, false, expected, current));
}

QTEST_GUILESS_MAIN(TestActivitySaveWorkflow)

#include "testActivitySaveWorkflow.moc"
