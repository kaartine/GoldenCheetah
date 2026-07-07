/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/TrainingTelemetryTimeline.h"

#include <QTest>

#include <limits>

class TestTrainingTelemetryTimeline : public QObject
{
    Q_OBJECT

private:
    using Channel = TrainingTelemetryTimeline::Channel;
    using SessionState = TrainingTelemetryTimeline::SessionState;
    using Timeline = TrainingTelemetryTimeline::Timeline;

    static QList<Channel> channels()
    {
        return {
            Channel::Rr,
            Channel::Position,
            Channel::CoreTemperature,
            Channel::Vo2
        };
    }

private slots:
    void pausedAndCalibrationSamplesAreRejectedForEveryChannel()
    {
        Timeline timeline;

        for (Channel channel : channels()) {
            const auto beforePause = timeline.timestamp(
                    channel, SessionState{true, true, false, false, 1000, 100});
            QVERIFY(beforePause.accepted);
            QCOMPARE(beforePause.msecs, qint64(1100));

            const auto duringPause = timeline.timestamp(
                    channel, SessionState{true, true, true, false, 1100, 5000});
            QVERIFY(!duringPause.accepted);

            const auto afterPause = timeline.timestamp(
                    channel, SessionState{true, true, false, false, 1100, 0});
            QVERIFY(afterPause.accepted);
            QVERIFY(afterPause.msecs > beforePause.msecs);

            const auto duringCalibration = timeline.timestamp(
                    channel, SessionState{true, true, false, true,
                                          afterPause.msecs, 5000});
            QVERIFY(!duringCalibration.accepted);

            const auto afterCalibration = timeline.timestamp(
                    channel, SessionState{true, true, false, false,
                                          afterPause.msecs, 0});
            QVERIFY(afterCalibration.accepted);
            QVERIFY(afterCalibration.msecs > afterPause.msecs);
        }
    }

    void inactiveSessionOrRecordingRejectsSamples()
    {
        Timeline timeline;

        for (Channel channel : channels()) {
            const auto stopped = timeline.timestamp(
                    channel, SessionState{false, true, false, false, 1000, 10});
            QVERIFY(!stopped.accepted);

            const auto notRecording = timeline.timestamp(
                    channel, SessionState{true, false, false, false, 1000, 10});
            QVERIFY(!notRecording.accepted);

            const auto active = timeline.timestamp(
                    channel, SessionState{true, true, false, false, 1000, 10});
            QVERIFY(active.accepted);
            QCOMPARE(active.msecs, qint64(1010));
        }
    }

    void timestampUsesAccumulatedActiveTime()
    {
        Timeline timeline;

        const auto sample = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 2000, 300});

        QVERIFY(sample.accepted);
        QCOMPARE(sample.msecs, qint64(2300));
    }

    void timestampStrictlyIncreasesWhenRawClockStallsOrMovesBack()
    {
        Timeline timeline;

        const auto first = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 1000, 0});
        const auto stalled = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 1000, 0});
        const auto movedBack = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 900, 0});

        QCOMPARE(first.msecs, qint64(1000));
        QCOMPARE(stalled.msecs, qint64(1001));
        QCOMPARE(movedBack.msecs, qint64(1002));
    }

    void channelsTrackMonotonicityIndependently()
    {
        Timeline timeline;

        const auto rr = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 1000, 0});
        const auto position = timeline.timestamp(
                Channel::Position,
                SessionState{true, true, false, false, 500, 0});
        const auto rrMovedBack = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, 900, 0});

        QCOMPARE(rr.msecs, qint64(1000));
        QCOMPARE(position.msecs, qint64(500));
        QCOMPARE(rrMovedBack.msecs, qint64(1001));
    }

    void invalidChannelIsRejected()
    {
        Timeline timeline;

        const auto sample = timeline.timestamp(
                static_cast<Channel>(999),
                SessionState{true, true, false, false, 1000, 0});

        QVERIFY(!sample.accepted);
        QCOMPARE(sample.msecs, qint64(0));
    }

    void negativeAndOverflowingClockValuesAreHandled()
    {
        Timeline timeline;

        const auto negative = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, -100, -20});
        QVERIFY(negative.accepted);
        QCOMPARE(negative.msecs, qint64(0));

        const qint64 maximum = std::numeric_limits<qint64>::max();
        const auto saturated = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, maximum - 2, 10});
        QVERIFY(saturated.accepted);
        QCOMPARE(saturated.msecs, maximum);

        const auto exhausted = timeline.timestamp(
                Channel::Rr,
                SessionState{true, true, false, false, maximum, 0});
        QVERIFY(!exhausted.accepted);
        QCOMPARE(exhausted.msecs, qint64(0));
    }

    void resetStartsANewSessionTimeline()
    {
        Timeline timeline;

        const auto oldSession = timeline.timestamp(
                Channel::Vo2,
                SessionState{true, true, false, false, 5000, 0});
        QCOMPARE(oldSession.msecs, qint64(5000));

        timeline.reset();

        const auto newSession = timeline.timestamp(
                Channel::Vo2,
                SessionState{true, true, false, false, 0, 0});
        QCOMPARE(newSession.msecs, qint64(0));
    }
};

QTEST_MAIN(TestTrainingTelemetryTimeline)
#include "testTrainingTelemetryTimeline.moc"
