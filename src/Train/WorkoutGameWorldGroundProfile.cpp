/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWorldGroundProfile.h"

#include "WorkoutGameAssetPhysicsSampler.h"
#include "WorkoutGameRoadCourse.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double CoincidentPointToleranceMeters = 1.0e-8;
// Box2D rejects segments whose length is at most its 5 mm linear slop.
// Asset breakpoints are authoritative, so discard nearby generated samples
// instead of perturbing the authored profile.
constexpr double MinimumGeneratedPointSeparationMeters = 0.006;

}

std::vector<double> WorkoutGameWorldGroundProfile::mergeBreakpoints(
        const WorkoutGameRoadCourse &course,
        double distanceBaseMeters,
        double riderStartMeters,
        double localStartMeters,
        double localEndMeters,
        std::vector<double> samplePoints)
{
    if (!course.assetPhysicsSnapshot
            || !std::isfinite(distanceBaseMeters)
            || !std::isfinite(riderStartMeters)
            || !std::isfinite(localStartMeters)
            || !std::isfinite(localEndMeters)
            || localStartMeters > localEndMeters) {
        return samplePoints;
    }

    std::vector<double> authoredPoints;
    const auto &snapshot = *course.assetPhysicsSnapshot;
    for (std::size_t pieceIndex = 0;
            pieceIndex < snapshot.pieceBindings.size(); ++pieceIndex) {
        const auto breakpoints =
                WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                    snapshot, pieceIndex);
        for (double courseDistance : breakpoints) {
            const double local = courseDistance - distanceBaseMeters
                    + riderStartMeters;
            if (local < localStartMeters || local > localEndMeters) continue;
            authoredPoints.push_back(local);
        }
    }
    std::sort(authoredPoints.begin(), authoredPoints.end());
    authoredPoints.erase(
            std::unique(
                authoredPoints.begin(), authoredPoints.end(),
                [](double left, double right) {
                    return std::abs(left - right)
                            <= CoincidentPointToleranceMeters;
                }),
            authoredPoints.end());
    samplePoints.erase(
            std::remove_if(
                samplePoints.begin(), samplePoints.end(),
                [&authoredPoints](double point) {
                    const auto right = std::lower_bound(
                            authoredPoints.begin(), authoredPoints.end(), point);
                    if (right != authoredPoints.end()
                            && std::abs(point - *right)
                                < MinimumGeneratedPointSeparationMeters) {
                        return true;
                    }
                    return right != authoredPoints.begin()
                            && std::abs(point - *(right - 1))
                                < MinimumGeneratedPointSeparationMeters;
                }),
            samplePoints.end());
    samplePoints.insert(samplePoints.end(),
                        authoredPoints.begin(), authoredPoints.end());

    std::sort(samplePoints.begin(), samplePoints.end());
    samplePoints.erase(
            std::unique(
                samplePoints.begin(), samplePoints.end(),
                [](double left, double right) {
                    return std::abs(left - right)
                            <= CoincidentPointToleranceMeters;
                }),
            samplePoints.end());
    return samplePoints;
}

WorkoutGameWorldGroundMaterial WorkoutGameWorldGroundProfile::materialAt(
        const WorkoutGameRoadCourse &course,
        double courseDistanceMeters)
{
    WorkoutGameWorldGroundMaterial result;
    if (!course.assetPhysicsSnapshot
            || !std::isfinite(courseDistanceMeters)) {
        return result;
    }

    const auto &snapshot = *course.assetPhysicsSnapshot;
    for (std::size_t pieceIndex = 0;
            pieceIndex < snapshot.pieceBindings.size(); ++pieceIndex) {
        const WorkoutGameAssetPhysicsSample sample =
                WorkoutGameAssetPhysicsSampler::sample(
                    snapshot, pieceIndex, courseDistanceMeters);
        if (!sample.surfacePresent) continue;
        result.assetDefined = true;
        result.coulombFriction = sample.coulombFriction;
        result.restitution = sample.restitution;
        return result;
    }
    return result;
}
