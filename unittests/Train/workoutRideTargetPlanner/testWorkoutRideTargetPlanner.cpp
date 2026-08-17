/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/VirtualDrivetrain.h"
#include "Train/BluetoothTrainerCapabilities.h"
#include "Train/WorkoutRideTargetPlanner.h"

#include <QTest>

#include <cmath>
#include <limits>

class TestWorkoutRideTargetPlanner : public QObject
{
    Q_OBJECT

private slots:
    void disabledModePreservesStandardErgTarget()
    {
        WorkoutRideTargetInput input;
        input.enabled = false;
        input.workoutWatts = 240.0;
        input.cadenceRpm = 90.0;
        input.relativeGearRatio = 0.75;

        const PlannedTrainerTarget result =
                WorkoutRideTargetPlanner::plan(
                        input, TrainerControlCapabilities::targetPowerOnly());

        QCOMPARE(result.mode, PlannedTrainerTargetMode::StandardErg);
        QCOMPARE(result.targetWatts, 240.0);
    }

    void unsupportedTrainerFallsBackToStandardErg()
    {
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 240.0;
        input.cadenceRpm = 90.0;
        input.relativeGearRatio = 0.75;

        const PlannedTrainerTarget result =
                WorkoutRideTargetPlanner::plan(
                        input, TrainerControlCapabilities());

        QCOMPARE(result.mode, PlannedTrainerTargetMode::StandardErg);
        QCOMPARE(result.targetWatts, 240.0);
    }

    void referenceCadenceAndInitialGearPreserveWorkoutTarget()
    {
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 240.0;
        input.cadenceRpm = 85.0;
        input.relativeGearRatio = 1.0;

        const PlannedTrainerTarget result =
                WorkoutRideTargetPlanner::plan(
                        input, TrainerControlCapabilities::targetPowerOnly());

        QCOMPARE(result.mode, PlannedTrainerTargetMode::WorkoutRidePower);
        QCOMPARE(result.targetWatts, 240.0);
    }

    void easierAndHarderGearsChangeRequiredPower()
    {
        VirtualDrivetrain drivetrain;
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 240.0;
        input.cadenceRpm = 85.0;

        QVERIFY(drivetrain.shiftDown());
        input.relativeGearRatio = drivetrain.relativeRatio();
        const PlannedTrainerTarget easier = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        drivetrain.reset();
        QVERIFY(drivetrain.shiftUp());
        input.relativeGearRatio = drivetrain.relativeRatio();
        const PlannedTrainerTarget harder = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        QVERIFY(easier.targetWatts < 240.0);
        QVERIFY(harder.targetWatts > 240.0);
        QVERIFY(easier.targetWatts < harder.targetWatts);
    }

    void cadenceRestoresPowerAfterShiftingDown()
    {
        VirtualDrivetrain drivetrain;
        QVERIFY(drivetrain.shiftDown());

        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 240.0;
        input.relativeGearRatio = drivetrain.relativeRatio();
        input.cadenceRpm = 85.0 / input.relativeGearRatio;

        const PlannedTrainerTarget result = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        QVERIFY(std::abs(result.targetWatts - 240.0) < 1e-9);
    }

    void missingCadenceUsesReferenceCadence_data()
    {
        QTest::addColumn<double>("cadence");
        QTest::newRow("zero") << 0.0;
        QTest::newRow("negative") << -1.0;
        QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN();
    }

    void missingCadenceUsesReferenceCadence()
    {
        QFETCH(double, cadence);
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 200.0;
        input.cadenceRpm = cadence;
        input.relativeGearRatio = 1.0;

        const PlannedTrainerTarget result = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        QCOMPARE(result.targetWatts, 200.0);
    }

    void cadenceAndOutputAreBoundedAgainstSpikes_data()
    {
        QTest::addColumn<double>("watts");
        QTest::addColumn<double>("cadence");
        QTest::addColumn<double>("ratio");
        QTest::addColumn<double>("expected");

        QTest::newRow("cadence-floor") << 200.0 << 1.0 << 1.0 << 100.0;
        QTest::newRow("cadence-ceiling") << 200.0 << 1000.0 << 1.0 << 350.0;
        QTest::newRow("power-ceiling") << 1400.0 << 170.0 << 2.0 << 1500.0;
        QTest::newRow("zero-workout") << 0.0 << 85.0 << 1.0 << 0.0;
    }

    void cadenceAndOutputAreBoundedAgainstSpikes()
    {
        QFETCH(double, watts);
        QFETCH(double, cadence);
        QFETCH(double, ratio);
        QFETCH(double, expected);
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = watts;
        input.cadenceRpm = cadence;
        input.relativeGearRatio = ratio;

        const PlannedTrainerTarget result = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        QCOMPARE(result.targetWatts, expected);
    }

    void invalidGearRatioFallsBackWithoutSendingNonFinitePower_data()
    {
        QTest::addColumn<double>("ratio");
        QTest::newRow("zero") << 0.0;
        QTest::newRow("negative") << -1.0;
        QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("infinite") << std::numeric_limits<double>::infinity();
    }

    void invalidGearRatioFallsBackWithoutSendingNonFinitePower()
    {
        QFETCH(double, ratio);
        WorkoutRideTargetInput input;
        input.enabled = true;
        input.workoutWatts = 220.0;
        input.cadenceRpm = 85.0;
        input.relativeGearRatio = ratio;

        const PlannedTrainerTarget result = WorkoutRideTargetPlanner::plan(
                input, TrainerControlCapabilities::targetPowerOnly());

        QCOMPARE(result.mode, PlannedTrainerTargetMode::StandardErg);
        QCOMPARE(result.targetWatts, 220.0);
        QVERIFY(std::isfinite(result.targetWatts));
    }

    void capabilityIntersectionRequiresEveryTrainerToSupportFeature()
    {
        TrainerControlCapabilities full;
        full.targetPower = true;
        full.targetResistance = true;
        full.simulation = true;
        TrainerControlCapabilities powerOnly =
                TrainerControlCapabilities::targetPowerOnly();

        const TrainerControlCapabilities common =
                TrainerControlCapabilities::commonCapabilities(
                        {full, powerOnly});

        QVERIFY(common.targetPower);
        QVERIFY(!common.targetResistance);
        QVERIFY(!common.simulation);
        QVERIFY(!common.nativeVirtualGearing);
    }

    void emptyCapabilitySetSupportsNothing()
    {
        const TrainerControlCapabilities common =
                TrainerControlCapabilities::commonCapabilities({});
        QVERIFY(!common.targetPower);
        QVERIFY(!common.targetResistance);
        QVERIFY(!common.simulation);
        QVERIFY(!common.nativeVirtualGearing);
    }

    void capabilityIntersectionCanBeAccumulated()
    {
        TrainerControlCapabilities common;
        common.targetPower = true;
        common.targetResistance = true;
        common.simulation = true;
        common.nativeVirtualGearing = true;

        TrainerControlCapabilities powerAndSimulation;
        powerAndSimulation.targetPower = true;
        powerAndSimulation.simulation = true;
        common.intersectWith(powerAndSimulation);

        QVERIFY(common.targetPower);
        QVERIFY(!common.targetResistance);
        QVERIFY(common.simulation);
        QVERIFY(!common.nativeVirtualGearing);
    }

    void bluetoothProtocolCapabilities_data()
    {
        QTest::addColumn<BluetoothTrainerControlProtocol>("protocol");
        QTest::addColumn<bool>("power");
        QTest::addColumn<bool>("resistance");
        QTest::addColumn<bool>("simulation");

        QTest::newRow("none")
                << BluetoothTrainerControlProtocol::None
                << false << false << false;
        QTest::newRow("tacx-uart")
                << BluetoothTrainerControlProtocol::TacxUart
                << true << false << true;
        QTest::newRow("wahoo-kickr")
                << BluetoothTrainerControlProtocol::WahooKickr
                << true << true << true;
        QTest::newRow("kurt-inride")
                << BluetoothTrainerControlProtocol::KurtInRide
                << false << false << false;
        QTest::newRow("kurt-smart-control")
                << BluetoothTrainerControlProtocol::KurtSmartControl
                << true << true << true;
    }

    void bluetoothProtocolCapabilities()
    {
        QFETCH(BluetoothTrainerControlProtocol, protocol);
        QFETCH(bool, power);
        QFETCH(bool, resistance);
        QFETCH(bool, simulation);

        const TrainerControlCapabilities result =
                BluetoothTrainerCapabilities::forProtocol(protocol);

        QCOMPARE(result.targetPower, power);
        QCOMPARE(result.targetResistance, resistance);
        QCOMPARE(result.simulation, simulation);
        QVERIFY(!result.nativeVirtualGearing);
    }

    void ftmsCapabilitiesFollowAdvertisedTargetFeatures()
    {
        BluetoothFtmsControlFeatures features;
        features.targetPower = true;
        features.simulation = true;

        const TrainerControlCapabilities result =
                BluetoothTrainerCapabilities::forProtocol(
                        BluetoothTrainerControlProtocol::Ftms, features);

        QVERIFY(result.targetPower);
        QVERIFY(!result.targetResistance);
        QVERIFY(result.simulation);
        QVERIFY(!result.nativeVirtualGearing);
    }

    void ftmsWithoutFeatureResponseSupportsNothing()
    {
        const TrainerControlCapabilities result =
                BluetoothTrainerCapabilities::forProtocol(
                        BluetoothTrainerControlProtocol::Ftms);

        QVERIFY(!result.targetPower);
        QVERIFY(!result.targetResistance);
        QVERIFY(!result.simulation);
        QVERIFY(!result.nativeVirtualGearing);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutRideTargetPlanner)
#include "testWorkoutRideTargetPlanner.moc"
