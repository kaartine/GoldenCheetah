/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCoursePreviewMetrics.h"

#include <cmath>
#include <cstdint>

std::vector<WorkoutGameCoursePreviewPoint>
WorkoutGameCoursePreviewMetrics::workoutPowerProfile(
        const std::vector<WorkoutGameInterval> &intervals)
{
    std::vector<WorkoutGameCoursePreviewPoint> result;
    if (intervals.empty()) return result;
    const std::int64_t totalDurationMs = intervals.back().startMs
            + intervals.back().durationMs;
    if (totalDurationMs <= 0) return result;

    result.reserve(intervals.size() * 2u);
    for (const WorkoutGameInterval &interval : intervals) {
        if (interval.startMs < 0 || interval.durationMs <= 0
                || !std::isfinite(interval.startWatts)
                || !std::isfinite(interval.endWatts)) {
            return {};
        }
        result.push_back({double(interval.startMs) / double(totalDurationMs),
                          interval.startWatts});
        result.push_back({double(interval.startMs + interval.durationMs)
                            / double(totalDurationMs),
                          interval.endWatts});
    }
    return result;
}

WorkoutGameCoursePreviewRoadMetrics
WorkoutGameCoursePreviewMetrics::roadMetrics(const WorkoutGameRoadPlan &plan)
{
    WorkoutGameCoursePreviewRoadMetrics result;
    result.roadPieceCount = int(plan.pieces.size());
    if (plan.pieces.empty()) return result;

    constexpr double CurveThresholdRadians =
            12.0 * 3.14159265358979323846 / 180.0;
    std::size_t sourceSection = plan.pieces.front().sourceSectionIndex;
    double accumulatedTurn = 0.0;
    const auto finishEvent = [&]() {
        if (accumulatedTurn >= CurveThresholdRadians) {
            ++result.curveEventCount;
        }
    };
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        if (piece.sourceSectionIndex != sourceSection) {
            finishEvent();
            sourceSection = piece.sourceSectionIndex;
            accumulatedTurn = 0.0;
        }
        accumulatedTurn += std::abs(piece.turnRadians);
    }
    finishEvent();
    return result;
}
