/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameSessionState_h
#define _GC_WorkoutGameSessionState_h

class WorkoutGameSessionState
{
public:
    void workoutSelected();
    void started();
    void stopped();

    bool acceptsPositionUpdate(bool trainingRunning) const;
    bool holdsStoppedFrame() const;

private:
    bool sessionActive = false;
    bool stoppedFrameHeld = false;
};

#endif
