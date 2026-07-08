/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PythonChartOwner.h"

#include <utility>

PythonChartOwner::PythonChartOwner(
    Port port,
    PythonChartRunner::Execute execute,
    PythonChartRunState::Cancel cancel,
    PythonChartRunState::BeforeShutdownWait beforeShutdownWait)
    : port_(std::move(port))
    , lifetime_(std::make_shared<Lifetime>())
    , runner_(
        std::move(execute),
        std::move(cancel),
        [this](PythonRunResult result) {
            completed(std::move(result));
        },
        std::move(beforeShutdownWait))
{
    lifetime_->owner = this;
}

PythonChartOwner::~PythonChartOwner()
{
    shutdown();
}

void
PythonChartOwner::trigger()
{
    if (shuttingDown_ || !port_.prepare) return;

    PreparedRun prepared = port_.prepare();
    switch (prepared.action) {
    case Action::Ignore:
        return;
    case Action::Clear:
        runner_.cancelCurrentAndDropPending();
        return;
    case Action::Run:
        if (runner_.request(std::move(prepared.input))
            == PythonChartRunState::RequestDisposition::Started) {
            ++lifetime_->generation;
            if (lifetime_->generation == 0) ++lifetime_->generation;

            if (!busy_) {
                busy_ = true;
                const auto setBusy = port_.setBusy;
                if (setBusy) setBusy(true);
            }
        }
        return;
    }
}

void
PythonChartOwner::cancelCurrent()
{
    if (!shuttingDown_) runner_.cancelCurrent();
}

void
PythonChartOwner::shutdown()
{
    if (shuttingDown_) return;

    shuttingDown_ = true;
    lifetime_->owner = nullptr;
    runner_.shutdown();
    port_ = Port();
    busy_ = false;
}

bool
PythonChartOwner::active() const
{
    return runner_.active();
}

void
PythonChartOwner::completed(PythonRunResult result)
{
    if (shuttingDown_) return;

    const std::shared_ptr<Lifetime> lifetime = lifetime_;
    const quint64 generation = lifetime->generation;
    const auto apply = port_.apply;
    if (!result.cancelled && apply) {
        apply(std::move(result));
    }

    PythonChartOwner *owner = lifetime->owner;
    if (owner && lifetime->generation == generation) {
        owner->finishBusy(generation);
    }
}

void
PythonChartOwner::finishBusy(quint64 generation)
{
    if (shuttingDown_ || !busy_
        || lifetime_->generation != generation) {
        return;
    }

    busy_ = false;
    const auto setBusy = port_.setBusy;
    if (setBusy) setBusy(false);
}
