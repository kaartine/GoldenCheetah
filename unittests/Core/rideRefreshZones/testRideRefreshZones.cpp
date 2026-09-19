#include <QtTest>

#include "RideRefreshZones.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "Zones.h"

#include <QThread>
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
