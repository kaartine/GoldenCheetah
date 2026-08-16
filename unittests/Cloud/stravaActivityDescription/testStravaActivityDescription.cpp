#include "Cloud/StravaActivityDescription.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <limits>

class TestStravaActivityDescription : public QObject
{
    Q_OBJECT

private slots:
    void parsesModesWithoutChangingLegacyDefault();
    void composesNotesAndSummary();
    void omitsNotesUsedAsActivityName();
    void wrapsAutomaticSummaryInManagedBlock();
    void appendsManagedSummaryWithoutLosingRemoteDescription();
    void replacesManagedSummaryIdempotently();
    void preservesTextAfterManagedSummary();
    void preservesRemoteDescriptionWhitespace();
    void migratesUnmarkedAutomaticSummary();
    void preparesDescriptionUpdateFromStravaActivity();
    void rejectsInvalidStravaActivityResponses();
    void summarizesRecordedMetrics();
    void omitsUnavailableChannels();
    void rendersPowerZonesAndBoundedSparkline();
    void groupsRepeatedWorkIntervals();
    void ignoresInvalidValues();
};

void TestStravaActivityDescription::parsesModesWithoutChangingLegacyDefault()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::modeFromSetting(QString()),
             Description::Mode::NotesOnly);
    QCOMPARE(Description::modeFromSetting(QStringLiteral("unknown")),
             Description::Mode::NotesOnly);
    QCOMPARE(Description::modeFromSetting(
                 QStringLiteral("Automatic summary")),
             Description::Mode::SummaryOnly);
    QCOMPARE(Description::modeFromSetting(
                 QStringLiteral("Notes + automatic summary")),
             Description::Mode::NotesAndSummary);
}

void TestStravaActivityDescription::composesNotesAndSummary()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::compose(
                 QStringLiteral("Felt controlled"), false,
                 Description::Mode::NotesAndSummary,
                 QStringLiteral("Duration 45:00")),
             QStringLiteral("Felt controlled\n\nDuration 45:00"));
    QCOMPARE(Description::compose(
                 QStringLiteral("Felt controlled"), false,
                 Description::Mode::NotesOnly,
                 QStringLiteral("Duration 45:00")),
             QStringLiteral("Felt controlled"));
    QCOMPARE(Description::compose(
                 QStringLiteral("Felt controlled"), false,
                 Description::Mode::SummaryOnly,
                 QStringLiteral("Duration 45:00")),
             QStringLiteral("Duration 45:00"));
}

void TestStravaActivityDescription::omitsNotesUsedAsActivityName()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::compose(
                 QStringLiteral("Workout title"), true,
                 Description::Mode::NotesAndSummary,
                 QStringLiteral("Duration 45:00")),
             QStringLiteral("Duration 45:00"));
    QVERIFY(Description::compose(
                QStringLiteral("Workout title"), true,
                Description::Mode::NotesOnly,
                QStringLiteral("Duration 45:00")).isEmpty());
}

void TestStravaActivityDescription::wrapsAutomaticSummaryInManagedBlock()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::managedSummaryBlock(QStringLiteral("Duration 45:00")),
             QStringLiteral("--- GoldenCheetah summary ---\n"
                            "Duration 45:00\n"
                            "--- /GoldenCheetah summary ---"));
    QVERIFY(Description::managedSummaryBlock(QStringLiteral("  \n")).isEmpty());
}

void TestStravaActivityDescription::appendsManagedSummaryWithoutLosingRemoteDescription()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::mergeManagedSummary(
                 QStringLiteral("Windy ride. Felt good."),
                 QStringLiteral("Duration 45:00")),
             QStringLiteral("Windy ride. Felt good.\n\n"
                            "--- GoldenCheetah summary ---\n"
                            "Duration 45:00\n"
                            "--- /GoldenCheetah summary ---"));
}

void TestStravaActivityDescription::replacesManagedSummaryIdempotently()
{
    using Description = StravaActivityDescription;
    const QString first = Description::mergeManagedSummary(
        QStringLiteral("Original description"),
        QStringLiteral("Duration 45:00"));
    QCOMPARE(Description::mergeManagedSummary(
                 first, QStringLiteral("Duration 45:00")), first);

    const QString updated = Description::mergeManagedSummary(
        first, QStringLiteral("Duration 46:00"));
    QVERIFY(updated.startsWith(QStringLiteral("Original description\n\n")));
    QVERIFY(updated.contains(QStringLiteral("Duration 46:00")));
    QVERIFY(!updated.contains(QStringLiteral("Duration 45:00")));
    QCOMPARE(updated.count(QStringLiteral("--- GoldenCheetah summary ---")), 1);
}

void TestStravaActivityDescription::preservesTextAfterManagedSummary()
{
    using Description = StravaActivityDescription;
    const QString existing = QStringLiteral(
        "Before\n\n"
        "--- GoldenCheetah summary ---\n"
        "Duration 40:00\n"
        "--- /GoldenCheetah summary ---\n\n"
        "Added later in Strava");
    QCOMPARE(Description::mergeManagedSummary(
                 existing, QStringLiteral("Duration 45:00")),
             QStringLiteral(
                 "Before\n\n"
                 "--- GoldenCheetah summary ---\n"
                 "Duration 45:00\n"
                 "--- /GoldenCheetah summary ---\n\n"
                 "Added later in Strava"));
}

void TestStravaActivityDescription::preservesRemoteDescriptionWhitespace()
{
    using Description = StravaActivityDescription;
    const QString existing = QStringLiteral(
        "  Before  \n\n"
        "--- GoldenCheetah summary ---\n"
        "Duration 40:00\n"
        "--- /GoldenCheetah summary ---\n\n"
        "  Added later in Strava  \n");
    QCOMPARE(Description::mergeManagedSummary(
                 existing, QStringLiteral("Duration 45:00")),
             QStringLiteral(
                 "  Before  \n\n"
                 "--- GoldenCheetah summary ---\n"
                 "Duration 45:00\n"
                 "--- /GoldenCheetah summary ---\n\n"
                 "  Added later in Strava  \n"));

    QCOMPARE(Description::mergeManagedSummary(
                 QStringLiteral("  User text  \n"),
                 QStringLiteral("Duration 45:00")),
             QStringLiteral(
                 "  User text  \n\n"
                 "--- GoldenCheetah summary ---\n"
                 "Duration 45:00\n"
                 "--- /GoldenCheetah summary ---"));
}

void TestStravaActivityDescription::migratesUnmarkedAutomaticSummary()
{
    using Description = StravaActivityDescription;
    QCOMPARE(Description::mergeManagedSummary(
                 QStringLiteral("User text\n\nDuration 45:00"),
                 QStringLiteral("Duration 45:00")),
             QStringLiteral(
                 "User text\n\n"
                 "--- GoldenCheetah summary ---\n"
                 "Duration 45:00\n"
                 "--- /GoldenCheetah summary ---"));
}

void TestStravaActivityDescription::preparesDescriptionUpdateFromStravaActivity()
{
    using Description = StravaActivityDescription;
    const QByteArray response = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), 123.0},
        {QStringLiteral("description"), QStringLiteral("Existing text")}
    }).toJson(QJsonDocument::Compact);

    const Description::RemoteUpdate update =
        Description::prepareRemoteUpdate(
            response, QStringLiteral("Duration 45:00"));
    QVERIFY2(update.valid, qPrintable(update.error));
    QVERIFY(update.changed);
    const QJsonDocument request = QJsonDocument::fromJson(update.requestBody);
    QCOMPARE(request.object().size(), 1);
    QCOMPARE(request.object().value(QStringLiteral("description")).toString(),
             Description::mergeManagedSummary(
                 QStringLiteral("Existing text"),
                 QStringLiteral("Duration 45:00")));

    const Description::RemoteUpdate unchanged =
        Description::prepareRemoteUpdate(
            QJsonDocument(QJsonObject{
                {QStringLiteral("description"),
                 request.object().value(QStringLiteral("description"))}
            }).toJson(QJsonDocument::Compact),
            QStringLiteral("Duration 45:00"));
    QVERIFY2(unchanged.valid, qPrintable(unchanged.error));
    QVERIFY(!unchanged.changed);
    QVERIFY(unchanged.requestBody.isEmpty());
}

void TestStravaActivityDescription::rejectsInvalidStravaActivityResponses()
{
    using Description = StravaActivityDescription;
    QVERIFY(!Description::prepareRemoteUpdate(
                 QByteArrayLiteral("not json"),
                 QStringLiteral("Duration 45:00")).valid);
    QVERIFY(!Description::prepareRemoteUpdate(
                 QByteArrayLiteral("[]"),
                 QStringLiteral("Duration 45:00")).valid);
    QVERIFY(!Description::prepareRemoteUpdate(
                 QByteArrayLiteral("{\"description\":42}"),
                 QStringLiteral("Duration 45:00")).valid);

    const Description::RemoteUpdate nullDescription =
        Description::prepareRemoteUpdate(
            QByteArrayLiteral("{\"description\":null}"),
            QStringLiteral("Duration 45:00"));
    QVERIFY2(nullDescription.valid, qPrintable(nullDescription.error));
    QVERIFY(nullDescription.changed);
}

void TestStravaActivityDescription::summarizesRecordedMetrics()
{
    StravaActivityDescription::Input input;
    input.hasPower = true;
    input.hasHeartRate = true;
    input.hasCadence = true;
    input.recordingInterval = 1.0;
    input.ftp = 200.0;
    for (int second = 0; second < 60; ++second) {
        input.samples.append({
            static_cast<double>(second),
            second < 30 ? 100.0 : 200.0,
            second < 30 ? 120.0 : 160.0,
            90.0
        });
    }

    const QString summary = StravaActivityDescription::summary(input);
    QVERIFY2(summary.contains(QStringLiteral("Duration 1:00")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("Work 9 kJ")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("150 W avg")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("200 W max")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("140 bpm avg")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("160 bpm max")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("Cadence 90 rpm")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("IF ")),
             qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("BikeStress ")),
             qPrintable(summary));
}

void TestStravaActivityDescription::omitsUnavailableChannels()
{
    StravaActivityDescription::Input input;
    input.samples.append({0.0, 300.0, 170.0, 95.0});
    input.samples.append({1.0, 300.0, 170.0, 95.0});

    const QString summary = StravaActivityDescription::summary(input);
    QCOMPARE(summary, QStringLiteral("Duration 0:02"));
}

void TestStravaActivityDescription::rendersPowerZonesAndBoundedSparkline()
{
    StravaActivityDescription::Input input;
    input.hasPower = true;
    input.recordingInterval = 1.0;
    input.powerZones = {
        {QStringLiteral("Z1"), 120.0},
        {QStringLiteral("Z2"), 180.0},
        {QStringLiteral("Z3"), 240.0}
    };
    for (int second = 0; second < 96; ++second) {
        input.samples.append({
            static_cast<double>(second),
            second < 32 ? 100.0 : (second < 64 ? 150.0 : 220.0),
            0.0, 0.0
        });
    }

    const QString summary = StravaActivityDescription::summary(input);
    QVERIFY2(summary.contains(
                 QStringLiteral("Zones Z1 32s | Z2 32s | Z3 32s")),
             qPrintable(summary));
    const QString powerLine = summary.split(QLatin1Char('\n')).constLast();
    QVERIFY(powerLine.startsWith(QStringLiteral("Zones ")));
    const QStringList lines = summary.split(QLatin1Char('\n'));
    const auto sparklineLine = std::find_if(
        lines.cbegin(), lines.cend(), [](const QString &line) {
            return line.startsWith(QStringLiteral("Power "))
                && !line.contains(QStringLiteral(" W "));
        });
    QVERIFY(sparklineLine != lines.cend());
    QCOMPARE(sparklineLine->size() - QStringLiteral("Power ").size(), 32);
}

void TestStravaActivityDescription::groupsRepeatedWorkIntervals()
{
    StravaActivityDescription::Input input;
    input.hasPower = true;
    input.recordingInterval = 1.0;
    for (int second = 0; second < 720; ++second) {
        double power = 100.0;
        if ((second >= 60 && second <= 179)
            || (second >= 240 && second <= 359)
            || (second >= 420 && second <= 539)) {
            power = 220.0;
        }
        input.samples.append({static_cast<double>(second), power, 0.0, 0.0});
    }
    input.intervals = {
        {60.0, 179.0, QStringLiteral("Work 1")},
        {240.0, 359.0, QStringLiteral("Work 2")},
        {420.0, 539.0, QStringLiteral("Work 3")}
    };

    const QString summary = StravaActivityDescription::summary(input);
    QVERIFY2(summary.contains(
                 QStringLiteral("Intervals 3 x 1:59 @ 220 W")),
             qPrintable(summary));
}

void TestStravaActivityDescription::ignoresInvalidValues()
{
    StravaActivityDescription::Input input;
    input.hasPower = true;
    input.hasHeartRate = true;
    input.recordingInterval = std::numeric_limits<double>::quiet_NaN();
    input.samples = {
        {0.0, std::numeric_limits<double>::quiet_NaN(), -1.0, 0.0},
        {1.0, -100.0, std::numeric_limits<double>::infinity(), 0.0}
    };

    const QString summary = StravaActivityDescription::summary(input);
    QCOMPARE(summary, QStringLiteral("Duration 0:02"));
    QVERIFY(!summary.contains(QStringLiteral("nan"), Qt::CaseInsensitive));
    QVERIFY(!summary.contains(QStringLiteral("inf"), Qt::CaseInsensitive));
}

QTEST_APPLESS_MAIN(TestStravaActivityDescription)
#include "testStravaActivityDescription.moc"
