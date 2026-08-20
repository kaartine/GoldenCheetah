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
#include "WorkoutGameCompetition.h"
#include "WorkoutGameCourseRuntime.h"
#include "WorkoutGameSimulation.h"
#include "WorkoutGameSessionState.h"
#include "WorkoutGameWorld.h"

class Context;
class ErgFile;
class WorkoutGameCanvas;
class WorkoutGameOpenGLCanvas;
class WorkoutGameSceneGraphWindow;
class QStackedWidget;
class QWidget;

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
    void stop();
    void useOpenGLFallback();
    void usePainterFallback();

private:
    double currentFtp(ErgFile *workout) const;
    WorkoutGameGhostReplay loadGhost(const WorkoutGameCourse &course) const;
    void storeGhost();
    void updateAtWorkoutPosition(std::int64_t workoutPosition);
    void updateSimulation(std::int64_t workoutTimeMs);

    Context *context;
    QStackedWidget *renderStack;
    WorkoutGameSceneGraphWindow *sceneGraphWindow;
    QWidget *sceneGraphContainer;
    WorkoutGameCanvas *painterCanvas;
    WorkoutGameOpenGLCanvas *openGLCanvas;
    WorkoutGameCourse currentCourse;
    WorkoutGameCourseRuntime distanceRuntime;
    WorkoutGameDistancePlaybackSnapshot distanceSnapshot;
    WorkoutGameSimulation simulation;
    WorkoutGameSessionState sessionState;
    WorkoutGamePhysics physics;
    WorkoutGameCamera camera;
    WorkoutGameCompetition competition;
    WorkoutGameGhostRecorder ghostRecorder;
    RealtimeData latestTelemetry;
    bool hasTelemetry = false;
    bool paused = false;
    bool sessionActive = false;
    bool worldClockInitialized = false;
    std::int64_t lastWorldTimeMs = 0;
    double ftpWatts = 0.0;
};

#endif
