/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DViewModel_h
#define _GC_WorkoutGame3DViewModel_h

#include "WorkoutGame3DGeometry.h"
#include "WorkoutGameEngine.h"
#include "WorkoutGameFeatureHud.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <array>
#include <memory>
#include <limits>

class WorkoutGame3DViewModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *trailGeometry READ trailGeometry CONSTANT)
    Q_PROPERTY(QObject *floorGeometry READ floorGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY sceneChanged)
    Q_PROPERTY(double riderX READ riderX NOTIFY sceneChanged)
    Q_PROPERTY(double riderY READ riderY NOTIFY sceneChanged)
    Q_PROPERTY(double riderZ READ riderZ NOTIFY sceneChanged)
    Q_PROPERTY(double groundY READ groundY NOTIFY sceneChanged)
    Q_PROPERTY(double riderYaw READ riderYaw NOTIFY sceneChanged)
    Q_PROPERTY(double riderPitch READ riderPitch NOTIFY sceneChanged)
    Q_PROPERTY(double riderRoll READ riderRoll NOTIFY sceneChanged)
    Q_PROPERTY(double pedalAngle READ pedalAngle NOTIFY sceneChanged)
    Q_PROPERTY(double speedKph READ speedKph NOTIFY sceneChanged)
    Q_PROPERTY(double distanceMeters READ distanceMeters NOTIFY sceneChanged)
    Q_PROPERTY(int workoutTimeSeconds READ workoutTimeSeconds
               NOTIFY sceneChanged)
    Q_PROPERTY(double workoutProgress READ workoutProgress NOTIFY sceneChanged)
    Q_PROPERTY(double gradePercent READ gradePercent NOTIFY sceneChanged)
    Q_PROPERTY(double watts READ watts NOTIFY telemetryChanged)
    Q_PROPERTY(double targetWatts READ targetWatts NOTIFY telemetryChanged)
    Q_PROPERTY(int cadenceRpm READ cadenceRpm NOTIFY telemetryChanged)
    Q_PROPERTY(int heartRate READ heartRate NOTIFY telemetryChanged)
    Q_PROPERTY(int virtualGear READ virtualGear NOTIFY telemetryChanged)
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)
    Q_PROPERTY(QString terrainName READ terrainName NOTIFY sceneChanged)
    Q_PROPERTY(QString featureStatus READ featureStatus NOTIFY sceneChanged)
    Q_PROPERTY(int readinessPercent READ readinessPercent NOTIFY sceneChanged)
    Q_PROPERTY(bool featureHudVisible READ featureHudVisible NOTIFY sceneChanged)
    Q_PROPERTY(QString featureName READ featureName NOTIFY sceneChanged)
    Q_PROPERTY(QString featureActionText READ featureActionText NOTIFY sceneChanged)
    Q_PROPERTY(int featureState READ featureState NOTIFY sceneChanged)
    Q_PROPERTY(int featureDistanceKind READ featureDistanceKind NOTIFY sceneChanged)
    Q_PROPERTY(double featureDistanceMeters READ featureDistanceMeters
               NOTIFY sceneChanged)
    Q_PROPERTY(bool powerRequired READ powerRequired NOTIFY sceneChanged)
    Q_PROPERTY(double requiredPowerWatts READ requiredPowerWatts
               NOTIFY sceneChanged)
    Q_PROPERTY(int powerReadinessPercent READ powerReadinessPercent
               NOTIFY sceneChanged)
    Q_PROPERTY(bool cadenceRequired READ cadenceRequired NOTIFY sceneChanged)
    Q_PROPERTY(double requiredCadenceRpm READ requiredCadenceRpm
               NOTIFY sceneChanged)
    Q_PROPERTY(int cadenceReadinessPercent READ cadenceReadinessPercent
               NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList powerProfileSegments READ powerProfileSegments
               NOTIFY courseChanged)
    Q_PROPERTY(double powerProfileMaximumWatts READ powerProfileMaximumWatts
               NOTIFY courseChanged)
    Q_PROPERTY(QVariantList features READ features NOTIFY courseChanged)
    Q_PROPERTY(QVariantList trees READ trees NOTIFY treesChanged)
    Q_PROPERTY(QString cameraComposition READ cameraComposition CONSTANT)
    Q_PROPERTY(double cameraBackMeters READ cameraBackMeters CONSTANT)
    Q_PROPERTY(double cameraSideMeters READ cameraSideMeters CONSTANT)
    Q_PROPERTY(double cameraHeightMeters READ cameraHeightMeters CONSTANT)
    Q_PROPERTY(double cameraLookAheadMeters READ cameraLookAheadMeters CONSTANT)
    Q_PROPERTY(double cameraTargetHeightMeters READ cameraTargetHeightMeters CONSTANT)
    Q_PROPERTY(double cameraX READ cameraX NOTIFY sceneChanged)
    Q_PROPERTY(double cameraY READ cameraY NOTIFY sceneChanged)
    Q_PROPERTY(double cameraZ READ cameraZ NOTIFY sceneChanged)
    Q_PROPERTY(double cameraTargetX READ cameraTargetX NOTIFY sceneChanged)
    Q_PROPERTY(double cameraTargetY READ cameraTargetY NOTIFY sceneChanged)
    Q_PROPERTY(double cameraTargetZ READ cameraTargetZ NOTIFY sceneChanged)

public:
    explicit WorkoutGame3DViewModel(QObject *parent = nullptr);
    ~WorkoutGame3DViewModel() override;

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
    void setFps(double value);

    QObject *trailGeometry() const { return trail.get(); }
    QObject *floorGeometry() const
    {
        return floorBuffers[std::size_t(activeFloorBuffer)].get();
    }
    bool ready() const { return sceneReady; }
    double riderX() const { return riderPositionX; }
    double riderY() const { return riderPositionY; }
    double riderZ() const { return riderPositionZ; }
    double groundY() const { return cameraGroundY; }
    double riderYaw() const { return riderHeadingDegrees; }
    double riderPitch() const { return riderPitchDegrees; }
    double riderRoll() const { return riderRollDegrees; }
    double pedalAngle() const { return currentPedalAngle; }
    double speedKph() const { return currentSpeedKph; }
    double distanceMeters() const { return currentDistanceMeters; }
    int workoutTimeSeconds() const { return currentWorkoutTimeSeconds; }
    double workoutProgress() const { return currentWorkoutProgress; }
    double gradePercent() const { return currentGradePercent; }
    double watts() const { return currentWatts; }
    double targetWatts() const { return currentTargetWatts; }
    int cadenceRpm() const { return currentCadenceRpm; }
    int heartRate() const { return currentHeartRate; }
    int virtualGear() const { return currentVirtualGear; }
    double fps() const { return currentFps; }
    QString terrainName() const { return currentTerrainName; }
    QString featureStatus() const { return currentFeatureStatus; }
    int readinessPercent() const { return currentReadinessPercent; }
    bool featureHudVisible() const { return currentFeatureHud.visible; }
    QString featureName() const { return currentFeatureName; }
    QString featureActionText() const { return currentFeatureActionText; }
    int featureState() const { return int(currentFeatureHud.state); }
    int featureDistanceKind() const
    {
        return int(currentFeatureHud.distanceKind);
    }
    double featureDistanceMeters() const
    {
        return currentFeatureHud.distanceMeters;
    }
    bool powerRequired() const { return currentFeatureHud.powerRequired; }
    double requiredPowerWatts() const
    {
        return currentFeatureHud.requiredPowerWatts;
    }
    int powerReadinessPercent() const
    {
        return currentFeatureHud.powerReadinessPercent;
    }
    bool cadenceRequired() const { return currentFeatureHud.cadenceRequired; }
    double requiredCadenceRpm() const
    {
        return currentFeatureHud.requiredCadenceRpm;
    }
    int cadenceReadinessPercent() const
    {
        return currentFeatureHud.cadenceReadinessPercent;
    }
    QVariantList powerProfileSegments() const { return currentPowerProfile; }
    double powerProfileMaximumWatts() const
    {
        return currentPowerProfileMaximumWatts;
    }
    QVariantList features() const { return courseFeatures; }
    QVariantList trees() const { return visibleTrees; }
    QString cameraComposition() const { return currentCameraComposition; }
    double cameraBackMeters() const { return cameraBackDistanceMeters; }
    double cameraSideMeters() const { return cameraSideDistanceMeters; }
    double cameraHeightMeters() const { return cameraHeightDistanceMeters; }
    double cameraLookAheadMeters() const { return cameraLookAheadDistanceMeters; }
    double cameraTargetHeightMeters() const { return cameraTargetHeightDistanceMeters; }
    double cameraX() const { return cameraPositionX; }
    double cameraY() const { return cameraPositionY; }
    double cameraZ() const { return cameraPositionZ; }
    double cameraTargetX() const { return cameraTargetPositionX; }
    double cameraTargetY() const { return cameraTargetPositionY; }
    double cameraTargetZ() const { return cameraTargetPositionZ; }

signals:
    void sceneChanged();
    void telemetryChanged();
    void fpsChanged();
    void courseChanged();
    void treesChanged();
    void floorGeometryChanged();

private:
    static QString terrainText(WorkoutGameTerrainKind terrain);
    static QString featureText(
            const WorkoutGameFeatureRuntimeSnapshot &feature);
    static QString featureActionText(
            const WorkoutGameFeatureHudSnapshot &hud);
    void rebuildFeatures(double distanceMeters);
    void rebuildFloor(double distanceMeters);
    void rebuildTrees(double distanceMeters);
    void rebuildPowerProfile(const WorkoutGameCourse &course);
    void updateCameraPose(double distanceMeters);

    std::unique_ptr<WorkoutGame3DGeometry> trail;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> floorBuffers;
    int activeFloorBuffer = 0;
    WorkoutGameRoadCourse roadCourse;
    std::int64_t courseDurationMs = 0;
    QVariantList currentPowerProfile;
    double currentPowerProfileMaximumWatts = 1.0;
    QVariantList courseFeatures;
    QVariantList visibleTrees;
    bool sceneReady = false;
    int floorBucket = std::numeric_limits<int>::min();
    int featureBucket = std::numeric_limits<int>::min();
    int treeBucket = std::numeric_limits<int>::min();
    double riderPositionX = 0.0;
    double riderPositionY = 0.0;
    double riderPositionZ = 0.0;
    double cameraGroundY = 0.0;
    double riderHeadingDegrees = 0.0;
    double riderPitchDegrees = 0.0;
    double riderRollDegrees = 0.0;
    double currentPedalAngle = 0.0;
    double currentSpeedKph = 0.0;
    double currentDistanceMeters = 0.0;
    int currentWorkoutTimeSeconds = 0;
    double currentWorkoutProgress = 0.0;
    double currentGradePercent = 0.0;
    double currentWatts = 0.0;
    double currentTargetWatts = 0.0;
    int currentCadenceRpm = 0;
    int currentHeartRate = 0;
    int currentVirtualGear = 1;
    double currentFps = 0.0;
    QString currentTerrainName;
    QString currentFeatureStatus;
    int currentReadinessPercent = 0;
    WorkoutGameFeatureHudSnapshot currentFeatureHud;
    QString currentFeatureName;
    QString currentFeatureActionText;
    QString currentCameraComposition;
    double cameraBackDistanceMeters = 8.2;
    double cameraSideDistanceMeters = 0.0;
    double cameraHeightDistanceMeters = 3.2;
    double cameraLookAheadDistanceMeters = 12.0;
    double cameraTargetHeightDistanceMeters = 0.85;
    double cameraPositionX = 0.0;
    double cameraPositionY = 3.2;
    double cameraPositionZ = -8.2;
    double cameraTargetPositionX = 0.0;
    double cameraTargetPositionY = 0.85;
    double cameraTargetPositionZ = 12.0;
};

#endif
