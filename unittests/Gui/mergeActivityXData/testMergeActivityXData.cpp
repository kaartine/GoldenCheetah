#include <QtTest>

#include "MergeActivityXData.h"
#include "RideFile.h"

#include <limits>
#include <memory>

namespace {

void appendPoint(
    XDataSeries &series,
    double seconds,
    double distance,
    double value,
    const QString &text)
{
    XDataPoint *point = new XDataPoint;
    point->secs = seconds;
    point->km = distance;
    point->number[0] = value;
    point->string[0] = text;
    series.datapoints.append(point);
}

XDataSeries makeSeries()
{
    XDataSeries series;
    series.name = QStringLiteral("UNIQUE");
    series.valuename << QStringLiteral("value");
    series.unitname << QStringLiteral("unit");
    series.valuetype << RideFile::none;
    return series;
}

} // namespace

class TestMergeActivityXData : public QObject
{
    Q_OBJECT

private slots:
    void shiftsBySampleDurationAndPreservesPayload();
    void clipsToInclusiveMergedTimeline();
    void emptyTimelineProducesEmptySeries();
    void skipsNullAndNonFinitePoints();
};

void TestMergeActivityXData::shiftsBySampleDurationAndPreservesPayload()
{
    XDataSeries source = makeSeries();
    appendPoint(source, 4.0, 1.25, 123.0, QStringLiteral("payload"));

    std::unique_ptr<XDataSeries> shifted =
        MergeActivityXData::shiftedCopy(source, 3, 2.0, 0.0, 20.0);

    QCOMPARE(shifted->name, source.name);
    QCOMPARE(shifted->valuename, source.valuename);
    QCOMPARE(shifted->unitname, source.unitname);
    QCOMPARE(shifted->valuetype, source.valuetype);
    QCOMPARE(shifted->datapoints.size(), 1);
    QVERIFY(shifted->datapoints.first() != source.datapoints.first());
    QCOMPARE(shifted->datapoints.first()->secs, 10.0);
    QCOMPARE(shifted->datapoints.first()->km, 1.25);
    QCOMPARE(shifted->datapoints.first()->number[0], 123.0);
    QCOMPARE(
        shifted->datapoints.first()->string[0],
        QStringLiteral("payload"));
    QCOMPARE(source.datapoints.first()->secs, 4.0);
}

void TestMergeActivityXData::clipsToInclusiveMergedTimeline()
{
    XDataSeries source = makeSeries();
    appendPoint(source, -7.0, 0.0, 1.0, QStringLiteral("before"));
    appendPoint(source, -6.0, 0.0, 2.0, QStringLiteral("start"));
    appendPoint(source, 14.0, 0.0, 3.0, QStringLiteral("stop"));
    appendPoint(source, 14.5, 0.0, 4.0, QStringLiteral("after"));

    std::unique_ptr<XDataSeries> shifted =
        MergeActivityXData::shiftedCopy(source, 3, 2.0, 0.0, 20.0);

    QCOMPARE(shifted->datapoints.size(), 2);
    QCOMPARE(shifted->datapoints.at(0)->secs, 0.0);
    QCOMPARE(
        shifted->datapoints.at(0)->string[0],
        QStringLiteral("start"));
    QCOMPARE(shifted->datapoints.at(1)->secs, 20.0);
    QCOMPARE(
        shifted->datapoints.at(1)->string[0],
        QStringLiteral("stop"));
}

void TestMergeActivityXData::emptyTimelineProducesEmptySeries()
{
    XDataSeries source = makeSeries();
    appendPoint(source, 0.0, 0.0, 1.0, QStringLiteral("point"));

    std::unique_ptr<XDataSeries> shifted =
        MergeActivityXData::shiftedCopy(source, 0, 1.0, 0.0, -1.0);

    QCOMPARE(shifted->name, source.name);
    QVERIFY(shifted->datapoints.isEmpty());
}

void TestMergeActivityXData::skipsNullAndNonFinitePoints()
{
    XDataSeries source = makeSeries();
    source.datapoints.append(nullptr);
    appendPoint(
        source,
        std::numeric_limits<double>::quiet_NaN(),
        0.0,
        1.0,
        QStringLiteral("invalid"));
    appendPoint(source, 5.0, 0.0, 2.0, QStringLiteral("valid"));

    std::unique_ptr<XDataSeries> shifted =
        MergeActivityXData::shiftedCopy(source, 0, 1.0, 0.0, 10.0);

    QCOMPARE(shifted->datapoints.size(), 1);
    QCOMPARE(shifted->datapoints.first()->secs, 5.0);
    QCOMPARE(
        shifted->datapoints.first()->string[0],
        QStringLiteral("valid"));
}

QTEST_GUILESS_MAIN(TestMergeActivityXData)
#include "testMergeActivityXData.moc"
