/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCompetition_h
#define _GC_WorkoutGameCompetition_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameSimulation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct WorkoutGameGhostPoint
{
    std::int64_t timeMs = 0;
    std::uint64_t score = 0;
    WorkoutGameRoute route = WorkoutGameRoute::MainLine;
};

struct WorkoutGameGhostReplay
{
    std::uint32_t courseSeed = 0;
    std::int64_t durationMs = 0;
    std::vector<WorkoutGameGhostPoint> points;
};

class WorkoutGameGhostCodec
{
public:
    static constexpr std::size_t MaximumEncodedBytes = 256 * 1024;
    static constexpr std::size_t MaximumPoints = 7200;

    static std::string encode(const WorkoutGameGhostReplay &replay);
    static bool decode(
            const std::string &encoded,
            WorkoutGameGhostReplay &replay);
    static bool isValid(const WorkoutGameGhostReplay &replay);
    static bool isBetter(
            const WorkoutGameGhostReplay &candidate,
            const WorkoutGameGhostReplay &current);
};

class WorkoutGameGhostRecorder
{
public:
    bool configure(std::uint32_t courseSeed, std::int64_t durationMs);
    void reset();
    void record(const WorkoutGameSimulationSnapshot &snapshot);
    const WorkoutGameGhostReplay &replay() const { return recorded; }

private:
    WorkoutGameGhostReplay recorded;
};

enum class WorkoutGameCompetitorKind
{
    Ai,
    Ghost
};

struct WorkoutGameCompetitorSnapshot
{
    WorkoutGameCompetitorKind kind = WorkoutGameCompetitorKind::Ai;
    int identity = 0;
    int lane = 0;
    std::uint64_t score = 0;
    double courseProgress = 0.0;
    WorkoutGameRoute route = WorkoutGameRoute::MainLine;
    double relativeProgress = 0.0;
};

struct WorkoutGameCompetitionSnapshot
{
    bool ready = false;
    int playerRank = 0;
    int totalRiders = 0;
    std::vector<WorkoutGameCompetitorSnapshot> competitors;
};

class WorkoutGameCompetition
{
public:
    bool configure(
            const WorkoutGameCourse &course,
            const WorkoutGameGhostReplay &ghostReplay);
    WorkoutGameCompetitionSnapshot update(
            const WorkoutGameSimulationSnapshot &player) const;

private:
    std::uint64_t aiScore(int identity, std::int64_t workoutTimeMs) const;
    bool ghostAt(
            std::int64_t workoutTimeMs,
            WorkoutGameGhostPoint &point) const;
    void assignVisualProgress(
            const WorkoutGameSimulationSnapshot &player,
            std::vector<WorkoutGameCompetitorSnapshot> &competitors) const;

    WorkoutGameCourse configuredCourse;
    WorkoutGameGhostReplay configuredGhost;
};

#endif
