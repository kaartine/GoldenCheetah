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

#include "WorkoutGame3DChunkBuilder.h"
#include "WorkoutGame3DCameraPresentation.h"
#include "WorkoutGame3DGeometry.h"
#include "WorkoutGameDiagnostics.h"
#include "WorkoutGameEngine.h"
#include "WorkoutGameFeatureHud.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

struct WorkoutGame3DFrameWorkCounters
{
    std::uint64_t frameCalls = 0;
    std::uint64_t sceneSignals = 0;
    std::uint64_t telemetrySignals = 0;
    std::uint64_t courseSignals = 0;
    std::uint64_t treeSignals = 0;
    std::uint64_t forestSignals = 0;
    std::uint64_t floorSignals = 0;
    std::uint64_t floorBuildRequests = 0;
    std::uint64_t floorChunkBuildsCompleted = 0;
    std::uint64_t floorChunkInstalls = 0;
    std::uint64_t featureModelRegenerations = 0;
    std::uint64_t treeModelRegenerations = 0;
    std::uint64_t forestModelRegenerations = 0;
    std::uint64_t treeClearanceEntriesVisited = 0;
};

class WorkoutGame3DViewModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *trailGeometry READ trailGeometry CONSTANT)
    Q_PROPERTY(QObject *bermGeometry READ bermGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *bypassGeometry READ bypassGeometry CONSTANT)
    Q_PROPERTY(QObject *gapJumpGeometry READ gapJumpGeometry CONSTANT)
    Q_PROPERTY(QObject *climbGeometry READ climbGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *rootsGeometry READ rootsGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *rockGardenGeometry READ rockGardenGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *rockSlabGeometry READ rockSlabGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *skinnyGeometry READ skinnyGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *floorGeometry READ floorGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(QObject *forestDressingGeometry READ forestDressingGeometry
               NOTIFY floorGeometryChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY sceneChanged)
    Q_PROPERTY(double riderX READ riderX NOTIFY sceneChanged)
    Q_PROPERTY(double riderY READ riderY NOTIFY sceneChanged)
    Q_PROPERTY(double riderZ READ riderZ NOTIFY sceneChanged)
    Q_PROPERTY(double groundY READ groundY NOTIFY sceneChanged)
    Q_PROPERTY(double riderYaw READ riderYaw NOTIFY sceneChanged)
    Q_PROPERTY(double riderPitch READ riderPitch NOTIFY sceneChanged)
    Q_PROPERTY(double riderRoll READ riderRoll NOTIFY sceneChanged)
    Q_PROPERTY(double riderPump READ riderPump NOTIFY sceneChanged)
    Q_PROPERTY(double riderAirHeight READ riderAirHeight NOTIFY sceneChanged)
    Q_PROPERTY(double landingImpact READ landingImpact NOTIFY sceneChanged)
    Q_PROPERTY(qulonglong landingEffectId READ landingEffectId
               NOTIFY sceneChanged)
    Q_PROPERTY(double landingEffectStrength READ landingEffectStrength
               NOTIFY sceneChanged)
    Q_PROPERTY(qulonglong successEffectId READ successEffectId
               NOTIFY sceneChanged)
    Q_PROPERTY(QString successEffectText READ successEffectText
               NOTIFY sceneChanged)
    Q_PROPERTY(double riderStandingBlend READ riderStandingBlend
               NOTIFY sceneChanged)
    Q_PROPERTY(double riderPedalEffort READ riderPedalEffort
               NOTIFY sceneChanged)
    Q_PROPERTY(double rearSuspensionCompression READ rearSuspensionCompression
               NOTIFY sceneChanged)
    Q_PROPERTY(double frontSuspensionCompression READ frontSuspensionCompression
               NOTIFY sceneChanged)
    Q_PROPERTY(bool riderWalking READ riderWalking NOTIFY sceneChanged)
    Q_PROPERTY(QString riderPoseState READ riderPoseState NOTIFY sceneChanged)
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
    Q_PROPERTY(bool diagnosticsEnabled READ diagnosticsEnabled CONSTANT)
    Q_PROPERTY(QString diagnosticsText READ diagnosticsText
               NOTIFY diagnosticsChanged)
    Q_PROPERTY(int visibleTriangles READ visibleTriangles
               NOTIFY renderWorkChanged)
    Q_PROPERTY(int geometryQueueDepth READ geometryQueueDepth
               NOTIFY renderWorkChanged)
    Q_PROPERTY(QString generatorState READ generatorState
               NOTIFY generatorStateChanged)
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
    Q_PROPERTY(QVariantList gapJumpFeatures READ gapJumpFeatures
               NOTIFY courseChanged)
    Q_PROPERTY(QVariantList trees READ trees NOTIFY treesChanged)
    Q_PROPERTY(QVariantList forestFloorProps READ forestFloorProps
               NOTIFY forestDressingChanged)
    Q_PROPERTY(QVariantList forestVergeClusters READ forestVergeClusters
               NOTIFY forestDressingChanged)
    Q_PROPERTY(QString cameraComposition READ cameraComposition CONSTANT)
    Q_PROPERTY(QString cameraPresentation READ cameraPresentation
               NOTIFY sceneChanged)
    Q_PROPERTY(double cameraPresentationBlend READ cameraPresentationBlend
               NOTIFY sceneChanged)
    Q_PROPERTY(bool extendedRenderStatsEnabled
               READ extendedRenderStatsEnabled CONSTANT)
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
    void setDiagnostics(const WorkoutGameDiagnosticsSnapshot &snapshot);
    void setGeneratorState(const QString &state);
    WorkoutGame3DFrameWorkCounters frameWorkCounters() const;
    void resetFrameWorkCounters();

    QObject *trailGeometry() const { return trail.get(); }
    QObject *bermGeometry() const
    {
        return bermBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *bypassGeometry() const { return bypass.get(); }
    QObject *gapJumpGeometry() const { return gapJump.get(); }
    QObject *climbGeometry() const
    {
        return climbBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *rootsGeometry() const
    {
        return rootBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *rockGardenGeometry() const
    {
        return rockGardenBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *rockSlabGeometry() const
    {
        return rockSlabBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *skinnyGeometry() const
    {
        return skinnyBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *floorGeometry() const
    {
        return floorBuffers[std::size_t(activeFloorBuffer)].get();
    }
    QObject *forestDressingGeometry() const
    {
        return forestDressingBuffers[std::size_t(activeFloorBuffer)].get();
    }
    bool ready() const { return sceneReady; }
    double riderX() const { return riderPositionX; }
    double riderY() const { return riderPositionY; }
    double riderZ() const { return riderPositionZ; }
    double groundY() const { return cameraGroundY; }
    double riderYaw() const { return riderHeadingDegrees; }
    double riderPitch() const { return riderPitchDegrees; }
    double riderRoll() const { return riderRollDegrees; }
    double riderPump() const { return riderPumpMeters; }
    double riderAirHeight() const { return currentRiderAirHeightMeters; }
    double landingImpact() const { return currentLandingImpact; }
    qulonglong landingEffectId() const
    {
        return qulonglong(currentLandingEffectId);
    }
    double landingEffectStrength() const
    {
        return currentLandingEffectStrength;
    }
    qulonglong successEffectId() const
    {
        return qulonglong(currentSuccessEffectId);
    }
    QString successEffectText() const { return currentSuccessEffectText; }
    double riderStandingBlend() const { return currentRiderStandingBlend; }
    double riderPedalEffort() const { return currentRiderPedalEffort; }
    double rearSuspensionCompression() const
    {
        return currentRearSuspensionCompression;
    }
    double frontSuspensionCompression() const
    {
        return currentFrontSuspensionCompression;
    }
    bool riderWalking() const { return currentRiderWalking; }
    QString riderPoseState() const { return currentRiderPoseState; }
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
    bool diagnosticsEnabled() const
    {
        return qEnvironmentVariableIntValue(
                "GC_WORKOUT_GAME_DIAGNOSTICS") != 0;
    }
    QString diagnosticsText() const { return currentDiagnosticsText; }
    int visibleTriangles() const { return currentVisibleTriangles; }
    int geometryQueueDepth() const;
    QString generatorState() const { return currentGeneratorState; }
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
    QVariantList gapJumpFeatures() const { return courseGapJumpFeatures; }
    QVariantList trees() const { return visibleTrees; }
    QVariantList forestFloorProps() const { return visibleForestFloorProps; }
    QVariantList forestVergeClusters() const
    {
        return visibleForestVergeClusters;
    }
    QString cameraComposition() const { return currentCameraComposition; }
    QString cameraPresentation() const;
    double cameraPresentationBlend() const
    {
        return cameraPresentationSnapshot.sideBlend;
    }
    bool extendedRenderStatsEnabled() const
    {
        return qEnvironmentVariableIntValue(
                "GC_WORKOUT_GAME_RENDER_STATS") != 0;
    }
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
    void diagnosticsChanged();
    void renderWorkChanged();
    void generatorStateChanged();
    void courseChanged();
    void treesChanged();
    void forestDressingChanged();
    void floorGeometryChanged();

private:
    static QString terrainText(WorkoutGameTerrainKind terrain);
    static QString gapJumpFeatureText(
            WorkoutGameGapJumpLine line,
            double predictedSpeedMetersPerSecond,
            bool locked);
    static QString featureText(
            const WorkoutGameFeatureRuntimeSnapshot &feature);
    static QString featureActionText(
            const WorkoutGameFeatureHudSnapshot &hud);
    void rebuildFeatures(double distanceMeters);
    void rebuildFloor(double distanceMeters, bool immediate = false);
    void installReadyFloorChunk();
    void scheduleReadyFloorChunk();
    void updateVisibleTriangleCount();
    void rebuildTrees(double distanceMeters);
    void rebuildForestDressing(double distanceMeters);
    void rebuildPowerProfile(const WorkoutGameCourse &course);
    void updateCameraPose(double distanceMeters, double lateralMeters,
                          std::int64_t workoutTimeMs);

    std::unique_ptr<WorkoutGame3DGeometry> trail;
    std::unique_ptr<WorkoutGame3DGeometry> bypass;
    std::unique_ptr<WorkoutGame3DGeometry> gapJump;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> floorBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> bermBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2>
            forestDressingBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> climbBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> rootBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> rockGardenBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> rockSlabBuffers;
    std::array<std::unique_ptr<WorkoutGame3DGeometry>, 2> skinnyBuffers;
    int activeFloorBuffer = 0;
    WorkoutGameRoadCourse roadCourse;
    std::shared_ptr<const WorkoutGameRoadCourse> roadCourseSnapshot;
    std::vector<std::size_t> rollerChallengePieceIndices;
    std::vector<std::size_t> climbChallengePieceIndices;
    std::int64_t courseDurationMs = 0;
    QVariantList currentPowerProfile;
    double currentPowerProfileMaximumWatts = 1.0;
    QVariantList courseFeatures;
    QVariantList courseGapJumpFeatures;
    QVariantList visibleTrees;
    QVariantList visibleForestFloorProps;
    QVariantList visibleForestVergeClusters;
    int forestDressingFirstSlot = std::numeric_limits<int>::min();
    int forestDressingLastSlot = std::numeric_limits<int>::min();
    bool sceneReady = false;
    int floorBucket = std::numeric_limits<int>::min();
    int requestedFloorBucket = std::numeric_limits<int>::min();
    int nextFloorBucket = 0;
    WorkoutGame3DStreamingCoverage requestedFloorCoverage;
    WorkoutGame3DStreamingCoverage featureCoverage;
    WorkoutGame3DStreamingCoverage treeCoverage;
    double riderPositionX = 0.0;
    double riderPositionY = 0.0;
    double riderPositionZ = 0.0;
    double cameraGroundY = 0.0;
    double riderHeadingDegrees = 0.0;
    double riderPitchDegrees = 0.0;
    double riderRollDegrees = 0.0;
    bool riderPoseInitialized = false;
    bool riderBankRollActive = false;
    double riderPumpMeters = 0.0;
    double currentRiderAirHeightMeters = 0.0;
    double currentLandingImpact = 0.0;
    double previousLandingImpact = 0.0;
    double currentLandingEffectStrength = 0.0;
    std::uint64_t currentLandingEffectId = 0;
    std::uint64_t currentSuccessEffectId = 0;
    std::uint64_t lastSuccessActionId = 0;
    QString currentSuccessEffectText;
    double currentRiderStandingBlend = 0.0;
    double currentRiderPedalEffort = 0.0;
    double currentRearSuspensionCompression = 0.0;
    double currentFrontSuspensionCompression = 0.0;
    bool currentRiderWalking = false;
    QString currentRiderPoseState = QStringLiteral("pedal");
    bool rootCompressionInitialized = false;
    double previousRootCompression = 0.0;
    std::int64_t lastRiderPoseTimeMs = -1;
    bool rockCompressionInitialized = false;
    double previousRockCompression = 0.0;
    bool slabCompressionInitialized = false;
    double previousSlabCompression = 0.0;
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
    QString currentDiagnosticsText;
    int currentVisibleTriangles = 0;
    QString currentGeneratorState;
    QString currentTerrainName;
    QString currentFeatureStatus;
    int currentReadinessPercent = 0;
    WorkoutGameFeatureHudSnapshot currentFeatureHud;
    QString currentFeatureName;
    QString currentFeatureActionText;
    QString currentCameraComposition = QStringLiteral("medium-centre");
    WorkoutGame3DCameraPresentation cameraPresentationController;
    WorkoutGame3DCameraPresentationSnapshot cameraPresentationSnapshot;
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
    bool cameraPoseInitialized = false;
    double cameraYawRadians = 0.0;
    double cameraYawVelocityRadiansPerSecond = 0.0;
    double cameraPitchRadians = 0.0;
    double cameraPresentationPoseBlend = 1.0;
    std::int64_t lastCameraPoseTimeMs = 0;
    std::uint64_t courseGeneration = 0;
    WorkoutGame3DChunkBuilder chunkBuilder;
    std::atomic<bool> floorInstallQueued{false};
    WorkoutGame3DFrameWorkCounters workCounters;
    std::uint64_t completedFloorBuildCounterBaseline = 0;
};

#endif
