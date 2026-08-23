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
        double requestedActualWatts,
        double requestedCadenceRpm)
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
    for (std::size_t sectionIndex = 0;
         sectionIndex < course.sections.size(); ++sectionIndex) {
        const WorkoutGameSection &section = course.sections[sectionIndex];
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
                simulation.activeSection == int(sectionIndex)
                    && simulation.challenge.enabled
                ? simulation.challenge
                : WorkoutGameFeatureChallenge::profile(section);
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
                simulation.challenge.enabled
                ? simulation.challenge
                : WorkoutGameFeatureChallenge::profile(section);
        if (challenge.enabled) {
            const double progress = std::clamp(
                    simulation.sectionProgress, 0.0, 1.0);
            WorkoutGameFeatureChallengeMetrics metrics;
            result.cue.measurementActive =
                    simulation.challengeMeasurementActive;
            if (result.cue.measurementActive) {
                metrics = simulation.challengeMetrics;
            } else {
                metrics.averageActualWatts = result.actualWatts;
                metrics.averageTargetWatts = section.targetWatts;
                metrics.averageEffortRatio = section.targetWatts > 0.0
                        ? result.actualWatts / section.targetWatts : 0.0;
                metrics.averageCadenceRpm = std::max(
                        0.0, std::isfinite(requestedCadenceRpm)
                            ? requestedCadenceRpm : 0.0);
                metrics.averageSpeedKph = std::max(
                        0.0, std::isfinite(simulation.speedKph)
                            ? simulation.speedKph : 0.0);
                metrics.averageAdherence = std::clamp(
                        std::isfinite(simulation.adherence)
                            ? simulation.adherence : 0.0,
                        0.0, 1.0);
            }
            const WorkoutGameFeatureChallengeAssessment assessment =
                    WorkoutGameFeatureChallenge::assess(challenge, metrics);
            const double measuredTarget = metrics.averageTargetWatts > 0.0
                    ? metrics.averageTargetWatts : section.targetWatts;
            result.cue.powerRequired = challenge.minimumEffortRatio > 0.0;
            result.cue.cadenceRequired = challenge.minimumCadenceRpm > 0.0;
            result.cue.speedRequired = challenge.minimumSpeedKph > 0.0
                    || challenge.maximumSpeedKph > 0.0;
            result.cue.requiredWatts = result.cue.powerRequired
                    ? measuredTarget * challenge.minimumEffortRatio : 0.0;
            result.cue.actualWatts = metrics.averageActualWatts;
            result.cue.requiredCadenceRpm = challenge.minimumCadenceRpm;
            result.cue.actualCadenceRpm = metrics.averageCadenceRpm;
            result.cue.requiredSpeedKph = challenge.minimumSpeedKph;
            result.cue.maximumSpeedKph = challenge.maximumSpeedKph;
            result.cue.actualSpeedKph = metrics.averageSpeedKph;
            result.cue.powerReadiness = assessment.effortReadiness;
            result.cue.cadenceReadiness = assessment.cadenceReadiness;
            result.cue.speedReadiness = assessment.speedReadiness;
            result.cue.readiness = assessment.readiness;
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
