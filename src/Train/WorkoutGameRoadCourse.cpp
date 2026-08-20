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

double smootherStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * clamped
            * (clamped * (clamped * 6.0 - 15.0) + 10.0);
}

double challengeSurfaceOffset(
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
    double offset = 0.0;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Roots:
        offset += 0.07 * envelope
                * std::pow(std::sin(8.0 * Pi * progress), 2.0);
        break;
    case WorkoutGameTerrainKind::RockGarden:
        offset += 0.16 * envelope
                * (0.35 + 0.65
                    * std::pow(std::sin(7.0 * Pi * progress), 2.0));
        break;
    case WorkoutGameTerrainKind::Rollers:
        offset += 0.28 * envelope * std::sin(4.0 * Pi * progress);
        break;
    default:
        break;
    }
    if (!piece.challenge.enabled) return offset;

    const double local = distanceMeters
            - piece.challenge.obstacleDistanceMeters;
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
        if (std::abs(local) < 1.0) {
            const double height = piece.terrain
                    == WorkoutGameTerrainKind::LogOver ? 0.28 : 0.18;
            offset += height * (1.0 - smootherStep(std::abs(local)));
        }
        break;
    case WorkoutGameTerrainKind::Tabletop:
        if (local >= -4.0 && local < 0.0) {
            offset += 0.75 * smootherStep((local + 4.0) / 4.0);
        } else if (local >= 0.0 && local <= 4.0) {
            offset += 0.75;
        } else if (local > 4.0 && local < 8.0) {
            offset += 0.75 * (1.0 - smootherStep((local - 4.0) / 4.0));
        }
        break;
    case WorkoutGameTerrainKind::RockSlab:
        if (local >= -3.0 && local < 0.0) {
            offset += 0.42 * smootherStep((local + 3.0) / 3.0);
        } else if (local >= 0.0 && local <= 3.0) {
            offset += 0.42;
        } else if (local > 3.0 && local < 6.0) {
            offset += 0.42 * (1.0 - smootherStep((local - 3.0) / 3.0));
        }
        break;
    case WorkoutGameTerrainKind::Drop:
        if (local >= -1.5 && local <= 0.5) {
            offset += 0.18 * std::sin(
                    Pi * (local + 1.5) / 2.0);
        }
        break;
    default:
        break;
    }
    return offset;
}

double targetHalfWidth(WorkoutGameTerrainKind terrain)
{
    return 1.45 * WorkoutGameFeatureCatalog::definition(
            terrain).trailWidthScale;
}

double estimatedLength(
        const WorkoutGameSection &section,
        double ftpWatts)
{
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
    case WorkoutGameTerrainKind::Berm: amount = 0.52; break;
    case WorkoutGameTerrainKind::Rollers: amount = 0.16; break;
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden: amount = 0.12; break;
    case WorkoutGameTerrainKind::Skinny: amount = 0.08; break;
    case WorkoutGameTerrainKind::Climb:
    case WorkoutGameTerrainKind::RockSlab: amount = 0.05; break;
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
        double obstacleProgress = std::min(
                0.98, challenge.decisionProgress + 0.03);
        switch (section.terrain) {
        case WorkoutGameTerrainKind::BunnyHop:
        case WorkoutGameTerrainKind::LogOver:
        case WorkoutGameTerrainKind::Tabletop:
        case WorkoutGameTerrainKind::Drop:
            obstacleProgress = std::clamp(
                    challenge.decisionProgress + 0.08, 0.70, 0.82);
            break;
        case WorkoutGameTerrainKind::Roots:
        case WorkoutGameTerrainKind::RockGarden:
        case WorkoutGameTerrainKind::RockSlab:
            obstacleProgress = std::min(
                    0.92, challenge.decisionProgress + 0.05);
            break;
        default:
            break;
        }
        const double obstacleDistance = sectionStart
                + sectionLength * obstacleProgress;

        for (int part = 0; part < pieceCount; ++part) {
            WorkoutGameRoadPiece piece;
            piece.sourceSectionIndex = sectionIndex;
            piece.terrain = section.terrain;
            piece.startDistanceMeters = result.totalLengthMeters;
            piece.lengthMeters = pieceLength;
            piece.turnRadians = pieceTurn(
                    section, std::size_t(part));
            piece.riseMeters = pieceLength * section.gradePercent / 100.0;
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
                piece.challenge.prepareDistanceMeters = std::max(
                        measuredPreparationDistance,
                        challengeDistance - 6.0);
                piece.challenge.decisionDistanceMeters = challengeDistance;
                piece.challenge.obstacleDistanceMeters = obstacleDistance;
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
    auto found = std::upper_bound(
            course.pieces.begin(), course.pieces.end(), distance,
            [](double value, const WorkoutGameRoadPiece &piece) {
                return value < piece.startDistanceMeters;
            });
    const std::size_t index = found == course.pieces.begin()
            ? 0
            : std::size_t(std::distance(course.pieces.begin(), found) - 1);
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
    result.center.elevationMeters += challengeSurfaceOffset(piece, distance);
    const double sampleRadius = std::min(0.05, piece.lengthMeters * 0.1);
    if (sampleRadius > 0.0) {
        const double low = std::max(
                piece.startDistanceMeters, distance - sampleRadius);
        const double high = std::min(
                piece.startDistanceMeters + piece.lengthMeters,
                distance + sampleRadius);
        if (high > low) {
            result.center.gradePercent +=
                    (challengeSurfaceOffset(piece, high)
                     - challengeSurfaceOffset(piece, low))
                    / (high - low) * 100.0;
        }
    }
    return result;
}
