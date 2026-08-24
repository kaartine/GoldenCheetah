/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DWindow_h
#define _GC_WorkoutGame3DWindow_h

#include "WorkoutGame3DViewModel.h"
#include "WorkoutGameVisualSmoother.h"

#include <QElapsedTimer>
#include <QQuickView>

class WorkoutGame3DWindow : public QQuickView
{
    Q_OBJECT

public:
    explicit WorkoutGame3DWindow(
            bool rendererEnabled,
            QWindow *parent = nullptr);
    ~WorkoutGame3DWindow() override;

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
    void setSessionRunning(bool running);
    bool rendererAvailable() const { return status() != QQuickView::Error; }
    QString rendererLabel() const { return QStringLiteral("Qt Quick 3D"); }

signals:
    void rendererFailed();

private slots:
    void presentFrame();
    void handleStatusChanged(QQuickView::Status status);
    void handleSceneGraphError(
            QQuickWindow::SceneGraphError error,
            const QString &message);

private:
    void reportFailure(const QString &message);

    WorkoutGame3DViewModel *viewModel;
    WorkoutGameVisualSmoother visualSmoother;
    WorkoutGameFrameRateCounter frameRateCounter;
    QElapsedTimer monotonicClock;
    double watts = 0.0;
    double targetWatts = 0.0;
    int cadenceRpm = 0;
    int heartRate = 0;
    int virtualGear = 1;
    bool hasFrame = false;
    bool sessionRunning = false;
    bool failureReported = false;
};

#endif
