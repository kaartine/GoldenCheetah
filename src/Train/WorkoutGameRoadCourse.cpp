/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameRoadPlan.h"
#include "WorkoutGameRoadQuality.h"

#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameChallengeGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGameFeatureCatalog.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameGapJumpGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTabletopGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int IntegrationSteps = 48;
constexpr int MaximumRoadPiecesPerSection = 4096;
constexpr double Pi = 3.14159265358979323846;
constexpr double VisualRunoutMeters = 90.0;

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

double technicalSurfaceOffset(
        const WorkoutGameRoadPiece &piece,
        double distanceMeters)
{
    const double progress = piece.lengthMeters > 0.0
            ? std::clamp(
                (distanceMeters - piece.startDistanceMeters)
                    / piece.lengthMeters,
                0.0, 1.0)
            : 0.0;
    const double envelope = std::pow(std::sin(Pi * progress), 2.0);
    const double lengthScale = std::clamp(
            piece.lengthMeters / 24.0, 0.25, 1.0);
    const double curvatureScale = lengthScale * lengthScale;
    double offset = 0.0;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Roots:
        if (!piece.challenge.enabled) {
            offset += 0.012 * curvatureScale * envelope
                    * std::pow(std::sin(2.0 * Pi * progress), 2.0);
        }
        break;
    case WorkoutGameTerrainKind::RockGarden:
        if (!piece.challenge.enabled) {
            offset += 0.04 * curvatureScale * envelope
                    * (0.35 + 0.65
                        * std::pow(std::sin(2.0 * Pi * progress), 2.0));
        }
        break;
    default:
        break;
    }
    return offset;
}

double featureSurfaceOffset(
        const WorkoutGameRoadPiece &piece,
        double distanceMeters)
{
    if (!piece.challenge.enabled) return 0.0;
    const double local = distanceMeters
            - piece.challenge.obstacleDistanceMeters;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Roots:
        return WorkoutGameRootGeometry::profile(
                piece.difficulty).surfaceOffsetMeters(local, 0.0);
    case WorkoutGameTerrainKind::RockGarden:
        return WorkoutGameRockGardenGeometry::profile(
                piece.difficulty).surfaceOffsetMeters(local, 0.0);
    case WorkoutGameTerrainKind::RockSlab:
        return WorkoutGameRockSlabGeometry::profile(
                piece.difficulty).surfaceOffsetMeters(local, 0.0);
    case WorkoutGameTerrainKind::Skinny:
        return WorkoutGameSkinnyGeometry::profile(
                piece.difficulty).surfaceOffsetMeters(local, 0.0);
    case WorkoutGameTerrainKind::Climb:
        return WorkoutGameClimbGeometry::profile(
                piece.difficulty).surfaceOffsetMeters(local, 0.0);
    case WorkoutGameTerrainKind::Rollers:
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::Drop:
        return WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty).surfaceOffset(local);
    case WorkoutGameTerrainKind::GapJump:
        // The authored branch meshes own the three different jump profiles.
        break;
    default:
        break;
    }
    return 0.0;
}

double legacyTrailReliefOffset(
        const WorkoutGameRoadPiece &piece,
        double distanceMeters)
{
    const double progress = piece.lengthMeters > 0.0
            ? std::clamp(
                (distanceMeters - piece.startDistanceMeters)
                    / piece.lengthMeters,
                0.0, 1.0)
            : 0.0;
    const double phase = double((piece.sourceSectionIndex * 37u
            + std::size_t(std::llround(piece.startDistanceMeters))) % 101u)
            / 101.0 * 2.0 * Pi;
    double amplitude = (1.25 + 1.00 * piece.difficulty)
            * piece.reliefScale;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Climb:
        // The sustained grade already supplies the large visible landform.
        // Keep local relief low enough for continuous wheel contact.
        amplitude *= 0.54;
        break;
    case WorkoutGameTerrainKind::Skinny:
        amplitude *= 0.48;
        break;
    case WorkoutGameTerrainKind::Roots:
        amplitude *= 0.62;
        break;
    case WorkoutGameTerrainKind::RockGarden:
        // The main line gets its texture from the authored stone profile;
        // broad relief must not make the safe line equally rough.
        amplitude = 0.0;
        break;
    case WorkoutGameTerrainKind::Rollers:
        // Keep the broad trail relief subordinate to the three authored
        // roller crests so their silhouette remains readable from the chase
        // camera.
        amplitude *= 0.42;
        break;
    case WorkoutGameTerrainKind::RockSlab:
        // Avoid stacking broad undulation on the slab's authored tyre-contact
        // profile; the surrounding forest floor still carries full relief.
        amplitude *= 0.65;
        break;
    case WorkoutGameTerrainKind::GapJump:
        // Keep the common sockets calm while the authored lines add relief.
        amplitude *= 0.18;
        break;
    default:
        break;
    }
    const double envelope = std::pow(std::sin(Pi * progress), 2.0);
    const double polarity = std::sin(phase) >= -0.15 ? 1.0 : -0.72;
    const double rollingShape = polarity
            * (0.78 + 0.22 * std::cos(2.0 * Pi * progress + phase))
            + 0.26 * std::sin(2.0 * Pi * progress + phase);
    return amplitude * envelope * rollingShape;
}

double trailReliefOffset(
        const WorkoutGameRoadPiece &piece,
        double distanceMeters)
{
    if (!piece.relief.enabled) {
        return legacyTrailReliefOffset(piece, distanceMeters);
    }
    const double progress = piece.lengthMeters > 0.0
            ? std::clamp(
                (distanceMeters - piece.startDistanceMeters)
                    / piece.lengthMeters,
                0.0, 1.0)
            : 0.0;
    const double envelope = std::pow(std::sin(Pi * progress), 2.0);
    const double angle = 2.0 * Pi * progress
            + piece.relief.phaseRadians;
    return envelope
            * (piece.relief.constantCoefficientMeters
               + piece.relief.cosineCoefficientMeters * std::cos(angle)
               + piece.relief.sineCoefficientMeters * std::sin(angle));
}

double targetHalfWidth(WorkoutGameTerrainKind terrain)
{
    if (terrain == WorkoutGameTerrainKind::Tabletop) {
        return WorkoutGameTabletopGeometry::profile(
                0.0).socketHalfWidthMeters;
    }
    if (terrain == WorkoutGameTerrainKind::GapJump) {
        return WorkoutGameGapJumpGeometry::canonicalProfile()
                .socketHalfWidthMeters;
    }
    return 0.68 * WorkoutGameFeatureCatalog::definition(
            terrain).trailWidthScale;
}

double featureClearanceHalfWidth(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: return 1.4;
    case WorkoutGameTerrainKind::Rollers: return 1.0;
    case WorkoutGameTerrainKind::Climb: return 1.25;
    case WorkoutGameTerrainKind::RockGarden: return 1.4;
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver: return 0.95;
    case WorkoutGameTerrainKind::Drop: return 1.25;
    case WorkoutGameTerrainKind::Skinny: return 1.55;
    case WorkoutGameTerrainKind::Berm: return 1.4;
    case WorkoutGameTerrainKind::Tabletop: return 1.10;
    case WorkoutGameTerrainKind::RockSlab: return 1.42;
    case WorkoutGameTerrainKind::GapJump: return 5.60;
    case WorkoutGameTerrainKind::SmoothTrail: return 0.68;
    }
    return 1.4;
}

std::size_t pieceIndexAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const auto found = std::upper_bound(
            course.pieces.begin(), course.pieces.end(), distanceMeters,
            [](double value, const WorkoutGameRoadPiece &piece) {
                return value < piece.startDistanceMeters;
            });
    return found == course.pieces.begin()
            ? 0u
            : std::size_t(std::distance(course.pieces.begin(), found) - 1);
}

double surfaceOffsetAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const std::size_t index = pieceIndexAt(course, distanceMeters);
    double offset = technicalSurfaceOffset(
            course.pieces[index], distanceMeters);
    if (course.challengePieceIndexReady) {
        for (std::size_t candidate : course.challengePieceIndices) {
            offset += featureSurfaceOffset(
                    course.pieces[candidate], distanceMeters);
        }
    } else {
        for (const WorkoutGameRoadPiece &piece : course.pieces) {
            if (piece.challenge.enabled) {
                offset += featureSurfaceOffset(piece, distanceMeters);
            }
        }
    }
    return offset;
}

double nonPhysicalFeatureOffsetAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    double offset = 0.0;
    const auto accumulate = [&](const WorkoutGameRoadPiece &piece) {
        if (piece.terrain == WorkoutGameTerrainKind::BunnyHop
                || piece.terrain == WorkoutGameTerrainKind::LogOver) {
            offset += featureSurfaceOffset(piece, distanceMeters);
        }
    };
    if (course.challengePieceIndexReady) {
        for (std::size_t candidate : course.challengePieceIndices) {
            accumulate(course.pieces[candidate]);
        }
    } else {
        for (const WorkoutGameRoadPiece &piece : course.pieces) {
            if (piece.challenge.enabled) accumulate(piece);
        }
    }
    return offset;
}

bool rideableSurfaceAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const auto surfacePresent = [&](const WorkoutGameRoadPiece &piece) {
        if (!piece.challenge.enabled) return true;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        if (!profile.ready) return true;
        const double local = distanceMeters
                - piece.challenge.obstacleDistanceMeters;
        return profile.surfacePresent(local);
    };
    if (course.challengePieceIndexReady) {
        for (std::size_t candidate : course.challengePieceIndices) {
            if (!surfacePresent(course.pieces[candidate])) return false;
        }
    } else {
        for (const WorkoutGameRoadPiece &piece : course.pieces) {
            if (piece.challenge.enabled && !surfacePresent(piece)) return false;
        }
    }
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.gapJump.enabled) continue;
        const WorkoutGameRoadGapJumpLine &line = piece.gapJump.lines[1];
        if (distanceMeters > line.takeoffDistanceMeters
                && distanceMeters < line.landingDistanceMeters) {
            return false;
        }
    }
    return true;
}

const WorkoutGameRoadGapJumpGate *gapJumpGateAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (piece.gapJump.enabled
                && distanceMeters >= piece.gapJump.splitStartDistanceMeters
                && distanceMeters < piece.gapJump.mergeEndDistanceMeters) {
            return &piece.gapJump;
        }
    }
    return nullptr;
}

double estimatedLength(
        const WorkoutGameSection &section,
        double ftpWatts)
{
    if (std::isfinite(section.lengthMeters) && section.lengthMeters > 0.0) {
        return section.lengthMeters;
    }
    const double seconds = double(section.durationMs) / 1000.0;
    const double intensity = std::clamp(
            section.targetWatts / ftpWatts, 0.0, 2.5);
    const double speed = section.gravityAssisted
            ? 8.5
            : 3.0 + 3.2 * std::sqrt(intensity);
    return std::clamp(seconds * speed, 24.0, 1200.0);
}

int boundedPieceCount(
        double lengthMeters,
        double targetLengthMeters,
        int maximum)
{
    if (!std::isfinite(lengthMeters) || lengthMeters <= 0.0
            || !std::isfinite(targetLengthMeters)
            || targetLengthMeters <= 0.0 || maximum <= 1) {
        return 1;
    }
    const double requested = std::ceil(
            lengthMeters / targetLengthMeters);
    return int(std::clamp(requested, 1.0, double(maximum)));
}

bool isRecoverySection(const WorkoutGameSection &section)
{
    return section.feature == WorkoutGameFeature::RecoveryDescent
            || section.feature == WorkoutGameFeature::CooldownDescent;
}

double flowingTurnFactor(
        const WorkoutGameSection &section,
        std::size_t pieceIndex)
{
    static constexpr double Factors[] = {
        0.82, 1.08, 0.82, 1.08, 0.92, 1.14, 0.92, 1.14
    };
    const std::size_t phase = std::size_t(section.visualVariant / 2u)
            % std::size(Factors);
    return Factors[(pieceIndex + phase) % std::size(Factors)];
}

double flowingTurnDirection(
        const WorkoutGameSection &section,
        std::size_t pieceIndex)
{
    static constexpr double Directions[] = {
        -1.0, -1.0, 1.0, 1.0, 1.0, 1.0, -1.0, -1.0
    };
    const std::size_t phase = std::size_t(section.visualVariant / 2u)
            % std::size(Directions);
    const double mirror = (section.visualVariant & 1u) == 0u
            ? 1.0 : -1.0;
    return mirror * Directions[
            (pieceIndex + phase) % std::size(Directions)];
}

double pieceTurn(
        const WorkoutGameSection &section,
        std::size_t pieceIndex)
{
    if (section.terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(section.difficulty);
        if (!isRecoverySection(section)) {
            const bool left = ((section.visualVariant + pieceIndex) & 1u)
                    == 0u;
            return left ? -profile.turnMagnitudeRadians
                        : profile.turnMagnitudeRadians;
        }
        const bool sharpTurn = (pieceIndex
                + std::size_t(section.visualVariant & 7u)) % 5u == 2u;
        const double magnitude = sharpTurn
                ? std::clamp(profile.turnMagnitudeRadians, 1.30, 1.48)
                : std::min(profile.turnMagnitudeRadians, 0.72);
        return flowingTurnDirection(section, pieceIndex)
                * std::min(1.48,
                    magnitude * flowingTurnFactor(section, pieceIndex));
    }
    double amount = 0.0;
    switch (section.terrain) {
    case WorkoutGameTerrainKind::SmoothTrail:
        amount = section.visualVariant == 0u ? 0.13 : 0.20;
        break;
    case WorkoutGameTerrainKind::Rollers: amount = 0.28; break;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden: amount = 0.23; break;
    case WorkoutGameTerrainKind::Skinny: amount = 0.13; break;
    case WorkoutGameTerrainKind::Climb:
    case WorkoutGameTerrainKind::RockSlab: amount = 0.16; break;
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop: amount = 0.08; break;
    case WorkoutGameTerrainKind::GapJump: amount = 0.04; break;
    case WorkoutGameTerrainKind::Drop:
        amount = isRecoverySection(section) ? 0.25 : 0.08;
        break;
    default: amount = 0.0; break;
    }
    amount *= 0.75 + 0.6 * std::clamp(
            section.difficulty, 0.0, 1.0);
    return flowingTurnDirection(section, pieceIndex)
            * amount * flowingTurnFactor(section, pieceIndex);
}

double ordinaryPieceTurn(
        const WorkoutGameCourse &course,
        const WorkoutGameSection &section,
        std::size_t ordinaryPieceIndex)
{
    const std::size_t sharpPhase = std::size_t(course.seed % 12u);
    const bool nearNinety = (ordinaryPieceIndex + sharpPhase) % 12u == 6u;
    const bool sharp = (ordinaryPieceIndex + sharpPhase) % 9u == 4u;
    const bool pronounced = (ordinaryPieceIndex + sharpPhase) % 5u == 2u;
    const double ordinaryMagnitude = std::clamp(
            0.27 + 0.10 * section.difficulty,
            0.27, 0.37);
    const double magnitude = nearNinety ? 1.3962634015954636
            : sharp ? 0.96
            : pronounced ? 0.62 : ordinaryMagnitude;
    const double mirror = (course.seed & 1u) == 0u ? 1.0 : -1.0;
    const double direction = (ordinaryPieceIndex & 1u) == 0u
            ? mirror : -mirror;
    return direction * magnitude;
}

bool validGenerationParameters(
        const WorkoutGameRoadCourseGenerationParameters &parameters)
{
    if (parameters.generationVersion
            != WorkoutGameRoadCourseGenerationParameters::CurrentVersion) {
        return false;
    }
    switch (parameters.preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
    case WorkoutGameCoursePreset::Balanced:
    case WorkoutGameCoursePreset::RideFirst:
        return true;
    }
    return false;
}

double presetTurnScale(WorkoutGameCoursePreset preset)
{
    // Generation version 1 preserves the established Workout First geometry;
    // the other modes add deterministic curvature without changing distance.
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst: return 1.0;
    case WorkoutGameCoursePreset::Balanced: return 1.30;
    case WorkoutGameCoursePreset::RideFirst: return 2.60;
    }
    return 1.0;
}

double presetScaledTurn(
        double turnRadians,
        const WorkoutGameRoadCourseGenerationParameters &parameters)
{
    constexpr double MaximumTurnRadians = 85.0 * Pi / 180.0;
    return std::clamp(
            turnRadians * presetTurnScale(parameters.preset),
            -MaximumTurnRadians, MaximumTurnRadians);
}

std::uint32_t routeHash(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

double terrainReliefScale(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Climb: return 0.54;
    case WorkoutGameTerrainKind::Skinny: return 0.48;
    case WorkoutGameTerrainKind::Roots: return 0.62;
    case WorkoutGameTerrainKind::RockGarden: return 0.0;
    case WorkoutGameTerrainKind::Rollers: return 0.42;
    case WorkoutGameTerrainKind::RockSlab: return 0.65;
    case WorkoutGameTerrainKind::GapJump: return 0.18;
    default: return 1.0;
    }
}

WorkoutGameRoadReliefProfile generatedRelief(
        const WorkoutGameCourse &course,
        const WorkoutGameSection &section,
        std::size_t pieceIndex,
        bool ownsChallenge,
        double pieceLengthMeters)
{
    WorkoutGameRoadReliefProfile result;
    result.enabled = true;
    if (ownsChallenge) return result;
    const std::uint32_t hash = routeHash(
            course.seed ^ std::uint32_t(pieceIndex * 0x9e3779b9u)
            ^ std::uint32_t(section.visualVariant * 0x85ebca6bu));
    result.phaseRadians = (double(hash & 0xffffu) / 65535.0 * 2.0 - 1.0)
            * Pi;
    const double gradeAllowance = std::clamp(
            1.0 - std::abs(section.gradePercent) / 35.0, 0.45, 1.0);
    const double amplitude = (1.65 + 0.75
            * std::clamp(section.difficulty, 0.0, 1.0))
            * std::clamp(
                std::isfinite(section.reliefScale)
                    ? section.reliefScale : 1.0,
                0.0, 2.5)
            * terrainReliefScale(section.terrain)
            * gradeAllowance
            * std::clamp(pieceLengthMeters / 14.0, 0.0, 1.0);
    const double polarity = (hash & 0x10000u) == 0u ? 1.0 : -1.0;
    result.constantCoefficientMeters = polarity * 0.78 * amplitude;
    result.cosineCoefficientMeters = polarity * 0.22 * amplitude;
    result.sineCoefficientMeters = ((hash & 0x20000u) == 0u ? 1.0 : -1.0)
            * 0.26 * amplitude;
    constexpr double GeneratedReliefGradeLimitPercent = 110.0;
    constexpr int DerivativeSamples = 128;
    double maximumGradePercent = 0.0;
    for (int index = 0; index <= DerivativeSamples; ++index) {
        const double progress = double(index) / DerivativeSamples;
        const double envelope = std::pow(std::sin(Pi * progress), 2.0);
        const double envelopeDerivative = Pi
                * std::sin(2.0 * Pi * progress);
        const double angle = 2.0 * Pi * progress + result.phaseRadians;
        const double inner = result.constantCoefficientMeters
                + result.cosineCoefficientMeters * std::cos(angle)
                + result.sineCoefficientMeters * std::sin(angle);
        const double innerDerivative = 2.0 * Pi
                * (-result.cosineCoefficientMeters * std::sin(angle)
                   + result.sineCoefficientMeters * std::cos(angle));
        maximumGradePercent = std::max(
                maximumGradePercent,
                std::abs(100.0 * (envelopeDerivative * inner
                    + envelope * innerDerivative) / pieceLengthMeters));
    }
    if (maximumGradePercent > GeneratedReliefGradeLimitPercent) {
        const double scale = GeneratedReliefGradeLimitPercent
                / maximumGradePercent;
        result.constantCoefficientMeters *= scale;
        result.cosineCoefficientMeters *= scale;
        result.sineCoefficientMeters *= scale;
    }
    return result;
}

double designSpeedMetersPerSecond(
        const WorkoutGameSection &section,
        double ftpWatts)
{
    const double intensity = std::clamp(
            section.targetWatts / ftpWatts, 0.0, 2.5);
    double speed = 4.0 + 3.5 * std::sqrt(intensity);
    if (section.gravityAssisted) speed += 2.0;
    speed -= 0.12 * section.gradePercent;
    return std::clamp(speed, 3.0, 13.0);
}

WorkoutGameRoadBankProfile generatedBank(
        const WorkoutGameCourse &course,
        const WorkoutGameSection &section,
        const WorkoutGameRoadPiece &piece,
        std::size_t pieceIndex,
        double ftpWatts,
        bool ownsChallenge)
{
    WorkoutGameRoadBankProfile result;
    if (ownsChallenge) return result;
    if (section.terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile legacy =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        result.enabled = legacy.ready;
        result.startDistanceMeters = piece.geometryAnchorDistanceMeters
                + legacy.startMeters;
        result.curveStartDistanceMeters = piece.geometryAnchorDistanceMeters
                + legacy.curveStartMeters;
        result.curveEndDistanceMeters = piece.geometryAnchorDistanceMeters
                + legacy.curveEndMeters;
        result.endDistanceMeters = piece.geometryAnchorDistanceMeters
                + legacy.endMeters;
        result.socketHalfWidthMeters = piece.entry.halfWidthMeters;
        result.activeHalfWidthMeters = std::max(
                legacy.activeHalfWidthMeters,
                result.socketHalfWidthMeters + 0.20);
        result.maximumBankRadians = legacy.maximumBankRadians;
        result.maximumLineOffsetMeters = legacy.maximumLineOffsetMeters;
        result.designSpeedMetersPerSecond = legacy.designSpeedMetersPerSecond;
        return result;
    }
    const double turn = std::abs(piece.turnRadians);
    if (turn < 0.50 || piece.lengthMeters < 8.0
            || std::abs(piece.entry.halfWidthMeters
                        - piece.exit.halfWidthMeters) > 1.0e-9) {
        return result;
    }

    const std::uint32_t hash = routeHash(
            course.seed ^ std::uint32_t(pieceIndex * 0x27d4eb2du));
    const double variation = 0.92
            + 0.16 * double(hash & 0xffu) / 255.0;
    const double speed = designSpeedMetersPerSecond(section, ftpWatts);
    const double activeLength = piece.lengthMeters * 0.84;
    const double ideal = std::atan(
            speed * speed * turn
                / std::max(1.0, activeLength * 9.80665));
    const double minimumBank = turn >= 1.30 ? 0.30
            : turn >= 0.90 ? 0.22 : 0.14;
    const double maximumBank = turn >= 1.30 ? 0.62
            : turn >= 0.90 ? 0.48 : 0.34;
    result.enabled = true;
    result.startDistanceMeters = piece.startDistanceMeters;
    result.curveStartDistanceMeters = piece.startDistanceMeters
            + piece.lengthMeters * 0.08;
    result.curveEndDistanceMeters = piece.startDistanceMeters
            + piece.lengthMeters * 0.92;
    result.endDistanceMeters = piece.startDistanceMeters + piece.lengthMeters;
    result.socketHalfWidthMeters = piece.entry.halfWidthMeters;
    const double maximumOrdinaryHalfWidth = std::max(
            result.socketHalfWidthMeters, 0.75);
    result.activeHalfWidthMeters = std::clamp(
            result.socketHalfWidthMeters + 0.12 + 0.18 * turn / 1.40,
            result.socketHalfWidthMeters, maximumOrdinaryHalfWidth);
    result.maximumBankRadians = std::clamp(
            ideal * variation, minimumBank, maximumBank);
    result.maximumLineOffsetMeters = std::min(
            0.52, result.activeHalfWidthMeters - 0.30);
    result.designSpeedMetersPerSecond = speed;
    return result;
}

WorkoutGameRoadAnimation animationFor(
        WorkoutGameTerrainKind terrain,
        double turnRadians,
        bool challengePiece)
{
    if (challengePiece) {
        switch (terrain) {
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::LogOver:
        case WorkoutGameTerrainKind::Tabletop:
        case WorkoutGameTerrainKind::GapJump:
            return WorkoutGameRoadAnimation::Jump;
        case WorkoutGameTerrainKind::Drop:
            return WorkoutGameRoadAnimation::Drop;
        case WorkoutGameTerrainKind::Skinny:
            return WorkoutGameRoadAnimation::Balance;
        default:
            break;
        }
    }
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::RockSlab:
        return WorkoutGameRoadAnimation::Absorb;
    case WorkoutGameTerrainKind::Rollers:
        return WorkoutGameRoadAnimation::Pump;
    case WorkoutGameTerrainKind::Climb:
        return WorkoutGameRoadAnimation::Climb;
    case WorkoutGameTerrainKind::Berm:
        return turnRadians < 0.0
                ? WorkoutGameRoadAnimation::LeanLeft
                : WorkoutGameRoadAnimation::LeanRight;
    default:
        return WorkoutGameRoadAnimation::None;
    }
}

WorkoutGameRoadConnector connectorAt(
        const WorkoutGameRoadPiece &piece,
        double requestedProgress)
{
    const double progress = std::clamp(requestedProgress, 0.0, 1.0);
    WorkoutGameRoadConnector result = piece.entry;
    const WorkoutGameBermGeometryProfile roadBank =
            WorkoutGameBermGeometry::profile(piece);
    const auto headingProgress = [&piece, &roadBank](double sampleProgress) {
        if (roadBank.ready) {
            const double distance = piece.startDistanceMeters
                    + piece.lengthMeters * sampleProgress;
            return roadBank.headingProgress(
                    distance - piece.geometryAnchorDistanceMeters);
        }
        return smoothStep(sampleProgress);
    };
    result.headingRadians = piece.entry.headingRadians
            + piece.turnRadians * headingProgress(progress);
    const double startElevation = piece.entry.elevationMeters;
    const double endElevation = startElevation + piece.riseMeters;
    const double startTangent = piece.entry.gradePercent
            * piece.lengthMeters / 100.0;
    const double endTangent = piece.exit.gradePercent
            * piece.lengthMeters / 100.0;
    const double p2 = progress * progress;
    const double p3 = p2 * progress;
    result.elevationMeters = (2.0 * p3 - 3.0 * p2 + 1.0)
                * startElevation
            + (p3 - 2.0 * p2 + progress) * startTangent
            + (-2.0 * p3 + 3.0 * p2) * endElevation
            + (p3 - p2) * endTangent;
    const double elevationDerivative =
            (6.0 * p2 - 6.0 * progress) * startElevation
            + (3.0 * p2 - 4.0 * progress + 1.0) * startTangent
            + (-6.0 * p2 + 6.0 * progress) * endElevation
            + (3.0 * p2 - 2.0 * progress) * endTangent;
    result.gradePercent = piece.lengthMeters > 0.0
            ? elevationDerivative / piece.lengthMeters * 100.0
            : piece.exit.gradePercent;
    result.halfWidthMeters = piece.entry.halfWidthMeters
            + (piece.exit.halfWidthMeters - piece.entry.halfWidthMeters)
                * smoothStep(progress);

    double x = piece.entry.xMeters;
    double z = piece.entry.zMeters;
    if (roadBank.ready) {
        const double travelMeters = piece.lengthMeters * progress;
        const double curveStartMeters = std::clamp(
                piece.geometryAnchorDistanceMeters
                    + roadBank.curveStartMeters - piece.startDistanceMeters,
                0.0, piece.lengthMeters);
        const double curveEndMeters = std::clamp(
                piece.geometryAnchorDistanceMeters
                    + roadBank.curveEndMeters - piece.startDistanceMeters,
                curveStartMeters, piece.lengthMeters);
        const double straightEntryMeters = std::min(
                travelMeters, curveStartMeters);
        x += std::sin(piece.entry.headingRadians) * straightEntryMeters;
        z += std::cos(piece.entry.headingRadians) * straightEntryMeters;

        const double activeEndMeters = std::min(
                travelMeters, curveEndMeters);
        if (activeEndMeters > curveStartMeters) {
            constexpr int SimpsonIntervals = 128;
            const double stepMeters =
                    (activeEndMeters - curveStartMeters)
                    / double(SimpsonIntervals);
            double sumX = 0.0;
            double sumZ = 0.0;
            for (int interval = 0;
                 interval <= SimpsonIntervals; ++interval) {
                const double localPieceDistance = curveStartMeters
                        + stepMeters * double(interval);
                const double sampleProgress = piece.lengthMeters > 0.0
                        ? localPieceDistance / piece.lengthMeters : 0.0;
                const double heading = piece.entry.headingRadians
                        + piece.turnRadians
                            * headingProgress(sampleProgress);
                const double weight = interval == 0
                            || interval == SimpsonIntervals
                        ? 1.0 : (interval & 1 ? 4.0 : 2.0);
                sumX += weight * std::sin(heading);
                sumZ += weight * std::cos(heading);
            }
            x += sumX * stepMeters / 3.0;
            z += sumZ * stepMeters / 3.0;
        }
        const double straightExitMeters = std::max(
                0.0, travelMeters - curveEndMeters);
        x += std::sin(piece.entry.headingRadians + piece.turnRadians)
                * straightExitMeters;
        z += std::cos(piece.entry.headingRadians + piece.turnRadians)
                * straightExitMeters;
        result.xMeters = x;
        result.zMeters = z;
        return result;
    }

    double priorProgress = 0.0;
    const int steps = std::max(1, int(std::ceil(
            progress * double(IntegrationSteps))));
    for (int step = 1; step <= steps; ++step) {
        const double currentProgress = progress * double(step) / double(steps);
        const double middle = (priorProgress + currentProgress) * 0.5;
        const double heading = piece.entry.headingRadians
                + piece.turnRadians * headingProgress(middle);
        const double distance = piece.lengthMeters
                * (currentProgress - priorProgress);
        x += std::sin(heading) * distance;
        z += std::cos(heading) * distance;
        priorProgress = currentProgress;
    }
    result.xMeters = x;
    result.zMeters = z;
    return result;
}

}

namespace {

WorkoutGameRoadCourse generateRoadCourse(
        const WorkoutGameCourse &course,
        double ftpWatts,
        const WorkoutGameRoadCourseGenerationParameters &parameters)
{
    WorkoutGameRoadCourse result;
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty()
            || !std::isfinite(ftpWatts)
            || ftpWatts <= 0.0
            || !validGenerationParameters(parameters)) {
        return result;
    }
    result.seed = course.seed;
    WorkoutGameRoadConnector connector;
    connector.halfWidthMeters = targetHalfWidth(
            course.sections.front().terrain);
    std::size_t ordinaryPieceIndex = 0;
    for (std::size_t sectionIndex = 0;
         sectionIndex < course.sections.size(); ++sectionIndex) {
        const WorkoutGameSection &section = course.sections[sectionIndex];
        const WorkoutGameClimbGeometryProfile climbProfile =
                WorkoutGameClimbGeometry::profile(section.difficulty);
        const double requestedSectionLength = estimatedLength(section, ftpWatts);
        const double sectionLength = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? std::max(requestedSectionLength,
                           climbProfile.minimumLengthMeters)
                : requestedSectionLength;
        const double climbEntryLength = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? std::min(climbProfile.entryTransitionMeters,
                           sectionLength * 0.25)
                : 0.0;
        const double climbCrestLength = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? std::min(climbProfile.crestTransitionMeters,
                           sectionLength * 0.25)
                : 0.0;
        const double climbSustainedLength = std::max(
                0.0, sectionLength - climbEntryLength - climbCrestLength);
        const int climbSustainedPieceCount = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? boundedPieceCount(
                    climbSustainedLength, 22.0,
                    MaximumRoadPiecesPerSection)
                : 0;
        const double climbExitGrade = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? (sectionIndex + 1 < course.sections.size()
                    ? course.sections[sectionIndex + 1].gradePercent : 0.0)
                : section.gradePercent;
        const double climbSustainedGrade = section.terrain
                    == WorkoutGameTerrainKind::Climb
                ? climbProfile.sustainedGradePercent(
                    sectionLength, section.gradePercent,
                    connector.gradePercent, climbExitGrade,
                    climbEntryLength, climbCrestLength)
                : section.gradePercent;
        const bool flowingRecovery = isRecoverySection(section);
        const int pieceCount = section.terrain == WorkoutGameTerrainKind::Berm
                    && !flowingRecovery
                ? 1
                : section.terrain == WorkoutGameTerrainKind::Climb
                    ? climbSustainedPieceCount + 2
                : flowingRecovery
                    ? boundedPieceCount(
                        sectionLength, 22.0,
                        MaximumRoadPiecesPerSection)
                : section.terrain == WorkoutGameTerrainKind::SmoothTrail
                    ? boundedPieceCount(
                        sectionLength, 22.0,
                        MaximumRoadPiecesPerSection)
                : section.terrain == WorkoutGameTerrainKind::Rollers
                    ? boundedPieceCount(
                        sectionLength, 22.0,
                        MaximumRoadPiecesPerSection)
                : section.terrain == WorkoutGameTerrainKind::Skinny
                    ? boundedPieceCount(
                        sectionLength, 32.0,
                        MaximumRoadPiecesPerSection)
                    : boundedPieceCount(
                        sectionLength, 22.0,
                        MaximumRoadPiecesPerSection);
        const double pieceLength = sectionLength / double(pieceCount);
        WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
        // Berms are normal trail turns: they alter line and lean without a
        // scored challenge, prompt or bypass, including migrated courses.
        if (section.terrain == WorkoutGameTerrainKind::Berm) {
            challenge = WorkoutGameFeatureChallengeProfile();
        }
        const double sectionStart = result.totalLengthMeters;
        result.timeline.push_back({
            sectionIndex,
            section.startMs,
            section.durationMs,
            sectionStart,
            sectionStart + sectionLength
        });
        double challengeDistance = result.totalLengthMeters
                + sectionLength * challenge.decisionProgress;
        double obstacleDistance = sectionStart + sectionLength * std::min(
                0.98, challenge.decisionProgress + 0.03);
        switch (section.terrain) {
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::LogOver:
        case WorkoutGameTerrainKind::Tabletop:
        case WorkoutGameTerrainKind::GapJump:
        case WorkoutGameTerrainKind::Drop:
            obstacleDistance = std::min(
                    sectionStart + sectionLength * 0.80,
                    challengeDistance + 7.0);
            break;
        case WorkoutGameTerrainKind::RockSlab:
            obstacleDistance = std::min(
                    sectionStart + sectionLength - 7.0,
                    challengeDistance + 7.0);
            break;
        case WorkoutGameTerrainKind::Skinny:
            obstacleDistance = std::min(
                    sectionStart + sectionLength - 9.0,
                    challengeDistance + 9.0);
            break;
        case WorkoutGameTerrainKind::Roots:
            obstacleDistance = std::min(
                    sectionStart + sectionLength - 6.0,
                    challengeDistance + 6.0);
            break;
        case WorkoutGameTerrainKind::RockGarden:
            obstacleDistance = std::min(
                    sectionStart + sectionLength - 7.0,
                    challengeDistance + 7.0);
            break;
        case WorkoutGameTerrainKind::Climb:
            obstacleDistance = sectionStart + sectionLength;
            break;
        default:
            break;
        }
        const WorkoutGameFeatureGeometryProfile featureGeometry =
                WorkoutGameFeatureGeometry::profile(
                    section.terrain, section.difficulty);
        bool featureFitsSection = true;
        if (section.terrain == WorkoutGameTerrainKind::Tabletop
                && featureGeometry.ready) {
            const WorkoutGameTabletopGeometryProfile tabletop =
                    WorkoutGameTabletopGeometry::profile(
                        section.difficulty);
            featureFitsSection = sectionLength
                    >= tabletop.splitLeadMeters
                        + tabletop.minimumBypassLengthMeters;
            if (featureFitsSection) {
                const double minimumDecision = sectionStart
                        + tabletop.splitLeadMeters;
                const double maximumDecision = sectionStart + sectionLength
                        - tabletop.minimumBypassLengthMeters;
                challengeDistance = std::clamp(
                        challengeDistance - tabletop.splitLeadMeters,
                        minimumDecision, maximumDecision);
                obstacleDistance = challengeDistance
                        + tabletop.splitLeadMeters + 4.0;
                const double requiredEnd = std::max(
                        challengeDistance
                            + tabletop.minimumBypassLengthMeters,
                        obstacleDistance + tabletop.endMeters
                            + tabletop.bypassExitRunoutMeters);
                featureFitsSection = requiredEnd
                        <= sectionStart + sectionLength + 1e-9;
            }
        } else if (section.terrain == WorkoutGameTerrainKind::GapJump) {
            const WorkoutGameGapJumpGeometryProfile gap =
                    WorkoutGameGapJumpGeometry::profile(section.difficulty);
            const double maximumGap = gap.ready
                    ? gap.lines.back().gapLengthMeters : 0.0;
            const double recoveryMeters = 6.0;
            const double minimumTakeoff = sectionStart
                    + gap.prepareLeadMeters;
            const double maximumTakeoff = sectionStart + sectionLength
                    - maximumGap - recoveryMeters - gap.mergeLengthMeters;
            featureFitsSection = gap.ready
                    && maximumTakeoff >= minimumTakeoff;
            if (featureFitsSection) {
                obstacleDistance = std::clamp(
                        challengeDistance + 7.0,
                        minimumTakeoff, maximumTakeoff);
                challengeDistance = obstacleDistance - gap.lockLeadMeters;
            }
        }
        if (challenge.enabled
                && section.terrain == WorkoutGameTerrainKind::RockSlab) {
            const WorkoutGameRockSlabGeometryProfile slab =
                    WorkoutGameRockSlabGeometry::profile(
                        section.difficulty);
            const double localObstacle = std::clamp(
                    obstacleDistance - sectionStart,
                    0.0, std::max(0.0, sectionLength - 1e-9));
            const int ownerPart = std::min(
                    pieceCount - 1,
                    int(std::floor(localObstacle / pieceLength)));
            const double ownerStart = sectionStart
                    + double(ownerPart) * pieceLength;
            const double minimumObstacle = ownerStart - slab.startMeters;
            const double maximumObstacle = ownerStart + pieceLength
                    - slab.endMeters;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        obstacleDistance, minimumObstacle, maximumObstacle);
                challengeDistance = obstacleDistance + slab.startMeters;
            }
        } else if (challenge.enabled
                && section.terrain == WorkoutGameTerrainKind::Skinny) {
            const WorkoutGameSkinnyGeometryProfile skinny =
                    WorkoutGameSkinnyGeometry::profile(section.difficulty);
            const double localObstacle = std::clamp(
                    obstacleDistance - sectionStart,
                    0.0, std::max(0.0, sectionLength - 1e-9));
            const int ownerPart = std::min(
                    pieceCount - 1,
                    int(std::floor(localObstacle / pieceLength)));
            const double ownerStart = sectionStart
                    + double(ownerPart) * pieceLength;
            const double minimumObstacle = ownerStart - skinny.startMeters;
            const double maximumObstacle = ownerStart + pieceLength
                    - skinny.endMeters;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        obstacleDistance, minimumObstacle, maximumObstacle);
                challengeDistance = obstacleDistance + skinny.startMeters;
            }
        } else if (challenge.enabled && featureGeometry.ready) {
            const double minimumObstacle = sectionStart
                    - featureGeometry.startMeters + 1.5;
            const double maximumObstacle = sectionStart + sectionLength
                    - featureGeometry.endMeters - 1.5;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        std::max(obstacleDistance,
                                 challengeDistance
                                    - featureGeometry.startMeters + 1.5),
                        minimumObstacle, maximumObstacle);
            }
        } else if (challenge.enabled
                && section.terrain == WorkoutGameTerrainKind::Berm) {
            const WorkoutGameBermGeometryProfile berm =
                    WorkoutGameBermGeometry::profile(section.difficulty);
            const double minimumObstacle = sectionStart
                    - berm.startMeters + 1.5;
            const double maximumObstacle = sectionStart + sectionLength
                    - berm.endMeters - 1.5;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        std::max(obstacleDistance,
                                 challengeDistance
                                    - berm.startMeters + 1.5),
                        minimumObstacle, maximumObstacle);
            }
        } else if (challenge.enabled
                && section.terrain == WorkoutGameTerrainKind::Roots) {
            const WorkoutGameRootGeometryProfile roots =
                    WorkoutGameRootGeometry::profile(section.difficulty);
            const double minimumObstacle = sectionStart - roots.startMeters;
            const double maximumObstacle = sectionStart + sectionLength
                    - roots.endMeters;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        obstacleDistance, minimumObstacle, maximumObstacle);
                challengeDistance = obstacleDistance + roots.startMeters;
            }
        } else if (challenge.enabled
                && section.terrain == WorkoutGameTerrainKind::RockGarden) {
            const WorkoutGameRockGardenGeometryProfile rocks =
                    WorkoutGameRockGardenGeometry::profile(
                        section.difficulty);
            const double minimumObstacle = sectionStart - rocks.startMeters;
            const double maximumObstacle = sectionStart + sectionLength
                    - rocks.endMeters;
            if (maximumObstacle >= minimumObstacle) {
                obstacleDistance = std::clamp(
                        obstacleDistance, minimumObstacle, maximumObstacle);
                challengeDistance = obstacleDistance + rocks.startMeters;
            }
        }
        for (int part = 0; part < pieceCount; ++part) {
            const double currentPieceLength = section.terrain
                        == WorkoutGameTerrainKind::Climb
                    ? (part == 0 ? climbEntryLength
                        : part + 1 == pieceCount ? climbCrestLength
                        : climbSustainedLength
                            / double(climbSustainedPieceCount))
                    : pieceLength;
            const double pieceStart = result.totalLengthMeters;
            const double pieceEnd = pieceStart + currentPieceLength;
            const bool ownsChallenge = challenge.enabled
                    && featureFitsSection
                    && obstacleDistance >= pieceStart
                    && (obstacleDistance < pieceEnd
                        || (part + 1 == pieceCount
                            && obstacleDistance
                                <= sectionStart + sectionLength + 1e-9));
            WorkoutGameRoadPiece piece;
            piece.sourceSectionIndex = sectionIndex;
            piece.terrain = section.terrain;
            piece.startDistanceMeters = pieceStart;
            piece.lengthMeters = currentPieceLength;
            const bool explicitBerm = section.terrain
                    == WorkoutGameTerrainKind::Berm;
            const double baseTurn = ownsChallenge || explicitBerm
                    ? pieceTurn(section, std::size_t(part))
                    : ordinaryPieceTurn(
                        course, section, ordinaryPieceIndex++);
            piece.turnRadians = presetScaledTurn(baseTurn, parameters);
            piece.difficulty = std::clamp(section.difficulty, 0.0, 1.0);
            piece.reliefScale = std::clamp(
                    std::isfinite(section.reliefScale)
                        ? section.reliefScale : 1.0,
                    0.0, 2.5);
            piece.geometryAnchorDistanceMeters = piece.startDistanceMeters
                    + piece.lengthMeters * 0.5;
            piece.entry = connector;
            piece.exit = connector;
            piece.exit.halfWidthMeters = targetHalfWidth(section.terrain);
            const std::size_t generatedPieceIndex = result.pieces.size();
            piece.relief = generatedRelief(
                    course, section, generatedPieceIndex, ownsChallenge,
                    piece.lengthMeters);
            piece.bank = generatedBank(
                    course, section, piece, generatedPieceIndex,
                    ftpWatts, ownsChallenge);
            if (section.terrain == WorkoutGameTerrainKind::Climb) {
                if (part == 0) {
                    piece.exit.gradePercent = climbSustainedGrade;
                } else if (part + 1 == pieceCount) {
                    piece.exit.gradePercent = climbExitGrade;
                } else {
                    piece.exit.gradePercent = climbSustainedGrade;
                }
                piece.riseMeters = currentPieceLength
                        * (piece.entry.gradePercent
                           + piece.exit.gradePercent) / 200.0;
            } else {
                piece.exit.gradePercent = section.gradePercent;
                piece.riseMeters = currentPieceLength
                        * section.gradePercent / 100.0;
            }
            piece.exit = connectorAt(piece, 1.0);

            piece.animation = animationFor(
                    piece.terrain, piece.turnRadians, ownsChallenge);
            if (ownsChallenge) {
                piece.challenge.enabled = true;
                piece.challenge.profile = challenge;
                const double measuredPreparationDistance = sectionStart
                        + sectionLength * challenge.measurementStartProgress;
                const double maximumPreparationMeters = challenge.cue
                        == WorkoutGameChallengeCue::Jump ? 3.0 : 6.0;
                piece.challenge.prepareDistanceMeters =
                        section.terrain == WorkoutGameTerrainKind::Tabletop
                    ? std::max(
                        sectionStart,
                        challengeDistance
                            - WorkoutGameTabletopGeometry::profile(
                                piece.difficulty).splitLeadMeters)
                    : section.terrain == WorkoutGameTerrainKind::GapJump
                    ? obstacleDistance
                        - WorkoutGameGapJumpGeometry::profile(
                            piece.difficulty).prepareLeadMeters
                    : section.terrain == WorkoutGameTerrainKind::Roots
                            || section.terrain
                                == WorkoutGameTerrainKind::RockGarden
                            || section.terrain
                                == WorkoutGameTerrainKind::RockSlab
                            || section.terrain
                                == WorkoutGameTerrainKind::Skinny
                    ? std::max(measuredPreparationDistance,
                               challengeDistance
                                    - maximumPreparationMeters)
                    : challenge.cue
                        == WorkoutGameChallengeCue::Jump
                    ? std::max(measuredPreparationDistance,
                               challengeDistance
                                    - maximumPreparationMeters)
                    : measuredPreparationDistance;
                piece.challenge.decisionDistanceMeters = challengeDistance;
                piece.challenge.prepareDistanceMeters = std::clamp(
                        piece.challenge.prepareDistanceMeters,
                        std::max(0.0, sectionStart),
                        std::max(std::max(0.0, sectionStart),
                                 challengeDistance));
                piece.challenge.obstacleDistanceMeters = obstacleDistance;
                piece.geometryAnchorDistanceMeters = obstacleDistance;
                if (section.terrain == WorkoutGameTerrainKind::GapJump) {
                    const WorkoutGameGapJumpGeometryProfile gap =
                            WorkoutGameGapJumpGeometry::profile(
                                piece.difficulty);
                    piece.gapJump.enabled = gap.ready;
                    piece.gapJump.prepareDistanceMeters = obstacleDistance
                            - gap.prepareLeadMeters;
                    piece.gapJump.launchWindowStartDistanceMeters =
                            obstacleDistance - gap.launchWindowLeadMeters;
                    piece.gapJump.lockDistanceMeters = obstacleDistance
                            - gap.lockLeadMeters;
                    piece.gapJump.splitStartDistanceMeters = obstacleDistance
                            - gap.splitLengthMeters;
                    const double recoveryMeters = 6.0;
                    piece.gapJump.mergeEndDistanceMeters = obstacleDistance
                            + gap.lines.back().gapLengthMeters
                            + recoveryMeters + gap.mergeLengthMeters;
                    for (std::size_t index = 0;
                            index < gap.lines.size(); ++index) {
                        const WorkoutGameGapJumpLineDefinition &sourceLine =
                                gap.lines[index];
                        WorkoutGameRoadGapJumpLine &line =
                                piece.gapJump.lines[index];
                        line.id = sourceLine.id;
                        line.takeoffDistanceMeters = obstacleDistance;
                        line.landingDistanceMeters = obstacleDistance
                                + sourceLine.gapLengthMeters;
                        line.lateralMeters = sourceLine.lateralMeters;
                        line.gapLengthMeters = sourceLine.gapLengthMeters;
                        line.minimumSpeedMetersPerSecond =
                                sourceLine.coldThresholdMetersPerSecond;
                        line.nominalFlightSeconds =
                                sourceLine.nominalFlightSeconds;
                        line.lipHeightMeters = sourceLine.lipHeightMeters;
                        line.landingDropMeters =
                                sourceLine.landingDropMeters;
                    }
                }
                const double featureEnd = obstacleDistance
                        + (section.terrain == WorkoutGameTerrainKind::GapJump
                            ? piece.gapJump.mergeEndDistanceMeters
                                - obstacleDistance
                            : section.terrain
                                    == WorkoutGameTerrainKind::RockSlab
                            ? WorkoutGameRockSlabGeometry::profile(
                                piece.difficulty).endMeters
                            : section.terrain
                                    == WorkoutGameTerrainKind::Skinny
                                ? WorkoutGameSkinnyGeometry::profile(
                                    piece.difficulty).endMeters
                            : featureGeometry.ready
                                ? featureGeometry.endMeters : 4.0);
                if (section.terrain == WorkoutGameTerrainKind::Rollers
                        || section.terrain == WorkoutGameTerrainKind::Climb
                        || section.terrain == WorkoutGameTerrainKind::Berm
                        || section.terrain == WorkoutGameTerrainKind::Roots
                        || section.terrain
                            == WorkoutGameTerrainKind::RockGarden
                        || section.terrain
                            == WorkoutGameTerrainKind::RockSlab
                        || section.terrain
                            == WorkoutGameTerrainKind::Skinny) {
                    // These are fully rollable trail surfaces. Their easier
                    // lines remain part of the same widened tread.
                    piece.challenge.bypassStartDistanceMeters =
                            challengeDistance;
                    piece.challenge.bypassEndDistanceMeters =
                            challengeDistance;
                    piece.challenge.bypassLateralMeters = 0.0;
                } else {
                    piece.challenge.bypassStartDistanceMeters =
                            challengeDistance;
                    double minimumBypassLengthMeters = 18.0;
                    double bypassExitRunoutMeters = 10.0;
                    if (section.terrain
                            == WorkoutGameTerrainKind::Tabletop) {
                        const WorkoutGameTabletopGeometryProfile tabletop =
                                WorkoutGameTabletopGeometry::profile(
                                    piece.difficulty);
                        piece.challenge.bypassStartDistanceMeters =
                                challengeDistance;
                        minimumBypassLengthMeters =
                                tabletop.minimumBypassLengthMeters;
                        bypassExitRunoutMeters =
                                tabletop.bypassExitRunoutMeters;
                    } else if (section.terrain
                            == WorkoutGameTerrainKind::GapJump) {
                        piece.challenge.bypassStartDistanceMeters =
                                piece.gapJump.splitStartDistanceMeters;
                        minimumBypassLengthMeters =
                                piece.gapJump.mergeEndDistanceMeters
                                - piece.gapJump.splitStartDistanceMeters;
                        bypassExitRunoutMeters = 0.0;
                    }
                    piece.challenge.bypassEndDistanceMeters = std::min(
                            sectionStart + sectionLength,
                            std::max(
                                featureEnd + bypassExitRunoutMeters,
                                piece.challenge.bypassStartDistanceMeters
                                    + minimumBypassLengthMeters));
                    const double bypassClearance =
                            featureClearanceHalfWidth(section.terrain)
                            + 0.35 + 0.38 + 0.25;
                    piece.challenge.bypassLateralMeters = section.terrain
                            == WorkoutGameTerrainKind::GapJump
                        ? WorkoutGameGapJumpGeometry::profile(
                            piece.difficulty).bypassLateralMeters
                        : (sectionIndex & 1u) == 0u
                            ? -bypassClearance : bypassClearance;
                }
                piece.challenge.profile.measurementStartProgress =
                        (piece.challenge.prepareDistanceMeters - sectionStart)
                        / sectionLength;
                piece.challenge.profile.decisionProgress =
                        (piece.challenge.decisionDistanceMeters - sectionStart)
                        / sectionLength;
                piece.qualityExempt = true;
                piece.qualityExemptionStartDistanceMeters = pieceStart;
                piece.qualityExemptionEndDistanceMeters = pieceEnd;
            }

            if (section.terrain == WorkoutGameTerrainKind::Berm) {
                piece.exit = connector;
                piece.exit.halfWidthMeters = piece.entry.halfWidthMeters;
                piece.exit.gradePercent = section.gradePercent;
                piece.exit = connectorAt(piece, 1.0);
                piece.animation = animationFor(
                        section.terrain, piece.turnRadians, ownsChallenge);
            }

            connector = piece.exit;
            result.totalLengthMeters = pieceEnd;
            result.pieces.push_back(piece);
        }
    }
    bool bankRemoved = false;
    for (const WorkoutGameRoadPiece &challengePiece : result.pieces) {
        if (!challengePiece.challenge.enabled) continue;
        const auto [protectedStart, protectedEnd] =
                workoutGameChallengeProtectedSpan(challengePiece);
        for (WorkoutGameRoadPiece &candidate : result.pieces) {
            if (!candidate.bank.enabled
                    || candidate.bank.endDistanceMeters <= protectedStart
                    || candidate.bank.startDistanceMeters >= protectedEnd) {
                continue;
            }
            candidate.bank = WorkoutGameRoadBankProfile();
            bankRemoved = true;
        }
    }
    if (bankRemoved) {
        WorkoutGameRoadConnector rebuiltConnector =
                result.pieces.front().entry;
        for (WorkoutGameRoadPiece &piece : result.pieces) {
            const double exitHalfWidthMeters = piece.exit.halfWidthMeters;
            const double exitGradePercent = piece.exit.gradePercent;
            piece.entry = rebuiltConnector;
            piece.exit = rebuiltConnector;
            piece.exit.halfWidthMeters = exitHalfWidthMeters;
            piece.exit.gradePercent = exitGradePercent;
            piece.exit = connectorAt(piece, 1.0);
            rebuiltConnector = piece.exit;
        }
    }

    result.ready = !result.pieces.empty()
            && std::isfinite(result.totalLengthMeters)
            && result.totalLengthMeters > 0.0;
    if (!result.ready) return {};
    result.visualLengthMeters = result.totalLengthMeters + VisualRunoutMeters;
    return result;
}

}

WorkoutGameRoadPlan WorkoutGameRoadCourseBuilder::generatePlan(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    return generatePlan(course, ftpWatts, {
        WorkoutGameRoadCourseGenerationParameters::CurrentVersion,
        WorkoutGameCoursePreset::WorkoutFirst
    });
}

WorkoutGameRoadPlan WorkoutGameRoadCourseBuilder::generatePlan(
        const WorkoutGameCourse &course,
        double ftpWatts,
        const WorkoutGameRoadCourseGenerationParameters &parameters)
{
    WorkoutGameCourse source = course;
    source.roadPlan.reset();
    const WorkoutGameRoadCourse road = generateRoadCourse(
            source, ftpWatts, parameters);
    WorkoutGameRoadPlan plan;
    if (!road.ready) return plan;
    plan.pieces = road.pieces;
    return plan;
}

WorkoutGameRoadCourse WorkoutGameRoadCourseBuilder::materialize(
        const WorkoutGameCourse &course,
        const WorkoutGameRoadPlan &plan)
{
    WorkoutGameRoadCourse result;
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty()
            || WorkoutGameRoadPlanValidator::validate(
                plan, course.sections.size())
                != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(plan).accepted()) {
        return result;
    }

    result.seed = course.seed;
    result.pieces = plan.pieces;
    result.challengePieceIndexReady = true;
    result.challengePieceIndices.reserve(result.pieces.size());
    for (std::size_t index = 0; index < result.pieces.size(); ++index) {
        if (result.pieces[index].challenge.enabled) {
            result.challengePieceIndices.push_back(index);
        }
    }
    result.totalLengthMeters = result.pieces.back().startDistanceMeters
            + result.pieces.back().lengthMeters;
    result.visualLengthMeters = result.totalLengthMeters + VisualRunoutMeters;
    result.timeline.reserve(course.sections.size());
    std::size_t pieceIndex = 0;
    for (std::size_t sectionIndex = 0;
         sectionIndex < course.sections.size(); ++sectionIndex) {
        const std::size_t firstPiece = pieceIndex;
        while (pieceIndex < result.pieces.size()
                && result.pieces[pieceIndex].sourceSectionIndex
                    == sectionIndex) {
            ++pieceIndex;
        }
        if (firstPiece == pieceIndex) return {};
        const WorkoutGameSection &section = course.sections[sectionIndex];
        const double start = result.pieces[firstPiece].startDistanceMeters;
        const WorkoutGameRoadPiece &last = result.pieces[pieceIndex - 1];
        result.timeline.push_back({
            sectionIndex,
            section.startMs,
            section.durationMs,
            start,
            last.startDistanceMeters + last.lengthMeters
        });
    }
    if (pieceIndex != result.pieces.size()) return {};
    result.ready = true;
    return result;
}

WorkoutGameRoadCourse WorkoutGameRoadCourseBuilder::build(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty()
            || !std::isfinite(ftpWatts)
            || ftpWatts <= 0.0) {
        return {};
    }
    if (course.roadPlan) return materialize(course, *course.roadPlan);
    const WorkoutGameRoadPlan plan = generatePlan(course, ftpWatts);
    return materialize(course, plan);
}

WorkoutGameRoadTimelineSample
WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
        const WorkoutGameRoadCourse &course,
        std::int64_t requestedWorkoutTimeMs)
{
    WorkoutGameRoadTimelineSample result;
    if (!course.ready || course.timeline.empty()) return result;

    const WorkoutGameRoadTimelineSection &first = course.timeline.front();
    const WorkoutGameRoadTimelineSection &last = course.timeline.back();
    const std::int64_t endTimeMs = last.startTimeMs
            + std::max<std::int64_t>(0, last.durationMs);
    const std::int64_t workoutTimeMs = std::clamp(
            requestedWorkoutTimeMs, first.startTimeMs, endTimeMs);
    auto found = std::upper_bound(
            course.timeline.begin(), course.timeline.end(), workoutTimeMs,
            [](std::int64_t value,
               const WorkoutGameRoadTimelineSection &section) {
                return value < section.startTimeMs;
            });
    const WorkoutGameRoadTimelineSection &section = found
            == course.timeline.begin() ? first : *(found - 1);
    const double progress = section.durationMs > 0
            ? std::clamp(
                double(workoutTimeMs - section.startTimeMs)
                    / double(section.durationMs),
                0.0, 1.0)
            : 1.0;
    result.ready = true;
    result.sourceSectionIndex = section.sourceSectionIndex;
    result.sectionProgress = progress;
    result.distanceMeters = section.startDistanceMeters
            + (section.endDistanceMeters - section.startDistanceMeters)
                * progress;
    return result;
}

WorkoutGameRoadSample WorkoutGameRoadCourseBuilder::sample(
        const WorkoutGameRoadCourse &course,
        double requestedDistanceMeters)
{
    WorkoutGameRoadSample result;
    if (!course.ready || course.pieces.empty()
            || !std::isfinite(requestedDistanceMeters)) {
        return result;
    }
    const double distance = std::clamp(
            requestedDistanceMeters, 0.0, course.totalLengthMeters);
    const std::size_t index = pieceIndexAt(course, distance);
    const WorkoutGameRoadPiece &piece = course.pieces[index];
    const double progress = piece.lengthMeters > 0.0
            ? std::clamp(
                (distance - piece.startDistanceMeters) / piece.lengthMeters,
                0.0, 1.0)
            : 0.0;
    result.ready = true;
    result.pieceIndex = index;
    result.terrain = piece.terrain;
    result.distanceMeters = distance;
    result.pieceProgress = progress;
    result.center = connectorAt(piece, progress);
    result.baseElevationMeters = result.center.elevationMeters;
    result.baseGradePercent = result.center.gradePercent;
    result.surfaceOffsetMeters = surfaceOffsetAt(course, distance);
    result.nonPhysicalFeatureOffsetMeters =
            nonPhysicalFeatureOffsetAt(course, distance);
    result.rideableSurface = rideableSurfaceAt(course, distance);
    const WorkoutGameBermGeometryProfile bank =
            WorkoutGameBermGeometry::profile(piece);
    if (bank.ready) {
        const double local = distance
                - piece.geometryAnchorDistanceMeters;
        result.bermBankRadians = bank.bankRadians(local, piece.turnRadians);
        result.center.halfWidthMeters = bank.halfWidthMeters(local);
        result.renderableTrailSurface = local <= bank.startMeters
                || local >= bank.endMeters;
    } else if (piece.terrain == WorkoutGameTerrainKind::Roots
            && piece.challenge.enabled) {
        const WorkoutGameRootGeometryProfile roots =
                WorkoutGameRootGeometry::profile(piece.difficulty);
        const double local = distance
                - piece.challenge.obstacleDistanceMeters;
        result.center.halfWidthMeters = roots.halfWidthMeters(local);
    } else if (piece.terrain == WorkoutGameTerrainKind::RockGarden
            && piece.challenge.enabled) {
        const WorkoutGameRockGardenGeometryProfile rocks =
                WorkoutGameRockGardenGeometry::profile(piece.difficulty);
        const double local = distance
                - piece.challenge.obstacleDistanceMeters;
        result.center.halfWidthMeters = rocks.halfWidthMeters(local);
    } else if (piece.terrain == WorkoutGameTerrainKind::RockSlab
            && piece.challenge.enabled) {
        const WorkoutGameRockSlabGeometryProfile slab =
                WorkoutGameRockSlabGeometry::profile(piece.difficulty);
        const double local = distance
                - piece.challenge.obstacleDistanceMeters;
        result.center.halfWidthMeters = slab.halfWidthMeters(local);
    } else if (piece.terrain == WorkoutGameTerrainKind::Skinny
            && piece.challenge.enabled) {
        const WorkoutGameSkinnyGeometryProfile skinny =
                WorkoutGameSkinnyGeometry::profile(piece.difficulty);
        const double local = distance
                - piece.challenge.obstacleDistanceMeters;
        result.center.halfWidthMeters = skinny.halfWidthMeters(local);
        result.renderableTrailSurface = local <= skinny.activeStartMeters
                || local >= skinny.activeEndMeters;
    }
    if (gapJumpGateAt(course, distance) != nullptr) {
        result.renderableTrailSurface = false;
    }
    const double reliefOffset = trailReliefOffset(piece, distance);
    result.center.elevationMeters += reliefOffset
            + result.surfaceOffsetMeters;
    const double sampleRadius = std::min(0.05, piece.lengthMeters * 0.1);
    if (sampleRadius > 0.0
            && distance > piece.startDistanceMeters + sampleRadius
            && distance < piece.startDistanceMeters
                + piece.lengthMeters - sampleRadius) {
        const double low = std::max(
                piece.startDistanceMeters, distance - sampleRadius);
        const double high = std::min(
                piece.startDistanceMeters + piece.lengthMeters,
                distance + sampleRadius);
        if (high > low) {
            double reliefGrade = (trailReliefOffset(piece, high)
                     + surfaceOffsetAt(course, high)
                     - trailReliefOffset(piece, low)
                     - surfaceOffsetAt(course, low))
                    / (high - low) * 100.0;
            if (piece.terrain == WorkoutGameTerrainKind::Climb) {
                reliefGrade = std::clamp(reliefGrade, -2.0, 2.0);
            }
            result.center.gradePercent += reliefGrade;
        }
    }
    return result;
}

WorkoutGameRoadSample WorkoutGameRoadCourseBuilder::sampleVisual(
        const WorkoutGameRoadCourse &course,
        double requestedDistanceMeters)
{
    if (!course.ready || course.pieces.empty()
            || !std::isfinite(requestedDistanceMeters)) {
        return {};
    }
    const double visualLength = std::max(
            course.totalLengthMeters, course.visualLengthMeters);
    const double distance = std::clamp(
            requestedDistanceMeters, 0.0, visualLength);
    if (distance <= course.totalLengthMeters) {
        return sample(course, distance);
    }

    WorkoutGameRoadSample result;
    const WorkoutGameRoadConnector &socket = course.pieces.back().exit;
    const double runoutDistance = distance - course.totalLengthMeters;
    result.ready = true;
    result.pieceIndex = course.pieces.size() - 1u;
    result.terrain = WorkoutGameTerrainKind::SmoothTrail;
    result.distanceMeters = distance;
    result.pieceProgress = 1.0;
    result.center = socket;
    result.center.xMeters += std::sin(socket.headingRadians) * runoutDistance;
    result.center.zMeters += std::cos(socket.headingRadians) * runoutDistance;
    result.center.gradePercent = 0.0;
    result.center.halfWidthMeters = 0.68;
    result.baseElevationMeters = result.center.elevationMeters;
    result.baseGradePercent = 0.0;
    return result;
}
