/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_PYTHONCHARTOWNER_H
#define GC_PYTHONCHARTOWNER_H

#include "PythonChartRunner.h"

#include <functional>
#include <memory>

class PythonChartOwner final
{
public:
    enum class Action { Ignore, Clear, Run };

    struct PreparedRun
    {
        Action action = Action::Ignore;
        PythonChartRunInput input;
    };

    struct Port
    {
        std::function<PreparedRun()> prepare;
        std::function<void(bool)> setBusy;
        std::function<void(PythonRunResult)> apply;
    };

    PythonChartOwner(
        Port port,
        PythonChartRunner::Execute execute,
        PythonChartRunState::Cancel cancel,
        PythonChartRunState::BeforeShutdownWait beforeShutdownWait = {});
    ~PythonChartOwner();

    PythonChartOwner(const PythonChartOwner &) = delete;
    PythonChartOwner &operator=(const PythonChartOwner &) = delete;

    void trigger();
    void cancelCurrent();
    void shutdown();
    bool active() const;

private:
    struct Lifetime
    {
        PythonChartOwner *owner = nullptr;
        quint64 generation = 0;
    };

    void completed(PythonRunResult result);
    void finishBusy(quint64 generation);

    Port port_;
    std::shared_ptr<Lifetime> lifetime_;
    PythonChartRunner runner_;
    bool shuttingDown_ = false;
    bool busy_ = false;
};

#endif
