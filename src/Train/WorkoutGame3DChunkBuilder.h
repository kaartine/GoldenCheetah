/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGame3DChunkBuilder_h
#define _GC_WorkoutGame3DChunkBuilder_h

#include "WorkoutGame3DGeometry.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct WorkoutGame3DChunk
{
    static constexpr std::size_t LayerCount = 8;

    std::array<WorkoutGame3DMeshData, LayerCount> layers;
    int bucket = 0;
    std::uint64_t courseGeneration = 0;
    std::uint64_t requestRevision = 0;
    double startDistanceMeters = 0.0;
    double endDistanceMeters = 0.0;

    int triangleCount() const;
    bool floorReady() const { return layers[0].ready; }
};

class WorkoutGame3DChunkBuilder
{
public:
    using CompletionCallback = std::function<void()>;

    WorkoutGame3DChunkBuilder();
    ~WorkoutGame3DChunkBuilder();

    WorkoutGame3DChunkBuilder(const WorkoutGame3DChunkBuilder &) = delete;
    WorkoutGame3DChunkBuilder &operator=(
            const WorkoutGame3DChunkBuilder &) = delete;

    void request(
            std::shared_ptr<const WorkoutGameRoadCourse> course,
            double startDistanceMeters,
            double endDistanceMeters,
            int bucket,
            std::uint64_t courseGeneration);
    bool takeLatest(WorkoutGame3DChunk &chunk);
    void setCompletionCallback(CompletionCallback callback);
    void shutdown();

    std::size_t maximumPendingDepth() const;
    std::size_t maximumResultDepth() const;
    std::size_t pendingDepth() const;
    std::size_t resultDepth() const;
    std::uint64_t completedBuildCount() const;
    std::uint64_t discardedBuildCount() const;

private:
    struct Request
    {
        std::shared_ptr<const WorkoutGameRoadCourse> course;
        double startDistanceMeters = 0.0;
        double endDistanceMeters = 0.0;
        int bucket = 0;
        std::uint64_t courseGeneration = 0;
        std::uint64_t revision = 0;
    };

    void run();
    bool superseded(std::uint64_t revision) const;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::optional<Request> pending;
    std::optional<WorkoutGame3DChunk> latest;
    std::thread worker;
    bool stopping = false;
    std::uint64_t nextRevision = 0;
    std::uint64_t newestRevision = 0;
    std::uint64_t completedBuilds = 0;
    std::uint64_t discardedBuilds = 0;
    std::size_t maximumPending = 0;
    std::size_t maximumResults = 0;
    CompletionCallback completionCallback;
};

#endif
