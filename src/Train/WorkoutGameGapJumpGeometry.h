/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameGapJumpGeometry_h
#define _GC_WorkoutGameGapJumpGeometry_h

#include <array>
#include <cstdint>

enum class WorkoutGameGapJumpLine : std::uint8_t {
    None,
    Short,
    Medium,
    Long
};

struct WorkoutGameGapJumpLineDefinition
{
    WorkoutGameGapJumpLine id = WorkoutGameGapJumpLine::None;
    double lateralMeters = 0.0;
    double gapLengthMeters = 0.0;
    double coldThresholdMetersPerSecond = 0.0;
    double nominalFlightSeconds = 0.0;
    double lipHeightMeters = 0.0;
    double landingDropMeters = 0.0;
};

struct WorkoutGameGapJumpGeometryProfile
{
    bool ready = false;
    double difficulty = 0.0;
    double socketHalfWidthMeters = 0.0;
    double prepareLeadMeters = 0.0;
    double lockLeadMeters = 0.0;
    double splitLengthMeters = 0.0;
    double mergeLengthMeters = 0.0;
    double bypassLateralMeters = 0.0;
    double featureStartMeters = 0.0;
    double featureEndMeters = 0.0;
    double hysteresisMetersPerSecond = 0.0;
    double maximumFlightSeconds = 0.0;
    std::array<WorkoutGameGapJumpLineDefinition, 3> lines{};
};

class WorkoutGameGapJumpGeometry
{
public:
    static WorkoutGameGapJumpGeometryProfile canonicalProfile();
    static WorkoutGameGapJumpGeometryProfile profile(double difficulty);
    static WorkoutGameGapJumpGeometryProfile mirrored(
            const WorkoutGameGapJumpGeometryProfile &profile);
    static bool validate(const WorkoutGameGapJumpGeometryProfile &profile);
    static const WorkoutGameGapJumpLineDefinition *line(
            const WorkoutGameGapJumpGeometryProfile &profile,
            WorkoutGameGapJumpLine id);
};

#endif
