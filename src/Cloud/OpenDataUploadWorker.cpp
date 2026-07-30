/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataUploadWorker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <utility>
#include <vector>

namespace {

class CompletionPoller final : public QObject
{
public:
    CompletionPoller(
        std::function<bool()> poll,
        QObject *parent)
        : QObject(parent)
        , poll_(std::move(poll))
    {
        timer_.setInterval(10);
        connect(
            &timer_, &QTimer::timeout,
            this,
            [this]() {
                if (poll_ && poll_()) return;
                timer_.stop();
                deleteLater();
            });
        timer_.start();
    }

private:
    std::function<bool()> poll_;
    QTimer timer_;
};

} // namespace

struct OpenDataUploadWorker::State
{
    struct ProgressEvent
    {
        int step = 0;
        int lastStep = 0;
        QString message;
    };

    State(
        std::shared_ptr<const OpenDataExport::Request> capturedRequest,
        OpenDataExport::UploadTask capturedTask)
        : request(std::move(capturedRequest))
        , task(std::move(capturedTask))
    {
    }

    std::shared_ptr<const OpenDataExport::Request> request;
    OpenDataExport::UploadTask task;
    std::atomic<bool> cancellationRequested{false};
    std::mutex mutex;
    std::condition_variable finishedCondition;
    OpenDataExport::UploadResult result;
    std::vector<ProgressEvent> progressEvents;
    bool started = false;
    bool finished = false;
    bool delivered = false;
};

OpenDataUploadWorker::OpenDataUploadWorker(
    std::shared_ptr<const OpenDataExport::Request> request,
    OpenDataExport::UploadTask task,
    QObject *parent)
    : QObject(parent)
    , state_(std::make_shared<State>(
          std::move(request), std::move(task)))
{
}

OpenDataUploadWorker::~OpenDataUploadWorker()
{
    requestCancellation();
    if (currentThreadIsWorker()) {
        joinThread();
        return;
    }
    if (waitForCompletion(5000, false)) return;

    qWarning() << "OpenData upload did not stop within five seconds";
    joinThread();
}

void OpenDataUploadWorker::start()
{
    const std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->started) return;
    state->started = true;
    state->finished = false;
    state->delivered = false;
    state->progressEvents.clear();

    QPointer<OpenDataUploadWorker> guard(this);
    QObject *pollerOwner = QCoreApplication::instance();
    new CompletionPoller(
        [guard]() mutable {
            if (!guard) return false;
            guard->deliverCompletion();
            if (!guard) return false;
            const std::shared_ptr<State> current =
                guard->state_;
            std::lock_guard<std::mutex> stateLock(
                current->mutex);
            return !current->delivered;
        },
        pollerOwner);

    try {
        {
            std::lock_guard<std::mutex> joinLock(joinMutex_);
            thread_ = std::thread([state]() {
                execute(state);
            });
        }
    } catch (...) {
        state->finished = true;
        state->result = OpenDataExport::UploadResult::failed(
            QStringLiteral("Cannot start OpenData upload"));
    }
}

bool OpenDataUploadWorker::wait(unsigned long timeoutMs)
{
    return waitForCompletion(timeoutMs, true);
}

bool OpenDataUploadWorker::waitForCompletion(
    unsigned long timeoutMs,
    bool deliver)
{
    if (currentThreadIsWorker()) return false;

    const std::shared_ptr<State> state = state_;
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->started) return true;
    if (!state->finished) {
        const bool completed = state->finishedCondition.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [state]() {
                return state->finished;
            });
        if (!completed) return false;
    }
    lock.unlock();
    joinThread();
    if (deliver
        && QThread::currentThread() == thread()) {
        deliverCompletion();
    }
    return true;
}

bool OpenDataUploadWorker::cancelAndWait(unsigned long timeoutMs)
{
    requestCancellation();
    return wait(timeoutMs);
}

void OpenDataUploadWorker::cancelAndJoin()
{
    requestCancellation();
    if (currentThreadIsWorker()) return;
    if (!waitForCompletion(5000, true)) {
        qWarning() << "Waiting for OpenData upload shutdown";
        joinThread();
        if (QThread::currentThread() == thread())
            deliverCompletion();
    }
}

void OpenDataUploadWorker::requestCancellation()
{
    const std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished) return;
    state->cancellationRequested.store(
        true, std::memory_order_release);
}

bool OpenDataUploadWorker::isRunning() const
{
    const std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->started && !state->finished;
}

void OpenDataUploadWorker::startManaged()
{
    if (managed_ || isRunning()) return;
    managed_ = true;
    connect(
        this, &OpenDataUploadWorker::finished,
        this,
        [this]() {
            wait();
            deleteLater();
        },
        Qt::QueuedConnection);
    start();
}

void OpenDataUploadWorker::joinThread()
{
    std::lock_guard<std::mutex> joinLock(joinMutex_);
    if (!thread_.joinable()) return;
    if (std::this_thread::get_id() == thread_.get_id()) {
        thread_.detach();
    } else {
        thread_.join();
    }
}

bool OpenDataUploadWorker::currentThreadIsWorker() const
{
    std::lock_guard<std::mutex> joinLock(joinMutex_);
    return thread_.joinable()
        && std::this_thread::get_id() == thread_.get_id();
}

void OpenDataUploadWorker::deliverCompletion()
{
    QPointer<OpenDataUploadWorker> guard(this);
    const std::shared_ptr<State> state = state_;
    OpenDataExport::UploadResult result;
    std::vector<State::ProgressEvent> progressEvents;
    bool shouldDeliver = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->finished && !state->delivered) {
            state->delivered = true;
            result = state->result;
            progressEvents =
                std::move(state->progressEvents);
            shouldDeliver = true;
        }
    }
    if (!shouldDeliver) return;

    joinThread();

    for (const State::ProgressEvent &event : progressEvents) {
        emit progress(event.step, event.lastStep, event.message);
        if (!guard) return;
    }

    if (!state->cancellationRequested.load(
            std::memory_order_acquire)
        && result.status
            != OpenDataExport::UploadResult::Status::Cancelled) {
        if (result.status
            == OpenDataExport::UploadResult::Status::Succeeded) {
            emit succeeded(
                state->request->cyclist,
                state->request->rideCount,
                state->request->formatVersion);
        } else {
            emit failed(
                result.error.isEmpty()
                    ? QStringLiteral("OpenData upload failed")
                    : result.error);
        }
        if (!guard) return;
    }
    emit finished();
}

void OpenDataUploadWorker::execute(
    const std::shared_ptr<State> &state)
{
    OpenDataExport::UploadResult result;
    if (!state->request || !state->task) {
        result = OpenDataExport::UploadResult::failed(
            QStringLiteral("Invalid OpenData upload request"));
    } else {
        const OpenDataExport::CancellationCheck cancelled = [state]() {
            return state->cancellationRequested.load(
                std::memory_order_acquire);
        };
        const OpenDataExport::ProgressCallback reportProgress =
            [state](int step, int lastStep, const QString &message) {
                if (!state->cancellationRequested.load(
                        std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->progressEvents.push_back(
                        {step, lastStep, message});
                }
            };

        try {
            result = state->task(
                *state->request, cancelled, reportProgress);
        } catch (const std::exception &) {
            result = OpenDataExport::UploadResult::failed(
                QStringLiteral("Unexpected OpenData upload failure"));
        } catch (...) {
            result = OpenDataExport::UploadResult::failed(
                QStringLiteral("Unexpected OpenData upload failure"));
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = std::move(result);
        state->finished = true;
    }
    state->finishedCondition.notify_all();
}
