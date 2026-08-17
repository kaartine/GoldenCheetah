/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingDataGenerator_h
#define _GC_TrainingDataGenerator_h

#include <cstdint>

struct TrainingDataGeneratorSample
{
    double watts = 0.0;
    double cadence = 0.0;
    double heartRate = 0.0;
    double leftRightBalance = 50.0;
    double smO2 = 50.0;
    double totalHb = 12.0;
    double coreTemperature = 37.5;
    double skinTemperature = 35.0;
    double heatStrain = 2.0;
};

class TrainingDataGenerator
{
public:
    TrainingDataGenerator();

    void reset();
    void setTargetWatts(double watts);
    double targetWatts() const { return target; }
    TrainingDataGeneratorSample nextSample();

private:
    double target = 100.0;
    double currentHeartRate = 105.0;
    std::uint64_t sampleNumber = 0;
};

#endif // _GC_TrainingDataGenerator_h
