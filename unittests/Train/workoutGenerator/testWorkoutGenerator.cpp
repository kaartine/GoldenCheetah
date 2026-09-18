/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGenerator.h"

#include <QSet>
#include <QTest>

#include <cmath>
#include <limits>

class TestWorkoutGenerator : public QObject
{
    Q_OBJECT

private slots:
    void everyFocusHasDistinctValidDefaults()
    {
        QSet<QString> signatures;
        for (WorkoutTrainingFocus focus : WorkoutGenerator::focuses()) {
            const WorkoutGenerationSettings settings =
                    WorkoutGenerator::defaultsFor(focus);
            QCOMPARE(settings.focus, focus);

            const WorkoutGenerationResult result =
                    WorkoutGenerator::generate(settings);
            QCOMPARE(result.status, WorkoutGenerationStatus::Ready);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(!result.intervals.empty());
            QVERIFY(result.summary.durationSeconds >= 20 * 60);
            QVERIFY(result.summary.averagePercentFtp >= 30.0);
            QVERIFY(result.summary.estimatedStress > 0.0);

            const QString signature = QStringLiteral("%1:%2:%3:%4:%5")
                    .arg(settings.workPercentFtp)
                    .arg(settings.workSeconds)
                    .arg(settings.repetitionsPerBlock)
                    .arg(settings.blockCount)
                    .arg(settings.repetitionDeltaPerBlock);
            QVERIFY2(!signatures.contains(signature),
                     qPrintable(WorkoutGenerator::focusName(focus)));
            signatures.insert(signature);
        }
        QCOMPARE(signatures.size(), WorkoutGenerator::focuses().size());
    }

    void pamPresetReproducesDescendingTwentyTwentyBlocks()
    {
        const WorkoutGenerationSettings settings =
                WorkoutGenerator::defaultsFor(
                    WorkoutTrainingFocus::AnaerobicCapacity20_20);
        QCOMPARE(settings.workSeconds, 20);
        QCOMPARE(settings.recoverySeconds, 20);
        QCOMPARE(settings.blockCount, 4);
        QCOMPARE(settings.repetitionsPerBlock, 14);
        QCOMPARE(settings.repetitionDeltaPerBlock, -2);
        QCOMPARE(settings.blockRecoverySeconds, 4 * 60);
        QCOMPARE(settings.lastBlockRecoverySeconds, 3 * 60);

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QCOMPARE(result.status, WorkoutGenerationStatus::Ready);
        QCOMPARE(result.summary.repetitionsByBlock,
                 QVector<int>({14, 12, 10, 8}));
        QCOMPARE(result.summary.workIntervalCount, 44);
        QCOMPARE(result.summary.durationSeconds, 53 * 60 + 20);
        QCOMPARE(result.intervals.size(), std::size_t(95));
        QVector<int> blockRecoveries;
        for (const WorkoutGeneratedInterval &interval : result.intervals) {
            if (interval.role == WorkoutGeneratedIntervalRole::BlockRecovery) {
                blockRecoveries.append(interval.durationSeconds);
            }
        }
        QCOMPARE(blockRecoveries, QVector<int>({240, 240, 180}));
        QVERIFY(std::abs(result.summary.averagePercentFtp - 77.6875) < 0.001);
        QVERIFY(std::abs(result.summary.estimatedStress - 63.25556) < 0.001);
    }

    void customSettingsDriveTheGeneratedStructure()
    {
        WorkoutGenerationSettings settings = WorkoutGenerator::defaultsFor(
                WorkoutTrainingFocus::Vo2Max);
        settings.ftpWatts = 190;
        settings.warmupSeconds = 8 * 60;
        settings.cooldownSeconds = 7 * 60;
        settings.primerSeconds = 0;
        settings.preWorkRecoverySeconds = 0;
        settings.workPercentFtp = 118;
        settings.recoveryPercentFtp = 52;
        settings.workSeconds = 150;
        settings.recoverySeconds = 120;
        settings.repetitionsPerBlock = 4;
        settings.blockCount = 2;
        settings.repetitionDeltaPerBlock = -1;
        settings.blockRecoverySeconds = 5 * 60;
        settings.lastBlockRecoverySeconds = 5 * 60;
        settings.includeRecoveryAfterLastRep = true;

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QCOMPARE(result.status, WorkoutGenerationStatus::Ready);
        QCOMPARE(result.summary.repetitionsByBlock, QVector<int>({4, 3}));
        QCOMPARE(result.summary.workIntervalCount, 7);
        QCOMPARE(result.summary.durationSeconds,
                 8 * 60 + 7 * (150 + 120) + 5 * 60 + 7 * 60);
        QCOMPARE(result.summary.ftpWatts, 190);
        QCOMPARE(result.intervals.front().role,
                 WorkoutGeneratedIntervalRole::Warmup);
        QCOMPARE(result.intervals.back().role,
                 WorkoutGeneratedIntervalRole::Cooldown);
    }

    void serializesContinuousMrcCourseData()
    {
        WorkoutGenerationSettings settings = WorkoutGenerator::defaultsFor(
                WorkoutTrainingFocus::Endurance);
        settings.warmupSeconds = 60;
        settings.cooldownSeconds = 60;
        settings.workSeconds = 120;

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QCOMPARE(result.status, WorkoutGenerationStatus::Ready);

        const QByteArray data = WorkoutGenerator::mrcCourseData(result);
        QVERIFY(data.startsWith("[COURSE DATA]\n"));
        QVERIFY(data.endsWith("[END COURSE DATA]\n"));
        QVERIFY(data.contains("0.0000 "));
        QVERIFY(data.contains("4.0000 "));
        QCOMPARE(data.count("[COURSE DATA]"), 1);
        QCOMPARE(data.count("[END COURSE DATA]"), 1);
    }

    void rejectsInvalidAndUnboundedSettings_data()
    {
        QTest::addColumn<QString>("field");
        QTest::newRow("ftp") << QStringLiteral("ftp");
        QTest::newRow("work-percent") << QStringLiteral("workPercent");
        QTest::newRow("recovery-percent") << QStringLiteral("recoveryPercent");
        QTest::newRow("work-duration") << QStringLiteral("workSeconds");
        QTest::newRow("recovery-duration") << QStringLiteral("recoverySeconds");
        QTest::newRow("repetitions") << QStringLiteral("repetitions");
        QTest::newRow("negative-final-block") << QStringLiteral("delta");
        QTest::newRow("too-many-intervals") << QStringLiteral("many");
        QTest::newRow("too-long") << QStringLiteral("long");
    }

    void rejectsInvalidAndUnboundedSettings()
    {
        QFETCH(QString, field);
        WorkoutGenerationSettings settings = WorkoutGenerator::defaultsFor(
                WorkoutTrainingFocus::Threshold);
        if (field == QStringLiteral("ftp")) settings.ftpWatts = 0;
        if (field == QStringLiteral("workPercent")) settings.workPercentFtp = 251;
        if (field == QStringLiteral("recoveryPercent")) settings.recoveryPercentFtp = 5;
        if (field == QStringLiteral("workSeconds")) settings.workSeconds = 0;
        if (field == QStringLiteral("recoverySeconds")) settings.recoverySeconds = -1;
        if (field == QStringLiteral("repetitions")) settings.repetitionsPerBlock = 0;
        if (field == QStringLiteral("delta")) {
            settings.blockCount = 3;
            settings.repetitionsPerBlock = 2;
            settings.repetitionDeltaPerBlock = -1;
        }
        if (field == QStringLiteral("many")) {
            settings.blockCount = 100;
            settings.repetitionsPerBlock = 100;
        }
        if (field == QStringLiteral("long")) {
            settings.workSeconds = 7200;
            settings.repetitionsPerBlock = 4;
            settings.blockCount = 2;
        }

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QVERIFY(result.status != WorkoutGenerationStatus::Ready);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.intervals.empty());
        QCOMPARE(result.summary.durationSeconds, 0);
    }

    void rejectsEveryValidatedFieldOutsideItsBoundary_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<double>("value");

        QTest::newRow("ftp-low") << QStringLiteral("ftp") << 49.0;
        QTest::newRow("ftp-high") << QStringLiteral("ftp") << 601.0;
        const QStringList percentFields = {
            QStringLiteral("warmupStart"),
            QStringLiteral("warmupEnd"),
            QStringLiteral("primerPercent"),
            QStringLiteral("workPercent"),
            QStringLiteral("recoveryPercent"),
            QStringLiteral("cooldownStart"),
            QStringLiteral("cooldownEnd")
        };
        for (const QString &field : percentFields) {
            QTest::newRow(qPrintable(field + QStringLiteral("-low")))
                    << field << 19.99;
            QTest::newRow(qPrintable(field + QStringLiteral("-high")))
                    << field << 250.01;
        }
        QTest::newRow("percent-nan")
                << QStringLiteral("workPercent")
                << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("percent-infinity")
                << QStringLiteral("recoveryPercent")
                << std::numeric_limits<double>::infinity();

        const QList<QPair<QString, int>> durations = {
            {QStringLiteral("warmupSeconds"), 3600},
            {QStringLiteral("primerSeconds"), 1200},
            {QStringLiteral("preWorkRecoverySeconds"), 1800},
            {QStringLiteral("recoverySeconds"), 3600},
            {QStringLiteral("blockRecoverySeconds"), 3600},
            {QStringLiteral("lastBlockRecoverySeconds"), 3600},
            {QStringLiteral("cooldownSeconds"), 3600}
        };
        for (const auto &duration : durations) {
            QTest::newRow(qPrintable(duration.first + QStringLiteral("-low")))
                    << duration.first << -1.0;
            QTest::newRow(qPrintable(duration.first + QStringLiteral("-high")))
                    << duration.first << double(duration.second + 1);
        }
        QTest::newRow("work-seconds-low")
                << QStringLiteral("workSeconds") << 4.0;
        QTest::newRow("work-seconds-high")
                << QStringLiteral("workSeconds") << 7201.0;
        QTest::newRow("repetitions-low")
                << QStringLiteral("repetitions") << 0.0;
        QTest::newRow("repetitions-high")
                << QStringLiteral("repetitions") << 101.0;
        QTest::newRow("sets-low") << QStringLiteral("sets") << 0.0;
        QTest::newRow("sets-high") << QStringLiteral("sets") << 21.0;
    }

    void rejectsEveryValidatedFieldOutsideItsBoundary()
    {
        QFETCH(QString, field);
        QFETCH(double, value);
        WorkoutGenerationSettings settings = WorkoutGenerator::defaultsFor(
                WorkoutTrainingFocus::Threshold);

        if (field == QStringLiteral("ftp")) settings.ftpWatts = int(value);
        else if (field == QStringLiteral("warmupStart"))
            settings.warmupStartPercentFtp = value;
        else if (field == QStringLiteral("warmupEnd"))
            settings.warmupEndPercentFtp = value;
        else if (field == QStringLiteral("primerPercent"))
            settings.primerPercentFtp = value;
        else if (field == QStringLiteral("workPercent"))
            settings.workPercentFtp = value;
        else if (field == QStringLiteral("recoveryPercent"))
            settings.recoveryPercentFtp = value;
        else if (field == QStringLiteral("cooldownStart"))
            settings.cooldownStartPercentFtp = value;
        else if (field == QStringLiteral("cooldownEnd"))
            settings.cooldownEndPercentFtp = value;
        else if (field == QStringLiteral("warmupSeconds"))
            settings.warmupSeconds = int(value);
        else if (field == QStringLiteral("primerSeconds"))
            settings.primerSeconds = int(value);
        else if (field == QStringLiteral("preWorkRecoverySeconds"))
            settings.preWorkRecoverySeconds = int(value);
        else if (field == QStringLiteral("workSeconds"))
            settings.workSeconds = int(value);
        else if (field == QStringLiteral("recoverySeconds"))
            settings.recoverySeconds = int(value);
        else if (field == QStringLiteral("blockRecoverySeconds"))
            settings.blockRecoverySeconds = int(value);
        else if (field == QStringLiteral("lastBlockRecoverySeconds"))
            settings.lastBlockRecoverySeconds = int(value);
        else if (field == QStringLiteral("cooldownSeconds"))
            settings.cooldownSeconds = int(value);
        else if (field == QStringLiteral("repetitions"))
            settings.repetitionsPerBlock = int(value);
        else if (field == QStringLiteral("sets"))
            settings.blockCount = int(value);
        else QFAIL("Unknown boundary-test field");

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QVERIFY(result.status != WorkoutGenerationStatus::Ready);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.intervals.empty());
        QCOMPARE(result.summary.durationSeconds, 0);
    }

    void rejectsUnknownFocusWithoutPartialOutput()
    {
        WorkoutGenerationSettings settings = WorkoutGenerator::defaultsFor(
                WorkoutTrainingFocus::Endurance);
        settings.focus = static_cast<WorkoutTrainingFocus>(999);

        const WorkoutGenerationResult result =
                WorkoutGenerator::generate(settings);
        QCOMPARE(result.status, WorkoutGenerationStatus::InvalidSettings);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.intervals.empty());
        QCOMPARE(result.summary.durationSeconds, 0);
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGenerator)

#include "testWorkoutGenerator.moc"
