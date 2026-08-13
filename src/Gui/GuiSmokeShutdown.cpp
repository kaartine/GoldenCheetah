/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "GuiSmokeShutdown.h"

#include <QObject>

#include <memory>

namespace GuiSmokeShutdown {

namespace {

struct CompletionState
{
    bool exited = false;
    std::function<void(int)> exitApplication;
};

}

bool complete(
    QObject *window,
    const std::function<bool()> &closeWindow,
    const std::function<void(int)> &exitApplication,
    int completedCode,
    int closeFailureCode)
{
    if (!exitApplication) return false;
    if (!window || !closeWindow) {
        exitApplication(closeFailureCode);
        return false;
    }

    auto state = std::make_shared<CompletionState>();
    state->exitApplication = exitApplication;
    const auto exitOnce = [state](int code) {
        if (state->exited) return;
        state->exited = true;
        state->exitApplication(code);
    };

    QObject::connect(
        window,
        &QObject::destroyed,
        window,
        [exitOnce, completedCode]() { exitOnce(completedCode); });

    if (closeWindow()) return true;

    exitOnce(closeFailureCode);
    return false;
}

}
