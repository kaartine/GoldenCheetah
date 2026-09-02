/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameGapJumpLaunchWindow.h"

#include <algorithm>
#include <cassert>
#include <cmath>

WorkoutGameGapJumpLaunchWindow::Metrics::Metrics(
        double rollingSpeedAverageMetersPerSecond,
        double bestFullWindowSpeedAverageMetersPerSecond,
        int rollingSpeedDurationMilliseconds,
        bool hasFullSpeedWindow,
        int atOrAboveTargetPowerHoldMilliseconds,
        bool telemetryValid)
    : rollingSpeedAverageMetersPerSecond_(
          rollingSpeedAverageMetersPerSecond),
      bestFullWindowSpeedAverageMetersPerSecond_(
          bestFullWindowSpeedAverageMetersPerSecond),
      rollingSpeedDurationMilliseconds_(rollingSpeedDurationMilliseconds),
      hasFullSpeedWindow_(hasFullSpeedWindow),
      atOrAboveTargetPowerHoldMilliseconds_(
          atOrAboveTargetPowerHoldMilliseconds),
      telemetryValid_(telemetryValid)
{
}

double WorkoutGameGapJumpLaunchWindow::Metrics::
rollingSpeedAverageMetersPerSecond() const
{
    return rollingSpeedAverageMetersPerSecond_;
}

double WorkoutGameGapJumpLaunchWindow::Metrics::
bestFullWindowSpeedAverageMetersPerSecond() const
{
    return bestFullWindowSpeedAverageMetersPerSecond_;
}

int WorkoutGameGapJumpLaunchWindow::Metrics::
rollingSpeedDurationMilliseconds() const
{
    return rollingSpeedDurationMilliseconds_;
}

bool WorkoutGameGapJumpLaunchWindow::Metrics::hasFullSpeedWindow() const
{
    return hasFullSpeedWindow_;
}

int WorkoutGameGapJumpLaunchWindow::Metrics::
atOrAboveTargetPowerHoldMilliseconds() const
{
    return atOrAboveTargetPowerHoldMilliseconds_;
}

bool WorkoutGameGapJumpLaunchWindow::Metrics::telemetryValid() const
{
    return telemetryValid_;
}

void WorkoutGameGapJumpLaunchWindow::addSample(
        double speedMetersPerSecond,
        double actualWatts,
        double targetWatts,
        int durationMilliseconds)
{
    if (durationMilliseconds <= 0
            || durationMilliseconds > MaximumSampleDurationMilliseconds) {
        return;
    }

    telemetryValid_ = std::isfinite(speedMetersPerSecond)
            && speedMetersPerSecond >= 0.0
            && std::isfinite(actualWatts)
            && actualWatts >= 0.0
            && std::isfinite(targetWatts)
            && targetWatts > 0.0;
    if (!telemetryValid_) {
        clearTelemetryHistory();
        return;
    }

    addSpeedDuration(speedMetersPerSecond, durationMilliseconds);
    if (actualWatts >= targetWatts) {
        atOrAboveTargetPowerHoldMilliseconds_ = std::min(
                SpeedWindowDurationMilliseconds,
                atOrAboveTargetPowerHoldMilliseconds_
                        + durationMilliseconds);
    } else {
        atOrAboveTargetPowerHoldMilliseconds_ = 0;
    }
}

void WorkoutGameGapJumpLaunchWindow::reset()
{
    clearTelemetryHistory();
    telemetryValid_ = false;
}

WorkoutGameGapJumpLaunchWindow::Metrics
WorkoutGameGapJumpLaunchWindow::metrics() const
{
    return Metrics(rollingSpeedAverage(),
                   bestFullWindowSpeedAverageMetersPerSecond_,
                   rollingSpeedDurationMilliseconds_,
                   hasFullSpeedWindow_,
                   atOrAboveTargetPowerHoldMilliseconds_,
                   telemetryValid_);
}

void WorkoutGameGapJumpLaunchWindow::addSpeedDuration(
        double speedMetersPerSecond,
        int durationMilliseconds)
{
    int remainingMilliseconds = durationMilliseconds;
    while (remainingMilliseconds > 0) {
        if (rollingSpeedDurationMilliseconds_
                < SpeedWindowDurationMilliseconds) {
            const int appendedMilliseconds = std::min(
                    remainingMilliseconds,
                    SpeedWindowDurationMilliseconds
                            - rollingSpeedDurationMilliseconds_);
            appendSpeedSlice(speedMetersPerSecond, appendedMilliseconds);
            rollingSpeedDurationMilliseconds_ += appendedMilliseconds;
            remainingMilliseconds -= appendedMilliseconds;
            if (rollingSpeedDurationMilliseconds_
                    == SpeedWindowDurationMilliseconds) {
                recordFullWindowAverage();
            }
            continue;
        }

        assert(!speedSlices_.empty());
        const int replacedMilliseconds = std::min(
                remainingMilliseconds,
                speedSlices_.front().durationMilliseconds);
        speedSlices_.front().durationMilliseconds -= replacedMilliseconds;
        if (speedSlices_.front().durationMilliseconds == 0) {
            speedSlices_.pop_front();
        }
        appendSpeedSlice(speedMetersPerSecond, replacedMilliseconds);
        remainingMilliseconds -= replacedMilliseconds;
        recordFullWindowAverage();
    }

    assert(rollingSpeedDurationMilliseconds_
           <= SpeedWindowDurationMilliseconds);
    assert(speedSlices_.size()
           <= static_cast<std::size_t>(SpeedWindowDurationMilliseconds));
}

void WorkoutGameGapJumpLaunchWindow::appendSpeedSlice(
        double speedMetersPerSecond,
        int durationMilliseconds)
{
    if (!speedSlices_.empty()
            && speedSlices_.back().speedMetersPerSecond
                    == speedMetersPerSecond) {
        speedSlices_.back().durationMilliseconds += durationMilliseconds;
        return;
    }
    speedSlices_.push_back({speedMetersPerSecond, durationMilliseconds});
}

void WorkoutGameGapJumpLaunchWindow::recordFullWindowAverage()
{
    const double average = rollingSpeedAverage();
    if (!hasFullSpeedWindow_
            || average > bestFullWindowSpeedAverageMetersPerSecond_) {
        bestFullWindowSpeedAverageMetersPerSecond_ = average;
    }
    hasFullSpeedWindow_ = true;
}

double WorkoutGameGapJumpLaunchWindow::rollingSpeedAverage() const
{
    if (rollingSpeedDurationMilliseconds_ == 0) return 0.0;

    double average = 0.0;
    int accumulatedMilliseconds = 0;
    for (const SpeedSlice &slice : speedSlices_) {
        const int combinedMilliseconds = accumulatedMilliseconds
                + slice.durationMilliseconds;
        average += (slice.speedMetersPerSecond - average)
                * (static_cast<double>(slice.durationMilliseconds)
                   / combinedMilliseconds);
        accumulatedMilliseconds = combinedMilliseconds;
    }
    return average;
}

void WorkoutGameGapJumpLaunchWindow::clearTelemetryHistory()
{
    speedSlices_.clear();
    rollingSpeedDurationMilliseconds_ = 0;
    bestFullWindowSpeedAverageMetersPerSecond_ = 0.0;
    hasFullSpeedWindow_ = false;
    atOrAboveTargetPowerHoldMilliseconds_ = 0;
}
