/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DChunkBuilder.h"

#include <algorithm>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

constexpr std::array<WorkoutGame3DGeometry::Layer,
                     WorkoutGame3DChunk::LayerCount> ChunkLayers = {{
    WorkoutGame3DGeometry::Layer::ForestFloor,
    WorkoutGame3DGeometry::Layer::Roots,
    WorkoutGame3DGeometry::Layer::Climb,
    WorkoutGame3DGeometry::Layer::RockGarden,
    WorkoutGame3DGeometry::Layer::RockSlab,
    WorkoutGame3DGeometry::Layer::Skinny,
    WorkoutGame3DGeometry::Layer::ForestDressing
}};

void lowerWorkerPriority()
{
#if defined(__linux__)
    setpriority(PRIO_PROCESS, 0, 7);
#endif
}

}

int WorkoutGame3DChunk::triangleCount() const
{
    int total = 0;
    for (const WorkoutGame3DMeshData &layer : layers) {
        total += layer.triangleCount();
    }
    return total;
}

WorkoutGame3DChunkBuilder::WorkoutGame3DChunkBuilder()
{
    worker = std::thread(&WorkoutGame3DChunkBuilder::run, this);
}

WorkoutGame3DChunkBuilder::~WorkoutGame3DChunkBuilder()
{
    shutdown();
}

void WorkoutGame3DChunkBuilder::request(
        std::shared_ptr<const WorkoutGameRoadCourse> course,
        double startDistanceMeters,
        double endDistanceMeters,
        int bucket,
        std::uint64_t courseGeneration)
{
    if (!course) return;
    Request request;
    request.course = std::move(course);
    request.startDistanceMeters = startDistanceMeters;
    request.endDistanceMeters = endDistanceMeters;
    request.bucket = bucket;
    request.courseGeneration = courseGeneration;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return;
        request.revision = ++nextRevision;
        newestRevision = request.revision;
        pending = std::move(request);
        maximumPending = std::max<std::size_t>(maximumPending, 1);
    }
    changed.notify_one();
}

bool WorkoutGame3DChunkBuilder::takeLatest(WorkoutGame3DChunk &chunk)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!latest) return false;
    chunk = std::move(*latest);
    latest.reset();
    return true;
}

void WorkoutGame3DChunkBuilder::setCompletionCallback(
        CompletionCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex);
    completionCallback = std::move(callback);
}

void WorkoutGame3DChunkBuilder::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping && !worker.joinable()) return;
        stopping = true;
        pending.reset();
    }
    changed.notify_all();
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(mutex);
    latest.reset();
}

std::size_t WorkoutGame3DChunkBuilder::maximumPendingDepth() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return maximumPending;
}

std::size_t WorkoutGame3DChunkBuilder::maximumResultDepth() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return maximumResults;
}

std::size_t WorkoutGame3DChunkBuilder::pendingDepth() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return pending ? 1 : 0;
}

std::size_t WorkoutGame3DChunkBuilder::resultDepth() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return latest ? 1 : 0;
}

std::uint64_t WorkoutGame3DChunkBuilder::completedBuildCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return completedBuilds;
}

std::uint64_t WorkoutGame3DChunkBuilder::discardedBuildCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return discardedBuilds;
}

bool WorkoutGame3DChunkBuilder::superseded(std::uint64_t revision) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return stopping || revision != newestRevision;
}

void WorkoutGame3DChunkBuilder::run()
{
    lowerWorkerPriority();
    while (true) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this]() { return stopping || pending; });
            if (stopping) return;
            request = std::move(*pending);
            pending.reset();
        }

        WorkoutGame3DChunk result;
        result.bucket = request.bucket;
        result.courseGeneration = request.courseGeneration;
        result.requestRevision = request.revision;
        result.startDistanceMeters = request.startDistanceMeters;
        result.endDistanceMeters = request.endDistanceMeters;
        bool obsolete = false;
        for (std::size_t index = 0; index < ChunkLayers.size(); ++index) {
            result.layers[index] = WorkoutGame3DGeometry::buildMeshData(
                    ChunkLayers[index], *request.course,
                    request.startDistanceMeters,
                    request.endDistanceMeters);
            if (superseded(request.revision)) {
                obsolete = true;
                break;
            }
        }

        CompletionCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            if (obsolete || request.revision != newestRevision) {
                ++discardedBuilds;
                continue;
            }
            latest = std::move(result);
            ++completedBuilds;
            maximumResults = std::max<std::size_t>(maximumResults, 1);
            callback = completionCallback;
        }
        if (callback) callback();
    }
}
