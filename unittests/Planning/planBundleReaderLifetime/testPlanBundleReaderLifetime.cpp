/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

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


QTEST_GUILESS_MAIN(TestPlanBundleReaderLifetime)

#include "testPlanBundleReaderLifetime.moc"
