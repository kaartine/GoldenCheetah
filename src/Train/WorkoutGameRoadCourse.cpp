/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadCourse.h"

#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGameFeatureCatalog.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTabletopGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int IntegrationSteps = 48;
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
        break;
    default:
        break;
    }
    return 0.0;
}

double trailReliefOffset(
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
    double amplitude = (0.68 + 0.58 * piece.difficulty)
            * piece.reliefScale;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Climb:
        return 0.0;
    case WorkoutGameTerrainKind::Skinny:
        amplitude *= 0.35;
        break;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::Rollers:
        amplitude *= 0.50;
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

double targetHalfWidth(WorkoutGameTerrainKind terrain)
{
    if (terrain == WorkoutGameTerrainKind::Tabletop) {
        return WorkoutGameTabletopGeometry::profile(
                0.0).socketHalfWidthMeters;
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
    const std::size_t first = index > 0u ? index - 1u : 0u;
    const std::size_t last = std::min(
            course.pieces.size() - 1u, index + 1u);
    for (std::size_t candidate = first; candidate <= last; ++candidate) {
        offset += featureSurfaceOffset(
                course.pieces[candidate], distanceMeters);
    }
    return offset;
}

double nonPhysicalFeatureOffsetAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const std::size_t index = pieceIndexAt(course, distanceMeters);
    const std::size_t first = index > 0u ? index - 1u : 0u;
    const std::size_t last = std::min(
            course.pieces.size() - 1u, index + 1u);
    double offset = 0.0;
    for (std::size_t candidate = first; candidate <= last; ++candidate) {
        const WorkoutGameRoadPiece &piece = course.pieces[candidate];
        if (piece.terrain == WorkoutGameTerrainKind::BunnyHop
                || piece.terrain == WorkoutGameTerrainKind::LogOver) {
            offset += featureSurfaceOffset(piece, distanceMeters);
        }
    }
    return offset;
}

bool rideableSurfaceAt(
        const WorkoutGameRoadCourse &course,
        double distanceMeters)
{
    const std::size_t index = pieceIndexAt(course, distanceMeters);
    const std::size_t first = index > 0u ? index - 1u : 0u;
    const std::size_t last = std::min(
            course.pieces.size() - 1u, index + 1u);
    for (std::size_t candidate = first; candidate <= last; ++candidate) {
        const WorkoutGameRoadPiece &piece = course.pieces[candidate];
        if (!piece.challenge.enabled) continue;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece.terrain, piece.difficulty);
        if (!profile.ready) continue;
        const double local = distanceMeters
                - piece.challenge.obstacleDistanceMeters;
        if (!profile.surfacePresent(local)) return false;
    }
    return true;
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

int boundedPieceCount(double lengthMeters, int maximum)
{
    if (!std::isfinite(lengthMeters) || lengthMeters <= 0.0
            || maximum <= 1) {
        return 1;
    }
    const double requested = std::ceil(lengthMeters / 90.0);
    return int(std::clamp(requested, 1.0, double(maximum)));
}

double pieceTurn(
        const WorkoutGameSection &section,
        std::size_t pieceIndex)
{
    if (section.terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(section.difficulty);
        const bool left = ((section.visualVariant + pieceIndex) & 1u) == 0u;
        return left ? -profile.turnMagnitudeRadians
                    : profile.turnMagnitudeRadians;
    }
    double amount = 0.0;
    switch (section.terrain) {
    case WorkoutGameTerrainKind::SmoothTrail:
        amount = section.visualVariant == 0u ? 0.0 : 0.10;
        break;
    case WorkoutGameTerrainKind::Rollers: amount = 0.16; break;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden: amount = 0.12; break;
    case WorkoutGameTerrainKind::Skinny: amount = 0.08; break;
    case WorkoutGameTerrainKind::Climb:
    case WorkoutGameTerrainKind::RockSlab: amount = 0.05; break;
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::Drop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop: amount = 0.04; break;
    default: amount = 0.0; break;
    }
    amount *= 0.65 + 0.7 * std::clamp(section.difficulty, 0.0, 1.0);
    const bool left = ((section.visualVariant + pieceIndex) & 1u) == 0u;
    return left ? -amount : amount;
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
    const auto headingProgress = [&piece](double sampleProgress) {
        if (piece.terrain == WorkoutGameTerrainKind::Berm) {
            const WorkoutGameBermGeometryProfile profile =
                    WorkoutGameBermGeometry::profile(piece.difficulty);
            const double distance = piece.startDistanceMeters
                    + piece.lengthMeters * sampleProgress;
            return profile.headingProgress(
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
    if (piece.terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile berm =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        const double travelMeters = piece.lengthMeters * progress;
        const double curveStartMeters = std::clamp(
                piece.geometryAnchorDistanceMeters
                    + berm.curveStartMeters - piece.startDistanceMeters,
                0.0, piece.lengthMeters);
        const double curveEndMeters = std::clamp(
                piece.geometryAnchorDistanceMeters
                    + berm.curveEndMeters - piece.startDistanceMeters,
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

WorkoutGameRoadCourse WorkoutGameRoadCourseBuilder::build(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    WorkoutGameRoadCourse result;
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty()
            || !std::isfinite(ftpWatts)
            || ftpWatts <= 0.0) {
        return result;
    }

    result.seed = course.seed;
    WorkoutGameRoadConnector connector;
    connector.halfWidthMeters = targetHalfWidth(
            course.sections.front().terrain);
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
                ? boundedPieceCount(climbSustainedLength, 22)
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
        const int pieceCount = section.terrain == WorkoutGameTerrainKind::Berm
                ? 1
                : section.terrain == WorkoutGameTerrainKind::Climb
                    ? climbSustainedPieceCount + 2
                : boundedPieceCount(sectionLength, 24);
        const double pieceLength = sectionLength / double(pieceCount);
        const WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
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
            WorkoutGameRoadPiece piece;
            piece.sourceSectionIndex = sectionIndex;
            piece.terrain = section.terrain;
            piece.startDistanceMeters = result.totalLengthMeters;
            piece.lengthMeters = currentPieceLength;
            piece.turnRadians = pieceTurn(
                    section, std::size_t(part));
            piece.difficulty = std::clamp(section.difficulty, 0.0, 1.0);
            piece.reliefScale = std::clamp(
                    std::isfinite(section.reliefScale)
                        ? section.reliefScale : 1.0,
                    0.0, 2.5);
            piece.geometryAnchorDistanceMeters =
                    section.terrain == WorkoutGameTerrainKind::Berm
                ? sectionStart + sectionLength * 0.5
                : piece.startDistanceMeters + piece.lengthMeters * 0.5;
            piece.entry = connector;
            piece.exit = connector;
            piece.exit.halfWidthMeters = targetHalfWidth(section.terrain);
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

            const double pieceEnd = piece.startDistanceMeters
                    + piece.lengthMeters;
            const bool ownsChallenge = challenge.enabled
                    && featureFitsSection
                    && obstacleDistance >= piece.startDistanceMeters
                    && (obstacleDistance < pieceEnd
                        || (part + 1 == pieceCount
                            && obstacleDistance <= pieceEnd));
            piece.animation = animationFor(
                    section.terrain, piece.turnRadians, ownsChallenge);
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
                piece.challenge.obstacleDistanceMeters = obstacleDistance;
                piece.geometryAnchorDistanceMeters = obstacleDistance;
                const double featureEnd = obstacleDistance
                        + (section.terrain
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
                    piece.challenge.bypassLateralMeters =
                            (sectionIndex & 1u) == 0u
                            ? -bypassClearance : bypassClearance;
                }
                piece.challenge.profile.measurementStartProgress =
                        (piece.challenge.prepareDistanceMeters - sectionStart)
                        / sectionLength;
                piece.challenge.profile.decisionProgress =
                        (piece.challenge.decisionDistanceMeters - sectionStart)
                        / sectionLength;
            }

            if (section.terrain == WorkoutGameTerrainKind::Berm) {
                piece.exit = connector;
                piece.exit.halfWidthMeters = targetHalfWidth(section.terrain);
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
    result.ready = !result.pieces.empty()
            && std::isfinite(result.totalLengthMeters)
            && result.totalLengthMeters > 0.0;
    if (!result.ready) return {};
    result.visualLengthMeters = result.totalLengthMeters + VisualRunoutMeters;
    return result;
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
    if (piece.terrain == WorkoutGameTerrainKind::Berm) {
        const WorkoutGameBermGeometryProfile berm =
                WorkoutGameBermGeometry::profile(piece.difficulty);
        const double local = distance
                - piece.geometryAnchorDistanceMeters;
        result.bermBankRadians = berm.bankRadians(local, piece.turnRadians);
        result.center.halfWidthMeters = berm.halfWidthMeters(local);
        result.renderableTrailSurface = local <= berm.startMeters
                || local >= berm.endMeters;
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
    const double reliefOffset = trailReliefOffset(piece, distance);
    result.center.elevationMeters += reliefOffset
            + result.surfaceOffsetMeters;
    const double sampleRadius = std::min(0.05, piece.lengthMeters * 0.1);
    if (sampleRadius > 0.0) {
        const double low = std::max(
                piece.startDistanceMeters, distance - sampleRadius);
        const double high = std::min(
                piece.startDistanceMeters + piece.lengthMeters,
                distance + sampleRadius);
        if (high > low) {
            result.center.gradePercent +=
                    (trailReliefOffset(piece, high)
                     + surfaceOffsetAt(course, high)
                     - trailReliefOffset(piece, low)
                     - surfaceOffsetAt(course, low))
                    / (high - low) * 100.0;
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
