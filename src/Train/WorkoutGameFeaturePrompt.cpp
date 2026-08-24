/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameFeaturePrompt.h"

#include <algorithm>
#include <cmath>

WorkoutGameFeaturePromptSnapshot WorkoutGameFeaturePrompt::build(
        const WorkoutGamePowerProfileSnapshot &profile,
        const WorkoutGameFeatureRuntimeSnapshot &feature)
{
    WorkoutGameFeaturePromptSnapshot result;
    if (!profile.ready || !feature.ready
            || feature.phase == WorkoutGameFeaturePhase::None) {
        return result;
    }

    result.featureActive = true;
    result.terrain = feature.terrain;
    result.distanceMeters = std::max(
            0.0, std::isfinite(feature.distanceToObstacleMeters)
                ? feature.distanceToObstacleMeters : 0.0);
    result.requiredWatts = std::max(
            0.0, std::isfinite(profile.cue.requiredWatts)
                ? profile.cue.requiredWatts : 0.0);
    result.actualWatts = std::max(
            0.0, std::isfinite(profile.cue.actualWatts)
                ? profile.cue.actualWatts : 0.0);

    if (profile.cue.state == WorkoutGamePowerCueState::Bypassed
            || feature.route == WorkoutGameRoute::SafeBypass) {
        result.instruction = WorkoutGameFeatureInstruction::SafeLine;
        return result;
    }
    if (feature.phase == WorkoutGameFeaturePhase::Action
            || feature.phase == WorkoutGameFeaturePhase::Recovery) {
        switch (feature.motion) {
        case WorkoutGameFeatureMotion::Jump:
            result.instruction = WorkoutGameFeatureInstruction::Takeoff;
            break;
        case WorkoutGameFeatureMotion::Absorb:
            result.instruction = WorkoutGameFeatureInstruction::Absorb;
            break;
        case WorkoutGameFeatureMotion::Drop:
            result.instruction = WorkoutGameFeatureInstruction::Drop;
            break;
        case WorkoutGameFeatureMotion::None:
            result.instruction =
                    WorkoutGameFeatureInstruction::FeatureCommitted;
            break;
        }
        return result;
    }
    if (profile.cue.state == WorkoutGamePowerCueState::Committed) {
        result.instruction = WorkoutGameFeatureInstruction::FeatureCommitted;
        return result;
    }
    if (profile.cue.state == WorkoutGamePowerCueState::Prepare) {
        result.instruction = WorkoutGameFeatureInstruction::GetReady;
        return result;
    }
    if (profile.cue.state != WorkoutGamePowerCueState::PushNow) {
        return result;
    }
    if (profile.cue.readiness >= 1.0 - 1e-9) {
        result.instruction = WorkoutGameFeatureInstruction::Ready;
        return result;
    }
    if (profile.cue.powerRequired) {
        result.instruction = WorkoutGameFeatureInstruction::ReachTargetPower;
        return result;
    }

    switch (profile.cue.challengeCue) {
    case WorkoutGameChallengeCue::Jump:
        result.instruction = WorkoutGameFeatureInstruction::PedalHard;
        break;
    case WorkoutGameChallengeCue::CarrySpeed:
        result.instruction = WorkoutGameFeatureInstruction::CarrySpeed;
        break;
    case WorkoutGameChallengeCue::HoldLine:
        result.instruction = WorkoutGameFeatureInstruction::HoldLine;
        break;
    case WorkoutGameChallengeCue::Climb:
        result.instruction = WorkoutGameFeatureInstruction::KeepClimbing;
        break;
    case WorkoutGameChallengeCue::None:
        result.instruction = WorkoutGameFeatureInstruction::GetReady;
        break;
    }
    return result;
}
