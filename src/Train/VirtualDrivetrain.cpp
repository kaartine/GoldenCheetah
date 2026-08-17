/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "VirtualDrivetrain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int ChainringTeeth = 34;
constexpr std::array<int, 12> SprocketTeeth = {
    51, 45, 39, 33, 28, 24, 21, 18, 16, 14, 12, 10
};

}

VirtualDrivetrain::VirtualDrivetrain(int initialGear)
    : initialGear_(clampGear(initialGear)),
      gear_(initialGear_)
{
}

int VirtualDrivetrain::minimumGear() const
{
    return 1;
}

int VirtualDrivetrain::maximumGear() const
{
    return int(SprocketTeeth.size());
}

int VirtualDrivetrain::gear() const
{
    return gear_;
}

int VirtualDrivetrain::chainringTeeth() const
{
    return ChainringTeeth;
}

int VirtualDrivetrain::sprocketTeeth() const
{
    return SprocketTeeth.at(size_t(gear_ - 1));
}

bool VirtualDrivetrain::shiftUp()
{
    return setGear(gear_ + 1);
}

bool VirtualDrivetrain::shiftDown()
{
    return setGear(gear_ - 1);
}

bool VirtualDrivetrain::setGear(int gear)
{
    const int clamped = clampGear(gear);
    if (clamped == gear_) return false;
    gear_ = clamped;
    return true;
}

void VirtualDrivetrain::reset()
{
    gear_ = initialGear_;
}

double VirtualDrivetrain::gearRatio() const
{
    return double(ChainringTeeth) / double(sprocketTeeth());
}

double VirtualDrivetrain::relativeRatio() const
{
    const double initialRatio = double(ChainringTeeth)
            / double(SprocketTeeth.at(size_t(initialGear_ - 1)));
    return gearRatio() / initialRatio;
}

double VirtualDrivetrain::speedKph(
        double cadenceRpm,
        double wheelCircumferenceMeters) const
{
    if (!std::isfinite(cadenceRpm)
            || !std::isfinite(wheelCircumferenceMeters)
            || cadenceRpm <= 0.0
            || wheelCircumferenceMeters <= 0.0) {
        return 0.0;
    }

    const double speed = cadenceRpm * gearRatio()
            * wheelCircumferenceMeters * 60.0 / 1000.0;
    return std::isfinite(speed) ? speed : 0.0;
}

int VirtualDrivetrain::clampGear(int gear)
{
    return std::clamp(gear, 1, int(SprocketTeeth.size()));
}
