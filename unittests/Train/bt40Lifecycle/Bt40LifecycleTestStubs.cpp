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
#include "Train/VMProWidget.h"
#include "ANT/ANTMessage.h"

#include <cstring>

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

}

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

VMProWidget::VMProWidget(QLowEnergyService *service, QObject *parent) :
    QObject(parent),
    m_vmProService(service),
    m_vmProConfigurator(nullptr),
    m_widget(nullptr),
    m_deviceLog(nullptr),
    m_userPiecePicker(nullptr),
    m_volumePicker(nullptr),
    m_autocalibPicker(nullptr),
    m_idleTimeoutPicker(nullptr),
    m_calibrationProgressLabel(nullptr)
{
}

void VMProWidget::onReconnected(QLowEnergyService *service)
{
    m_vmProService = service;
}

void VMProWidget::addStatusMessage(const QString &) {}
void VMProWidget::onVolumeCorrectionModeChanged(VMProVolumeCorrectionMode) {}
void VMProWidget::onUserPieceSizeChanged(VMProVenturiSize) {}
void VMProWidget::onIdleTimeoutChanged(VMProIdleTimeout) {}
void VMProWidget::onCalibrationProgressChanged(quint8) {}
void VMProWidget::onUserPieceSizePickerChanged(int) {}
void VMProWidget::onIdleTimeoutPickerChanged(int) {}
void VMProWidget::onVolumeCorrectionModePickerChanged(int) {}
void VMProWidget::onSaveClicked() {}

RealtimeData::RealtimeData() {}
void RealtimeData::setWatts(double) {}
void RealtimeData::setHr(double) {}
void RealtimeData::setSpeed(double) {}
void RealtimeData::setWheelRpm(double, bool) {}
void RealtimeData::setCadence(double) {}
void RealtimeData::setRf(double) {}
void RealtimeData::setRMV(double) {}
void RealtimeData::setVO2_VCO2(double, double) {}
void RealtimeData::setTv(double) {}
void RealtimeData::setFeO2(double) {}
double RealtimeData::getSpeed() const { return 0.0; }
double RealtimeData::getWheelRpm() const { return 0.0; }
double RealtimeData::getRf() const { return 0.0; }
double RealtimeData::getRMV() const { return 0.0; }
double RealtimeData::getVO2() const { return 0.0; }
double RealtimeData::getVCO2() const { return 0.0; }
double RealtimeData::getTv() const { return 0.0; }
double RealtimeData::getFeO2() const { return 0.0; }

std::chrono::high_resolution_clock::time_point
RealtimeData::getWheelRpmSampleTime() const
{
    return {};
}

ANTMessage::ANTMessage()
{
    length = 0;
    std::memset(data, 0, sizeof(data));
}

ANTMessage
ANTMessage::fecSetTargetPower(const uint8_t, const uint16_t)
{
    return ANTMessage();
}

ANTMessage
ANTMessage::fecSetTrackResistance(const uint8_t, const double, const double)
{
    return ANTMessage();
}
