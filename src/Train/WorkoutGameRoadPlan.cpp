/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadPlan.h"
#include "WorkoutGameChallengeGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double DistanceToleranceMeters = 1.0e-6;
constexpr double MaximumCourseDistanceMeters = 250000.0;
constexpr double MaximumPieceLengthMeters = 500.0;
constexpr double MaximumTurnRadians = 1.4835298641951802; // 85 degrees
constexpr double MaximumLegacyBermTurnRadians = 1.7453292519943295; // 100 degrees
constexpr double MaximumReliefGradePercent = 120.0;
constexpr std::uint64_t MaximumExactJsonInteger = 9007199254740991ULL;
constexpr double Pi = 3.14159265358979323846;

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

bool decisionMustPrecedeObstacle(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::Drop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::GapJump:
        return true;
    default:
        return false;
    }
}

double latestChallengeDecisionDistance(const WorkoutGameRoadPiece &piece)
{
    if (piece.terrain == WorkoutGameTerrainKind::BunnyHop
            || piece.terrain == WorkoutGameTerrainKind::LogOver
            || piece.terrain == WorkoutGameTerrainKind::Tabletop) {
        const WorkoutGameFeatureGeometryProfile geometry =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        if (geometry.ready) {
            const double takeoffOffset = piece.terrain
                    == WorkoutGameTerrainKind::Tabletop
                ? geometry.plateauStartMeters : geometry.startMeters;
            return piece.challenge.obstacleDistanceMeters + takeoffOffset;
        }
    }
    return piece.challenge.obstacleDistanceMeters;
}

bool zeroBank(const WorkoutGameRoadBankProfile &bank)
{
    return !bank.enabled
            && bank.startDistanceMeters == 0.0
            && bank.curveStartDistanceMeters == 0.0
            && bank.curveEndDistanceMeters == 0.0
            && bank.endDistanceMeters == 0.0
            && bank.socketHalfWidthMeters == 0.0
            && bank.activeHalfWidthMeters == 0.0
            && bank.maximumBankRadians == 0.0
            && bank.maximumLineOffsetMeters == 0.0
            && bank.designSpeedMetersPerSecond == 0.0;
}

bool validBank(const WorkoutGameRoadPiece &piece)
{
    const WorkoutGameRoadBankProfile &bank = piece.bank;
    if (!finiteValue(bank.startDistanceMeters)
            || !finiteValue(bank.curveStartDistanceMeters)
            || !finiteValue(bank.curveEndDistanceMeters)
            || !finiteValue(bank.endDistanceMeters)
            || !finiteValue(bank.socketHalfWidthMeters)
            || !finiteValue(bank.activeHalfWidthMeters)
            || !finiteValue(bank.maximumBankRadians)
            || !finiteValue(bank.maximumLineOffsetMeters)
            || !finiteValue(bank.designSpeedMetersPerSecond)) {
        return false;
    }
    if (!bank.enabled) return zeroBank(bank);
    const double pieceEnd = piece.startDistanceMeters + piece.lengthMeters;
    return !piece.challenge.enabled
            && !piece.gapJump.enabled
            && std::abs(piece.turnRadians) >= 0.20
            && bank.startDistanceMeters >= piece.startDistanceMeters
                - DistanceToleranceMeters
            && bank.startDistanceMeters
                <= bank.curveStartDistanceMeters
                    + DistanceToleranceMeters
            && bank.curveStartDistanceMeters + DistanceToleranceMeters
                < bank.curveEndDistanceMeters
            && bank.curveEndDistanceMeters
                <= bank.endDistanceMeters + DistanceToleranceMeters
            && bank.endDistanceMeters <= pieceEnd + DistanceToleranceMeters
            && bank.socketHalfWidthMeters >= 0.4
            && bank.socketHalfWidthMeters <= 2.0
            && std::abs(bank.socketHalfWidthMeters
                        - piece.entry.halfWidthMeters)
                <= DistanceToleranceMeters
            && std::abs(bank.socketHalfWidthMeters
                        - piece.exit.halfWidthMeters)
                <= DistanceToleranceMeters
            && bank.activeHalfWidthMeters >= bank.socketHalfWidthMeters
            && bank.activeHalfWidthMeters <= 2.0
            && bank.maximumBankRadians > 0.0
            && bank.maximumBankRadians <= Pi * 0.25
            && bank.maximumLineOffsetMeters >= 0.0
            && bank.maximumLineOffsetMeters
                <= bank.activeHalfWidthMeters - 0.2
            && bank.designSpeedMetersPerSecond >= 1.0
            && bank.designSpeedMetersPerSecond <= 30.0;
}

bool zeroRelief(const WorkoutGameRoadReliefProfile &relief)
{
    return !relief.enabled
            && relief.phaseRadians == 0.0
            && relief.constantCoefficientMeters == 0.0
            && relief.cosineCoefficientMeters == 0.0
            && relief.sineCoefficientMeters == 0.0;
}

bool validRelief(const WorkoutGameRoadPiece &piece)
{
    const WorkoutGameRoadReliefProfile &relief = piece.relief;
    if (!relief.enabled
            || !finiteValue(relief.phaseRadians)
            || !finiteValue(relief.constantCoefficientMeters)
            || !finiteValue(relief.cosineCoefficientMeters)
            || !finiteValue(relief.sineCoefficientMeters)) {
        return false;
    }
    if (!(relief.phaseRadians >= -Pi
            && relief.phaseRadians <= Pi
            && std::abs(relief.constantCoefficientMeters) <= 5.0
            && std::abs(relief.cosineCoefficientMeters) <= 5.0
            && std::abs(relief.sineCoefficientMeters) <= 5.0
            && std::abs(relief.constantCoefficientMeters)
                + std::abs(relief.cosineCoefficientMeters)
                + std::abs(relief.sineCoefficientMeters) <= 8.0)) {
        return false;
    }
    constexpr int DerivativeSamples = 128;
    for (int index = 0; index <= DerivativeSamples; ++index) {
        const double progress = double(index) / DerivativeSamples;
        const double envelope = std::pow(std::sin(Pi * progress), 2.0);
        const double envelopeDerivative = Pi * std::sin(2.0 * Pi * progress);
        const double angle = 2.0 * Pi * progress + relief.phaseRadians;
        const double inner = relief.constantCoefficientMeters
                + relief.cosineCoefficientMeters * std::cos(angle)
                + relief.sineCoefficientMeters * std::sin(angle);
        const double innerDerivative = 2.0 * Pi
                * (-relief.cosineCoefficientMeters * std::sin(angle)
                   + relief.sineCoefficientMeters * std::cos(angle));
        const double gradePercent = 100.0
                * (envelopeDerivative * inner
                   + envelope * innerDerivative)
                / piece.lengthMeters;
        if (!finiteValue(gradePercent)
                || std::abs(gradePercent) > MaximumReliefGradePercent) {
            return false;
        }
    }
    return true;
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
            && (!decisionMustPrecedeObstacle(piece.terrain)
                || gate.decisionDistanceMeters
                    <= latestChallengeDecisionDistance(piece)
                        + DistanceToleranceMeters)
            && gate.bypassStartDistanceMeters
                <= gate.bypassEndDistanceMeters + DistanceToleranceMeters;
    const bool distancesValid = gate.prepareDistanceMeters >= 0.0
            && gate.prepareDistanceMeters
                <= courseEndMeters + DistanceToleranceMeters
            && gate.decisionDistanceMeters >= 0.0
            && gate.decisionDistanceMeters
                <= courseEndMeters + DistanceToleranceMeters
            && gate.obstacleDistanceMeters >= 0.0
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
                || gap.lockDistanceMeters
                    > line.takeoffDistanceMeters + DistanceToleranceMeters
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
                != WorkoutGameRoadPlan::LegacyGenerationVersion
            && plan.generationVersion
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
                || std::abs(piece.turnRadians)
                    > (piece.terrain == WorkoutGameTerrainKind::Berm
                        ? MaximumLegacyBermTurnRadians
                        : MaximumTurnRadians) + 1.0e-9
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
                || (plan.generationVersion
                        == WorkoutGameRoadPlan::LegacyGenerationVersion
                    ? (!zeroBank(piece.bank) || !zeroRelief(piece.relief))
                    : (!validBank(piece) || !validRelief(piece)
                       || (piece.terrain == WorkoutGameTerrainKind::Berm
                           && !piece.bank.enabled)))
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
    for (const WorkoutGameRoadPiece &banked : plan.pieces) {
        if (!banked.bank.enabled) continue;
        for (const WorkoutGameRoadPiece &feature : plan.pieces) {
            if (!feature.challenge.enabled) continue;
            const auto [protectedStart, protectedEnd] =
                    workoutGameChallengeProtectedSpan(feature);
            if (banked.bank.startDistanceMeters < protectedEnd
                    && banked.bank.endDistanceMeters > protectedStart) {
                return WorkoutGameRoadPlanValidationStatus::InvalidPlan;
            }
        }
    }
    return WorkoutGameRoadPlanValidationStatus::Ready;
}
