/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/KurtInRide.h"
#include "Train/KurtSmartControl.h"

#include <QTest>

#include <cstring>
#include <memory>

static std::unique_ptr<char[]> exactPacket(int size)
{
    std::unique_ptr<char[]> packet(size > 0 ? new char[size] : nullptr);
    if (packet) std::memset(packet.get(), 0, size);
    return packet;
}

class TestKineticPacketBounds : public QObject
{
    Q_OBJECT

private slots:
    void inrideConfigLengths_data();
    void inrideConfigLengths();
    void inridePowerLengths_data();
    void inridePowerLengths();
    void smartControlPowerLengths_data();
    void smartControlPowerLengths();
    void smartControlConfigLengths_data();
    void smartControlConfigLengths();
    void smartControlPointerParserLengths_data();
    void smartControlPointerParserLengths();
};

static void addLengthRows(int minimum, int maximum)
{
    QTest::addColumn<int>("size");
    QTest::addColumn<bool>("valid");

    for (int size = 0; size <= 21; ++size) {
        QTest::newRow(qPrintable(QString("size-%1").arg(size, 2, 10, QLatin1Char('0'))))
            << size << (size >= minimum && size <= maximum);
    }
}

void TestKineticPacketBounds::inrideConfigLengths_data()
{
    addLengthRows(20, 20);
}

void TestKineticPacketBounds::inrideConfigLengths()
{
    QFETCH(int, size);
    QFETCH(bool, valid);
    const std::unique_ptr<char[]> packet = exactPacket(size);
    inride_config_data output;
    std::memset(&output, 0xa5, sizeof(output));
    unsigned char expected[sizeof(output)];
    std::memcpy(expected, &output, sizeof(output));

    const inride_parse_result result = inride_parse_config_data(
        QByteArrayView(packet.get(), size), output);

    QCOMPARE(result == INRIDE_PARSE_SUCCESS, valid);
    if (!valid) QVERIFY(std::memcmp(&output, expected, sizeof(output)) == 0);
}

void TestKineticPacketBounds::inridePowerLengths_data()
{
    addLengthRows(20, 20);
}

void TestKineticPacketBounds::inridePowerLengths()
{
    QFETCH(int, size);
    QFETCH(bool, valid);
    const std::unique_ptr<char[]> packet = exactPacket(size);
    inride_power_data output;
    std::memset(&output, 0xa5, sizeof(output));
    unsigned char expected[sizeof(output)];
    std::memcpy(expected, &output, sizeof(output));

    const inride_parse_result result = inride_parse_power_data(
        QByteArrayView(packet.get(), size), output);

    QCOMPARE(result == INRIDE_PARSE_SUCCESS, valid);
    if (!valid) QVERIFY(std::memcmp(&output, expected, sizeof(output)) == 0);
}

void TestKineticPacketBounds::smartControlPowerLengths_data()
{
    addLengthRows(14, 20);
}

void TestKineticPacketBounds::smartControlPowerLengths()
{
    QFETCH(int, size);
    QFETCH(bool, valid);
    const std::unique_ptr<char[]> packet = exactPacket(size);
    smart_control_power_data output;
    std::memset(&output, 0xa5, sizeof(output));
    unsigned char expected[sizeof(output)];
    std::memcpy(expected, &output, sizeof(output));

    const smart_control_parse_result result = smart_control_parse_power_data(
        QByteArrayView(packet.get(), size), output);

    QCOMPARE(result == SMART_CONTROL_PARSE_SUCCESS, valid);
    if (!valid) QVERIFY(std::memcmp(&output, expected, sizeof(output)) == 0);
}

void TestKineticPacketBounds::smartControlConfigLengths_data()
{
    addLengthRows(5, 20);
}

void TestKineticPacketBounds::smartControlConfigLengths()
{
    QFETCH(int, size);
    QFETCH(bool, valid);
    const std::unique_ptr<char[]> packet = exactPacket(size);
    smart_control_config_data output;
    std::memset(&output, 0xa5, sizeof(output));
    unsigned char expected[sizeof(output)];
    std::memcpy(expected, &output, sizeof(output));

    const smart_control_parse_result result = smart_control_parse_config_data(
        QByteArrayView(packet.get(), size), output);

    QCOMPARE(result == SMART_CONTROL_PARSE_SUCCESS, valid);
    if (!valid) QVERIFY(std::memcmp(&output, expected, sizeof(output)) == 0);
}

void TestKineticPacketBounds::smartControlPointerParserLengths_data()
{
    QTest::addColumn<int>("size");

    for (int size = 0; size <= 21; ++size) {
        QTest::newRow(qPrintable(QString("size-%1").arg(size, 2, 10, QLatin1Char('0'))))
            << size;
    }
}

void TestKineticPacketBounds::smartControlPointerParserLengths()
{
    QFETCH(int, size);
    const std::unique_ptr<char[]> packet = exactPacket(size);

    const smart_control_power_data power = smart_control_process_power_data(
        reinterpret_cast<const uint8_t *>(packet.get()), static_cast<size_t>(size));
    const smart_control_config_data config = smart_control_process_config_data(
        reinterpret_cast<const uint8_t *>(packet.get()), static_cast<size_t>(size));

    if (size < 14 || size > 20) {
        QCOMPARE(power.mode, SMART_CONTROL_MODE_ERG);
        QCOMPARE(power.targetResistance, uint16_t(0));
        QCOMPARE(power.power, uint16_t(0));
        QCOMPARE(power.cadenceRPM, uint8_t(0));
        QCOMPARE(power.speedKPH, 0.0);
    }

    if (size < 5 || size > 20) {
        QCOMPARE(config.updateRate, uint8_t(1));
        QCOMPARE(config.calibrationState, SMART_CONTROL_CALIBRATION_STATE_NOT_PERFORMED);
        QCOMPARE(config.spindownTime, 0.0);
        QCOMPARE(config.calibrationThresholdKPH, 33.8);
        QCOMPARE(config.brakeCalibrationThresholdKPH, 45.0);
        QCOMPARE(config.tickRate, uint32_t(10000));
        QCOMPARE(config.systemStatus, uint16_t(0));
        QCOMPARE(config.firmwareUpdateState, uint8_t(0));
        QCOMPARE(config.brakeStrength, uint8_t(55));
        QCOMPARE(config.brakeOffset, uint8_t(128));
        QCOMPARE(config.noiseFilter, uint8_t(1));
    }
}

QTEST_APPLESS_MAIN(TestKineticPacketBounds)
#include "testKineticPacketBounds.moc"
