/*
 * Copyright (c) 2026
 *
 * Regression coverage for DEV-003 ANT synchronization and DEV-004 telemetry
 * freshness.
 */

#include "ANT.h"
#include "ANTMessage.h"
#include "ANTTelemetryFreshness.h"
#include "LibUsb.h"
#include "TrainSidebar.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <cmath>
#include <initializer_list>
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

QByteArray antFrame(const unsigned char messageId,
                    std::initializer_list<unsigned char> payload)
{
    QByteArray frame;
    frame.append(static_cast<char>(ANT_SYNC_BYTE));
    frame.append(static_cast<char>(payload.size()));
    frame.append(static_cast<char>(messageId));
    for (const unsigned char byte : payload)
        frame.append(static_cast<char>(byte));

    unsigned char checksum = 0;
    for (const char byte : frame)
        checksum ^= static_cast<unsigned char>(byte);
    frame.append(static_cast<char>(checksum));
    return frame;
}

QByteArray heartRateFrame(const unsigned char channel,
                          const unsigned char heartRate,
                          const unsigned char beatCount)
{
    return antFrame(ANT_BROADCAST_DATA,
                    {channel, 0, 0, 0, 0, 0, 0, beatCount, heartRate});
}

bool isFecCapabilityRequest(const FakeAntTransport::WriteCall &call)
{
    return isMessage(call) && call.bytes.size() > 10 &&
            static_cast<unsigned char>(call.bytes.at(ANT_OFFSET_ID)) ==
                    ANT_ACK_DATA &&
            static_cast<unsigned char>(call.bytes.at(4)) ==
                    FITNESS_EQUIPMENT_REQUEST_DATA_PAGE &&
            static_cast<unsigned char>(call.bytes.at(10)) ==
                    FITNESS_EQUIPMENT_TRAINER_CAPABILITIES_PAGE;
}

quintptr currentThreadId()
{
    return reinterpret_cast<quintptr>(QThread::currentThreadId());
}

void discardQtMessage(QtMsgType, const QMessageLogContext &, const QString &) {}

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

    void channelTimingStateIsInitialized()
    {
        const double before = get_timestamp();
        ANT ant;
        const double after = get_timestamp();

        for (int index = 0; index < ANT_MAX_CHANNELS; ++index) {
            ANTChannel *channel = ant.antChannel[index];
            QVERIFY(channel->blanking_timestamp >= before);
            QVERIFY(channel->blanking_timestamp <= after);
            QVERIFY(channel->last_message_timestamp >= before);
            QVERIFY(channel->last_message_timestamp <= after);
        }
    }

    void freshTelemetryRefreshesChannelDeadline()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        ANTChannel *channel = ant.antChannel[0];
        channel->channel_type = ANTChannel::CHANNEL_TYPE_HR;
        channel->messages_received = 1;
        channel->blanking_timestamp = get_timestamp() - 10.0;
        channel->blanked = 0;

        QByteArray frame = heartRateFrame(0, 147, 1);
        channel->receiveMessage(
                reinterpret_cast<unsigned char *>(frame.data()));

        QCOMPARE(channel->blanked, 0);
        QVERIFY(channel->blanking_timestamp > get_timestamp() - 1.0);
        QCOMPARE(ant.closePort(), 0);
    }

    void staleChannelClearsItsHeartRate()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        ANTChannel *channel = ant.antChannel[0];
        channel->channel_type = ANTChannel::CHANNEL_TYPE_HR;

        for (unsigned char beat = 1; beat <= 3; ++beat) {
            QByteArray frame = heartRateFrame(0, 153, beat);
            channel->receiveMessage(
                    reinterpret_cast<unsigned char *>(frame.data()));
        }

        RealtimeData snapshot;
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getHr(), 153.0);

        channel->blanking_timestamp = get_timestamp() - 10.0;
        channel->blanked = 0;
        unsigned char noTelemetry[] = {ANT_SYNC_BYTE, 0, 0xff};
        channel->receiveMessage(noTelemetry);

        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getHr(), 0.0);
        QCOMPARE(ant.closePort(), 0);
    }

    void staleSourceDoesNotClearFreshReplacement()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        ant.setBPM(141.0f, 0);
        ant.setBPM(156.0f, 1);
        ant.setCadence(91.0f, 2);

        ant.staleInfo(0);
        RealtimeData snapshot;
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getHr(), 156.0);
        QCOMPARE(snapshot.getCadence(), 91.0);

        ant.staleInfo(1);
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getHr(), 0.0);
        QCOMPARE(snapshot.getCadence(), 91.0);
        QCOMPARE(ant.closePort(), 0);
    }

    void lostChannelClearsItsTelemetryAndNotifies()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        QSignalSpy lostDevice(&ant, &ANT::lostDevice);
        ant.setWatts(251.0f, 0);

        ant.lostInfo(0);
        RealtimeData snapshot;
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getWatts(), 0.0);
        QCOMPARE(lostDevice.count(), 1);
        QCOMPARE(ant.closePort(), 0);
    }

    void stalePrimaryCadenceAllowsSecondaryFallback()
    {
        ANT ant;
        QCOMPARE(ant.openPort(), 0);
        ant.setCadence(94.0f, 0);
        ant.setSecondaryCadence(76.0f, 1);

        RealtimeData snapshot;
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getCadence(), 94.0);

        ant.staleInfo(0);
        ant.setSecondaryCadence(76.0f, 1);
        ant.getRealtimeData(snapshot);
        QCOMPARE(snapshot.getCadence(), 76.0);
        QCOMPARE(ant.closePort(), 0);
    }

    void metricSpecificExpiryUsesSuppliedMonotonicTime()
    {
        ANT ant;
        ant.setBPM(149.0f, 0);
        ant.setCadence(88.0f, 1);
        ant.setTemp(22.5, 2);

        const qint64 publishedAt = ant.telemetryNowMs();
        RealtimeData snapshot;
        ant.getRealtimeDataAt(
                snapshot,
                publishedAt + ANTTelemetryFreshness::FastTimeoutMs + 1);
        QCOMPARE(snapshot.getHr(), 0.0);
        QCOMPARE(snapshot.getCadence(), 0.0);
        QCOMPARE(snapshot.getTemp(), 22.5);

        ant.getRealtimeDataAt(
                snapshot,
                publishedAt + ANTTelemetryFreshness::SlowTimeoutMs + 1);
        QCOMPARE(snapshot.getTemp(), 0.0);
    }

    void freshnessTrackerRejectsInvalidAndLowerPrioritySamples()
    {
        ANTTelemetryFreshness freshness;
        QVERIFY(!freshness.publish(-2, ANTTelemetryMetric::HeartRate,
                                   ANTTelemetryPriority::Primary, 0));
        QVERIFY(!freshness.publish(
                0, ANTTelemetryMetric::Count,
                ANTTelemetryPriority::Primary, 0));
        QVERIFY(!freshness.publish(
                0, ANTTelemetryMetric::HeartRate,
                static_cast<ANTTelemetryPriority>(0), 0));
        QVERIFY(!freshness.publish(
                0, ANTTelemetryMetric::HeartRate,
                ANTTelemetryPriority::Primary, -1));
        QVERIFY(freshness.expire(-1).isEmpty());
        QVERIFY(freshness.removeSource(-2).isEmpty());
        QVERIFY(freshness.publish(3, ANTTelemetryMetric::Cadence,
                                  ANTTelemetryPriority::Primary, 10));
        QVERIFY(!freshness.publish(4, ANTTelemetryMetric::Cadence,
                                   ANTTelemetryPriority::Secondary, 11));

        const auto expired = freshness.expire(
                10 + ANTTelemetryFreshness::FastTimeoutMs + 1);
        QVERIFY(expired.contains(ANTTelemetryMetric::Cadence));
        QVERIFY(freshness.publish(
                4, ANTTelemetryMetric::Cadence,
                ANTTelemetryPriority::Secondary,
                10 + ANTTelemetryFreshness::FastTimeoutMs + 1));

        QVERIFY(freshness.removeSource(3).isEmpty());
        const auto removed = freshness.removeSource(4);
        QCOMPARE(removed.size(), 1);
        QVERIFY(removed.contains(ANTTelemetryMetric::Cadence));
        ANTTelemetryFreshness dynamics;
        QVERIFY(dynamics.publish(
                3, ANTTelemetryMetric::RightPowerPhase,
                ANTTelemetryPriority::Primary, 0));
        QVERIFY(dynamics.publish(
                3, ANTTelemetryMetric::Torque,
                ANTTelemetryPriority::Primary,
                ANTTelemetryFreshness::FastTimeoutMs));
        const auto dynamicsExpired =
                dynamics.expire(ANTTelemetryFreshness::FastTimeoutMs + 1);
        QVERIFY(dynamicsExpired.contains(ANTTelemetryMetric::RightPowerPhase));
        QVERIFY(!dynamicsExpired.contains(ANTTelemetryMetric::Torque));
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

        const qint64 publishedAt = ant.telemetryNowMs();
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

        ant.getRealtimeDataAt(
                snapshot,
                publishedAt + ANTTelemetryFreshness::SlowTimeoutMs + 1);
        QCOMPARE(snapshot.getHr(), 0.0);
        QCOMPARE(snapshot.getCadence(), 0.0);
        QCOMPARE(snapshot.getWheelRpm(), 0.0);
        QCOMPARE(snapshot.getSpeed(), 0.0);
        QCOMPARE(snapshot.getAltDistance(), 4.0);
        QCOMPARE(snapshot.getWatts(), 0.0);
        QCOMPARE(snapshot.getAltWatts(), 0.0);
        QCOMPARE(snapshot.getSmO2(), 0.0);
        QCOMPARE(snapshot.gettHb(), 0.0);
        QCOMPARE(snapshot.getCoreTemp(), 0.0);
        QCOMPARE(snapshot.getSkinTemp(), 0.0);
        QCOMPARE(snapshot.getHeatStrain(), 0.0);
        QCOMPARE(snapshot.getTemp(), 0.0);
        QCOMPARE(snapshot.getLRBalance(), 0.0);
        QCOMPARE(snapshot.getLTE(), 0.0);
        QCOMPARE(snapshot.getRTE(), 0.0);
        QCOMPARE(snapshot.getLPS(), 0.0);
        QCOMPARE(snapshot.getRPS(), 0.0);
        QCOMPARE(snapshot.getRppb(), 0.0);
        QCOMPARE(snapshot.getRppe(), 0.0);
        QCOMPARE(snapshot.getRpppb(), 0.0);
        QCOMPARE(snapshot.getRpppe(), 0.0);
        QCOMPARE(snapshot.getLppb(), 0.0);
        QCOMPARE(snapshot.getLppe(), 0.0);
        QCOMPARE(snapshot.getLpppb(), 0.0);
        QCOMPARE(snapshot.getLpppe(), 0.0);
        QCOMPARE(snapshot.getRightPCO(), 0.0);
        QCOMPARE(snapshot.getLeftPCO(), 0.0);
        QCOMPARE(snapshot.getPosition(), RealtimeData::seated);
        QCOMPARE(snapshot.getTorque(), 0.0);
        QVERIFY(!snapshot.getTrainerStatusAvailable());
        QVERIFY(!snapshot.getTrainerCalibRequired());
        QVERIFY(!snapshot.getTrainerConfigRequired());
        QVERIFY(!snapshot.getTrainerBrakeFault());
        QVERIFY(!snapshot.getTrainerReady());
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
    void setupTransportRunsOnWorkerThread()
    {
        ANT ant;
        const quintptr callerThread = currentThreadId();
        QCOMPARE(ant.start(), 0);
        QCOMPARE(ant.setup(), 0);

        const FakeAntTransport::Snapshot transport =
                FakeAntTransport::snapshot();
        QCOMPARE(ant.stop(), 0);

        const QVector<FakeAntTransport::WriteCall> frames =
                messages(transport);
        QVERIFY(!frames.isEmpty());
        for (const FakeAntTransport::WriteCall &frame : frames) {
            QVERIFY2(frame.threadId != callerThread,
                     "ANT::setup wrote transport data outside the worker");
        }
    }

    void runtimeControlAndTimerTransportRunOnWorkerThread()
    {
        ANT ant;
        ant.setVortexData(0, 1234);
        ant.setControlChannel(0);
        QCOMPARE(ant.start(), 0);

        const quintptr callerThread = currentThreadId();
        ant.setLoad(245.0);
        ant.slotControlTimerEvent();
        const bool written = FakeAntTransport::waitForMessageWrites(2);
        const FakeAntTransport::Snapshot transport =
                FakeAntTransport::snapshot();
        QCOMPARE(ant.stop(), 0);

        QVERIFY(written);
        const QVector<FakeAntTransport::WriteCall> frames =
                messages(transport);
        QCOMPARE(frames.size(), 2);
        for (const FakeAntTransport::WriteCall &frame : frames) {
            QVERIFY2(frame.threadId != callerThread,
                     "runtime transport command bypassed the worker");
        }
    }

    void channelShutdownRunsOnWorkerThread()
    {
        ANT ant;
        QCOMPARE(ant.start(), 0);
        ant.setChannel(0, 1234, ANTChannel::CHANNEL_TYPE_POWER);
        QVERIFY(FakeAntTransport::waitForMessageWrites(2));

        const int writesBeforeStop =
                FakeAntTransport::snapshot().messageWrites;
        const quintptr callerThread = currentThreadId();
        QCOMPARE(ant.stop(), 0);
        const FakeAntTransport::Snapshot transport =
                FakeAntTransport::snapshot();
        const QVector<FakeAntTransport::WriteCall> frames =
                messages(transport);

        QVERIFY(frames.size() > writesBeforeStop);
        for (int index = writesBeforeStop; index < frames.size(); ++index) {
            QVERIFY2(frames.at(index).threadId != callerThread,
                     "ANT::stop closed a channel outside the worker");
        }
    }

    void channelDiscoveryFollowupUsesWorkerAndDiscoveredChannel()
    {
        ANT ant;
        QCOMPARE(ant.start(), 0);
        ant.setChannel(0, 0,
                       ANTChannel::CHANNEL_TYPE_FITNESS_EQUIPMENT);
        QVERIFY(FakeAntTransport::waitForMessageWrites(2));

        const int writesBeforeDiscovery =
                FakeAntTransport::snapshot().writes.size();
        const quintptr callerThread = currentThreadId();
        FakeAntTransport::queueReadBytes(antFrame(
                ANT_CHANNEL_ID,
                { 0, 0x34, 0x12, ANT_SPORT_FITNESS_EQUIPMENT_TYPE, 0 }));

        FakeAntTransport::WriteCall capabilityRequest;
        bool found = false;
        QElapsedTimer deadline;
        deadline.start();
        while (!found && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const FakeAntTransport::Snapshot transport =
                    FakeAntTransport::snapshot();
            for (int index = writesBeforeDiscovery;
                 index < transport.writes.size(); ++index) {
                if (isFecCapabilityRequest(transport.writes.at(index))) {
                    capabilityRequest = transport.writes.at(index);
                    found = true;
                    break;
                }
            }
            if (!found) QTest::qWait(1);
        }

        QCOMPARE(ant.stop(), 0);
        QVERIFY2(found, "FE-C discovery did not request capabilities");
        QVERIFY2(capabilityRequest.threadId != callerThread,
                 "FE-C discovery performed transport I/O on the GUI thread");
        QCOMPARE(static_cast<unsigned char>(capabilityRequest.bytes.at(3)),
                 static_cast<unsigned char>(0));
    }

    void channelValuesArePublishedWithoutDataRaces()
    {
        ANT ant;
        ANTChannel *channel = ant.antChannel[0];
        channel->channel_type = ANTChannel::CHANNEL_TYPE_HR;
        const QByteArray low = antFrame(
                ANT_BROADCAST_DATA,
                { 0, 0, 0, 0, 0, 0, 0, 0, 95 });
        const QByteArray high = antFrame(
                ANT_BROADCAST_DATA,
                { 0, 0, 0, 0, 0, 0, 0, 0, 175 });
        QSemaphore started;
        std::atomic<bool> stop(false);

        std::thread writer([&]() {
            quint64 sample = 0;
            channel->receiveMessage(reinterpret_cast<unsigned char *>(
                    const_cast<char *>(low.constData())));
            started.release();
            while (!stop.load(std::memory_order_acquire)) {
                const QByteArray &frame = (sample++ & 1U) ? low : high;
                channel->receiveMessage(reinterpret_cast<unsigned char *>(
                        const_cast<char *>(frame.constData())));
            }
        });

        started.acquire();
        double observed = 0.0;
        for (int sample = 0; sample < 250000; ++sample) {
            observed += ant.channelValue(0);
            observed += ant.channelValue2(0);
        }

        stop.store(true, std::memory_order_release);
        writer.join();
        QVERIFY(std::isfinite(observed));
        QVERIFY(observed > 0.0);
    }

    void controlStatePublicationIsSynchronized()
    {
        ANT ant;
        QCOMPARE(ant.start(), 0);
        QSemaphore ready;
        QSemaphore start;
        std::atomic<bool> finished(false);

        std::thread writer([&]() {
            ready.release();
            start.acquire();
            for (int sample = 0; sample < 5000; ++sample) {
                ant.setMode((sample & 1) ? RT_MODE_ERGO : RT_MODE_SPIN);
                ant.setLoad(100.0 + (sample % 300));
                ant.setGradient(-5.0 + (sample % 20));
            }
            finished.store(true, std::memory_order_release);
        });

        ready.acquire();
        start.release();

        RealtimeData snapshot;
        double observed = 0.0;
        do {
            ant.getRealtimeData(snapshot);
            observed += snapshot.getLoad();
        } while (!finished.load(std::memory_order_acquire));

        writer.join();
        ant.getRealtimeData(snapshot);
        observed += snapshot.getLoad();
        QCOMPARE(ant.stop(), 0);
        QVERIFY(std::isfinite(observed));
    }

    void calibrationPublicationIsSynchronized()
    {
        ANT ant;
        QSemaphore ready;
        QSemaphore start;
        std::atomic<bool> finished(false);
        const QtMessageHandler previousHandler =
                qInstallMessageHandler(discardQtMessage);

        std::thread writer([&]() {
            ready.release();
            start.acquire();
            for (int sample = 0; sample < 5000; ++sample) {
                const uint8_t channel = static_cast<uint8_t>(sample % 8);
                if ((sample % 31) == 0) ant.resetCalibrationState();
                ant.setCalibrationType(channel,
                                       CALIBRATION_TYPE_ZERO_OFFSET);
                ant.setCalibrationState(static_cast<uint8_t>(
                        sample % (CALIBRATION_STATE_FAILURE + 1)));
                ant.setCalibrationTargetSpeed(sample % 50);
                ant.setCalibrationZeroOffset(
                        static_cast<uint16_t>(sample));
                ant.setCalibrationSlope(static_cast<uint16_t>(sample + 1));
                ant.setCalibrationSpindownTime(
                        static_cast<uint16_t>(sample + 2));
                ant.setCalibrationTimestamp(channel, sample);
                ant.setCalibrationRequired(channel, (sample & 1) != 0);
            }
            finished.store(true, std::memory_order_release);
        });

        ready.acquire();
        start.release();

        quint64 observed = 0;
        do {
            observed += ant.getCalibrationType();
            observed += ant.getCalibrationState();
            observed += static_cast<quint64>(
                    ant.getCalibrationTargetSpeed());
            observed += ant.getCalibrationZeroOffset();
            observed += ant.getCalibrationSlope();
            observed += ant.getCalibrationSpindownTime();
            observed += ant.getCalibrationChannel();
        } while (!finished.load(std::memory_order_acquire));

        writer.join();
        observed += ant.getCalibrationZeroOffset();
        qInstallMessageHandler(previousHandler);
        QVERIFY(observed > 0);
    }

};

QTEST_GUILESS_MAIN(TestAntThreadSafety)

#include "testAntThreadSafety.moc"
