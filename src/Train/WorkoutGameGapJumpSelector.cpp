/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameGapJumpSelector.h"

#include <algorithm>
#include <cmath>

namespace {

int lineRank(WorkoutGameGapJumpLine line)
{
    return static_cast<int>(line);
}

}

WorkoutGameGapJumpSelector::WorkoutGameGapJumpSelector()
    : WorkoutGameGapJumpSelector(
          WorkoutGameGapJumpGeometry::canonicalProfile())
{
}

WorkoutGameGapJumpSelector::WorkoutGameGapJumpSelector(
        const WorkoutGameGapJumpGeometryProfile &profile)
    : profile_(WorkoutGameGapJumpGeometry::validate(profile)
                   ? profile
                   : WorkoutGameGapJumpGeometryProfile{})
{
}

void WorkoutGameGapJumpSelector::reset()
{
    state_ = {};
    initialized_ = false;
    clearCandidate();
}

WorkoutGameGapJumpSelectionState WorkoutGameGapJumpSelector::update(
        double speed,
        int durationMilliseconds)
{
    if (state_.locked || !profile_.ready) return state_;
    if (!std::isfinite(speed) || speed < 0.0) {
        state_.provisionalLine = WorkoutGameGapJumpLine::None;
        initialized_ = true;
        clearCandidate();
        return state_;
    }
    if (durationMilliseconds <= 0
            || durationMilliseconds > MaximumInputDurationMilliseconds) {
        return state_;
    }

    if (!initialized_) {
        state_.provisionalLine = coldSelection(profile_, speed);
        initialized_ = true;
        return state_;
    }

    const WorkoutGameGapJumpLine next = schmittCandidate(speed);
    if (next == state_.provisionalLine) {
        clearCandidate();
        return state_;
    }
    if (candidate_ != next) {
        candidate_ = next;
        candidateDurationMilliseconds_ = 0;
    }
    candidateDurationMilliseconds_ = std::min(
            candidateDurationMilliseconds_ + durationMilliseconds,
            DemotionDurationMilliseconds);

    const bool promotion = lineRank(next)
            > lineRank(state_.provisionalLine);
    const int requiredDuration = promotion
            ? PromotionDurationMilliseconds
            : DemotionDurationMilliseconds;
    if (candidateDurationMilliseconds_ >= requiredDuration) {
        state_.provisionalLine = next;
        clearCandidate();
    }
    return state_;
}

WorkoutGameGapJumpSelectionState WorkoutGameGapJumpSelector::lock(
        std::uint64_t actionId,
        bool effortReady,
        bool telemetryFresh)
{
    return lock(actionId, state_.provisionalLine,
                effortReady, telemetryFresh);
}

WorkoutGameGapJumpSelectionState WorkoutGameGapJumpSelector::lock(
        std::uint64_t actionId,
        WorkoutGameGapJumpLine requestedLine,
        bool effortReady,
        bool telemetryFresh)
{
    if (state_.locked) return state_;
    state_.locked = true;
    state_.actionId = actionId;
    const bool validLine = requestedLine != WorkoutGameGapJumpLine::None
            && WorkoutGameGapJumpGeometry::line(
                    profile_, requestedLine) != nullptr;
    state_.lockedLine = profile_.ready && validLine && effortReady
                    && telemetryFresh
                ? requestedLine : WorkoutGameGapJumpLine::None;
    state_.provisionalLine = state_.lockedLine;
    clearCandidate();
    return state_;
}

WorkoutGameGapJumpSelectionState WorkoutGameGapJumpSelector::state() const
{
    return state_;
}

WorkoutGameGapJumpLine WorkoutGameGapJumpSelector::coldSelection(double speed)
{
    return coldSelection(
            WorkoutGameGapJumpGeometry::canonicalProfile(), speed);
}

WorkoutGameGapJumpLine WorkoutGameGapJumpSelector::coldSelection(
        const WorkoutGameGapJumpGeometryProfile &profile,
        double speed)
{
    if (!WorkoutGameGapJumpGeometry::validate(profile)
            || !std::isfinite(speed) || speed < 0.0) {
        return WorkoutGameGapJumpLine::None;
    }
    if (speed >= profile.lines[2].coldThresholdMetersPerSecond) {
        return WorkoutGameGapJumpLine::Long;
    }
    if (speed >= profile.lines[1].coldThresholdMetersPerSecond) {
        return WorkoutGameGapJumpLine::Medium;
    }
    if (speed >= profile.lines[0].coldThresholdMetersPerSecond) {
        return WorkoutGameGapJumpLine::Short;
    }
    return WorkoutGameGapJumpLine::None;
}

WorkoutGameGapJumpLine WorkoutGameGapJumpSelector::schmittCandidate(
        double speed) const
{
    const double hysteresis = profile_.hysteresisMetersPerSecond;
    const double shortThreshold =
            profile_.lines[0].coldThresholdMetersPerSecond;
    const double mediumThreshold =
            profile_.lines[1].coldThresholdMetersPerSecond;
    const double longThreshold =
            profile_.lines[2].coldThresholdMetersPerSecond;

    switch (state_.provisionalLine) {
    case WorkoutGameGapJumpLine::None:
        if (speed >= longThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Long;
        }
        if (speed >= mediumThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Medium;
        }
        if (speed >= shortThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Short;
        }
        return WorkoutGameGapJumpLine::None;

    case WorkoutGameGapJumpLine::Short:
        if (speed >= longThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Long;
        }
        if (speed >= mediumThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Medium;
        }
        if (speed < shortThreshold - hysteresis) {
            return WorkoutGameGapJumpLine::None;
        }
        return WorkoutGameGapJumpLine::Short;

    case WorkoutGameGapJumpLine::Medium:
        if (speed >= longThreshold + hysteresis) {
            return WorkoutGameGapJumpLine::Long;
        }
        if (speed < mediumThreshold - hysteresis) {
            return speed < shortThreshold - hysteresis
                    ? WorkoutGameGapJumpLine::None
                    : WorkoutGameGapJumpLine::Short;
        }
        return WorkoutGameGapJumpLine::Medium;

    case WorkoutGameGapJumpLine::Long:
        if (speed < longThreshold - hysteresis) {
            if (speed < shortThreshold - hysteresis) {
                return WorkoutGameGapJumpLine::None;
            }
            if (speed < mediumThreshold - hysteresis) {
                return WorkoutGameGapJumpLine::Short;
            }
            return WorkoutGameGapJumpLine::Medium;
        }
        return WorkoutGameGapJumpLine::Long;
    }
    return WorkoutGameGapJumpLine::None;
}

void WorkoutGameGapJumpSelector::clearCandidate()
{
    candidate_ = WorkoutGameGapJumpLine::None;
    candidateDurationMilliseconds_ = 0;
}
