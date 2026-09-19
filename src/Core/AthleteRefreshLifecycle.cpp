/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AthleteRefreshLifecycle.h"

#include <QThread>

#include <utility>

AthleteRefreshLifecycle::AthleteRefreshLifecycle()
    : ownerThread_(QThread::currentThread())
{
}

bool AthleteRefreshLifecycle::onOwnerThread() const
{
    return QThread::currentThread() == ownerThread_;
}

bool AthleteRefreshLifecycle::registerParticipant(
    Participant participant)
{
    if (!onOwnerThread() || state_ != State::Open
        || !participant.identity || !participant.quiesce
        || participant_.identity) {
        return false;
    }
    participant_ = std::move(participant);
    return true;
}

bool AthleteRefreshLifecycle::unregisterParticipant(
    const void *identity)
{
    if (!onOwnerThread() || !identity
        || participant_.identity != identity) {
        return false;
    }
    participant_ = {};
    return true;
}

bool AthleteRefreshLifecycle::beginConfigTransition()
{
    if (!onOwnerThread() || state_ == State::Shutdown) return false;
    if (state_ == State::ConfigTransition) {
        ++transitionDepth_;
        return true;
    }

    state_ = State::ConfigTransition;
    transitionDepth_ = 1;
    if (participant_.quiesce) participant_.quiesce();
    return true;
}

bool AthleteRefreshLifecycle::finishConfigTransition()
{
    if (!onOwnerThread() || state_ != State::ConfigTransition)
        return false;

    if (--transitionDepth_ == 0) {
        state_ = State::Open;
        if (participant_.resume) participant_.resume();
    }
    return true;
}

bool AthleteRefreshLifecycle::beginShutdown()
{
    if (!onOwnerThread()) return false;
    if (state_ == State::Shutdown) return true;

    const bool needsQuiesce = state_ == State::Open;
    state_ = State::Shutdown;
    transitionDepth_ = 0;
    if (needsQuiesce && participant_.quiesce)
        participant_.quiesce();
    return true;
}

bool AthleteRefreshLifecycle::admitsWork() const
{
    return onOwnerThread() && state_ == State::Open;
}

bool AthleteRefreshLifecycle::transitionActive() const
{
    return state_ == State::ConfigTransition;
}

bool AthleteRefreshLifecycle::shutdownStarted() const
{
    return state_ == State::Shutdown;
}
