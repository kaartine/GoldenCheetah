/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureChallenge_h
#define _GC_WorkoutGameFeatureChallenge_h

#include "WorkoutGameCourse.h"

#include <cstdint>

enum class WorkoutGameChallengeCue
{
    None,
    CarrySpeed,
    Jump,
    HoldLine,
    Climb
};

struct WorkoutGameFeatureChallengeProfile
{
    bool enabled = false;
    WorkoutGameChallengeCue cue = WorkoutGameChallengeCue::None;
    double decisionProgress = 1.0;
    double minimumEffortRatio = 0.0;
    double minimumCadenceRpm = 0.0;
    double minimumSpeedKph = 0.0;
    double maximumSpeedKph = 0.0;
    double minimumAdherence = 0.0;
    std::uint64_t bonusPoints = 0;
};

struct WorkoutGameFeatureChallengeMetrics
{
    double averageEffortRatio = 0.0;
    double averageCadenceRpm = 0.0;
    double averageSpeedKph = 0.0;
    double averageAdherence = 0.0;
};

struct WorkoutGameFeatureChallengeAssessment
{
    double readiness = 0.0;
    bool completed = false;
};

class WorkoutGameFeatureChallenge
{
public:
    static WorkoutGameFeatureChallengeProfile profile(
            const WorkoutGameSection &section);
    static WorkoutGameFeatureChallengeAssessment assess(
            const WorkoutGameFeatureChallengeProfile &profile,
            const WorkoutGameFeatureChallengeMetrics &metrics);
};

#endif
