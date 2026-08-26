/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <type_traits>

#define private public
#include "PlanBundle.h"
#undef private


class TestPlanBundleReaderLifetime : public QObject
{
    Q_OBJECT

private slots:
    void contextStorageTracksQObjectLifetime();
    void selectionChangeRecalculatesTargetsAndRange();
    void gapChangeRecalculatesTargetsAndRange();
    void linkedConflictUsesTargetDateTime();
    void partialActivityExportIsRejected();
    void validWorkoutReferenceIsParsed();
    void invalidWorkoutReferenceIsRejected_data();
    void invalidWorkoutReferenceIsRejected();
    void importSourceUsesSinglePlanPublication();
    void startupRecoveryCompletesBundleBeforePlanCleanup();
};


void TestPlanBundleReaderLifetime::
contextStorageTracksQObjectLifetime()
{
    using ContextStorage =
        decltype(PlanBundleReader::context);

    QVERIFY((std::is_same_v<
        ContextStorage, QPointer<Context>>));
}


void TestPlanBundleReaderLifetime::
selectionChangeRecalculatesTargetsAndRange()
{
    const QList<PlanImportScheduleActivity> activities {
        {{QDate(2026, 1, 1), QTime(8, 0)}, false},
        {{QDate(2026, 1, 5), QTime(9, 0)}, true},
        {{QDate(2026, 1, 10), QTime(10, 0)}, true}
    };

    const PlanImportSchedule schedule =
        calculatePlanImportSchedule(
            activities, QDate(2026, 2, 1),
            false, 0, 0);

    QCOMPARE(schedule.durationDays, 6);
    QCOMPARE(schedule.targetRangeEnd, QDate(2026, 2, 6));
    QCOMPARE(
        schedule.targetDateTimes,
        QList<QDateTime>({
            {QDate(2026, 1, 28), QTime(8, 0)},
            {QDate(2026, 2, 1), QTime(9, 0)},
            {QDate(2026, 2, 6), QTime(10, 0)}}));
}


void TestPlanBundleReaderLifetime::
gapChangeRecalculatesTargetsAndRange()
{
    const QList<PlanImportScheduleActivity> activities {
        {{QDate(2026, 1, 1), QTime(8, 0)}, true},
        {{QDate(2026, 1, 10), QTime(10, 0)}, true}
    };

    const PlanImportSchedule withGaps =
        calculatePlanImportSchedule(
            activities, QDate(2026, 3, 1),
            true, 2, 3);
    QCOMPARE(withGaps.durationDays, 15);
    QCOMPARE(withGaps.targetRangeEnd, QDate(2026, 3, 15));
    QCOMPARE(
        withGaps.targetDateTimes.constFirst(),
        QDateTime(QDate(2026, 3, 3), QTime(8, 0)));

    const PlanImportSchedule withoutGaps =
        calculatePlanImportSchedule(
            activities, QDate(2026, 3, 1),
            false, 2, 3);
    QCOMPARE(withoutGaps.durationDays, 10);
    QCOMPARE(withoutGaps.targetRangeEnd, QDate(2026, 3, 10));
    QCOMPARE(
        withoutGaps.targetDateTimes.constFirst(),
        QDateTime(QDate(2026, 3, 1), QTime(8, 0)));
    QCOMPARE(
        withoutGaps.targetDateTimes.constLast(),
        QDateTime(QDate(2026, 3, 10), QTime(10, 0)));
}


void TestPlanBundleReaderLifetime::
linkedConflictUsesTargetDateTime()
{
    const PlanImportScheduleActivity activity {
        {QDate(2026, 1, 1), QTime(8, 0)}, true};
    const QDateTime target(
        QDate(2026, 3, 1), QTime(8, 0));
    const QSet<QDateTime> existingLinked {target};

    QVERIFY(planImportHasLinkedConflict(
        activity, target, existingLinked));
    QVERIFY(!planImportHasLinkedConflict(
        activity, activity.sourceDateTime,
        existingLinked));
}


void TestPlanBundleReaderLifetime::
partialActivityExportIsRejected()
{
    QVERIFY(planExportCopiedAllActivities(2, 2));
    QVERIFY(!planExportCopiedAllActivities(2, 1));
    QVERIFY(!planExportCopiedAllActivities(1, 0));
    QVERIFY(!planExportCopiedAllActivities(0, 0));
}


void TestPlanBundleReaderLifetime::validWorkoutReferenceIsParsed()
{
    PlanBundleWorkoutReference reference;
    const QString hash =
        QStringLiteral("0123456789abcdef0123456789abcdef");

    QVERIFY(parsePlanBundleWorkoutReference(
        hash + QStringLiteral("-threshold.erg"),
        reference));
    QCOMPARE(reference.hash, hash);
    QCOMPARE(reference.originalFileName,
             QStringLiteral("threshold.erg"));
}


void TestPlanBundleReaderLifetime::
invalidWorkoutReferenceIsRejected_data()
{
    QTest::addColumn<QString>("fileName");
    const QString hash =
        QStringLiteral("0123456789abcdef0123456789abcdef");

    QTest::newRow("missing-name") << (hash + QStringLiteral("-"));
    QTest::newRow("short-hash")
        << QStringLiteral("01234567-workout.erg");
    QTest::newRow("non-hex-hash")
        << QStringLiteral(
            "z123456789abcdef0123456789abcdef-workout.erg");
    QTest::newRow("forward-traversal")
        << (hash + QStringLiteral("-../workout.erg"));
    QTest::newRow("backslash-traversal")
        << (hash + QStringLiteral("-..\\workout.erg"));
    QTest::newRow("dot-component")
        << (hash + QStringLiteral("-."));
    QTest::newRow("dot-dot-component")
        << (hash + QStringLiteral("-.."));
    QTest::newRow("windows-device")
        << (hash + QStringLiteral("-CON.erg"));
    QTest::newRow("trailing-dot")
        << (hash + QStringLiteral("-workout.erg."));
    QTest::newRow("forbidden-colon")
        << (hash + QStringLiteral("-workout:one.erg"));
}


void TestPlanBundleReaderLifetime::
invalidWorkoutReferenceIsRejected()
{
    QFETCH(QString, fileName);
    PlanBundleWorkoutReference reference;
    reference.hash = QStringLiteral("stale");
    reference.originalFileName = QStringLiteral("stale");

    QVERIFY(!parsePlanBundleWorkoutReference(
        fileName, reference));
    QVERIFY(reference.hash.isEmpty());
    QVERIFY(reference.originalFileName.isEmpty());
}


void TestPlanBundleReaderLifetime::
importSourceUsesSinglePlanPublication()
{
    const QString sourcePath = QDir(
        QStringLiteral(GC_TEST_SOURCE_ROOT)).filePath(
            QStringLiteral("src/Planning/PlanBundle.cpp"));
    QVERIFY2(QFileInfo::exists(sourcePath),
             "PlanBundle.cpp test data was not found");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray code = source.readAll();
    const qsizetype importStart = code.indexOf(
        "PlanBundleReader::importBundle");
    const qsizetype importEnd = code.indexOf(
        "PlanBundleReader::getTargetRangeStart",
        importStart);
    QVERIFY(importStart >= 0);
    QVERIFY(importEnd > importStart);
    const QByteArray importBody = code.mid(
        importStart, importEnd - importStart);

    QVERIFY(importBody.contains(
        "replacePlannedActivityFiles"));
    QVERIFY(!importBody.contains("removeRidesResult"));
    QVERIFY(!importBody.contains("addRide("));
    QVERIFY(!importBody.contains("cleanAndCopyActivity"));
}


void TestPlanBundleReaderLifetime::
startupRecoveryCompletesBundleBeforePlanCleanup()
{
    const QString sourcePath = QDir(
        QStringLiteral(GC_TEST_SOURCE_ROOT)).filePath(
            QStringLiteral("src/Core/RideCache.cpp"));
    QVERIFY2(QFileInfo::exists(sourcePath),
             "RideCache.cpp test data was not found");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray code = source.readAll();
    const qsizetype constructorStart = code.indexOf(
        "RideCache::RideCache");
    const qsizetype bundleRecovery = code.indexOf(
        "PlanBundle::reconcilePendingImport",
        constructorStart);
    const qsizetype planRecovery = code.indexOf(
        "PlanReplacement::Journal::reconcileAll",
        constructorStart);

    QVERIFY(constructorStart >= 0);
    QVERIFY(bundleRecovery > constructorStart);
    QVERIFY(planRecovery > bundleRecovery);
}


QTEST_GUILESS_MAIN(TestPlanBundleReaderLifetime)

#include "testPlanBundleReaderLifetime.moc"
