/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutRideTargetPlanner.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double ReferenceCadenceRpm = 85.0;
constexpr double MinimumCadenceFactor = 0.5;
constexpr double MaximumCadenceFactor = 1.75;
constexpr double MaximumTargetWatts = 1500.0;

}

TrainerControlCapabilities TrainerControlCapabilities::targetPowerOnly()
{
    TrainerControlCapabilities result;
    result.targetPower = true;
    return result;
}

TrainerControlCapabilities TrainerControlCapabilities::commonCapabilities(
        std::initializer_list<TrainerControlCapabilities> capabilities)
{
    if (capabilities.size() == 0) return TrainerControlCapabilities();

    auto current = capabilities.begin();
    TrainerControlCapabilities result = *current;
    for (++current; current != capabilities.end(); ++current) {
        result.targetPower &= current->targetPower;
        result.targetResistance &= current->targetResistance;
        result.simulation &= current->simulation;
        result.nativeVirtualGearing &= current->nativeVirtualGearing;
    }
    return result;
}

PlannedTrainerTarget WorkoutRideTargetPlanner::plan(
        const WorkoutRideTargetInput &input,
        const TrainerControlCapabilities &capabilities)
{
    const PlannedTrainerTarget standardTarget = {
        PlannedTrainerTargetMode::StandardErg,
        input.workoutWatts
    };

    if (!input.enabled
            || !capabilities.targetPower
            || !std::isfinite(input.workoutWatts)
            || input.workoutWatts < 0.0
            || !std::isfinite(input.relativeGearRatio)
            || input.relativeGearRatio <= 0.0) {
        return standardTarget;
    }

    double cadenceFactor = 1.0;
    if (std::isfinite(input.cadenceRpm) && input.cadenceRpm > 0.0) {
        cadenceFactor = input.cadenceRpm / ReferenceCadenceRpm;
    }
    cadenceFactor = std::clamp(
            cadenceFactor, MinimumCadenceFactor, MaximumCadenceFactor);

    const double targetWatts = input.workoutWatts
            * cadenceFactor * input.relativeGearRatio;
    if (!std::isfinite(targetWatts)) return standardTarget;

    return {
        PlannedTrainerTargetMode::WorkoutRidePower,
        std::clamp(targetWatts, 0.0, MaximumTargetWatts)
    };
}
