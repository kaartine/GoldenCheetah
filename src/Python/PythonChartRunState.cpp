/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PythonChartRunState.h"

#include <utility>

PythonChartRunState::PythonChartRunState(
    BeforeShutdownWait beforeShutdownWait)
    : beforeShutdownWait_(std::move(beforeShutdownWait))
{
}

PythonChartRunState::~PythonChartRunState()
{
    cancelAndWait();
}

PythonChartRunState::RequestDisposition
PythonChartRunState::request(PythonChartRunInput input, Cancel cancel)
{
    if (active_) {
        pending_ = std::move(input);
        requestCurrentCancellation(false);
        return RequestDisposition::Queued;
    }

    active_ = true;
    cancellationIssued_ = false;
    current_ = std::move(input);
    pending_.reset();
    cancel_ = std::move(cancel);
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    future_ = QFuture<PythonRunResult>();
    futureAttached_ = false;
    return RequestDisposition::Started;
}

const PythonChartRunInput &
PythonChartRunState::currentInput() const
{
    return current_;
}

std::shared_ptr<std::atomic_bool>
PythonChartRunState::cancellationFlag() const
{
    return cancellation_;
}

void
PythonChartRunState::attach(QFuture<PythonRunResult> future)
{
    future_ = std::move(future);
    futureAttached_ = true;
}

PythonRunResult
PythonChartRunState::result()
{
    if (!futureAttached_) return PythonRunResult();
    future_.waitForFinished();
    return future_.result();
}

std::optional<PythonChartRunInput>
PythonChartRunState::finish()
{
    if (futureAttached_) future_.waitForFinished();
    std::optional<PythonChartRunInput> pending = std::move(pending_);
    resetCurrent();
    return pending;
}

void
PythonChartRunState::cancelCurrent()
{
    requestCurrentCancellation(true);
}

void
PythonChartRunState::cancelCurrentAndDropPending()
{
    pending_.reset();
    requestCurrentCancellation(true);
}

void
PythonChartRunState::cancelAndWait()
{
    if (!active_) return;

    pending_.reset();
    requestCurrentCancellation(true);
    if (futureAttached_) {
        const BeforeShutdownWait beforeWait = beforeShutdownWait_;
        if (beforeWait) beforeWait();
        future_.waitForFinished();
    }
    resetCurrent();
}

bool
PythonChartRunState::active() const
{
    return active_;
}

bool
PythonChartRunState::cancellationRequested() const
{
    return cancellationIssued_;
}

void
PythonChartRunState::requestCurrentCancellation(bool repeat)
{
    if (!active_) return;

    if (!cancellationIssued_) {
        cancellationIssued_ = true;
        if (cancellation_) {
            cancellation_->store(true, std::memory_order_release);
        }
    } else if (!repeat) {
        return;
    }

    if (cancel_) cancel_(current_.token);
}

void
PythonChartRunState::resetCurrent()
{
    active_ = false;
    cancellationIssued_ = false;
    current_ = PythonChartRunInput();
    cancel_ = Cancel();
    cancellation_.reset();
    future_ = QFuture<PythonRunResult>();
    futureAttached_ = false;
}
