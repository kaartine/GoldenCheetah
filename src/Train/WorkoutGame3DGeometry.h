/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DGeometry_h
#define _GC_WorkoutGame3DGeometry_h

#include "WorkoutGameRoadCourse.h"

#include <QtQuick3D/qquick3dgeometry.h>

class WorkoutGame3DGeometry : public QQuick3DGeometry
{
    Q_OBJECT

public:
    enum class Layer
    {
        Berm,
        Bypass,
        ForestFloor,
        RockGarden,
        RockSlab,
        Roots,
        Trail
    };

    explicit WorkoutGame3DGeometry(
            Layer layer,
            QQuick3DObject *parent = nullptr);

    void setCourse(const WorkoutGameRoadCourse &course);
    void setCourseRange(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    bool ready() const { return geometryReady; }
    int sampleCount() const { return generatedSampleCount; }

private:
    void build(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    void buildBypasses(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    void buildBerms(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    void buildRoots(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    void buildRockGardens(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    void buildRockSlabs(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);

    Layer layer;
    bool geometryReady = false;
    int generatedSampleCount = 0;
};

#endif
