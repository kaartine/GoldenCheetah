/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainingDataGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr std::array<double, 8> WattOffsets = {
    0.0, 2.0, -1.0, 3.0, -2.0, 1.0, -3.0, 2.0
};
constexpr std::array<double, 8> CadenceOffsets = {
    0.0, 1.0, 0.0, -1.0, 1.0, 0.0, -2.0, 1.0
};
constexpr std::array<double, 8> BalanceOffsets = {
    0.0, 0.5, 1.0, 0.5, 0.0, -0.5, -1.0, -0.5
};

double bounded(double value, double minimum, double maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

}

TrainingDataGenerator::TrainingDataGenerator()
{
    reset();
}

void TrainingDataGenerator::reset()
{
    sampleNumber = 0;
    currentWatts = target;
    currentHeartRate = bounded(90.0 + target * 0.28, 90.0, 185.0);
}

void TrainingDataGenerator::setTargetWatts(double watts)
{
    target = std::isfinite(watts) ? bounded(watts, 0.0, 2500.0) : 0.0;
}

TrainingDataGeneratorSample TrainingDataGenerator::nextSample()
{
    const std::size_t phase = static_cast<std::size_t>(
            sampleNumber % WattOffsets.size());
    currentWatts += bounded(target - currentWatts, -40.0, 40.0);
    const double riderPhase = static_cast<double>(sampleNumber % 150U)
            * (2.0 * std::acos(-1.0) / 150.0);
    const double simulatedEffort = currentWatts
            * (1.0 + 0.10 * std::sin(riderPhase));
    const double actualWatts = bounded(
            simulatedEffort + WattOffsets[phase], 0.0, 2500.0);

    const double desiredHeartRate = bounded(
            90.0 + actualWatts * 0.28, 90.0, 185.0);
    const double heartRateDelta = bounded(
            desiredHeartRate - currentHeartRate, -0.35, 0.8);
    currentHeartRate += heartRateDelta;

    TrainingDataGeneratorSample sample;
    sample.watts = actualWatts;
    sample.cadence = bounded(
            78.0 + actualWatts / 20.0 + CadenceOffsets[phase], 70.0, 105.0);
    sample.heartRate = currentHeartRate;
    sample.leftRightBalance = 50.0 + BalanceOffsets[phase];
    sample.smO2 = 52.0 - bounded(actualWatts / 100.0, 0.0, 12.0)
            + BalanceOffsets[phase];
    sample.totalHb = 12.4 + BalanceOffsets[phase] * 0.05;
    sample.coreTemperature = 37.5 + static_cast<double>(phase) * 0.01;
    sample.skinTemperature = 35.0 - static_cast<double>(phase) * 0.01;
    sample.heatStrain = 2.0 + bounded(actualWatts / 1000.0, 0.0, 2.5);

    ++sampleNumber;
    return sample;
}
