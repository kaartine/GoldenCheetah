/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGamePowerProfile.h"

#include "WorkoutGameFeatureChallenge.h"

#include <algorithm>
#include <cmath>

WorkoutGamePowerProfileSnapshot WorkoutGamePowerProfile::build(
        const WorkoutGameCourse &course,
        const WorkoutGameSimulationSnapshot &simulation,
        double requestedActualWatts)
{
    WorkoutGamePowerProfileSnapshot result;
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty() || course.durationMs <= 0
            || !simulation.ready) {
        return result;
    }
    result.actualWatts = std::max(
            0.0, std::isfinite(requestedActualWatts)
                ? requestedActualWatts : 0.0);
    result.cursor = std::clamp(
            double(std::max<std::int64_t>(0, simulation.workoutTimeMs))
                / double(course.durationMs),
            0.0, 1.0);
    result.maximumWatts = std::max(1.0, result.actualWatts);
    result.segments.reserve(course.sections.size());
    for (const WorkoutGameSection &section : course.sections) {
        WorkoutGamePowerProfileSegment segment;
        segment.start = std::clamp(
                double(section.startMs) / double(course.durationMs), 0.0, 1.0);
        segment.end = std::clamp(
                double(section.startMs + section.durationMs)
                    / double(course.durationMs), 0.0, 1.0);
        segment.targetWatts = std::max(
                0.0, std::isfinite(section.targetWatts)
                    ? section.targetWatts : 0.0);
        const WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
        segment.challenge = challenge.enabled;
        if (challenge.enabled) {
            const double span = segment.end - segment.start;
            segment.challengeStart = segment.start
                    + span * challenge.measurementStartProgress;
            segment.challengeEnd = segment.start
                    + span * challenge.decisionProgress;
            segment.requiredWatts = segment.targetWatts
                    * (challenge.minimumEffortRatio > 0.0
                        ? challenge.minimumEffortRatio : 1.0);
        }
        result.maximumWatts = std::max(
                result.maximumWatts,
                std::max(segment.targetWatts, segment.requiredWatts));
        result.segments.push_back(segment);
    }
    result.maximumWatts *= 1.1;

    if (simulation.activeSection >= 0
            && simulation.activeSection < int(course.sections.size())) {
        const WorkoutGameSection &section =
                course.sections[std::size_t(simulation.activeSection)];
        result.targetWatts = std::max(0.0, section.targetWatts);
        const WorkoutGameFeatureChallengeProfile challenge =
                WorkoutGameFeatureChallenge::profile(section);
        if (challenge.enabled) {
            const double progress = std::clamp(
                    simulation.sectionProgress, 0.0, 1.0);
            result.cue.requiredWatts = section.targetWatts
                    * (challenge.minimumEffortRatio > 0.0
                        ? challenge.minimumEffortRatio : 1.0);
            result.cue.readiness = std::clamp(
                    simulation.challengeReadiness, 0.0, 1.0);
            if (progress < challenge.measurementStartProgress) {
                result.cue.state = WorkoutGamePowerCueState::Prepare;
                result.cue.secondsUntilWindow =
                        (challenge.measurementStartProgress - progress)
                        * double(section.durationMs) / 1000.0;
            } else if (progress < challenge.decisionProgress) {
                result.cue.state = WorkoutGamePowerCueState::PushNow;
                result.cue.windowProgress = std::clamp(
                        (progress - challenge.measurementStartProgress)
                            / std::max(1e-9,
                                challenge.decisionProgress
                                - challenge.measurementStartProgress),
                        0.0, 1.0);
            } else if (simulation.featureOutcome
                    == WorkoutGameFeatureOutcome::Bypassed) {
                result.cue.state = WorkoutGamePowerCueState::Bypassed;
            } else {
                result.cue.state = WorkoutGamePowerCueState::Committed;
            }
        }
    }
    result.ready = true;
    return result;
}
