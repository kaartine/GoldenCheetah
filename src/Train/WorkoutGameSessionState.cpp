/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameSessionState.h"

void WorkoutGameSessionState::workoutSelected()
{
    sessionActive = false;
    stoppedFrameHeld = false;
}

void WorkoutGameSessionState::started()
{
    sessionActive = true;
    stoppedFrameHeld = false;
}

void WorkoutGameSessionState::stopped()
{
    sessionActive = false;
    stoppedFrameHeld = true;
}

bool WorkoutGameSessionState::acceptsPositionUpdate(
        bool trainingRunning) const
{
    if (sessionActive && !trainingRunning) return false;
    return trainingRunning || !stoppedFrameHeld;
}

bool WorkoutGameSessionState::holdsStoppedFrame() const
{
    return stoppedFrameHeld;
}
