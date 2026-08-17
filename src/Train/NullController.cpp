/*
 * Copyright (c) 2009 Steve Gribble (gribble [at] cs.washington.edu) and
 *                    Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <cmath>

#include "NullController.h"
#include "RealtimeData.h"
#include "PhysicsUtility.h"

NullController::NullController(TrainSidebar *parent,
                               DeviceConfiguration *dc)
  : RealtimeController(parent, dc), parent(parent), beats(0), count(0),
    bicycle(NULL)
{
}

int NullController::start() {
  beats = 0;
  count = 0;
  generator.reset();
  bicycle.clear();
  return 0;
}

int NullController::stop() {
  return 0;
}

int NullController::pause() {
  return 0;
}

int NullController::restart() {
    beats = 0;
    count = 0;
    generator.reset();
    bicycle.clear();
    return 0;
}

bool NullController::find() {
    return true;
}

void NullController::setMode(int ) {
    restart();
}

void NullController::setLoad(double watts) {
    generator.setTargetWatts(watts);
}

void NullController::getRealtimeData(RealtimeData &rtData) {
    const TrainingDataGeneratorSample sample = generator.nextSample();
    rtData.setName((char *)"Data Generator");
    rtData.setWatts(sample.watts);
    rtData.setLoad(generator.targetWatts());
    rtData.setLRBalance(sample.leftRightBalance);

    // Derive speed from the generated power so route and game views receive
    // internally consistent telemetry.
    BicycleSimState newState(rtData);
    SpeedDistance ret = bicycle.SampleSpeed(newState);
    rtData.setSpeed(ret.v);
    rtData.setCadence(sample.cadence);
    rtData.setHr(sample.heartRate);
    rtData.setHb(sample.smO2, sample.totalHb);
    rtData.setCoreTemp(sample.coreTemperature, sample.skinTemperature,
                       sample.heatStrain);

    processRealtimeData(rtData); // for testing virtual power etc

    // Generate an R-R data signal from the same heart-rate sample.
    if (count++%5 == 0) {

        // Emit measurement time in 1/1024s, an incremental beat count and bpm.
        uint16_t m = beats * 1024;
        uint8_t b = ++beats;
        uint8_t bpm = static_cast<uint8_t>(std::lround(sample.heartRate));

        //qDebug()<<"rrdata:"<<m<<b<<bpm;
        emit rrData(m, b, bpm);
    }
}

void NullController::pushRealtimeData(RealtimeData &) {
}
