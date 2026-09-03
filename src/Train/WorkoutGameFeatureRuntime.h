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

#include "WorkoutGameGapJumpLaunchWindow.h"
#include "WorkoutGameGapJumpSelector.h"
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
    Balance,
    Climb,
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
    double launchWindowStartDistanceMeters = 0.0;
    double decisionDistanceMeters = 0.0;
    double obstacleDistanceMeters = 0.0;
    double physicalTakeoffDistanceMeters = 0.0;
    double actionStartDistanceMeters = 0.0;
    double actionEndDistanceMeters = 0.0;
    double distanceToObstacleMeters = 0.0;
    double readiness = 0.0;
    double bermLineBias = 0.0;
    double lateralOffsetMeters = 0.0;
    double verticalOffsetMeters = 0.0;
    double flightDurationSeconds = 0.0;
    double pitchDegrees = 0.0;
    double vibration = 0.0;
    double landingImpact = 0.0;
    WorkoutGameGapJumpLine provisionalGapLine =
            WorkoutGameGapJumpLine::None;
    WorkoutGameGapJumpLine lockedGapLine = WorkoutGameGapJumpLine::None;
    WorkoutGameGapJumpLine steeringGapLine = WorkoutGameGapJumpLine::None;
    double predictedApproachSpeedMetersPerSecond = 0.0;
    double launchRollingSpeedMetersPerSecond = 0.0;
    double launchBestSpeedMetersPerSecond = 0.0;
    int launchPowerHoldMilliseconds = 0;
    double selectedGapLengthMeters = 0.0;
    bool launchWindowActive = false;
    bool launchSpeedReady = false;
    bool launchPowerReady = false;
    bool gapLineReachable = true;
    bool gapLineLocked = false;
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
            const WorkoutGameSimulationSnapshot &simulation,
            double actualWatts = 0.0,
            double targetWatts = 0.0);

private:
    struct SectionLayout
    {
        bool valid = false;
        WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
        double startDistanceMeters = 0.0;
        double endDistanceMeters = 0.0;
        std::int64_t durationMs = 0;
        std::size_t challengePieceIndex =
                std::numeric_limits<std::size_t>::max();
        std::size_t terrainPieceIndex =
                std::numeric_limits<std::size_t>::max();
    };

    struct GapJumpState
    {
        int sourceSectionIndex = -1;
        std::uint64_t baseActionId = 0;
        std::int64_t lastWorkoutTimeMs = 0;
        bool hasTimestamp = false;
        double lastVisualDistanceMeters = 0.0;
        bool hasVisualDistance = false;
        bool launchWindowStarted = false;
        WorkoutGameGapJumpLaunchWindow launchWindow;
        WorkoutGameGapJumpLine launchLine = WorkoutGameGapJumpLine::None;
        bool lineReachable = true;
        double lateralOffsetMeters = 0.0;
        double lateralVelocityMetersPerSecond = 0.0;
        WorkoutGameGapJumpSelector selector;
    };

    struct BankLineState
    {
        bool hasTimestamp = false;
        std::int64_t lastWorkoutTimeMs = 0;
        double lateralOffsetMeters = 0.0;
        double lateralVelocityMetersPerSecond = 0.0;
        double maximumLineOffsetMeters = 0.0;
    };

    WorkoutGameRoadCourse configuredCourse;
    std::vector<SectionLayout> sections;
    GapJumpState gapJumpState;
    BankLineState bankLineState;
};

#endif
