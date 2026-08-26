/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAudioEvents.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double LandingThreshold = 0.08;

double boundedImpact(double impact)
{
    return std::isfinite(impact)
            ? std::clamp(impact, 0.0, 1.0) : 0.0;
}

}

WorkoutGameAudioEventJournal WorkoutGameAudioEventJournalBuilder::update(
        const WorkoutGameFeatureRuntimeSnapshot &feature,
        const WorkoutGameWorldSnapshot &world,
        std::int64_t workoutTimeMs)
{
    if (journal.epoch == 0
            || (previousWorkoutTimeMs >= 0
                && workoutTimeMs < previousWorkoutTimeMs)) {
        reset();
    }
    previousWorkoutTimeMs = workoutTimeMs;

    if (feature.ready
            && feature.phase == WorkoutGameFeaturePhase::Measure
            && feature.actionId != 0
            && feature.actionId != lastFeatureActionId) {
        lastFeatureActionId = feature.actionId;
        append({0, WorkoutGameAudioEventKind::Feature,
                feature.actionId, 0.0});
    }

    if (world.ready) {
        const double impact = boundedImpact(world.landingImpact);
        if (impact > LandingThreshold
                && previousLandingImpact <= LandingThreshold) {
            append({0, WorkoutGameAudioEventKind::Landing, 0, impact});
        }
        previousLandingImpact = impact;
    }
    return journal;
}

void WorkoutGameAudioEventJournalBuilder::reset()
{
    const std::uint64_t nextEpoch = journal.epoch + 1;
    journal = WorkoutGameAudioEventJournal();
    journal.epoch = nextEpoch == 0 ? 1 : nextEpoch;
    nextEventId = 1;
    lastFeatureActionId = 0;
    previousWorkoutTimeMs = -1;
    previousLandingImpact = 0.0;
}

void WorkoutGameAudioEventJournalBuilder::append(
        WorkoutGameAudioEvent event)
{
    event.id = nextEventId++;
    if (journal.count < WorkoutGameAudioEventJournal::Capacity) {
        journal.events[journal.count++] = event;
        return;
    }
    std::move(journal.events.begin() + 1, journal.events.end(),
              journal.events.begin());
    journal.events.back() = event;
}

WorkoutGameAudioEvents WorkoutGameAudioCueTracker::update(
        const WorkoutGameAudioEventJournal &journal)
{
    WorkoutGameAudioEvents result;
    if (journal.epoch == 0) return result;
    if (journal.epoch != currentJournalEpoch) {
        currentJournalEpoch = journal.epoch;
        lastEventId = 0;
    }
    for (std::size_t index = 0; index < journal.count; ++index) {
        const WorkoutGameAudioEvent &event = journal.events[index];
        if (event.id <= lastEventId) continue;
        lastEventId = event.id;
        if (event.kind == WorkoutGameAudioEventKind::Feature) {
            result.feature = true;
        } else if (event.kind == WorkoutGameAudioEventKind::Landing) {
            result.landing = true;
            result.landingStrength = std::max(
                    result.landingStrength, boundedImpact(event.strength));
        }
    }
    return result;
}

void WorkoutGameAudioCueTracker::reset()
{
    currentJournalEpoch = 0;
    lastEventId = 0;
}
