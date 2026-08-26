/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameAudio_h
#define _GC_WorkoutGameAudio_h

#include "WorkoutGameAudioEvents.h"

#include <memory>

class WorkoutGameAudioFeedback
{
public:
    WorkoutGameAudioFeedback();
    ~WorkoutGameAudioFeedback();

    WorkoutGameAudioFeedback(const WorkoutGameAudioFeedback &) = delete;
    WorkoutGameAudioFeedback &operator=(
            const WorkoutGameAudioFeedback &) = delete;

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled; }
    void reset();
    void update(const WorkoutGameAudioEventJournal &journal);

private:
    class Effects;

    WorkoutGameAudioCueTracker tracker;
    std::unique_ptr<Effects> effects;
    bool enabled = false;
};

#endif
