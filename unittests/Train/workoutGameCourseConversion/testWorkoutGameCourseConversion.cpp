/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseConversion.h"
#include "Train/WorkoutGameCoursePrescription.h"
#include "Train/WorkoutGameCourseTerrain.h"
#include "Train/WorkoutGameFeatureCatalog.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <set>

namespace {

void appendInterval(
        std::vector<WorkoutGameInterval> &intervals,
        std::int64_t durationMs,
        double watts)
{
    const std::int64_t startMs = intervals.empty()
            ? 0
            : intervals.back().startMs + intervals.back().durationMs;
    intervals.push_back({startMs, durationMs, watts, watts});
}

std::vector<WorkoutGameInterval> sampleWorkout()
{
    std::vector<WorkoutGameInterval> intervals;
    // Ten equal palette-eligible intervals make the published per-ten terrain
    // density bands directly measurable without changing the prescription.
    for (int index = 0; index < 8; ++index) {
        appendInterval(intervals, 60000, 140.0);
    }
    for (int repetition = 0; repetition < 2; ++repetition) {
        appendInterval(intervals, 4 * 60000, 205.0 + repetition * 3.0);
        appendInterval(intervals, 10000, 250.0 + repetition * 5.0);
        appendInterval(intervals, 3 * 60000, 110.0);
    }
    // This is deliberately internal: it is an ordinary prescribed recovery,
    // not a cooldown or a flow transition inferred from its position.
    appendInterval(intervals, 5 * 60000, 100.0);
    appendInterval(intervals, 4 * 60000, 211.0);
    appendInterval(intervals, 10000, 260.0);
    appendInterval(intervals, 3 * 60000, 110.0);
    appendInterval(intervals, 60000, 140.0);
    appendInterval(intervals, 60000, 140.0);
    return intervals;
}

constexpr std::size_t FiveMinuteRecoveryIndex = 14;

}

class TestWorkoutGameCourseConversion : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputIsRejected()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 0.0;

        const WorkoutGameCourseConversionResult result =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseConversionStatus::InvalidInput);
        QVERIFY(result.course.sections.empty());
    }

    void prescriptionMetadataValidationFailsClosed()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.prescriptionMetadata.version = 99;
        request.prescriptionMetadata.intervalRoles.assign(
                request.intervals.size(),
                WorkoutGameCourseIntervalRole::Prescribed);

        QCOMPARE(WorkoutGameCourseConverter::convert(request).status,
                 WorkoutGameCourseConversionStatus::GenerationFailed);

        request.prescriptionMetadata.version = 1;
        request.prescriptionMetadata.intervalRoles.pop_back();
        QCOMPARE(WorkoutGameCourseConverter::convert(request).status,
                 WorkoutGameCourseConversionStatus::GenerationFailed);

        request.prescriptionMetadata.intervalRoles.assign(
                request.intervals.size(),
                WorkoutGameCourseIntervalRole::Prescribed);
        request.prescriptionMetadata.intervalRoles[FiveMinuteRecoveryIndex] =
                WorkoutGameCourseIntervalRole::NonPrescriptiveTransition;
        QCOMPARE(WorkoutGameCourseConverter::convert(request).status,
                 WorkoutGameCourseConversionStatus::GenerationFailed);
    }

    void explicitCooldownIsSeparateFromOrdinaryRecoveryRetention()
    {
        const std::vector<WorkoutGameInterval> source {
            {0, 10000, 150.0, 250.0},
            {10000, 20600, 100.0, 100.0}
        };
        const std::vector<WorkoutGameInterval> generated {
            {0, 10000, 150.0, 250.0},
            {10000, 20000, 100.0, 100.0}
        };
        WorkoutGameCoursePrescriptionMetadata metadata;
        metadata.version = 1;
        metadata.intervalRoles = {
            WorkoutGameCourseIntervalRole::Prescribed,
            WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown
        };

        const WorkoutGameCoursePrescriptionAudit audit =
                WorkoutGameCoursePrescription::audit(
                    source, generated, 190.0,
                    WorkoutGameCoursePreset::Balanced, metadata);

        QCOMPARE(audit.status, WorkoutGameCoursePrescriptionStatus::Ready);
        QCOMPARE(audit.recoveryCount, 0);
        QCOMPARE(audit.preservedRecoveryCount, 0);
        QCOMPARE(audit.recoveryDurationDeviationPercent, 0.0);
        QCOMPARE(audit.durationChanges.size(), std::size_t(1));
        QCOMPARE(audit.durationChanges.front().role,
                 WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown);
    }

    void terrainPaletteIsDeterministicAndRecoverySafe()
    {
        for (WorkoutGameCoursePreset preset : {
                WorkoutGameCoursePreset::WorkoutFirst,
                WorkoutGameCoursePreset::Balanced,
                WorkoutGameCoursePreset::RideFirst}) {
            int technical = 0;
            for (std::size_t index = 0; index < 10; ++index) {
                WorkoutGameSection first;
                first.feature = WorkoutGameFeature::Trail;
                first.challengeCount = 1;
                WorkoutGameSection second = first;
                WorkoutGameCourseTerrain::apply(
                        first, preset, index, 10u, 42u, false);
                WorkoutGameCourseTerrain::apply(
                        second, preset, index, 10u, 42u, false);
                QCOMPARE(first.terrain, second.terrain);
                QCOMPARE(first.challengeCount, second.challengeCount);
                technical += WorkoutGameFeatureCatalog::definition(
                        first.terrain).technical ? 1 : 0;
            }
            const WorkoutGameCourseModeContract contract =
                    WorkoutGameCoursePrescription::contractFor(preset);
            QVERIFY(double(technical) + 1.0e-9
                    >= contract.minimumTechnicalFeatureDensityPerTenSections);
            QVERIFY(double(technical)
                    <= contract.maximumTechnicalFeatureDensityPerTenSections
                        + 1.0e-9);

            WorkoutGameSection recovery;
            recovery.feature = WorkoutGameFeature::WarmupTrail;
            recovery.challengeCount = 3;
            WorkoutGameCourseTerrain::apply(
                    recovery, preset, 0u, 10u, 42u, true);
            QCOMPARE(recovery.challengeCount, 0);
            QVERIFY(recovery.terrain != WorkoutGameTerrainKind::GapJump);
        }
    }

    void balancedPresetProducesCompletePreviewSummary()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.preset = WorkoutGameCoursePreset::Balanced;
        request.seed = 42u;

        const WorkoutGameCourseConversionResult result =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(result.course.status, WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(result.course.seed, std::uint32_t(42));
        QCOMPARE(result.summary.sourceDurationMs,
                 std::int64_t(36 * 60000 + 30000));
        QCOMPARE(result.summary.nominalDurationMs,
                 result.summary.sourceDurationMs);
        QVERIFY(result.summary.sourceLoadPoints > 0.0);
        QCOMPARE(result.summary.estimatedLoadPoints,
                 result.summary.sourceLoadPoints);
        QCOMPARE(result.summary.loadDeviationPercent, 0.0);
        QCOMPARE(result.summary.workDurationDeviationPercent, 0.0);
        QCOMPARE(result.summary.recoveryDurationDeviationPercent, 0.0);
        QCOMPARE(result.summary.totalDurationDeviationPercent, 0.0);
        QCOMPARE(result.summary.preservedKeyEffortCount,
                 result.summary.keyEffortCount);
        QVERIFY(result.summary.recoveryCount > 0);
        QCOMPARE(result.summary.preservedRecoveryCount,
                 result.summary.recoveryCount);
        QVERIFY(result.summary.distanceMeters > 5000.0);
        QVERIFY(result.summary.elevationGainMeters > 0.0);
        QVERIFY(result.summary.nominalEstimate.finished);
        QVERIFY(result.summary.fastEstimate.finished);
        QVERIFY(result.summary.slowEstimate.finished);
        QVERIFY(result.summary.fastEstimate.elapsedTimeMs
                < result.summary.nominalEstimate.elapsedTimeMs);
        QVERIFY(result.summary.nominalEstimate.elapsedTimeMs
                < result.summary.slowEstimate.elapsedTimeMs);
        QCOMPARE(result.summary.climbCount, 3);
        QCOMPARE(result.summary.jumpCount, 3);
        QCOMPARE(result.summary.descentCount, 4);
    }

    void presetsChangeTerrainWithoutChangingWorkoutTargets()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.seed = 77u;

        request.preset = WorkoutGameCoursePreset::WorkoutFirst;
        const WorkoutGameCourseConversionResult workoutFirst =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::Balanced;
        const WorkoutGameCourseConversionResult balanced =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::RideFirst;
        const WorkoutGameCourseConversionResult rideFirst =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(workoutFirst.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(balanced.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(rideFirst.status, WorkoutGameCourseConversionStatus::Ready);
        QCOMPARE(workoutFirst.course.sections.size(), balanced.course.sections.size());
        QCOMPARE(balanced.course.sections.size(), rideFirst.course.sections.size());
        QVERIFY(workoutFirst.summary.elevationGainMeters
                < balanced.summary.elevationGainMeters);
        QVERIFY(balanced.summary.elevationGainMeters
                < rideFirst.summary.elevationGainMeters);
        QVERIFY(workoutFirst.summary.technicalFeatureCount
                < balanced.summary.technicalFeatureCount);
        QVERIFY(balanced.summary.technicalFeatureCount
                < rideFirst.summary.technicalFeatureCount);
        QCOMPARE(workoutFirst.summary.nominalDurationMs,
                 balanced.summary.nominalDurationMs);
        QCOMPARE(balanced.summary.nominalDurationMs,
                 rideFirst.summary.nominalDurationMs);
        QCOMPARE(workoutFirst.summary.loadDeviationPercent, 0.0);
        QCOMPARE(balanced.summary.loadDeviationPercent, 0.0);
        QCOMPARE(rideFirst.summary.loadDeviationPercent, 0.0);
        QVERIFY(workoutFirst.summary.technicalTerrainExposurePercent
                < balanced.summary.technicalTerrainExposurePercent);
        QVERIFY(balanced.summary.technicalTerrainExposurePercent
                < rideFirst.summary.technicalTerrainExposurePercent);
        QVERIFY(workoutFirst.summary.technicalFeatureDensityPerTenSections
                < balanced.summary.technicalFeatureDensityPerTenSections);
        QVERIFY(balanced.summary.technicalFeatureDensityPerTenSections
                < rideFirst.summary.technicalFeatureDensityPerTenSections);
        QVERIFY(workoutFirst.summary.curvatureDegreesPer100m + 15.0
                <= balanced.summary.curvatureDegreesPer100m + 1.0e-9);
        QVERIFY(balanced.summary.curvatureDegreesPer100m + 15.0
                <= rideFirst.summary.curvatureDegreesPer100m + 1.0e-9);
        bool hasNewRideFeature = false;
        for (const WorkoutGameDistanceCourseSection &section
                : rideFirst.course.sections) {
            hasNewRideFeature = hasNewRideFeature
                    || section.terrain == WorkoutGameTerrainKind::LogOver
                    || section.terrain == WorkoutGameTerrainKind::Tabletop
                    || section.terrain == WorkoutGameTerrainKind::RockSlab;
        }
        QVERIFY(hasNewRideFeature);

        bool workoutFirstHasTechnicalPlay = false;
        for (const WorkoutGameDistanceCourseSection &section
                : workoutFirst.course.sections) {
            workoutFirstHasTechnicalPlay = workoutFirstHasTechnicalPlay
                    || section.terrain == WorkoutGameTerrainKind::Roots
                    || section.terrain == WorkoutGameTerrainKind::Rollers
                    || section.terrain == WorkoutGameTerrainKind::RockGarden
                    || section.terrain == WorkoutGameTerrainKind::LogOver;
            QVERIFY(section.terrain != WorkoutGameTerrainKind::GapJump);
        }
        QVERIFY(workoutFirstHasTechnicalPlay);
        const std::set<WorkoutGameTerrainKind> expectedWorkoutFirstTerrain {
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::Rollers,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::LogOver
        };
        std::set<WorkoutGameTerrainKind> actualWorkoutFirstTerrain;
        for (const WorkoutGameDistanceCourseSection &section
                : workoutFirst.course.sections) {
            if (expectedWorkoutFirstTerrain.count(section.terrain) != 0) {
                actualWorkoutFirstTerrain.insert(section.terrain);
            }
        }
        QVERIFY(actualWorkoutFirstTerrain == expectedWorkoutFirstTerrain);

        for (std::size_t index = 0; index < balanced.course.sections.size(); ++index) {
            QCOMPARE(workoutFirst.course.sections[index].targetStartWatts,
                     balanced.course.sections[index].targetStartWatts);
            QCOMPARE(balanced.course.sections[index].targetStartWatts,
                     rideFirst.course.sections[index].targetStartWatts);
            QVERIFY(workoutFirst.course.sections[index].minimumDurationMs
                    >= rideFirst.course.sections[index].minimumDurationMs);
            QVERIFY(workoutFirst.course.sections[index].maximumDurationMs
                    <= rideFirst.course.sections[index].maximumDurationMs);
        }
    }

    void modesMeetPublishedTrainingContract()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.seed = 123u;

        const WorkoutGameCoursePreset presets[] = {
            WorkoutGameCoursePreset::WorkoutFirst,
            WorkoutGameCoursePreset::Balanced,
            WorkoutGameCoursePreset::RideFirst
        };
        for (WorkoutGameCoursePreset preset : presets) {
            request.preset = preset;
            const WorkoutGameCourseConversionResult result =
                    WorkoutGameCourseConverter::convert(request);
            QCOMPARE(result.status,
                     WorkoutGameCourseConversionStatus::Ready);
            const WorkoutGameCourseModeContract contract =
                    WorkoutGameCoursePrescription::contractFor(preset);
            QVERIFY(std::abs(result.summary.workDurationDeviationPercent)
                    <= contract.maximumWorkDurationDeviationPercent + 1.0e-9);
            QVERIFY(std::abs(result.summary.recoveryDurationDeviationPercent)
                    <= contract.maximumRecoveryDurationDeviationPercent + 1.0e-9);
            QVERIFY(std::abs(result.summary.totalDurationDeviationPercent)
                    <= contract.maximumTotalDurationDeviationPercent + 1.0e-9);
            QVERIFY(std::abs(result.summary.loadDeviationPercent)
                    <= contract.maximumLoadDeviationPercent + 1.0e-9);
            QCOMPARE(result.summary.keyEffortCount,
                     result.summary.preservedKeyEffortCount);
            // No versioned role metadata was supplied, so fail safe and
            // preserve every interval in every mode.
            QCOMPARE(result.summary.totalDurationDeviationPercent, 0.0);
            QCOMPARE(result.summary.recoveryDurationDeviationPercent, 0.0);
            QVERIFY(result.summary.technicalTerrainExposurePercent + 1.0e-9
                    >= contract.minimumTechnicalTerrainExposurePercent);
            QVERIFY(result.summary.technicalTerrainExposurePercent
                    <= contract.maximumTechnicalTerrainExposurePercent
                        + 1.0e-9);
            QVERIFY(result.summary.technicalFeatureDensityPerTenSections
                    + 1.0e-9
                    >= contract.minimumTechnicalFeatureDensityPerTenSections);
            QVERIFY(result.summary.technicalFeatureDensityPerTenSections
                    <= contract.maximumTechnicalFeatureDensityPerTenSections
                        + 1.0e-9);
            QVERIFY(result.summary.curvatureDegreesPer100m + 1.0e-9
                    >= contract.minimumCurvatureDegreesPer100m);
            QVERIFY(result.summary.curvatureDegreesPer100m
                    <= contract.maximumCurvatureDegreesPer100m + 1.0e-9);

            for (std::size_t index = 0;
                    index < request.intervals.size(); ++index) {
                const WorkoutGameInterval &source = request.intervals[index];
                const WorkoutGameDistanceCourseSection &output =
                        result.course.sections[index];
                QCOMPARE(output.targetStartWatts, source.startWatts);
                QCOMPARE(output.targetEndWatts, source.endWatts);
                QCOMPARE(output.nominalDurationMs, source.durationMs);
                QVERIFY(std::abs(output.gradePercent) <= 12.0 + 1.0e-9);
                if (WorkoutGameCoursePrescription::isKeyEffort(
                            source, request.ftpWatts)) {
                    QCOMPARE(output.nominalDurationMs, source.durationMs);
                }
                if (WorkoutGameCoursePrescription::isRecovery(
                            source, request.ftpWatts)) {
                    QVERIFY(double(output.nominalDurationMs)
                            >= double(source.durationMs)
                                * contract.minimumRecoveryRetention - 1.0);
                    QVERIFY(double(output.minimumDurationMs)
                            >= double(source.durationMs)
                                * contract.minimumRecoveryExposure - 1.0);
                    QVERIFY(output.terrain
                            != WorkoutGameTerrainKind::GapJump);
                    if (result.course.roadPlan) {
                        for (const WorkoutGameRoadPiece &piece
                                : result.course.roadPlan->pieces) {
                            if (piece.sourceSectionIndex == index) {
                                QVERIFY(!piece.challenge.enabled);
                            }
                        }
                    }
                }
            }

            const WorkoutGameInterval &fiveMinuteRecovery =
                    request.intervals[FiveMinuteRecoveryIndex];
            const WorkoutGameDistanceCourseSection &generatedRecovery =
                    result.course.sections[FiveMinuteRecoveryIndex];
            QCOMPARE(fiveMinuteRecovery.durationMs,
                     std::int64_t(5 * 60000));
            QVERIFY(WorkoutGameCoursePrescription::isRecovery(
                        fiveMinuteRecovery, request.ftpWatts));
            QCOMPARE(generatedRecovery.nominalDurationMs,
                     fiveMinuteRecovery.durationMs);
            QVERIFY(generatedRecovery.terrain
                    != WorkoutGameTerrainKind::GapJump);
        }
    }

    void zeroSeedIsStableAcrossModeSwitches()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;

        request.preset = WorkoutGameCoursePreset::Balanced;
        const WorkoutGameCourseConversionResult balanced =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::RideFirst;
        const WorkoutGameCourseConversionResult rideFirst =
                WorkoutGameCourseConverter::convert(request);
        request.preset = WorkoutGameCoursePreset::Balanced;
        const WorkoutGameCourseConversionResult repeated =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(balanced.course.seed, rideFirst.course.seed);
        QCOMPARE(repeated.course.seed, balanced.course.seed);
        QCOMPARE(repeated.summary.nominalDurationMs,
                 balanced.summary.nominalDurationMs);
        QCOMPARE(repeated.summary.distanceMeters,
                 balanced.summary.distanceMeters);
    }

    void sameRequestProducesIdenticalResult()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;
        request.preset = WorkoutGameCoursePreset::Balanced;

        const WorkoutGameCourseConversionResult first =
                WorkoutGameCourseConverter::convert(request);
        const WorkoutGameCourseConversionResult second =
                WorkoutGameCourseConverter::convert(request);

        QCOMPARE(first.course.seed, second.course.seed);
        QCOMPARE(first.summary.distanceMeters, second.summary.distanceMeters);
        QCOMPARE(first.summary.nominalEstimate.elapsedTimeMs,
                 second.summary.nominalEstimate.elapsedTimeMs);
        QCOMPARE(first.course.sections.size(), second.course.sections.size());
        for (std::size_t index = 0; index < first.course.sections.size(); ++index) {
            QCOMPARE(first.course.sections[index].lengthMeters,
                     second.course.sections[index].lengthMeters);
            QCOMPARE(first.course.sections[index].gradePercent,
                     second.course.sections[index].gradePercent);
        }
    }

    void gapJumpIsLimitedToRideFirstPreset()
    {
        WorkoutGameCourseConversionRequest request;
        request.intervals = sampleWorkout();
        request.ftpWatts = 190.0;

        bool found = false;
        for (std::uint32_t seed = 1u; seed <= 256u && !found; ++seed) {
            request.seed = seed;
            request.preset = WorkoutGameCoursePreset::RideFirst;
            const WorkoutGameCourseConversionResult rideFirst =
                    WorkoutGameCourseConverter::convert(request);
            const bool hasGap = std::any_of(
                    rideFirst.course.sections.begin(),
                    rideFirst.course.sections.end(),
                    [](const WorkoutGameDistanceCourseSection &section) {
                        return section.terrain
                                == WorkoutGameTerrainKind::GapJump;
                    });
            if (!hasGap) continue;

            request.preset = WorkoutGameCoursePreset::WorkoutFirst;
            const WorkoutGameCourseConversionResult workoutFirst =
                    WorkoutGameCourseConverter::convert(request);
            request.preset = WorkoutGameCoursePreset::Balanced;
            const WorkoutGameCourseConversionResult balanced =
                    WorkoutGameCourseConverter::convert(request);
            for (const WorkoutGameDistanceCourseSection &section
                    : workoutFirst.course.sections) {
                QVERIFY(section.terrain != WorkoutGameTerrainKind::GapJump);
            }
            for (const WorkoutGameDistanceCourseSection &section
                    : balanced.course.sections) {
                QVERIFY(section.terrain != WorkoutGameTerrainKind::GapJump);
            }

            request.preset = WorkoutGameCoursePreset::RideFirst;
            const WorkoutGameCourseConversionResult repeated =
                    WorkoutGameCourseConverter::convert(request);
            QCOMPARE(repeated.course.sections.size(),
                     rideFirst.course.sections.size());
            for (std::size_t index = 0;
                    index < rideFirst.course.sections.size(); ++index) {
                QCOMPARE(repeated.course.sections[index].terrain,
                         rideFirst.course.sections[index].terrain);
            }
            found = true;
        }
        QVERIFY(found);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseConversion)
#include "testWorkoutGameCourseConversion.moc"
