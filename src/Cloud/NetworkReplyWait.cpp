/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "NetworkReplyWait.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QTimer>

NetworkReplyWaitResult
waitForNetworkReply(
        QNetworkReply *reply,
        int timeoutMs,
        const std::function<bool()> &interrupted)
{
    if (!reply) return NetworkReplyWaitResult::Finished;

    const auto interruptionRequested = [&interrupted]() {
        return interrupted && interrupted();
    };
    if (interruptionRequested()) {
        reply->abort();
        return NetworkReplyWaitResult::Interrupted;
    }
    if (reply->isFinished())
        return NetworkReplyWaitResult::Finished;

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QTimer interruptionTimer;
    interruptionTimer.setInterval(10);
    bool timedOut = false;
    bool wasInterrupted = false;

    QObject::connect(
        reply, &QNetworkReply::finished,
        &loop, &QEventLoop::quit);
    QObject::connect(
        &timeoutTimer, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            reply->abort();
            loop.quit();
        });
    QObject::connect(
        &interruptionTimer, &QTimer::timeout, &loop, [&]() {
            if (!interruptionRequested()) return;
            wasInterrupted = true;
            reply->abort();
            loop.quit();
        });

    if (timeoutMs >= 0)
        timeoutTimer.start(timeoutMs);
    if (interrupted)
        interruptionTimer.start();
    loop.exec();

    if (wasInterrupted || interruptionRequested())
        return NetworkReplyWaitResult::Interrupted;
    if (timedOut)
        return NetworkReplyWaitResult::TimedOut;
    return NetworkReplyWaitResult::Finished;
}
