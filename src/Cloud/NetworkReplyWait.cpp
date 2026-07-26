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
#include <QPointer>
#include <QTimer>

NetworkReplyWaitResult
waitForNetworkReply(
        QNetworkReply *reply,
        int timeoutMs,
        const std::function<bool()> &interrupted)
{
    QPointer<QNetworkReply> guardedReply(reply);
    if (!guardedReply)
        return NetworkReplyWaitResult::Finished;

    const auto interruptionRequested = [&interrupted]() {
        if (!interrupted) return false;
        try {
            return interrupted();
        } catch (...) {
            return true;
        }
    };
    if (interruptionRequested()) {
        guardedReply->abort();
        return NetworkReplyWaitResult::Interrupted;
    }
    if (guardedReply->isFinished())
        return NetworkReplyWaitResult::Finished;

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QTimer interruptionTimer;
    interruptionTimer.setInterval(10);
    bool timedOut = false;
    bool wasInterrupted = false;
    bool wasDestroyed = false;

    QObject::connect(
        guardedReply, &QNetworkReply::finished,
        &loop, &QEventLoop::quit);
    QObject::connect(
        guardedReply, &QObject::destroyed,
        &loop, [&]() {
            wasDestroyed = true;
            loop.quit();
        });
    QObject::connect(
        &timeoutTimer, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            if (guardedReply)
                guardedReply->abort();
            loop.quit();
        });
    QObject::connect(
        &interruptionTimer, &QTimer::timeout, &loop, [&]() {
            if (!interruptionRequested()) return;
            wasInterrupted = true;
            if (guardedReply)
                guardedReply->abort();
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
    if (wasDestroyed)
        return NetworkReplyWaitResult::Destroyed;
    return NetworkReplyWaitResult::Finished;
}
