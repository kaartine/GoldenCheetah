/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameGapJumpLaunchWindow_h
#define _GC_WorkoutGameGapJumpLaunchWindow_h

#include <deque>

class WorkoutGameGapJumpLaunchWindow
{
public:
    static constexpr int SpeedWindowDurationMilliseconds = 500;
    static constexpr int MaximumSampleDurationMilliseconds = 2000;

    class Metrics
    {
    public:
        double rollingSpeedAverageMetersPerSecond() const;
        double bestFullWindowSpeedAverageMetersPerSecond() const;
        int rollingSpeedDurationMilliseconds() const;
        bool hasFullSpeedWindow() const;
        int atOrAboveTargetPowerHoldMilliseconds() const;
        bool telemetryValid() const;

    private:
        friend class WorkoutGameGapJumpLaunchWindow;

        Metrics(double rollingSpeedAverageMetersPerSecond,
                double bestFullWindowSpeedAverageMetersPerSecond,
                int rollingSpeedDurationMilliseconds,
                bool hasFullSpeedWindow,
                int atOrAboveTargetPowerHoldMilliseconds,
                bool telemetryValid);

        double rollingSpeedAverageMetersPerSecond_ = 0.0;
        double bestFullWindowSpeedAverageMetersPerSecond_ = 0.0;
        int rollingSpeedDurationMilliseconds_ = 0;
        bool hasFullSpeedWindow_ = false;
        int atOrAboveTargetPowerHoldMilliseconds_ = 0;
        bool telemetryValid_ = false;
    };

    void addSample(double speedMetersPerSecond,
                   double actualWatts,
                   double targetWatts,
                   int durationMilliseconds);
    void reset();
    Metrics metrics() const;

private:
    struct SpeedSlice
    {
        double speedMetersPerSecond = 0.0;
        int durationMilliseconds = 0;
    };

    void addSpeedDuration(double speedMetersPerSecond,
                          int durationMilliseconds);
    void appendSpeedSlice(double speedMetersPerSecond,
                          int durationMilliseconds);
    void recordFullWindowAverage();
    double rollingSpeedAverage() const;
    void clearTelemetryHistory();

    std::deque<SpeedSlice> speedSlices_;
    int rollingSpeedDurationMilliseconds_ = 0;
    double bestFullWindowSpeedAverageMetersPerSecond_ = 0.0;
    bool hasFullSpeedWindow_ = false;
    int atOrAboveTargetPowerHoldMilliseconds_ = 0;
    bool telemetryValid_ = false;
};

#endif
