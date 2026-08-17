/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainerTargetCoordinator.h"

namespace {

constexpr double WorkoutFinishedTarget = -100.0;

}

TrainerTarget TrainerTarget::erg(double load, double workoutPosition)
{
    TrainerTarget target;
    target.mode = TrainerTargetMode::Erg;
    target.value = load;
    target.workoutPosition = workoutPosition;
    return target;
}

TrainerTarget TrainerTarget::slope(double gradient,
                                   double windResistance,
                                   double workoutPosition)
{
    TrainerTarget target;
    target.mode = TrainerTargetMode::Slope;
    target.value = gradient;
    target.windResistance = windResistance;
    target.workoutPosition = workoutPosition;
    return target;
}

TrainerTargetResult TrainerTargetCoordinator::apply(
        const TrainerTarget &target,
        const std::vector<TrainerTargetDevice *> &devices) const
{
    if (target.value == WorkoutFinishedTarget) {
        return TrainerTargetResult::WorkoutFinished;
    }

    for (TrainerTargetDevice *device : devices) {
        if (!device) continue;

        if (target.mode == TrainerTargetMode::Erg) {
            device->setLoad(target.value);
        } else {
            device->setGradient(target.value);
            device->setWindResistance(target.windResistance);
        }
    }

    return TrainerTargetResult::Applied;
}
