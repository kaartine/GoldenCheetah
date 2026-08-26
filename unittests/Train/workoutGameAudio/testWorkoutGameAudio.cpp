/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAudioEvents.h"

#include <QFile>
#include <QTest>

#include <limits>

namespace {

WorkoutGameFeatureRuntimeSnapshot feature(
        WorkoutGameFeaturePhase phase,
        std::uint64_t actionId)
{
    WorkoutGameFeatureRuntimeSnapshot result;
    result.ready = true;
    result.phase = phase;
    result.actionId = actionId;
    return result;
}

WorkoutGameWorldSnapshot world(double landingImpact = 0.0)
{
    WorkoutGameWorldSnapshot result;
    result.ready = true;
    result.landingImpact = landingImpact;
    return result;
}

WorkoutGameAudioEvents consume(
        WorkoutGameAudioCueTracker &tracker,
        const WorkoutGameAudioEventJournal &journal)
{
    return tracker.update(journal);
}

}

class TestWorkoutGameAudio : public QObject
{
    Q_OBJECT

private slots:
    void packagedCuesAreValidWaveResources()
    {
        const QString paths[] = {
            QStringLiteral(":/audio/workout-game-feature.wav"),
            QStringLiteral(":/audio/workout-game-landing.wav")
        };
        for (const QString &path : paths) {
            QFile cue(path);
            QVERIFY2(cue.open(QIODevice::ReadOnly), qPrintable(path));
            QVERIFY(cue.size() > 1000);
            const QByteArray header = cue.read(12);
            QCOMPARE(header.left(4), QByteArray("RIFF"));
            QCOMPARE(header.mid(8, 4), QByteArray("WAVE"));
        }
    }

    void unavailableStateProducesNoCue()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;

        const WorkoutGameAudioEventJournal journal = builder.update(
                {}, {}, 1000);
        QCOMPARE(journal.count, std::size_t(0));
        const WorkoutGameAudioEvents events = consume(tracker, journal);
        QVERIFY(!events.feature);
        QVERIFY(!events.landing);
    }

    void measurementProducesOneFeatureCuePerAction()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;

        consume(tracker, builder.update(feature(
                WorkoutGameFeaturePhase::Approach, 41), world(), 1000));
        QVERIFY(consume(tracker, builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 41), world(), 1100)).feature);
        QVERIFY(!consume(tracker, builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 41), world(), 1200)).feature);
        QVERIFY(!consume(tracker, builder.update(feature(
                WorkoutGameFeaturePhase::Committed, 41), world(), 1300)).feature);
        QVERIFY(consume(tracker, builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 42), world(), 1400)).feature);
    }

    void overwrittenFramesCannotLoseAnEvent()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;

        builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 51), world(), 1000);
        builder.update(feature(
                WorkoutGameFeaturePhase::Committed, 51), world(), 1020);
        const WorkoutGameAudioEventJournal latest = builder.update(feature(
                WorkoutGameFeaturePhase::Action, 51), world(), 1040);

        QVERIFY(consume(tracker, latest).feature);
        QVERIFY(!consume(tracker, latest).feature);
    }

    void journalIsBoundedAndRetainsNewestEvents()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioEventJournal journal;
        const std::size_t eventCount =
                WorkoutGameAudioEventJournal::Capacity + 3;
        for (std::size_t index = 0; index < eventCount; ++index) {
            journal = builder.update(feature(
                    WorkoutGameFeaturePhase::Measure,
                    std::uint64_t(index + 1)), world(),
                    std::int64_t(1000 + index * 20));
        }

        QCOMPARE(journal.count, WorkoutGameAudioEventJournal::Capacity);
        QCOMPARE(journal.events.front().id, std::uint64_t(4));
        QCOMPARE(journal.events[journal.count - 1].id,
                 std::uint64_t(eventCount));
    }

    void landingUsesAuthoritativeRisingEdgeAndBoundedStrength()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;
        const auto action = feature(WorkoutGameFeaturePhase::Action, 71);

        QVERIFY(!consume(tracker, builder.update(
                action, world(0.08), 1000)).landing);
        const WorkoutGameAudioEvents first = consume(tracker, builder.update(
                action, world(0.42), 1100));
        QVERIFY(first.landing);
        QCOMPARE(first.landingStrength, 0.42);
        QVERIFY(!consume(tracker, builder.update(
                action, world(0.65), 1200)).landing);
        QVERIFY(!consume(tracker, builder.update(
                action, world(0.0), 1300)).landing);
        const WorkoutGameAudioEvents bounded = consume(
                tracker, builder.update(action, world(4.0), 1400));
        QVERIFY(bounded.landing);
        QCOMPARE(bounded.landingStrength, 1.0);
    }

    void invalidLandingImpactCannotTriggerAudio()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;
        const double invalid[] = {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            -1.0
        };
        std::int64_t timeMs = 1000;
        for (double impact : invalid) {
            const WorkoutGameAudioEvents events = consume(
                    tracker, builder.update(feature(
                        WorkoutGameFeaturePhase::Action, 91),
                        world(impact), timeMs));
            QVERIFY(!events.landing);
            QCOMPARE(events.landingStrength, 0.0);
            timeMs += 20;
        }
    }

    void resetAndBackwardSeekStartANewJournalEpoch()
    {
        WorkoutGameAudioEventJournalBuilder builder;
        WorkoutGameAudioCueTracker tracker;
        auto journal = builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 101), world(), 3000);
        QVERIFY(consume(tracker, journal).feature);
        QVERIFY(!consume(tracker, journal).feature);

        const std::uint64_t firstEpoch = journal.epoch;
        journal = builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 101), world(), 2000);
        QVERIFY(journal.epoch != firstEpoch);
        QVERIFY(consume(tracker, journal).feature);

        builder.reset();
        journal = builder.update(feature(
                WorkoutGameFeaturePhase::Measure, 101), world(), 4000);
        QVERIFY(consume(tracker, journal).feature);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameAudio)
#include "testWorkoutGameAudio.moc"
