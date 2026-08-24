/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameForestFloor_h
#define _GC_WorkoutGameForestFloor_h

#include "WorkoutGameRoadProjection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

class WorkoutGameForestFloor
{
public:
    static constexpr double BlendWidthMeters = 3.2;

    static double offsetMeters(
            double worldDistanceMeters,
            double lateralMeters,
            double trailHalfWidthMeters)
    {
        const double awayFromTrail = std::max(
                0.0, std::abs(lateralMeters) - trailHalfWidthMeters);
        const double blend = std::clamp(
                awayFromTrail / BlendWidthMeters, 0.0, 1.0);
        return std::clamp(
                blend * rollingOffsetMeters(
                    worldDistanceMeters, lateralMeters < 0.0),
                -1.05, 1.05);
    }

    static double rollingOffsetMeters(
            double worldDistanceMeters,
            bool left)
    {
        const double side = left ? -1.0 : 1.0;
        const double phase = side < 0.0 ? 0.65 : 2.35;
        const double rolling = 0.72
                * std::sin(worldDistanceMeters * 0.052 + phase)
                + 0.34 * std::sin(
                    worldDistanceMeters * 0.137 - phase * 0.6)
                + side * 0.18
                    * std::sin(worldDistanceMeters * 0.031 + 1.2);
        return rolling;
    }

    static double offsetFromOuterMeters(
            double lateralMeters,
            double trailHalfWidthMeters,
            double leftRollingOffsetMeters,
            double rightRollingOffsetMeters)
    {
        const double awayFromTrail = std::max(
                0.0, std::abs(lateralMeters) - trailHalfWidthMeters);
        const double blend = std::clamp(
                awayFromTrail / BlendWidthMeters, 0.0, 1.0);
        return std::clamp(
                blend * (lateralMeters < 0.0
                    ? leftRollingOffsetMeters : rightRollingOffsetMeters),
                -1.05, 1.05);
    }

    static double outerLateralMeters(
            double trailHalfWidthMeters,
            bool left)
    {
        const double lateral = trailHalfWidthMeters + BlendWidthMeters;
        return left ? -lateral : lateral;
    }
};

class WorkoutGameForestFloorProjection
{
public:
    static WorkoutGameForestFloorProjection build(
            const WorkoutGameRoadProjectionFrame &projection)
    {
        WorkoutGameForestFloorProjection result;
        if (!projection.ready
                || !std::isfinite(projection.verticalExaggeration)) {
            return result;
        }
        result.verticalExaggeration = projection.verticalExaggeration;
        result.slices.reserve(projection.slices.size());
        for (const WorkoutGameRoadProjectedSlice &slice : projection.slices) {
            if (!std::isfinite(slice.worldDistanceMeters)
                    || !std::isfinite(slice.centerX)
                    || !std::isfinite(slice.centerY)
                    || !std::isfinite(slice.halfWidthMeters)
                    || !std::isfinite(slice.pixelsPerMeter)
                    || slice.pixelsPerMeter <= 0.0) {
                return {};
            }
            result.slices.push_back({
                slice.worldDistanceMeters,
                slice.centerX,
                slice.centerY,
                slice.halfWidthMeters,
                slice.pixelsPerMeter,
                WorkoutGameForestFloor::rollingOffsetMeters(
                    slice.worldDistanceMeters, true),
                WorkoutGameForestFloor::rollingOffsetMeters(
                    slice.worldDistanceMeters, false)
            });
        }
        result.ready = !result.slices.empty();
        return result;
    }

    double occlusionY(double worldDistanceMeters, double screenX) const
    {
        double result = std::numeric_limits<double>::infinity();
        if (!ready || !std::isfinite(worldDistanceMeters)
                || !std::isfinite(screenX)) return result;
        for (const Slice &slice : slices) {
            if (slice.worldDistanceMeters > worldDistanceMeters + 1e-9) {
                continue;
            }
            const double lateralMeters =
                    (screenX - slice.centerX) / slice.pixelsPerMeter;
            const double groundOffset =
                    WorkoutGameForestFloor::offsetFromOuterMeters(
                    lateralMeters,
                    slice.halfWidthMeters,
                    slice.leftRollingOffsetMeters,
                    slice.rightRollingOffsetMeters);
            const double groundY = slice.centerY
                    - groundOffset * slice.pixelsPerMeter
                        * verticalExaggeration;
            result = std::min(result, groundY);
        }
        return result;
    }

    bool isReady() const { return ready; }

private:
    struct Slice
    {
        double worldDistanceMeters = 0.0;
        double centerX = 0.0;
        double centerY = 0.0;
        double halfWidthMeters = 0.0;
        double pixelsPerMeter = 0.0;
        double leftRollingOffsetMeters = 0.0;
        double rightRollingOffsetMeters = 0.0;
    };

    bool ready = false;
    double verticalExaggeration = 1.0;
    std::vector<Slice> slices;
};

#endif
