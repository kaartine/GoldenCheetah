/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "OAuthTokenReplyController.h"

#include <QNetworkReply>
#include <QObject>

OAuthTokenReplyController::OAuthTokenReplyController(
    QObject *lifetimeContext)
{
    timer_.setSingleShot(true);
    QObject::connect(
        &timer_, &QTimer::timeout,
        lifetimeContext,
        [this] {
            QNetworkReply *reply = reply_.data();
            if (!reply || reply->isFinished()) return;

            pendingCompletion_ = Completion::TimedOut;
            reply->abort();
        });
}

OAuthTokenReplyController::~OAuthTokenReplyController()
{
    timer_.stop();
    reply_.clear();
}

bool OAuthTokenReplyController::start(
    QNetworkReply *reply,
    int timeoutMs)
{
    if (!reply
        || reply_
        || timeoutMs <= 0
        || timeoutMs > MaximumTimeoutMs) {
        return false;
    }

    timer_.stop();
    pendingCompletion_ = Completion::Finished;
    reply_ = reply;
    if (!reply->isFinished()) timer_.start(timeoutMs);
    return true;
}

OAuthTokenReplyController::Completion
OAuthTokenReplyController::complete(QNetworkReply *reply)
{
    if (!reply) return Completion::Untracked;

    reply->deleteLater();
    if (reply_.data() != reply) return Completion::Untracked;

    timer_.stop();
    const Completion completion = pendingCompletion_;
    reply_.clear();
    pendingCompletion_ = Completion::Finished;
    return completion;
}

void OAuthTokenReplyController::cancel()
{
    QNetworkReply *reply = reply_.data();
    if (!reply) return;

    timer_.stop();
    if (pendingCompletion_ == Completion::Finished) {
        pendingCompletion_ = Completion::Cancelled;
    }
    if (!reply->isFinished()) reply->abort();
}
