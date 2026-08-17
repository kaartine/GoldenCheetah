/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingCommandRouter_h
#define _GC_TrainingCommandRouter_h

#include <QtCore/qnamespace.h>

enum class TrainingCommand {
    None,
    ToggleStartPause,
    RequestStop,
    ShiftUp,
    ShiftDown
};

class TrainingCommandRouter
{
public:
    static TrainingCommand commandForKey(
            int key,
            Qt::KeyboardModifiers modifiers,
            bool autoRepeat);
};

#endif
