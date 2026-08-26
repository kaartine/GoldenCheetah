/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAudioEvents_h
#define _GC_WorkoutGameAudioEvents_h

#include "WorkoutGameFeatureRuntime.h"
#include "WorkoutGameWorld.h"

#include <array>
#include <cstddef>
#include <cstdint>

enum class WorkoutGameAudioEventKind
{
    Feature,
    Landing
};

struct WorkoutGameAudioEvent
{
    std::uint64_t id = 0;
    WorkoutGameAudioEventKind kind = WorkoutGameAudioEventKind::Feature;
    std::uint64_t actionId = 0;
    double strength = 0.0;
};

struct WorkoutGameAudioEventJournal
{
    static constexpr std::size_t Capacity = 8;

    std::array<WorkoutGameAudioEvent, Capacity> events{};
    std::size_t count = 0;
    std::uint64_t epoch = 0;
};

class WorkoutGameAudioEventJournalBuilder
{
public:
    WorkoutGameAudioEventJournal update(
            const WorkoutGameFeatureRuntimeSnapshot &feature,
            const WorkoutGameWorldSnapshot &world,
            std::int64_t workoutTimeMs);
    void reset();

private:
    void append(WorkoutGameAudioEvent event);

    WorkoutGameAudioEventJournal journal;
    std::uint64_t nextEventId = 1;
    std::uint64_t lastFeatureActionId = 0;
    std::int64_t previousWorkoutTimeMs = -1;
    double previousLandingImpact = 0.0;
};

struct WorkoutGameAudioEvents
{
    bool feature = false;
    bool landing = false;
    double landingStrength = 0.0;
};

class WorkoutGameAudioCueTracker
{
public:
    WorkoutGameAudioEvents update(
            const WorkoutGameAudioEventJournal &journal);
    void reset();

private:
    std::uint64_t currentJournalEpoch = 0;
    std::uint64_t lastEventId = 0;
};

#endif
