/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadCourse.h"

#include "WorkoutGameFeatureCatalog.h"
#include "WorkoutGameFeatureGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int IntegrationSteps = 48;
constexpr double Pi = 3.14159265358979323846;

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
        offset += 0.012 * curvatureScale * envelope
                * std::pow(std::sin(2.0 * Pi * progress), 2.0);
        break;
    case WorkoutGameTerrainKind::RockGarden:
        offset += 0.04 * curvatureScale * envelope
                * (0.35 + 0.65
                    * std::pow(std::sin(2.0 * Pi * progress), 2.0));
        break;
    case WorkoutGameTerrainKind::Rollers:
        offset += 0.10 * curvatureScale * envelope
                * std::sin(2.0 * Pi * progress);
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
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::RockSlab:
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
    double amplitude = 0.68 + 0.58 * piece.difficulty;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Climb:
        amplitude *= 1.20;
        break;
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
    case WorkoutGameTerrainKind::Skinny: return 0.38;
    case WorkoutGameTerrainKind::Berm: return 1.4;
    case WorkoutGameTerrainKind::Tabletop: return 1.10;
    case WorkoutGameTerrainKind::RockSlab: return 1.30;
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
                || piece.terrain == WorkoutGameTerrainKind::LogOver
                || piece.terrain == WorkoutGameTerrainKind::Tabletop) {
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

double pieceTurn(
        const WorkoutGameSection &section,
        std::size_t pieceIndex)
{
    double amount = 0.0;
    switch (section.terrain) {
    case WorkoutGameTerrainKind::SmoothTrail:
        amount = section.visualVariant == 0u ? 0.0 : 0.10;
        break;
    case WorkoutGameTerrainKind::Berm: amount = 0.52; break;
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
    result.headingRadians = piece.entry.headingRadians
            + piece.turnRadians * smoothStep(progress);
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
    double priorProgress = 0.0;
    const int steps = std::max(1, int(std::ceil(
            progress * double(IntegrationSteps))));
    for (int step = 1; step <= steps; ++step) {
        const double currentProgress = progress * double(step) / double(steps);
        const double middle = (priorProgress + currentProgress) * 0.5;
        const double heading = piece.entry.headingRadians
                + piece.turnRadians * smoothStep(middle);
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
        const double sectionLength = estimatedLength(section, ftpWatts);
        const int pieceCount = std::clamp(
                int(std::ceil(sectionLength / 90.0)), 1, 24);
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
        const double challengeDistance = result.totalLengthMeters
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
                    challengeDistance + 4.0);
            break;
        case WorkoutGameTerrainKind::Roots:
        case WorkoutGameTerrainKind::RockGarden:
        case WorkoutGameTerrainKind::RockSlab:
            obstacleDistance = sectionStart + sectionLength * std::min(
                    0.92, challenge.decisionProgress + 0.05);
            break;
        default:
            break;
        }
        const WorkoutGameFeatureGeometryProfile featureGeometry =
                WorkoutGameFeatureGeometry::profile(
                    section.terrain, section.difficulty);
        if (challenge.enabled && featureGeometry.ready) {
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
        }
        for (int part = 0; part < pieceCount; ++part) {
            WorkoutGameRoadPiece piece;
            piece.sourceSectionIndex = sectionIndex;
            piece.terrain = section.terrain;
            piece.startDistanceMeters = result.totalLengthMeters;
            piece.lengthMeters = pieceLength;
            piece.turnRadians = pieceTurn(
                    section, std::size_t(part));
            piece.riseMeters = pieceLength * section.gradePercent / 100.0;
            piece.difficulty = std::clamp(section.difficulty, 0.0, 1.0);
            piece.entry = connector;
            piece.exit = connector;
            piece.exit.halfWidthMeters = targetHalfWidth(section.terrain);
            piece.exit.gradePercent = section.gradePercent;
            piece.exit = connectorAt(piece, 1.0);

            const double pieceEnd = piece.startDistanceMeters
                    + piece.lengthMeters;
            const bool ownsChallenge = challenge.enabled
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
                const double maximumPreparationMeters = section.terrain
                        == WorkoutGameTerrainKind::BunnyHop ? 3.0 : 6.0;
                piece.challenge.prepareDistanceMeters = challenge.cue
                        == WorkoutGameChallengeCue::Jump
                    ? std::max(measuredPreparationDistance,
                               challengeDistance
                                    - maximumPreparationMeters)
                    : measuredPreparationDistance;
                piece.challenge.decisionDistanceMeters = challengeDistance;
                piece.challenge.obstacleDistanceMeters = obstacleDistance;
                const double featureEnd = obstacleDistance
                        + (featureGeometry.ready
                            ? featureGeometry.endMeters : 4.0);
                piece.challenge.bypassStartDistanceMeters =
                        challengeDistance;
                constexpr double MinimumBypassLengthMeters = 18.0;
                constexpr double BypassExitRunoutMeters = 10.0;
                piece.challenge.bypassEndDistanceMeters = std::min(
                        sectionStart + sectionLength,
                        std::max(
                            featureEnd + BypassExitRunoutMeters,
                            piece.challenge.bypassStartDistanceMeters
                                + MinimumBypassLengthMeters));
                const double bypassClearance =
                        featureClearanceHalfWidth(section.terrain)
                        + 0.35 + 0.38 + 0.25;
                piece.challenge.bypassLateralMeters =
                        (sectionIndex & 1u) == 0u
                        ? -bypassClearance : bypassClearance;
                piece.challenge.profile.measurementStartProgress =
                        (piece.challenge.prepareDistanceMeters - sectionStart)
                        / sectionLength;
                piece.challenge.profile.decisionProgress =
                        (piece.challenge.decisionDistanceMeters - sectionStart)
                        / sectionLength;
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
