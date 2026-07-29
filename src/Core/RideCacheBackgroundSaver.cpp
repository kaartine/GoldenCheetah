/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCacheBackgroundSaver.h"

#include "RideCacheSaveSnapshot.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <QDebug>

namespace {

struct SaveCompletion
{
    std::mutex mutex;
    std::condition_variable completed;
    bool done = false;
    bool saved = false;
    bool abandoned = false;
    QString error;
};

struct SaveTask
{
    std::shared_ptr<const RideCacheSave::Snapshot> snapshot;
    std::shared_ptr<SaveCompletion> completion;
};

} // namespace

struct RideCacheBackgroundSaver::State
{
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable drained;
    std::deque<SaveTask> queue;
    std::thread worker;
    std::thread::id workerId;
    RideCacheBackgroundSaver::Writer writer;
    QString pendingError;
    bool accepting = true;
    bool active = false;
    bool stopping = false;

    explicit State(RideCacheBackgroundSaver::Writer writer)
        : writer(std::move(writer))
    {
    }
};

RideCacheBackgroundSaver::RideCacheBackgroundSaver(Writer writer)
    : state_(new State(
          writer
              ? std::move(writer)
              : Writer([](
                    const RideCacheSave::Snapshot &snapshot,
                    QString &error) {
                    return RideCacheSave::write(
                        snapshot, error);
                })))
{
    State *state = state_.get();
    state->worker = std::thread([state]() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->workerId = std::this_thread::get_id();
        }

        for (;;) {
            SaveTask task;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->workAvailable.wait(lock, [state]() {
                    return state->stopping || !state->queue.empty();
                });
                if (state->queue.empty()) break;
                task = std::move(state->queue.front());
                state->queue.pop_front();
                state->active = true;
            }

            QString error;
            const bool saved =
                state->writer(*task.snapshot, error);
            bool abandoned = false;
            if (task.completion) {
                {
                    std::lock_guard<std::mutex> lock(
                        task.completion->mutex);
                    abandoned = task.completion->abandoned;
                    task.completion->saved = saved;
                    task.completion->error = error;
                    task.completion->done = true;
                }
                task.completion->completed.notify_one();
            } else if (!saved) {
                qWarning().noquote()
                    << "Cannot save ride cache:" << error;
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if ((!task.completion || abandoned)
                    && !saved
                    && state->pendingError.isEmpty()) {
                    state->pendingError = error;
                }
                state->active = false;
                if (state->queue.empty()) {
                    state->drained.notify_all();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->active = false;
            state->drained.notify_all();
        }
    });
}

RideCacheBackgroundSaver::~RideCacheBackgroundSaver()
{
    stop();
}

bool
RideCacheBackgroundSaver::isRunning() const
{
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->accepting;
}

bool
RideCacheBackgroundSaver::enqueue(
    const std::shared_ptr<
        const RideCacheSave::Snapshot> &snapshot)
{
    if (!snapshot || !state_) return false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting) return false;
        state_->queue.push_back({snapshot, nullptr});
    }
    state_->workAvailable.notify_one();
    return true;
}

bool
RideCacheBackgroundSaver::writeAndWait(
    const std::shared_ptr<
        const RideCacheSave::Snapshot> &snapshot,
    QString &error,
    qint64 timeoutMs)
{
    error.clear();
    if (!snapshot || !state_) {
        error = QStringLiteral(
            "Ride cache background saver is not running");
        return false;
    }
    if (timeoutMs < 0) {
        error = QStringLiteral(
            "Invalid ride cache save timeout");
        return false;
    }

    const std::shared_ptr<SaveCompletion> completion =
        std::make_shared<SaveCompletion>();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting) {
            error = QStringLiteral(
                "Ride cache background saver is not running");
            return false;
        }
        if (state_->workerId == std::this_thread::get_id()) {
            error = QStringLiteral(
                "Cannot wait for a ride cache save "
                "from the background saver thread");
            return false;
        }
        state_->queue.push_back({snapshot, completion});
    }
    state_->workAvailable.notify_one();

    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->completed.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [completion]() {
                return completion->done;
            })) {
        completion->abandoned = true;
        error = QStringLiteral(
            "Timed out waiting for ride cache save");
        return false;
    }
    error = completion->error;
    return completion->saved;
}

bool
RideCacheBackgroundSaver::drain(QString *error)
{
    if (!state_) return true;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->workerId == std::this_thread::get_id()) {
        if (error) {
            *error = QStringLiteral(
                "Cannot drain the ride cache save queue "
                "from the background saver thread");
        }
        return false;
    }
    state_->drained.wait(lock, [this]() {
        return state_->queue.empty() && !state_->active;
    });
    if (!state_->pendingError.isEmpty()) {
        if (error) *error = state_->pendingError;
        state_->pendingError.clear();
        return false;
    }
    if (error) error->clear();
    return true;
}

void
RideCacheBackgroundSaver::stop()
{
    if (!state_) return;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->workerId == std::this_thread::get_id()) {
            qCritical()
                << "Cannot stop the ride cache background saver "
                   "from its worker thread";
            return;
        }
        state_->accepting = false;
        state_->stopping = true;
    }
    state_->workAvailable.notify_one();
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}
