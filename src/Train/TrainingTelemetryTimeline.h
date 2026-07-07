/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingTelemetryTimeline_h
#define _GC_TrainingTelemetryTimeline_h

#include <QtGlobal>

#include <array>
#include <limits>

namespace TrainingTelemetryTimeline
{

enum class Channel {
    Rr,
    Position,
    CoreTemperature,
    Vo2
};

struct SessionState {
    bool running = false;
    bool recording = false;
    bool paused = false;
    bool calibrating = false;
    qint64 accumulatedMsecs = 0;
    qint64 currentSegmentMsecs = 0;
};

struct SampleTime {
    bool accepted = false;
    qint64 msecs = 0;
};

class Timeline
{
public:
    Timeline() { reset(); }

    void reset()
    {
        lastMsecs.fill(-1);
    }

    SampleTime timestamp(Channel channel, const SessionState &state)
    {
        if (!state.running || !state.recording ||
                state.paused || state.calibrating) {
            return {};
        }

        const int index = channelIndex(channel);
        if (index < 0) return {};

        const qint64 accumulated = state.accumulatedMsecs > 0
                ? state.accumulatedMsecs : 0;
        const qint64 currentSegment = state.currentSegmentMsecs > 0
                ? state.currentSegmentMsecs : 0;
        const qint64 maximum = std::numeric_limits<qint64>::max();
        const qint64 raw = accumulated > maximum - currentSegment
                ? maximum : accumulated + currentSegment;

        qint64 next = raw;
        if (lastMsecs[index] >= raw) {
            if (lastMsecs[index] == maximum) return {};
            next = lastMsecs[index] + 1;
        }

        lastMsecs[index] = next;
        return {true, next};
    }

private:
    static int channelIndex(Channel channel)
    {
        switch (channel) {
        case Channel::Rr:
            return 0;
        case Channel::Position:
            return 1;
        case Channel::CoreTemperature:
            return 2;
        case Channel::Vo2:
            return 3;
        }
        return -1;
    }

    std::array<qint64, 4> lastMsecs;
};

}

#endif
