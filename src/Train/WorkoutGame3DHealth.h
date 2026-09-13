/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DHealth_h
#define _GC_WorkoutGame3DHealth_h

#include <QFlags>
#include <QString>

#include <cstdint>

struct WorkoutGame3DPoint
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

enum class WorkoutGame3DHealthReason : std::uint32_t
{
    None = 0,
    WorldNotReady = 1U << 0,
    NonFinitePose = 1U << 1,
    DegenerateCamera = 1U << 2,
    CameraBelowGround = 1U << 3,
    RiderBelowGround = 1U << 4,
    RiderBehindCamera = 1U << 5,
    RiderOutsideFrustum = 1U << 6,
    NoVisibleGeometry = 1U << 7
};
Q_DECLARE_FLAGS(WorkoutGame3DHealthReasons, WorkoutGame3DHealthReason)
Q_DECLARE_OPERATORS_FOR_FLAGS(WorkoutGame3DHealthReasons)

struct WorkoutGame3DHealthInput
{
    bool worldReady = false;
    WorkoutGame3DPoint camera;
    WorkoutGame3DPoint cameraTarget;
    WorkoutGame3DPoint rider;
    double cameraGroundY = 0.0;
    double riderGroundY = 0.0;
    double verticalFieldOfViewDegrees = 47.0;
    double aspectRatio = 16.0 / 9.0;
    double nearClipMeters = 0.15;
    double farClipMeters = 650.0;
    int visibleTriangles = 0;
};

struct WorkoutGame3DHealthSnapshot
{
    WorkoutGame3DHealthReasons reasons;
    double cameraGroundClearanceMeters = 0.0;
    double riderGroundClearanceMeters = 0.0;
    double cameraTargetDistanceMeters = 0.0;
    double riderDepthMeters = 0.0;
    double riderHorizontalNdc = 0.0;
    double riderVerticalNdc = 0.0;
    double verticalFieldOfViewDegrees = 0.0;
    double aspectRatio = 0.0;
    bool riderInFrustum = false;

    bool healthy() const { return reasons == WorkoutGame3DHealthReason::None; }
    QString reasonCodes() const;
};

class WorkoutGame3DHealth
{
public:
    static WorkoutGame3DHealthSnapshot evaluate(
            const WorkoutGame3DHealthInput &input);
};

class WorkoutGame3DHealthMonitor
{
public:
    static constexpr std::int64_t HealthyLogIntervalMs = 10000;
    static constexpr std::int64_t AnomalyLogIntervalMs = 5000;
    static constexpr std::int64_t MinimumLogIntervalMs = 1000;

    void reset();
    bool shouldLog(
            const WorkoutGame3DHealthSnapshot &snapshot,
            std::int64_t monotonicTimeMs);

private:
    WorkoutGame3DHealthReasons previousReasons;
    std::int64_t lastLogTimeMs = -1;
    bool initialized = false;
};

#endif
