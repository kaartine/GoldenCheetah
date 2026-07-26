/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_OAUTH_TOKEN_REPLY_CONTROLLER_H
#define GC_OAUTH_TOKEN_REPLY_CONTROLLER_H

#include <QPointer>
#include <QTimer>

class QNetworkReply;
class QObject;

class OAuthTokenReplyController final
{
public:
    enum class Completion
    {
        Finished,
        TimedOut,
        Cancelled,
        Untracked
    };

    explicit OAuthTokenReplyController(
        QObject *lifetimeContext);
    ~OAuthTokenReplyController();

    bool start(QNetworkReply *reply, int timeoutMs);
    Completion complete(QNetworkReply *reply);
    void cancel();

private:
    static constexpr int MaximumTimeoutMs = 5 * 60 * 1000;

    QPointer<QNetworkReply> reply_;
    QTimer timer_;
    Completion pendingCompletion_ = Completion::Finished;
};

#endif
