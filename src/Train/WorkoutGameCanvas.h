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
    void setSnapshot(
            const WorkoutGameSimulationSnapshot &snapshot,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);
    static QImage addRiderContrastKeyline(const QImage &sprite);
    static void paintScene(
            QPainter &painter,
            const QRect &viewport,
            const WorkoutGameCourse &course,
            const WorkoutGameSimulationSnapshot &snapshot,
            const WorkoutGameCompetitionSnapshot &competition,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear,
            int animationFrame);

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
    static QString featureName(WorkoutGameFeature feature);

    WorkoutGameCourse course;
    WorkoutGameSimulationSnapshot current;
    WorkoutGameCompetitionSnapshot competition;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    int animationFrame = 0;
    QTimer animationTimer;
};

#endif
