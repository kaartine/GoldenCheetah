/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameWindow_h
#define _GC_WorkoutGameWindow_h

#include "GoldenCheetah.h"
#include "RealtimeData.h"
#include "WorkoutGameSimulation.h"

class Context;
class ErgFile;
class WorkoutGameCanvas;

class WorkoutGameWindow : public GcChartWindow
{
    Q_OBJECT

public:
    explicit WorkoutGameWindow(Context *context);

private slots:
    void ergFileSelected(ErgFile *workout);
    void telemetryUpdate(const RealtimeData &telemetry);
    void setNow(long workoutTimeMs);
    void start();
    void pause();
    void unpause();

private:
    double currentFtp(ErgFile *workout) const;
    void updateSimulation(std::int64_t workoutTimeMs);

    Context *context;
    WorkoutGameCanvas *canvas;
    WorkoutGameSimulation simulation;
    RealtimeData latestTelemetry;
    bool hasTelemetry = false;
    bool paused = false;
    double ftpWatts = 0.0;
};

#endif
