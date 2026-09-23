/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameVisualSmoother.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

double lerp(double from, double to, double amount)
{
    return from + (to - from) * amount;
}

double lerpAngle(double from, double to, double amount, double period)
{
    double difference = std::fmod(to - from, period);
    if (difference > period * 0.5) difference -= period;
    if (difference < -period * 0.5) difference += period;
    return from + difference * amount;
}

bool sameCompetitors(
        const WorkoutGameCompetitionSnapshot &from,
        const WorkoutGameCompetitionSnapshot &to)
{
    if (from.ready != to.ready
            || from.competitors.size() != to.competitors.size()) {
        return false;
    }
    for (std::size_t index = 0; index < from.competitors.size(); ++index) {
        const WorkoutGameCompetitorSnapshot &left = from.competitors[index];
        const WorkoutGameCompetitorSnapshot &right = to.competitors[index];
        if (left.kind != right.kind
                || left.identity != right.identity
                || left.lane != right.lane) {
            return false;
        }
    }
    return true;
}

double extrapolate(
        double previous,
        double target,
        std::int64_t predictionMs,
        std::int64_t sourceIntervalMs)
{
    return target + (target - previous)
            * double(predictionMs)
            / double(std::max<std::int64_t>(1, sourceIntervalMs));
}

}

void WorkoutGameVisualSmoother::reset()
{
    initialized = false;
    sourceAdvancing = false;
    fixedStepSnapshots = false;
    transitionStartMs = 0;
    lastTargetMonotonicMs = 0;
    sourceIntervalMs = 1000;
    previousPresentationTimeMs = 0;
    targetPresentationTimeMs = 0;
    previous = WorkoutGameVisualSnapshot();
    predictionOrigin = WorkoutGameVisualSnapshot();
    target = WorkoutGameVisualSnapshot();
    fixedStepHistory.clear();
    courseAnchorHistory.clear();
    coursePresentationStarted = false;
    courseHasMovement = false;
    courseHolding = false;
    courseCursorMs = 0.0;
    courseBufferMs = DistanceCoursePresentationDelayMs;
    courseSpeedMetersPerSecond = 0.0;
    courseVelocityMetersPerSecond = 0.0;
    courseLastSampleMs = 0;
    courseTiming = DistanceTiming();
    terrainTransition.reset();
}

void WorkoutGameVisualSmoother::setTarget(
        const WorkoutGameVisualSnapshot &snapshot,
        std::int64_t monotonicTimeMs)
{
    const bool fixedStepTarget = snapshot.presentationTimeMs > 0;
    const bool visualDiscontinuity = initialized
            && isDiscontinuity(target, snapshot);
    const bool courseDiscontinuity = fixedStepTarget
            && snapshot.distanceAnchoredPresentation
            && !courseAnchorHistory.empty()
            && isCoursePositionDiscontinuity(
                courseAnchorHistory.back(), snapshot);
    if (visualDiscontinuity || courseDiscontinuity) {
        terrainTransition.reset();
    }
    // Buffered distance motion owns its terrain transition at the displayed
    // boundary, not at an assumed arrival+200 ms wall-clock deadline.
    if (!fixedStepTarget || !snapshot.distanceAnchoredPresentation) {
        terrainTransition.setTarget(snapshot.world, monotonicTimeMs);
    }
    if (fixedStepTarget) {
        const std::int64_t presentationTimeMs = snapshot.presentationTimeMs;
        if (!initialized || !fixedStepSnapshots
                || presentationTimeMs <= targetPresentationTimeMs
                || visualDiscontinuity
                || courseDiscontinuity) {
            initialized = true;
            fixedStepSnapshots = true;
            sourceAdvancing = false;
            previous = snapshot;
            target = snapshot;
            previousPresentationTimeMs = presentationTimeMs;
            targetPresentationTimeMs = presentationTimeMs;
            fixedStepHistory.clear();
            fixedStepHistory.push_back(snapshot);
            courseAnchorHistory.clear();
            coursePresentationStarted = false;
            courseHasMovement = false;
            courseHolding = false;
            courseBufferMs = DistanceCoursePresentationDelayMs;
            courseSpeedMetersPerSecond = std::max(
                    snapshot.world.speedMetersPerSecond,
                    snapshot.simulation.speedKph / 3.6);
            courseVelocityMetersPerSecond = 0.0;
            courseTiming = DistanceTiming();
            if (snapshot.distanceAnchoredPresentation) {
                courseAnchorHistory.push_back(snapshot);
            }
            return;
        }
        previous = target;
        target = snapshot;
        previousPresentationTimeMs = targetPresentationTimeMs;
        targetPresentationTimeMs = presentationTimeMs;
        sourceAdvancing = target.simulation.workoutTimeMs
                > previous.simulation.workoutTimeMs;
        fixedStepHistory.push_back(snapshot);
        while (fixedStepHistory.size() > 8) {
            fixedStepHistory.pop_front();
        }
        if (snapshot.distanceAnchoredPresentation
                && (courseAnchorHistory.empty()
                || coursePositionChanged(
                    courseAnchorHistory.back(), snapshot))) {
            if (!courseAnchorHistory.empty()) {
                const auto &last = courseAnchorHistory.back();
                const auto gapMs = presentationTimeMs - last.presentationTimeMs;
                const double distance = snapshot.world.rider.distanceMeters
                        - last.world.rider.distanceMeters;
                courseTiming.maximumAnchorGapMs = std::max(
                        courseTiming.maximumAnchorGapMs, gapMs);
                double motionIntervalMs = double(gapMs);
                const double incomingSpeed = std::max(
                        snapshot.world.speedMetersPerSecond,
                        snapshot.simulation.speedKph / 3.6);
                // Before first movement, the zero-speed origin may include
                // session waiting rather than a measured motion interval.
                // Bootstrap presentation once from the first confirmation;
                // keep the identical origin and all distance/speed bounds.
                // This is not evidence of fresh stationary trainer telemetry.
                if (!courseHasMovement && distance > 0.0
                        && courseSpeedMetersPerSecond == 0.0
                        && gapMs > DistanceCoursePresentationDelayMs
                        && std::isfinite(incomingSpeed) && incomingSpeed > 0.0) {
                    motionIntervalMs = double(DistanceCoursePresentationDelayMs);
                    auto bridge = last;
                    bridge.presentationTimeMs = presentationTimeMs
                            - DistanceCoursePresentationDelayMs;
                    courseAnchorHistory.push_back(bridge);
                }
                // A true stop need not replay seconds of zero-distance history.
                // Unlike the former fixed 200 ms bridge, preserve enough time
                // for all confirmed distance at the observed moving speed.
                else if (gapMs > 1000 && courseSpeedMetersPerSecond > 0.0) {
                    motionIntervalMs = std::min(double(gapMs), std::max(
                            200.0, distance / courseSpeedMetersPerSecond * 1000.0));
                    if (motionIntervalMs < gapMs) {
                        auto bridge = last;
                        bridge.presentationTimeMs = presentationTimeMs
                                - std::int64_t(std::ceil(motionIntervalMs));
                        courseAnchorHistory.push_back(bridge);
                    }
                }
                if (distance > 0.0) courseHasMovement = true;
                if (motionIntervalMs > 0.0 && distance > 0.0) {
                    courseSpeedMetersPerSecond = distance * 1000.0
                            / std::max(20.0, motionIntervalMs);
                    const double measuredSpeed = std::max(
                            snapshot.world.speedMetersPerSecond,
                            snapshot.simulation.speedKph / 3.6);
                    if (measuredSpeed > 0.0) {
                        courseSpeedMetersPerSecond = std::min(
                                courseSpeedMetersPerSecond, measuredSpeed * 1.25);
                    }
                }
                const double observedBufferMs = motionIntervalMs > 200.0
                        ? std::min(1000.0, motionIntervalMs + 40.0) : 200.0;
                // Learn larger gaps immediately; shed excess buffering slowly.
                courseBufferMs = std::max(observedBufferMs,
                        courseBufferMs - double(gapMs) * 0.005);
            }
            courseAnchorHistory.push_back(snapshot);
            pruneCourseHistory();
            if (courseAnchorHistory.size() > MaximumCourseAnchors) {
                // Bounded degradation after prolonged missing presentation:
                // decimate intermediate visual samples, retaining the actual
                // displayed origin and newest confirmation. Some intermediate
                // visual boundaries may be omitted, never authoritative events.
                // Crucially, do not teleport/reset the cursor to the latest.
                auto origin = coursePresentationStarted
                        ? courseAt(courseCursorMs) : courseAnchorHistory.front();
                if (coursePresentationStarted) {
                    origin.presentationTimeMs = std::int64_t(courseCursorMs);
                    courseCursorMs = double(origin.presentationTimeMs);
                }
                std::deque<WorkoutGameVisualSnapshot> compacted;
                compacted.push_back(origin);
                for (std::size_t i = 1; i + 1 < courseAnchorHistory.size(); i += 2) {
                    compacted.push_back(courseAnchorHistory[i]);
                }
                compacted.push_back(courseAnchorHistory.back());
                courseAnchorHistory.swap(compacted);
                ++courseTiming.historyCompactions;
            }
        }
        return;
    }

    if (!initialized
            || monotonicTimeMs < transitionStartMs
            || isDiscontinuity(target, snapshot)) {
        initialized = true;
        fixedStepSnapshots = false;
        sourceAdvancing = false;
        transitionStartMs = monotonicTimeMs;
        lastTargetMonotonicMs = monotonicTimeMs;
        sourceIntervalMs = 1000;
        previous = snapshot;
        predictionOrigin = snapshot;
        target = snapshot;
        return;
    }

    const WorkoutGameVisualSnapshot sampled = sample(monotonicTimeMs);
    const bool advancing = snapshot.simulation.workoutTimeMs
            > target.simulation.workoutTimeMs;
    const std::int64_t workoutIntervalMs =
            snapshot.simulation.workoutTimeMs
            - target.simulation.workoutTimeMs;
    const std::int64_t monotonicIntervalMs =
            monotonicTimeMs - lastTargetMonotonicMs;
    const std::int64_t observedIntervalMs = workoutIntervalMs > 0
            ? workoutIntervalMs
            : monotonicIntervalMs;
    if (observedIntervalMs > 0) {
        sourceIntervalMs = std::clamp(
                observedIntervalMs,
                MinimumSourceIntervalMs,
                MaximumSourceIntervalMs);
    }
    previous = sampled;
    predictionOrigin = target;
    target = snapshot;
    transitionStartMs = monotonicTimeMs;
    lastTargetMonotonicMs = monotonicTimeMs;
    sourceAdvancing = advancing;

    if (sourceAdvancing
            && target.simulation.workoutTimeMs
                < sampled.simulation.workoutTimeMs) {
        target.simulation.workoutTimeMs =
                sampled.simulation.workoutTimeMs;
        target.simulation.courseProgress = std::max(
                target.simulation.courseProgress,
                sampled.simulation.courseProgress);
        if (target.simulation.activeSection
                == sampled.simulation.activeSection) {
            target.simulation.sectionProgress = std::max(
                    target.simulation.sectionProgress,
                    sampled.simulation.sectionProgress);
        }
        if (target.world.ready && sampled.world.ready
                && target.world.generation == sampled.world.generation) {
            target.world.rider.distanceMeters = std::max(
                    target.world.rider.distanceMeters,
                    sampled.world.rider.distanceMeters);
        }
        if (target.camera.ready && sampled.camera.ready) {
            target.camera.centerDistanceMeters = std::max(
                    target.camera.centerDistanceMeters,
                    sampled.camera.centerDistanceMeters);
        }
        predictionOrigin = target;
        transitionStartMs -= TransitionDurationMs;
    }
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::sample(
        std::int64_t monotonicTimeMs)
{
    if (!initialized) return WorkoutGameVisualSnapshot();
    if (fixedStepSnapshots) {
        if (fixedStepHistory.empty()) return target;
        const std::int64_t renderTimeMs = monotonicTimeMs
                - FixedStepPresentationDelayMs;
        std::size_t upperIndex = 0;
        while (upperIndex < fixedStepHistory.size()
                && fixedStepHistory[upperIndex].presentationTimeMs
                    <= renderTimeMs) {
            ++upperIndex;
        }
        WorkoutGameVisualSnapshot result;
        if (upperIndex == 0) {
            result = fixedStepHistory.front();
        } else if (upperIndex >= fixedStepHistory.size()) {
            const WorkoutGameVisualSnapshot &latest =
                    fixedStepHistory.back();
            const std::int64_t predictionMs = std::clamp<std::int64_t>(
                    renderTimeMs - latest.presentationTimeMs,
                    0, FixedStepMaximumPredictionMs);
            const double speedMetersPerSecond = std::max(
                    latest.world.speedMetersPerSecond,
                    latest.simulation.speedKph / 3.6);
            const bool movingForward = sourceAdvancing
                    && latest.simulation.ready
                    && !latest.simulation.finished
                    && latest.world.ready
                    && speedMetersPerSecond > 0.0;
            if (predictionMs <= 0 || !movingForward
                    || fixedStepHistory.size() < 2) {
                result = latest;
            } else {
                const WorkoutGameVisualSnapshot &from =
                        fixedStepHistory[fixedStepHistory.size() - 2];
                const std::int64_t intervalMs = latest.presentationTimeMs
                        - from.presentationTimeMs;
                const double amount = intervalMs > 0
                        ? 1.0 + double(predictionMs) / double(intervalMs)
                        : 1.0;
                result = interpolate(from, latest, amount);
                result.simulation.courseProgress = std::clamp(
                        result.simulation.courseProgress, 0.0, 1.0);
                result.simulation.sectionProgress = std::clamp(
                        result.simulation.sectionProgress, 0.0, 1.0);
                result.simulation.adherence = std::clamp(
                        result.simulation.adherence, 0.0, 1.0);
                result.simulation.challengeReadiness = std::clamp(
                        result.simulation.challengeReadiness, 0.0, 1.0);
                result.world.speedMetersPerSecond = std::max(
                        0.0, result.world.speedMetersPerSecond);
                result.feature.readiness = std::clamp(
                        result.feature.readiness, 0.0, 1.0);
                for (WorkoutGameCompetitorSnapshot &competitor :
                     result.competition.competitors) {
                    competitor.courseProgress = std::clamp(
                            competitor.courseProgress, 0.0, 1.0);
                }
            }
            result.presentationTimeMs = latest.presentationTimeMs
                    + predictionMs;
        } else {
            const WorkoutGameVisualSnapshot &from =
                    fixedStepHistory[upperIndex - 1];
            const WorkoutGameVisualSnapshot &to =
                    fixedStepHistory[upperIndex];
            const std::int64_t intervalMs = to.presentationTimeMs
                    - from.presentationTimeMs;
            const double amount = intervalMs > 0
                    ? std::clamp(
                        double(renderTimeMs - from.presentationTimeMs)
                            / double(intervalMs),
                        0.0, 1.0)
                    : 1.0;
            result = interpolate(from, to, amount);
        }
        if (target.distanceAnchoredPresentation && !courseAnchorHistory.empty()) {
            applyCourseMotion(result, sampleCourse(monotonicTimeMs));
        }
        result.terrainTransition = terrainTransition.sample(monotonicTimeMs);
        return result;
    }
    const std::int64_t elapsedMs = std::max<std::int64_t>(
            0, monotonicTimeMs - transitionStartMs);
    const double amount = std::clamp(
            double(elapsedMs) / double(TransitionDurationMs),
            0.0, 1.0);
    WorkoutGameVisualSnapshot result = interpolate(previous, target, amount);
    result.terrainTransition = terrainTransition.sample(monotonicTimeMs);

    const std::int64_t predictionMs = std::clamp<std::int64_t>(
            elapsedMs - TransitionDurationMs, 0, MaximumPredictionMs);
    const bool movingForward = sourceAdvancing
            && target.simulation.ready
            && !target.simulation.finished
            && target.simulation.workoutTimeMs >= 0;
    if (predictionMs <= 0 || !movingForward) return result;

    result.simulation.workoutTimeMs += predictionMs;
    result.simulation.courseProgress = std::clamp(extrapolate(
            predictionOrigin.simulation.courseProgress,
            target.simulation.courseProgress,
            predictionMs, sourceIntervalMs), 0.0, 1.0);
    result.simulation.sectionProgress = std::clamp(extrapolate(
            predictionOrigin.simulation.sectionProgress,
            target.simulation.sectionProgress,
            predictionMs, sourceIntervalMs), 0.0, 1.0);
    if (result.world.ready) {
        const double predictionSeconds = double(predictionMs) / 1000.0;
        result.world.rider.distanceMeters +=
                result.world.speedMetersPerSecond * predictionSeconds;
        result.world.rider.elevationMeters = extrapolate(
                predictionOrigin.world.rider.elevationMeters,
                target.world.rider.elevationMeters,
                predictionMs, sourceIntervalMs);
    }
    if (result.camera.ready) {
        const double predictionSeconds = double(predictionMs) / 1000.0;
        result.camera.centerDistanceMeters +=
                result.world.speedMetersPerSecond * predictionSeconds;
        result.camera.centerElevationMeters = extrapolate(
                predictionOrigin.camera.centerElevationMeters,
                target.camera.centerElevationMeters,
                predictionMs, sourceIntervalMs);
    }
    for (std::size_t index = 0;
         index < result.competition.competitors.size(); ++index) {
        const WorkoutGameCompetitorSnapshot &origin =
                predictionOrigin.competition.competitors[index];
        result.competition.competitors[index].courseProgress = std::clamp(
                extrapolate(
                    origin.courseProgress,
                    target.competition.competitors[index].courseProgress,
                    predictionMs, sourceIntervalMs),
                0.0, 1.0);
        result.competition.competitors[index].relativeProgress = extrapolate(
                origin.relativeProgress,
                target.competition.competitors[index].relativeProgress,
                predictionMs, sourceIntervalMs);
    }
    return result;
}

void WorkoutGameVisualSmoother::pruneCourseHistory()
{
    if (!coursePresentationStarted) return;
    while (courseAnchorHistory.size() > 1
            && courseAnchorHistory[1].presentationTimeMs <= courseCursorMs) {
        courseAnchorHistory.pop_front();
    }
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::courseAt(double cursorMs) const
{
    for (std::size_t i = 1; i < courseAnchorHistory.size(); ++i) {
        const auto &from = courseAnchorHistory[i - 1];
        const auto &to = courseAnchorHistory[i];
        if (cursorMs < to.presentationTimeMs) {
            const double interval = double(to.presentationTimeMs - from.presentationTimeMs);
            return interpolateCourseMotion(from, to, interval > 0.0
                    ? std::clamp((cursorMs - from.presentationTimeMs) / interval, 0.0, 1.0)
                    : 1.0);
        }
    }
    return courseAnchorHistory.back();
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::sampleCourse(std::int64_t nowMs)
{
    if (!coursePresentationStarted) {
        coursePresentationStarted = true;
        courseCursorMs = std::clamp(double(nowMs) - courseBufferMs,
                double(courseAnchorHistory.front().presentationTimeMs),
                double(courseAnchorHistory.back().presentationTimeMs));
        courseLastSampleMs = nowMs;
        const double remaining = std::max(0.0,
                courseAnchorHistory.back().world.rider.distanceMeters
                - courseAt(courseCursorMs).world.rider.distanceMeters);
        courseVelocityMetersPerSecond = std::min(courseSpeedMetersPerSecond,
                std::sqrt(80.0 * remaining));
    } else if (nowMs > courseLastSampleMs) {
        const auto gapMs = nowMs - courseLastSampleMs;
        courseTiming.maximumPresentationGapMs = std::max(
                courseTiming.maximumPresentationGapMs, gapMs);
        courseLastSampleMs = nowMs;
        const double elapsedMs = double(std::min(gapMs, MaximumDistancePresentationStepMs));
        const double dt = elapsedMs / 1000.0;

        // Skip only an identical stationary bridge, never an unpresented
        // section/feature/terrain boundary.
        pruneCourseHistory();
        if (courseAnchorHistory.size() > 1) {
            const auto &from = courseAnchorHistory[0];
            const auto &to = courseAnchorHistory[1];
            if (from.simulation.workoutTimeMs == to.simulation.workoutTimeMs
                    && from.world.rider.distanceMeters == to.world.rider.distanceMeters
                    && from.world.generation == to.world.generation
                    && from.simulation.activeSection == to.simulation.activeSection
                    && from.feature.phase == to.feature.phase) {
                courseCursorMs = std::max(courseCursorMs, double(to.presentationTimeMs));
                pruneCourseHistory();
            }
        }

        const auto current = courseAt(courseCursorMs);
        const double desiredCursor = double(nowMs) - courseBufferMs;
        // Allow buffering to grow smoothly. Recovery is at most 5% faster,
        // never a rebase to wall-clock time when a late anchor arrives.
        const double rate = std::clamp(1.0 + (desiredCursor - courseCursorMs) / 1000.0,
                0.8, 1.05);
        double nextCursor = std::min(courseCursorMs + elapsedMs * rate,
                double(courseAnchorHistory.back().presentationTimeMs));
        const auto proposed = courseAt(nextCursor);
        const double currentDistance = current.world.rider.distanceMeters;
        const double wantedDistance = std::max(0.0,
                proposed.world.rider.distanceMeters - currentDistance);
        const double remainingDistance = std::max(0.0,
                courseAnchorHistory.back().world.rider.distanceMeters - currentDistance);
        constexpr double MaximumAcceleration = 40.0;
        const double desiredVelocity = std::min(wantedDistance / dt,
                courseSpeedMetersPerSecond * 1.05);
        double nextVelocity = std::clamp(desiredVelocity,
                std::max(0.0, courseVelocityMetersPerSecond - MaximumAcceleration * dt),
                courseVelocityMetersPerSecond + MaximumAcceleration * dt);
        // Preserve a stopping envelope AFTER this step, not just before it:
        // (v+nextV)*dt/2 + nextV^2/(2*a) <= remaining confirmed distance.
        const double adt = MaximumAcceleration * dt;
        const double safeVelocity = (std::sqrt(std::max(0.0,
                adt * adt + 8.0 * MaximumAcceleration * remainingDistance
                - 4.0 * adt * courseVelocityMetersPerSecond)) - adt) * 0.5;
        double allowedDistance;
        if (safeVelocity < 0.0) {
            // Finish braking inside this frame instead of spreading the stop
            // over its full duration and overshooting the remaining distance.
            nextVelocity = 0.0;
            allowedDistance = courseVelocityMetersPerSecond * courseVelocityMetersPerSecond
                    / (2.0 * MaximumAcceleration);
        } else {
            nextVelocity = std::min(nextVelocity, safeVelocity);
            allowedDistance = (courseVelocityMetersPerSecond + nextVelocity) * 0.5 * dt;
        }
        allowedDistance = std::clamp(allowedDistance, 0.0, remainingDistance);
        if (std::abs(allowedDistance - wantedDistance) > 1e-12) {
            const double distanceLimit = currentDistance + allowedDistance;
            nextCursor = double(courseAnchorHistory.back().presentationTimeMs);
            for (std::size_t i = 1; i < courseAnchorHistory.size(); ++i) {
                const auto &from = courseAnchorHistory[i - 1];
                const auto &to = courseAnchorHistory[i];
                if (to.world.rider.distanceMeters > distanceLimit) {
                    const double span = to.world.rider.distanceMeters - from.world.rider.distanceMeters;
                    nextCursor = std::max(courseCursorMs,
                            double(from.presentationTimeMs)
                            + (to.presentationTimeMs - from.presentationTimeMs)
                            * std::clamp((distanceLimit - from.world.rider.distanceMeters) / span, 0.0, 1.0));
                    break;
                }
            }
        }
        courseCursorMs = nextCursor;
        courseVelocityMetersPerSecond = nextVelocity;
        const bool holding = allowedDistance <= 1e-9;
        if (holding && !courseHolding) ++courseTiming.bufferHolds;
        courseHolding = holding;
    }
    pruneCourseHistory();
    // Profile changes begin only when their confirmed anchor is displayed.
    // Do not repeatedly restart transitions with interpolated grade values.
    terrainTransition.setTarget(courseAnchorHistory.front().world, nowMs);
    courseTiming.lagMs = std::max(0.0, double(nowMs) - courseCursorMs);
    return courseAt(courseCursorMs);
}

bool WorkoutGameVisualSmoother::isDiscontinuity(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to)
{
    const bool worldReset = from.world.ready && to.world.ready
            && from.world.generation != to.world.generation
            && to.world.rider.distanceMeters + 2.0
                    < from.world.rider.distanceMeters;
    return from.sessionGeneration != to.sessionGeneration
            || from.simulation.ready != to.simulation.ready
            || from.simulation.finished != to.simulation.finished
            || from.world.ready != to.world.ready
            || worldReset
            || from.camera.ready != to.camera.ready
            || from.distanceAnchoredPresentation
                != to.distanceAnchoredPresentation
            || from.presentationDiscontinuityGeneration
                != to.presentationDiscontinuityGeneration
            || !sameCompetitors(from.competition, to.competition);
}

bool WorkoutGameVisualSmoother::coursePositionChanged(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to)
{
    return from.simulation.workoutTimeMs != to.simulation.workoutTimeMs
            || std::abs(from.world.rider.distanceMeters
                        - to.world.rider.distanceMeters) > 1e-9;
}

bool WorkoutGameVisualSmoother::isCoursePositionDiscontinuity(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to)
{
    const std::int64_t presentationIntervalMs =
            to.presentationTimeMs - from.presentationTimeMs;
    const std::int64_t workoutIntervalMs =
            to.simulation.workoutTimeMs - from.simulation.workoutTimeMs;
    const double distanceDelta = to.world.rider.distanceMeters
            - from.world.rider.distanceMeters;
    if (presentationIntervalMs <= 0 || workoutIntervalMs < 0
            || distanceDelta < -1e-6) {
        return true;
    }

    const double intervalSeconds = double(presentationIntervalMs) / 1000.0;
    const double speedMetersPerSecond = std::max({
            0.0,
            from.world.speedMetersPerSecond,
            to.world.speedMetersPerSecond,
            from.simulation.speedKph / 3.6,
            to.simulation.speedKph / 3.6
    });
    const double maximumContinuousDistance = std::max(
            10.0, speedMetersPerSecond * intervalSeconds * 4.0 + 2.0);
    const std::int64_t maximumContinuousWorkoutInterval = std::max<std::int64_t>(
            5000, presentationIntervalMs * 8);
    return distanceDelta > maximumContinuousDistance
            || workoutIntervalMs > maximumContinuousWorkoutInterval;
}

void WorkoutGameVisualSmoother::applyCourseMotion(
        WorkoutGameVisualSnapshot &result,
        const WorkoutGameVisualSnapshot &motion)
{
    result.simulation = motion.simulation;
    result.competition = motion.competition;
    result.world = motion.world;
    result.camera = motion.camera;
    result.feature = motion.feature;
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::interpolateCourseMotion(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to,
        double amount)
{
    const WorkoutGameVisualSnapshot continuous = interpolate(from, to, amount);
    if (amount >= 1.0) return continuous;

    WorkoutGameVisualSnapshot result = from;
    result.simulation.workoutTimeMs =
            continuous.simulation.workoutTimeMs;
    result.simulation.courseProgress =
            continuous.simulation.courseProgress;
    result.simulation.speedKph = continuous.simulation.speedKph;
    if (from.simulation.activeSection == to.simulation.activeSection) {
        result.simulation.sectionProgress =
                continuous.simulation.sectionProgress;
        result.simulation.adherence = continuous.simulation.adherence;
        result.simulation.streakSeconds = continuous.simulation.streakSeconds;
        result.simulation.challengeMetrics =
                continuous.simulation.challengeMetrics;
        result.simulation.challengeAssessment =
                continuous.simulation.challengeAssessment;
        result.simulation.challengeAssessment.completed =
                from.simulation.challengeAssessment.completed;
        result.simulation.challengeReadiness =
                continuous.simulation.challengeReadiness;
    }

    result.competition = from.competition;
    if (result.competition.competitors.size()
            == continuous.competition.competitors.size()) {
        for (std::size_t index = 0;
             index < result.competition.competitors.size(); ++index) {
            result.competition.competitors[index].courseProgress =
                    continuous.competition.competitors[index].courseProgress;
            result.competition.competitors[index].relativeProgress =
                    continuous.competition.competitors[index].relativeProgress;
        }
    }

    result.world.speedMetersPerSecond =
            continuous.world.speedMetersPerSecond;
    result.world.landingImpact = continuous.world.landingImpact;
    if (from.world.generation == to.world.generation) {
        result.world.gradePercent = continuous.world.gradePercent;
        result.world.difficulty = continuous.world.difficulty;
        result.world.terrainOffsetMeters =
                continuous.world.terrainOffsetMeters;
        result.world.surfaceElevationMeters =
                continuous.world.surfaceElevationMeters;
    }
    result.world.rider.distanceMeters =
            continuous.world.rider.distanceMeters;
    result.world.rider.elevationMeters =
            continuous.world.rider.elevationMeters;
    result.world.rider.pitchDegrees =
            continuous.world.rider.pitchDegrees;
    result.world.rider.rollDegrees = continuous.world.rider.rollDegrees;
    result.world.rider.rearSuspension =
            continuous.world.rider.rearSuspension;
    result.world.rider.frontSuspension =
            continuous.world.rider.frontSuspension;
    result.world.rider.rearWheelRadians =
            continuous.world.rider.rearWheelRadians;
    result.world.rider.frontWheelRadians =
            continuous.world.rider.frontWheelRadians;
    result.world.rider.clearanceMeters =
            continuous.world.rider.clearanceMeters;

    result.camera.centerDistanceMeters =
            continuous.camera.centerDistanceMeters;
    result.camera.centerElevationMeters =
            continuous.camera.centerElevationMeters;
    result.camera.lookAheadMeters = continuous.camera.lookAheadMeters;
    result.camera.zoom = continuous.camera.zoom;
    result.camera.yawDegrees = continuous.camera.yawDegrees;
    result.camera.pitchDegrees = continuous.camera.pitchDegrees;

    result.feature = from.feature;
    if (from.feature.sourceSectionIndex == to.feature.sourceSectionIndex
            && from.feature.actionId == to.feature.actionId) {
        result.feature.visualDistanceMeters =
                continuous.feature.visualDistanceMeters;
        result.feature.distanceToObstacleMeters =
                continuous.feature.distanceToObstacleMeters;
        result.feature.readiness = continuous.feature.readiness;
        result.feature.bermLineBias = continuous.feature.bermLineBias;
        result.feature.lateralOffsetMeters =
                continuous.feature.lateralOffsetMeters;
        result.feature.verticalOffsetMeters =
                continuous.feature.verticalOffsetMeters;
        result.feature.flightDurationSeconds =
                continuous.feature.flightDurationSeconds;
        result.feature.pitchDegrees = continuous.feature.pitchDegrees;
        result.feature.vibration = continuous.feature.vibration;
        result.feature.landingImpact = continuous.feature.landingImpact;
    }
    return result;
}

WorkoutGameVisualSnapshot WorkoutGameVisualSmoother::interpolate(
        const WorkoutGameVisualSnapshot &from,
        const WorkoutGameVisualSnapshot &to,
        double amount)
{
    WorkoutGameVisualSnapshot result = to;

    result.simulation.workoutTimeMs = std::llround(lerp(
            double(from.simulation.workoutTimeMs),
            double(to.simulation.workoutTimeMs), amount));
    result.simulation.courseProgress = lerp(
            from.simulation.courseProgress, to.simulation.courseProgress, amount);
    result.simulation.sectionProgress = lerp(
            from.simulation.sectionProgress, to.simulation.sectionProgress, amount);
    result.simulation.speedKph = lerp(
            from.simulation.speedKph, to.simulation.speedKph, amount);
    result.simulation.adherence = lerp(
            from.simulation.adherence, to.simulation.adherence, amount);
    result.simulation.streakSeconds = lerp(
            from.simulation.streakSeconds, to.simulation.streakSeconds, amount);
    result.simulation.challengeReadiness = lerp(
            from.simulation.challengeReadiness,
            to.simulation.challengeReadiness,
            amount);
    result.simulation.challengeMetrics.averageActualWatts = lerp(
            from.simulation.challengeMetrics.averageActualWatts,
            to.simulation.challengeMetrics.averageActualWatts, amount);
    result.simulation.challengeMetrics.averageTargetWatts = lerp(
            from.simulation.challengeMetrics.averageTargetWatts,
            to.simulation.challengeMetrics.averageTargetWatts, amount);
    result.simulation.challengeMetrics.averageEffortRatio = lerp(
            from.simulation.challengeMetrics.averageEffortRatio,
            to.simulation.challengeMetrics.averageEffortRatio, amount);
    result.simulation.challengeMetrics.averageCadenceRpm = lerp(
            from.simulation.challengeMetrics.averageCadenceRpm,
            to.simulation.challengeMetrics.averageCadenceRpm, amount);
    result.simulation.challengeMetrics.averageSpeedKph = lerp(
            from.simulation.challengeMetrics.averageSpeedKph,
            to.simulation.challengeMetrics.averageSpeedKph, amount);
    result.simulation.challengeMetrics.averageAdherence = lerp(
            from.simulation.challengeMetrics.averageAdherence,
            to.simulation.challengeMetrics.averageAdherence, amount);
    result.simulation.challengeAssessment.readiness = lerp(
            from.simulation.challengeAssessment.readiness,
            to.simulation.challengeAssessment.readiness, amount);
    result.simulation.challengeAssessment.effortReadiness = lerp(
            from.simulation.challengeAssessment.effortReadiness,
            to.simulation.challengeAssessment.effortReadiness, amount);
    result.simulation.challengeAssessment.cadenceReadiness = lerp(
            from.simulation.challengeAssessment.cadenceReadiness,
            to.simulation.challengeAssessment.cadenceReadiness, amount);
    result.simulation.challengeAssessment.speedReadiness = lerp(
            from.simulation.challengeAssessment.speedReadiness,
            to.simulation.challengeAssessment.speedReadiness, amount);
    result.simulation.challengeAssessment.adherenceReadiness = lerp(
            from.simulation.challengeAssessment.adherenceReadiness,
            to.simulation.challengeAssessment.adherenceReadiness, amount);
    result.riderPedalCycles = lerp(
            from.riderPedalCycles, to.riderPedalCycles, amount);
    for (std::size_t index = 0;
         index < result.competition.competitors.size(); ++index) {
        result.competition.competitors[index].courseProgress = lerp(
                from.competition.competitors[index].courseProgress,
                to.competition.competitors[index].courseProgress, amount);
        result.competition.competitors[index].relativeProgress = lerp(
                from.competition.competitors[index].relativeProgress,
                to.competition.competitors[index].relativeProgress, amount);
    }

    result.world.gradePercent = lerp(
            from.world.gradePercent, to.world.gradePercent, amount);
    result.world.difficulty = lerp(
            from.world.difficulty, to.world.difficulty, amount);
    result.world.terrainOffsetMeters = from.world.generation
            == to.world.generation
            ? lerp(from.world.terrainOffsetMeters,
                   to.world.terrainOffsetMeters, amount)
            : to.world.terrainOffsetMeters;
    result.world.surfaceElevationMeters = from.world.generation
            == to.world.generation
            ? lerp(from.world.surfaceElevationMeters,
                   to.world.surfaceElevationMeters, amount)
            : to.world.surfaceElevationMeters;
    result.world.speedMetersPerSecond = lerp(
            from.world.speedMetersPerSecond,
            to.world.speedMetersPerSecond, amount);
    result.world.landingImpact = lerp(
            from.world.landingImpact, to.world.landingImpact, amount);
    result.world.rider.distanceMeters = lerp(
            from.world.rider.distanceMeters,
            to.world.rider.distanceMeters, amount);
    result.world.rider.elevationMeters = lerp(
            from.world.rider.elevationMeters,
            to.world.rider.elevationMeters, amount);
    result.world.rider.pitchDegrees = lerpAngle(
            from.world.rider.pitchDegrees,
            to.world.rider.pitchDegrees, amount, 360.0);
    result.world.rider.rollDegrees = lerpAngle(
            from.world.rider.rollDegrees,
            to.world.rider.rollDegrees, amount, 360.0);
    result.world.rider.rearSuspension = lerp(
            from.world.rider.rearSuspension,
            to.world.rider.rearSuspension, amount);
    result.world.rider.frontSuspension = lerp(
            from.world.rider.frontSuspension,
            to.world.rider.frontSuspension, amount);
    result.world.rider.rearWheelRadians = lerpAngle(
            from.world.rider.rearWheelRadians,
            to.world.rider.rearWheelRadians, amount, 2.0 * std::acos(-1.0));
    result.world.rider.frontWheelRadians = lerpAngle(
            from.world.rider.frontWheelRadians,
            to.world.rider.frontWheelRadians, amount, 2.0 * std::acos(-1.0));
    result.world.rider.clearanceMeters = lerp(
            from.world.rider.clearanceMeters,
            to.world.rider.clearanceMeters, amount);

    if (from.feature.ready && to.feature.ready
            && from.feature.sourceSectionIndex
                == to.feature.sourceSectionIndex
            && from.feature.actionId == to.feature.actionId) {
        result.feature.visualDistanceMeters = lerp(
                from.feature.visualDistanceMeters,
                to.feature.visualDistanceMeters, amount);
        result.feature.distanceToObstacleMeters = lerp(
                from.feature.distanceToObstacleMeters,
                to.feature.distanceToObstacleMeters, amount);
        result.feature.readiness = lerp(
                from.feature.readiness, to.feature.readiness, amount);
        result.feature.bermLineBias = lerp(
                from.feature.bermLineBias,
                to.feature.bermLineBias, amount);
        result.feature.lateralOffsetMeters = lerp(
                from.feature.lateralOffsetMeters,
                to.feature.lateralOffsetMeters, amount);
        result.feature.verticalOffsetMeters = lerp(
                from.feature.verticalOffsetMeters,
                to.feature.verticalOffsetMeters, amount);
        result.feature.flightDurationSeconds = lerp(
                from.feature.flightDurationSeconds,
                to.feature.flightDurationSeconds, amount);
        result.feature.pitchDegrees = lerp(
                from.feature.pitchDegrees,
                to.feature.pitchDegrees, amount);
        result.feature.vibration = lerp(
                from.feature.vibration, to.feature.vibration, amount);
        result.feature.landingImpact = lerp(
                from.feature.landingImpact,
                to.feature.landingImpact, amount);
    }

    result.camera.centerDistanceMeters = lerp(
            from.camera.centerDistanceMeters,
            to.camera.centerDistanceMeters, amount);
    result.camera.centerElevationMeters = lerp(
            from.camera.centerElevationMeters,
            to.camera.centerElevationMeters, amount);
    result.camera.lookAheadMeters = lerp(
            from.camera.lookAheadMeters, to.camera.lookAheadMeters, amount);
    result.camera.zoom = lerp(from.camera.zoom, to.camera.zoom, amount);
    result.camera.yawDegrees = lerpAngle(
            from.camera.yawDegrees, to.camera.yawDegrees, amount, 360.0);
    result.camera.pitchDegrees = lerpAngle(
            from.camera.pitchDegrees, to.camera.pitchDegrees, amount, 360.0);
    return result;
}

void WorkoutGameFrameRateCounter::reset()
{
    initialized = false;
    lastFrameNs = 0;
    intervalTotalNs = 0;
    recentIntervalsNs.clear();
    currentFps = 0.0;
    p50FrameIntervalMs = 0.0;
    p95FrameIntervalMs = 0.0;
    p99FrameIntervalMs = 0.0;
}

double WorkoutGameFrameRateCounter::frameRendered(
        std::int64_t monotonicTimeMs)
{
    return frameRenderedNanoseconds(monotonicTimeMs * 1000000);
}

double WorkoutGameFrameRateCounter::frameRenderedNanoseconds(
        std::int64_t monotonicTimeNs)
{
    if (!initialized || monotonicTimeNs < lastFrameNs) {
        initialized = true;
        lastFrameNs = monotonicTimeNs;
        intervalTotalNs = 0;
        recentIntervalsNs.clear();
        currentFps = 0.0;
        p50FrameIntervalMs = 0.0;
        p95FrameIntervalMs = 0.0;
        p99FrameIntervalMs = 0.0;
        return currentFps;
    }

    const std::int64_t intervalNs = monotonicTimeNs - lastFrameNs;
    lastFrameNs = monotonicTimeNs;
    if (intervalNs <= 0) return currentFps;
    recentIntervalsNs.push_back(intervalNs);
    intervalTotalNs += intervalNs;
    while (recentIntervalsNs.size() > 2
            && intervalTotalNs > 1000000000ll) {
        intervalTotalNs -= recentIntervalsNs.front();
        recentIntervalsNs.pop_front();
    }
    currentFps = double(recentIntervalsNs.size()) * 1000000000.0
            / double(std::max<std::int64_t>(1, intervalTotalNs));
    std::vector<std::int64_t> ordered(
            recentIntervalsNs.begin(), recentIntervalsNs.end());
    std::sort(ordered.begin(), ordered.end());
    const auto percentileMilliseconds = [&ordered](double percentile) {
        if (ordered.empty()) return 0.0;
        const std::size_t index = std::min(
                ordered.size() - 1,
                std::size_t(std::ceil(ordered.size() * percentile)) - 1);
        return double(ordered[index]) / 1000000.0;
    };
    p50FrameIntervalMs = percentileMilliseconds(0.50);
    p95FrameIntervalMs = percentileMilliseconds(0.95);
    p99FrameIntervalMs = percentileMilliseconds(0.99);
    return currentFps;
}
