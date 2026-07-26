#include <QtTest>

#include <climits>
#include <limits>

#include "Core/DataFilterSafety.h"

class TestDataFilterSafety : public QObject
{
    Q_OBJECT

private slots:
    void zoneArguments_data();
    void zoneArguments();
    void zoneArgumentsRejectUnknownLiterals_data();
    void zoneArgumentsRejectUnknownLiterals();
    void repeatedVectorIndexWrapsAtBoundary();
    void vectorAssignmentIndexRejectsUnsafeValues();
    void estimatePairRejectsUnmatchedInputs();
    void estimatePairMapsFields_data();
    void estimatePairMapsFields();
    void estimatePairMapsDuration();
};

void TestDataFilterSafety::zoneArguments_data()
{
    QTest::addColumn<QString>("series");
    QTest::addColumn<QString>("field");
    QTest::addColumn<QString>("expectedSeries");
    QTest::addColumn<QString>("expectedField");

    const QStringList seriesValues = {
        QStringLiteral("hr"),
        QStringLiteral("power"),
        QStringLiteral("pace"),
        QStringLiteral("fatigue")
    };
    const QStringList fieldValues = {
        QStringLiteral("name"),
        QStringLiteral("description"),
        QStringLiteral("units"),
        QStringLiteral("low"),
        QStringLiteral("high"),
        QStringLiteral("time"),
        QStringLiteral("percent")
    };

    for (const QString &series : seriesValues) {
        for (const QString &field : fieldValues) {
            const QByteArray rowName =
                QStringLiteral("%1-%2").arg(series, field).toLatin1();
            QTest::newRow(rowName.constData())
                << series << field << series << field;
        }
    }

    QTest::newRow("mixed-case")
        << QStringLiteral("PoWeR") << QStringLiteral("NaMe")
        << QStringLiteral("power") << QStringLiteral("name");
}

void TestDataFilterSafety::zoneArguments()
{
    QFETCH(QString, series);
    QFETCH(QString, field);
    QFETCH(QString, expectedSeries);
    QFETCH(QString, expectedField);

    const DataFilterSafety::ZoneArguments result =
        DataFilterSafety::zoneArguments(series, field);

    QVERIFY(result.valid);
    QCOMPARE(result.series, expectedSeries);
    QCOMPARE(result.field, expectedField);
}

void TestDataFilterSafety::zoneArgumentsRejectUnknownLiterals_data()
{
    QTest::addColumn<QString>("series");
    QTest::addColumn<QString>("field");

    QTest::newRow("unknown-series")
        << QStringLiteral("watts") << QStringLiteral("name");
    QTest::newRow("unknown-field")
        << QStringLiteral("power") << QStringLiteral("colour");
    QTest::newRow("both-unknown")
        << QStringLiteral("watts") << QStringLiteral("colour");
    QTest::newRow("empty-series")
        << QString() << QStringLiteral("name");
    QTest::newRow("empty-field")
        << QStringLiteral("power") << QString();
}

void TestDataFilterSafety::zoneArgumentsRejectUnknownLiterals()
{
    QFETCH(QString, series);
    QFETCH(QString, field);

    const DataFilterSafety::ZoneArguments result =
        DataFilterSafety::zoneArguments(series, field);

    QVERIFY(!result.valid);
    QVERIFY(result.series.isEmpty());
    QVERIFY(result.field.isEmpty());
}

void TestDataFilterSafety::repeatedVectorIndexWrapsAtBoundary()
{
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(0, 0), qsizetype(-1));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(1, 0), qsizetype(0));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(1, 1), qsizetype(0));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(1, 2), qsizetype(0));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(2, 0), qsizetype(0));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(2, 1), qsizetype(1));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(2, 2), qsizetype(0));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(2, 3), qsizetype(1));
    QCOMPARE(DataFilterSafety::repeatedVectorIndex(2, -1), qsizetype(-1));
}

void TestDataFilterSafety::vectorAssignmentIndexRejectsUnsafeValues()
{
    int index = 77;

    QVERIFY(!DataFilterSafety::vectorAssignmentIndex(-1.0, &index));
    QCOMPARE(index, 77);
    QVERIFY(!DataFilterSafety::vectorAssignmentIndex(
        std::numeric_limits<double>::quiet_NaN(), &index));
    QCOMPARE(index, 77);
    QVERIFY(!DataFilterSafety::vectorAssignmentIndex(
        std::numeric_limits<double>::infinity(), &index));
    QCOMPARE(index, 77);
    QVERIFY(!DataFilterSafety::vectorAssignmentIndex(double(INT_MAX), &index));
    QCOMPARE(index, 77);
    QVERIFY(!DataFilterSafety::vectorAssignmentIndex(0.0, nullptr));

    QVERIFY(DataFilterSafety::vectorAssignmentIndex(2.9, &index));
    QCOMPARE(index, 2);
    QVERIFY(DataFilterSafety::vectorAssignmentIndex(double(INT_MAX - 1), &index));
    QCOMPARE(index, INT_MAX - 1);
}

void TestDataFilterSafety::estimatePairRejectsUnmatchedInputs()
{
    DataFilterSafety::EstimateValues values;
    const DataFilterSafety::EstimatePair missingModel =
        DataFilterSafety::estimatePair(QStringLiteral("2p"), true, false, 321.5, values);
    QVERIFY(!missingModel.valid);
    QCOMPARE(missingModel.first, 0.0);
    QCOMPARE(missingModel.second, 0.0);

    const DataFilterSafety::EstimatePair unknown =
        DataFilterSafety::estimatePair(QStringLiteral("unknown"), false, false, 0.0, values);
    QVERIFY(!unknown.valid);
    QCOMPARE(unknown.first, 0.0);
    QCOMPARE(unknown.second, 0.0);
}

void TestDataFilterSafety::estimatePairMapsFields_data()
{
    QTest::addColumn<QString>("parameter");
    QTest::addColumn<double>("first");
    QTest::addColumn<double>("second");

    QTest::newRow("cp") << QStringLiteral("cp") << 250.0 << 250.0;
    QTest::newRow("w-prime") << QStringLiteral("w'") << 18000.0 << 18000.0;
    QTest::newRow("ftp") << QStringLiteral("ftp") << 235.0 << 235.0;
    QTest::newRow("pmax") << QStringLiteral("pmax") << 900.0 << 900.0;
    QTest::newRow("date") << QStringLiteral("date") << 45000.0 << 45006.0;
}

void TestDataFilterSafety::estimatePairMapsFields()
{
    QFETCH(QString, parameter);
    QFETCH(double, first);
    QFETCH(double, second);

    DataFilterSafety::EstimateValues values;
    values.cp = 250.0;
    values.wPrime = 18000.0;
    values.ftp = 235.0;
    values.pMax = 900.0;
    values.dateFrom = 45000.0;
    values.dateTo = 45006.0;

    const DataFilterSafety::EstimatePair result =
        DataFilterSafety::estimatePair(parameter, false, false, 0.0, values);
    QVERIFY(result.valid);
    QCOMPARE(result.first, first);
    QCOMPARE(result.second, second);
}

void TestDataFilterSafety::estimatePairMapsDuration()
{
    DataFilterSafety::EstimateValues values;
    const DataFilterSafety::EstimatePair result =
        DataFilterSafety::estimatePair(QStringLiteral("2p"), true, true, 321.5, values);

    QVERIFY(result.valid);
    QCOMPARE(result.first, 321.5);
    QCOMPARE(result.second, 321.5);
}

QTEST_APPLESS_MAIN(TestDataFilterSafety)

#include "testDataFilterSafety.moc"
