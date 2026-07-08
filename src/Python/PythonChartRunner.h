/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_PYTHONCHARTRUNNER_H
#define GC_PYTHONCHARTRUNNER_H

#include "PythonChartRunState.h"

#include <QFutureWatcher>

#include <functional>

class PythonChartRunner final
{
public:
    using Execute = std::function<PythonRunResult(
        PythonChartRunInput, std::shared_ptr<std::atomic_bool>)>;
    using Complete = std::function<void(PythonRunResult)>;

    PythonChartRunner(
        Execute execute,
        PythonChartRunState::Cancel cancel,
        Complete complete,
        PythonChartRunState::BeforeShutdownWait beforeShutdownWait = {});
    ~PythonChartRunner();

    PythonChartRunner(const PythonChartRunner &) = delete;
    PythonChartRunner &operator=(const PythonChartRunner &) = delete;

    PythonChartRunState::RequestDisposition request(
        PythonChartRunInput input);
    void cancelCurrent();
    void cancelCurrentAndDropPending();
    void shutdown();
    bool active() const;

private:
    void startCurrent();
    void currentFinished();

    PythonChartRunState state_;
    QFutureWatcher<PythonRunResult> watcher_;
    Execute execute_;
    PythonChartRunState::Cancel cancel_;
    Complete complete_;
    bool shuttingDown_ = false;
};

#endif
