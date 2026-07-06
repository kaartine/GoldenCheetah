/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/VMProWidget.h"

#include <QVariant>

namespace {

qulonglong serviceIdentity(QLowEnergyService *service)
{
    return static_cast<qulonglong>(
        reinterpret_cast<quintptr>(service));
}

} // namespace

VMProWidget::VMProWidget(
    QLowEnergyService *service, QObject *parent)
    : QObject(parent),
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
    setProperty("testService", serviceIdentity(service));
}

void VMProWidget::onReconnected(QLowEnergyService *service)
{
    m_vmProService = service;
    setProperty("testService", serviceIdentity(service));
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
