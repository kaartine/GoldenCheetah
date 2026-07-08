/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_PYTHONCHARTRUNSTATE_H
#define GC_PYTHONCHARTRUNSTATE_H

#include "PythonEmbed.h"

#include <QFuture>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

struct PythonChartRunInput
{
    std::shared_ptr<const ScriptContext> context;
    QString source;
    quint64 token = 0;
};

class PythonChartRunState final
{
public:
    enum class RequestDisposition { Started, Queued };
    using Cancel = std::function<void(quint64)>;
    using BeforeShutdownWait = std::function<void()>;

    explicit PythonChartRunState(
        BeforeShutdownWait beforeShutdownWait = {});
    ~PythonChartRunState();

    RequestDisposition request(PythonChartRunInput input, Cancel cancel);
    const PythonChartRunInput &currentInput() const;
    std::shared_ptr<std::atomic_bool> cancellationFlag() const;
    void attach(QFuture<PythonRunResult> future);
    PythonRunResult result();
    std::optional<PythonChartRunInput> finish();
    void cancelCurrent();
    void cancelCurrentAndDropPending();
    void cancelAndWait();
    bool active() const;
    bool cancellationRequested() const;

private:
    void requestCurrentCancellation(bool repeat);
    void resetCurrent();

    bool active_ = false;
    bool cancellationIssued_ = false;
    PythonChartRunInput current_;
    std::optional<PythonChartRunInput> pending_;
    Cancel cancel_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    QFuture<PythonRunResult> future_;
    bool futureAttached_ = false;
    BeforeShutdownWait beforeShutdownWait_;
};

#endif
