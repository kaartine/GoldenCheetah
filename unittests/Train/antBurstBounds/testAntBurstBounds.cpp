/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "ANT/ANTMessage.h"
#include "Settings.h"

#include <QByteArray>
#include <QTest>

#include <array>
#include <cstring>

int OperatingSystem = LINUX;

GSettings::GSettings(QString, QString)
    : newFormat(false),
      systemsettings(NULL),
      oldsystemsettings(NULL),
      global(NULL)
{
}

GSettings::~GSettings()
{
}

QVariant GSettings::value(const QObject *, const QString, const QVariant def)
{
    return def;
}

void GSettings::setValue(QString, QVariant)
{
}

QVariant GSettings::cvalue(QString, QString, QVariant def)
{
    return def;
}

static GSettings testSettings(QStringLiteral("test"), QStringLiteral("test"));
GSettings *appsettings = &testSettings;

class ScopedANT : public ANT
{
public:
    ~ScopedANT()
    {
        for (int i = 0; i < ANT_MAX_CHANNELS; ++i) {
            delete antChannel[i];
        }
    }
};

static const unsigned char StandardBurstMessageLength = 9;
static const int AntWireFrameOverhead = 4;

static_assert(StandardBurstMessageLength == ANT_MAX_BURST_DATA + 1,
              "ANT burst data is one control byte plus eight payload bytes");

typedef std::array<unsigned char, ANT_MAX_BURST_DATA> BurstPayload;

static BurstPayload repeatedPayload(const unsigned char value)
{
    BurstPayload payload;
    payload.fill(value);
    return payload;
}

static unsigned char burstControl(const unsigned char sequence,
                                  const bool last = false)
{
    return static_cast<unsigned char>(((sequence & 0x03) << 5) |
                                      (last ? 0x80 : 0x00));
}

static QByteArray burstFrame(const int length,
                             const unsigned char control,
                             const BurstPayload &payload)
{
    QByteArray frame;
    frame.reserve(length + AntWireFrameOverhead);

    unsigned char checksum = ANT_SYNC_BYTE;
    frame.append(static_cast<char>(ANT_SYNC_BYTE));

    checksum ^= static_cast<unsigned char>(length);
    frame.append(static_cast<char>(length));

    checksum ^= ANT_BURST_DATA;
    frame.append(static_cast<char>(ANT_BURST_DATA));

    for (int i = 0; i < length; ++i) {
        unsigned char data = 0;
        if (i == 0) {
            data = control;
        } else if (i <= ANT_MAX_BURST_DATA) {
            data = payload[static_cast<size_t>(i - 1)];
        }

        checksum ^= data;
        frame.append(static_cast<char>(data));
    }

    frame.append(static_cast<char>(checksum));
    return frame;
}

static QByteArray standardBurstFrame(const unsigned char sequence,
                                     const bool last,
                                     const BurstPayload &payload)
{
    return burstFrame(StandardBurstMessageLength,
                      burstControl(sequence, last), payload);
}

static unsigned char xorBytes(const QByteArray &bytes)
{
    unsigned char checksum = 0;
    for (const char byte : bytes) {
        checksum ^= static_cast<unsigned char>(byte);
    }
    return checksum;
}

static void feedFrame(ANT *ant, const QByteArray &frame)
{
    for (const char byte : frame) {
        ant->receiveByte(static_cast<unsigned char>(byte));
    }
}

static void addMalformedLengthRows()
{
    QTest::addColumn<int>("length");

    QTest::newRow("zero") << 0;
    QTest::newRow("one") << 1;
    QTest::newRow("two") << 2;
    QTest::newRow("three") << 3;
    QTest::newRow("four") << 4;
    QTest::newRow("five") << 5;
    QTest::newRow("six") << 6;
    QTest::newRow("seven") << 7;
    QTest::newRow("eight") << 8;
    QTest::newRow("above-protocol-maximum") << 10;
    QTest::newRow("byte-maximum") << 255;
}

class TestAntBurstBounds : public QObject
{
    Q_OBJECT

    static void prepareChannel(ANTChannel *channel)
    {
        channel->burstInit();
        std::memset(channel->rx_burst_data, 0xa5,
                    sizeof(channel->rx_burst_data));

        // receiveMessage also performs stale-data bookkeeping.
        channel->blanking_timestamp = get_timestamp();
        channel->blanked = 1;
    }

    static void dispatchFrame(ANTChannel *channel, QByteArray frame)
    {
        channel->receiveMessage(
            reinterpret_cast<unsigned char *>(frame.data()));
    }

private slots:

    void rejectsNonStandardLengths_data()
    {
        addMalformedLengthRows();
    }

    void rejectsNonStandardLengths()
    {
        QFETCH(int, length);

        QVERIFY(!ANTMessage::isValidBurstLength(
            static_cast<unsigned char>(length)));

        const BurstPayload payload = repeatedPayload(0x5a);
        QByteArray frame = burstFrame(length, burstControl(0), payload);
        QCOMPARE(xorBytes(frame), static_cast<unsigned char>(0));

        const ANTMessage::PayloadView view = ANTMessage::burstPayload(
            reinterpret_cast<const unsigned char *>(frame.constData()));
        QVERIFY(!view.isValid());
        QCOMPARE(view.data, static_cast<const unsigned char *>(NULL));
        QCOMPARE(view.size, static_cast<size_t>(0));
    }

    void malformedWireFramesRecover_data()
    {
        addMalformedLengthRows();
    }

    void malformedWireFramesRecover()
    {
        QFETCH(int, length);

        ScopedANT ant;
        int received = 0;
        QObject::connect(&ant, &ANT::receivedAntMessage,
                         [&received](const unsigned char, const ANTMessage,
                                     const struct timeval) {
            ++received;
        });

        const BurstPayload malformedPayload = repeatedPayload(0x00);
        const QByteArray malformed = burstFrame(
            length, burstControl(0), malformedPayload);
        QCOMPARE(xorBytes(malformed), static_cast<unsigned char>(0));
        feedFrame(&ant, malformed);
        QCOMPARE(received, 0);

        const BurstPayload validPayload = repeatedPayload(0x3c);
        const QByteArray valid = standardBurstFrame(0, false, validPayload);
        QCOMPARE(valid.size(),
                 static_cast<int>(StandardBurstMessageLength) +
                     AntWireFrameOverhead);
        QCOMPARE(static_cast<unsigned char>(valid[ANT_OFFSET_LENGTH]),
                 StandardBurstMessageLength);
        QCOMPARE(xorBytes(valid), static_cast<unsigned char>(0));

        feedFrame(&ant, valid);
        QCOMPARE(received, 1);
    }

    void channelDispatchRejectsMalformedAndRecovers_data()
    {
        addMalformedLengthRows();
    }

    void channelDispatchRejectsMalformedAndRecovers()
    {
        QFETCH(int, length);

        ScopedANT ant;
        ANTChannel *channel = ant.antChannel[0];
        prepareChannel(channel);

        const BurstPayload malformedPayload = repeatedPayload(0x7e);
        dispatchFrame(channel,
                      burstFrame(length, burstControl(0), malformedPayload));

        QCOMPARE(channel->rx_burst_data_index, 0);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 0);
        QCOMPARE(channel->rx_burst_data[0], static_cast<unsigned char>(0xa5));

        const BurstPayload validPayload = repeatedPayload(0x31);
        dispatchFrame(channel, standardBurstFrame(0, false, validPayload));

        QCOMPARE(channel->rx_burst_data_index, ANT_MAX_BURST_DATA);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);
        QVERIFY(std::memcmp(channel->rx_burst_data, validPayload.data(),
                            validPayload.size()) == 0);
    }

    void channelDispatchCopiesExactlyEightPayloadBytes()
    {
        ScopedANT ant;
        ANTChannel *channel = ant.antChannel[0];
        prepareChannel(channel);

        const BurstPayload payload = {{
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
        }};
        const QByteArray frame = standardBurstFrame(0, false, payload);

        QVERIFY(ANTMessage::isValidBurstLength(
            StandardBurstMessageLength));
        QCOMPARE(static_cast<unsigned char>(frame[ANT_OFFSET_LENGTH]),
                 StandardBurstMessageLength);
        dispatchFrame(channel, frame);

        QCOMPARE(channel->rx_burst_data_index, ANT_MAX_BURST_DATA);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);
        QVERIFY(std::memcmp(channel->rx_burst_data, payload.data(),
                            payload.size()) == 0);
        QCOMPARE(channel->rx_burst_data[ANT_MAX_BURST_DATA],
                 static_cast<unsigned char>(0xa5));
    }

    void channelDispatchResetsOnSequenceMismatch()
    {
        ScopedANT ant;
        ANTChannel *channel = ant.antChannel[0];
        prepareChannel(channel);

        const BurstPayload first = repeatedPayload(0x10);
        dispatchFrame(channel, standardBurstFrame(0, false, first));
        QCOMPARE(channel->rx_burst_data_index, 8);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);

        const BurstPayload mismatch = repeatedPayload(0xee);
        dispatchFrame(channel, standardBurstFrame(2, false, mismatch));
        QCOMPARE(channel->rx_burst_data_index, 0);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 0);
        QCOMPARE(channel->rx_burst_data[8], static_cast<unsigned char>(0xa5));

        const BurstPayload restart = repeatedPayload(0x18);
        dispatchFrame(channel, standardBurstFrame(0, false, restart));
        QCOMPARE(channel->rx_burst_data_index, 8);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);
        QVERIFY(std::memcmp(channel->rx_burst_data, restart.data(),
                            restart.size()) == 0);

        const unsigned char sequences[] = {1, 2, 3, 1};
        for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
            const BurstPayload payload = repeatedPayload(
                static_cast<unsigned char>(0x20 + i));
            dispatchFrame(channel,
                          standardBurstFrame(sequences[i], false, payload));
            QVERIFY(std::memcmp(channel->rx_burst_data + 8 * (i + 1),
                                payload.data(), payload.size()) == 0);
        }

        QCOMPARE(channel->rx_burst_data_index, 40);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 2);
    }

    void lastPacketResetsAssembly()
    {
        ScopedANT ant;
        ANTChannel *channel = ant.antChannel[0];
        prepareChannel(channel);

        const BurstPayload lastPayload = repeatedPayload(0x44);
        dispatchFrame(channel, standardBurstFrame(0, true, lastPayload));

        QCOMPARE(channel->rx_burst_data_index, 0);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 0);
        QVERIFY(channel->rx_burst_disposition == NULL);
        QVERIFY(std::memcmp(channel->rx_burst_data, lastPayload.data(),
                            lastPayload.size()) == 0);

        const BurstPayload nextPayload = repeatedPayload(0x55);
        dispatchFrame(channel, standardBurstFrame(0, false, nextPayload));

        QCOMPARE(channel->rx_burst_data_index, ANT_MAX_BURST_DATA);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);
        QVERIFY(std::memcmp(channel->rx_burst_data, nextPayload.data(),
                            nextPayload.size()) == 0);
    }

    void channelDispatchStopsAtCapacity()
    {
        ScopedANT ant;
        ANTChannel *channel = ant.antChannel[0];
        prepareChannel(channel);

        std::array<unsigned char, RX_BURST_DATA_LEN> expected;
        for (int packet = 0;
             packet < RX_BURST_DATA_LEN / ANT_MAX_BURST_DATA;
             ++packet) {
            const unsigned char sequence = packet == 0
                ? 0
                : static_cast<unsigned char>(((packet - 1) % 3) + 1);

            BurstPayload payload;
            for (int byte = 0; byte < ANT_MAX_BURST_DATA; ++byte) {
                payload[static_cast<size_t>(byte)] =
                    static_cast<unsigned char>(packet * ANT_MAX_BURST_DATA +
                                               byte);
                expected[static_cast<size_t>(packet * ANT_MAX_BURST_DATA +
                                             byte)] =
                    payload[static_cast<size_t>(byte)];
            }

            dispatchFrame(channel,
                          standardBurstFrame(sequence, false, payload));
            QCOMPARE(channel->rx_burst_data_index,
                     (packet + 1) * ANT_MAX_BURST_DATA);
        }

        QCOMPARE(channel->rx_burst_data_index, RX_BURST_DATA_LEN);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 1);
        QVERIFY(std::memcmp(channel->rx_burst_data, expected.data(),
                            expected.size()) == 0);

        const BurstPayload beyondCapacity = repeatedPayload(0xff);
        dispatchFrame(channel,
                      standardBurstFrame(1, false, beyondCapacity));

        QCOMPARE(channel->rx_burst_data_index, RX_BURST_DATA_LEN);
        QCOMPARE(static_cast<int>(channel->rx_burst_next_sequence), 2);
        QVERIFY(std::memcmp(channel->rx_burst_data, expected.data(),
                            expected.size()) == 0);
    }
};

QTEST_MAIN(TestAntBurstBounds)
#include "testAntBurstBounds.moc"
