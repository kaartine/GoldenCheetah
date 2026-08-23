/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRoadCourse_h
#define _GC_WorkoutGameRoadCourse_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameFeatureChallenge.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class WorkoutGameRoadAnimation
{
    None,
    Absorb,
    Pump,
    Climb,
    Jump,
    Drop,
    Balance,
    LeanLeft,
    LeanRight
};

struct WorkoutGameRoadConnector
{
    double xMeters = 0.0;
    double zMeters = 0.0;
    double elevationMeters = 0.0;
    double headingRadians = 0.0;
    double halfWidthMeters = 0.68;
    double gradePercent = 0.0;
};

struct WorkoutGameRoadChallengeGate
{
    bool enabled = false;
    double prepareDistanceMeters = 0.0;
    double decisionDistanceMeters = 0.0;
    double obstacleDistanceMeters = 0.0;
    double bypassEndDistanceMeters = 0.0;
    double bypassLateralMeters = 0.0;
    WorkoutGameFeatureChallengeProfile profile;
};

struct WorkoutGameRoadPiece
{
    std::size_t sourceSectionIndex = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    WorkoutGameRoadAnimation animation = WorkoutGameRoadAnimation::None;
    double startDistanceMeters = 0.0;
    double lengthMeters = 0.0;
    double turnRadians = 0.0;
    double riseMeters = 0.0;
    double difficulty = 0.0;
    WorkoutGameRoadConnector entry;
    WorkoutGameRoadConnector exit;
    WorkoutGameRoadChallengeGate challenge;
};

struct WorkoutGameRoadTimelineSection
{
    std::size_t sourceSectionIndex = 0;
    std::int64_t startTimeMs = 0;
    std::int64_t durationMs = 0;
    double startDistanceMeters = 0.0;
    double endDistanceMeters = 0.0;
};

struct WorkoutGameRoadCourse
{
    bool ready = false;
    std::uint32_t seed = 0;
    double totalLengthMeters = 0.0;
    std::vector<WorkoutGameRoadPiece> pieces;
    std::vector<WorkoutGameRoadTimelineSection> timeline;
};

struct WorkoutGameRoadSample
{
    bool ready = false;
    std::size_t pieceIndex = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double distanceMeters = 0.0;
    double pieceProgress = 0.0;
    double surfaceOffsetMeters = 0.0;
    double nonPhysicalFeatureOffsetMeters = 0.0;
    WorkoutGameRoadConnector center;

    double surfaceElevationMeters() const
    {
        return center.elevationMeters;
    }
};

struct WorkoutGameRoadTimelineSample
{
    bool ready = false;
    std::size_t sourceSectionIndex = 0;
    double sectionProgress = 0.0;
    double distanceMeters = 0.0;
};

class WorkoutGameRoadCourseBuilder
{
public:
    static WorkoutGameRoadCourse build(
            const WorkoutGameCourse &course,
            double ftpWatts);
    static WorkoutGameRoadSample sample(
            const WorkoutGameRoadCourse &course,
            double distanceMeters);
    static WorkoutGameRoadTimelineSample sampleAtWorkoutTime(
            const WorkoutGameRoadCourse &course,
            std::int64_t workoutTimeMs);
};

#endif
