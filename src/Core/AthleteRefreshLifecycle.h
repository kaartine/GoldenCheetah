/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_ATHLETEREFRESHLIFECYCLE_H
#define GC_ATHLETEREFRESHLIFECYCLE_H

#include <functional>

class QThread;

// Owner-thread coordinator for work that reads an athlete's mutable runtime
// configuration.  Participants must synchronously join their workers from
// quiesce; resume is called only after a completed configuration transaction.
class AthleteRefreshLifecycle final
{
public:
    struct Participant {
        const void *identity = nullptr;
        std::function<void()> quiesce;
        std::function<void()> resume;
    };

    AthleteRefreshLifecycle();

    bool registerParticipant(Participant participant);
    bool unregisterParticipant(const void *identity);

    bool beginConfigTransition();
    bool finishConfigTransition();
    bool beginShutdown();

    bool admitsWork() const;
    bool transitionActive() const;
    bool shutdownStarted() const;

private:
    enum class State { Open, ConfigTransition, Shutdown };

    bool onOwnerThread() const;

    QThread *ownerThread_ = nullptr;
    State state_ = State::Open;
    unsigned int transitionDepth_ = 0;
    Participant participant_;
};

#endif // GC_ATHLETEREFRESHLIFECYCLE_H
