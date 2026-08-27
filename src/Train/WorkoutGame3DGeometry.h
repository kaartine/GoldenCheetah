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

#include <QByteArray>
#include <QVector3D>
#include <QtQuick3D/qquick3dgeometry.h>

struct WorkoutGame3DMeshData
{
    QByteArray vertexData;
    QByteArray indexData;
    QVector3D boundsMin;
    QVector3D boundsMax;
    int sampleCount = 0;
    bool ready = false;

    int triangleCount() const
    {
        return indexData.size() / int(3 * sizeof(quint32));
    }
};

class WorkoutGame3DGeometry : public QQuick3DGeometry
{
    Q_OBJECT

public:
    enum class Layer
    {
        Berm,
        Bypass,
        Climb,
        ForestDressing,
        ForestFloor,
        RockGarden,
        RockSlab,
        Roots,
        Skinny,
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
    void setMeshData(const WorkoutGame3DMeshData &data);
    static WorkoutGame3DMeshData buildMeshData(
            Layer layer,
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    bool ready() const { return geometryReady; }
    int sampleCount() const { return generatedSampleCount; }
    int triangleCount() const
    {
        return indexData().size() / int(3 * sizeof(quint32));
    }

private:
    static WorkoutGame3DMeshData buildBypasses(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildBerms(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildClimbs(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildForestDressing(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildRoots(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildRockGardens(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildRockSlabs(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);
    static WorkoutGame3DMeshData buildSkinnies(
            const WorkoutGameRoadCourse &course,
            double startDistanceMeters,
            double endDistanceMeters);

    Layer layer;
    bool geometryReady = false;
    int generatedSampleCount = 0;
};

#endif
