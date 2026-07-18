#include <QtTest>

#include "MergeActivityDistanceCursor.h"

#include "RideFile.h"

class TestMergeActivityDistanceCursor : public QObject
{
    Q_OBJECT

private slots:
    void emptySourceHasNoSample();
    void selectsFirstSampleAtOrBeyondDistance();
    void exhaustedSourceHasNoSample();
    void skipsNullSourceEntries();
};

void TestMergeActivityDistanceCursor::emptySourceHasNoSample()
{
    const QVector<RideFilePoint *> points;
    MergeActivityDistance::SourceCursor cursor(points);

    QCOMPARE(cursor.atOrAfter(0.0), nullptr);
    QCOMPARE(cursor.atOrAfter(10.0), nullptr);
}

void TestMergeActivityDistanceCursor::selectsFirstSampleAtOrBeyondDistance()
{
    RideFilePoint first;
    RideFilePoint second;
    RideFilePoint third;
    first.km = 0.0;
    second.km = 1.0;
    third.km = 2.0;
    const QVector<RideFilePoint *> points = {&first, &second, &third};
    MergeActivityDistance::SourceCursor cursor(points);

    QCOMPARE(cursor.atOrAfter(-1.0), &first);
    QCOMPARE(cursor.atOrAfter(0.5), &second);
    QCOMPARE(cursor.atOrAfter(1.0), &second);
    QCOMPARE(cursor.atOrAfter(1.5), &third);
    QCOMPARE(cursor.atOrAfter(2.0), &third);
}

void TestMergeActivityDistanceCursor::exhaustedSourceHasNoSample()
{
    RideFilePoint first;
    RideFilePoint second;
    first.km = 0.0;
    second.km = 1.0;
    const QVector<RideFilePoint *> points = {&first, &second};
    MergeActivityDistance::SourceCursor cursor(points);

    QCOMPARE(cursor.atOrAfter(2.0), nullptr);
    QCOMPARE(cursor.atOrAfter(0.0), nullptr);
}

void TestMergeActivityDistanceCursor::skipsNullSourceEntries()
{
    RideFilePoint sample;
    sample.km = 1.0;
    const QVector<RideFilePoint *> points = {nullptr, &sample};
    MergeActivityDistance::SourceCursor cursor(points);

    QCOMPARE(cursor.atOrAfter(0.5), &sample);
    QCOMPARE(cursor.atOrAfter(1.1), nullptr);
}

QTEST_GUILESS_MAIN(TestMergeActivityDistanceCursor)
#include "testMergeActivityDistanceCursor.moc"
