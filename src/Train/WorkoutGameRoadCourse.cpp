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

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
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
    result.elevationMeters = piece.entry.elevationMeters
            + piece.riseMeters * smoothStep(progress);
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
                piece.challenge.prepareDistanceMeters = sectionStart
                        + sectionLength
                            * challenge.measurementStartProgress;
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
    return result;
}
