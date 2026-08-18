/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameOpenGLCanvas_h
#define _GC_WorkoutGameOpenGLCanvas_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameCompetition.h"
#include "WorkoutGameSimulation.h"
#include "WorkoutGameWorld.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QTimer>

class WorkoutGameOpenGLCanvas :
        public QOpenGLWidget,
        protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit WorkoutGameOpenGLCanvas(QWidget *parent = nullptr);

    void setCourse(const WorkoutGameCourse &course);
    void setCompetition(const WorkoutGameCompetitionSnapshot &competition);
    void setWorld(
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameCameraSnapshot &camera);
    void setSnapshot(
            const WorkoutGameSimulationSnapshot &snapshot,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);

signals:
    void rendererFailed();

protected:
    void initializeGL() override;
    void paintGL() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void reportFailure();

    WorkoutGameCourse course;
    WorkoutGameSimulationSnapshot current;
    WorkoutGameCompetitionSnapshot competition;
    WorkoutGameWorldSnapshot world;
    WorkoutGameCameraSnapshot camera;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    int animationFrame = 0;
    bool failureReported = false;
    QTimer animationTimer;
};

#endif
