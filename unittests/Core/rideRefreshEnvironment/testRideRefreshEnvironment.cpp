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
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

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
    void backgroundRefreshRejectsMutableOwnerState();
    void mutableItemAccessPreservesWorkerOwnedStaging();
    void measuresSnapshotIsRetainedByTheGeneration();
    void routesSnapshotIsRetainedByTheGeneration();
    void zonesSnapshotIsRetainedByTheGeneration();
    void sessionPublishesWholeGenerationsAtomically();
    void publicationIsOwnerThreadOnlyAndClosedByLifecycle();
    void productionRideItemMutationInventoryIsClosed();
    void productionWorkersRetainTheirPublishedGeneration();
};

void TestRideRefreshEnvironment::
backgroundRefreshRejectsMutableOwnerState()
{
    QVERIFY(rideRefreshBackgroundBuildAllowed(false, false, false));
    QVERIFY(!rideRefreshBackgroundBuildAllowed(true, false, false));
    QVERIFY(!rideRefreshBackgroundBuildAllowed(false, true, false));
    QVERIFY(!rideRefreshBackgroundBuildAllowed(false, false, true));
    QVERIFY(!rideRefreshBackgroundBuildAllowed(true, true, true));
}

void TestRideRefreshEnvironment::
mutableItemAccessPreservesWorkerOwnedStaging()
{
    QThread cacheOwner;
    QThread worker;
    QThread otherOwner;

    QVERIFY(rideRefreshMutableItemThreadAllowed(
        &cacheOwner, &cacheOwner, &cacheOwner));
    QVERIFY(rideRefreshMutableItemThreadAllowed(
        &worker, &worker, &cacheOwner));
    QVERIFY(!rideRefreshMutableItemThreadAllowed(
        &worker, &cacheOwner, &cacheOwner));
    QVERIFY(!rideRefreshMutableItemThreadAllowed(
        &worker, &otherOwner, &cacheOwner));
    QVERIFY(!rideRefreshMutableItemThreadAllowed(
        &cacheOwner, &worker, &cacheOwner));
    QVERIFY(!rideRefreshMutableItemThreadAllowed(
        nullptr, &worker, &cacheOwner));
}

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
productionRideItemMutationInventoryIsClosed()
{
    const QDir root(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)));
    const QRegularExpression assignment(
        QStringLiteral(
            R"(->\s*(?:isdirty|isstale|isedit|path|fileName|dateTime|present|planned|sport|isBike|isRun|isSwim|isXtrain|isAero|samples|zoneRange|hrZoneRange|paceZoneRange|fingerprint|metacrc|crc|timestamp|dbversion|udbversion|weight)\s*(?<![=!<>])=(?!=))"));
    QVERIFY(assignment.isValid());
    const QRegularExpression containerMutation(
        QStringLiteral(
            R"(->\s*(?:metrics|counts|stdmeans|stdvariances|metadata|xdata|intervals)\(\)\s*(?:\.\s*(?:clear|insert|remove|resize|fill|append|prepend|push_back|push_front|erase)\s*\(|\[[^\]]+\]\s*(?<![=!<>])=(?!=)))"));
    QVERIFY(containerMutation.isValid());
    const QRegularExpression directContainerMutation(
        QStringLiteral(
            R"(->\s*(?:metrics_|count_|stdmean_|stdvariance_|metadata_|xdata_|errors_|overrides_|intervals_)\s*(?:(?<![=!<>])=(?!=)|\.\s*(?:clear|insert|remove|resize|fill|append|prepend|push_back|push_front|erase)\s*\(|\[[^\]]+\]\s*(?<![=!<>])=(?!=)))"));
    QVERIFY(directContainerMutation.isValid());
    const QRegularExpression intervalDeclaration(
        QStringLiteral(
            R"(\b(?:const\s+)?IntervalItem(?:\s+const)?\s*[*&]\s*(?:const\s+)?([A-Za-z_]\w*)|\bIntervalItem\s+([A-Za-z_]\w*)\s*(?:[;({=])|\b(?:QPointer|QSharedPointer|QScopedPointer)\s*<\s*(?:const\s+)?IntervalItem\s*>\s+([A-Za-z_]\w*))"));
    QVERIFY(intervalDeclaration.isValid());
    const QString intervalWritePattern = QStringLiteral(
        R"((?:(?:name|type|start|stop|startKM|stopKM|displaySequence|color|route|test|selected|rideInterval)\s*(?<![=!<>])=(?!=)|(?:metrics_|count_|stdmean_|stdvariance_)\s*(?:(?<![=!<>])=(?!=)|\.\s*(?:clear|insert|remove|resize|fill|append|prepend|push_back|push_front|erase)\s*\(|\[[^\]]+\]\s*(?<![=!<>])=(?!=))|(?:metrics|counts|stdmeans|stdvariances)\(\)\s*(?:\.\s*(?:clear|insert|remove|resize|fill|append|prepend|push_back|push_front|erase)\s*\(|\[[^\]]+\]\s*(?<![=!<>])=(?!=))))");
    const QRegularExpression intervalMemberMutation(
        QStringLiteral(
            R"(\bintervals_\s*(?:\.\s*(?:at|value)\s*\([^;]*\)|\[[^\]]+\])\s*->\s*%1)")
            .arg(intervalWritePattern));
    QVERIFY(intervalMemberMutation.isValid());

    QSet<QString> actual;
    QMap<QString, int> intervalWrites;
    QDirIterator files(
        root.filePath(QStringLiteral("src")),
        {QStringLiteral("*.cpp"), QStringLiteral("*.h"),
         QStringLiteral("*.y")},
        QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString absolutePath = files.next();
        if (absolutePath.endsWith(QStringLiteral("_yacc.cpp"))
            || absolutePath.endsWith(QStringLiteral("_yacc.h"))
            || absolutePath.endsWith(QStringLiteral("_lex.cpp"))) {
            continue;
        }
        QFile file(absolutePath);
        QVERIFY2(file.open(QIODevice::ReadOnly),
                 qPrintable(absolutePath));
        const QString relativePath = root.relativeFilePath(absolutePath);
        QString content = QString::fromUtf8(file.readAll());
        QString declarationContent = content;
        if (absolutePath.endsWith(QStringLiteral(".cpp"))
            || absolutePath.endsWith(QStringLiteral(".y"))) {
            QFile siblingHeader(
                QFileInfo(absolutePath).absolutePath()
                + QLatin1Char('/')
                + QFileInfo(absolutePath).completeBaseName()
                + QStringLiteral(".h"));
            if (siblingHeader.open(QIODevice::ReadOnly)) {
                declarationContent += QLatin1Char('\n')
                    + QString::fromUtf8(siblingHeader.readAll());
            }
        }
        QSet<QString> intervalIdentifiers;
        auto declarations = intervalDeclaration.globalMatch(
            declarationContent);
        while (declarations.hasNext()) {
            const QRegularExpressionMatch match = declarations.next();
            QString identifier;
            for (int capture = 1; capture <= 3; ++capture) {
                if (!match.captured(capture).isEmpty()) {
                    identifier = match.captured(capture);
                    break;
                }
            }
            if (!identifier.isEmpty()) intervalIdentifiers.insert(identifier);
        }
        QStringList escapedIntervalIdentifiers;
        for (const QString &identifier : intervalIdentifiers) {
            escapedIntervalIdentifiers.append(
                QRegularExpression::escape(identifier));
        }
        const QRegularExpression intervalMutation(
            escapedIntervalIdentifiers.isEmpty()
                ? QStringLiteral("(?!)")
                : QStringLiteral(
                    R"(\b(?:%1)\s*(?:->|\.)\s*%2)")
                    .arg(escapedIntervalIdentifiers.join(
                        QLatin1Char('|')), intervalWritePattern));
        QVERIFY(intervalMutation.isValid());
        const QStringList lines = content.split(QLatin1Char('\n'));
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            if (assignment.match(line).hasMatch()
                || containerMutation.match(line).hasMatch()
                || directContainerMutation.match(line).hasMatch()) {
                actual.insert(relativePath + QStringLiteral(":") + line);
            }
            if (intervalMutation.match(line).hasMatch()
                || intervalMemberMutation.match(line).hasMatch()) {
                const QString key =
                    relativePath + QStringLiteral(":") + line;
                intervalWrites[key] += 1;
            }
        }
    }

    // Every pointer-form write to a publication-relevant field is classified.
    // RideItem.cpp owns guarded setters, cache transaction files either mutate
    // before membership or carry an explicit revision hook, and the remaining
    // entries are non-RideItem types or deliberately unregistered temporaries.
    const QSet<QString> expected = {
        QStringLiteral("src/Charts/IntervalSummaryWindow.cpp:fake->samples = f.dataPoints().count() > 0;"),
        QStringLiteral("src/Charts/IntervalSummaryWindow.cpp:fake->intervals_.clear(); // don't accidentally wipe these!!!!"),
        QStringLiteral("src/Charts/IntervalSummaryWindow.cpp:notfake->samples = notf.dataPoints().count() > 0;"),
        QStringLiteral("src/Core/RideCache.cpp:item->isstale = disposition.keepStale;"),
        QStringLiteral("src/Core/RideCache.cpp:item->metadata_.insert("),
        QStringLiteral("src/Core/RideCacheCalendarMutations.cpp:item->dateTime = entry.targetDateTime;"),
        QStringLiteral("src/Core/RideCacheCalendarMutations.cpp:item->metadata_.insert("),
        QStringLiteral("src/Core/RideCacheCalendarMutations.cpp:item->metadata_.remove("),
        QStringLiteral("src/Core/RideCacheImport.cpp:item->path = path;"),
        QStringLiteral("src/Core/RideCacheImport.cpp:item->fileName = name;"),
        QStringLiteral("src/Core/RideCacheImport.cpp:item->planned = itemPlanned;"),
        QStringLiteral("src/Core/RideCacheImport.cpp:item->isdirty = false;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->path = canonicalPlannedRoot;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->fileName = fileName;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->dateTime = dateTime;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->planned = true;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->isdirty = false;"),
        QStringLiteral("src/Core/RideCacheRemoval.cpp:item->isstale = true;"),
        QStringLiteral("src/Core/RideCacheSnapshot.cpp:interval->metrics_ = std::move(value.metrics);"),
        QStringLiteral("src/Core/RideCacheSnapshot.cpp:interval->count_ = std::move(value.counts);"),
        QStringLiteral("src/Core/RideCacheSnapshot.cpp:interval->stdmean_ = std::move(value.stdmeans);"),
        QStringLiteral("src/Core/RideCacheSnapshot.cpp:interval->stdvariance_ = std::move(value.stdvariances);"),
        QStringLiteral("src/Core/RideItem.cpp:this->path = path;"),
        QStringLiteral("src/Core/RideItem.cpp:this->fileName = fileName;"),
        QStringLiteral("src/FileIO/FixPyScriptsDialog.cpp:pyFixScript->path = path;"),
        QStringLiteral("src/FileIO/FixPySettings.cpp:script->path = fixPath;"),
        QStringLiteral("src/FileIO/RideFile.cpp:copy->intervals_.append(new RideFileInterval(*interval));"),
        QStringLiteral("src/FileIO/RideFile.cpp:copy->xdata_.insert("),
        QStringLiteral("src/FileIO/RideFileCommand.cpp:ride->xdata().remove(name);"),
        QStringLiteral("src/FileIO/RideFileCommand.cpp:ride->xdata().insert(name, series);"),
        QStringLiteral("src/FileIO/RideFileCommand.cpp:ride->xdata().insert(series->name, series);"),
        QStringLiteral("src/FileIO/RideFileCommand.cpp:ride->xdata().remove(series->name);"),
        QStringLiteral("src/FileIO/RideFileCache.h:void    setFileName(QString fileName) { this->fileName = fileName; }"),
        QStringLiteral("src/FileIO/SrdRideFile.cpp:w->samples = (w->bytes - offset)/sample_size;"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->isRun = add.data->isRun();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->isSwim = add.data->isSwim();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->sport = add.data->sport();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->present = add.data->getTag(\"Data\", \"\");"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->samples = add.data->dataPoints().count() > 0;"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->metadata_ = add.data->tags();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->metrics_.fill(0, factory.metricCount());"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->count_.fill(0, factory.metricCount());"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->metrics_[l.value()->index()] = l.value()->value();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->count_[l.value()->index()] = l.value()->count();"),
        QStringLiteral("src/Gui/ComparePane.cpp:add.rideItem->metrics_[j] = 0.00f;"),
        QStringLiteral("src/Gui/SaveDialogs.cpp:guardedCandidate->path = currentIdentity.path;"),
        QStringLiteral("src/Gui/SaveDialogs.cpp:guardedCandidate->fileName = currentIdentity.fileName;"),
        QStringLiteral("src/Gui/SaveDialogs.cpp:guardedCandidate->planned = currentIdentity.planned;"),
        QStringLiteral("src/Metrics/RideMetric.cpp:item->metrics().resize(factory.metricCount());"),
        QStringLiteral("src/Metrics/RideMetric.cpp:spec.interval()->metrics().resize(factory.metricCount());"),
        QStringLiteral("src/Metrics/RideMetric.cpp:if (spec.interval()) spec.interval()->metrics()[m->index()] = m->value();"),
        QStringLiteral("src/Metrics/RideMetric.cpp:else item->metrics()[m->index()] = m->value();"),
        QStringLiteral("src/R/RGraphicsDevice.cpp:pDev->path = RGraphicsDevice::Path;"),
        QStringLiteral("src/Train/Fortius.cpp:this->weight = weight;"),
        QStringLiteral("src/Train/Imagic.cpp:this->weight = weight;"),
        QStringLiteral("src/Train/StravaRoutesDownloadPipeline.cpp:staging->path = staging->directory.displayPath();"),
        QStringLiteral("src/Train/TrainDB.cpp:generation->path = info.absoluteFilePath();")
    };
    QCOMPARE(actual, expected);
    // Classify each occurrence matched through explicit IntervalItem pointer,
    // reference, value, or Qt smart-pointer declarations and the known
    // intervals_.at/value/index member receivers. RideDB and IntervalSummary
    // use detached temporaries; RideItem and Route initialize new/staged
    // intervals; RideCacheSnapshot materializes a prepared snapshot behind the
    // target fence; and the GUI paths fence their live owner before the first
    // write. Internal direct-field mutators are checked below. Inferred aliases
    // and multiline/compound expressions remain outside this lexical grammar.
    const QStringList expectedIntervalWriteList = QStringLiteral(R"GC(src/Charts/IntervalSummaryWindow.cpp:temp.metrics()[i.value()->index()] = i.value()->value();
src/Charts/IntervalSummaryWindow.cpp:temp.metrics()[i.value()->index()] = i.value()->value();
src/Charts/IntervalSummaryWindow.cpp:temp.metrics().fill(0, factory.metricCount());
src/Charts/IntervalSummaryWindow.cpp:temp.metrics().fill(0, factory.metricCount());
src/Charts/IntervalSummaryWindow.cpp:temp.metrics()[j] = 0.00f;
src/Charts/IntervalSummaryWindow.cpp:temp.metrics()[j] = 0.00f;
src/Charts/IntervalSummaryWindow.cpp:temp.name = QString(tr("%1 selected intervals")).arg(intervals.count());
src/Charts/IntervalSummaryWindow.cpp:temp.name = QString(tr("Excluding %1 selected")).arg(intervals.count());
src/Charts/RideMapWindow.cpp:last->rideInterval->start = last->start = point->secs;
src/Charts/RideMapWindow.cpp:last->rideInterval->start = last->start = secondPoint->secs;
src/Charts/RideMapWindow.cpp:last->rideInterval->stop = last->stop = point->secs;
src/Charts/RideMapWindow.cpp:last->rideInterval->stop = last->stop = secondPoint->secs;
src/Charts/RideMapWindow.cpp:last->startKM = last->rideItem()->ride()->timeToDistance(last->start);
src/Charts/RideMapWindow.cpp:last->stopKM = last->rideItem()->ride()->timeToDistance(last->stop);
src/Core/RideCacheSnapshot.cpp:interval->color = std::move(value.color);
src/Core/RideCacheSnapshot.cpp:interval->count_ = std::move(value.counts);
src/Core/RideCacheSnapshot.cpp:interval->displaySequence = value.displaySequence;
src/Core/RideCacheSnapshot.cpp:interval->metrics_ = std::move(value.metrics);
src/Core/RideCacheSnapshot.cpp:interval->name = std::move(value.name);
src/Core/RideCacheSnapshot.cpp:interval->rideInterval = nullptr;
src/Core/RideCacheSnapshot.cpp:interval->rideInterval = target.ride_->intervals().at(
src/Core/RideCacheSnapshot.cpp:interval->route = std::move(value.route);
src/Core/RideCacheSnapshot.cpp:interval->selected = value.selected;
src/Core/RideCacheSnapshot.cpp:interval->start = value.start;
src/Core/RideCacheSnapshot.cpp:interval->startKM = value.startKm;
src/Core/RideCacheSnapshot.cpp:interval->stdmean_ = std::move(value.stdmeans);
src/Core/RideCacheSnapshot.cpp:interval->stdvariance_ = std::move(value.stdvariances);
src/Core/RideCacheSnapshot.cpp:interval->stop = value.stop;
src/Core/RideCacheSnapshot.cpp:interval->stopKM = value.stopKm;
src/Core/RideCacheSnapshot.cpp:interval->test = value.test;
src/Core/RideCacheSnapshot.cpp:interval->type = static_cast<RideFileInterval::IntervalType>(
src/Core/RideItem.cpp:add->rideInterval = NULL;
src/Core/RideItem.cpp:add->rideInterval = ride()->newInterval(name, start, stop, color, test);
src/Core/RideItem.cpp:entire->rideInterval = NULL;
src/Core/RideItem.cpp:foreach(IntervalItem *x, intervals()) x->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->name = QString(tr("L%1 %5 %2 (%3w %4 kJ)"))
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = NULL;
src/Core/RideItem.cpp:intervalItem->rideInterval = interval;
src/Core/RideItem.cpp:intervals_.at(index)->rideInterval = ride_->intervals().at(findex);
src/Core/Route.cpp:intervalItem->route = id();
src/Gui/AnalysisSidebar.cpp:activeInterval->color = temp.color;
src/Gui/AnalysisSidebar.cpp:activeInterval->name = temp.name;
src/Gui/AnalysisSidebar.cpp:activeInterval->start = temp.start;
src/Gui/AnalysisSidebar.cpp:activeInterval->startKM = activeInterval->rideItem()->ride()->timeToDistance(temp.start);
src/Gui/AnalysisSidebar.cpp:activeInterval->stop = temp.stop;
src/Gui/AnalysisSidebar.cpp:activeInterval->stopKM = activeInterval->rideItem()->ride()->timeToDistance(temp.stop);
src/Gui/AnalysisSidebar.cpp:activeInterval->test = temp.test;
src/Gui/AnalysisSidebar.cpp:item->rideInterval->name = item->name =
src/Gui/AnalysisSidebar.cpp:item->rideInterval->test = item->test = true;
src/Core/RideDB.y:else if ($1 == "color") jc->interval.color = QColor($3);
src/Core/RideDB.y:else if ($1 == "route") jc->interval.route = QUuid($3);
src/Core/RideDB.y:else if ($1 == "seq") jc->interval.displaySequence = $3.toInt();
src/Core/RideDB.y:else if ($1 == "start") jc->interval.start = $3.toDouble();
src/Core/RideDB.y:else if ($1 == "startKM") jc->interval.startKM = $3.toDouble();
src/Core/RideDB.y:else if ($1 == "stop") jc->interval.stop = $3.toDouble();
src/Core/RideDB.y:else if ($1 == "stopKM") jc->interval.stopKM = $3.toDouble();
src/Core/RideDB.y:else if ($1 == "test") jc->interval.test = $3 == "true" ? true : false;
src/Core/RideDB.y:else if ($1 == "type") jc->interval.type = static_cast<RideFileInterval::intervaltype>($3.toInt());
src/Core/RideDB.y:if ($1 == "name") jc->interval.name = $3;
src/Core/RideDB.y:jc->interval.counts().fill(0.0f);
src/Core/RideDB.y:jc->interval.counts().fill(0.0f);
src/Core/RideDB.y:jc->interval.counts()[m->index()] = $6.toDouble();
src/Core/RideDB.y:jc->interval.counts()[m->index()] = $6.toDouble();
src/Core/RideDB.y:jc->interval.counts()[m->index()] = 0; /* we don't write zeroes */
src/Core/RideDB.y:jc->interval.metrics().fill(0.0f);
src/Core/RideDB.y:jc->interval.metrics().fill(0.0f);
src/Core/RideDB.y:jc->interval.metrics()[m->index()] = $3.toDouble();
src/Core/RideDB.y:jc->interval.metrics()[m->index()] = $4.toDouble();
src/Core/RideDB.y:jc->interval.metrics()[m->index()] = $4.toDouble();
src/Core/RideDB.y:jc->interval.route = QUuid();
src/Core/RideDB.y:jc->interval.stdmeans().clear();
src/Core/RideDB.y:jc->interval.stdmeans().clear();
src/Core/RideDB.y:jc->interval.stdmeans().insert(m->index(), $8.toDouble());
src/Core/RideDB.y:jc->interval.stdvariances().clear();
src/Core/RideDB.y:jc->interval.stdvariances().clear();
src/Core/RideDB.y:jc->interval.stdvariances().insert(m->index(), $10.toDouble());)GC")
        .split(QLatin1Char('\n'));
    QMap<QString, int> expectedIntervalWrites;
    for (const QString &entry : expectedIntervalWriteList)
        expectedIntervalWrites[entry] += 1;
    QCOMPARE(intervalWrites, expectedIntervalWrites);
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
    const qsizetype workIndex = source.indexOf(
        "work.workIndex = mutableWorkset->size()", validation);
    const qsizetype targetToken = source.indexOf(
        "work.targetToken = ensureRefreshTarget(item)", workIndex);
    const qsizetype worksetCapture = source.indexOf(
        "work.inputs = item->captureRefreshInputs(*environment)",
        targetToken);
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
    const qsizetype backgroundGate = source.indexOf(
        "!work->inputs.backgroundRefreshAllowed", staleCheck);
    const qsizetype refreshCall = source.indexOf(
        "const bool refreshed = item->refresh()", backgroundGate);
    const qsizetype disposition = source.indexOf(
        "refreshResultDisposition(", refreshCall);
    const qsizetype refreshOutcome = source.indexOf(
        "refreshed);", disposition);
    const qsizetype constructor = source.indexOf(
        "RideCacheRefreshThread::RideCacheRefreshThread(");
    const qsizetype retained = source.indexOf(
        "environment(std::move(environment))", constructor);
    const qsizetype retainedWorkset = source.indexOf(
        "workset(std::move(workset))", retained);
    const qsizetype constructorBody = source.indexOf("\n{", constructor);
    const qsizetype refresh = source.indexOf("RideCache::refresh()");
    const qsizetype nonOwner = source.indexOf(
        "QThread::currentThread() != thread()", refresh);
    const qsizetype reservation = source.indexOf(
        "request = refreshGeneration_.request()", nonOwner);
    const qsizetype queued = source.indexOf(
        "QMetaObject::invokeMethod(", reservation);
    const qsizetype reservedHandler = source.indexOf(
        "handleRefreshRequest(request)", queued);
    const qsizetype abandon = source.indexOf(
        "abandonRequest(request)", reservedHandler);
    const qsizetype handleDefinition = source.indexOf(
        "RideCache::handleRefreshRequest(quint64 request)",
        abandon);
    const qsizetype replacementGate = source.indexOf(
        "replacementRefreshBlocked_", handleDefinition);
    const qsizetype removalGate = source.indexOf(
        "removalInProgress_", replacementGate);
    const qsizetype action = source.indexOf(
        "reservedRefreshAction(", removalGate);
    const qsizetype quiesce = source.indexOf(
        "RideCache::quiesceForConfigTransition()", action);
    const qsizetype quiesceLock = source.indexOf(
        "QMutexLocker locker(&updateMutex)", quiesce);
    const qsizetype resumeDecision = source.indexOf(
        "refreshNeedsResume(", quiesceLock);
    QVERIFY(start >= 0);
    QVERIFY(capture > start);
    QVERIFY(validation > capture);
    QVERIFY(workIndex > validation);
    QVERIFY(targetToken > workIndex);
    QVERIFY(worksetCapture > targetToken);
    QVERIFY(publish > worksetCapture);
    QVERIFY(worker > publish);
    QVERIFY(run > worker);
    QVERIFY(guard > run);
    QVERIFY(loop > guard);
    QVERIFY(nextRefresh > loop);
    QVERIFY(staleCheck > nextRefresh);
    QVERIFY(backgroundGate > staleCheck);
    QVERIFY(refreshCall > backgroundGate);
    QVERIFY(disposition > refreshCall);
    QVERIFY(refreshOutcome > disposition);
    QVERIFY(constructor >= 0);
    QVERIFY(retained > constructor);
    QVERIFY(retainedWorkset > retained);
    QVERIFY(constructorBody > retainedWorkset);
    QVERIFY(refresh >= 0);
    QVERIFY(nonOwner > refresh);
    QVERIFY(reservation > nonOwner);
    QVERIFY(queued > reservation);
    QVERIFY(reservedHandler > queued);
    QVERIFY(abandon > reservedHandler);
    QVERIFY(handleDefinition > abandon);
    QVERIFY(replacementGate > handleDefinition);
    QVERIFY(removalGate > replacementGate);
    QVERIFY(action > removalGate);
    QVERIFY(quiesce > action);
    QVERIFY(quiesceLock > quiesce);
    QVERIFY(resumeDecision > quiesceLock);
    QVERIFY(!source.mid(nonOwner, queued - nonOwner).contains(
        "&RideCache::refresh"));

    const qsizetype advanceRevision = source.indexOf(
        "RideCache::advanceRefreshTargetRevision(RideItem *item)");
    const qsizetype advanceThreadGuard = source.indexOf(
        "QThread::currentThread() != thread()", advanceRevision);
    const qsizetype advanceMembership = source.indexOf(
        "ownsLiveRide(item)", advanceThreadGuard);
    const qsizetype advanceRegistry = source.indexOf(
        "refreshTargets_->invalidateTarget(item)", advanceMembership);
    QVERIFY(advanceRevision >= 0);
    QVERIFY(advanceThreadGuard > advanceRevision);
    QVERIFY(advanceMembership > advanceThreadGuard);
    QVERIFY(advanceRegistry > advanceMembership);

    QFile headerFile(root.filePath(QStringLiteral("src/Core/RideCache.h")));
    QVERIFY(headerFile.open(QIODevice::ReadOnly));
    const QByteArray header = headerFile.readAll();
    QVERIFY(header.contains(
        "const std::shared_ptr<const RideRefreshEnvironment> environment;"));
    QVERIFY(header.contains(
        "const std::shared_ptr<const QVector<RideRefreshWorkItem>> workset;"));

    QFile inputsFile(root.filePath(
        QStringLiteral("src/Core/RideRefreshItemInputs.h")));
    QVERIFY(inputsFile.open(QIODevice::ReadOnly));
    const QByteArray inputs = inputsFile.readAll();
    QVERIFY(inputs.contains("quint64 cacheEpoch = 0;"));
    QVERIFY(inputs.contains("quint64 targetId = 0;"));
    QVERIFY(inputs.contains("quint64 revision = 0;"));
    QVERIFY(inputs.contains("RideRefreshTargetToken targetToken;"));
    QVERIFY(inputs.contains("qsizetype workIndex = -1;"));

    const QByteArray workerBody = source.mid(run);
    QVERIFY(workerBody.contains("candidate.target"));
    QVERIFY(!workerBody.contains("resolveRefreshTarget("));

    QFile removalFile(root.filePath(
        QStringLiteral("src/Core/RideCacheRemoval.cpp")));
    QVERIFY(removalFile.open(QIODevice::ReadOnly));
    const QByteArray removal = removalFile.readAll();
    QVERIFY(removal.count("retireRefreshTarget(") >= 4);

    QFile liveViewFile(root.filePath(
        QStringLiteral("src/Core/RideCacheLiveView.cpp")));
    QVERIFY(liveViewFile.open(QIODevice::ReadOnly));
    QVERIFY(liveViewFile.readAll().contains("retireRefreshTarget(item)"));

    QFile importFile(root.filePath(
        QStringLiteral("src/Core/RideCacheImport.cpp")));
    QVERIFY(importFile.open(QIODevice::ReadOnly));
    const QByteArray importSource = importFile.readAll();
    const qsizetype retireImported = importSource.indexOf(
        "RideCache::retireImportedRideItems(");
    const qsizetype importRetirement = importSource.indexOf(
        "retireRefreshTarget(item)", retireImported);
    QVERIFY(retireImported >= 0);
    QVERIFY(importRetirement > retireImported);

    QFile projectFile(root.filePath(QStringLiteral("src/src.pro")));
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    QVERIFY(projectFile.readAll().contains(
        "Core/RideRefreshTargetRegistry.h"));

    QFile itemSourceFile(root.filePath(QStringLiteral("src/Core/RideItem.cpp")));
    QVERIFY(itemSourceFile.open(QIODevice::ReadOnly));
    const QByteArray itemSource = itemSourceFile.readAll();
    const qsizetype itemDestructor = itemSource.indexOf(
        "RideItem::~RideItem()");
    const qsizetype destructorRetirement = itemSource.indexOf(
        "cache->retireRefreshTarget(this)", itemDestructor);
    QVERIFY(itemDestructor >= 0);
    QVERIFY(destructorRetirement > itemDestructor);
    const qsizetype mutationFence = itemSource.indexOf(
        "RideItem::prepareForRefreshRelevantMutation()");
    const qsizetype registrationCheck = itemSource.indexOf(
        "refreshTargetRegistered_.load", mutationFence);
    const qsizetype revisionAdvance = itemSource.indexOf(
        "advanceRefreshTargetRevision(this)", registrationCheck);
    QVERIFY(mutationFence >= 0);
    QVERIFY(registrationCheck > mutationFence);
    QVERIFY(revisionAdvance > registrationCheck);
    QVERIFY(itemSource.count("prepareForRefreshRelevantMutation()") >= 14);
    QVERIFY(itemSource.contains("RideItem::markStale()"));
    QVERIFY(itemSource.contains("RideItem::clearIntervals()"));
    const qsizetype weightGetter = itemSource.indexOf(
        "RideItem::getWeight(int type)");
    const qsizetype localWeight = itemSource.indexOf(
        "double resolvedWeight = m", weightGetter);
    const qsizetype weightFence = itemSource.indexOf(
        "prepareForRefreshRelevantMutation()", localWeight);
    const qsizetype weightWrite = itemSource.indexOf(
        "weight = resolvedWeight", weightFence);
    QVERIFY(weightGetter >= 0);
    QVERIFY(localWeight > weightGetter);
    QVERIFY(weightFence > localWeight);
    QVERIFY(weightWrite > weightFence);
    const qsizetype rideOpen = itemSource.indexOf(
        "RideFile *RideItem::ride(bool open)");
    const qsizetype openBarrier = itemSource.indexOf(
        "settleBeforeRideOpen(", rideOpen);
    const qsizetype sourceOpen = itemSource.indexOf(
        "QFile file(path + \"/\" + fileName)", openBarrier);
    const qsizetype setRide = itemSource.indexOf(
        "RideItem::setRide(RideFile *overwrite)");
    const qsizetype setRideBarrier = itemSource.indexOf(
        "settleBeforeRideOpen(", setRide);
    const qsizetype setRideAssignment = itemSource.indexOf(
        "ride_ = overwrite", setRideBarrier);
    const qsizetype setDirty = itemSource.indexOf(
        "RideItem::setDirty(bool val)");
    const qsizetype setDirtyBarrier = itemSource.indexOf(
        "settleBeforeRideOpen(", setDirty);
    const qsizetype dirtyAssignment = itemSource.indexOf(
        "isdirty = val", setDirtyBarrier);
    QVERIFY(rideOpen >= 0);
    QVERIFY(openBarrier > rideOpen);
    QVERIFY(sourceOpen > openBarrier);
    QVERIFY(setRide >= 0);
    QVERIFY(setRideBarrier > setRide);
    QVERIFY(setRideAssignment > setRideBarrier);
    QVERIFY(setDirty >= 0);
    QVERIFY(setDirtyBarrier > setDirty);
    QVERIFY(dirtyAssignment > setDirtyBarrier);
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
    const qsizetype backgroundPolicy = captureBody.indexOf(
        "rideRefreshBackgroundBuildAllowed(", ownerGuard);
    QVERIFY(backgroundPolicy > ownerGuard);
    QVERIFY(backgroundPolicy < firstItemRead);
    QVERIFY(captureBody.contains("ride_ != nullptr, isdirty, isedit"));
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
    QVERIFY(itemHeader.contains(
        "std::atomic<bool> refreshTargetRegistered_{false};"));
    QVERIFY(itemHeader.contains("bool markStale();"));
    QVERIFY(!itemHeader.contains(
        "void clearIntervals() { intervals_.clear(); }"));

    QFile calendarFile(root.filePath(
        QStringLiteral("src/Core/RideCacheCalendarMutations.cpp")));
    QVERIFY(calendarFile.open(QIODevice::ReadOnly));
    const QByteArray calendarSource = calendarFile.readAll();
    const qsizetype calendarFence = calendarSource.indexOf(
        "prepareForRefreshRelevantMutation()");
    const qsizetype calendarPublish = calendarSource.indexOf(
        "journal->publishAndCommit", calendarFence);
    const qsizetype calendarIdentityWrite = calendarSource.indexOf(
        "item->dateTime = entry.targetDateTime", calendarPublish);
    QVERIFY(calendarFence >= 0);
    QVERIFY(calendarPublish > calendarFence);
    QVERIFY(calendarIdentityWrite > calendarPublish);

    QFile snapshotFile(root.filePath(
        QStringLiteral("src/Core/RideCacheSnapshot.cpp")));
    QVERIFY(snapshotFile.open(QIODevice::ReadOnly));
    const QByteArray snapshotSource = snapshotFile.readAll();
    const qsizetype snapshotApply = snapshotSource.indexOf(
        "RideCacheItemSnapshot::applyTo(RideItem &target)");
    const qsizetype snapshotFence = snapshotSource.indexOf(
        "target.prepareForRefreshRelevantMutation()", snapshotApply);
    const qsizetype computedApply = snapshotSource.indexOf(
        "computed_.applyTo(target)", snapshotFence);
    QVERIFY(snapshotApply >= 0);
    QVERIFY(snapshotFence > snapshotApply);
    QVERIFY(computedApply > snapshotFence);
    const QList<QByteArray> computedSurface = {
        "target.metrics_", "target.count_", "target.stdmean_",
        "target.stdvariance_", "target.metadata_", "target.xdata_",
        "target.errors_", "target.intervals_", "target.zoneRange",
        "target.hrZoneRange", "target.paceZoneRange",
        "target.fingerprint", "target.metacrc", "target.crc",
        "target.timestamp", "target.dbversion", "target.udbversion",
        "target.color", "target.present", "target.sport",
        "target.isBike", "target.isRun", "target.isSwim",
        "target.isXtrain", "target.isAero", "target.weight",
        "target.overrides_", "target.samples"
    };
    for (const QByteArray &field : computedSurface)
        QVERIFY2(snapshotSource.contains(field), field.constData());

    QFile intervalFile(root.filePath(
        QStringLiteral("src/Core/IntervalItem.cpp")));
    QVERIFY(intervalFile.open(QIODevice::ReadOnly));
    const QByteArray intervalSource = intervalFile.readAll();
    const qsizetype intervalSetFrom = intervalSource.indexOf(
        "IntervalItem::setFrom");
    const qsizetype setFromFence = intervalSource.indexOf(
        "prepareForMutation()", intervalSetFrom);
    const qsizetype setFromWrite = intervalSource.indexOf(
        "*this = other", setFromFence);
    const qsizetype intervalSetValues = intervalSource.indexOf(
        "IntervalItem::setValues");
    const qsizetype setValuesFence = intervalSource.indexOf(
        "prepareForMutation()", intervalSetValues);
    const qsizetype setValuesWrite = intervalSource.indexOf(
        "this->name = name", setValuesFence);
    const qsizetype intervalRefresh = intervalSource.indexOf(
        "IntervalItem::refresh()");
    const qsizetype refreshFence = intervalSource.indexOf(
        "prepareForMutation()", intervalRefresh);
    const qsizetype refreshWrite = intervalSource.indexOf(
        "metrics_.fill", refreshFence);
    const qsizetype intervalSetSelected = intervalSource.indexOf(
        "IntervalItem::setSelected(bool value)");
    const qsizetype setSelectedFence = intervalSource.indexOf(
        "prepareForMutation()", intervalSetSelected);
    const qsizetype setSelectedWrite = intervalSource.indexOf(
        "selected = value", setSelectedFence);
    QVERIFY(intervalSetFrom >= 0);
    QVERIFY(setFromFence > intervalSetFrom);
    QVERIFY(setFromWrite > setFromFence);
    QVERIFY(setFromWrite < intervalSetValues);
    QVERIFY(intervalSetValues >= 0);
    QVERIFY(setValuesFence > intervalSetValues);
    QVERIFY(setValuesWrite > setValuesFence);
    QVERIFY(setValuesWrite < intervalSetSelected);
    QVERIFY(intervalSetSelected >= 0);
    QVERIFY(setSelectedFence > intervalSetSelected);
    QVERIFY(setSelectedWrite > setSelectedFence);
    QVERIFY(setSelectedWrite < intervalRefresh);
    QVERIFY(intervalRefresh >= 0);
    QVERIFY(refreshFence > intervalRefresh);
    QVERIFY(refreshWrite > refreshFence);

    QFile intervalHeaderFile(root.filePath(
        QStringLiteral("src/Core/IntervalItem.h")));
    QVERIFY(intervalHeaderFile.open(QIODevice::ReadOnly));
    const QByteArray intervalHeader = intervalHeaderFile.readAll();
    const qsizetype displaySetter = intervalHeader.indexOf(
        "void setDisplaySequence(int seq)");
    const qsizetype displayFence = intervalHeader.indexOf(
        "prepareForMutation()", displaySetter);
    const qsizetype displayWrite = intervalHeader.indexOf(
        "displaySequence = seq", displayFence);
    QVERIFY(displaySetter >= 0);
    QVERIFY(displayFence > displaySetter);
    QVERIFY(displayWrite > displayFence);

    QFile metricFile(root.filePath(
        QStringLiteral("src/Metrics/RideMetric.cpp")));
    QVERIFY(metricFile.open(QIODevice::ReadOnly));
    const QByteArray metricSource = metricFile.readAll();
    const qsizetype metricFence = metricSource.indexOf(
        "item->prepareForRefreshRelevantMutation()");
    const qsizetype intervalResize = metricSource.indexOf(
        "spec.interval()->metrics().resize", metricFence);
    const qsizetype userMetricFence = metricSource.indexOf(
        "if (hasUserMetrics", intervalResize);
    const qsizetype intervalMetricWrite = metricSource.indexOf(
        "spec.interval()->metrics()[m->index()] = m->value()",
        userMetricFence);
    QVERIFY(metricFence >= 0);
    QVERIFY(intervalResize > metricFence);
    QVERIFY(userMetricFence > intervalResize);
    QVERIFY(intervalMetricWrite > userMetricFence);

    QFile sidebarFile(root.filePath(
        QStringLiteral("src/Gui/AnalysisSidebar.cpp")));
    QVERIFY(sidebarFile.open(QIODevice::ReadOnly));
    const QByteArray sidebarSource = sidebarFile.readAll();
    QVERIFY(sidebarSource.contains("ride->markStale()"));
    QVERIFY(!sidebarSource.contains("->selected ="));
    const qsizetype performanceInterval = sidebarSource.indexOf(
        "AnalysisSidebar::perfTestIntervalSelected()");
    const qsizetype performanceFence = sidebarSource.indexOf(
        "item->prepareForMutation()", performanceInterval);
    const qsizetype performanceWrite = sidebarSource.indexOf(
        "item->rideInterval->test", performanceFence);
    const qsizetype renameIntervals = sidebarSource.indexOf(
        "AnalysisSidebar::renameIntervalsSelected()");
    const qsizetype renameFence = sidebarSource.indexOf(
        "item->prepareForMutation()", renameIntervals);
    const qsizetype renameWrite = sidebarSource.indexOf(
        "item->rideInterval->name", renameFence);
    QVERIFY(performanceInterval >= 0);
    QVERIFY(performanceFence > performanceInterval);
    QVERIFY(performanceWrite > performanceFence);
    QVERIFY(renameIntervals >= 0);
    QVERIFY(renameFence > renameIntervals);
    QVERIFY(renameWrite > renameFence);
    const qsizetype editInterval = sidebarSource.indexOf(
        "AnalysisSidebar::editInterval()");
    const qsizetype editFence = sidebarSource.indexOf(
        "activeInterval->prepareForMutation()", editInterval);
    const qsizetype editWrite = sidebarSource.indexOf(
        "activeInterval->name = temp.name", editFence);
    QVERIFY(editInterval >= 0);
    QVERIFY(editFence > editInterval);
    QVERIFY(editWrite > editFence);

    QFile mapFile(root.filePath(
        QStringLiteral("src/Charts/RideMapWindow.cpp")));
    QVERIFY(mapFile.open(QIODevice::ReadOnly));
    const QByteArray mapSource = mapFile.readAll();
    QVERIFY(!mapSource.contains("->selected ="));
    const qsizetype mapFence = mapSource.indexOf(
        "last->prepareForMutation()");
    const qsizetype mapWrite = mapSource.indexOf(
        "last->rideInterval->start", mapFence);
    QVERIFY(mapFence >= 0);
    QVERIFY(mapWrite > mapFence);

    QFile plotIntervalFile(root.filePath(
        QStringLiteral("src/Charts/AllPlotInterval.cpp")));
    QVERIFY(plotIntervalFile.open(QIODevice::ReadOnly));
    const QByteArray plotIntervalSource = plotIntervalFile.readAll();
    QVERIFY(!plotIntervalSource.contains("interval->selected ="));
    QVERIFY(plotIntervalSource.contains("interval->setSelected("));

    QFile plotWindowFile(root.filePath(
        QStringLiteral("src/Charts/AllPlotWindow.cpp")));
    QVERIFY(plotWindowFile.open(QIODevice::ReadOnly));
    const QByteArray plotWindowSource = plotWindowFile.readAll();
    QVERIFY(!plotWindowSource.contains("interval->selected ="));
    QVERIFY(plotWindowSource.contains("interval->setSelected(true)"));

    QFile swimFixFile(root.filePath(
        QStringLiteral("src/FileIO/FixLapSwim.cpp")));
    QVERIFY(swimFixFile.open(QIODevice::ReadOnly));
    QVERIFY(swimFixFile.readAll().contains(
        "ride->context->rideItem()->markStale()"));
}

QTEST_GUILESS_MAIN(TestRideRefreshEnvironment)

#include "testRideRefreshEnvironment.moc"
