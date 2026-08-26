/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/TrainingDataGenerator.h"
#include "Train/TrainingDeviceWizardRouting.h"
#include "Train/TrainingDataGeneratorTargetRouting.h"
#include "Train/DeviceTypes.h"
#include "Train/RealtimeData.h"

#include <QTest>

#include <cmath>
#include <cstring>
#include <limits>

class TestTrainingDataGenerator : public QObject
{
    Q_OBJECT

private slots:
    void exposesGeneratorAsARealtimeDevice()
    {
        const DeviceTypes deviceTypes;
        bool found = false;
        for (const DeviceType &device : deviceTypes.Supported) {
            if (device.type != DEV_NULL) continue;

            found = true;
            QCOMPARE(QString::fromLatin1(device.name),
                     QStringLiteral("Data Generator"));
            QVERIFY(device.realtime);
            QVERIFY(!device.download);
        }
        QVERIFY(found);
        QVERIFY(TrainingDataGenerator::acceptsTargetPowerCommands());
    }

    void generatorSkipsTheVirtualPowerWizardPage()
    {
        using namespace TrainingDeviceWizardRouting;

        QCOMPARE(pageAfterSuccessfulScan(DEV_NULL), ConfirmationPage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_CT), VirtualPowerPage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_ANTLOCAL), AntPairingPage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_BT40), BluetoothPairingPage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_BT40_HEARTRATE),
                 BluetoothPairingPage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_FORTIUS), FortiusFirmwarePage);
        QCOMPARE(pageAfterSuccessfulScan(DEV_IMAGIC), ImagicFirmwarePage);
    }

    void producesDeterministicTelemetry()
    {
        TrainingDataGenerator first;
        TrainingDataGenerator second;

        for (int i = 0; i < 32; ++i) {
            const TrainingDataGeneratorSample a = first.nextSample();
            const TrainingDataGeneratorSample b = second.nextSample();
            QCOMPARE(a.watts, b.watts);
            QCOMPARE(a.cadence, b.cadence);
            QCOMPARE(a.heartRate, b.heartRate);
            QCOMPARE(a.leftRightBalance, b.leftRightBalance);
            QCOMPARE(a.smO2, b.smO2);
            QCOMPARE(a.totalHb, b.totalHb);
        }
    }

    void parsesOnlyDocumentedGeneratorProfiles()
    {
        using Mode = TrainingDataGeneratorMode;

        QCOMPARE(TrainingDataGenerator::modeFromProfile(""),
                 Mode::FollowTarget);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("follow-target"),
                 Mode::FollowTarget);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("ON-TARGET"),
                 Mode::OnTarget);
        QCOMPARE(TrainingDataGenerator::modeFromProfile(" over_target "),
                 Mode::OverTarget);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("under target"),
                 Mode::UnderTarget);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("cadence-low"),
                 Mode::CadenceLow);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("cadence_high"),
                 Mode::CadenceHigh);
        QCOMPARE(TrainingDataGenerator::modeFromProfile("real-trainer"),
                 Mode::FollowTarget);
    }

    void deterministicModesHaveExplicitPowerAndCadenceContracts()
    {
        struct Expectation {
            TrainingDataGeneratorMode mode;
            double watts;
            double cadence;
        };
        const Expectation expectations[] = {
            {TrainingDataGeneratorMode::OnTarget, 200.0, 88.0},
            {TrainingDataGeneratorMode::OverTarget, 240.0, 92.0},
            {TrainingDataGeneratorMode::UnderTarget, 84.0, 45.0},
            {TrainingDataGeneratorMode::CadenceLow, 200.0, 55.0},
            {TrainingDataGeneratorMode::CadenceHigh, 200.0, 105.0}
        };

        for (const Expectation &expectation : expectations) {
            TrainingDataGenerator generator;
            generator.setMode(expectation.mode);
            generator.setTargetWatts(200.0);

            for (int i = 0; i < 16; ++i) {
                const TrainingDataGeneratorSample sample =
                        generator.nextSample();
                QCOMPARE(sample.watts, expectation.watts);
                QCOMPARE(sample.cadence, expectation.cadence);
            }
            QCOMPARE(generator.mode(), expectation.mode);
            QVERIFY(QString::fromLatin1(generator.modeLabel()).size() > 0);
        }
    }

    void onTargetSurvivesIntegerRealtimeTelemetry()
    {
        TrainingDataGenerator generator;
        generator.setMode(TrainingDataGeneratorMode::OnTarget);
        generator.setTargetWatts(199.5);

        const TrainingDataGeneratorSample sample = generator.nextSample();
        QVERIFY(sample.watts >= 199.5);
        QVERIFY(std::floor(sample.watts) >= 199.5);
        QCOMPARE(sample.cadence, 88.0);
    }

    void changingModeDoesNotLeakIntoAnotherGenerator()
    {
        TrainingDataGenerator scripted;
        TrainingDataGenerator ordinary;
        scripted.setMode(TrainingDataGeneratorMode::UnderTarget);
        scripted.setTargetWatts(300.0);
        ordinary.setTargetWatts(300.0);

        QCOMPARE(scripted.nextSample().watts, 126.0);
        QCOMPARE(ordinary.mode(), TrainingDataGeneratorMode::FollowTarget);
        QVERIFY(ordinary.nextSample().watts > 126.0);
    }

    void workoutGameTargetsAreRoutedOnlyToDataGenerators()
    {
        const DeviceTypes deviceTypes;
        for (const DeviceType &device : deviceTypes.Supported) {
            QCOMPARE(TrainingDataGeneratorTargetRouting::acceptsDeviceType(
                         device.type), device.type == DEV_NULL);
        }

        double normalized = -1.0;
        QVERIFY(TrainingDataGeneratorTargetRouting::normalizeTarget(
                    275.0, normalized));
        QCOMPARE(normalized, 275.0);
        QVERIFY(TrainingDataGeneratorTargetRouting::normalizeTarget(
                    9000.0, normalized));
        QCOMPARE(normalized, 2500.0);
        QVERIFY(!TrainingDataGeneratorTargetRouting::normalizeTarget(
                    std::numeric_limits<double>::quiet_NaN(), normalized));
    }

    void selectedGeneratorPublishesItsAuthoritativeTargetAndState()
    {
        RealtimeData generator;
        generator.setLoad(237.0);
        generator.setName("Data Generator [On target]");
        RealtimeData aggregate;
        aggregate.setLoad(100.0);
        aggregate.setName("ordinary source");

        QVERIFY(TrainingDataGeneratorTargetRouting::publishPowerSourceState(
                    DEV_NULL, generator, aggregate));
        QCOMPARE(aggregate.getLoad(), 237.0);
        QCOMPARE(QString::fromLatin1(aggregate.getName()),
                 QStringLiteral("Data Generator [On target]"));

        RealtimeData realTrainer;
        realTrainer.setLoad(900.0);
        realTrainer.setName("real trainer");
        QVERIFY(!TrainingDataGeneratorTargetRouting::publishPowerSourceState(
                    DEV_BT40, realTrainer, aggregate));
        QCOMPARE(aggregate.getLoad(), 237.0);
        QCOMPARE(QString::fromLatin1(aggregate.getName()),
                 QStringLiteral("Data Generator [On target]"));
    }

    void realtimeSourceNamesAreAlwaysBoundedAndTerminated()
    {
        const QByteArray oversized(200, 'x');
        RealtimeData data;
        data.setName(oversized.constData());
        QCOMPARE(std::strlen(data.getName()), std::size_t(63));
    }

    void followsWorkoutTargetAboveAndBelow()
    {
        TrainingDataGenerator generator;
        generator.setTargetWatts(300.0);

        bool wentBelow = false;
        bool wentAbove = false;
        for (int i = 0; i < 240; ++i) {
            const TrainingDataGeneratorSample sample = generator.nextSample();
            wentBelow = wentBelow || sample.watts < 285.0;
            wentAbove = wentAbove || sample.watts > 315.0;
            if (i >= 5) {
                QVERIFY(sample.watts >= 255.0);
                QVERIFY(sample.watts <= 345.0);
            }
            QVERIFY(sample.cadence >= 70.0);
            QVERIFY(sample.cadence <= 105.0);
            QVERIFY(sample.heartRate >= 90.0);
            QVERIFY(sample.heartRate <= 185.0);
        }
        QVERIFY(wentBelow);
        QVERIFY(wentAbove);
    }

    void heartRateRespondsGraduallyToLoadChanges()
    {
        TrainingDataGenerator generator;
        const double baseline = generator.nextSample().heartRate;

        generator.setTargetWatts(400.0);
        const double rising = generator.nextSample().heartRate;
        QVERIFY(rising > baseline);
        QVERIFY(rising - baseline <= 0.8);

        generator.setTargetWatts(50.0);
        const double falling = generator.nextSample().heartRate;
        QVERIFY(falling < rising);
        QVERIFY(rising - falling <= 0.35);
    }

    void clampsInvalidAndExtremeTargets()
    {
        TrainingDataGenerator generator;

        generator.setTargetWatts(-100.0);
        QCOMPARE(generator.targetWatts(), 0.0);
        QVERIFY(generator.nextSample().watts >= 0.0);

        generator.setTargetWatts(9000.0);
        QCOMPARE(generator.targetWatts(), 2500.0);
        QVERIFY(generator.nextSample().watts <= 2500.0);

        generator.setTargetWatts(std::numeric_limits<double>::quiet_NaN());
        QCOMPARE(generator.targetWatts(), 0.0);
        generator.setTargetWatts(std::numeric_limits<double>::infinity());
        QCOMPARE(generator.targetWatts(), 0.0);
    }

    void resetReplaysTheSameSequenceAtTheCurrentTarget()
    {
        TrainingDataGenerator generator;
        generator.setTargetWatts(190.0);
        generator.reset();
        const TrainingDataGeneratorSample first = generator.nextSample();
        generator.nextSample();
        generator.nextSample();

        generator.reset();
        const TrainingDataGeneratorSample replay = generator.nextSample();
        QCOMPARE(replay.watts, first.watts);
        QCOMPARE(replay.cadence, first.cadence);
        QCOMPARE(replay.heartRate, first.heartRate);
        QCOMPARE(replay.coreTemperature, first.coreTemperature);
    }
};

QTEST_APPLESS_MAIN(TestTrainingDataGenerator)

#include "testTrainingDataGenerator.moc"
