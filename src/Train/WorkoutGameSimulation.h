/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameSimulation_h
#define _GC_WorkoutGameSimulation_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameFeatureChallenge.h"

#include <cstdint>
#include <deque>
#include <vector>

enum class WorkoutGameFeatureOutcome
{
    None,
    Active,
    Completed,
    Bypassed
};

enum class WorkoutGameRoute
{
    MainLine,
    SafeBypass
};

struct WorkoutGameSimulationInput
{
    std::int64_t workoutTimeMs = 0;
    double actualWatts = 0.0;
    double targetWatts = 0.0;
    double cadenceRpm = 0.0;
    double authoritativeSpeedKph = -1.0;
    double drivetrainSpeedLimitKph = -1.0;
    int virtualGear = 1;
    bool paused = false;
};

struct WorkoutGameSimulationSnapshot
{
    bool ready = false;
    bool finished = false;
    std::int64_t workoutTimeMs = 0;
    std::int64_t droppedCatchupMs = 0;
    int activeSection = -1;
    double courseProgress = 0.0;
    double sectionProgress = 0.0;
    double speedKph = 0.0;
    double adherence = 0.0;
    std::uint64_t score = 0;
    double streakSeconds = 0.0;
    WorkoutGameFeatureOutcome featureOutcome = WorkoutGameFeatureOutcome::None;
    int previousFeatureSection = -1;
    WorkoutGameFeatureOutcome previousFeatureOutcome =
            WorkoutGameFeatureOutcome::None;
    double previousFeatureReadiness = 0.0;
    WorkoutGameRoute route = WorkoutGameRoute::MainLine;
    WorkoutGameFeatureChallengeProfile challenge;
    WorkoutGameFeatureChallengeMetrics challengeMetrics;
    WorkoutGameFeatureChallengeAssessment challengeAssessment;
    bool challengeMeasurementActive = false;
    double challengeReadiness = 0.0;
};

class WorkoutGameSimulation
{
public:
    bool configure(const WorkoutGameCourse &course, double ftpWatts);
    void reset();
    WorkoutGameSimulationSnapshot update(
            const WorkoutGameSimulationInput &input);
    bool commitGapJumpOutcome(
            int sectionIndex,
            WorkoutGameFeatureOutcome outcome,
            double readiness);
    std::uint64_t currentScore() const;

    const std::vector<WorkoutGameFeatureOutcome> &sectionOutcomes() const
    {
        return outcomes;
    }

private:
    struct ChallengeSample
    {
        std::int64_t durationMs = 0;
        double actualWatts = 0.0;
        double targetWatts = 0.0;
        double effortRatio = 0.0;
        double cadenceRpm = 0.0;
        double speedKph = 0.0;
        double adherence = 0.0;
    };

    int sectionAt(std::int64_t workoutTimeMs) const;
    void moveToSection(int sectionIndex);
    void finalizeActiveSection();
    void updateSpeed(
            const WorkoutGameSimulationInput &input,
            const WorkoutGameSection &section,
            double actualWatts,
            std::int64_t stepDurationMs,
            bool immediate);
    void simulateStep(
            const WorkoutGameSimulationInput &input,
            std::int64_t stepTimeMs,
            std::int64_t stepDurationMs);
    void appendChallengeSample(
            double actualWatts,
            double targetWatts,
            double effortRatio,
            double cadenceRpm,
            double speedKph,
            double adherence,
            std::int64_t durationMs);
    WorkoutGameFeatureChallengeMetrics challengeMetrics() const;
    WorkoutGameSimulationSnapshot snapshot(
            std::int64_t workoutTimeMs,
            std::int64_t droppedCatchupMs) const;

    WorkoutGameCourse configuredCourse;
    double configuredFtpWatts = 0.0;
    std::vector<WorkoutGameFeatureOutcome> outcomes;
    std::vector<WorkoutGameFeatureChallengeProfile> challengeProfiles;
    bool initialized = false;
    std::int64_t lastWorkoutTimeMs = 0;
    int activeSection = -1;
    double sectionActualWattsMs = 0.0;
    double sectionTargetWattsMs = 0.0;
    double sectionEffortRatioMs = 0.0;
    double sectionCadenceMs = 0.0;
    double sectionSpeedKphMs = 0.0;
    double sectionAdherenceMs = 0.0;
    std::int64_t sectionSampleMs = 0;
    std::deque<ChallengeSample> challengeSamples;
    WorkoutGameFeatureChallengeProfile activeChallenge;
    double activeChallengeReadiness = 0.0;
    double currentSpeedKph = 0.0;
    double currentAdherence = 0.0;
    double scorePoints = 0.0;
    std::int64_t streakMs = 0;
};

#endif
