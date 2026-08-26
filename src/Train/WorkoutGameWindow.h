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
#include "WorkoutGameAudio.h"
#include "WorkoutGameCourseRuntime.h"
#include "WorkoutGamePositionRate.h"
#include "WorkoutGameRunner.h"
#include "WorkoutGameSessionState.h"

class Context;
class ErgFile;
class WorkoutGame3DWindow;
class WorkoutGameCanvas;
class WorkoutGameOpenGLCanvas;
class WorkoutGameSceneGraphWindow;
class QStackedWidget;
class QTimer;
class QWidget;
class QHideEvent;
class QShowEvent;

class WorkoutGameWindow : public GcChartWindow
{
    Q_OBJECT

public:
    explicit WorkoutGameWindow(Context *context);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    void ergFileSelected(ErgFile *workout);
    void telemetryUpdate(const RealtimeData &telemetry);
    void setNow(long workoutTimeMs);
    void start();
    void pause();
    void unpause();
    void stop();
    void useSceneGraphFallback();
    void useOpenGLFallback();
    void usePainterFallback();
    void configChanged(qint32);

private:
    double currentFtp(ErgFile *workout) const;
    WorkoutGameGhostReplay loadGhost(const WorkoutGameCourse &course) const;
    void storeGhost();
    void updateAtWorkoutPosition(std::int64_t workoutPosition);
    void updateRunnerTelemetry();
    void drainRunnerFrame();
    void displayFrame(const WorkoutGameEngineFrame &frame);
    double anchorRate(
            std::int64_t workoutTimeMs,
            std::int64_t monotonicTimeMs);

    Context *context;
    QStackedWidget *renderStack;
    WorkoutGame3DWindow *threeDWindow;
    QWidget *threeDContainer;
    WorkoutGameSceneGraphWindow *sceneGraphWindow;
    QWidget *sceneGraphContainer;
    WorkoutGameCanvas *painterCanvas;
    WorkoutGameOpenGLCanvas *openGLCanvas;
    WorkoutGameCourse currentCourse;
    WorkoutGameCourseRuntime distanceRuntime;
    WorkoutGameDistancePlaybackSnapshot distanceSnapshot;
    WorkoutGameSessionState sessionState;
    WorkoutGameCompetition competition;
    WorkoutGameGhostRecorder ghostRecorder;
    WorkoutGameAudioFeedback audioFeedback;
    WorkoutGameRunner runner;
    QTimer *frameDrainTimer;
    WorkoutGameEngineFrame lastFrame;
    RealtimeData latestTelemetry;
    bool hasTelemetry = false;
    bool hasFrame = false;
    bool paused = false;
    bool sessionActive = false;
    bool presentationSuspended = false;
    bool featureLabEnabled = false;
    WorkoutGamePositionRate positionRate;
    std::int64_t currentWorkoutTimeMs = 0;
    std::int64_t lastTelemetryMonotonicTimeMs = -1;
    double currentAnchorRate = 1.0;
    double ftpWatts = 0.0;
};

#endif
