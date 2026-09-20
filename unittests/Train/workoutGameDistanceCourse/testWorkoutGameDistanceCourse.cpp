/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameDistanceCourse.h"
#include "Train/WorkoutGameGapJumpGeometry.h"

#include <QTest>

#include <cmath>
#include <limits>
#include <set>

namespace {

void appendInterval(
        std::vector<WorkoutGameInterval> &intervals,
        std::int64_t durationMs,
        double startWatts,
        double endWatts)
{
    const std::int64_t startMs = intervals.empty()
            ? 0
            : intervals.back().startMs + intervals.back().durationMs;
    intervals.push_back({startMs, durationMs, startWatts, endWatts});
}

std::vector<WorkoutGameInterval> fiveByFourWorkout()
{
    std::vector<WorkoutGameInterval> intervals;
    appendInterval(intervals, 18 * 60000, 100.0, 175.0);
    appendInterval(intervals, 60000, 110.0, 110.0);
    const double efforts[] = {205.0, 207.0, 210.0, 210.0, 212.0};
    const double kicks[] = {235.0, 240.0, 245.0, 250.0, 255.0};
    for (int repetition = 0; repetition < 5; ++repetition) {
        appendInterval(intervals, 230000, efforts[repetition], efforts[repetition]);
        appendInterval(intervals, 10000, kicks[repetition], kicks[repetition]);
        appendInterval(intervals, 180000,
                       repetition == 4 ? 120.0 : 115.0,
                       repetition == 4 ? 120.0 : 115.0);
    }
    appendInterval(intervals, 6 * 60000, 120.0, 90.0);
    return intervals;
}

}

class TestWorkoutGameDistanceCourse : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputsAreRejected()
    {
        const WorkoutGameDistanceCourseGenerationParameters defaults;
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build({}, 190.0).status,
                 WorkoutGameDistanceCourseStatus::EmptyWorkout);
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build(
                    {{0, 60000, 150.0, 150.0}}, 0.0).status,
                 WorkoutGameDistanceCourseStatus::InvalidFtp);

        WorkoutGameDistanceCourseGenerationParameters invalid = defaults;
        invalid.roadPhysics.totalMassKg = 0.0;
        QCOMPARE(WorkoutGameDistanceCourseBuilder::build(
                    {{0, 60000, 150.0, 150.0}}, 190.0, invalid).status,
                 WorkoutGameDistanceCourseStatus::InvalidParameters);
    }

    void excessiveWorkoutIsRejectedBeforeSimulation()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        const std::int64_t excessive = parameters.maximumWorkoutDurationMs + 1;

        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    {{0, excessive, 150.0, 150.0}}, 190.0, parameters);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::ResourceLimit);
        QVERIFY(course.sections.empty());
    }

    void fiveByFourWorkoutProducesACompleteMtbCourse()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 123u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(course.seed, std::uint32_t(123));
        QCOMPARE(course.nominalDurationMs, std::int64_t(60 * 60000));
        QVERIFY(course.sections.size() > std::size_t(18));
        QVERIFY(course.totalDistanceMeters > 10000.0);
        QVERIFY(course.totalDistanceMeters < 30000.0);
        QVERIFY(course.elevationGainMeters > 100.0);
        QVERIFY(course.elevationLossMeters < course.elevationGainMeters);

        int climbs = 0;
        int jumps = 0;
        int recoveries = 0;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            QVERIFY(section.lengthMeters > 0.0);
            QVERIFY(section.maximumDurationMs >= section.nominalDurationMs);
            QVERIFY(section.minimumDurationMs <= section.nominalDurationMs);
            climbs += section.feature == WorkoutGameFeature::Climb ? 1 : 0;
            jumps += section.feature == WorkoutGameFeature::SprintJump ? 1 : 0;
            recoveries += section.feature
                    == WorkoutGameFeature::RecoveryDescent ? 1 : 0;
        }
        QVERIFY(climbs >= 5);
        QVERIFY(jumps >= 5);
        QVERIFY(recoveries >= 6);
    }

    void zeroVariationPreservesTheSourceEffortProfile()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.terrainVariationPercent = 0.0;
        parameters.variationLengthMeters = 60.0;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    {{0, 10 * 60000, 170.0, 230.0}},
                    200.0, parameters, 44u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QVERIFY(course.sections.size() > std::size_t(1));
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            const double sourceAverage =
                    (section.targetStartWatts + section.targetEndWatts) * 0.5;
            const double terrainAverage = (section.referenceEffortStartWatts
                    + section.referenceEffortEndWatts) * 0.5;
            QVERIFY(std::abs(sourceAverage - terrainAverage) < 0.01);
        }
    }

    void terrainVariationIsDeterministicBoundedAndAveragePreserving()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.terrainVariationPercent = 15.0;
        parameters.variationLengthMeters = 60.0;
        const std::vector<WorkoutGameInterval> workout = {
            {0, 20 * 60000, 200.0, 200.0}
        };
        const WorkoutGameDistanceCourse first =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 200.0, parameters, 901u);
        const WorkoutGameDistanceCourse repeat =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 200.0, parameters, 901u);

        QCOMPARE(first.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(first.sections.size(), repeat.sections.size());
        QVERIFY(first.sections.size() > std::size_t(20));
        double weightedEffort = 0.0;
        double totalDuration = 0.0;
        bool below = false;
        bool above = false;
        for (std::size_t index = 0; index < first.sections.size(); ++index) {
            const WorkoutGameDistanceCourseSection &section =
                    first.sections[index];
            const double effort = (section.referenceEffortStartWatts
                    + section.referenceEffortEndWatts) * 0.5;
            QCOMPARE(section.referenceEffortStartWatts,
                     repeat.sections[index].referenceEffortStartWatts);
            QCOMPARE(section.referenceEffortEndWatts,
                     repeat.sections[index].referenceEffortEndWatts);
            QVERIFY(effort >= 170.0 - 0.01);
            QVERIFY(effort <= 230.0 + 0.01);
            below |= effort < 195.0;
            above |= effort > 205.0;
            weightedEffort += effort * section.nominalDurationMs;
            totalDuration += section.nominalDurationMs;
            if (index + 1u < first.sections.size()
                    && std::abs(section.targetEndWatts
                        - first.sections[index + 1u].targetStartWatts)
                            < 1.0e-9) {
                QVERIFY(std::abs(section.referenceEffortEndWatts
                        - first.sections[index + 1u]
                            .referenceEffortStartWatts) < 1.0e-9);
            }
        }
        QVERIFY(below);
        QVERIFY(above);
        QVERIFY(std::abs(weightedEffort / totalDuration - 200.0) < 0.5);
    }

    void variationLengthControlsTerrainFrequencyWithoutChangingMeanEffort()
    {
        WorkoutGameDistanceCourseGenerationParameters shortWaves;
        shortWaves.terrainVariationPercent = 15.0;
        shortWaves.variationLengthMeters = 25.0;
        WorkoutGameDistanceCourseGenerationParameters longWaves = shortWaves;
        longWaves.variationLengthMeters = 150.0;
        const std::vector<WorkoutGameInterval> workout = {
            {0, 15 * 60000, 180.0, 180.0}
        };

        const WorkoutGameDistanceCourse shortCourse =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 200.0, shortWaves, 17u);
        const WorkoutGameDistanceCourse longCourse =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 200.0, longWaves, 17u);

        QCOMPARE(shortCourse.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(longCourse.status, WorkoutGameDistanceCourseStatus::Ready);
        QVERIFY(shortCourse.sections.size() > longCourse.sections.size() * 2u);
        auto meanEffort = [](const WorkoutGameDistanceCourse &course) {
            double sum = 0.0;
            double duration = 0.0;
            for (const WorkoutGameDistanceCourseSection &section : course.sections) {
                sum += 0.5 * (section.referenceEffortStartWatts
                        + section.referenceEffortEndWatts)
                        * section.nominalDurationMs;
                duration += section.nominalDurationMs;
            }
            return sum / duration;
        };
        QVERIFY(std::abs(meanEffort(shortCourse) - 180.0) < 0.5);
        QVERIFY(std::abs(meanEffort(longCourse) - 180.0) < 0.5);
    }

    void sustainedWorkoutProducesAnUphillBiasedRideableCourse()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.terrainVariationPercent = 15.0;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    {{0, 45 * 60000, 150.0, 150.0}},
                    200.0, parameters, 718u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QVERIFY(course.elevationGainMeters > course.elevationLossMeters);
        QVERIFY(course.elevationGainMeters > 50.0);
        QVERIFY(course.elevationLossMeters < course.elevationGainMeters * 0.5);
    }

    void longSteadyWorkoutDistributesFeaturesAcrossTheWholeCourse()
    {
        std::vector<WorkoutGameInterval> intervals;
        for (int index = 0; index < 18; ++index) {
            appendInterval(intervals, 5 * 60000,
                           index % 2 == 0 ? 120.0 : 140.0,
                           index % 2 == 0 ? 120.0 : 140.0);
        }
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    intervals, 187.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 2026u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        QVERIFY(course.sections.size() >= std::size_t(90));
        double previousFeatureDistance = 0.0;
        double maximumFeatureGap = 0.0;
        int featureCount = 0;
        std::set<WorkoutGameTerrainKind> featureKinds;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            if (section.challengeCount <= 0) continue;
            maximumFeatureGap = std::max(
                    maximumFeatureGap,
                    section.startDistanceMeters - previousFeatureDistance);
            previousFeatureDistance = section.startDistanceMeters;
            featureKinds.insert(section.terrain);
            ++featureCount;
        }
        maximumFeatureGap = std::max(
                maximumFeatureGap,
                course.totalDistanceMeters - previousFeatureDistance);
        QVERIFY(featureCount >= 40);
        QVERIFY(featureKinds.size() >= std::size_t(8));
        QVERIFY(course.sections.front().challengeCount > 0);
        QVERIFY(maximumFeatureGap < 1500.0);
    }

    void recoveryJustAboveSixtyPercentFtpBecomesDescent()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 140.0, 140.0},
                    {60000, 180000, 115.0, 115.0},
                    {240000, 60000, 140.0, 140.0}
                }, 190.0);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        const auto recovery = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs >= 60000
                            && section.sourceStartMs < 240000;
                });
        QVERIFY(recovery != course.sections.end());
        QCOMPARE(recovery->feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(recovery->adjustableConnector);
    }

    void thirtySecondAnaerobicEffortBecomesApproachFeature()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 120.0, 120.0},
                    {60000, 30000, 250.0, 250.0},
                    {90000, 60000, 110.0, 110.0}
                }, 190.0);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        const auto jump = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs >= 60000
                            && section.sourceStartMs < 90000;
                });
        QVERIFY(jump != course.sections.end());
        QCOMPARE(jump->feature,
                 WorkoutGameFeature::SprintJump);
        QVERIFY(jump->terrain
                    == WorkoutGameTerrainKind::BunnyHop
                || jump->terrain
                    == WorkoutGameTerrainKind::LogOver
                || jump->terrain == WorkoutGameTerrainKind::Tabletop
                || jump->terrain == WorkoutGameTerrainKind::RockSlab
                || jump->terrain == WorkoutGameTerrainKind::GapJump);
    }

    void onlyHighTechnicalitySelectsGapJumpDeterministically()
    {
        const std::vector<WorkoutGameInterval> workout = {
            {0, 60000, 120.0, 120.0},
            {60000, 90000, 150.0, 300.0},
            {150000, 60000, 110.0, 110.0}
        };
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.variationLengthMeters = 200.0;
        parameters.terrainVariationPercent = 0.0;

        std::uint32_t gapSeed = 0u;
        WorkoutGameDistanceCourse rideFirst;
        parameters.technicality = 0.95;
        for (std::uint32_t seed = 1u; seed <= 64u; ++seed) {
            const WorkoutGameDistanceCourse candidate =
                    WorkoutGameDistanceCourseBuilder::build(
                        workout, 190.0, parameters, seed);
            const auto gap = std::find_if(
                    candidate.sections.begin(), candidate.sections.end(),
                    [](const WorkoutGameDistanceCourseSection &section) {
                        return section.sourceStartMs >= 60000
                                && section.sourceStartMs < 150000
                                && section.terrain
                                    == WorkoutGameTerrainKind::GapJump;
                    });
            if (candidate.status == WorkoutGameDistanceCourseStatus::Ready
                    && gap != candidate.sections.end()) {
                gapSeed = seed;
                rideFirst = candidate;
                break;
            }
        }
        QVERIFY(gapSeed != 0u);

        parameters.technicality = 0.15;
        const WorkoutGameDistanceCourse workoutFirst =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);
        parameters.technicality = 0.55;
        const WorkoutGameDistanceCourse balanced =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);
        parameters.technicality = 0.95;
        const WorkoutGameDistanceCourse repeated =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 190.0, parameters, gapSeed);

        const auto jumpSection = [](const WorkoutGameDistanceCourse &course) {
            return std::find_if(
                    course.sections.begin(), course.sections.end(),
                    [](const WorkoutGameDistanceCourseSection &section) {
                        return section.sourceStartMs >= 60000
                                && section.sourceStartMs < 150000;
                    });
        };
        const auto calmJump = jumpSection(workoutFirst);
        const auto balancedJump = jumpSection(balanced);
        const auto technicalJump = std::find_if(
                rideFirst.sections.begin(), rideFirst.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs >= 60000
                            && section.sourceStartMs < 150000
                            && section.terrain == WorkoutGameTerrainKind::GapJump;
                });
        const auto repeatedJump = std::find_if(
                repeated.sections.begin(), repeated.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs >= 60000
                            && section.sourceStartMs < 150000
                            && section.terrain == WorkoutGameTerrainKind::GapJump;
                });
        QVERIFY(calmJump != workoutFirst.sections.end());
        QVERIFY(balancedJump != balanced.sections.end());
        QVERIFY(technicalJump != rideFirst.sections.end());
        QVERIFY(repeatedJump != repeated.sections.end());
        QCOMPARE(calmJump->feature, WorkoutGameFeature::SprintJump);
        QCOMPARE(balancedJump->feature, WorkoutGameFeature::SprintJump);
        QCOMPARE(technicalJump->feature, WorkoutGameFeature::SprintJump);
        const WorkoutGameGapJumpGeometryProfile gapProfile =
                WorkoutGameGapJumpGeometry::profile(
                    technicalJump->difficulty);
        const WorkoutGameGapJumpGeometryProfile canonical =
                WorkoutGameGapJumpGeometry::canonicalProfile();
        QVERIFY(gapProfile.ready);
        QVERIFY(canonical.ready);
        QVERIFY(technicalJump->lengthMeters + 1.0e-9
                >= gapProfile.prepareLeadMeters
                    + canonical.lines.back().gapLengthMeters
                    + 6.0 + canonical.mergeLengthMeters);
        QVERIFY(calmJump->terrain != WorkoutGameTerrainKind::GapJump);
        QVERIFY(balancedJump->terrain != WorkoutGameTerrainKind::GapJump);
        QCOMPARE(repeatedJump->visualVariant,
                 technicalJump->visualVariant);
    }

    void effortShapePlacesFeaturesWhereTheirRidingEffortMakesSense()
    {
        std::vector<WorkoutGameInterval> workout;
        appendInterval(workout, 2 * 60000, 120.0, 120.0);
        appendInterval(workout, 3 * 60000, 130.0, 260.0);
        appendInterval(workout, 2 * 60000, 105.0, 105.0);
        appendInterval(workout, 3 * 60000, 235.0, 235.0);
        appendInterval(workout, 2 * 60000, 105.0, 90.0);

        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.technicality = 0.95;
        parameters.terrainVariationPercent = 0.0;
        parameters.variationLengthMeters = 60.0;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    workout, 200.0, parameters, 313u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        bool risingApproach = false;
        bool effortDrop = false;
        bool sustainedHigh = false;
        for (std::size_t index = 0; index < course.sections.size(); ++index) {
            const WorkoutGameDistanceCourseSection &section =
                    course.sections[index];
            const double effort = 0.5
                    * (section.referenceEffortStartWatts
                       + section.referenceEffortEndWatts);
            const double previousEffort = index == 0 ? effort : 0.5
                    * (course.sections[index - 1].referenceEffortStartWatts
                       + course.sections[index - 1].referenceEffortEndWatts);
            const double nextEffort = index + 1 == course.sections.size()
                    ? effort : 0.5
                        * (course.sections[index + 1].referenceEffortStartWatts
                           + course.sections[index + 1].referenceEffortEndWatts);
            const bool rises = section.referenceEffortEndWatts
                        - section.referenceEffortStartWatts >= 6.0
                    || effort - previousEffort >= 6.0
                    || nextEffort - effort >= 6.0;
            const bool falls = section.referenceEffortStartWatts
                        - section.referenceEffortEndWatts >= 6.0
                    || effort - nextEffort >= 6.0;
            if (rises && (section.terrain == WorkoutGameTerrainKind::RockSlab
                    || section.terrain == WorkoutGameTerrainKind::Tabletop
                    || section.terrain == WorkoutGameTerrainKind::GapJump)) {
                risingApproach = true;
            }
            if (falls && section.terrain == WorkoutGameTerrainKind::Drop) {
                effortDrop = true;
            }
            if (effort >= 0.90 * 200.0
                    && std::abs(nextEffort - effort) < 6.0
                    && (section.terrain == WorkoutGameTerrainKind::Climb
                        || section.terrain
                            == WorkoutGameTerrainKind::RockSlab)) {
                sustainedHigh = true;
            }
        }
        QVERIFY2(risingApproach,
                 "A rising effort needs an acceleration or rock approach");
        QVERIFY2(effortDrop,
                 "A falling effort needs a drop at the effort release");
        QVERIFY2(sustainedHigh,
                 "A sustained high effort needs a climb or rock slab");
    }

    void referenceEffortProducesAFiniteDistanceBasedEstimate()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 42u);

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics);

        QVERIFY(estimate.finished);
        QVERIFY(estimate.elapsedTimeMs > 0);
        QVERIFY(estimate.elapsedTimeMs < course.nominalDurationMs * 3);
        QVERIFY(estimate.distanceMeters >= course.totalDistanceMeters);
    }

    void powerChangesCompletionTime()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 77u);

        const WorkoutGameDistanceCourseEstimate slower =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 0.85);
        const WorkoutGameDistanceCourseEstimate nominal =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0);
        const WorkoutGameDistanceCourseEstimate faster =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.15);

        QVERIFY(slower.finished);
        QVERIFY(nominal.finished);
        QVERIFY(faster.finished);
        QVERIFY(slower.elapsedTimeMs > nominal.elapsedTimeMs);
        QVERIFY(nominal.elapsedTimeMs > faster.elapsedTimeMs);
    }

    void legacyExposureLimitsDoNotChangeDistanceBasedEstimate()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 300.0, 300.0},
                    {60000, 60000, 300.0, 300.0}
                }, 190.0, parameters, 91u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        QVERIFY(WorkoutGameDistanceCourseBuilder::validCourse(course));

        WorkoutGameDistanceCourse changedExposure = course;
        for (WorkoutGameDistanceCourseSection &section :
                changedExposure.sections) {
            section.minimumDurationMs = 1;
            section.maximumDurationMs = section.nominalDurationMs * 3;
        }
        const WorkoutGameDistanceCourseEstimate baseline =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);
        const WorkoutGameDistanceCourseEstimate changed =
                WorkoutGameDistanceCourseEstimator::estimate(
                    changedExposure, parameters.roadPhysics, 1.0, 100);

        QVERIFY(baseline.finished);
        QVERIFY(changed.finished);
        QCOMPARE(changed.elapsedTimeMs, baseline.elapsedTimeMs);
        QCOMPARE(changed.distanceMeters, baseline.distanceMeters);
    }

    void estimateBootstrapsARampThatStartsAtZeroPower()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 0.0, 200.0}
                }, 190.0, parameters, 117u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);
        QVERIFY(estimate.finished);
        QVERIFY(estimate.elapsedTimeMs > 0);
    }

    void maximumExposureCannotForceDistanceCompletion()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.variationLengthMeters = 200.0;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 10000, 150.0, 150.0}
                }, 190.0, parameters, 118u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        course.sections[0].gradePercent = 0.0;
        course.sections[0].startElevationMeters = 0.0;
        course.sections[0].endElevationMeters = 0.0;
        course.sections[0].lengthMeters = 100000.0;
        course.sections[0].minimumDurationMs = 10000;
        course.sections[0].maximumDurationMs = 10000;
        course.totalDistanceMeters = 100000.0;
        course.elevationGainMeters = 0.0;
        course.elevationLossMeters = 0.0;
        QVERIFY(WorkoutGameDistanceCourseBuilder::validCourse(course));

        const WorkoutGameDistanceCourseEstimate estimate =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics, 1.0, 100);
        QVERIFY(!estimate.finished);
        QVERIFY(estimate.distanceMeters < course.totalDistanceMeters);
    }

    void sameInputProducesTheSameCourse()
    {
        const WorkoutGameDistanceCourse first =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 99u);
        const WorkoutGameDistanceCourse second =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0,
                    WorkoutGameDistanceCourseGenerationParameters(), 99u);

        QCOMPARE(first.totalDistanceMeters, second.totalDistanceMeters);
        QCOMPARE(first.elevationGainMeters, second.elevationGainMeters);
        QCOMPARE(first.sections.size(), second.sections.size());
        for (std::size_t index = 0; index < first.sections.size(); ++index) {
            QCOMPARE(first.sections[index].lengthMeters,
                     second.sections[index].lengthMeters);
            QCOMPARE(first.sections[index].gradePercent,
                     second.sections[index].gradePercent);
            QCOMPARE(first.sections[index].feature,
                     second.sections[index].feature);
        }
    }

    void balancedCourseUsesAVariedTechnicalPalette()
    {
        std::vector<WorkoutGameInterval> intervals;
        for (int index = 0; index < 14; ++index) {
            appendInterval(intervals, 45000,
                    index % 3 == 0 ? 175.0 : 155.0,
                    index % 3 == 0 ? 175.0 : 155.0);
        }
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.variationLengthMeters = 200.0;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    intervals, 190.0, parameters, 55u);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);

        std::set<WorkoutGameTerrainKind> terrains;
        int challenges = 0;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            terrains.insert(section.terrain);
            challenges += section.challengeCount > 0;
        }
        QVERIFY(terrains.size() >= std::size_t(4));
        QVERIFY(challenges >= 3);
    }

    void highPowerVariationPreservesThePlannedAverageWithoutClipping()
    {
        WorkoutGameDistanceCourseGenerationParameters parameters;
        parameters.terrainVariationPercent = 30.0;
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    {{0, 20 * 60000, 2600.0, 2600.0}},
                    200.0, parameters, 918u);

        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        double weightedEffort = 0.0;
        double duration = 0.0;
        bool exceededLegacyClamp = false;
        for (const WorkoutGameDistanceCourseSection &section : course.sections) {
            const double effort = 0.5
                    * (section.referenceEffortStartWatts
                       + section.referenceEffortEndWatts);
            weightedEffort += effort * section.nominalDurationMs;
            duration += section.nominalDurationMs;
            exceededLegacyClamp |= section.referenceEffortStartWatts > 2500.0
                    || section.referenceEffortEndWatts > 2500.0;
        }
        QVERIFY(exceededLegacyClamp);
        QVERIFY(std::abs(weightedEffort / duration - 2600.0) < 0.5);
    }

    void longRecoverySectionsUseGentleDescents()
    {
        const WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build({
                    {0, 60000, 140.0, 140.0},
                    {60000, 60000, 225.0, 225.0},
                    {120000, 10 * 60000, 100.0, 100.0},
                    {720000, 5 * 60000, 110.0, 80.0}
        }, 190.0);
        QCOMPARE(course.status, WorkoutGameDistanceCourseStatus::Ready);
        const auto recovery = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs == 120000;
                });
        const auto cooldown = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameDistanceCourseSection &section) {
                    return section.sourceStartMs == 720000;
                });
        QVERIFY(recovery != course.sections.end());
        QVERIFY(cooldown != course.sections.end());
        QCOMPARE(recovery->feature,
                 WorkoutGameFeature::RecoveryDescent);
        QVERIFY(recovery->gradePercent > -2.0);
        QCOMPARE(cooldown->feature,
                 WorkoutGameFeature::CooldownDescent);
        QVERIFY(cooldown->gradePercent > -2.0);
        QVERIFY(course.elevationLossMeters < course.totalDistanceMeters * 0.025);
    }

    void estimatorRejectsMalformedCourse()
    {
        WorkoutGameDistanceCourse malformed;
        malformed.status = WorkoutGameDistanceCourseStatus::Ready;
        malformed.nominalDurationMs = 60000;
        malformed.totalDistanceMeters = 100.0;
        WorkoutGameDistanceCourseSection section;
        section.lengthMeters = std::numeric_limits<double>::quiet_NaN();
        malformed.sections.push_back(section);

        const WorkoutGameDistanceCourseEstimate result =
                WorkoutGameDistanceCourseEstimator::estimate(
                    malformed, WorkoutGameRoadPhysicsParameters());

        QVERIFY(!result.finished);
        QCOMPARE(result.elapsedTimeMs, std::int64_t(0));
    }

    void estimatorAcceptsSerializedDistanceRounding()
    {
        const WorkoutGameDistanceCourseGenerationParameters parameters;
        WorkoutGameDistanceCourse course =
                WorkoutGameDistanceCourseBuilder::build(
                    fiveByFourWorkout(), 190.0, parameters, 101u);
        QVERIFY(course.sections.size() > 1);
        course.sections[1].startDistanceMeters += 0.0005;

        const WorkoutGameDistanceCourseEstimate result =
                WorkoutGameDistanceCourseEstimator::estimate(
                    course, parameters.roadPhysics);

        QVERIFY(result.finished);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameDistanceCourse)
#include "testWorkoutGameDistanceCourse.moc"
