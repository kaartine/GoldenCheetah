/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameSceneGraphWindow_h
#define _GC_WorkoutGameSceneGraphWindow_h

#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameVisualSmoother.h"

#include <QElapsedTimer>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>

#include <cstdint>

class WorkoutGameSceneGraphItem : public QQuickItem
{
    Q_OBJECT

public:
    explicit WorkoutGameSceneGraphItem(QQuickItem *parent = nullptr);

    void setCourse(const WorkoutGameCourse &course, double ftpWatts);
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

protected:
    QSGNode *updatePaintNode(
            QSGNode *oldNode,
            UpdatePaintNodeData *data) override;

private:
    void rebuildHud();
    void publishFps(double fps);

    WorkoutGameRoadCourse roadCourse;
    WorkoutGameVisualSnapshot currentFrame;
    WorkoutGameVisualSmoother visualSmoother;
    QElapsedTimer visualClock;
    QImage backgroundImage;
    QImage riderImage;
    QImage hudImage;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    double displayedFps = 0.0;
    double publishedFps = -1.0;
    std::uint64_t hudRevision = 0;
    WorkoutGameFrameRateCounter frameRateCounter;
};

class WorkoutGameSceneGraphWindow : public QQuickWindow
{
    Q_OBJECT

public:
    explicit WorkoutGameSceneGraphWindow(QWindow *parent = nullptr);

    void setCourse(const WorkoutGameCourse &course, double ftpWatts);
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

signals:
    void rendererFailed();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    WorkoutGameSceneGraphItem *sceneItem;
    bool failureReported = false;
};

#endif
