#include <QtTest>

#include "RideRefreshCacheInputs.h"
#include "RideRefreshZones.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "Zones.h"

#include <QThread>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>

#include <cmath>
#include <limits>

namespace {

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

RideRefreshZones::PowerRange powerRange(
    QDate begin, QDate end, int cp = 300, int aet = 0)
{
    return {begin, end, cp, aet, 285, 20000, 1100,
            {{QStringLiteral("Z1"), QStringLiteral("Easy"), 0, 200, 0.0},
             {QStringLiteral("Z2"), QStringLiteral("Hard"), 200, 1000, 0.0}}};
}

std::shared_ptr<const RideRefreshZones> snapshot(
    RideRefreshZones::PowerHistory bike = {},
    RideRefreshZones::PowerHistory runPower = {})
{
    QHash<QString, RideRefreshZones::PowerHistory> power;
    if (!bike.sport.isNull()) power.insert(QStringLiteral("Bike"), bike);
    if (!runPower.sport.isNull()) power.insert(QStringLiteral("Run"), runPower);
    return RideRefreshZones::create(
        power, {}, RideRefreshZones::PaceHistory{},
        RideRefreshZones::PaceHistory{});
}

QByteArray legacyAnalysisFingerprint(
    const Zones *power,
    const HrZones *heartRate,
    const PaceZones *pace,
    const RideRefreshCacheSettings &settings,
    const QDate &date,
    const QString &sport,
    bool isSwim,
    double weight)
{
    QByteArray canonical;
    QDataStream stream(&canonical, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(1)
           << qint64(date.toJulianDay())
           << sport
           << isSwim
           << weight;

    const int powerRange = power ? power->whichRange(date) : -1;
    stream << bool(power) << qint32(powerRange);
    if (powerRange >= 0) {
        stream << qint32(power->getCP(powerRange))
               << qint32(power->getAeT(powerRange))
               << qint32(power->getFTP(powerRange))
               << qint32(power->getWprime(powerRange))
               << qint32(power->getPmax(powerRange))
               << power->getZoneLows(powerRange)
               << power->getZoneHighs(powerRange);
    }

    const int heartRateRange = heartRate
        ? heartRate->whichRange(date) : -1;
    stream << bool(heartRate) << qint32(heartRateRange);
    if (heartRateRange >= 0) {
        stream << qint32(heartRate->getLT(heartRateRange))
               << qint32(heartRate->getAeT(heartRateRange))
               << qint32(heartRate->getRestHr(heartRateRange))
               << qint32(heartRate->getMaxHr(heartRateRange))
               << heartRate->getZoneLows(heartRateRange)
               << heartRate->getZoneHighs(heartRateRange);
    }

    const int paceRange = pace ? pace->whichRange(date) : -1;
    stream << bool(pace) << qint32(paceRange);
    if (paceRange >= 0) {
        stream << pace->getCV(paceRange)
               << pace->getAeT(paceRange)
               << pace->getZoneLows(paceRange)
               << pace->getZoneHighs(paceRange);
    }
    stream << settings.wbalFormula
           << qint32(settings.wbalTau)
           << qint32(settings.wheelSize);
    return QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256);
}

} // namespace

class TestRideRefreshZones : public QObject
{
    Q_OBJECT

private slots:
    void rangeSelectionPreservesHalfOpenFirstMatchSemantics();
    void bikeFallbackIsExactAndAbsenceIsRepresented();
    void zoneClassificationPreservesBoundariesAndSpecialValues();
    void rawAndResolvedThresholdsStayDistinct();
    void fingerprintsPreserveLegacyInputsAndQuantization();
    void hrValuesDescriptionsAndTrimpsAreRetained();
    void paceValuesFormattingAndUnitsAreGenerationValues();
    void createNormalizesRunAndSwimIdentity();
    void cacheAnalysisFingerprintPreservesLegacyCanonicalInputs();
    void cacheInputsDistinguishMissingSnapshotAndAbsentDomains();
    void distributionInputsOwnSelectedImmutableRanges();
    void productionCaptureMatchesLivePowerZones();
    void productionCaptureMatchesLiveHeartRateZones();
    void productionCaptureMatchesLivePaceZones();
    void ownerThreadAssemblyUsesCapturedSettingsAndPreservesNulls();
    void athleteWrapperChecksOwnerBeforeReadingState();
};

void TestRideRefreshZones::
rangeSelectionPreservesHalfOpenFirstMatchSemantics()
{
    RideRefreshZones::PowerHistory history;
    history.ranges = {
        powerRange({}, QDate(2024, 2, 1), 100),
        powerRange(QDate(2024, 1, 15), QDate(2024, 3, 1), 200),
        powerRange(QDate(2024, 4, 1), {}, 300)};

    QCOMPARE(history.whichRange(QDate(2020, 1, 1)), 0);
    QCOMPARE(history.whichRange(QDate(2024, 1, 15)), 0); // first match
    QCOMPARE(history.whichRange(QDate(2024, 2, 1)), 1); // end excluded
    QCOMPARE(history.whichRange(QDate(2024, 3, 1)), -1);
    QCOMPARE(history.whichRange(QDate(2024, 4, 1)), 2);
    QCOMPARE(history.rangeForDate(QDate(2024, 2, 1))->cp, 200);
}

void TestRideRefreshZones::bikeFallbackIsExactAndAbsenceIsRepresented()
{
    RideRefreshZones::PowerHistory bike;
    bike.present = true;
    bike.sport = QStringLiteral("Bike");
    bike.ranges = {powerRange({}, {}, 250)};
    RideRefreshZones::PowerHistory run;
    run.present = true;
    run.sport = QStringLiteral("Run");
    run.ranges = {powerRange({}, {}, 310)};
    auto zones = snapshot(bike, run);

    QCOMPARE(zones->power(QStringLiteral("Run"))->ranges.first().cp, 310);
    QCOMPARE(zones->power(QStringLiteral("run"))->ranges.first().cp, 250);
    QCOMPARE(zones->power(QStringLiteral("Other"))->sport,
             QStringLiteral("Bike"));
    QVERIFY(!snapshot()->power(QStringLiteral("Other")));

    QHash<QString, RideRefreshZones::PowerHistory> exactNull;
    exactNull.insert(QStringLiteral("Bike"), bike);
    exactNull.insert(QStringLiteral("Run"), {});
    auto withExactNull = RideRefreshZones::create(
        exactNull, {}, {}, {});
    QVERIFY(!withExactNull->power(QStringLiteral("Run")));
    QCOMPARE(withExactNull->power(QStringLiteral("Other"))->sport,
             QStringLiteral("Bike"));
}

void TestRideRefreshZones::
zoneClassificationPreservesBoundariesAndSpecialValues()
{
    const auto range = powerRange({}, {});
    QCOMPARE(range.whichZone(-1.0), -1);
    QCOMPARE(range.whichZone(0.0), 0);
    QCOMPARE(range.whichZone(199.999), 0);
    QCOMPARE(range.whichZone(200.0), 1);
    QCOMPARE(range.whichZone(999.999), 1);
    QCOMPARE(range.whichZone(1000.0), -1);
    QCOMPARE(range.whichZone(std::numeric_limits<double>::quiet_NaN()), -1);
    QCOMPARE(range.whichZone(std::numeric_limits<double>::infinity()), -1);
}

void TestRideRefreshZones::rawAndResolvedThresholdsStayDistinct()
{
    const auto powerDefault = powerRange({}, {}, 301, 0);
    QCOMPARE(powerDefault.rawAeT, 0);
    QCOMPARE(powerDefault.resolvedAeT(), 256);

    RideRefreshZones::HeartRateRange hr;
    hr.lt = 171;
    QCOMPARE(hr.resolvedAeT(), 154);
    hr.rawAeT = 145;
    QCOMPARE(hr.resolvedAeT(), 145);

    RideRefreshZones::PaceRange pace;
    pace.cv = 10.0;
    QCOMPARE(pace.resolvedAeT(false), 9.0);
    QCOMPARE(pace.resolvedAeT(true), 9.75);
    pace.rawAeT = 8.5;
    QCOMPARE(pace.resolvedAeT(false), 8.5);
}

void TestRideRefreshZones::
fingerprintsPreserveLegacyInputsAndQuantization()
{
    auto power = powerRange({}, {}, 300, 0);
    const quint16 initialPower = power.fingerprint();
    power.bands[0].name = QStringLiteral("Renamed");
    power.bands[0].description = QStringLiteral("Changed");
    power.bands[0].high = 199;
    QCOMPARE(power.fingerprint(), initialPower);
    power.bands[0].low = 1;
    QVERIFY(power.fingerprint() != initialPower);

    RideRefreshZones::HeartRateRange hr;
    hr.lt = 170;
    hr.bands = {{QStringLiteral("H"), {}, 100, 200, 1.239}};
    const quint16 hrFingerprint = hr.fingerprint();
    hr.bands[0].trimp = 1.2301; // both truncate to 123
    QCOMPARE(hr.fingerprint(), hrFingerprint);
    hr.bands[0].trimp = 1.24;
    QVERIFY(hr.fingerprint() != hrFingerprint);

    RideRefreshZones::PaceRange pace;
    pace.cv = 10.009;
    pace.rawAeT = 8.009;
    pace.bands = {{QStringLiteral("P"), {}, 5.009, 20.0, 0.0}};
    const quint16 paceFingerprint = pace.fingerprint();
    pace.cv = 10.001;
    pace.rawAeT = 8.001;
    pace.bands[0].low = 5.001;
    QCOMPARE(pace.fingerprint(), paceFingerprint);

    RideRefreshZones::PowerHistory empty;
    QCOMPARE(empty.fingerprint(QDate(2024, 1, 1)),
             qChecksum(QByteArray::number(quint64(0))));
}

void TestRideRefreshZones::hrValuesDescriptionsAndTrimpsAreRetained()
{
    RideRefreshZones::HeartRateRange range;
    range.lt = 170;
    range.rawAeT = 145;
    range.restHr = 48;
    range.maxHr = 191;
    range.bands = {{QStringLiteral("Tempo"), QStringLiteral("Steady"),
                    140, 160, 1.7}};

    QCOMPARE(range.lt, 170);
    QCOMPARE(range.resolvedAeT(), 145);
    QCOMPARE(range.restHr, 48);
    QCOMPARE(range.maxHr, 191);
    QCOMPARE(range.bands.first().name, QStringLiteral("Tempo"));
    QCOMPARE(range.bands.first().description, QStringLiteral("Steady"));
    QCOMPARE(range.bands.first().trimp, 1.7);
    QCOMPARE(range.whichZone(159.9), 0);
}

void TestRideRefreshZones::
paceValuesFormattingAndUnitsAreGenerationValues()
{
    RideRefreshZones::PaceHistory run;
    run.present = true;
    run.metricPace = true;
    run.metricUnits = QStringLiteral("min/km");
    run.imperialUnits = QStringLiteral("min/mile");
    RideRefreshZones::PaceHistory swim;
    swim.present = true;
    swim.metricPace = false;
    swim.metricUnits = QStringLiteral("min/100m");
    swim.imperialUnits = QStringLiteral("min/100yd");
    auto zones = RideRefreshZones::create({}, {}, run, swim);

    QCOMPARE(zones->pace(false)->formatPace(12.0), QStringLiteral("05:00"));
    QCOMPARE(zones->pace(false)->paceUnits(), QStringLiteral("min/km"));
    QCOMPARE(zones->pace(true)->formatPace(3.6), QStringLiteral("01:31"));
    QCOMPARE(zones->pace(true)->paceUnits(), QStringLiteral("min/100yd"));
    QCOMPARE(zones->pace(false)->formatPace(0.0), QStringLiteral("00:00"));
    QCOMPARE(zones->pace(false)->formatPace(100.0), QStringLiteral("xx:xx"));
}

void TestRideRefreshZones::createNormalizesRunAndSwimIdentity()
{
    RideRefreshZones::PaceHistory wrongRun;
    wrongRun.present = true;
    wrongRun.swim = true;
    RideRefreshZones::PaceHistory wrongSwim;
    wrongSwim.present = true;
    wrongSwim.swim = false;
    auto zones = RideRefreshZones::create({}, {}, wrongRun, wrongSwim);
    QVERIFY(!zones->pace(false)->swim);
    QVERIFY(zones->pace(true)->swim);
    QVERIFY(!RideRefreshZones::create({}, {}, {}, {})->pace(false));
}

void TestRideRefreshZones::
cacheAnalysisFingerprintPreservesLegacyCanonicalInputs()
{
    const QDate date(2024, 2, 1);
    const QString sport = QStringLiteral("Bike");
    Zones power(sport);
    power.addZoneRange(QDate(2024, 1, 1), QDate(),
                       301, 0, 286, 21001, 1201);
    ZoneRange powerSource = power.getZoneRange(0);
    powerSource.zones = {
        ZoneInfo(QStringLiteral("P1"), {}, 0, 201),
        ZoneInfo(QStringLiteral("P2"), {}, 201, 2001)};
    power.setZoneRange(0, powerSource);

    HrZones heartRate(sport);
    heartRate.addHrZoneRange(
        QDate(2024, 1, 1), QDate(), 171, 0, 49, 194);
    HrZoneRange heartRateSource = heartRate.getHrZoneRange(0);
    heartRateSource.zones = {
        HrZoneInfo(QStringLiteral("H1"), {}, 0, 151, 1.0),
        HrZoneInfo(QStringLiteral("H2"), {}, 151, 251, 2.0)};
    heartRate.setHrZoneRange(0, heartRateSource);

    PaceZones pace(false);
    pace.addZoneRange(QDate(2024, 1, 1), QDate(), 12.25, 0.0);
    PaceZoneRange paceSource = pace.getZoneRange(0);
    paceSource.zones = {
        PaceZoneInfo(QStringLiteral("V1"), {}, 0.0, 9.5),
        PaceZoneInfo(QStringLiteral("V2"), {}, 9.5, 30.0)};
    pace.setZoneRange(0, paceSource);

    QHash<QString, RideRefreshZones::PowerHistory> powers;
    powers.insert(sport, captureRideRefreshPowerZones(&power, 0));
    QHash<QString, RideRefreshZones::HeartRateHistory> heartRates;
    heartRates.insert(sport, captureRideRefreshHeartRateZones(&heartRate));
    const auto zones = RideRefreshZones::create(
        powers, heartRates,
        captureRideRefreshPaceZones(&pace, false, true), {});
    const RideRefreshCacheSettings settings{
        QStringLiteral("integral"), 337, 2134};
    const QByteArray expected = legacyAnalysisFingerprint(
        &power, &heartRate, &pace, settings,
        date, sport, false, 73.25);
    const QByteArray actual = rideRefreshCacheAnalysisFingerprint(
        zones.get(), settings, date, sport, false, 73.25);
    QCOMPARE(actual, expected);
    QCOMPARE(actual.size(), 32);

    auto changed = settings;
    changed.wbalFormula = QStringLiteral("differential");
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        zones.get(), changed, date, sport, false, 73.25) != actual);
    changed = settings;
    ++changed.wbalTau;
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        zones.get(), changed, date, sport, false, 73.25) != actual);
    changed = settings;
    ++changed.wheelSize;
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        zones.get(), changed, date, sport, false, 73.25) != actual);
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        zones.get(), settings, date, sport, false, 73.5) != actual);

    PaceZones swimPace(true);
    swimPace.addZoneRange(QDate(2024, 1, 1), QDate(), 3.75, 0.0);
    PaceZoneRange swimSource = swimPace.getZoneRange(0);
    swimSource.zones = {
        PaceZoneInfo(QStringLiteral("S1"), {}, 0.0, 3.0),
        PaceZoneInfo(QStringLiteral("S2"), {}, 3.0, 10.0)};
    swimPace.setZoneRange(0, swimSource);
    const auto swimZones = RideRefreshZones::create(
        {}, {}, {}, captureRideRefreshPaceZones(
            &swimPace, true, true));
    QCOMPARE(rideRefreshCacheAnalysisFingerprint(
                 swimZones.get(), settings, date,
                 QStringLiteral("Swim"), true, 73.25),
             legacyAnalysisFingerprint(
                 nullptr, nullptr, &swimPace, settings, date,
                 QStringLiteral("Swim"), true, 73.25));
}

void TestRideRefreshZones::
cacheInputsDistinguishMissingSnapshotAndAbsentDomains()
{
    const QDate date(2024, 2, 1);
    const QString sport = QStringLiteral("Bike");
    const RideRefreshCacheSettings settings;
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        nullptr, settings, date, sport, false, 75.0).isEmpty());
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        RideRefreshZones::create({}, {}, {}, {}).get(), settings,
        date, sport, false, 0.0).isEmpty());
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        RideRefreshZones::create({}, {}, {}, {}).get(), settings,
        date, sport, false, -1.0).isEmpty());
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        RideRefreshZones::create({}, {}, {}, {}).get(), settings,
        date, sport, false,
        std::numeric_limits<double>::quiet_NaN()).isEmpty());
    QVERIFY(rideRefreshCacheAnalysisFingerprint(
        RideRefreshZones::create({}, {}, {}, {}).get(), settings,
        date, sport, false,
        std::numeric_limits<double>::infinity()).isEmpty());

    const auto absent = RideRefreshZones::create({}, {}, {}, {});
    const QByteArray fingerprint = rideRefreshCacheAnalysisFingerprint(
        absent.get(), settings, date, sport, false, 75.0);
    QCOMPARE(fingerprint, legacyAnalysisFingerprint(
        nullptr, nullptr, nullptr, settings,
        date, sport, false, 75.0));

    Zones emptyPower(sport);
    HrZones emptyHeartRate(sport);
    PaceZones emptyPace(false);
    const auto presentWithoutRanges = RideRefreshZones::create(
        {{sport, captureRideRefreshPowerZones(&emptyPower, 0)}},
        {{sport, captureRideRefreshHeartRateZones(&emptyHeartRate)}},
        captureRideRefreshPaceZones(&emptyPace, false, true), {});
    QCOMPARE(rideRefreshCacheAnalysisFingerprint(
                 presentWithoutRanges.get(), settings,
                 date, sport, false, 75.0),
             legacyAnalysisFingerprint(
                 &emptyPower, &emptyHeartRate, &emptyPace, settings,
                 date, sport, false, 75.0));

    RideRefreshZones::PowerHistory bike;
    bike.present = true;
    bike.sport = QStringLiteral("Bike");
    bike.ranges = {powerRange({}, {}, 299)};
    QHash<QString, RideRefreshZones::PowerHistory> exactNullPower = {
        {QStringLiteral("Bike"), bike},
        {QStringLiteral("Run"), {}}};
    const auto exactNull = RideRefreshZones::create(
        exactNullPower, {}, {}, {});
    QCOMPARE(rideRefreshCacheAnalysisFingerprint(
                 exactNull.get(), settings, date,
                 QStringLiteral("Run"), false, 75.0),
             legacyAnalysisFingerprint(
                 nullptr, nullptr, nullptr, settings, date,
                 QStringLiteral("Run"), false, 75.0));

    const auto missing = rideRefreshDistributionZones(
        nullptr, date, sport, false);
    QVERIFY(!missing.valid);
    const auto explicitlyAbsent = rideRefreshDistributionZones(
        absent.get(), date, sport, false);
    QVERIFY(explicitlyAbsent.valid);
    QVERIFY(!explicitlyAbsent.powerDomainPresent);
    QVERIFY(!explicitlyAbsent.heartRateDomainPresent);
    QVERIFY(!explicitlyAbsent.paceDomainPresent);
    QVERIFY(!explicitlyAbsent.power.has_value());
    QVERIFY(!explicitlyAbsent.heartRate.has_value());
    QVERIFY(!explicitlyAbsent.pace.has_value());
}

void TestRideRefreshZones::distributionInputsOwnSelectedImmutableRanges()
{
    RideRefreshZones::PowerHistory power;
    power.present = true;
    power.sport = QStringLiteral("Bike");
    power.ranges = {powerRange(QDate(2024, 1, 1), QDate(), 307, 251)};
    RideRefreshZones::HeartRateHistory heartRate;
    heartRate.present = true;
    heartRate.sport = QStringLiteral("Bike");
    RideRefreshZones::HeartRateRange heartRateRange;
    heartRateRange.begin = QDate(2024, 1, 1);
    heartRateRange.lt = 173;
    heartRateRange.rawAeT = 147;
    heartRateRange.bands = {{QStringLiteral("H"), {}, 100, 200, 1.0}};
    heartRate.ranges = {heartRateRange};
    RideRefreshZones::PaceHistory pace;
    pace.present = true;
    RideRefreshZones::PaceRange paceRange;
    paceRange.begin = QDate(2024, 1, 1);
    paceRange.cv = 11.75;
    paceRange.rawAeT = 9.25;
    paceRange.bands = {{QStringLiteral("V"), {}, 0.0, 20.0, 0.0}};
    pace.ranges = {paceRange};
    const auto zones = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}},
        {{QStringLiteral("Bike"), heartRate}}, pace, {});

    auto inputs = rideRefreshDistributionZones(
        zones.get(), QDate(2024, 2, 1), QStringLiteral("Bike"), false);
    QVERIFY(inputs.valid);
    QVERIFY(inputs.powerDomainPresent);
    QVERIFY(inputs.heartRateDomainPresent);
    QVERIFY(inputs.paceDomainPresent);
    QVERIFY(inputs.power.has_value());
    QVERIFY(inputs.heartRate.has_value());
    QVERIFY(inputs.pace.has_value());
    QCOMPARE(inputs.power->cp, 307);
    QCOMPARE(inputs.power->resolvedAeT(), 251);
    QCOMPARE(inputs.power->wprime, 20000);
    QCOMPARE(inputs.power->whichZone(200.0), 1);
    QCOMPARE(inputs.heartRate->lt, 173);
    QCOMPARE(inputs.heartRate->resolvedAeT(), 147);
    QCOMPARE(inputs.heartRate->whichZone(150.0), 0);
    QCOMPARE(inputs.pace->cv, 11.75);
    QCOMPARE(inputs.pace->resolvedAeT(false), 9.25);
    QCOMPARE(inputs.pace->whichZone(10.0), 0);

    inputs.power->cp = -1;
    QCOMPARE(zones->power(QStringLiteral("Bike"))
                 ->rangeForDate(QDate(2024, 2, 1))->cp, 307);

    const auto outside = rideRefreshDistributionZones(
        zones.get(), QDate(2023, 1, 1), QStringLiteral("Bike"), false);
    QVERIFY(outside.valid);
    QVERIFY(outside.powerDomainPresent);
    QVERIFY(outside.heartRateDomainPresent);
    QVERIFY(outside.paceDomainPresent);
    QVERIFY(!outside.power.has_value());
    QVERIFY(!outside.heartRate.has_value());
    QVERIFY(!outside.pace.has_value());
}

void TestRideRefreshZones::productionCaptureMatchesLivePowerZones()
{
    Zones live(QStringLiteral("Bike"));
    live.addZoneRange(
        QDate(2024, 1, 1), QDate(2024, 3, 1),
        301, 0, 285, 21000, 1200);
    ZoneRange source = live.getZoneRange(0);
    source.zones = {
        ZoneInfo(QStringLiteral("P1"), QStringLiteral("Easy"), 0, 200),
        ZoneInfo(QStringLiteral("P2"), QStringLiteral("Hard"), 200, 2000)};
    live.setZoneRange(0, source);

    const auto captured = captureRideRefreshPowerZones(&live, 1);
    QCOMPARE(captured.sport, live.sport());
    QCOMPARE(captured.rawCpForFtpSetting, 1);
    QVERIFY(!captured.cpOverridesFtp());
    for (const QDate date : {QDate(2023, 12, 31), QDate(2024, 1, 1),
                             QDate(2024, 2, 29), QDate(2024, 3, 1)}) {
        QCOMPARE(captured.whichRange(date), live.whichRange(date));
        QCOMPARE(captured.fingerprint(date), live.getFingerprint(date));
    }
    const auto *range = captured.rangeForDate(QDate(2024, 2, 1));
    QVERIFY(range);
    QCOMPARE(range->resolvedAeT(), live.getAeT(0));
    for (double value : {-1.0, 0.0, 199.99, 200.0, 1999.9, 2000.0,
                         std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
        QCOMPARE(range->whichZone(value), live.whichZone(0, value));
    }
    QCOMPARE(range->bands[1].name, QStringLiteral("P2"));
    QCOMPARE(range->bands[1].description, QStringLiteral("Hard"));
}

void TestRideRefreshZones::productionCaptureMatchesLiveHeartRateZones()
{
    HrZones live(QStringLiteral("Bike"));
    live.addHrZoneRange(
        QDate(2024, 1, 1), QDate(), 171, 0, 48, 193);
    HrZoneRange source = live.getHrZoneRange(0);
    source.zones = {
        HrZoneInfo(QStringLiteral("H1"), QStringLiteral("Easy"),
                   0, 150, 1.239),
        HrZoneInfo(QStringLiteral("H2"), QStringLiteral("Hard"),
                   150, 250, 2.0)};
    live.setHrZoneRange(0, source);

    const auto captured = captureRideRefreshHeartRateZones(&live);
    QCOMPARE(captured.whichRange(QDate(2024, 1, 1)), live.whichRange(QDate(2024, 1, 1)));
    QCOMPARE(captured.fingerprint(QDate(2024, 2, 1)), live.getFingerprint(QDate(2024, 2, 1)));
    const auto *range = captured.rangeForDate(QDate(2024, 2, 1));
    QVERIFY(range);
    QCOMPARE(range->resolvedAeT(), live.getAeT(0));
    QCOMPARE(range->whichZone(150.0), live.whichZone(0, 150.0));
    QCOMPARE(range->bands[0].trimp, 1.239);
}

void TestRideRefreshZones::productionCaptureMatchesLivePaceZones()
{
    PaceZones live(true);
    live.addZoneRange(QDate(), QDate(), 3.6, 0.0);
    PaceZoneRange source = live.getZoneRange(0);
    source.zones = {
        PaceZoneInfo(QStringLiteral("S1"), QStringLiteral("Easy"),
                     0.0, 3.0),
        PaceZoneInfo(QStringLiteral("S2"), QStringLiteral("Hard"),
                     3.0, 20.0)};
    live.setZoneRange(0, source);

    const auto captured = captureRideRefreshPaceZones(&live, true, false);
    QCOMPARE(captured.fingerprint(QDate(2024, 1, 1)),
             live.getFingerprint(QDate(2024, 1, 1)));
    const auto *range = captured.rangeForDate(QDate(2024, 1, 1));
    QVERIFY(range);
    QCOMPARE(range->resolvedAeT(true), live.getAeT(0));
    QCOMPARE(range->whichZone(3.0), live.whichZone(0, 3.0));
    QCOMPARE(captured.formatPace(3.6), live.kphToPaceString(3.6, false));
    QCOMPARE(captured.paceUnits(), live.paceUnits(false));
}

void TestRideRefreshZones::
ownerThreadAssemblyUsesCapturedSettingsAndPreservesNulls()
{
    QObject owner;
    Zones bike(QStringLiteral("Bike"));
    bike.addZoneRange(QDate(), QDate(), 300, 0, 280, 20000, 1000);
    PaceZones run(false);
    PaceZones swim(true);

    QHash<QString, Zones *> power = {
        {QStringLiteral("Bike"), &bike},
        {QStringLiteral("Run"), nullptr}
    };
    QHash<QString, HrZones *> heartRate = {
        {QStringLiteral("Run"), nullptr}
    };
    QHash<QString, QVariant> globalSettings;
    globalSettings.insert(run.paceSetting(), false);
    globalSettings.insert(swim.paceSetting(), true);
    QHash<QString, QVariant> athleteSettings;
    athleteSettings.insert(bike.useCPforFTPSetting(), 1);

    const auto captured = captureRideRefreshZonesForOwner(
        &owner, power, heartRate, &run, &swim,
        globalSettings, athleteSettings, true);
    QVERIFY(captured);
    QVERIFY(captured->power(QStringLiteral("Bike")));
    QVERIFY(!captured->power(QStringLiteral("Bike"))->cpOverridesFtp());
    QVERIFY(!captured->power(QStringLiteral("Run")));
    QCOMPARE(captured->power(QStringLiteral("Other"))->sport,
             QStringLiteral("Bike"));
    QVERIFY(!captured->heartRate(QStringLiteral("Run")));
    QVERIFY(captured->pace(false));
    QVERIFY(!captured->pace(false)->metricPace);
    QVERIFY(captured->pace(true)->metricPace);

    QThread foreignThread;
    QObject foreignOwner;
    foreignOwner.moveToThread(&foreignThread);
    foreignThread.start();
    QVERIFY(!captureRideRefreshZonesForOwner(
        &foreignOwner, power, heartRate, &run, &swim,
        globalSettings, athleteSettings, true));
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

void TestRideRefreshZones::athleteWrapperChecksOwnerBeforeReadingState()
{
    QFile file(QDir(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)))
                   .filePath(QStringLiteral(
                       "src/Core/RideRefreshZonesAthleteCapture.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray source = file.readAll();
    const qsizetype function = source.indexOf("captureRideRefreshZones(");
    const qsizetype ownerCheck = source.indexOf(
        "QThread::currentThread() != athlete->thread()", function);
    const qsizetype firstPowerRead = source.indexOf("athlete->zones_", function);
    const qsizetype firstHeartRateRead = source.indexOf(
        "athlete->hrzones_", function);
    const qsizetype firstPaceRead = source.indexOf(
        "athlete->paceZones(false)", function);

    QVERIFY(function >= 0);
    QVERIFY(ownerCheck > function);
    QVERIFY(firstPowerRead > ownerCheck);
    QVERIFY(firstHeartRateRead > ownerCheck);
    QVERIFY(firstPaceRead > ownerCheck);
}

QTEST_GUILESS_MAIN(TestRideRefreshZones)

#include "testRideRefreshZones.moc"
