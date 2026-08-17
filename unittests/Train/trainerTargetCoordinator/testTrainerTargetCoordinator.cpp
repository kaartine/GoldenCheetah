/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/TrainerTargetCoordinator.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QTest>

#include <vector>

namespace {

class FakeTargetDevice final : public TrainerTargetDevice
{
public:
    void setLoad(double value) override
    {
        calls.append(QStringLiteral("load:%1").arg(value));
    }

    void setGradient(double value) override
    {
        calls.append(QStringLiteral("gradient:%1").arg(value));
    }

    void setWindResistance(double value) override
    {
        calls.append(QStringLiteral("wind:%1").arg(value));
    }

    QStringList calls;
};

}

class TestTrainerTargetCoordinator : public QObject
{
    Q_OBJECT

private slots:
    void ergTargetIsAppliedToEveryDevice()
    {
        FakeTargetDevice first;
        FakeTargetDevice second;
        TrainerTargetCoordinator coordinator;

        const TrainerTarget target = TrainerTarget::erg(235.0, 12345.0);
        const TrainerTargetResult result = coordinator.apply(
                target, std::vector<TrainerTargetDevice *>{&first, &second});

        QCOMPARE(result, TrainerTargetResult::Applied);
        QCOMPARE(first.calls, QStringList({QStringLiteral("load:235")}));
        QCOMPARE(second.calls, QStringList({QStringLiteral("load:235")}));
        QCOMPARE(target.workoutPosition, 12345.0);
    }

    void slopeTargetAppliesGradientThenWindResistance()
    {
        FakeTargetDevice device;
        TrainerTargetCoordinator coordinator;

        const TrainerTarget target = TrainerTarget::slope(
                7.25, 0.412, 9876.0);
        const TrainerTargetResult result = coordinator.apply(
                target, std::vector<TrainerTargetDevice *>{&device});

        QCOMPARE(result, TrainerTargetResult::Applied);
        QCOMPARE(device.calls, QStringList({
                QStringLiteral("gradient:7.25"),
                QStringLiteral("wind:0.412")
        }));
        QCOMPARE(target.workoutPosition, 9876.0);
    }

    void workoutEndDoesNotWriteToDevices_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("erg") << int(TrainerTargetMode::Erg);
        QTest::newRow("slope") << int(TrainerTargetMode::Slope);
    }

    void workoutEndDoesNotWriteToDevices()
    {
        QFETCH(int, mode);
        FakeTargetDevice device;
        TrainerTargetCoordinator coordinator;
        const TrainerTarget target = mode == int(TrainerTargetMode::Erg)
                ? TrainerTarget::erg(-100.0, 1000.0)
                : TrainerTarget::slope(-100.0, 0.3, 1000.0);

        QCOMPARE(coordinator.apply(
                         target,
                         std::vector<TrainerTargetDevice *>{&device}),
                 TrainerTargetResult::WorkoutFinished);
        QVERIFY(device.calls.isEmpty());
    }

    void nearbyValueIsNotMistakenForWorkoutEnd()
    {
        FakeTargetDevice device;
        TrainerTargetCoordinator coordinator;

        QCOMPARE(coordinator.apply(
                         TrainerTarget::erg(-99.9, 0.0),
                         std::vector<TrainerTargetDevice *>{&device}),
                 TrainerTargetResult::Applied);
        QCOMPARE(device.calls, QStringList({QStringLiteral("load:-99.9")}));
    }

    void noActiveDevicesIsStillAnAppliedTarget()
    {
        TrainerTargetCoordinator coordinator;

        QCOMPARE(coordinator.apply(
                         TrainerTarget::erg(200.0, 0.0), {}),
                 TrainerTargetResult::Applied);
    }

    void missingDeviceIsIgnoredWithoutSkippingValidDevices()
    {
        FakeTargetDevice device;
        TrainerTargetCoordinator coordinator;

        QCOMPARE(coordinator.apply(
                         TrainerTarget::erg(210.0, 0.0),
                         std::vector<TrainerTargetDevice *>{nullptr, &device}),
                 TrainerTargetResult::Applied);
        QCOMPARE(device.calls, QStringList({QStringLiteral("load:210")}));
    }
};

QTEST_GUILESS_MAIN(TestTrainerTargetCoordinator)
#include "testTrainerTargetCoordinator.moc"
