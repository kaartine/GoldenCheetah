/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/VirtualDrivetrain.h"

#include <QTest>

#include <cmath>
#include <limits>

class TestVirtualDrivetrain : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToMiddleOfTwelveSpeedRange()
    {
        VirtualDrivetrain drivetrain;

        QCOMPARE(drivetrain.minimumGear(), 1);
        QCOMPARE(drivetrain.maximumGear(), 12);
        QCOMPARE(drivetrain.gear(), 6);
        QCOMPARE(drivetrain.chainringTeeth(), 34);
        QCOMPARE(drivetrain.sprocketTeeth(), 24);
        QCOMPARE(drivetrain.relativeRatio(), 1.0);
    }

    void shiftingUpSelectsHarderRatios()
    {
        VirtualDrivetrain drivetrain;
        const double originalRatio = drivetrain.gearRatio();

        QVERIFY(drivetrain.shiftUp());
        QCOMPARE(drivetrain.gear(), 7);
        QVERIFY(drivetrain.gearRatio() > originalRatio);
        QCOMPARE(drivetrain.sprocketTeeth(), 21);
    }

    void shiftingDownSelectsEasierRatios()
    {
        VirtualDrivetrain drivetrain;
        const double originalRatio = drivetrain.gearRatio();

        QVERIFY(drivetrain.shiftDown());
        QCOMPARE(drivetrain.gear(), 5);
        QVERIFY(drivetrain.gearRatio() < originalRatio);
        QCOMPARE(drivetrain.sprocketTeeth(), 28);
    }

    void shiftsAndDirectSelectionClampAtBoundaries()
    {
        VirtualDrivetrain drivetrain;

        QVERIFY(drivetrain.setGear(-50));
        QCOMPARE(drivetrain.gear(), drivetrain.minimumGear());
        QVERIFY(!drivetrain.shiftDown());
        QVERIFY(!drivetrain.setGear(1));

        QVERIFY(drivetrain.setGear(500));
        QCOMPARE(drivetrain.gear(), drivetrain.maximumGear());
        QVERIFY(!drivetrain.shiftUp());
        QVERIFY(!drivetrain.setGear(12));
    }

    void resetRestoresInitialGear()
    {
        VirtualDrivetrain drivetrain(9);
        QCOMPARE(drivetrain.gear(), 9);
        QVERIFY(drivetrain.shiftUp());

        drivetrain.reset();

        QCOMPARE(drivetrain.gear(), 9);
        QCOMPARE(drivetrain.relativeRatio(), 1.0);
    }

    void initialGearIsClampedToSupportedRange()
    {
        VirtualDrivetrain tooLow(-1);
        VirtualDrivetrain tooHigh(99);

        QCOMPARE(tooLow.gear(), tooLow.minimumGear());
        QCOMPARE(tooHigh.gear(), tooHigh.maximumGear());
    }

    void cadenceAndWheelSizeDetermineVirtualSpeed()
    {
        VirtualDrivetrain drivetrain;
        const double expected = 90.0 * (34.0 / 24.0) * 2.105 * 60.0 / 1000.0;

        QVERIFY(std::abs(drivetrain.speedKph(90.0, 2.105) - expected) < 1e-9);
        QVERIFY(drivetrain.speedKph(90.0, 2.105) > 16.0);
    }

    void invalidSpeedInputsReturnZero_data()
    {
        QTest::addColumn<double>("cadence");
        QTest::addColumn<double>("circumference");

        QTest::newRow("negative-cadence") << -1.0 << 2.105;
        QTest::newRow("zero-cadence") << 0.0 << 2.105;
        QTest::newRow("negative-wheel") << 90.0 << -1.0;
        QTest::newRow("zero-wheel") << 90.0 << 0.0;
        QTest::newRow("nan-cadence")
                << std::numeric_limits<double>::quiet_NaN() << 2.105;
        QTest::newRow("infinite-wheel")
                << 90.0 << std::numeric_limits<double>::infinity();
    }

    void invalidSpeedInputsReturnZero()
    {
        QFETCH(double, cadence);
        QFETCH(double, circumference);
        VirtualDrivetrain drivetrain;

        QCOMPARE(drivetrain.speedKph(cadence, circumference), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestVirtualDrivetrain)
#include "testVirtualDrivetrain.moc"
