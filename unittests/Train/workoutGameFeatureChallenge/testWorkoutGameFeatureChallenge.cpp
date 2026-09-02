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

    void bermIsCourseGeometryNotAScoredChallenge()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Berm;
        section.challengeCount = 4;
        section.difficulty = 1.0;

        QVERIFY(!WorkoutGameFeatureChallenge::profile(section).enabled);
    }

    void terrainSelectsSkillButUsesOneVisiblePowerRequirement()
    {
        struct Expected {
            WorkoutGameTerrainKind terrain;
            WorkoutGameChallengeCue cue;
        };
        for (const Expected &expected : {
                Expected{WorkoutGameTerrainKind::Roots,
                         WorkoutGameChallengeCue::CarrySpeed},
                Expected{WorkoutGameTerrainKind::Rollers,
                         WorkoutGameChallengeCue::CarrySpeed},
                Expected{WorkoutGameTerrainKind::Climb,
                         WorkoutGameChallengeCue::Climb},
                Expected{WorkoutGameTerrainKind::RockGarden,
                         WorkoutGameChallengeCue::CarrySpeed},
                Expected{WorkoutGameTerrainKind::BunnyHop,
                         WorkoutGameChallengeCue::Jump},
                Expected{WorkoutGameTerrainKind::Drop,
                         WorkoutGameChallengeCue::CarrySpeed},
                Expected{WorkoutGameTerrainKind::Skinny,
                         WorkoutGameChallengeCue::HoldLine},
                Expected{WorkoutGameTerrainKind::LogOver,
                         WorkoutGameChallengeCue::Jump},
                Expected{WorkoutGameTerrainKind::Tabletop,
                         WorkoutGameChallengeCue::Jump},
                Expected{WorkoutGameTerrainKind::GapJump,
                         WorkoutGameChallengeCue::Jump},
                Expected{WorkoutGameTerrainKind::RockSlab,
                         WorkoutGameChallengeCue::CarrySpeed}}) {
            WorkoutGameSection section;
            section.terrain = expected.terrain;
            section.challengeCount = 1;
            section.difficulty = 0.5;
            const WorkoutGameFeatureChallengeProfile profile =
                    WorkoutGameFeatureChallenge::profile(section);
            QVERIFY(profile.enabled);
            QCOMPARE(profile.cue, expected.cue);
            QCOMPARE(profile.minimumEffortRatio, 1.0);
            QCOMPARE(profile.minimumCadenceRpm, 0.0);
            QCOMPARE(profile.minimumSpeedKph, 0.0);
            QCOMPARE(profile.maximumSpeedKph, 0.0);
            QCOMPARE(profile.minimumAdherence, 0.0);
            QVERIFY(profile.measurementStartProgress >= 0.0);
            QVERIFY(profile.measurementStartProgress
                    < profile.decisionProgress);
            QVERIFY(profile.decisionProgress > 0.5);
            QVERIFY(profile.decisionProgress <= 1.0);
            QVERIFY(profile.bonusPoints > 0u);
        }
    }

    void gapJumpUsesTheVisibleJumpEffortProfile()
    {
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::GapJump;
        section.challengeCount = 1;
        section.difficulty = 0.5;

        const WorkoutGameFeatureChallengeProfile profile =
                WorkoutGameFeatureChallenge::profile(section);
        QVERIFY(profile.enabled);
        QCOMPARE(profile.cue, WorkoutGameChallengeCue::Jump);
        QCOMPARE(profile.measurementStartProgress, 0.62);
        QCOMPARE(profile.decisionProgress, 0.72);
        QCOMPARE(profile.minimumEffortRatio, 1.0);
        QVERIFY(profile.bonusPoints > 0u);
    }

    void difficultyAndChallengeCountScaleBonusNotPowerThreshold()
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

        QCOMPARE(easy.minimumEffortRatio, 1.0);
        QCOMPARE(hard.minimumEffortRatio, 1.0);
        QVERIFY(hard.bonusPoints > easy.bonusPoints);
        QCOMPARE(hard.bonusPoints, std::uint64_t(1400));
    }

    void targetPowerSelectsMainLineWithoutHiddenRequirements()
    {
        WorkoutGameSection section;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.challengeCount = 1;
        const WorkoutGameFeatureChallengeProfile profile =
                WorkoutGameFeatureChallenge::profile(section);
        WorkoutGameFeatureChallengeMetrics metrics;
        metrics.averageEffortRatio = 1.0;
        metrics.averageCadenceRpm = 0.0;
        metrics.averageSpeedKph = 0.0;
        metrics.averageAdherence = 0.0;

        const WorkoutGameFeatureChallengeAssessment ready =
                WorkoutGameFeatureChallenge::assess(profile, metrics);
        QCOMPARE(ready.readiness, 1.0);
        QVERIFY(ready.completed);

        metrics.averageEffortRatio = 0.999;
        metrics.averageCadenceRpm = 200.0;
        metrics.averageSpeedKph = 80.0;
        metrics.averageAdherence = 1.0;
        const WorkoutGameFeatureChallengeAssessment belowTarget =
                WorkoutGameFeatureChallenge::assess(profile, metrics);
        QVERIFY(std::abs(belowTarget.readiness - 0.999) < 1e-9);
        QVERIFY(!belowTarget.completed);
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
