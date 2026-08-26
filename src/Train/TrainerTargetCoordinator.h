/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainerTargetCoordinator_h
#define _GC_TrainerTargetCoordinator_h

#include <vector>

class TrainerTargetDevice
{
public:
    virtual ~TrainerTargetDevice() = default;

    virtual void setLoad(double value) = 0;
    virtual void setGradient(double value) = 0;
    virtual void setWindResistance(double value) = 0;
};

enum class TrainerTargetMode {
    Erg,
    Slope
};

const char *trainerTargetModeTraceName(TrainerTargetMode mode);

struct TrainerTarget
{
    static TrainerTarget erg(double load, double workoutPosition);
    static TrainerTarget slope(double gradient,
                               double windResistance,
                               double workoutPosition);

    TrainerTargetMode mode = TrainerTargetMode::Erg;
    double value = 0.0;
    double windResistance = 0.0;
    double workoutPosition = 0.0;
};

enum class TrainerTargetResult {
    Applied,
    WorkoutFinished
};

class TrainerTargetCoordinator
{
public:
    TrainerTargetResult apply(
            const TrainerTarget &target,
            const std::vector<TrainerTargetDevice *> &devices) const;
};

#endif
