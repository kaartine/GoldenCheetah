/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadPlan.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double DistanceToleranceMeters = 1.0e-6;
constexpr double MaximumCourseDistanceMeters = 250000.0;
constexpr double MaximumPieceLengthMeters = 500.0;
constexpr double MaximumTurnRadians = 1.4835298641951802; // 85 degrees
constexpr std::uint64_t MaximumExactJsonInteger = 9007199254740991ULL;

bool finiteValue(double value)
{
    return std::isfinite(value);
}

bool validConnector(const WorkoutGameRoadConnector &connector)
{
    return finiteValue(connector.xMeters)
            && finiteValue(connector.zMeters)
            && finiteValue(connector.elevationMeters)
            && finiteValue(connector.headingRadians)
            && finiteValue(connector.halfWidthMeters)
            && connector.halfWidthMeters > 0.0
            && connector.halfWidthMeters <= 10.0
            && finiteValue(connector.gradePercent)
            && std::abs(connector.gradePercent) <= 100.0;
}

bool sameConnector(
        const WorkoutGameRoadConnector &left,
        const WorkoutGameRoadConnector &right)
{
    return std::abs(left.xMeters - right.xMeters) <= DistanceToleranceMeters
            && std::abs(left.zMeters - right.zMeters)
                <= DistanceToleranceMeters
            && std::abs(left.elevationMeters - right.elevationMeters)
                <= DistanceToleranceMeters
            && std::abs(left.headingRadians - right.headingRadians)
                <= 1.0e-9
            && std::abs(left.halfWidthMeters - right.halfWidthMeters)
                <= 1.0e-9
            && std::abs(left.gradePercent - right.gradePercent) <= 1.0e-9;
}

bool validTerrain(WorkoutGameTerrainKind terrain)
{
    return terrain >= WorkoutGameTerrainKind::SmoothTrail
            && terrain <= WorkoutGameTerrainKind::GapJump;
}

bool validAnimation(WorkoutGameRoadAnimation animation)
{
    return animation >= WorkoutGameRoadAnimation::None
            && animation <= WorkoutGameRoadAnimation::LeanRight;
}

bool validChallengeProfile(
        const WorkoutGameFeatureChallengeProfile &profile)
{
    return profile.cue >= WorkoutGameChallengeCue::None
            && profile.cue <= WorkoutGameChallengeCue::Climb
            && finiteValue(profile.measurementStartProgress)
            && finiteValue(profile.decisionProgress)
            && finiteValue(profile.minimumEffortRatio)
            && finiteValue(profile.minimumCadenceRpm)
            && finiteValue(profile.minimumSpeedKph)
            && finiteValue(profile.maximumSpeedKph)
            && finiteValue(profile.minimumAdherence)
            && profile.measurementStartProgress >= 0.0
            && profile.measurementStartProgress <= 1.0
            && profile.decisionProgress >= 0.0
            && profile.decisionProgress <= 1.0
            && profile.minimumEffortRatio >= 0.0
            && profile.minimumEffortRatio <= 5.0
            && profile.minimumCadenceRpm >= 0.0
            && profile.minimumCadenceRpm <= 300.0
            && profile.minimumSpeedKph >= 0.0
            && profile.minimumSpeedKph <= 200.0
            && profile.maximumSpeedKph >= 0.0
            && profile.maximumSpeedKph <= 200.0
            && profile.minimumAdherence >= 0.0
            && profile.minimumAdherence <= 1.0
            && profile.bonusPoints <= MaximumExactJsonInteger;
}

bool validChallenge(
        const WorkoutGameRoadPiece &piece,
        double courseEndMeters)
{
    const WorkoutGameRoadChallengeGate &gate = piece.challenge;
    const bool fieldsValid = finiteValue(gate.prepareDistanceMeters)
            && finiteValue(gate.decisionDistanceMeters)
            && finiteValue(gate.obstacleDistanceMeters)
            && finiteValue(gate.bypassStartDistanceMeters)
            && finiteValue(gate.bypassEndDistanceMeters)
            && finiteValue(gate.bypassLateralMeters)
            && validChallengeProfile(gate.profile);
    if (!fieldsValid) return false;
    if (!gate.enabled) {
        return !gate.profile.enabled && !piece.qualityExempt;
    }
    const bool ordered = gate.prepareDistanceMeters
                <= gate.decisionDistanceMeters + DistanceToleranceMeters
            && gate.decisionDistanceMeters
                <= gate.obstacleDistanceMeters + DistanceToleranceMeters
            && gate.bypassStartDistanceMeters
                <= gate.bypassEndDistanceMeters + DistanceToleranceMeters;
    const bool distancesValid = gate.prepareDistanceMeters >= 0.0
            && gate.obstacleDistanceMeters <= courseEndMeters
                + DistanceToleranceMeters
            && gate.bypassStartDistanceMeters >= 0.0
            && gate.bypassEndDistanceMeters <= courseEndMeters
                + DistanceToleranceMeters
            && std::abs(gate.bypassLateralMeters) <= 20.0;
    if (!ordered || !distancesValid || !gate.profile.enabled) {
        return false;
    }
    if (!piece.qualityExempt) return true;
    return finiteValue(piece.qualityExemptionStartDistanceMeters)
            && finiteValue(piece.qualityExemptionEndDistanceMeters)
            && std::abs(piece.qualityExemptionStartDistanceMeters
                        - piece.startDistanceMeters)
                <= DistanceToleranceMeters
            && std::abs(piece.qualityExemptionEndDistanceMeters
                        - piece.startDistanceMeters - piece.lengthMeters)
                <= DistanceToleranceMeters
            && piece.qualityExemptionStartDistanceMeters
                < piece.qualityExemptionEndDistanceMeters;
}

bool validGapJump(
        const WorkoutGameRoadGapJumpGate &gap,
        double courseEndMeters)
{
    if (!finiteValue(gap.prepareDistanceMeters)
            || !finiteValue(gap.launchWindowStartDistanceMeters)
            || !finiteValue(gap.lockDistanceMeters)
            || !finiteValue(gap.splitStartDistanceMeters)
            || !finiteValue(gap.mergeEndDistanceMeters)) {
        return false;
    }
    if (!gap.enabled) {
        for (const WorkoutGameRoadGapJumpLine &line : gap.lines) {
            if (!finiteValue(line.takeoffDistanceMeters)
                    || !finiteValue(line.landingDistanceMeters)
                    || !finiteValue(line.lateralMeters)
                    || !finiteValue(line.gapLengthMeters)
                    || !finiteValue(line.minimumSpeedMetersPerSecond)
                    || !finiteValue(line.nominalFlightSeconds)
                    || !finiteValue(line.lipHeightMeters)
                    || !finiteValue(line.landingDropMeters)) {
                return false;
            }
        }
        return true;
    }
    if (gap.prepareDistanceMeters < 0.0
            || gap.prepareDistanceMeters > gap.launchWindowStartDistanceMeters
            || gap.launchWindowStartDistanceMeters > gap.lockDistanceMeters
            || gap.splitStartDistanceMeters > gap.lockDistanceMeters
            || gap.mergeEndDistanceMeters > courseEndMeters
                + DistanceToleranceMeters) {
        return false;
    }
    WorkoutGameGapJumpLine previous = WorkoutGameGapJumpLine::None;
    for (const WorkoutGameRoadGapJumpLine &line : gap.lines) {
        if (line.id <= previous || line.id > WorkoutGameGapJumpLine::Long
                || !finiteValue(line.takeoffDistanceMeters)
                || !finiteValue(line.landingDistanceMeters)
                || !finiteValue(line.lateralMeters)
                || !finiteValue(line.gapLengthMeters)
                || !finiteValue(line.minimumSpeedMetersPerSecond)
                || !finiteValue(line.nominalFlightSeconds)
                || !finiteValue(line.lipHeightMeters)
                || !finiteValue(line.landingDropMeters)
                || line.takeoffDistanceMeters < 0.0
                || line.landingDistanceMeters
                    > courseEndMeters + DistanceToleranceMeters
                || line.landingDistanceMeters <= line.takeoffDistanceMeters
                || line.gapLengthMeters <= 0.0
                || std::abs((line.landingDistanceMeters
                    - line.takeoffDistanceMeters) - line.gapLengthMeters)
                        > DistanceToleranceMeters
                || line.minimumSpeedMetersPerSecond <= 0.0
                || line.minimumSpeedMetersPerSecond > 50.0
                || line.nominalFlightSeconds <= 0.0
                || line.nominalFlightSeconds > 2.0
                || std::abs(line.lateralMeters) > 20.0
                || line.lipHeightMeters < 0.0
                || line.lipHeightMeters > 10.0
                || std::abs(line.landingDropMeters) > 10.0) {
            return false;
        }
        previous = line.id;
    }
    return true;
}

}

WorkoutGameRoadPlanValidationStatus WorkoutGameRoadPlanValidator::validate(
        const WorkoutGameRoadPlan &plan,
        std::size_t sourceSectionCount)
{
    if (plan.generationVersion
            != WorkoutGameRoadPlan::CurrentGenerationVersion) {
        return WorkoutGameRoadPlanValidationStatus::UnsupportedVersion;
    }
    if (plan.pieces.size() > WorkoutGameRoadPlan::MaximumPieces) {
        return WorkoutGameRoadPlanValidationStatus::ResourceLimit;
    }
    if (plan.pieces.empty() || sourceSectionCount == 0) {
        return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
    }

    double expectedStartMeters = 0.0;
    const WorkoutGameRoadConnector *previousExit = nullptr;
    std::size_t previousSection = 0;
    const double courseEndMeters = plan.pieces.back().startDistanceMeters
            + plan.pieces.back().lengthMeters;
    if (!finiteValue(courseEndMeters) || courseEndMeters <= 0.0
            || courseEndMeters > MaximumCourseDistanceMeters) {
        return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
    }

    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        if (piece.sourceSectionIndex >= sourceSectionCount
                || piece.sourceSectionIndex < previousSection
                || piece.sourceSectionIndex > previousSection + 1
                || !validTerrain(piece.terrain)
                || !validAnimation(piece.animation)
                || !finiteValue(piece.startDistanceMeters)
                || !finiteValue(piece.lengthMeters)
                || !finiteValue(piece.turnRadians)
                || !finiteValue(piece.riseMeters)
                || !finiteValue(piece.difficulty)
                || !finiteValue(piece.reliefScale)
                || !finiteValue(piece.geometryAnchorDistanceMeters)
                || piece.lengthMeters <= 0.0
                || piece.lengthMeters > MaximumPieceLengthMeters
                || std::abs(piece.startDistanceMeters - expectedStartMeters)
                    > DistanceToleranceMeters
                || std::abs(piece.turnRadians) > MaximumTurnRadians + 1.0e-9
                || std::abs(piece.riseMeters) > piece.lengthMeters
                || piece.difficulty < 0.0 || piece.difficulty > 1.0
                || piece.reliefScale < 0.0 || piece.reliefScale > 2.5
                || piece.geometryAnchorDistanceMeters < 0.0
                || piece.geometryAnchorDistanceMeters
                    > courseEndMeters + DistanceToleranceMeters
                || !finiteValue(piece.qualityExemptionStartDistanceMeters)
                || !finiteValue(piece.qualityExemptionEndDistanceMeters)
                || (!piece.qualityExempt
                    && (piece.qualityExemptionStartDistanceMeters != 0.0
                        || piece.qualityExemptionEndDistanceMeters != 0.0))
                || !validConnector(piece.entry)
                || !validConnector(piece.exit)
                || (previousExit && !sameConnector(*previousExit, piece.entry))
                || !validChallenge(piece, courseEndMeters)
                || !validGapJump(piece.gapJump, courseEndMeters)
                || (piece.gapJump.enabled && !piece.challenge.enabled)) {
            return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
        }
        previousSection = piece.sourceSectionIndex;
        previousExit = &piece.exit;
        expectedStartMeters += piece.lengthMeters;
        if (!finiteValue(expectedStartMeters)
                || expectedStartMeters > MaximumCourseDistanceMeters) {
            return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
        }
    }
    if (plan.pieces.front().sourceSectionIndex != 0
            || plan.pieces.back().sourceSectionIndex + 1
                != sourceSectionCount) {
        return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
    }
    return WorkoutGameRoadPlanValidationStatus::Ready;
}
