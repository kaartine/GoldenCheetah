#include <QtTest>

#include "MergeActivityTimeOffset.h"

#include <cmath>

class TestMergeActivityTimeOffset : public QObject
{
    Q_OBJECT

private slots:
    void intervalBoundsUseSampleDuration();
    void displayedAdjustmentUsesSampleDuration();
    void zeroAdjustmentDoesNotRenderAsNegativeZero();
    void fractionalRecordingIntervalsStayPrecise();
};

void TestMergeActivityTimeOffset::intervalBoundsUseSampleDuration()
{
    QCOMPARE(
        MergeActivityTimeOffset::shiftTimestamp(10.0, 3, 2.0),
        16.0);
    QCOMPARE(
        MergeActivityTimeOffset::shiftTimestamp(20.0, 3, 2.0),
        26.0);
}

void TestMergeActivityTimeOffset::displayedAdjustmentUsesSampleDuration()
{
    const double adjustment =
        MergeActivityTimeOffset::displayAdjustmentSeconds(3, 2.0);

    QCOMPARE(adjustment, -6.0);
    QCOMPARE(
        QStringLiteral("%1 secs").arg(adjustment),
        QStringLiteral("-6 secs"));
    QCOMPARE(
        MergeActivityTimeOffset::displayAdjustmentSeconds(-3, 2.0),
        6.0);
}

void TestMergeActivityTimeOffset::zeroAdjustmentDoesNotRenderAsNegativeZero()
{
    const double adjustment =
        MergeActivityTimeOffset::displayAdjustmentSeconds(0, 2.0);

    QVERIFY(!std::signbit(adjustment));
    QCOMPARE(
        QStringLiteral("%1 secs").arg(adjustment),
        QStringLiteral("0 secs"));
}

void TestMergeActivityTimeOffset::fractionalRecordingIntervalsStayPrecise()
{
    QCOMPARE(
        MergeActivityTimeOffset::shiftTimestamp(10.0, 3, 0.5),
        11.5);
    QCOMPARE(
        MergeActivityTimeOffset::displayAdjustmentSeconds(3, 0.5),
        -1.5);
}

QTEST_GUILESS_MAIN(TestMergeActivityTimeOffset)
#include "testMergeActivityTimeOffset.moc"
