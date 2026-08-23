/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureRuntime_h
#define _GC_WorkoutGameFeatureRuntime_h

#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameSimulation.h"

#include <cstdint>
#include <limits>
#include <vector>

enum class WorkoutGameFeaturePhase
{
    None,
    Approach,
    Measure,
    Committed,
    Action,
    Recovery
};

enum class WorkoutGameFeatureMotion
{
    None,
    Jump,
    Absorb,
    Drop
};

struct WorkoutGameFeatureRuntimeSnapshot
{
    bool ready = false;
    int sourceSectionIndex = -1;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    WorkoutGameFeaturePhase phase = WorkoutGameFeaturePhase::None;
    WorkoutGameFeatureMotion motion = WorkoutGameFeatureMotion::None;
    WorkoutGameFeatureOutcome outcome = WorkoutGameFeatureOutcome::None;
    WorkoutGameRoute route = WorkoutGameRoute::MainLine;
    double visualDistanceMeters = 0.0;
    double prepareDistanceMeters = 0.0;
    double decisionDistanceMeters = 0.0;
    double obstacleDistanceMeters = 0.0;
    double physicalTakeoffDistanceMeters = 0.0;
    double distanceToObstacleMeters = 0.0;
    double readiness = 0.0;
    double lateralOffsetMeters = 0.0;
    double verticalOffsetMeters = 0.0;
    double pitchDegrees = 0.0;
    double vibration = 0.0;
    double landingImpact = 0.0;
    std::uint64_t actionId = 0;
    bool triggerJump = false;
};

class WorkoutGameFeatureRuntime
{
public:
    bool configure(const WorkoutGameRoadCourse &course);
    void reset();
    static bool airborneExpected(
            const WorkoutGameFeatureRuntimeSnapshot &feature);
    WorkoutGameFeatureRuntimeSnapshot update(
            const WorkoutGameSimulationSnapshot &simulation) const;

private:
    struct SectionLayout
    {
        bool valid = false;
        WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
        double startDistanceMeters = 0.0;
        double endDistanceMeters = 0.0;
        std::size_t challengePieceIndex =
                std::numeric_limits<std::size_t>::max();
    };

    WorkoutGameRoadCourse configuredCourse;
    std::vector<SectionLayout> sections;
};

#endif
