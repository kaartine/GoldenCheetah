#include "Cloud/OpenDataSummaryStatistics.h"

#include <QTest>
#include <QTextStream>

class TestOpenDataSummaryStatistics : public QObject
{
    Q_OBJECT

private slots:
    void incompleteInputWritesNothing();
    void completeInputWritesDistributionsAndMeanMax();
};

void TestOpenDataSummaryStatistics::incompleteInputWritesNothing()
{
    OpenDataSummaryStatistics::Statistics statistics;
    statistics.complete = false;
    QString output = QStringLiteral("unchanged");
    QTextStream stream(&output);
    stream.seek(output.size());
    QString error;

    QVERIFY(!OpenDataSummaryStatistics::append(
        statistics, stream, error));

    QCOMPARE(output, QStringLiteral("unchanged"));
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataSummaryStatistics::completeInputWritesDistributionsAndMeanMax()
{
    OpenDataSummaryStatistics::Statistics statistics;
    statistics.complete = true;
    OpenDataSummaryStatistics::Distribution power;
    power.type = QStringLiteral("power");
    power.values = {
        1, 2, 3, 4, 5, 6,
        7, 8, 9, 10, 11, 12
    };
    power.binSize = 10;
    power.divisor = 1;
    statistics.distributions.append(power);
    statistics.powerMeanMax = {
        0, 500, 450, 400, 350, 300
    };
    statistics.meanMaxDurations = {1, 2, 5, 10};
    QString output;
    QTextStream stream(&output);
    QString error;

    QVERIFY2(
        OpenDataSummaryStatistics::append(
            statistics, stream, error),
        qPrintable(error));
    stream.flush();

    QVERIFY(output.contains(
        QStringLiteral("\"power_dist\":[55, 23 ]")));
    QVERIFY(output.contains(
        QStringLiteral("\"power_dist_bins\":[0, 10 ]")));
    QVERIFY(output.contains(
        QStringLiteral("\"power_mmp\":[500, 450, 300 ]")));
    QVERIFY(output.contains(
        QStringLiteral("\"power_mmp_secs\":[1, 2, 5 ]")));
}

QTEST_GUILESS_MAIN(TestOpenDataSummaryStatistics)
#include "testOpenDataSummaryStatistics.moc"
