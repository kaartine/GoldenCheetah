#include <QtTest>

#include "Measures.h"
#include "RideRefreshMeasures.h"

#include <QDir>
#include <QFile>
#include <QThread>
#include <QTimeZone>

#include <memory>

QString gcroot;

namespace {

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

constexpr quint16 MissingObservationFingerprint = 0x1357;

RideRefreshMeasures::Field field(
    const QString &symbol, double factor = 1.0)
{
    return {
        symbol,
        symbol + QStringLiteral(" name"),
        QStringLiteral("metric"),
        QStringLiteral("imperial"),
        factor,
        {symbol.toLower()}
    };
}

RideRefreshMeasures::Observation observation(
    const QDateTime &when,
    double first,
    double second = 0.0,
    const QString &comment = {})
{
    RideRefreshMeasures::Observation result;
    result.when = when;
    result.comment = comment;
    result.values[0] = first;
    result.values[1] = second;
    return result;
}

QDateTime at(const QDate &date, int hour = 12)
{
    return QDateTime(
        date, QTime(hour, 0), QTimeZone(QByteArrayLiteral("UTC")));
}

RideRefreshMeasures::Group group(
    const QString &symbol,
    QVector<RideRefreshMeasures::Observation> observations = {})
{
    return {
        symbol,
        symbol + QStringLiteral(" group"),
        {field(QStringLiteral("FIRST"), 2.0),
         field(QStringLiteral("SECOND"), 3.0)},
        std::move(observations)
    };
}

std::shared_ptr<const RideRefreshMeasures> sample()
{
    return RideRefreshMeasures::create({
        group(QStringLiteral("Body"), {
            observation(at(QDate(2024, 1, 1)), 10.0, 11.0),
            observation(at(QDate(2024, 1, 2), 8), 20.0, 21.0),
            observation(at(QDate(2024, 1, 2), 18), 22.0, 23.0),
            observation(at(QDate(2024, 1, 3)), 30.0, 31.0)}),
        group(QStringLiteral("Hrv"), {
            observation(at(QDate(2024, 1, 1)), 100.0, 101.0),
            observation(at(QDate(2024, 1, 3)), 300.0, 301.0)})
    }, MissingObservationFingerprint);
}

} // namespace

class TestRideRefreshMeasures : public QObject
{
    Q_OBJECT

private slots:
    void bodyCarriesForwardAndUsesLastSameDayObservation();
    void nonBodyGroupsRequireAnExactDate();
    void unknownGroupsAndFieldsFailClosed();
    void metadataDatesAndUnitFactorsArePreserved();
    void missingObservationUsesLegacyDefaults();
    void capturedFingerprintIsNotRecomputed();
    void valueSnapshotIsIndependentOfInputValues();
    void groupIndexAndFieldMetadataLookupsAreFailClosed();
    void createPreservesGroupAndObservationOrder();
    void productionCaptureMatchesLiveMeasuresGroup();
    void ownerThreadAssemblyPreservesAllGroupOrder();
    void athleteWrapperRejectsBeforeReadingMeasures();
};

void TestRideRefreshMeasures::
bodyCarriesForwardAndUsesLastSameDayObservation()
{
    const auto measures = sample();
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2023, 12, 31), 0).value(),
             0.0);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2024, 1, 1), 0).value(),
             10.0);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2024, 1, 2), 0).value(),
             22.0);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2024, 2, 1), 0).value(),
             30.0);
}

void TestRideRefreshMeasures::nonBodyGroupsRequireAnExactDate()
{
    const auto measures = sample();
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Hrv"), QDate(2024, 1, 1), 0).value(),
             100.0);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Hrv"), QDate(2024, 1, 2), 0).value(),
             0.0);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Hrv"), QDate(2024, 1, 4), 0).value(),
             0.0);
}

void TestRideRefreshMeasures::unknownGroupsAndFieldsFailClosed()
{
    const auto measures = sample();
    QVERIFY(!measures->fieldValue(
        QStringLiteral("Unknown"), QDate(2024, 1, 1), 0));
    QVERIFY(!measures->fieldValue(
        QStringLiteral("Body"), QDate(2024, 1, 1), -1));
    QVERIFY(!measures->fieldValue(
        QStringLiteral("Body"), QDate(2024, 1, 1), 16));
    QVERIFY(!measures->fieldValue(
        QStringLiteral("Body"), QDate(2024, 1, 1),
        QStringLiteral("MISSING")));
    QVERIFY(!measures->fieldSymbols(QStringLiteral("Unknown")));
    QVERIFY(!measures->startDate(QStringLiteral("Unknown")));
    QVERIFY(!measures->fingerprint(
        QStringLiteral("Unknown"), QDate(2024, 1, 1)));
}

void TestRideRefreshMeasures::metadataDatesAndUnitFactorsArePreserved()
{
    const auto measures = sample();
    QCOMPARE(measures->groupSymbols(),
             QStringList({QStringLiteral("Body"), QStringLiteral("Hrv")}));
    QCOMPARE(measures->fieldSymbols(QStringLiteral("Body")).value(),
             QStringList({QStringLiteral("FIRST"), QStringLiteral("SECOND")}));
    QCOMPARE(measures->startDate(QStringLiteral("Body")).value(),
             QDate(2024, 1, 1));
    QCOMPARE(measures->endDate(QStringLiteral("Body")).value(),
             QDate(2024, 1, 3));
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2024, 1, 1),
                 QStringLiteral("SECOND"), false).value(),
             33.0);

    const auto *body = measures->group(QStringLiteral("Body"));
    QVERIFY(body);
    QCOMPARE(body->name, QStringLiteral("Body group"));
    QCOMPARE(body->fields[0].name, QStringLiteral("FIRST name"));
    QCOMPARE(body->fields[0].metricUnits, QStringLiteral("metric"));
    QCOMPARE(body->fields[0].imperialUnits, QStringLiteral("imperial"));
    QCOMPARE(body->fields[0].headers,
             QStringList({QStringLiteral("first")}));
}

void TestRideRefreshMeasures::missingObservationUsesLegacyDefaults()
{
    const auto measures = sample();
    const QDate missing(2024, 1, 2);
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Hrv"), missing, 0).value(), 0.0);

    QCOMPARE(measures->fingerprint(
                 QStringLiteral("Hrv"), missing).value(),
             MissingObservationFingerprint);

    const auto emptyGroup = RideRefreshMeasures::create({
        group(QStringLiteral("Custom"))}, MissingObservationFingerprint);
    QVERIFY(emptyGroup->startDate(QStringLiteral("Custom")).value().isNull());
    QVERIFY(emptyGroup->endDate(QStringLiteral("Custom")).value().isNull());
}

void TestRideRefreshMeasures::capturedFingerprintIsNotRecomputed()
{
    RideRefreshMeasures::Observation captured;
    captured.when = at(QDate(2024, 4, 5), 7);
    captured.comment = QStringLiteral("fingerprint ä");
    captured.values[0] = 72.3456;
    captured.legacyFingerprint = 0x5a3c;
    QCOMPARE(captured.fingerprint(), quint16(0x5a3c));

    captured.when = at(QDate(2030, 1, 1));
    captured.comment.clear();
    captured.values.fill(-999.0);
    QCOMPARE(captured.fingerprint(), quint16(0x5a3c));
}

void TestRideRefreshMeasures::valueSnapshotIsIndependentOfInputValues()
{
    QVector<RideRefreshMeasures::Group> input = {
        group(QStringLiteral("Body"), {
            observation(at(QDate(2024, 1, 1)), 70.0)})};
    const auto measures = RideRefreshMeasures::create(
        input, MissingObservationFingerprint);
    input[0].fields[0].symbol = QStringLiteral("CHANGED");
    input[0].observations[0].values[0] = 90.0;

    QCOMPARE(measures->fieldSymbols(QStringLiteral("Body")).value().first(),
             QStringLiteral("FIRST"));
    QCOMPARE(measures->fieldValue(
                 QStringLiteral("Body"), QDate(2024, 1, 1), 0).value(),
             70.0);
}

void TestRideRefreshMeasures::
groupIndexAndFieldMetadataLookupsAreFailClosed()
{
    const auto measures = sample();
    QCOMPARE(measures->groupNames(),
             QStringList({QStringLiteral("Body group"),
                          QStringLiteral("Hrv group")}));
    QCOMPARE(measures->group(0)->symbol, QStringLiteral("Body"));
    QCOMPARE(measures->group(1)->symbol, QStringLiteral("Hrv"));
    QVERIFY(!measures->group(-1));
    QVERIFY(!measures->group(2));
    QCOMPARE(measures->field(QStringLiteral("Body"), 0)->symbol,
             QStringLiteral("FIRST"));
    QCOMPARE(measures->field(
                 QStringLiteral("Body"), QStringLiteral("SECOND"))->name,
             QStringLiteral("SECOND name"));
    QVERIFY(!measures->field(QStringLiteral("Missing"), 0));
    QVERIFY(!measures->field(QStringLiteral("Body"), 2));
    QCOMPARE(measures->fieldUnits(
                 QStringLiteral("Body"), 0, true).value(),
             QStringLiteral("metric"));
    QCOMPARE(measures->fieldUnits(
                 QStringLiteral("Body"), QStringLiteral("FIRST"), false).value(),
             QStringLiteral("imperial"));
    QVERIFY(!measures->fieldUnits(QStringLiteral("Body"), 2));
    QCOMPARE(measures->fieldValue(
                 0, QDate(2024, 1, 1), 0).value(), 10.0);
    QVERIFY(!measures->fieldValue(2, QDate(2024, 1, 1), 0));
    QCOMPARE(measures->startDate(0).value(), QDate(2024, 1, 1));
    QCOMPARE(measures->endDate(0).value(), QDate(2024, 1, 3));
    QVERIFY(!measures->startDate(2));
    QCOMPARE(measures->fieldSymbols(0).value().first(),
             QStringLiteral("FIRST"));
    QVERIFY(!measures->fieldSymbols(2));
}

void TestRideRefreshMeasures::createPreservesGroupAndObservationOrder()
{
    const QDateTime later = at(QDate(2024, 2, 2));
    const QDateTime earlier = at(QDate(2024, 2, 1));
    const auto measures = RideRefreshMeasures::create({
        group(QStringLiteral("Second"), {
            observation(later, 2.0), observation(earlier, 1.0)}),
        group(QStringLiteral("First"))}, MissingObservationFingerprint);

    QCOMPARE(measures->groupSymbols(),
             QStringList({QStringLiteral("Second"), QStringLiteral("First")}));
    QCOMPARE(measures->groups().first().observations[0].when, later);
    QCOMPARE(measures->groups().first().observations[1].when, earlier);
}

void TestRideRefreshMeasures::productionCaptureMatchesLiveMeasuresGroup()
{
    MeasuresGroup live(
        QStringLiteral("Body"),
        QStringLiteral("Body display"),
        {QStringLiteral("WEIGHTKG"), QStringLiteral("FATPERCENT")},
        {QStringLiteral("Weight"), QStringLiteral("Fat Percent")},
        {QStringLiteral("kg"), QStringLiteral("%")},
        {QStringLiteral("lbs"), QStringLiteral("%")},
        {2.2046226218, 1.0},
        {{QStringLiteral("weightkg")},
         {QStringLiteral("fatpercent"), QStringLiteral("fat_pct")}});

    Measure early;
    early.when = at(QDate(2024, 5, 1), 7);
    early.comment = QStringLiteral("first ä");
    early.source = Measure::Withings;
    early.originalSource = QStringLiteral("cloud");
    early.values[0] = 70.25;
    early.values[1] = 15.5;
    early.values[15] = 123.456;

    Measure late = early;
    late.when = at(QDate(2024, 5, 3), 18);
    late.comment = QStringLiteral("second");
    late.values[0] = 69.5;
    late.values[1] = 15.0;
    late.values[15] = -7.25;

    QList<Measure> observations = {late, early};
    live.setMeasures(observations);
    const RideRefreshMeasures::Group capturedGroup =
        captureRideRefreshMeasuresGroup(&live);
    const quint16 missingFingerprint = Measure().getFingerprint();
    const auto captured = RideRefreshMeasures::create(
        {capturedGroup}, missingFingerprint);

    QCOMPARE(capturedGroup.symbol, live.getSymbol());
    QCOMPARE(capturedGroup.name, live.getName());
    QCOMPARE(capturedGroup.fields.size(), live.getFieldSymbols().size());
    QCOMPARE(capturedGroup.fields[1].headers, live.getFieldHeaders(1));
    QCOMPARE(capturedGroup.observations.size(), live.measures().size());
    QCOMPARE(capturedGroup.observations[0].source,
             int(live.measures()[0].source));
    QCOMPARE(capturedGroup.observations[0].originalSource,
             live.measures()[0].originalSource);
    QCOMPARE(capturedGroup.observations[0].values[15],
             live.measures()[0].values[15]);
    QCOMPARE(capturedGroup.observations[0].fingerprint(),
             live.measures()[0].getFingerprint());

    for (const QDate date : {QDate(2024, 4, 30), QDate(2024, 5, 1),
                             QDate(2024, 5, 2), QDate(2024, 5, 3),
                             QDate(2024, 5, 4)}) {
        QCOMPARE(captured->fieldValue(
                     QStringLiteral("Body"), date, 0, true).value(),
                 live.getFieldValue(date, 0, true));
        QCOMPARE(captured->fieldValue(
                     QStringLiteral("Body"), date, 0, false).value(),
                 live.getFieldValue(date, 0, false));

        Measure selected;
        live.getMeasure(date, selected);
        QCOMPARE(captured->fingerprint(
                     QStringLiteral("Body"), date).value(),
                 selected.getFingerprint());
    }
}

void TestRideRefreshMeasures::ownerThreadAssemblyPreservesAllGroupOrder()
{
    QObject owner;
    MeasuresGroup body(
        QStringLiteral("Body"), QStringLiteral("Body display"),
        {QStringLiteral("WEIGHTKG")}, {QStringLiteral("Weight")},
        {QStringLiteral("kg")}, {QStringLiteral("lbs")}, {2.2},
        {{QStringLiteral("weightkg")}});
    MeasuresGroup custom(
        QStringLiteral("Custom"), QStringLiteral("Custom display"),
        {QStringLiteral("VALUE")}, {QStringLiteral("Value")},
        {QStringLiteral("m")}, {QStringLiteral("ft")}, {3.0},
        {{QStringLiteral("value")}});

    const auto captured = captureRideRefreshMeasuresForOwner(
        &owner, {&custom, &body}, MissingObservationFingerprint);
    QVERIFY(captured);
    QCOMPARE(captured->groupSymbols(),
             QStringList({QStringLiteral("Custom"), QStringLiteral("Body")}));
    QCOMPARE(captured->groupNames(),
             QStringList({QStringLiteral("Custom display"),
                          QStringLiteral("Body display")}));
    QCOMPARE(captured->fingerprint(
                 QStringLiteral("Custom"), QDate(2025, 1, 1)).value(),
             MissingObservationFingerprint);
    QVERIFY(!captureRideRefreshMeasuresForOwner(
        nullptr, {&body}, MissingObservationFingerprint));

    QThread foreignThread;
    QObject foreignOwner;
    foreignOwner.moveToThread(&foreignThread);
    foreignThread.start();
    QVERIFY(!captureRideRefreshMeasuresForOwner(
        &foreignOwner, {&body}, MissingObservationFingerprint));
    QThread *testThread = QThread::currentThread();
    QMetaObject::invokeMethod(
        &foreignOwner,
        [&foreignOwner, testThread]() {
            foreignOwner.moveToThread(testThread);
        },
        Qt::BlockingQueuedConnection);
    foreignThread.quit();
    QVERIFY(foreignThread.wait());
}

void TestRideRefreshMeasures::athleteWrapperRejectsBeforeReadingMeasures()
{
    QFile file(QDir(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)))
                   .filePath(QStringLiteral(
                       "src/Core/RideRefreshMeasuresAthleteCapture.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray source = file.readAll();
    const qsizetype function = source.indexOf("captureRideRefreshMeasures(");
    const qsizetype ownerCheck = source.indexOf(
        "QThread::currentThread() != athlete->thread()", function);
    const qsizetype nullMeasuresCheck = source.indexOf(
        "!athlete->measures", function);
    const qsizetype firstMeasuresRead = source.indexOf(
        "athlete->measures->getGroups()", function);

    QVERIFY(function >= 0);
    QVERIFY(ownerCheck > function);
    QVERIFY(nullMeasuresCheck > function);
    QVERIFY(firstMeasuresRead > ownerCheck);
    QVERIFY(firstMeasuresRead > nullMeasuresCheck);
}

QTEST_MAIN(TestRideRefreshMeasures)

#include "testRideRefreshMeasures.moc"
