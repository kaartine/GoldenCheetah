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
#include "Train/DeviceTypes.h"

#include <QTest>

#include <cmath>
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

    void tracksWorkoutTargetWithinBoundedVariation()
    {
        TrainingDataGenerator generator;
        generator.setTargetWatts(300.0);

        for (int i = 0; i < 32; ++i) {
            const TrainingDataGeneratorSample sample = generator.nextSample();
            QVERIFY(sample.watts >= 297.0);
            QVERIFY(sample.watts <= 303.0);
            QVERIFY(sample.cadence >= 70.0);
            QVERIFY(sample.cadence <= 105.0);
            QVERIFY(sample.heartRate >= 90.0);
            QVERIFY(sample.heartRate <= 185.0);
        }
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
        QVERIFY(generator.nextSample().watts <= 2503.0);

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
