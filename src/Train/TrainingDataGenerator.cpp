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
#include <cctype>
#include <cmath>
#include <string>

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

std::string normalizedProfile(std::string_view profile)
{
    std::string result;
    bool pendingSeparator = false;
    for (const unsigned char character : profile) {
        if (std::isspace(character) || character == '_' || character == '-') {
            pendingSeparator = !result.empty();
            continue;
        }
        if (pendingSeparator) result.push_back('-');
        pendingSeparator = false;
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

}

TrainingDataGenerator::TrainingDataGenerator()
{
    reset();
}

TrainingDataGeneratorMode TrainingDataGenerator::modeFromProfile(
        std::string_view profile)
{
    const std::string normalized = normalizedProfile(profile);
    if (normalized == "on-target") return TrainingDataGeneratorMode::OnTarget;
    if (normalized == "over-target") {
        return TrainingDataGeneratorMode::OverTarget;
    }
    if (normalized == "under-target") {
        return TrainingDataGeneratorMode::UnderTarget;
    }
    if (normalized == "cadence-low") {
        return TrainingDataGeneratorMode::CadenceLow;
    }
    if (normalized == "cadence-high") {
        return TrainingDataGeneratorMode::CadenceHigh;
    }
    return TrainingDataGeneratorMode::FollowTarget;
}

void TrainingDataGenerator::reset()
{
    sampleNumber = 0;
    currentWatts = target;
    currentHeartRate = bounded(90.0 + target * 0.28, 90.0, 185.0);
}

void TrainingDataGenerator::setMode(TrainingDataGeneratorMode mode)
{
    generatorMode = mode;
    reset();
}

const char *TrainingDataGenerator::modeLabel() const
{
    switch (generatorMode) {
    case TrainingDataGeneratorMode::OnTarget: return "On target";
    case TrainingDataGeneratorMode::OverTarget: return "Over target";
    case TrainingDataGeneratorMode::UnderTarget: return "Under target";
    case TrainingDataGeneratorMode::CadenceLow: return "Low cadence";
    case TrainingDataGeneratorMode::CadenceHigh: return "High cadence";
    case TrainingDataGeneratorMode::FollowTarget: return "Follow target";
    }
    return "Follow target";
}

void TrainingDataGenerator::setTargetWatts(double watts)
{
    target = std::isfinite(watts) ? bounded(watts, 0.0, 2500.0) : 0.0;
}

TrainingDataGeneratorSample TrainingDataGenerator::nextSample()
{
    const std::size_t phase = static_cast<std::size_t>(
            sampleNumber % WattOffsets.size());
    double actualWatts = target;
    double cadence = 88.0;
    if (generatorMode == TrainingDataGeneratorMode::FollowTarget) {
        currentWatts += bounded(target - currentWatts, -40.0, 40.0);
        const double riderPhase = static_cast<double>(sampleNumber % 150U)
                * (2.0 * std::acos(-1.0) / 150.0);
        const double simulatedEffort = currentWatts
                * (1.0 + 0.10 * std::sin(riderPhase));
        actualWatts = simulatedEffort + WattOffsets[phase];
        cadence = 78.0 + actualWatts / 20.0 + CadenceOffsets[phase];
    } else if (generatorMode == TrainingDataGeneratorMode::OnTarget) {
        // RealtimeData stores power as an integer. Round upward so a
        // fractional target cannot turn the deterministic pass scenario into
        // an under-target sample after that conversion.
        actualWatts = std::ceil(target);
    } else if (generatorMode == TrainingDataGeneratorMode::OverTarget) {
        actualWatts = target * 1.2;
        cadence = 92.0;
    } else if (generatorMode == TrainingDataGeneratorMode::UnderTarget) {
        actualWatts = target * 0.42;
        cadence = 45.0;
    } else if (generatorMode == TrainingDataGeneratorMode::CadenceLow) {
        cadence = 55.0;
    } else if (generatorMode == TrainingDataGeneratorMode::CadenceHigh) {
        cadence = 105.0;
    }
    actualWatts = bounded(actualWatts, 0.0, 2500.0);
    cadence = bounded(cadence, 0.0, 300.0);

    const double desiredHeartRate = bounded(
            90.0 + actualWatts * 0.28, 90.0, 185.0);
    const double heartRateDelta = bounded(
            desiredHeartRate - currentHeartRate, -0.35, 0.8);
    currentHeartRate += heartRateDelta;

    TrainingDataGeneratorSample sample;
    sample.watts = actualWatts;
    sample.cadence = cadence;
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
