/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PythonChartRunner.h"

#include <QtConcurrent>

#include <exception>
#include <utility>

PythonChartRunner::PythonChartRunner(
    Execute execute,
    PythonChartRunState::Cancel cancel,
    Complete complete,
    PythonChartRunState::BeforeShutdownWait beforeShutdownWait)
    : state_(std::move(beforeShutdownWait))
    , execute_(std::move(execute))
    , cancel_(std::move(cancel))
    , complete_(std::move(complete))
{
    QObject::connect(
        &watcher_, &QFutureWatcher<PythonRunResult>::finished,
        &watcher_, [this]() { currentFinished(); });
}

PythonChartRunner::~PythonChartRunner()
{
    shutdown();
}

PythonChartRunState::RequestDisposition
PythonChartRunner::request(PythonChartRunInput input)
{
    if (shuttingDown_) {
        return PythonChartRunState::RequestDisposition::Queued;
    }

    const PythonChartRunState::RequestDisposition disposition =
            state_.request(std::move(input), cancel_);
    if (disposition == PythonChartRunState::RequestDisposition::Started) {
        startCurrent();
    }
    return disposition;
}

void
PythonChartRunner::cancelCurrent()
{
    state_.cancelCurrent();
}

void
PythonChartRunner::cancelCurrentAndDropPending()
{
    state_.cancelCurrentAndDropPending();
}

void
PythonChartRunner::shutdown()
{
    if (shuttingDown_) return;

    shuttingDown_ = true;
    QObject::disconnect(&watcher_, nullptr, &watcher_, nullptr);
    state_.cancelAndWait();
    execute_ = Execute();
    cancel_ = PythonChartRunState::Cancel();
    complete_ = Complete();
}

bool
PythonChartRunner::active() const
{
    return state_.active();
}

void
PythonChartRunner::startCurrent()
{
    QFuture<PythonRunResult> future = QtConcurrent::run(
        execute_, state_.currentInput(), state_.cancellationFlag());
    state_.attach(future);
    watcher_.setFuture(future);
}

void
PythonChartRunner::currentFinished()
{
    if (shuttingDown_) return;

    PythonRunResult result;
    try {
        result = state_.result();
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what());
    } catch (...) {
        result.error = QStringLiteral("Unknown Python execution error.");
    }

    // GUI invalidation wins over a worker that completed as cancellation raced.
    if (state_.cancellationRequested()) result.cancelled = true;

    std::optional<PythonChartRunInput> pending = state_.finish();
    if (pending) {
        state_.request(std::move(*pending), cancel_);
        startCurrent();
        return;
    }

    Complete complete = complete_;
    if (complete) complete(std::move(result));
}
