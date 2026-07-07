/*
 * Copyright (c) 2026
 *
 * Regression coverage for DEV-003 ANT telemetry, channel command, and
 * transport transaction synchronization.
 */

#include "ANT.h"
#include "ANTMessage.h"
#include "LibUsb.h"

#include <QSemaphore>
#include <QTest>

#include <atomic>
#include <cmath>
#include <thread>

namespace {

bool isMessage(const FakeAntTransport::WriteCall &call)
{
    return !call.bytes.isEmpty() &&
            static_cast<unsigned char>(call.bytes.at(0)) == ANT_SYNC_BYTE;
}

bool isPadding(const FakeAntTransport::WriteCall &call)
{
    if (call.bytes.size() != 5) return false;
    for (const char byte : call.bytes) {
        if (byte != 0) return false;
    }
    return true;
}

QVector<FakeAntTransport::WriteCall> messages(
        const FakeAntTransport::Snapshot &snapshot)
{
    QVector<FakeAntTransport::WriteCall> result;
    for (const FakeAntTransport::WriteCall &call : snapshot.writes) {
        if (isMessage(call)) result.append(call);
    }
    return result;
}

QString describeWrites(const FakeAntTransport::Snapshot &snapshot)
{
    QStringList result;
    for (const FakeAntTransport::WriteCall &call : snapshot.writes) {
        if (isPadding(call)) {
            result.append(QStringLiteral("padding"));
        } else if (isMessage(call) && call.bytes.size() > ANT_OFFSET_ID) {
            result.append(QStringLiteral("frame(0x%1)")
                                  .arg(static_cast<unsigned char>(
                                           call.bytes.at(ANT_OFFSET_ID)),
                                       2, 16, QLatin1Char('0')));
        } else {
            result.append(QStringLiteral("unknown(%1 bytes)")
                                  .arg(call.bytes.size()));
        }
    }
    return result.join(QStringLiteral(", "));
}

}

class TestAntThreadSafety : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        FakeAntTransport::reset();
    }

    void allTelemetrySettersArePublishedInSnapshot()
    {
        ANT ant;
        ant.setBPM(141.0f);
        ant.setCadence(87.0f);
        ant.setWheelRpm(93.0f);
        ant.setSpeed(32.5);
        ant.incAltDistance(1.25);
        ant.incAltDistance(2.75);
        ant.setWatts(245.0f);
        ant.setAltWatts(238.0f);
        ant.setHb(68.5, 12.3);
        ant.setCoreTemp(38.1, 35.2, 4.6);
        ant.setTemp(21.5);
        ant.setLRBalance(48.5);
        ant.setTE(71.0, 72.0);
        ant.setPS(23.0, 24.0);
        ant.setRppb(1.0);
        ant.setRppe(2.0);
        ant.setRpppb(3.0);
        ant.setRpppe(4.0);
        ant.setLppb(5.0);
        ant.setLppe(6.0);
        ant.setLpppb(7.0);
        ant.setLpppe(8.0);
        ant.setRightPCO(9);
        ant.setLeftPCO(10);
        ant.setPosition(RealtimeData::standing);
        ant.setRTorque(11.0);
        ant.setLTorque(12.0);
        ant.setTorque(13.0);
        ant.setTrainerStatusAvailable(true);
        ant.setTrainerCalibRequired(true);
        ant.setTrainerConfigRequired(false);
        ant.setTrainerBrakeFault(true);
        ant.setTrainerReady(true);
        ant.setTrainerRunning(false);

        RealtimeData snapshot;
        ant.getRealtimeData(snapshot);

        QCOMPARE(snapshot.getHr(), 141.0);
        QCOMPARE(snapshot.getCadence(), 87.0);
        QCOMPARE(snapshot.getWheelRpm(), 93.0);
        QCOMPARE(snapshot.getSpeed(), 32.5);
        QCOMPARE(snapshot.getAltDistance(), 4.0);
        QCOMPARE(snapshot.getWatts(), 245.0);
        QCOMPARE(snapshot.getAltWatts(), 238.0);
        QCOMPARE(snapshot.getSmO2(), 68.5);
        QCOMPARE(snapshot.gettHb(), 12.3);
        QCOMPARE(snapshot.getCoreTemp(), 38.1);
        QCOMPARE(snapshot.getSkinTemp(), 35.2);
        QCOMPARE(snapshot.getHeatStrain(), 4.6);
        QCOMPARE(snapshot.getTemp(), 21.5);
        QCOMPARE(snapshot.getLRBalance(), 48.5);
        QCOMPARE(snapshot.getLTE(), 71.0);
        QCOMPARE(snapshot.getRTE(), 72.0);
        QCOMPARE(snapshot.getLPS(), 23.0);
        QCOMPARE(snapshot.getRPS(), 24.0);
        QCOMPARE(snapshot.getRppb(), 1.0);
        QCOMPARE(snapshot.getRppe(), 2.0);
        QCOMPARE(snapshot.getRpppb(), 3.0);
        QCOMPARE(snapshot.getRpppe(), 4.0);
        QCOMPARE(snapshot.getLppb(), 5.0);
        QCOMPARE(snapshot.getLppe(), 6.0);
        QCOMPARE(snapshot.getLpppb(), 7.0);
        QCOMPARE(snapshot.getLpppe(), 8.0);
        QCOMPARE(snapshot.getRightPCO(), 9.0);
        QCOMPARE(snapshot.getLeftPCO(), 10.0);
        QCOMPARE(snapshot.getPosition(), RealtimeData::standing);
        QCOMPARE(snapshot.getTorque(), 13.0);
        QVERIFY(snapshot.getTrainerStatusAvailable());
        QVERIFY(snapshot.getTrainerCalibRequired());
        QVERIFY(!snapshot.getTrainerConfigRequired());
        QVERIFY(snapshot.getTrainerBrakeFault());
        QVERIFY(snapshot.getTrainerReady());
        QVERIFY(!snapshot.getTrainerRunning());

        ANT secondaryCadence;
        secondaryCadence.setSecondaryCadence(81.0f);
        secondaryCadence.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getCadence(), 81.0);
    }

    void telemetryWriterAndSnapshotAreSynchronized()
    {
        ANT ant;
        QSemaphore ready;
        QSemaphore started;
        QSemaphore start;
        std::atomic<bool> stop(false);

        std::thread writer([&]() {
            ready.release();
            start.acquire();
            quint64 sample = 0;
            ant.setWheelRpm(80.0f);
            started.release();
            while (!stop.load(std::memory_order_acquire)) {
                ant.setWheelRpm((sample++ & 1U) ? 80.0f : 120.0f);
            }
        });

        ready.acquire();
        start.release();
        started.acquire();

        RealtimeData snapshot;
        double observed = 0.0;
        for (int sample = 0; sample < 250000; ++sample) {
            ant.getRealtimeData(snapshot);
            observed += snapshot.getWheelRpm();
        }

        stop.store(true, std::memory_order_release);
        writer.join();
        QVERIFY(std::isfinite(observed));
        QVERIFY(observed > 0.0);
    }

    void setChannelProducerAndWorkerDequeueAreSynchronizedAndOrdered()
    {
        const int commandCount = 256;
        FakeAntTransport::enableProducerReadBarrier();

        ANT worker;
        QCOMPARE(worker.start(), 0);

        std::atomic<bool> synchronized(true);
        std::thread producer([&]() {
            for (int command = 0; command < commandCount; ++command) {
                if (!FakeAntTransport::synchronizeProducerWithRead()) {
                    synchronized.store(false, std::memory_order_release);
                    return;
                }
                worker.setChannel(command % ANT_MAX_CHANNELS,
                                  1000 + command,
                                  ANTChannel::CHANNEL_TYPE_POWER);
            }
        });

        producer.join();
        FakeAntTransport::disableProducerReadBarrier();

        const bool drained = FakeAntTransport::waitForMessageWrites(
                commandCount * 2, 15000);
        const FakeAntTransport::Snapshot transport =
                FakeAntTransport::snapshot();
        const QVector<FakeAntTransport::WriteCall> frames =
                messages(transport);
        QCOMPARE(worker.stop(), 0);

        QVERIFY2(synchronized.load(std::memory_order_acquire),
                 "fake transport failed to pair producer and worker");
        QVERIFY2(drained,
                 qPrintable(QStringLiteral("worker wrote %1 of %2 messages")
                                    .arg(transport.messageWrites)
                                    .arg(commandCount * 2)));
        QCOMPARE(frames.size(), commandCount * 2);

        for (int command = 0; command < commandCount; ++command) {
            const int channel = command % ANT_MAX_CHANNELS;
            const QByteArray unassign = frames.at(command * 2).bytes;
            const QByteArray assign = frames.at(command * 2 + 1).bytes;
            const bool ordered =
                    unassign.size() > ANT_OFFSET_CHANNEL_NUMBER &&
                    assign.size() > ANT_OFFSET_CHANNEL_NUMBER &&
                    static_cast<unsigned char>(
                        unassign.at(ANT_OFFSET_ID)) == ANT_UNASSIGN_CHANNEL &&
                    static_cast<unsigned char>(
                        assign.at(ANT_OFFSET_ID)) == ANT_ASSIGN_CHANNEL &&
                    static_cast<unsigned char>(
                        unassign.at(ANT_OFFSET_CHANNEL_NUMBER)) == channel &&
                    static_cast<unsigned char>(
                        assign.at(ANT_OFFSET_CHANNEL_NUMBER)) == channel;
            if (!ordered) {
                QFAIL(qPrintable(QStringLiteral(
                        "setChannel command %1 was not FIFO").arg(command)));
            }
        }
    }

    void sendMessageFrameAndPaddingAreOneTransaction()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        FakeAntTransport::armMessageInterleave();

        std::thread first([&]() {
            ant.sendMessage(ANTMessage::resetSystem());
        });
        const bool blocked =
                FakeAntTransport::waitForFirstMessageBlocked();

        std::thread second([&]() {
            ant.sendMessage(ANTMessage::close(3));
        });

        first.join();
        second.join();

        const FakeAntTransport::Snapshot transport =
                FakeAntTransport::snapshot();
        ant.closePort();

        QVERIFY2(blocked, "fake transport missed the first message");
        QCOMPARE(transport.writes.size(), 4);
        QVERIFY(isMessage(transport.writes.at(0)));
        QVERIFY2(isPadding(transport.writes.at(1)),
                 qPrintable(QStringLiteral(
                         "ANT::sendMessage split a wire transaction across "
                         "threads; observed: %1")
                                    .arg(describeWrites(transport))));
        QVERIFY(isMessage(transport.writes.at(2)));
        QVERIFY(isPadding(transport.writes.at(3)));
    }
};

QTEST_GUILESS_MAIN(TestAntThreadSafety)

#include "testAntThreadSafety.moc"
