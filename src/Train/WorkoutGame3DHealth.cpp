/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DHealth.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double Pi = 3.14159265358979323846;

bool finitePoint(const WorkoutGame3DPoint &point)
{
    return std::isfinite(point.x)
            && std::isfinite(point.y)
            && std::isfinite(point.z);
}

double dot(const WorkoutGame3DPoint &left, const WorkoutGame3DPoint &right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

WorkoutGame3DPoint subtract(
        const WorkoutGame3DPoint &left,
        const WorkoutGame3DPoint &right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

WorkoutGame3DPoint scale(const WorkoutGame3DPoint &point, double factor)
{
    return {point.x * factor, point.y * factor, point.z * factor};
}

WorkoutGame3DPoint cross(
        const WorkoutGame3DPoint &left,
        const WorkoutGame3DPoint &right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

double length(const WorkoutGame3DPoint &point)
{
    return std::sqrt(dot(point, point));
}

}

QString WorkoutGame3DHealthSnapshot::reasonCodes() const
{
    if (healthy()) return QStringLiteral("ok");
    QStringList codes;
    if (reasons.testFlag(WorkoutGame3DHealthReason::WorldNotReady))
        codes.append(QStringLiteral("world-not-ready"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::NonFinitePose))
        codes.append(QStringLiteral("non-finite-pose"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::DegenerateCamera))
        codes.append(QStringLiteral("degenerate-camera"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::CameraBelowGround))
        codes.append(QStringLiteral("camera-below-ground"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::RiderBelowGround))
        codes.append(QStringLiteral("rider-below-ground"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::RiderBehindCamera))
        codes.append(QStringLiteral("rider-behind-camera"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::RiderOutsideFrustum))
        codes.append(QStringLiteral("rider-outside-frustum"));
    if (reasons.testFlag(WorkoutGame3DHealthReason::NoVisibleGeometry))
        codes.append(QStringLiteral("no-visible-geometry"));
    return codes.join(QLatin1Char(','));
}

WorkoutGame3DHealthSnapshot WorkoutGame3DHealth::evaluate(
        const WorkoutGame3DHealthInput &input)
{
    WorkoutGame3DHealthSnapshot result;
    result.verticalFieldOfViewDegrees = input.verticalFieldOfViewDegrees;
    result.aspectRatio = input.aspectRatio;
    if (!input.worldReady) {
        result.reasons |= WorkoutGame3DHealthReason::WorldNotReady;
    }
    if (input.worldReady && input.visibleTriangles <= 0) {
        result.reasons |= WorkoutGame3DHealthReason::NoVisibleGeometry;
    }

    const bool finite = finitePoint(input.camera)
            && finitePoint(input.cameraTarget)
            && finitePoint(input.rider)
            && std::isfinite(input.cameraGroundY)
            && std::isfinite(input.riderGroundY)
            && std::isfinite(input.verticalFieldOfViewDegrees)
            && std::isfinite(input.aspectRatio)
            && std::isfinite(input.nearClipMeters)
            && std::isfinite(input.farClipMeters);
    if (!finite) {
        result.reasons |= WorkoutGame3DHealthReason::NonFinitePose;
        return result;
    }

    result.cameraGroundClearanceMeters =
            input.camera.y - input.cameraGroundY;
    result.riderGroundClearanceMeters =
            input.rider.y - input.riderGroundY;
    if (result.cameraGroundClearanceMeters < 0.4) {
        result.reasons |= WorkoutGame3DHealthReason::CameraBelowGround;
    }
    if (result.riderGroundClearanceMeters < -0.4) {
        result.reasons |= WorkoutGame3DHealthReason::RiderBelowGround;
    }

    const WorkoutGame3DPoint cameraDirection =
            subtract(input.cameraTarget, input.camera);
    result.cameraTargetDistanceMeters = length(cameraDirection);
    if (result.cameraTargetDistanceMeters < 0.05) {
        result.reasons |= WorkoutGame3DHealthReason::DegenerateCamera;
        return result;
    }

    const WorkoutGame3DPoint forward = scale(
            cameraDirection, 1.0 / result.cameraTargetDistanceMeters);
    const WorkoutGame3DPoint worldUp{0.0, 1.0, 0.0};
    WorkoutGame3DPoint right = cross(forward, worldUp);
    const double rightLength = length(right);
    if (rightLength < 0.01) {
        result.reasons |= WorkoutGame3DHealthReason::DegenerateCamera;
        return result;
    }
    right = scale(right, 1.0 / rightLength);
    const WorkoutGame3DPoint cameraUp = cross(right, forward);
    const WorkoutGame3DPoint riderFromCamera =
            subtract(input.rider, input.camera);
    result.riderDepthMeters = dot(riderFromCamera, forward);
    if (result.riderDepthMeters <= input.nearClipMeters) {
        result.reasons |= WorkoutGame3DHealthReason::RiderBehindCamera;
        return result;
    }

    const double clampedFov = std::clamp(
            input.verticalFieldOfViewDegrees, 10.0, 150.0);
    const double halfHeight = result.riderDepthMeters
            * std::tan(0.5 * clampedFov * Pi / 180.0);
    const double halfWidth = halfHeight * std::max(0.1, input.aspectRatio);
    result.riderHorizontalNdc = dot(riderFromCamera, right) / halfWidth;
    result.riderVerticalNdc = dot(riderFromCamera, cameraUp) / halfHeight;
    result.riderInFrustum = result.riderDepthMeters <= input.farClipMeters
            && std::abs(result.riderHorizontalNdc) <= 1.0
            && std::abs(result.riderVerticalNdc) <= 1.0;
    constexpr double FrustumAnomalyMargin = 1.25;
    if (result.riderDepthMeters > input.farClipMeters
            || std::abs(result.riderHorizontalNdc) > FrustumAnomalyMargin
            || std::abs(result.riderVerticalNdc) > FrustumAnomalyMargin) {
        result.reasons |= WorkoutGame3DHealthReason::RiderOutsideFrustum;
    }
    return result;
}

void WorkoutGame3DHealthMonitor::reset()
{
    previousReasons = {};
    lastLogTimeMs = -1;
    initialized = false;
}

bool WorkoutGame3DHealthMonitor::shouldLog(
        const WorkoutGame3DHealthSnapshot &snapshot,
        std::int64_t monotonicTimeMs)
{
    const bool reasonsChanged = initialized
            && snapshot.reasons != previousReasons;
    const std::int64_t elapsed = lastLogTimeMs < 0
            ? std::numeric_limits<std::int64_t>::max()
            : monotonicTimeMs - lastLogTimeMs;
    const bool minimumIntervalElapsed = lastLogTimeMs < 0
            || monotonicTimeMs < lastLogTimeMs
            || elapsed >= MinimumLogIntervalMs;
    const std::int64_t interval = snapshot.healthy()
            ? HealthyLogIntervalMs : AnomalyLogIntervalMs;
    const bool intervalElapsed = lastLogTimeMs < 0
            || monotonicTimeMs < lastLogTimeMs
            || elapsed >= interval;
    initialized = true;
    if ((!reasonsChanged && !intervalElapsed)
            || !minimumIntervalElapsed) {
        return false;
    }
    previousReasons = snapshot.reasons;
    lastLogTimeMs = monotonicTimeMs;
    return true;
}
