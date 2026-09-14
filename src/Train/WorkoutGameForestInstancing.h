/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameForestInstancing_h
#define _GC_WorkoutGameForestInstancing_h

#include <QtQuick3D/qquick3dinstancing.h>

#include <QVariantList>
#include <QVector3D>

class WorkoutGameForestInstancing : public QQuick3DInstancing
{
    Q_OBJECT

public:
    explicit WorkoutGameForestInstancing(
            bool treePresentation = false,
            QQuick3DObject *parent = nullptr);

    void setPlacements(const QVariantList &placements);
    void setPresentation(double riderDistanceMeters,
                         const QVector3D &cameraPosition,
                         const QVector3D &cameraTarget);

    int count() const { return currentPlacements.size(); }
    QString stableIdAt(int index) const;
    double opacityAt(int index) const;

protected:
    QByteArray getInstanceBuffer(int *instanceCount) override;

private:
    double edgeOpacity(double relativeDistanceMeters) const;
    double cameraOpacity(double x, double z, double radius,
                         const QVector3D &cameraPosition,
                         const QVector3D &cameraTarget) const;
    void rebuildBuffer(bool forceUpdate = false);

    QVariantList currentPlacements;
    QVector<double> currentOpacities;
    QByteArray currentBuffer;
    double currentRiderDistanceMeters = 0.0;
    QVector3D currentCameraPosition;
    QVector3D currentCameraTarget = QVector3D(0.0f, 0.0f, 1.0f);
    bool presentationReady = false;
    bool treePresentation = false;
};

#endif
