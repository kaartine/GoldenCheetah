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
#include "WorkoutGameSimulation.h"

#include <QTimer>
#include <QWidget>

class WorkoutGameCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit WorkoutGameCanvas(QWidget *parent = nullptr);

    void setCourse(const WorkoutGameCourse &course);
    void setSnapshot(
            const WorkoutGameSimulationSnapshot &snapshot,
            double watts,
            double targetWatts,
            int cadenceRpm,
            int heartRate,
            int virtualGear);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    double trailY(double x, const QRect &scene) const;
    QString featureName(WorkoutGameFeature feature) const;

    WorkoutGameCourse course;
    WorkoutGameSimulationSnapshot current;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    int animationFrame = 0;
    QTimer animationTimer;
};

#endif
