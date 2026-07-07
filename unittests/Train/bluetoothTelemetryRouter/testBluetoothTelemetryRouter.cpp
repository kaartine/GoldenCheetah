/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/BluetoothTelemetryRouter.h"

#include <QTest>

#include <limits>

class TestBluetoothTelemetryRouter : public QObject
{
    Q_OBJECT

private:
    using Metric = BluetoothTelemetryMetric;
    using Priority = BluetoothTelemetryPriority;
    using Router = BluetoothTelemetryRouter;

private slots:
    void dedicatedSensorWinsInterleavedPowerSamples()
    {
        Router router(1000);

        QVERIFY(router.publish(1, Metric::Power, 220.0, Priority::Trainer, 0));
        QCOMPARE(router.resolve(Metric::Power, 0).source, quintptr(1));

        QVERIFY(router.publish(
                2, Metric::Power, 210.0, Priority::DedicatedSensor, 10));
        QCOMPARE(router.resolve(Metric::Power, 10).source, quintptr(2));

        QVERIFY(router.publish(1, Metric::Power, 300.0, Priority::Trainer, 20));
        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::Power, 20);
        QVERIFY(resolved.available);
        QCOMPARE(resolved.source, quintptr(2));
        QCOMPARE(resolved.value, 210.0);
        QCOMPARE(resolved.priority, Priority::DedicatedSensor);
    }

    void removingOwnerFallsBackToFreshSource()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 250.0, Priority::Trainer, 20);
        router.publish(2, Metric::Power, 205.0,
                       Priority::DedicatedSensor, 10);
        QCOMPARE(router.resolve(Metric::Power, 20).source, quintptr(2));

        router.removeSource(2);

        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::Power, 30);
        QVERIFY(resolved.available);
        QCOMPARE(resolved.source, quintptr(1));
        QCOMPARE(resolved.value, 250.0);
    }

    void removingNonOwnerDoesNotClearOwner()
    {
        Router router(1000);
        router.publish(1, Metric::HeartRate, 145.0,
                       Priority::DedicatedSensor, 0);
        QCOMPARE(router.resolve(Metric::HeartRate, 0).source, quintptr(1));
        router.publish(2, Metric::HeartRate, 150.0,
                       Priority::DedicatedSensor, 10);
        QCOMPARE(router.resolve(Metric::HeartRate, 10).source, quintptr(1));

        router.removeSource(2);

        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::HeartRate, 20);
        QVERIFY(resolved.available);
        QCOMPARE(resolved.source, quintptr(1));
        QCOMPARE(resolved.value, 145.0);
    }

    void equalPriorityOwnerDoesNotFlapUntilStale()
    {
        Router router(1000);
        router.publish(1, Metric::HeartRate, 145.0,
                       Priority::DedicatedSensor, 0);
        QCOMPARE(router.resolve(Metric::HeartRate, 0).source, quintptr(1));

        router.publish(2, Metric::HeartRate, 150.0,
                       Priority::DedicatedSensor, 200);
        QCOMPARE(router.resolve(Metric::HeartRate, 200).source, quintptr(1));

        router.publish(2, Metric::HeartRate, 151.0,
                       Priority::DedicatedSensor, 900);
        const BluetoothTelemetryValue fallback =
                router.resolve(Metric::HeartRate, 1100);
        QVERIFY(fallback.available);
        QCOMPARE(fallback.source, quintptr(2));
        QCOMPARE(fallback.value, 151.0);
    }

    void staleHigherPrioritySourceFallsBackToFreshTrainer()
    {
        Router router(1000);
        router.publish(2, Metric::Cadence, 92.0,
                       Priority::DedicatedSensor, 0);
        QCOMPARE(router.resolve(Metric::Cadence, 0).source, quintptr(2));

        router.publish(1, Metric::Cadence, 88.0, Priority::Trainer, 900);
        const BluetoothTelemetryValue fallback =
                router.resolve(Metric::Cadence, 1100);

        QVERIFY(fallback.available);
        QCOMPARE(fallback.source, quintptr(1));
        QCOMPARE(fallback.value, 88.0);
    }

    void allStaleSourcesResolveUnavailable()
    {
        Router router(1000);
        router.publish(1, Metric::Speed, 35.0, Priority::Trainer, 0);

        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::Speed, 1001);

        QVERIFY(!resolved.available);
        QCOMPARE(resolved.source, quintptr(0));
        QCOMPARE(resolved.value, 0.0);
    }

    void metricsChooseOwnersIndependently()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 240.0, Priority::Trainer, 0);
        router.publish(1, Metric::Cadence, 85.0, Priority::Trainer, 0);
        router.publish(2, Metric::Power, 230.0,
                       Priority::DedicatedSensor, 10);
        router.publish(3, Metric::HeartRate, 148.0,
                       Priority::DedicatedSensor, 10);

        QCOMPARE(router.resolve(Metric::Power, 10).source, quintptr(2));
        QCOMPARE(router.resolve(Metric::Cadence, 10).source, quintptr(1));
        QCOMPARE(router.resolve(Metric::HeartRate, 10).source, quintptr(3));
    }

    void removingSourceClearsOnlyItsMetricSnapshots()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 240.0, Priority::Trainer, 0);
        router.publish(1, Metric::Speed, 35.0, Priority::Trainer, 0);
        router.publish(2, Metric::HeartRate, 148.0,
                       Priority::DedicatedSensor, 0);
        router.resolve(Metric::Power, 0);
        router.resolve(Metric::Speed, 0);
        router.resolve(Metric::HeartRate, 0);

        router.removeSource(1);

        QVERIFY(!router.resolve(Metric::Power, 10).available);
        QVERIFY(!router.resolve(Metric::Speed, 10).available);
        QCOMPARE(router.resolve(Metric::HeartRate, 10).source, quintptr(2));
    }

    void fallbackUsesLatestNonOwnerSnapshot()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 200.0, Priority::Trainer, 0);
        router.publish(2, Metric::Power, 210.0,
                       Priority::DedicatedSensor, 10);
        router.resolve(Metric::Power, 10);

        router.publish(1, Metric::Power, 260.0, Priority::Trainer, 20);
        router.removeSource(2);

        QCOMPARE(router.resolve(Metric::Power, 30).value, 260.0);
    }

    void invalidPublicationsDoNotReplaceValidSnapshot()
    {
        Router router(1000);
        QVERIFY(router.publish(1, Metric::Power, 220.0, Priority::Trainer, 0));

        QVERIFY(!router.publish(0, Metric::Power, 300.0,
                                Priority::DedicatedSensor, 10));
        QVERIFY(!router.publish(2, static_cast<Metric>(999), 300.0,
                                Priority::DedicatedSensor, 10));
        QVERIFY(!router.publish(
                2, Metric::Power,
                std::numeric_limits<double>::quiet_NaN(),
                Priority::DedicatedSensor, 10));
        QVERIFY(!router.publish(2, Metric::Power, 300.0,
                                static_cast<Priority>(999), 10));
        QVERIFY(!router.publish(2, Metric::Power, 300.0,
                                Priority::DedicatedSensor, -1));

        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::Power, 10);
        QCOMPARE(resolved.source, quintptr(1));
        QCOMPARE(resolved.value, 220.0);
        QVERIFY(!router.resolve(static_cast<Metric>(999), 10).available);
    }

    void backwardsClockDoesNotExpireCurrentOwner()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 220.0, Priority::Trainer, 500);
        QCOMPARE(router.resolve(Metric::Power, 500).source, quintptr(1));

        const BluetoothTelemetryValue resolved =
                router.resolve(Metric::Power, 400);
        QVERIFY(resolved.available);
        QCOMPARE(resolved.source, quintptr(1));
    }

    void clearRemovesSnapshotsAndOwners()
    {
        Router router(1000);
        router.publish(1, Metric::Power, 220.0, Priority::Trainer, 0);
        router.publish(2, Metric::HeartRate, 145.0,
                       Priority::DedicatedSensor, 0);
        router.resolve(Metric::Power, 0);
        router.resolve(Metric::HeartRate, 0);

        router.clear();

        QVERIFY(!router.resolve(Metric::Power, 1).available);
        QVERIFY(!router.resolve(Metric::HeartRate, 1).available);
    }
};

QTEST_APPLESS_MAIN(TestBluetoothTelemetryRouter)
#include "testBluetoothTelemetryRouter.moc"
