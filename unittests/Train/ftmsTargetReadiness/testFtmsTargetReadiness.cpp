/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/Ftms.h"

#include <QDataStream>
#include <QTest>

#include <limits>

namespace {

QByteArray targetRange(qint16 minimum, qint16 maximum, quint16 increment)
{
    QByteArray value;
    QDataStream stream(&value, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << minimum << maximum << increment;
    return value;
}

QByteArray featureFlags(quint32 machineFeatures, quint32 targetSettings)
{
    QByteArray value;
    QDataStream stream(&value, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << machineFeatures << targetSettings;
    return value;
}

}

class TestFtmsTargetReadiness : public QObject
{
    Q_OBJECT

private slots:
    void delayedPowerRangeSendsOnlyNewestTarget()
    {
        FtmsTargetController controller;

        QVERIFY(!controller.requestPower(181.0).isValid());
        QVERIFY(!controller.requestPower(234.0).isValid());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Power);

        const FtmsRangeResult result = controller.updatePowerRange(
                targetRange(100, 400, 10));

        QVERIFY(result.accepted);
        QCOMPARE(result.minimum, qint16(100));
        QCOMPARE(result.maximum, qint16(400));
        QCOMPARE(result.increment, quint16(10));
        QVERIFY(result.command.isValid());
        QCOMPARE(result.command.type, FtmsTargetType::Power);
        QCOMPARE(result.command.value, qint16(230));
        QCOMPARE(ftms_control_point_command(result.command),
                 QByteArray::fromHex("05e600"));
        QVERIFY(!controller.hasPendingTarget());
    }

    void absentPowerRangeNeverProducesACommand()
    {
        FtmsTargetController controller;

        const FtmsTargetCommand first = controller.requestPower(200.0);
        const FtmsTargetCommand replacement = controller.requestPower(225.0);

        QVERIFY(!first.isValid());
        QVERIFY(!replacement.isValid());
        QVERIFY(!controller.powerRangeReady());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Power);
    }

    void invalidPowerRangesKeepPendingTarget_data()
    {
        QTest::addColumn<QByteArray>("range");

        QTest::newRow("empty") << QByteArray();
        QTest::newRow("short") << targetRange(100, 400, 10).left(5);
        QTest::newRow("long") << (targetRange(100, 400, 10) + QByteArray(1, '\0'));
        QTest::newRow("reversed") << targetRange(400, 100, 10);
        QTest::newRow("zero-increment") << targetRange(100, 400, 0);
    }

    void invalidPowerRangesKeepPendingTarget()
    {
        QFETCH(QByteArray, range);
        FtmsTargetController controller;
        QVERIFY(!controller.requestPower(235.0).isValid());

        const FtmsRangeResult result = controller.updatePowerRange(range);

        QVERIFY(!result.accepted);
        QVERIFY(!result.command.isValid());
        QVERIFY(!controller.powerRangeReady());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Power);
    }

    void validPowerRangeAfterInvalidRangeFlushesPendingTarget()
    {
        FtmsTargetController controller;
        controller.requestPower(235.0);
        QVERIFY(!controller.updatePowerRange(targetRange(100, 400, 0)).accepted);

        const FtmsRangeResult result = controller.updatePowerRange(
                targetRange(100, 400, 10));

        QVERIFY(result.accepted);
        QVERIFY(result.command.isValid());
        QCOMPARE(result.command.value, qint16(240));
        QVERIFY(!controller.hasPendingTarget());
    }

    void delayedResistanceRangeScalesAndFlushesTarget()
    {
        FtmsTargetController controller;
        QVERIFY(!controller.requestResistance(0.26).isValid());

        const FtmsRangeResult result = controller.updateResistanceRange(
                targetRange(-100, 100, 10));

        QVERIFY(result.accepted);
        QVERIFY(result.command.isValid());
        QCOMPARE(result.command.type, FtmsTargetType::Resistance);
        QCOMPARE(result.command.value, qint16(-50));
        QCOMPARE(ftms_control_point_command(result.command),
                 QByteArray::fromHex("04ceff"));
    }

    void invalidResistanceRangeKeepsPendingTarget()
    {
        FtmsTargetController controller;
        controller.requestResistance(0.75);

        const FtmsRangeResult result = controller.updateResistanceRange(
                targetRange(-100, 100, 0));

        QVERIFY(!result.accepted);
        QVERIFY(!result.command.isValid());
        QVERIFY(!controller.resistanceRangeReady());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Resistance);
    }

    void requestsAreImmediateAfterRangesBecomeReady()
    {
        FtmsTargetController controller;
        QVERIFY(controller.updatePowerRange(targetRange(100, 400, 10)).accepted);
        QVERIFY(controller.updateResistanceRange(targetRange(-100, 100, 10)).accepted);

        const FtmsTargetCommand power = controller.requestPower(234.0);
        QVERIFY(power.isValid());
        QCOMPARE(power.type, FtmsTargetType::Power);
        QCOMPARE(power.value, qint16(230));

        const FtmsTargetCommand resistance = controller.requestResistance(0.76);
        QVERIFY(resistance.isValid());
        QCOMPARE(resistance.type, FtmsTargetType::Resistance);
        QCOMPARE(resistance.value, qint16(50));
        QVERIFY(!controller.hasPendingTarget());
    }

    void latestPendingTargetKindSupersedesEarlierKind()
    {
        FtmsTargetController controller;
        controller.requestPower(220.0);
        controller.requestResistance(0.75);

        const FtmsRangeResult power = controller.updatePowerRange(
                targetRange(100, 400, 10));
        QVERIFY(power.accepted);
        QVERIFY(!power.command.isValid());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Resistance);

        const FtmsRangeResult resistance = controller.updateResistanceRange(
                targetRange(0, 100, 10));
        QVERIFY(resistance.command.isValid());
        QCOMPARE(resistance.command.value, qint16(80));
        QVERIFY(!controller.hasPendingTarget());
    }

    void requestsClampToRangeAndNearestIncrement()
    {
        FtmsTargetController controller;
        controller.updatePowerRange(targetRange(-100, 300, 25));
        controller.updateResistanceRange(targetRange(-200, 200, 25));

        QCOMPARE(controller.requestPower(-1000.0).value, qint16(-100));
        QCOMPARE(controller.requestPower(1000.0).value, qint16(300));
        QCOMPARE(controller.requestPower(138.0).value, qint16(150));
        QCOMPARE(controller.requestResistance(-1.0).value, qint16(-200));
        QCOMPARE(controller.requestResistance(2.0).value, qint16(200));
        QCOMPARE(controller.requestResistance(0.54).value, qint16(25));
    }

    void extremeRangeDoesNotOverflow()
    {
        FtmsTargetController controller;
        const FtmsRangeResult range = controller.updatePowerRange(targetRange(
                std::numeric_limits<qint16>::min(),
                std::numeric_limits<qint16>::max(),
                std::numeric_limits<quint16>::max()));
        QVERIFY(range.accepted);

        QCOMPARE(controller.requestPower(
                         std::numeric_limits<double>::lowest()).value,
                 std::numeric_limits<qint16>::min());
        QCOMPARE(controller.requestPower(
                         std::numeric_limits<double>::max()).value,
                 std::numeric_limits<qint16>::max());
    }

    void nonFiniteRequestsAreRejectedWithoutReplacingPendingTarget()
    {
        FtmsTargetController controller;
        controller.requestPower(220.0);

        QVERIFY(!controller.requestPower(
                         std::numeric_limits<double>::quiet_NaN()).isValid());
        QVERIFY(!controller.requestResistance(
                         std::numeric_limits<double>::infinity()).isValid());
        QVERIFY(controller.hasPendingTarget());
        QCOMPARE(controller.pendingType(), FtmsTargetType::Power);

        const FtmsRangeResult result = controller.updatePowerRange(
                targetRange(100, 400, 10));
        QCOMPARE(result.command.value, qint16(220));
    }

    void clearAndResetRemoveStaleState()
    {
        FtmsTargetController controller;
        controller.updatePowerRange(targetRange(100, 400, 10));
        controller.requestResistance(0.5);
        controller.clearPendingTarget();
        QVERIFY(!controller.hasPendingTarget());
        QVERIFY(controller.powerRangeReady());

        controller.reset();
        QVERIFY(!controller.powerRangeReady());
        QVERIFY(!controller.resistanceRangeReady());
        QVERIFY(!controller.hasPendingTarget());
        QVERIFY(!controller.requestPower(200.0).isValid());
    }

    void featurePayloadRequiresExactlyEightBytes()
    {
        FtmsFeatureData parsed;
        const QByteArray valid = featureFlags(0x01020304u, 0x50607080u);

        QVERIFY(ftms_parse_feature_data(valid, parsed));
        QCOMPARE(parsed.machineFeatures, quint32(0x01020304u));
        QCOMPARE(parsed.targetSettings, quint32(0x50607080u));

        const FtmsFeatureData previous = parsed;
        QVERIFY(!ftms_parse_feature_data(valid.left(7), parsed));
        QCOMPARE(parsed.machineFeatures, previous.machineFeatures);
        QCOMPARE(parsed.targetSettings, previous.targetSettings);
        QVERIFY(!ftms_parse_feature_data(valid + QByteArray(1, '\0'), parsed));
        QCOMPARE(parsed.machineFeatures, previous.machineFeatures);
        QCOMPARE(parsed.targetSettings, previous.targetSettings);
    }

    void invalidTargetCommandSerializesToNothing()
    {
        const FtmsTargetCommand invalid;
        QVERIFY(!invalid.isValid());
        QVERIFY(ftms_control_point_command(invalid).isEmpty());

        const FtmsTargetCommand unknown{
                static_cast<FtmsTargetType>(99), 200};
        QVERIFY(!unknown.isValid());
        QVERIFY(ftms_control_point_command(unknown).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestFtmsTargetReadiness)
#include "testFtmsTargetReadiness.moc"
