/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameSimulation.h"

#include "WorkoutGameRoadCourse.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace {

constexpr std::int64_t SimulationStepMs = 50;
constexpr std::int64_t MaximumCatchupMs = 1000;
constexpr std::int64_t JumpMeasurementWindowMs = 1500;

double finiteNonNegative(double value)
{
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

}

bool WorkoutGameSimulation::configure(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.sections.empty()
            || course.durationMs <= 0
            || !std::isfinite(ftpWatts)
            || ftpWatts <= 0.0) {
        configuredCourse = WorkoutGameCourse();
        configuredFtpWatts = 0.0;
        outcomes.clear();
        challengeProfiles.clear();
        reset();
        return false;
    }

    configuredCourse = course;
    configuredFtpWatts = ftpWatts;
    outcomes.assign(
            configuredCourse.sections.size(),
            WorkoutGameFeatureOutcome::None);
    challengeProfiles.assign(
            configuredCourse.sections.size(),
            WorkoutGameFeatureChallengeProfile());
    const WorkoutGameRoadCourse road =
            WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    if (road.ready) {
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (piece.challenge.enabled
                    && piece.sourceSectionIndex < challengeProfiles.size()) {
                challengeProfiles[piece.sourceSectionIndex] =
                        piece.challenge.profile;
            }
        }
    }
    for (std::size_t index = 0; index < challengeProfiles.size(); ++index) {
        if (!challengeProfiles[index].enabled) {
            challengeProfiles[index] = WorkoutGameFeatureChallenge::profile(
                    configuredCourse.sections[index]);
        }
    }
    reset();
    return true;
}

void WorkoutGameSimulation::reset()
{
    initialized = false;
    lastWorkoutTimeMs = 0;
    activeSection = -1;
    sectionActualWattsMs = 0.0;
    sectionTargetWattsMs = 0.0;
    sectionEffortRatioMs = 0.0;
    sectionCadenceMs = 0.0;
    sectionSpeedKphMs = 0.0;
    sectionAdherenceMs = 0.0;
    sectionSampleMs = 0;
    challengeSamples.clear();
    activeChallenge = WorkoutGameFeatureChallengeProfile();
    activeChallengeReadiness = 0.0;
    currentSpeedKph = 0.0;
    currentAdherence = 0.0;
    scorePoints = 0.0;
    streakMs = 0;
    if (!outcomes.empty()) {
        std::fill(
                outcomes.begin(), outcomes.end(),
                WorkoutGameFeatureOutcome::None);
    }
}

int WorkoutGameSimulation::sectionAt(std::int64_t workoutTimeMs) const
{
    if (workoutTimeMs < 0
            || workoutTimeMs >= configuredCourse.durationMs) {
        return -1;
    }

    auto section = std::upper_bound(
            configuredCourse.sections.begin(),
            configuredCourse.sections.end(),
            workoutTimeMs,
            [](std::int64_t time, const WorkoutGameSection &candidate) {
                return time < candidate.startMs;
            });
    if (section == configuredCourse.sections.begin()) return 0;
    return int(std::distance(configuredCourse.sections.begin(), section) - 1);
}

void WorkoutGameSimulation::finalizeActiveSection()
{
    if (activeSection < 0
            || activeSection >= int(configuredCourse.sections.size())
            || outcomes[activeSection] != WorkoutGameFeatureOutcome::Active) {
        return;
    }

    const WorkoutGameFeatureChallengeMetrics metrics = challengeMetrics();
    const WorkoutGameFeatureChallengeAssessment assessment =
            WorkoutGameFeatureChallenge::assess(activeChallenge, metrics);
    activeChallengeReadiness = assessment.readiness;
    outcomes[activeSection] = assessment.completed
            ? WorkoutGameFeatureOutcome::Completed
            : WorkoutGameFeatureOutcome::Bypassed;
    if (assessment.completed) scorePoints += activeChallenge.bonusPoints;
}

void WorkoutGameSimulation::moveToSection(int sectionIndex)
{
    if (sectionIndex == activeSection) return;

    const int priorSection = activeSection;
    finalizeActiveSection();
    if (sectionIndex > priorSection + 1) {
        const int firstSkipped = std::max(0, priorSection + 1);
        for (int index = firstSkipped; index < sectionIndex; ++index) {
            if (challengeProfiles[std::size_t(index)].enabled
                    && outcomes[index] == WorkoutGameFeatureOutcome::None) {
                outcomes[index] = WorkoutGameFeatureOutcome::Bypassed;
            }
        }
    }

    activeSection = sectionIndex;
    sectionActualWattsMs = 0.0;
    sectionTargetWattsMs = 0.0;
    sectionEffortRatioMs = 0.0;
    sectionCadenceMs = 0.0;
    sectionSpeedKphMs = 0.0;
    sectionAdherenceMs = 0.0;
    sectionSampleMs = 0;
    challengeSamples.clear();
    activeChallenge = WorkoutGameFeatureChallengeProfile();
    activeChallengeReadiness = 0.0;
    if (activeSection >= 0
            && outcomes[activeSection] == WorkoutGameFeatureOutcome::None) {
        activeChallenge = challengeProfiles[std::size_t(activeSection)];
    }
    if (activeChallenge.enabled) {
        outcomes[activeSection] = WorkoutGameFeatureOutcome::Active;
    }
}

void WorkoutGameSimulation::appendChallengeSample(
        double actualWatts,
        double targetWatts,
        double effortRatio,
        double cadenceRpm,
        double speedKph,
        double adherence,
        std::int64_t durationMs)
{
    if (durationMs <= 0) return;
    sectionActualWattsMs += actualWatts * double(durationMs);
    sectionTargetWattsMs += targetWatts * double(durationMs);
    sectionEffortRatioMs += effortRatio * double(durationMs);
    sectionCadenceMs += cadenceRpm * double(durationMs);
    sectionSpeedKphMs += speedKph * double(durationMs);
    sectionAdherenceMs += adherence * double(durationMs);
    sectionSampleMs += durationMs;

    if (activeChallenge.cue != WorkoutGameChallengeCue::Jump) return;
    challengeSamples.push_back({
        durationMs, actualWatts, targetWatts,
        effortRatio, cadenceRpm, speedKph, adherence
    });
    std::int64_t excess = sectionSampleMs - JumpMeasurementWindowMs;
    while (excess > 0 && !challengeSamples.empty()) {
        ChallengeSample &oldest = challengeSamples.front();
        const std::int64_t removed = std::min(excess, oldest.durationMs);
        sectionActualWattsMs -= oldest.actualWatts * double(removed);
        sectionTargetWattsMs -= oldest.targetWatts * double(removed);
        sectionEffortRatioMs -= oldest.effortRatio * double(removed);
        sectionCadenceMs -= oldest.cadenceRpm * double(removed);
        sectionSpeedKphMs -= oldest.speedKph * double(removed);
        sectionAdherenceMs -= oldest.adherence * double(removed);
        sectionSampleMs -= removed;
        oldest.durationMs -= removed;
        excess -= removed;
        if (oldest.durationMs == 0) challengeSamples.pop_front();
    }
}

WorkoutGameFeatureChallengeMetrics
WorkoutGameSimulation::challengeMetrics() const
{
    WorkoutGameFeatureChallengeMetrics metrics;
    if (sectionSampleMs <= 0) return metrics;
    const double duration = double(sectionSampleMs);
    metrics.averageActualWatts = std::max(
            0.0, sectionActualWattsMs / duration);
    metrics.averageTargetWatts = std::max(
            0.0, sectionTargetWattsMs / duration);
    metrics.averageEffortRatio = std::max(0.0, sectionEffortRatioMs / duration);
    metrics.averageCadenceRpm = std::max(0.0, sectionCadenceMs / duration);
    metrics.averageSpeedKph = std::max(0.0, sectionSpeedKphMs / duration);
    metrics.averageAdherence = std::clamp(
            sectionAdherenceMs / duration, 0.0, 1.0);
    return metrics;
}

void WorkoutGameSimulation::updateSpeed(
        const WorkoutGameSimulationInput &input,
        const WorkoutGameSection &section,
        double actualWatts,
        std::int64_t stepDurationMs,
        bool immediate)
{
    if (std::isfinite(input.authoritativeSpeedKph)
            && input.authoritativeSpeedKph >= 0.0) {
        currentSpeedKph = std::clamp(input.authoritativeSpeedKph, 0.0, 108.0);
        if (!section.gravityAssisted
                && std::isfinite(input.drivetrainSpeedLimitKph)
                && input.drivetrainSpeedLimitKph >= 0.0) {
            currentSpeedKph = std::min(
                    currentSpeedKph, input.drivetrainSpeedLimitKph);
        }
        return;
    }

    const double desiredSpeed = section.gravityAssisted
            ? 32.0 + 3.0 * std::min(actualWatts / configuredFtpWatts, 2.0)
            : 8.0 + 18.0 * std::sqrt(
                    std::min(actualWatts / configuredFtpWatts, 2.5));
    if (immediate) {
        currentSpeedKph = desiredSpeed;
        return;
    }
    const double speedBlend = std::min(
            1.0, double(stepDurationMs) / 500.0);
    currentSpeedKph += (desiredSpeed - currentSpeedKph) * speedBlend;
}

void WorkoutGameSimulation::simulateStep(
        const WorkoutGameSimulationInput &input,
        std::int64_t stepTimeMs,
        std::int64_t stepDurationMs)
{
    moveToSection(sectionAt(stepTimeMs));
    if (activeSection < 0) return;

    const WorkoutGameSection &section = configuredCourse.sections[activeSection];
    const double actualWatts = finiteNonNegative(input.actualWatts);
    double targetWatts = finiteNonNegative(input.targetWatts);
    if (targetWatts <= 0.0) targetWatts = section.targetWatts;
    targetWatts = std::max(1.0, targetWatts);
    const double cadenceRpm = finiteNonNegative(input.cadenceRpm);
    const double effortRatio = actualWatts / targetWatts;

    if (section.gravityAssisted) {
        const double excess = std::max(0.0, actualWatts - targetWatts * 1.15);
        currentAdherence = 1.0 - std::min(
                1.0, excess / std::max(targetWatts, configuredFtpWatts * 0.2));
    } else {
        currentAdherence = 1.0 - std::min(
                1.0,
                std::abs(actualWatts - targetWatts)
                        / std::max(targetWatts, configuredFtpWatts * 0.2));
    }

    updateSpeed(input, section, actualWatts, stepDurationMs, false);

    if (currentAdherence >= 0.8) {
        streakMs += stepDurationMs;
    } else if (currentAdherence < 0.6) {
        streakMs = 0;
    }
    const double cadenceQuality = cadenceRpm <= 0.0
            ? 0.8
            : std::clamp(1.0 - std::abs(cadenceRpm - 85.0) / 100.0, 0.7, 1.0);
    const double streakMultiplier = 1.0
            + 0.5 * std::min(1.0, double(streakMs) / 30000.0);
    scorePoints += 100.0 * currentAdherence * cadenceQuality
            * streakMultiplier * double(stepDurationMs) / 1000.0;

    if (outcomes[activeSection] == WorkoutGameFeatureOutcome::Active) {
        const double progress = double(stepTimeMs - section.startMs)
                / double(section.durationMs);
        if (progress >= activeChallenge.measurementStartProgress) {
            appendChallengeSample(
                    actualWatts, targetWatts,
                    effortRatio, cadenceRpm, currentSpeedKph,
                    currentAdherence, stepDurationMs);
            const WorkoutGameFeatureChallengeMetrics metrics =
                    challengeMetrics();
            activeChallengeReadiness = WorkoutGameFeatureChallenge::assess(
                    activeChallenge, metrics).readiness;
        }
        if (progress >= activeChallenge.decisionProgress) {
            finalizeActiveSection();
        }
    }
}

WorkoutGameSimulationSnapshot WorkoutGameSimulation::snapshot(
        std::int64_t workoutTimeMs,
        std::int64_t droppedCatchupMs) const
{
    WorkoutGameSimulationSnapshot result;
    result.ready = configuredCourse.status == WorkoutGameCourseStatus::Ready;
    if (!result.ready) return result;

    result.workoutTimeMs = workoutTimeMs;
    result.droppedCatchupMs = droppedCatchupMs;
    result.finished = workoutTimeMs >= configuredCourse.durationMs;
    result.activeSection = activeSection;
    result.courseProgress = std::clamp(
            double(workoutTimeMs) / double(configuredCourse.durationMs),
            0.0, 1.0);
    result.speedKph = currentSpeedKph;
    result.adherence = currentAdherence;
    result.score = std::uint64_t(std::max(0.0, std::floor(scorePoints)));
    result.streakSeconds = double(streakMs) / 1000.0;

    if (activeSection >= 0) {
        const WorkoutGameSection &section = configuredCourse.sections[activeSection];
        result.sectionProgress = std::clamp(
                double(workoutTimeMs - section.startMs)
                        / double(section.durationMs),
                0.0, 1.0);
        result.featureOutcome = outcomes[activeSection];
        result.challenge = activeChallenge;
        result.challengeMetrics = challengeMetrics();
        result.challengeAssessment = WorkoutGameFeatureChallenge::assess(
                activeChallenge, result.challengeMetrics);
        result.challengeMeasurementActive = sectionSampleMs > 0;
        result.challengeReadiness = activeChallengeReadiness;
        if (result.featureOutcome == WorkoutGameFeatureOutcome::Bypassed) {
            result.route = WorkoutGameRoute::SafeBypass;
        }
    }
    return result;
}

WorkoutGameSimulationSnapshot WorkoutGameSimulation::update(
        const WorkoutGameSimulationInput &input)
{
    if (configuredCourse.status != WorkoutGameCourseStatus::Ready) {
        return WorkoutGameSimulationSnapshot();
    }

    const std::int64_t workoutTimeMs = std::clamp(
            input.workoutTimeMs,
            std::int64_t(0),
            configuredCourse.durationMs);
    if (initialized && workoutTimeMs < lastWorkoutTimeMs) reset();

    if (!initialized) {
        initialized = true;
        lastWorkoutTimeMs = workoutTimeMs;
        moveToSection(sectionAt(workoutTimeMs));
        if (activeSection >= 0) {
            const WorkoutGameSection &section = configuredCourse.sections[activeSection];
            const double actualWatts = finiteNonNegative(input.actualWatts);
            updateSpeed(input, section, actualWatts, 0, true);
        }
        return snapshot(workoutTimeMs, 0);
    }

    const std::int64_t elapsedMs = workoutTimeMs - lastWorkoutTimeMs;
    if (input.paused || elapsedMs <= 0) {
        lastWorkoutTimeMs = workoutTimeMs;
        moveToSection(sectionAt(workoutTimeMs));
        if (!input.paused && activeSection >= 0) {
            updateSpeed(
                    input,
                    configuredCourse.sections[activeSection],
                    finiteNonNegative(input.actualWatts),
                    0,
                    true);
        }
        return snapshot(workoutTimeMs, 0);
    }

    const std::int64_t simulatedMs = std::min(elapsedMs, MaximumCatchupMs);
    const std::int64_t droppedCatchupMs = elapsedMs - simulatedMs;
    std::int64_t stepStartMs = workoutTimeMs - simulatedMs;
    moveToSection(sectionAt(stepStartMs));
    while (stepStartMs < workoutTimeMs) {
        const std::int64_t stepDurationMs = std::min(
                SimulationStepMs, workoutTimeMs - stepStartMs);
        simulateStep(
                input,
                stepStartMs + stepDurationMs / 2,
                stepDurationMs);
        stepStartMs += stepDurationMs;
    }

    lastWorkoutTimeMs = workoutTimeMs;
    moveToSection(sectionAt(workoutTimeMs));
    return snapshot(workoutTimeMs, droppedCatchupMs);
}
