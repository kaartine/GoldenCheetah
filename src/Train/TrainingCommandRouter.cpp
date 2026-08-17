/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainingCommandRouter.h"

TrainingCommand TrainingCommandRouter::commandForKey(
        int key,
        Qt::KeyboardModifiers modifiers,
        bool autoRepeat)
{
    constexpr Qt::KeyboardModifiers reservedModifiers =
            Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (autoRepeat || (modifiers & reservedModifiers)) {
        return TrainingCommand::None;
    }

    switch (key) {
    case Qt::Key_Space:
        return TrainingCommand::ToggleStartPause;
    case Qt::Key_Escape:
        return TrainingCommand::RequestStop;
    case Qt::Key_Up:
    case Qt::Key_W:
        return TrainingCommand::ShiftUp;
    case Qt::Key_Down:
    case Qt::Key_S:
        return TrainingCommand::ShiftDown;
    default:
        return TrainingCommand::None;
    }
}
