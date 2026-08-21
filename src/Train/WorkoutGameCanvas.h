/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCanvas_h
#define _GC_WorkoutGameCanvas_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameCompetition.h"
#include "WorkoutGameSimulation.h"
#include "WorkoutGameVisualSmoother.h"

#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QWidget>

class QPainter;

class WorkoutGameCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit WorkoutGameCanvas(QWidget *parent = nullptr);

    void setCourse(const WorkoutGameCourse &course);
    void setCompetition(const WorkoutGameCompetitionSnapshot &competition);
    void setWorld(
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameCameraSnapshot &camera);
    void setFrame(
            const WorkoutGameVisualSnapshot &frame,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);
    void setTelemetry(
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);
    void setSnapshot(
            const WorkoutGameSimulationSnapshot &snapshot,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);
    static QString elapsedTimeText(std::int64_t workoutTimeMs);
    static int targetFrameIntervalMs(double refreshRateHz);
    static QImage addRiderContrastKeyline(const QImage &sprite);
    static void paintScene(
            QPainter &painter,
            const QRect &viewport,
            const WorkoutGameCourse &course,
            const WorkoutGameSimulationSnapshot &snapshot,
            const WorkoutGameCompetitionSnapshot &competition,
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameCameraSnapshot &camera,
            const WorkoutGameTerrainTransitionSnapshot &terrainTransition,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear,
            int animationFrame,
            double framesPerSecond,
            const QString &rendererLabel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    static double trailY(
            double x,
            const QRect &scene,
            const WorkoutGameCourse &course,
            const WorkoutGameSimulationSnapshot &snapshot);
    static double physicsTrailY(
            double x,
            const QRect &scene,
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameCameraSnapshot &camera);
    static double terrainProfileY(
            double x,
            const QRect &scene,
            const WorkoutGameWorldSnapshot &world,
            const WorkoutGameCameraSnapshot &camera,
            const WorkoutGameTerrainProfile &profile);
    static QString featureName(WorkoutGameFeature feature);
    static QString terrainName(WorkoutGameTerrainKind terrain);
    static QString challengeCueName(WorkoutGameChallengeCue cue);

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
    QElapsedTimer visualClock;
    WorkoutGameVisualSmoother visualSmoother;
    WorkoutGameFrameRateCounter frameRateCounter;
    QTimer animationTimer;
};

#endif
