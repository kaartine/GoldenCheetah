/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWorkoutAdapter.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr double MaximumTimeExclusive = 9223372036854775808.0; // 2^63

}

WorkoutGameWorkout WorkoutGameWorkoutAdapter::normalize(
        const std::vector<WorkoutGamePowerPoint> &points)
{
    WorkoutGameWorkout workout;
    if (points.empty()) return workout;

    std::vector<std::int64_t> times;
    times.reserve(points.size());
    std::int64_t priorTime = -1;
    for (const WorkoutGamePowerPoint &point : points) {
        if (!std::isfinite(point.timeMs)
                || !std::isfinite(point.watts)
                || point.timeMs < 0.0
                || point.timeMs >= MaximumTimeExclusive
                || point.watts < 0.0) {
            workout.status = WorkoutGameWorkoutStatus::InvalidPoint;
            return workout;
        }

        const std::int64_t time = std::llround(point.timeMs);
        if (time < priorTime) {
            workout.status = WorkoutGameWorkoutStatus::InvalidPoint;
            return workout;
        }
        times.push_back(time);
        priorTime = time;
    }

    if (times.front() > 0) {
        workout.intervals.push_back({
            0,
            times.front(),
            points.front().watts,
            points.front().watts
        });
    }

    for (std::size_t index = 0; index + 1 < points.size(); ++index) {
        if (times[index + 1] == times[index]) continue;
        workout.intervals.push_back({
            times[index],
            times[index + 1] - times[index],
            points[index].watts,
            points[index + 1].watts
        });
    }

    if (workout.intervals.empty()) return workout;
    workout.status = WorkoutGameWorkoutStatus::Ready;
    return workout;
}
