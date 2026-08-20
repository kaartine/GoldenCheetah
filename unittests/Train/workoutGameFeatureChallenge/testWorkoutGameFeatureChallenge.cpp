/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureChallenge.h"

#include <QTest>

#include <cmath>
#include <limits>

class TestWorkoutGameFeatureChallenge : public QObject
{
    Q_OBJECT

private slots:
    void sectionsWithoutChallengesAreDisabled()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Tabletop;

        QVERIFY(!WorkoutGameFeatureChallenge::profile(section).enabled);

        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.challengeCount = 1;
        QVERIFY(!WorkoutGameFeatureChallenge::profile(section).enabled);
    }

    void terrainSelectsSkillAndRequirements()
    {
        struct Expected {
            WorkoutGameTerrainKind terrain;
            WorkoutGameChallengeCue cue;
            bool speedRequired;
        };
        for (const Expected &expected : {
                Expected{WorkoutGameTerrainKind::Roots,
                         WorkoutGameChallengeCue::CarrySpeed, true},
                Expected{WorkoutGameTerrainKind::Rollers,
                         WorkoutGameChallengeCue::CarrySpeed, true},
                Expected{WorkoutGameTerrainKind::Climb,
                         WorkoutGameChallengeCue::Climb, false},
                Expected{WorkoutGameTerrainKind::RockGarden,
                         WorkoutGameChallengeCue::CarrySpeed, true},
                Expected{WorkoutGameTerrainKind::BunnyHop,
                         WorkoutGameChallengeCue::Jump, true},
                Expected{WorkoutGameTerrainKind::Drop,
                         WorkoutGameChallengeCue::CarrySpeed, true},
                Expected{WorkoutGameTerrainKind::Skinny,
                         WorkoutGameChallengeCue::HoldLine, true},
                Expected{WorkoutGameTerrainKind::Berm,
                         WorkoutGameChallengeCue::CarrySpeed, true},
                Expected{WorkoutGameTerrainKind::LogOver,
                         WorkoutGameChallengeCue::Jump, true},
                Expected{WorkoutGameTerrainKind::Tabletop,
                         WorkoutGameChallengeCue::Jump, true},
                Expected{WorkoutGameTerrainKind::RockSlab,
                         WorkoutGameChallengeCue::CarrySpeed, true}}) {
            WorkoutGameSection section;
            section.terrain = expected.terrain;
            section.challengeCount = 1;
            section.difficulty = 0.5;
            const WorkoutGameFeatureChallengeProfile profile =
                    WorkoutGameFeatureChallenge::profile(section);
            QVERIFY(profile.enabled);
            QCOMPARE(profile.cue, expected.cue);
            QCOMPARE(profile.minimumSpeedKph > 0.0, expected.speedRequired);
            QVERIFY(profile.measurementStartProgress >= 0.0);
            QVERIFY(profile.measurementStartProgress
                    < profile.decisionProgress);
            QVERIFY(profile.decisionProgress > 0.5);
            QVERIFY(profile.decisionProgress <= 1.0);
            QVERIFY(profile.bonusPoints > 0u);
        }
    }

    void difficultyAndChallengeCountScaleRequirementsAndBonus()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Roots;
        section.challengeCount = 1;
        const WorkoutGameFeatureChallengeProfile easy =
                WorkoutGameFeatureChallenge::profile(section);

        section.difficulty = 1.0;
        section.challengeCount = 15;
        const WorkoutGameFeatureChallengeProfile hard =
                WorkoutGameFeatureChallenge::profile(section);

        QVERIFY(hard.minimumEffortRatio > easy.minimumEffortRatio);
        QVERIFY(hard.minimumSpeedKph > easy.minimumSpeedKph);
        QVERIFY(hard.bonusPoints > easy.bonusPoints);
        QCOMPARE(hard.bonusPoints, std::uint64_t(1400));
    }

    void allRequirementsMustBeMet()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.challengeCount = 1;
        const WorkoutGameFeatureChallengeProfile profile =
                WorkoutGameFeatureChallenge::profile(section);
        WorkoutGameFeatureChallengeMetrics metrics;
        metrics.averageEffortRatio = 1.0;
        metrics.averageCadenceRpm = 85.0;
        metrics.averageSpeedKph = 20.0;
        metrics.averageAdherence = 1.0;

        const WorkoutGameFeatureChallengeAssessment ready =
                WorkoutGameFeatureChallenge::assess(profile, metrics);
        QCOMPARE(ready.readiness, 1.0);
        QVERIFY(ready.completed);

        metrics.averageSpeedKph = profile.minimumSpeedKph * 0.5;
        const WorkoutGameFeatureChallengeAssessment slow =
                WorkoutGameFeatureChallenge::assess(profile, metrics);
        QVERIFY(std::abs(slow.readiness - 0.5) < 1e-9);
        QVERIFY(!slow.completed);
    }

    void skinnyRejectsExcessiveSpeed()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Skinny;
        section.challengeCount = 1;
        const WorkoutGameFeatureChallengeProfile profile =
                WorkoutGameFeatureChallenge::profile(section);
        WorkoutGameFeatureChallengeMetrics metrics;
        metrics.averageEffortRatio = 1.0;
        metrics.averageCadenceRpm = 85.0;
        metrics.averageSpeedKph = profile.maximumSpeedKph * 2.0;
        metrics.averageAdherence = 1.0;

        const WorkoutGameFeatureChallengeAssessment result =
                WorkoutGameFeatureChallenge::assess(profile, metrics);
        QVERIFY(std::abs(result.readiness - 0.5) < 1e-9);
        QVERIFY(!result.completed);
    }

    void invalidMetricsFailSafely()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::RockGarden;
        section.challengeCount = 1;
        WorkoutGameFeatureChallengeMetrics metrics;
        metrics.averageEffortRatio = std::numeric_limits<double>::quiet_NaN();
        metrics.averageCadenceRpm = std::numeric_limits<double>::infinity();

        const WorkoutGameFeatureChallengeAssessment result =
                WorkoutGameFeatureChallenge::assess(
                    WorkoutGameFeatureChallenge::profile(section), metrics);
        QCOMPARE(result.readiness, 0.0);
        QVERIFY(!result.completed);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureChallenge)
#include "testWorkoutGameFeatureChallenge.moc"
