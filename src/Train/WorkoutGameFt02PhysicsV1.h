/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFt02PhysicsV1_h
#define _GC_WorkoutGameFt02PhysicsV1_h

#include "WorkoutGameAssetPhysicsSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Frozen migration/evaluation adapter for the first catalog-backed feature.
// Keep this independent of WorkoutGameFeatureGeometry so old courses do not
// change when the editable procedural model evolves.
class WorkoutGameFt02PhysicsV1
{
public:
    static WorkoutGameAssetPhysicsDefinition definitionForExtent(
            std::uint32_t extentMm)
    {
        WorkoutGameAssetPhysicsDefinition definition;
        definition.coulombFrictionMilli = 1100;
        definition.restitutionMilli = 0;
        if (extentMm < 1) return definition;

        constexpr double Pi = 3.14159265358979323846;
        constexpr int RadialSegments = 16;
        const double radius = double(extentMm) * 0.5;
        std::vector<std::int32_t> forwardCoordinates = {
            std::int32_t(std::floor(-radius)),
            0,
            std::int32_t(std::ceil(radius))
        };
        for (int facet = 1; facet < RadialSegments / 2; ++facet) {
            const double boundary = std::cos(
                    Pi - double(facet) * 2.0 * Pi
                        / double(RadialSegments)) * radius;
            const std::int32_t left = std::int32_t(std::floor(boundary));
            const std::int32_t right = std::int32_t(std::ceil(boundary));
            const std::int32_t leftHeight = heightAt(left, extentMm);
            const std::int32_t rightHeight = heightAt(right, extentMm);
            const std::int64_t forward = std::int64_t(right) - left;
            const std::int64_t height =
                    std::int64_t(rightHeight) - leftHeight;
            if (left != right && forward * forward + height * height > 25) {
                forwardCoordinates.push_back(left);
                forwardCoordinates.push_back(right);
            } else {
                forwardCoordinates.push_back(
                        std::int32_t(std::llround(boundary)));
            }
        }
        std::sort(forwardCoordinates.begin(), forwardCoordinates.end());
        forwardCoordinates.erase(
                std::unique(forwardCoordinates.begin(),
                            forwardCoordinates.end()),
                forwardCoordinates.end());

        WorkoutGameAssetPhysicsChain chain;
        chain.points.reserve(forwardCoordinates.size());
        for (std::int32_t forward : forwardCoordinates) {
            chain.points.push_back({forward, heightAt(forward, extentMm)});
        }
        chain.points.front().heightMm = 0;
        chain.points.back().heightMm = 0;
        // Bias the outer support point inward by one millimetre of height.
        // This leaves margin for the persisted millimetre anchor rounding at
        // the steepest facet while staying within one millimetre at the point.
        if (chain.points.size() >= 4) {
            ++chain.points[1].heightMm;
            ++chain.points[chain.points.size() - 2].heightMm;
        }
        definition.chains.push_back(std::move(chain));
        return definition;
    }

private:
    static std::int32_t heightAt(
            std::int32_t forwardMm,
            std::uint32_t extentMm)
    {
        constexpr double Pi = 3.14159265358979323846;
        constexpr int RadialSegments = 16;
        const double radius = double(extentMm) * 0.5;
        if (double(forwardMm) <= -radius
                || double(forwardMm) >= radius) {
            return 0;
        }
        for (int segment = 0; segment < RadialSegments / 2; ++segment) {
            const double fromAngle = Pi
                    - double(segment) * 2.0 * Pi
                        / double(RadialSegments);
            const double toAngle = Pi
                    - double(segment + 1) * 2.0 * Pi
                        / double(RadialSegments);
            const double fromX = std::cos(fromAngle) * radius;
            const double toX = std::cos(toAngle) * radius;
            if (double(forwardMm) <= toX + 1.0e-12) {
                const double amount = std::clamp(
                        (double(forwardMm) - fromX) / (toX - fromX),
                        0.0, 1.0);
                const double fromY =
                        std::sin(fromAngle) * double(extentMm);
                const double toY =
                        std::sin(toAngle) * double(extentMm);
                return std::int32_t(std::llround(
                        fromY + (toY - fromY) * amount));
            }
        }
        return std::int32_t(std::llround(radius));
    }
};

#endif
