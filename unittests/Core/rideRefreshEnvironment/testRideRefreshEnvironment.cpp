#include <QtTest>

#include "AthleteRefreshLifecycle.h"
#include "AthleteSession.h"
#include "RideRefreshEnvironment.h"
#include "RideRefreshCacheInputs.h"
#include "RideRefreshItemInputs.h"
#include "RideRefreshMeasures.h"
#include "RideRefreshRoutes.h"
#include "RideRefreshZones.h"
#include "SessionServices.h"

#include <QDir>
#include <QFile>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

namespace {

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

class NullApplicationService final : public AthleteApplicationService
{
public:
    QWebEngineProfile *webEngineProfile() const override { return nullptr; }
};

class NullPersistenceService final : public AthletePersistenceService
{
public:
    void reportCacheWriteFailure(
        const QString &, const QString &) override
    {
    }
};

std::shared_ptr<const RideRefreshEnvironment> environment(
    quint64 generation, int first, int second)
{
    RideRefreshEnvironment::Settings settings;
    settings.global.insert(QStringLiteral("first"), first);
    settings.global.insert(QStringLiteral("second"), second);
    settings.athlete.insert(QStringLiteral("present"), 42);
    settings.athlete.insert(QStringLiteral("missing"), QVariant());
    return RideRefreshEnvironment::create(
        generation, std::move(settings), true,
        QStringLiteral("Notes"), {}, {}, {QStringLiteral("Bike")});
}

} // namespace

class TestRideRefreshEnvironment : public QObject
{
    Q_OBJECT

private slots:
    void settingsAreImmutableAndDefaultsAreCallSpecific();
    void fixedSettingInventoryCoversWorkerReads();
    void colorRulesPreserveEngineOrderingAndFallback();
    void calendarFormattingPreservesFieldAndUnitSemantics();
    void rideItemFingerprintUsesOnlyImmutableGenerationValues();
    void rideItemFingerprintFailsClosedOnMissingDomains();
    void rideItemWeightPreservesLegacyFallbacks();
    void rideItemWeightMilligramsRejectsUnsafeConversions();
    void cacheInputsAndStoragePathsAreGenerationBound();
    void itemStaleGateIsGenerationBoundAndPreservesWriteOrder();
    void measuresSnapshotIsRetainedByTheGeneration();
    void routesSnapshotIsRetainedByTheGeneration();
    void zonesSnapshotIsRetainedByTheGeneration();
    void sessionPublishesWholeGenerationsAtomically();
    void publicationIsOwnerThreadOnlyAndClosedByLifecycle();
    void productionWorkersRetainTheirPublishedGeneration();
};

void TestRideRefreshEnvironment::
settingsAreImmutableAndDefaultsAreCallSpecific()
{
    auto snapshot = environment(7, 10, 20);
    QCOMPARE(snapshot->generation(), quint64(7));
    QCOMPARE(snapshot->globalSetting(QStringLiteral("first"), -1).toInt(), 10);
    QCOMPARE(snapshot->globalSetting(QStringLiteral("absent"), 31).toInt(), 31);
    QCOMPARE(snapshot->athleteSetting(QStringLiteral("present"), -1).toInt(), 42);
    QCOMPARE(snapshot->athleteSetting(QStringLiteral("missing"), 57).toInt(), 57);
    QCOMPARE(snapshot->athleteSetting(QStringLiteral("missing"), 99).toInt(), 99);
    QVERIFY(snapshot->useMetricUnits());
    QCOMPARE(snapshot->colorField(), QStringLiteral("Notes"));
    QCOMPARE(snapshot->sports(), QStringList({QStringLiteral("Bike")}));
}

void TestRideRefreshEnvironment::fixedSettingInventoryCoversWorkerReads()
{
    const QStringList globals = RideRefreshEnvironment::globalSettingKeys();
    const QStringList athlete = RideRefreshEnvironment::athleteSettingKeys();
    QCOMPARE(globals.size(), 11);
    QCOMPARE(globals, QStringList({
        QStringLiteral("<global-general>unit"),
        QStringLiteral("<global-general>pace"),
        QStringLiteral("<global-general>swimpace"),
        QStringLiteral("<global-general>elevationHysteresis"),
        QStringLiteral("<global-general>wbal/formula"),
        QStringLiteral("<global-general>garminSmartRecord"),
        QStringLiteral("<global-general>garminHWMark"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_max"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_min"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_filt"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_window")
    }));

    QCOMPARE(athlete.size(), 8);
    QCOMPARE(athlete, QStringList({
        QStringLiteral("<athlete-preferences>weight"),
        QStringLiteral("<athlete-preferences>height"),
        QStringLiteral("<athlete-preferences>crankLength"),
        QStringLiteral("<athlete-preferences>intervals/discovery"),
        QStringLiteral("<athlete-preferences>dob"),
        QStringLiteral("<athlete-preferences>sex"),
        QStringLiteral("<athlete-preferences>wbaltau"),
        QStringLiteral("<athlete-preferences>wheelsize")
    }));
}

void TestRideRefreshEnvironment::
colorRulesPreserveEngineOrderingAndFallback()
{
    QMap<QString, QColor> rules;
    rules.insert(QStringLiteral("Alpha"), QColor(Qt::red));
    rules.insert(QStringLiteral("zeta"), QColor(Qt::blue));
    auto snapshot = RideRefreshEnvironment::create(
        1, {}, true, QStringLiteral("Notes"), rules, {}, {});

    QCOMPARE(snapshot->colorFor(QStringLiteral("none")), QColor(1, 1, 1, 1));
    QCOMPARE(snapshot->colorFor(QStringLiteral("ALPHA")), QColor(Qt::red));
    // QMap iteration is lexical and the last matching rule wins.
    QCOMPARE(
        snapshot->colorFor(QStringLiteral("zeta and alpha")),
        QColor(Qt::blue));
}

void TestRideRefreshEnvironment::
calendarFormattingPreservesFieldAndUnitSemantics()
{
    const QVector<RideRefreshEnvironment::CalendarField> fields = {
        {QStringLiteral("Start Date"), QStringLiteral("Start Date"), 5, true, {}},
        {QStringLiteral("Start Time"), QStringLiteral("Start Time"), 6, true, {}},
        {QStringLiteral("Score"), QStringLiteral("Score"), 3, true, {}},
        {QStringLiteral("Notes"), QStringLiteral("Notes"), 0, true, {}},
        {QStringLiteral("Weight"), QStringLiteral("Athlete Weight"), 4, true,
         QStringLiteral("athlete_weight")},
        // Preserve the legacy TIME -> DATE fallthrough for odd definitions.
        {QStringLiteral("Start Date"), QStringLiteral("Odd Date"), 6, true, {}},
        // DATE cannot fall backwards into TIME in the legacy switch.
        {QStringLiteral("Start Time"), QStringLiteral("Odd Time"), 5, true, {}},
        {QStringLiteral("Hidden"), QStringLiteral("Hidden"), 0, false, {}, true}
    };
    auto snapshot = RideRefreshEnvironment::create(
        2, {}, false, {}, {}, fields, {});

    QString requestedTextName;
    QString requestedMetricSymbol;
    bool metricUnits = true;
    const QString text = snapshot->calendarText(
        [&](const QString &name) {
            requestedTextName = name;
            if (name == QStringLiteral("Start Date")) return QStringLiteral("1");
            if (name == QStringLiteral("Start Time")) return QStringLiteral("1");
            if (name == QStringLiteral("Score")) return QStringLiteral("9");
            if (name == QStringLiteral("Notes")) return QStringLiteral("steady");
            if (name == QStringLiteral("Odd Date")) return QStringLiteral("2");
            if (name == QStringLiteral("Odd Time")) return QStringLiteral("3");
            return QStringLiteral("ignored");
        },
        [&](const QString &symbol, bool useMetricUnits) -> QString {
            requestedMetricSymbol = symbol;
            metricUnits = useMetricUnits;
            return QStringLiteral("165 lb");
        },
        [](const QString &) { return true; });

    QVERIFY(text.contains(QStringLiteral("Start Date: 02/01/1900\n")));
    QVERIFY(text.contains(QStringLiteral("Start Time: 00:00:01.000\n")));
    QVERIFY(text.contains(QStringLiteral("Score: 9\n")));
    QVERIFY(text.contains(QStringLiteral("steady\n")));
    QVERIFY(text.contains(QStringLiteral("Weight: 165 lb\n")));
    QVERIFY(text.contains(QStringLiteral("Start Date: 03/01/1900\n")));
    QVERIFY(text.contains(QStringLiteral("Start Time: 3\n")));
    QVERIFY(!text.contains(QStringLiteral("Hidden")));
    QVERIFY(!metricUnits);
    QCOMPARE(requestedMetricSymbol, QStringLiteral("athlete_weight"));
    QCOMPARE(requestedTextName, QStringLiteral("Hidden"));
    QVERIFY(snapshot->metadataFields().constLast().interval);

    const QString irrelevant = snapshot->calendarText(
        [](const QString &) { return QString(); },
        [](const QString &, bool) { return QStringLiteral("wrong"); },
        [](const QString &) { return false; });
    QVERIFY(!irrelevant.contains(QStringLiteral("Weight")));
}

void TestRideRefreshEnvironment::measuresSnapshotIsRetainedByTheGeneration()
{
    auto measures = RideRefreshMeasures::create({}, 0x1234);
    auto snapshot = RideRefreshEnvironment::create(
        3, {}, true, {}, {}, {}, {}, {}, {}, measures);
    measures.reset();

    QVERIFY(snapshot->measures());
    QVERIFY(snapshot->measures()->groups().isEmpty());
}

void TestRideRefreshEnvironment::routesSnapshotIsRetainedByTheGeneration()
{
    auto routes = RideRefreshRoutes::create({}, 0x5678);
    auto snapshot = RideRefreshEnvironment::create(
        4, {}, true, {}, {}, {}, {}, {}, {}, {}, routes);
    routes.reset();

    QVERIFY(snapshot->routes());
    QCOMPARE(snapshot->routes()->fingerprint(), quint16(0x5678));
}

void TestRideRefreshEnvironment::zonesSnapshotIsRetainedByTheGeneration()
{
    auto zones = RideRefreshZones::create({}, {}, {}, {});
    auto snapshot = RideRefreshEnvironment::create(
        5, {}, true, {}, {}, {}, {}, {}, zones);
    zones.reset();

    QVERIFY(snapshot->zones());
    QVERIFY(!snapshot->zones()->power(QStringLiteral("Bike")));
}

void TestRideRefreshEnvironment::
rideItemFingerprintUsesOnlyImmutableGenerationValues()
{
    const QDate date(2024, 2, 1);
    RideRefreshZones::PowerRange powerRange;
    powerRange.begin = QDate(2024, 1, 1);
    powerRange.cp = 301;
    RideRefreshZones::PowerHistory power;
    power.present = true;
    power.sport = QStringLiteral("Bike");
    power.rawCpForFtpSetting = 1;
    power.ranges = {powerRange};
    RideRefreshZones::HeartRateRange heartRateRange;
    heartRateRange.begin = QDate(2024, 1, 1);
    heartRateRange.lt = 171;
    RideRefreshZones::HeartRateHistory heartRate;
    heartRate.present = true;
    heartRate.sport = QStringLiteral("Bike");
    heartRate.ranges = {heartRateRange};
    RideRefreshZones::PaceRange paceRange;
    paceRange.begin = QDate(2024, 1, 1);
    paceRange.cv = 12.5;
    RideRefreshZones::PaceHistory pace;
    pace.present = true;
    pace.ranges = {paceRange};
    const auto zones = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}},
        {{QStringLiteral("Bike"), heartRate}}, pace, {});

    RideRefreshMeasures::Group hrv;
    hrv.symbol = QStringLiteral("Hrv");
    RideRefreshMeasures::Observation observation;
    observation.when = QDateTime(date, QTime(12, 0));
    observation.legacyFingerprint = 0x1234;
    hrv.observations = {observation};
    const auto measures = RideRefreshMeasures::create({hrv}, 0x4321);
    const auto routes = RideRefreshRoutes::create({}, 0x2222);
    RideRefreshEnvironment::Settings settings;
    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>intervals/discovery"), 61);
    const auto snapshot = RideRefreshEnvironment::create(
        8, settings, true, {}, {}, {}, {}, {}, zones, measures, routes);

    unsigned long expected = power.fingerprint(date);
    expected += 1;
    expected += pace.fingerprint(date);
    expected += heartRate.fingerprint(date);
    expected += routes->fingerprint();
    expected += observation.fingerprint();
    expected += 61;
    QCOMPARE(snapshot->rideItemFingerprint(
                 date, QStringLiteral("Bike"), false).value(),
             expected);
    QCOMPARE(snapshot->rideItemFingerprint(
                 date, QStringLiteral("Run"), false).value(),
             expected); // absent Run domains use the legacy Bike fallback
    QVERIFY(!snapshot->rideItemFingerprint(
        date, QStringLiteral("Bike"), true));

    RideRefreshZones::PaceHistory swimPace;
    swimPace.present = true;
    RideRefreshZones::PaceRange swimRange;
    swimRange.begin = QDate(2024, 1, 1);
    swimRange.cv = 3.75;
    swimPace.ranges = {swimRange};
    const auto zonesWithSwim = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}},
        {{QStringLiteral("Bike"), heartRate}}, pace, swimPace);
    const auto swimSnapshot = RideRefreshEnvironment::create(
        8, settings, true, {}, {}, {}, {}, {},
        zonesWithSwim, measures, routes);
    QCOMPARE(swimSnapshot->rideItemFingerprint(
                 date, QStringLiteral("Swim"), true).value(),
             expected - pace.fingerprint(date)
                 + swimPace.fingerprint(date));

    auto zeroPower = power;
    zeroPower.rawCpForFtpSetting = 0;
    const auto zeroSettingZones = RideRefreshZones::create(
        {{QStringLiteral("Bike"), zeroPower}},
        {{QStringLiteral("Bike"), heartRate}}, pace, {});
    QCOMPARE(RideRefreshEnvironment::create(
                 8, settings, true, {}, {}, {}, {}, {},
                 zeroSettingZones, measures, routes)
                 ->rideItemFingerprint(
                     date, QStringLiteral("Bike"), false).value(),
             expected - 1UL);
    auto negativePower = power;
    negativePower.rawCpForFtpSetting = -1;
    const auto negativeSettingZones = RideRefreshZones::create(
        {{QStringLiteral("Bike"), negativePower}},
        {{QStringLiteral("Bike"), heartRate}}, pace, {});
    QCOMPARE(RideRefreshEnvironment::create(
                 8, settings, true, {}, {}, {}, {}, {},
                 negativeSettingZones, measures, routes)
                 ->rideItemFingerprint(
                     date, QStringLiteral("Bike"), false).value(),
             expected);

    auto exactNullPower = QHash<QString, RideRefreshZones::PowerHistory>{
        {QStringLiteral("Bike"), power},
        {QStringLiteral("Run"), {}}};
    const auto exactNullZones = RideRefreshZones::create(
        exactNullPower, {{QStringLiteral("Bike"), heartRate}}, pace, {});
    QVERIFY(!RideRefreshEnvironment::create(
        8, settings, true, {}, {}, {}, {}, {},
        exactNullZones, measures, routes)
                 ->rideItemFingerprint(
                     date, QStringLiteral("Run"), false));

    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>intervals/discovery"), -3);
    unsigned long signedExpected = expected - 61UL;
    signedExpected += -3;
    QCOMPARE(RideRefreshEnvironment::create(
                 8, settings, true, {}, {}, {}, {}, {},
                 zones, measures, routes)
                 ->rideItemFingerprint(
                     date, QStringLiteral("Bike"), false).value(),
             signedExpected);

    settings.athlete.clear();
    const auto defaulted = RideRefreshEnvironment::create(
        9, settings, true, {}, {}, {}, {}, {}, zones, measures, routes);
    QCOMPARE(defaulted->rideItemFingerprint(
                 date, QStringLiteral("Bike"), false).value(),
             expected - 61 + 57);
    QCOMPARE(defaulted->rideItemFingerprint(
                 QDate(2023, 1, 1), QStringLiteral("Bike"), false).value(),
             static_cast<unsigned long>(power.fingerprint(QDate(2023, 1, 1)))
                 + 1UL
                 + pace.fingerprint(QDate(2023, 1, 1))
                 + heartRate.fingerprint(QDate(2023, 1, 1))
                 + routes->fingerprint() + 0x4321UL + 57UL);
}

void TestRideRefreshEnvironment::
rideItemFingerprintFailsClosedOnMissingDomains()
{
    const QDate date(2024, 2, 1);
    RideRefreshZones::PowerHistory power;
    power.present = true;
    power.sport = QStringLiteral("Bike");
    RideRefreshZones::HeartRateHistory heartRate;
    heartRate.present = true;
    heartRate.sport = QStringLiteral("Bike");
    RideRefreshZones::PaceHistory pace;
    pace.present = true;
    const auto zones = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}},
        {{QStringLiteral("Bike"), heartRate}}, pace, {});
    const auto missingPower = RideRefreshZones::create(
        {}, {{QStringLiteral("Bike"), heartRate}}, pace, {});
    const auto missingHeartRate = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}}, {}, pace, {});
    const auto missingPace = RideRefreshZones::create(
        {{QStringLiteral("Bike"), power}},
        {{QStringLiteral("Bike"), heartRate}}, {}, {});
    RideRefreshMeasures::Group hrv;
    hrv.symbol = QStringLiteral("Hrv");
    const auto measures = RideRefreshMeasures::create({hrv}, 0);
    const auto missingHrv = RideRefreshMeasures::create({}, 0x9999);
    const auto routes = RideRefreshRoutes::create({}, 0);
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, {}, measures, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, zones, {}, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, zones, missingHrv, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, zones, measures, {})
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, missingPower, measures, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, missingHeartRate, measures, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
    QVERIFY(!RideRefreshEnvironment::create(
        1, {}, true, {}, {}, {}, {}, {}, missingPace, measures, routes)
                 ->rideItemFingerprint(date, QStringLiteral("Bike"), false));
}

void TestRideRefreshEnvironment::rideItemWeightPreservesLegacyFallbacks()
{
    RideRefreshMeasures::Group body;
    body.symbol = QStringLiteral("Body");
    RideRefreshMeasures::Observation observation;
    observation.when = QDateTime(QDate(2024, 1, 1), QTime(12, 0));
    observation.values[0] = 71.25;
    body.observations = {observation};
    const auto measures = RideRefreshMeasures::create({body}, 0);
    RideRefreshEnvironment::Settings settings;
    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>weight"),
        QStringLiteral("76.5"));
    const auto snapshot = RideRefreshEnvironment::create(
        1, settings, true, {}, {}, {}, {}, {}, {}, measures, {});

    QCOMPARE(snapshot->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             71.25);
    QCOMPARE(snapshot->rideItemWeight(
                 QDate(2024, 2, 1), QStringLiteral("68.0")).value(),
             71.25); // Body carries forward
    QCOMPARE(snapshot->rideItemWeight(
                 QDate(2023, 12, 31), QStringLiteral("68.0")).value(),
             68.0);
    QCOMPARE(snapshot->rideItemWeight(
                 QDate(2023, 12, 31), QStringLiteral("-1")).value(),
             76.5);

    const auto noBody = RideRefreshMeasures::create({}, 0);
    QCOMPARE(RideRefreshEnvironment::create(
                 2, settings, true, {}, {}, {}, {}, {}, {}, noBody, {})
                 ->rideItemWeight(
                     QDate(2024, 1, 1), QStringLiteral("69.5")).value(),
             69.5);
    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>weight"),
        QStringLiteral("0"));
    QCOMPARE(RideRefreshEnvironment::create(
                 3, settings, true, {}, {}, {}, {}, {}, {}, noBody, {})
                 ->rideItemWeight(
                     QDate(2024, 1, 1), QStringLiteral("0")).value(),
             80.0);
    settings.athlete.clear();
    QCOMPARE(RideRefreshEnvironment::create(
                 4, settings, true, {}, {}, {}, {}, {}, {}, noBody, {})
                 ->rideItemWeight(
                     QDate(2024, 1, 1), QStringLiteral("0")).value(),
             75.0);
    QVERIFY(!RideRefreshEnvironment::create(
        5, settings, true, {}, {}, {}, {}, {}, {}, {}, {})
                 ->rideItemWeight(
                     QDate(2024, 1, 1), QStringLiteral("69.5")));

    const auto invalidBodyWeight = [&](double value) {
        auto invalidBody = body;
        invalidBody.observations[0].values[0] = value;
        return RideRefreshEnvironment::create(
            6, settings, true, {}, {}, {}, {}, {}, {},
            RideRefreshMeasures::create({invalidBody}, 0), {});
    };
    QCOMPARE(invalidBodyWeight(0.0)->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             68.0);
    QCOMPARE(invalidBodyWeight(-1.0)->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             68.0);
    QCOMPARE(invalidBodyWeight(
                 std::numeric_limits<double>::quiet_NaN())->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             68.0);
    QCOMPARE(invalidBodyWeight(
                 std::numeric_limits<double>::infinity())->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             68.0);
    QCOMPARE(invalidBodyWeight(std::numeric_limits<double>::max())
                 ->rideItemWeight(
                     QDate(2024, 1, 1), QStringLiteral("68.0")).value(),
             68.0);

    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>weight"), 76.5);
    const auto metadataFallback = RideRefreshEnvironment::create(
        7, settings, true, {}, {}, {}, {}, {}, {}, noBody, {});
    QCOMPARE(metadataFallback->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("0")).value(),
             76.5);
    QCOMPARE(metadataFallback->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("nan")).value(),
             76.5);
    QCOMPARE(metadataFallback->rideItemWeight(
                 QDate(2024, 1, 1), QStringLiteral("inf")).value(),
             76.5);
    QCOMPARE(metadataFallback->rideItemWeight(
                 QDate(2024, 1, 1),
                 QString::number(
                     std::numeric_limits<double>::max(), 'g', 17)).value(),
             76.5);
    for (const QVariant &invalidSetting : {
             QVariant(-1.0), QVariant(0.0),
             QVariant(std::numeric_limits<double>::quiet_NaN()),
             QVariant(std::numeric_limits<double>::infinity()),
             QVariant(std::numeric_limits<double>::max())}) {
        settings.athlete.insert(
            QStringLiteral("<athlete-preferences>weight"), invalidSetting);
        QCOMPARE(RideRefreshEnvironment::create(
                     8, settings, true, {}, {}, {}, {}, {}, {}, noBody, {})
                     ->rideItemWeight(
                         QDate(2024, 1, 1), QStringLiteral("0")).value(),
                 80.0);
    }
}

void TestRideRefreshEnvironment::
rideItemWeightMilligramsRejectsUnsafeConversions()
{
    QCOMPARE(RideRefreshEnvironment::rideItemWeightMilligrams(0.0).value(),
             0UL);
    QCOMPARE(RideRefreshEnvironment::rideItemWeightMilligrams(71.2509).value(),
             71250UL);
    QVERIFY(!RideRefreshEnvironment::rideItemWeightMilligrams(-1.0));
    QVERIFY(!RideRefreshEnvironment::rideItemWeightMilligrams(
        std::numeric_limits<double>::quiet_NaN()));
    QVERIFY(!RideRefreshEnvironment::rideItemWeightMilligrams(
        std::numeric_limits<double>::infinity()));

    const double roundedBoundary = static_cast<double>(
        static_cast<long double>(std::numeric_limits<unsigned long>::max())
        / 1000.0L);
    const double belowBoundary = std::nextafter(roundedBoundary, 0.0);
    const double aboveBoundary = std::nextafter(
        roundedBoundary, std::numeric_limits<double>::infinity());
    QVERIFY(static_cast<long double>(1000.0f * belowBoundary)
            <= static_cast<long double>(
                std::numeric_limits<unsigned long>::max()));
    QVERIFY(static_cast<long double>(1000.0f * aboveBoundary)
            > static_cast<long double>(
                std::numeric_limits<unsigned long>::max()));
    QVERIFY(RideRefreshEnvironment::rideItemWeightMilligrams(belowBoundary));
    QVERIFY(!RideRefreshEnvironment::rideItemWeightMilligrams(
        aboveBoundary));
}

void TestRideRefreshEnvironment::
cacheInputsAndStoragePathsAreGenerationBound()
{
    RideRefreshEnvironment::Settings settings;
    settings.global.insert(
        QStringLiteral("<global-general>wbal/formula"),
        QStringLiteral("differential"));
    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>wbaltau"), 412);
    settings.athlete.insert(
        QStringLiteral("<athlete-preferences>wheelsize"), 2096);
    const auto zones = RideRefreshZones::create({}, {}, {}, {});
    const RideRefreshEnvironment::StoragePaths paths{
        QStringLiteral("/snapshot/cache"),
        QStringLiteral("/snapshot/activities"),
        QStringLiteral("/snapshot/planned")};
    const auto snapshot = RideRefreshEnvironment::create(
        12, settings, true, {}, {}, {}, {}, {}, zones, {}, {}, paths);
    QVERIFY(snapshot->storagePaths().isComplete());
    QCOMPARE(snapshot->storagePaths().cache, paths.cache);
    QCOMPARE(snapshot->storagePaths().activities, paths.activities);
    QCOMPARE(snapshot->storagePaths().planned, paths.planned);

    RideRefreshCacheSettings expectedSettings;
    expectedSettings.wbalFormula = QStringLiteral("differential");
    expectedSettings.wbalTau = 412;
    expectedSettings.wheelSize = 2096;
    const QDate date(2024, 6, 7);
    QCOMPARE(snapshot->rideFileCacheAnalysisFingerprint(
                 date, QStringLiteral("Bike"), false, 72.5),
             rideRefreshCacheAnalysisFingerprint(
                 zones.get(), expectedSettings,
                 date, QStringLiteral("Bike"), false, 72.5));
    QVERIFY(snapshot->rideFileCacheAnalysisFingerprint(
        date, QStringLiteral("Bike"), false, 0.0).isEmpty());

    const RideRefreshEnvironment::Settings defaultSettings;
    const auto defaultSnapshot = RideRefreshEnvironment::create(
        13, defaultSettings, true, {}, {}, {}, {}, {}, zones, {}, {}, paths);
    const RideRefreshCacheSettings expectedDefaultSettings;
    QCOMPARE(defaultSnapshot->rideFileCacheAnalysisFingerprint(
                 date, QStringLiteral("Bike"), false, 72.5),
             rideRefreshCacheAnalysisFingerprint(
                 zones.get(), expectedDefaultSettings,
                 date, QStringLiteral("Bike"), false, 72.5));
    QVERIFY(RideRefreshEnvironment::create(
        14, settings, true, {}, {}, {}, {}, {}, {}, {}, {}, paths)
                 ->rideFileCacheAnalysisFingerprint(
                     date, QStringLiteral("Bike"), false, 72.5).isEmpty());
    const RideRefreshEnvironment::StoragePaths incompletePaths{
        {}, paths.activities, paths.planned};
    QVERIFY(!incompletePaths.isComplete());
    const auto incompleteSnapshot = RideRefreshEnvironment::create(
        15, settings, true, {}, {}, {}, {}, {}, zones, {}, {},
        incompletePaths);
    QVERIFY(!incompleteSnapshot->storagePaths().isComplete());
    QCOMPARE(incompleteSnapshot->storagePaths().activities, paths.activities);
    const RideRefreshEnvironment::StoragePaths relativePaths{
        QStringLiteral("cache"),
        QStringLiteral("activities"),
        QStringLiteral("planned")};
    QVERIFY(!relativePaths.isComplete());

    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    QFile captureFile(root.filePath(
        QStringLiteral("src/Core/RideRefreshEnvironmentCapture.cpp")));
    QVERIFY(captureFile.open(QIODevice::ReadOnly));
    const QByteArray capture = captureFile.readAll();
    const qsizetype ownerGuard = capture.indexOf(
        "Q_ASSERT(QThread::currentThread() == context->thread())");
    const qsizetype homeGuard = capture.indexOf(
        "Q_ASSERT(context->athlete->home)", ownerGuard);
    const qsizetype createCall = capture.indexOf(
        "return RideRefreshEnvironment::create(", homeGuard);
    const qsizetype cacheRoot = capture.indexOf(
        "context->athlete->home->cache().absolutePath()", createCall);
    const qsizetype activitiesRoot = capture.indexOf(
        "context->athlete->home->activities().absolutePath()", cacheRoot);
    const qsizetype plannedRoot = capture.indexOf(
        "context->athlete->home->planned().absolutePath()", activitiesRoot);
    const qsizetype callClose = capture.indexOf("\n        });", plannedRoot);
    QVERIFY(ownerGuard >= 0);
    QVERIFY(homeGuard > ownerGuard);
    QVERIFY(createCall > homeGuard);
    QVERIFY(cacheRoot > createCall);
    QVERIFY(activitiesRoot > cacheRoot);
    QVERIFY(plannedRoot > activitiesRoot);
    QVERIFY(callClose > plannedRoot);
}

void TestRideRefreshEnvironment::
itemStaleGateIsGenerationBoundAndPreservesWriteOrder()
{
    QVERIFY(rideRefreshWorksetCaptureAllowed(true, false));
    QVERIFY(rideRefreshWorksetCaptureAllowed(false, true));
    QVERIFY(!rideRefreshWorksetCaptureAllowed(false, false));
    QThread otherThread;
    QVERIFY(rideRefreshCaptureThreadAllowed(
        QThread::currentThread(), QThread::currentThread()));
    QVERIFY(!rideRefreshCaptureThreadAllowed(
        QThread::currentThread(), &otherThread));
    QVERIFY(!rideRefreshCaptureThreadAllowed(nullptr, &otherThread));

    QMap<QString, QString> metadata;
    metadata.insert(QStringLiteral("Mood"), QStringLiteral("Fresh"));
    metadata.insert(QStringLiteral("Calendar Text"), QStringLiteral("old"));
    const QDateTime dateTime(
        QDate(2026, 9, 19), QTime(13, 14, 15));
    QCOMPARE(rideRefreshItemText(
                 metadata, dateTime, QStringLiteral("Start Date"), {}),
             QString::number(
                 QDate(1900, 1, 1).daysTo(dateTime.date())));
    QCOMPARE(rideRefreshItemText(
                 metadata, dateTime, QStringLiteral("Start Time"), {}),
             QString::number(
                 QTime(0, 0, 0).secsTo(dateTime.time())));
    QCOMPARE(rideRefreshItemText(
                 metadata, dateTime, QStringLiteral("Mood"), {}),
             QStringLiteral("Fresh"));
    const unsigned long metadataCrc = rideRefreshMetadataCrc(metadata);
    metadata[QStringLiteral("Calendar Text")] = QStringLiteral("new");
    QCOMPARE(rideRefreshMetadataCrc(metadata), metadataCrc);
    metadata[QStringLiteral("Mood")] = QStringLiteral("Tired");
    QVERIFY(rideRefreshMetadataCrc(metadata) != metadataCrc);

    RideRefreshItemStaleInputs inputs;
    inputs.generation = 41;
    inputs.initiallyStale = false;
    inputs.storedUserMetricSchemaVersion = 7;
    inputs.requiredUserMetricSchemaVersion = 7;
    inputs.storedDbVersion = 12;
    inputs.storedWeightMilligrams = 72500UL;
    inputs.resolvedWeight = 72.5009;
    inputs.resolvedWeightMilligrams = 72500UL;
    inputs.storedRefreshFingerprint = 99UL;
    inputs.refreshFingerprint = 99UL;

    auto decision = rideRefreshItemGateDecision(inputs, 42, 12);
    QVERIFY(decision.stale);
    QVERIFY(!decision.applyColor);
    QVERIFY(!decision.writeResolvedWeight);
    QVERIFY(!decision.continueWithSourceChecks);

    inputs.initiallyStale = true;
    decision = rideRefreshItemGateDecision(inputs, 41, 12);
    QVERIFY(decision.stale);
    QVERIFY(!decision.applyColor);
    inputs.initiallyStale = false;

    inputs.requiredUserMetricSchemaVersion.reset();
    decision = rideRefreshItemGateDecision(inputs, 41, 12);
    QVERIFY(decision.stale);
    QVERIFY(decision.applyColor);
    QVERIFY(!decision.writeResolvedWeight);
    inputs.requiredUserMetricSchemaVersion = 7;

    inputs.storedWeightMilligrams.reset();
    decision = rideRefreshItemGateDecision(inputs, 41, 12);
    QVERIFY(decision.stale);
    QVERIFY(decision.applyColor);
    QVERIFY(decision.writeResolvedWeight);
    QVERIFY(!decision.continueWithSourceChecks);
    inputs.storedWeightMilligrams = 72500UL;

    inputs.refreshFingerprint = 100UL;
    decision = rideRefreshItemGateDecision(inputs, 41, 12);
    QVERIFY(decision.stale);
    QVERIFY(decision.writeResolvedWeight);
    QVERIFY(!decision.continueWithSourceChecks);
    inputs.refreshFingerprint = 99UL;

    decision = rideRefreshItemGateDecision(inputs, 41, 12);
    QVERIFY(!decision.stale);
    QVERIFY(decision.applyColor);
    QVERIFY(decision.writeResolvedWeight);
    QVERIFY(decision.continueWithSourceChecks);

    inputs.storedTimestamp = 100;
    inputs.storedCrc = 55;
    inputs.samples = true;
    inputs.hasIntervals = true;
    auto sourceDecision = rideRefreshItemSourceDecision(
        inputs, 101, 55U);
    QVERIFY(!sourceDecision.stale);
    QVERIFY(!sourceDecision.crcUpdate);
    sourceDecision = rideRefreshItemSourceDecision(
        inputs, 101, std::nullopt);
    QVERIFY(sourceDecision.stale);
    QVERIFY(!sourceDecision.crcUpdate);
    sourceDecision = rideRefreshItemSourceDecision(
        inputs, 101, 56U);
    QVERIFY(sourceDecision.stale);
    QCOMPARE(sourceDecision.crcUpdate, std::optional<unsigned int>(56U));
    inputs.storedCrc = 0;
    sourceDecision = rideRefreshItemSourceDecision(
        inputs, 101, 55U);
    QVERIFY(sourceDecision.stale);
    QCOMPARE(sourceDecision.crcUpdate, std::optional<unsigned int>(55U));
    inputs.storedCrc = 55;
    inputs.hasIntervals = false;
    sourceDecision = rideRefreshItemSourceDecision(
        inputs, 100, std::nullopt);
    QVERIFY(sourceDecision.stale);
    QVERIFY(!sourceDecision.crcUpdate);
}

void TestRideRefreshEnvironment::sessionPublishesWholeGenerationsAtomically()
{
    AthleteSession session(
        std::make_unique<NullApplicationService>(),
        std::make_unique<NullPersistenceService>());
    const auto oldEnvironment = environment(1, 11, 12);
    const auto newEnvironment = environment(2, 21, 22);
    QVERIFY(session.publishRefreshEnvironment(oldEnvironment));

    std::atomic_bool stop{false};
    std::atomic_int mixed{0};
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            const auto current = session.refreshEnvironment();
            if (!current) continue;
            const int first = current->globalSetting(
                QStringLiteral("first")).toInt();
            const int second = current->globalSetting(
                QStringLiteral("second")).toInt();
            if (!((first == 11 && second == 12)
                  || (first == 21 && second == 22))) {
                mixed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    for (int index = 0; index < 10000; ++index) {
        QVERIFY(session.publishRefreshEnvironment(
            index % 2 ? oldEnvironment : newEnvironment));
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    QCOMPARE(mixed.load(std::memory_order_relaxed), 0);
}

void TestRideRefreshEnvironment::
publicationIsOwnerThreadOnlyAndClosedByLifecycle()
{
    AthleteSession session(
        std::make_unique<NullApplicationService>(),
        std::make_unique<NullPersistenceService>());
    const auto snapshot = environment(1, 1, 2);
    QVERIFY(session.publishRefreshEnvironment(snapshot));
    QVERIFY(session.refreshLifecycle().beginConfigTransition());
    QVERIFY(!session.publishRefreshEnvironment(environment(2, 3, 4)));
    QCOMPARE(session.refreshEnvironment()->generation(), quint64(1));
    QVERIFY(session.refreshLifecycle().finishConfigTransition());

    bool wrongThreadPublished = true;
    std::thread wrongThread([&]() {
        wrongThreadPublished =
            session.publishRefreshEnvironment(environment(3, 5, 6));
    });
    wrongThread.join();
    QVERIFY(!wrongThreadPublished);
    QCOMPARE(session.refreshEnvironment()->generation(), quint64(1));
}

void TestRideRefreshEnvironment::
productionWorkersRetainTheirPublishedGeneration()
{
    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    QFile sourceFile(root.filePath(QStringLiteral("src/Core/RideCache.cpp")));
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray source = sourceFile.readAll();
    const qsizetype start = source.indexOf("RideCache::startLatestRefresh()");
    const qsizetype capture = source.indexOf(
        "captureRideRefreshEnvironment(context, generation)", start);
    const qsizetype validation = source.indexOf(
        "!environment || environment->generation() != generation",
        capture);
    const qsizetype worksetCapture = source.indexOf(
        "work.inputs = item->captureRefreshInputs(*environment)",
        validation);
    const qsizetype publish = source.indexOf(
        "publishRefreshEnvironment(environment)", worksetCapture);
    const qsizetype worker = source.indexOf(
        "this, generation, environment, workset", publish);
    const qsizetype run = source.indexOf(
        "void RideCacheRefreshThread::run()", worker);
    const qsizetype guard = source.indexOf(
        "!environment || environment->generation() != generation", run);
    const qsizetype loop = source.indexOf(
        "while (!isInterruptionRequested())", run);
    const qsizetype nextRefresh = source.indexOf(
        "target->nextRefresh(\n            generation, workset->count())", run);
    const qsizetype staleCheck = source.indexOf(
        "item->checkStale(*environment, work->inputs)", nextRefresh);
    const qsizetype constructor = source.indexOf(
        "RideCacheRefreshThread::RideCacheRefreshThread(");
    const qsizetype retained = source.indexOf(
        "environment(std::move(environment))", constructor);
    const qsizetype retainedWorkset = source.indexOf(
        "workset(std::move(workset))", retained);
    const qsizetype constructorBody = source.indexOf("\n{", constructor);
    QVERIFY(start >= 0);
    QVERIFY(capture > start);
    QVERIFY(validation > capture);
    QVERIFY(worksetCapture > validation);
    QVERIFY(publish > worksetCapture);
    QVERIFY(worker > publish);
    QVERIFY(run > worker);
    QVERIFY(guard > run);
    QVERIFY(loop > guard);
    QVERIFY(nextRefresh > loop);
    QVERIFY(staleCheck > nextRefresh);
    QVERIFY(constructor >= 0);
    QVERIFY(retained > constructor);
    QVERIFY(retainedWorkset > retained);
    QVERIFY(constructorBody > retainedWorkset);

    QFile headerFile(root.filePath(QStringLiteral("src/Core/RideCache.h")));
    QVERIFY(headerFile.open(QIODevice::ReadOnly));
    const QByteArray header = headerFile.readAll();
    QVERIFY(header.contains(
        "const std::shared_ptr<const RideRefreshEnvironment> environment;"));
    QVERIFY(header.contains(
        "const std::shared_ptr<const QVector<RideRefreshWorkItem>> workset;"));

    QFile itemSourceFile(root.filePath(QStringLiteral("src/Core/RideItem.cpp")));
    QVERIFY(itemSourceFile.open(QIODevice::ReadOnly));
    const QByteArray itemSource = itemSourceFile.readAll();
    const qsizetype captureInputs = itemSource.indexOf(
        "RideItem::captureRefreshInputs(");
    const qsizetype boundOverload = itemSource.indexOf(
        "RideItem::checkStale(\n"
        "    const RideRefreshEnvironment &environment,",
        captureInputs);
    const qsizetype legacyImplementation = itemSource.indexOf(
        "RideItem::checkStaleImpl(", boundOverload);
    QVERIFY(captureInputs >= 0);
    QVERIFY(boundOverload > captureInputs);
    QVERIFY(legacyImplementation > boundOverload);

    const QByteArray boundBody = itemSource.mid(
        boundOverload, legacyImplementation - boundOverload);
    QVERIFY(boundBody.contains("rideRefreshItemGateDecision("));
    QVERIFY(boundBody.contains("inputs.resolvedWeight"));
    QVERIFY(boundBody.contains("QFile file(inputs.sourcePath)"));
    QVERIFY(boundBody.contains("rideRefreshItemSourceDecision("));
    QVERIFY(boundBody.contains("RideFileCache::checkStale(cacheInputs)"));
    QVERIFY(boundBody.contains("inputs.currentMetadataCrc"));
    QVERIFY(!boundBody.contains("context->"));
    QVERIFY(!boundBody.contains("appsettings"));
    QVERIFY(!boundBody.contains("getText("));
    QVERIFY(!boundBody.contains("getWeight("));
    QVERIFY(!boundBody.contains("dateTime"));
    QVERIFY(!boundBody.contains("metadata_"));
    QVERIFY(!boundBody.contains("intervals_"));
    QVERIFY(!boundBody.contains("->zones("));
    QVERIFY(!boundBody.contains("if (!isstale)"));
    QVERIFY(!boundBody.contains("return isstale"));

    const QByteArray captureBody = itemSource.mid(
        captureInputs, boundOverload - captureInputs);
    QVERIFY(captureBody.contains("rideRefreshItemText("));
    const qsizetype ownerGuard = captureBody.indexOf(
        "rideRefreshCaptureThreadAllowed(");
    const qsizetype firstItemRead = captureBody.indexOf(
        "inputs.initiallyStale = isstale");
    QVERIFY(ownerGuard >= 0);
    QVERIFY(firstItemRead > ownerGuard);
    QVERIFY(captureBody.contains("QStringLiteral(\"Weight\")"));
    QVERIFY(captureBody.contains("metaCRC()"));
    QVERIFY(captureBody.contains("cacheStoragePathsComplete = storage.isComplete()"));
    QVERIFY(captureBody.contains("cachePathForActivity("));
    QVERIFY(captureBody.contains(
        "environment.rideFileCacheAnalysisFingerprint("));

    QFile cacheSourceFile(root.filePath(
        QStringLiteral("src/FileIO/RideFileCache.cpp")));
    QVERIFY(cacheSourceFile.open(QIODevice::ReadOnly));
    const QByteArray cacheSource = cacheSourceFile.readAll();
    const qsizetype valueCacheCheck = cacheSource.indexOf(
        "RideFileCache::checkStale(const RideFileCacheStaleInputs &inputs)");
    const qsizetype nextCacheFunction = cacheSource.indexOf(
        "\nstatic bool meanMaxBlockForSeries", valueCacheCheck);
    QVERIFY(valueCacheCheck >= 0);
    QVERIFY(nextCacheFunction > valueCacheCheck);
    const QByteArray valueCacheBody = cacheSource.mid(
        valueCacheCheck, nextCacheFunction - valueCacheCheck);
    QVERIFY(valueCacheBody.contains("!inputs.storagePathsComplete"));
    QVERIFY(valueCacheBody.contains("inputs.analysisFingerprint.size() != 32"));
    QVERIFY(valueCacheBody.contains("cacheIsCurrentForSource("));
    QVERIFY(!valueCacheBody.contains("context"));
    QVERIFY(!valueCacheBody.contains("athlete"));
    QVERIFY(!valueCacheBody.contains("appsettings"));
    QVERIFY(!valueCacheBody.contains("RideItem"));

    QFile itemHeaderFile(root.filePath(QStringLiteral("src/Core/RideItem.h")));
    QVERIFY(itemHeaderFile.open(QIODevice::ReadOnly));
    const QByteArray itemHeader = itemHeaderFile.readAll();
    QVERIFY(itemHeader.contains(
        "const RideRefreshItemStaleInputs &inputs"));
    QVERIFY(itemHeader.contains(
        "bool checkStaleImpl(const RideRefreshEnvironment *environment);"));
}

QTEST_GUILESS_MAIN(TestRideRefreshEnvironment)

#include "testRideRefreshEnvironment.moc"
