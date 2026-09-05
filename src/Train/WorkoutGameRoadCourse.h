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
#include "WorkoutGameCoursePrescription.h"
#include "WorkoutGameFeatureChallenge.h"
#include "WorkoutGameGapJumpGeometry.h"

#include <array>
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
    double bypassStartDistanceMeters = 0.0;
    double bypassEndDistanceMeters = 0.0;
    double bypassLateralMeters = 0.0;
    WorkoutGameFeatureChallengeProfile profile;
};

struct WorkoutGameRoadGapJumpLine
{
    WorkoutGameGapJumpLine id = WorkoutGameGapJumpLine::None;
    double takeoffDistanceMeters = 0.0;
    double landingDistanceMeters = 0.0;
    double lateralMeters = 0.0;
    double gapLengthMeters = 0.0;
    double minimumSpeedMetersPerSecond = 0.0;
    double nominalFlightSeconds = 0.0;
    double lipHeightMeters = 0.0;
    double landingDropMeters = 0.0;
};

struct WorkoutGameRoadGapJumpGate
{
    bool enabled = false;
    double prepareDistanceMeters = 0.0;
    double launchWindowStartDistanceMeters = 0.0;
    double lockDistanceMeters = 0.0;
    double splitStartDistanceMeters = 0.0;
    double mergeEndDistanceMeters = 0.0;
    std::array<WorkoutGameRoadGapJumpLine, 3> lines{};
};

struct WorkoutGameRoadBankProfile
{
    bool enabled = false;
    double startDistanceMeters = 0.0;
    double curveStartDistanceMeters = 0.0;
    double curveEndDistanceMeters = 0.0;
    double endDistanceMeters = 0.0;
    double socketHalfWidthMeters = 0.0;
    double activeHalfWidthMeters = 0.0;
    double maximumBankRadians = 0.0;
    double maximumLineOffsetMeters = 0.0;
    double designSpeedMetersPerSecond = 0.0;
};

struct WorkoutGameRoadReliefProfile
{
    bool enabled = false;
    double phaseRadians = 0.0;
    double constantCoefficientMeters = 0.0;
    double cosineCoefficientMeters = 0.0;
    double sineCoefficientMeters = 0.0;
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
    double reliefScale = 1.0;
    double geometryAnchorDistanceMeters = 0.0;
    bool qualityExempt = false;
    double qualityExemptionStartDistanceMeters = 0.0;
    double qualityExemptionEndDistanceMeters = 0.0;
    WorkoutGameRoadBankProfile bank;
    WorkoutGameRoadReliefProfile relief;
    WorkoutGameRoadConnector entry;
    WorkoutGameRoadConnector exit;
    WorkoutGameRoadChallengeGate challenge;
    WorkoutGameRoadGapJumpGate gapJump;
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
    double visualLengthMeters = 0.0;
    std::vector<WorkoutGameRoadPiece> pieces;
    bool challengePieceIndexReady = false;
    std::vector<std::size_t> challengePieceIndices;
    std::vector<WorkoutGameRoadTimelineSection> timeline;
};

struct WorkoutGameRoadSample
{
    bool ready = false;
    std::size_t pieceIndex = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double distanceMeters = 0.0;
    double pieceProgress = 0.0;
    double baseElevationMeters = 0.0;
    double baseGradePercent = 0.0;
    double surfaceOffsetMeters = 0.0;
    double nonPhysicalFeatureOffsetMeters = 0.0;
    bool rideableSurface = true;
    bool renderableTrailSurface = true;
    double bermBankRadians = 0.0;
    WorkoutGameRoadConnector center;

    double surfaceElevationMeters() const
    {
        return center.elevationMeters;
    }

    double visualGroundElevationMeters() const
    {
        return center.elevationMeters - nonPhysicalFeatureOffsetMeters;
    }
};

struct WorkoutGameRoadTimelineSample
{
    bool ready = false;
    std::size_t sourceSectionIndex = 0;
    double sectionProgress = 0.0;
    double distanceMeters = 0.0;
};

struct WorkoutGameRoadCourseGenerationParameters
{
    static constexpr std::uint32_t CurrentVersion = 1;

    std::uint32_t generationVersion = CurrentVersion;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
};

class WorkoutGameRoadCourseBuilder
{
public:
    static WorkoutGameRoadPlan generatePlan(
            const WorkoutGameCourse &course,
            double ftpWatts);
    static WorkoutGameRoadPlan generatePlan(
            const WorkoutGameCourse &course,
            double ftpWatts,
            const WorkoutGameRoadCourseGenerationParameters &parameters);
    static WorkoutGameRoadCourse materialize(
            const WorkoutGameCourse &course,
            const WorkoutGameRoadPlan &plan);
    static WorkoutGameRoadCourse build(
            const WorkoutGameCourse &course,
            double ftpWatts);
    static WorkoutGameRoadSample sample(
            const WorkoutGameRoadCourse &course,
            double distanceMeters);
    static WorkoutGameRoadSample sampleVisual(
            const WorkoutGameRoadCourse &course,
            double distanceMeters);
    static WorkoutGameRoadTimelineSample sampleAtWorkoutTime(
            const WorkoutGameRoadCourse &course,
            std::int64_t workoutTimeMs);
};

#endif
