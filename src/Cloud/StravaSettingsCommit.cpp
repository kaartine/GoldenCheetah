/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaSettingsCommit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

namespace StravaSettingsCommit {
namespace {

constexpr int credentialWorkerShutdownWaitMilliseconds = 100;
std::atomic<int> credentialWorkerLiveInstances{0};

struct PendingOperation
{
    std::mutex mutex;
    std::condition_variable condition;
    bool abandoned = false;
    bool complete = false;
    bool started = false;
};

struct QueuedOperation
{
    std::function<void()> run;
    std::function<void()> abandon;
};

class CredentialWorker final
{
public:
    class WorkerThread final : public QThread
    {
    public:
        explicit WorkerThread(CredentialWorker &owner)
            : owner_(owner)
        {
        }

    protected:
        void run() override
        {
            owner_.run();
        }

    private:
        CredentialWorker &owner_;
    };

    CredentialWorker()
        : worker_(*this)
    {
        ++credentialWorkerLiveInstances;
        worker_.start();
    }

    ~CredentialWorker()
    {
        Q_ASSERT(worker_.isFinished());
        --credentialWorkerLiveInstances;
    }

    bool shutdown()
    {
        if (isCurrentThread()) return false;
        std::call_once(shutdownOnce_, [this] {
            std::deque<QueuedOperation> abandoned;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
                abandoned.swap(operations_);
            }
            worker_.requestInterruption();
            for (QueuedOperation &operation : abandoned) {
                if (!operation.abandon) continue;
                try {
                    operation.abandon();
                } catch (...) {
                }
            }
            condition_.notify_all();
            worker_.wait(
                credentialWorkerShutdownWaitMilliseconds);
        });
        return worker_.isFinished();
    }

    bool submit(
        std::function<void()> operation,
        std::function<void()> abandon = {})
    {
        if (!operation) return false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return false;
            operations_.push_back({
                std::move(operation), std::move(abandon)
            });
        }
        condition_.notify_one();
        return true;
    }

    bool isCurrentThread() const
    {
        return QThread::currentThread() == &worker_;
    }

    bool isStopping() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    bool workerStopped() const
    {
        return worker_.isFinished();
    }

private:
    void run()
    {
        for (;;) {
            QueuedOperation operation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return stopping_ || !operations_.empty();
                });
                if (stopping_ && operations_.empty()) return;
                operation = std::move(operations_.front());
                operations_.pop_front();
            }
            try {
                operation.run();
            } catch (...) {
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedOperation> operations_;
    WorkerThread worker_;
    std::once_flag shutdownOnce_;
    bool stopping_ = false;
};

struct CredentialWorkerLifecycle
{
    std::mutex mutex;
    std::shared_ptr<CredentialWorker> worker;
    std::uint64_t generation = 1;
    bool startAllowed = true;
};

CredentialWorkerLifecycle &credentialWorkerLifecycle()
{
    static CredentialWorkerLifecycle lifecycle;
    return lifecycle;
}

std::shared_ptr<CredentialWorker> credentialWorkerForSubmission(
    std::uint64_t *generation = nullptr)
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    const std::lock_guard<std::mutex> lock(lifecycle.mutex);
    if (!lifecycle.startAllowed) return nullptr;
    if (!lifecycle.worker) {
        lifecycle.worker = std::shared_ptr<CredentialWorker>(
            new CredentialWorker,
            [](CredentialWorker *worker) {
                // Deleting a running QThread is unsafe. A wedged native
                // backend remains process-owned, while every stopped
                // generation is reclaimed normally.
                if (worker && worker->workerStopped()) delete worker;
            });
    }
    if (generation) *generation = lifecycle.generation;
    return lifecycle.worker;
}

bool credentialWorkerGenerationCurrent(std::uint64_t generation)
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    const std::lock_guard<std::mutex> lock(lifecycle.mutex);
    return lifecycle.startAllowed
        && lifecycle.generation == generation;
}

bool cancellationRequested(const CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

} // namespace

DispatchResult runOnCredentialThread(
    const std::function<void()> &operation,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!operation || timeoutMs <= 0
        || cancellationRequested(cancelled)) {
        return {DispatchStatus::NotStarted};
    }

    const std::shared_ptr<CredentialWorker> worker =
        credentialWorkerForSubmission();
    if (!worker || worker->isStopping())
        return {DispatchStatus::NotStarted};
    if (worker->isCurrentThread()) {
        try {
            operation();
            return {DispatchStatus::Completed};
        } catch (...) {
            return {DispatchStatus::Completed};
        }
    }

    const auto pending = std::make_shared<PendingOperation>();
    const bool queued = worker->submit(
        [pending, operation] {
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                if (pending->abandoned) {
                    pending->complete = true;
                    pending->condition.notify_all();
                    return;
                }
                pending->started = true;
            }

            try {
                operation();
            } catch (...) {
            }
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                pending->complete = true;
            }
            pending->condition.notify_all();
        },
        [pending] {
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                pending->abandoned = true;
                pending->complete = true;
            }
            pending->condition.notify_all();
        });
    if (!queued) return {DispatchStatus::NotStarted};

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (pending->complete) {
            return {pending->abandoned
                    ? DispatchStatus::NotStarted
                    : DispatchStatus::Completed};
        }

        const bool expired =
            std::chrono::steady_clock::now() >= deadline;
        lock.unlock();
        const bool cancelledNow = cancellationRequested(cancelled);
        lock.lock();
        if (pending->complete) {
            return {pending->abandoned
                    ? DispatchStatus::NotStarted
                    : DispatchStatus::Completed};
        }
        if (expired || cancelledNow) {
            if (!pending->started) {
                pending->abandoned = true;
                return {DispatchStatus::NotStarted};
            }
            // The durable transaction identity remains the authority while
            // the already-started closure owns and tracks the late commit.
            return {DispatchStatus::Pending};
        }

        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        const auto waitSlice =
            std::min(remaining, std::chrono::milliseconds(10));
        QCoreApplication *application =
            QCoreApplication::instance();
        if (application
            && QThread::currentThread()
                == application->thread()) {
            lock.unlock();
            QEventLoop eventLoop;
            QTimer wakeup;
            wakeup.setSingleShot(true);
            QObject::connect(
                &wakeup, &QTimer::timeout,
                &eventLoop, &QEventLoop::quit);
            wakeup.start(static_cast<int>(
                std::max<std::int64_t>(
                    1, waitSlice.count())));
            eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
        } else {
            pending->condition.wait_for(lock, waitSlice);
        }
    }
}

bool runOnCredentialThreadAsync(
    const std::function<void()> &operation,
    const std::function<void()> &completed)
{
    std::uint64_t generation = 0;
    const std::shared_ptr<CredentialWorker> worker =
        credentialWorkerForSubmission(&generation);
    if (!operation || !worker || worker->isStopping()) return false;
    return worker->submit([operation, completed, generation] {
        try {
            operation();
        } catch (...) {
        }
        if (completed
            && credentialWorkerGenerationCurrent(generation)
            && !credentialThreadShutdownRequested()) {
            try {
                completed();
            } catch (...) {
            }
        }
    });
}

bool runOnCredentialThreadAsync(
    const std::function<void()> &operation,
    QObject *completionContext,
    const std::function<void()> &completed)
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!operation || !completionContext || !application
        || completionContext->thread() != application->thread()) {
        return false;
    }

    struct CompletionLifetime
    {
        std::mutex mutex;
        bool alive = true;
    };
    const auto lifetime = std::make_shared<CompletionLifetime>();
    QObject::connect(
        completionContext, &QObject::destroyed,
        application,
        [lifetime] {
            const std::lock_guard<std::mutex> lock(lifetime->mutex);
            lifetime->alive = false;
        },
        Qt::DirectConnection);

    std::uint64_t completionGeneration = 0;
    const std::shared_ptr<CredentialWorker> worker =
        credentialWorkerForSubmission(&completionGeneration);
    if (!worker || worker->isStopping()) return false;
    return worker->submit(
        [operation, application, lifetime, completed,
         completionGeneration] {
            try {
                operation();
            } catch (...) {
            }
            if (!completed
                || !credentialWorkerGenerationCurrent(
                    completionGeneration)
                || credentialThreadShutdownRequested()) {
                return;
            }
            QMetaObject::invokeMethod(
                application,
                [lifetime, completed, completionGeneration] {
                    if (!credentialWorkerGenerationCurrent(
                            completionGeneration)) {
                        return;
                    }
                    {
                        const std::lock_guard<std::mutex> lock(
                            lifetime->mutex);
                        if (!lifetime->alive) return;
                    }
                    completed();
                },
                Qt::QueuedConnection);
        });
}

bool credentialThreadShutdownRequested()
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    bool stopping = false;
    {
        const std::lock_guard<std::mutex> lock(lifecycle.mutex);
        stopping = !lifecycle.startAllowed
            || (lifecycle.worker && lifecycle.worker->isStopping());
    }
    return stopping
        || QThread::currentThread()->isInterruptionRequested();
}

bool shutdownCredentialThread()
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    std::shared_ptr<CredentialWorker> worker;
    {
        const std::lock_guard<std::mutex> lock(lifecycle.mutex);
        if (lifecycle.startAllowed) {
            lifecycle.startAllowed = false;
            ++lifecycle.generation;
        }
        worker = lifecycle.worker;
    }
    return !worker || worker->shutdown();
}

bool restartCredentialThread()
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    const std::lock_guard<std::mutex> lock(lifecycle.mutex);
    if (lifecycle.startAllowed) return true;
    if (lifecycle.worker && !lifecycle.worker->workerStopped())
        return false;
    lifecycle.worker = nullptr;
    lifecycle.startAllowed = true;
    return true;
}

bool credentialThreadStopped()
{
    CredentialWorkerLifecycle &lifecycle = credentialWorkerLifecycle();
    const std::lock_guard<std::mutex> lock(lifecycle.mutex);
    return !lifecycle.startAllowed
        && (!lifecycle.worker || lifecycle.worker->workerStopped());
}

#ifdef GC_CREDENTIAL_TEST_HOOKS
int credentialWorkerLiveInstancesForTest()
{
    return credentialWorkerLiveInstances.load();
}
#endif

} // namespace StravaSettingsCommit
