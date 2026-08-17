/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCompetition.h"

#include <QTest>

#include <limits>
#include <string>

namespace {

WorkoutGameCourse course(std::uint32_t seed = 123u)
{
    return WorkoutGameCourseBuilder::build(
            {{0, 60000, 180.0, 180.0}}, 200.0, seed);
}

WorkoutGameSimulationSnapshot player(
        std::int64_t timeMs,
        std::uint64_t score,
        double progress)
{
    WorkoutGameSimulationSnapshot snapshot;
    snapshot.ready = true;
    snapshot.workoutTimeMs = timeMs;
    snapshot.score = score;
    snapshot.courseProgress = progress;
    snapshot.route = WorkoutGameRoute::MainLine;
    return snapshot;
}

WorkoutGameGhostReplay ghost(std::uint32_t seed = 123u)
{
    WorkoutGameGhostReplay replay;
    replay.courseSeed = seed;
    replay.durationMs = 60000;
    replay.points = {
        {0, 0, WorkoutGameRoute::MainLine},
        {10000, 1000, WorkoutGameRoute::MainLine},
        {20000, 2500, WorkoutGameRoute::SafeBypass}
    };
    return replay;
}

}

class TestWorkoutGameCompetition : public QObject
{
    Q_OBJECT

private slots:
    void invalidCourseIsRejected()
    {
        WorkoutGameCompetition competition;
        QVERIFY(!competition.configure(WorkoutGameCourse(), {}));
        QVERIFY(!competition.update(player(1000, 100, 0.1)).ready);
    }

    void sameSeedProducesIdenticalAiRiders()
    {
        WorkoutGameCompetition first;
        WorkoutGameCompetition second;
        QVERIFY(first.configure(course(9u), {}));
        QVERIFY(second.configure(course(9u), {}));

        const auto a = first.update(player(20000, 1800, 1.0 / 3.0));
        const auto b = second.update(player(20000, 1800, 1.0 / 3.0));
        QCOMPARE(a.competitors.size(), b.competitors.size());
        for (std::size_t index = 0; index < a.competitors.size(); ++index) {
            QCOMPARE(a.competitors[index].score, b.competitors[index].score);
            QCOMPARE(a.competitors[index].courseProgress,
                     b.competitors[index].courseProgress);
        }
    }

    void differentSeedsChangeAiPacing()
    {
        WorkoutGameCompetition first;
        WorkoutGameCompetition second;
        QVERIFY(first.configure(course(9u), {}));
        QVERIFY(second.configure(course(10u), {}));

        const auto a = first.update(player(20000, 1800, 1.0 / 3.0));
        const auto b = second.update(player(20000, 1800, 1.0 / 3.0));
        QVERIFY(a.competitors.front().score != b.competitors.front().score);
    }

    void aiScoresAndProgressNeverMoveBackwards()
    {
        WorkoutGameCompetition competition;
        QVERIFY(competition.configure(course(), {}));
        const auto early = competition.update(player(10000, 800, 1.0 / 6.0));
        const auto later = competition.update(player(30000, 2600, 0.5));

        QCOMPARE(early.competitors.size(), std::size_t(3));
        for (std::size_t index = 0; index < early.competitors.size(); ++index) {
            QVERIFY(later.competitors[index].score
                    >= early.competitors[index].score);
            QVERIFY(later.competitors[index].courseProgress
                    >= early.competitors[index].courseProgress);
        }
    }

    void playerRankUsesPerformanceScore()
    {
        WorkoutGameCompetition competition;
        QVERIFY(competition.configure(course(), {}));

        const auto leading = competition.update(player(30000, 100000, 0.5));
        const auto trailing = competition.update(player(30000, 0, 0.5));

        QCOMPARE(leading.playerRank, 1);
        QCOMPARE(leading.totalRiders, 4);
        QVERIFY(trailing.playerRank > leading.playerRank);
    }

    void ghostScoreAndRouteAreInterpolatedWithoutExtrapolation()
    {
        WorkoutGameCompetition competition;
        QVERIFY(competition.configure(course(), ghost()));

        const auto middle = competition.update(player(15000, 1200, 0.25));
        QCOMPARE(middle.competitors.size(), std::size_t(4));
        const auto &rider = middle.competitors.back();
        QCOMPARE(rider.kind, WorkoutGameCompetitorKind::Ghost);
        QCOMPARE(rider.score, std::uint64_t(1750));
        QCOMPARE(rider.route, WorkoutGameRoute::MainLine);

        const auto after = competition.update(player(25001, 2600, 0.42));
        QCOMPARE(after.competitors.size(), std::size_t(3));
    }

    void ghostForAnotherCourseIsIgnored()
    {
        WorkoutGameCompetition competition;
        QVERIFY(competition.configure(course(123u), ghost(456u)));
        QCOMPARE(competition.update(player(10000, 1000, 0.2)).competitors.size(),
                 std::size_t(3));
    }

    void malformedGhostCodecInputIsRejected()
    {
        WorkoutGameGhostReplay decoded;
        QVERIFY(!WorkoutGameGhostCodec::decode("", decoded));
        QVERIFY(!WorkoutGameGhostCodec::decode("WG1|123|60000|100,1,0;50,2,0", decoded));
        QVERIFY(!WorkoutGameGhostCodec::decode("WG1|123|60000|0,2,0;10,1,0", decoded));
        QVERIFY(!WorkoutGameGhostCodec::decode("WG1|123|60000|0,1,9", decoded));
        QVERIFY(!WorkoutGameGhostCodec::decode(
                std::string(WorkoutGameGhostCodec::MaximumEncodedBytes + 1, 'x'),
                decoded));
    }

    void ghostCodecRoundTripsCanonicalReplay()
    {
        const WorkoutGameGhostReplay expected = ghost();
        const std::string encoded = WorkoutGameGhostCodec::encode(expected);
        WorkoutGameGhostReplay decoded;

        QVERIFY(!encoded.empty());
        QVERIFY(WorkoutGameGhostCodec::decode(encoded, decoded));
        QCOMPARE(decoded.courseSeed, expected.courseSeed);
        QCOMPARE(decoded.durationMs, expected.durationMs);
        QCOMPARE(decoded.points.size(), expected.points.size());
        QCOMPARE(WorkoutGameGhostCodec::encode(decoded), encoded);
    }

    void recorderSamplesAtBoundedCadenceAndKeepsRouteChanges()
    {
        WorkoutGameGhostRecorder recorder;
        QVERIFY(recorder.configure(123u, 60000));
        for (std::int64_t time = 0; time <= 10000; time += 100) {
            auto snapshot = player(time, std::uint64_t(time / 10),
                                   double(time) / 60000.0);
            if (time == 7300) snapshot.route = WorkoutGameRoute::SafeBypass;
            recorder.record(snapshot);
        }

        const auto replay = recorder.replay();
        QVERIFY(replay.points.size() <= std::size_t(5));
        QVERIFY(replay.points.size() >= std::size_t(3));
        bool sawBypass = false;
        for (const auto &point : replay.points) {
            if (point.route == WorkoutGameRoute::SafeBypass) sawBypass = true;
        }
        QVERIFY(sawBypass);
    }

    void recorderIgnoresStopTimeRewind()
    {
        WorkoutGameGhostRecorder recorder;
        QVERIFY(recorder.configure(123u, 60000));
        recorder.record(player(0, 0, 0.0));
        recorder.record(player(10000, 1000, 1.0 / 6.0));
        recorder.record(player(0, 0, 0.0));

        const auto replay = recorder.replay();
        QCOMPARE(replay.points.back().timeMs, std::int64_t(10000));
        QCOMPARE(replay.points.back().score, std::uint64_t(1000));
    }

    void betterGhostPrefersCoverageThenScore()
    {
        WorkoutGameGhostReplay shortFast = ghost();
        shortFast.points.back().score = 99999;
        WorkoutGameGhostReplay longSlow = ghost();
        longSlow.points.push_back(
                {30000, 2600, WorkoutGameRoute::MainLine});

        QVERIFY(WorkoutGameGhostCodec::isBetter(longSlow, shortFast));
        QVERIFY(!WorkoutGameGhostCodec::isBetter(shortFast, longSlow));

        WorkoutGameGhostReplay faster = longSlow;
        faster.points.back().score += 1;
        QVERIFY(WorkoutGameGhostCodec::isBetter(faster, longSlow));
    }

    void nonFinitePlayerProgressFailsClosed()
    {
        WorkoutGameCompetition competition;
        QVERIFY(competition.configure(course(), {}));
        auto invalid = player(10000, 1000, 0.2);
        invalid.courseProgress = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!competition.update(invalid).ready);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCompetition)
#include "testWorkoutGameCompetition.moc"
