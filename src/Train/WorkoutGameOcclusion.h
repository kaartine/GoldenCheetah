/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_WorkoutGameOcclusion_h
#define _GC_WorkoutGameOcclusion_h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

struct WorkoutGameOcclusionVertex
{
    double x = 0.0;
    double y = 0.0;
    double depthMeters = 0.0;
    double occlusionY = 0.0;
};

template<std::size_t Capacity>
struct WorkoutGameOcclusionPolygon
{
    std::array<WorkoutGameOcclusionVertex, Capacity> vertices = {};
    std::size_t count = 0u;

    bool empty() const { return count == 0u; }
    std::size_t size() const { return count; }
    const WorkoutGameOcclusionVertex *begin() const { return vertices.data(); }
    const WorkoutGameOcclusionVertex *end() const
    {
        return vertices.data() + count;
    }
    const WorkoutGameOcclusionVertex &operator[](std::size_t index) const
    {
        return vertices[index];
    }
};

class WorkoutGameOcclusion
{
public:
    template<std::size_t Size>
    static WorkoutGameOcclusionPolygon<Size + 1u> clip(
            const std::array<WorkoutGameOcclusionVertex, Size> &polygon,
            double allowancePixels = 0.0)
    {
        WorkoutGameOcclusionPolygon<Size + 1u> output;
        if (Size < 3u || !std::isfinite(allowancePixels)
                || allowancePixels < 0.0) {
            return output;
        }
        for (const WorkoutGameOcclusionVertex &vertex : polygon) {
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)
                    || !std::isfinite(vertex.depthMeters)
                    || !std::isfinite(vertex.occlusionY)) {
                return {};
            }
        }
        const auto visibility = [allowancePixels](
                const WorkoutGameOcclusionVertex &vertex) {
            return vertex.occlusionY + allowancePixels - vertex.y;
        };
        const auto interpolate = [](const WorkoutGameOcclusionVertex &from,
                                    const WorkoutGameOcclusionVertex &to,
                                    double amount) {
            const auto value = [amount](double first, double second) {
                return first + (second - first) * amount;
            };
            return WorkoutGameOcclusionVertex {
                value(from.x, to.x),
                value(from.y, to.y),
                value(from.depthMeters, to.depthMeters),
                value(from.occlusionY, to.occlusionY)
            };
        };
        const auto samePoint = [](const WorkoutGameOcclusionVertex &left,
                                  const WorkoutGameOcclusionVertex &right) {
            return std::abs(left.x - right.x) <= 1e-9
                    && std::abs(left.y - right.y) <= 1e-9;
        };
        const auto append = [&output, &samePoint](
                const WorkoutGameOcclusionVertex &vertex) {
            if (output.count > 0u
                    && samePoint(output.vertices[output.count - 1u], vertex)) {
                return;
            }
            if (output.count < output.vertices.size()) {
                output.vertices[output.count++] = vertex;
            }
        };

        WorkoutGameOcclusionVertex previous = polygon.back();
        double previousVisibility = visibility(previous);
        bool previousInside = previousVisibility >= -1e-6;
        for (const WorkoutGameOcclusionVertex &current : polygon) {
            const double currentVisibility = visibility(current);
            const bool currentInside = currentVisibility >= -1e-6;
            if (currentInside != previousInside) {
                const double span = previousVisibility - currentVisibility;
                const double amount = std::abs(span) > 1e-12
                        ? std::clamp(previousVisibility / span, 0.0, 1.0)
                        : 0.0;
                append(interpolate(previous, current, amount));
            }
            if (currentInside) append(current);
            previous = current;
            previousVisibility = currentVisibility;
            previousInside = currentInside;
        }
        if (output.count > 1u
                && samePoint(output.vertices.front(),
                             output.vertices[output.count - 1u])) {
            --output.count;
        }
        if (output.count < 3u) output.count = 0u;
        return output;
    }
};

#endif
