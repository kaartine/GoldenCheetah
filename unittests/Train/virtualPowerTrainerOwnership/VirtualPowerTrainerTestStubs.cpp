/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/DeviceConfiguration.h"
#include "Train/PolynomialRegression.h"
#include "Train/RealtimeData.h"

namespace {

class TestPolyFit final : public PolyFit<double>
{
public:
    double Fit(double value) const override { return value; }
    double Slope(double) const override { return 1.0; }
    double Integrate(double from, double to) const override { return to - from; }
    void append(std::string &value) const override { value.append("0"); }
};

TestPolyFit testPolyFit;

} // namespace

PolyFit<double> *PolyFitGenerator::GetPolyFit(
    const std::vector<double> &, const double &)
{
    return &testPolyFit;
}

PolyFit<double> *PolyFitGenerator::GetRationalPolyFit(
    const std::vector<double> &, const std::vector<double> &, const double &)
{
    return &testPolyFit;
}

PolyFit<double> *PolyFitGenerator::GetFractionalPolyFit(
    const std::vector<double> &, const double &)
{
    return &testPolyFit;
}

DeviceConfiguration::DeviceConfiguration() :
    type(0),
    wheelSize(2100),
    inertialMomentKGM2(0.0),
    stridelength(0),
    postProcess(0),
    controller(nullptr)
{
}

void RealtimeData::setWatts(double)
{
}

double RealtimeData::getSpeed() const
{
    return 0.0;
}

double RealtimeData::getWheelRpm() const
{
    return 0.0;
}

std::chrono::high_resolution_clock::time_point
RealtimeData::getWheelRpmSampleTime() const
{
    return {};
}
