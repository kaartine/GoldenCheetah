/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaCredentialPublisher.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace StravaCredentialPublisher {

namespace {

using StravaTokenPublication::PublicationResult;
using StravaTokenPublication::PublicationStatus;

struct PendingPublication
{
    std::mutex mutex;
    std::condition_variable condition;
    bool abandoned = false;
    bool complete = false;
    PublicationResult result;
};

PublicationResult invalidInput()
{
    return {
        PublicationStatus::InvalidInput,
        QStringLiteral("Strava credential publication inputs are invalid.")
    };
}

PublicationResult storageFailure()
{
    return {
        PublicationStatus::StorageFailure,
        QStringLiteral("Strava credentials could not be stored securely.")
    };
}

bool cancellationRequested(const CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

PublicationResult publishOnSettingsThread(const Request &request)
{
    if (!appsettings) return storageFailure();

    const QString accountKey = request.accountKey;
    StravaTokenPublication::PublicationCallbacks callbacks;
    callbacks.readCurrent = [accountKey] {
        return StravaTokenPublication::TokenPair{
            appsettings->cvalue(
                accountKey, GC_STRAVA_TOKEN, QString()).toString(),
            appsettings->cvalue(
                accountKey,
                GC_STRAVA_REFRESH_TOKEN,
                QString()).toString()
        };
    };
    callbacks.writeRefreshToken =
        [accountKey](const QString &value) {
            return appsettings->setCValueChecked(
                accountKey, GC_STRAVA_REFRESH_TOKEN, value);
        };
    callbacks.writeAccessToken =
        [accountKey](const QString &value) {
            return appsettings->setCValueChecked(
                accountKey, GC_STRAVA_TOKEN, value);
        };
    callbacks.writeTimestamp =
        [accountKey](const QString &value) {
            return appsettings->setCValueChecked(
                accountKey, GC_STRAVA_LAST_REFRESH, value);
        };

    return StravaTokenPublication::publish(
        request.expectedRefreshToken,
        request.replacement,
        request.refreshedAt,
        request.mode,
        callbacks);
}

} // namespace

bool Request::isValid() const
{
    return !accountKey.trimmed().isEmpty()
        && replacement.isValid()
        && !refreshedAt.isEmpty()
        && (mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || !expectedRefreshToken.isEmpty());
}

PublicationResult publish(
    const Request &request,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!request.isValid() || timeoutMs <= 0)
        return invalidInput();
    if (cancellationRequested(cancelled))
        return storageFailure();

    QCoreApplication *application = QCoreApplication::instance();
    if (!application) return storageFailure();
    if (QThread::currentThread() == application->thread())
        return publishOnSettingsThread(request);

    const auto pending = std::make_shared<PendingPublication>();
    const bool queued = QMetaObject::invokeMethod(
        application,
        [pending, request]() {
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                if (pending->abandoned) {
                    pending->complete = true;
                    pending->condition.notify_all();
                    return;
                }
            }

            const PublicationResult result =
                publishOnSettingsThread(request);
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                if (!pending->abandoned)
                    pending->result = result;
                pending->complete = true;
            }
            pending->condition.notify_all();
        },
        Qt::QueuedConnection);
    if (!queued) return storageFailure();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            if (pending->complete) return pending->result;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                pending->abandoned = true;
                return storageFailure();
            }
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - now);
            pending->condition.wait_for(
                lock,
                std::min(
                    remaining,
                    std::chrono::milliseconds(10)));
            if (pending->complete) return pending->result;
        }
        if (cancellationRequested(cancelled)) {
            const std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->complete) return pending->result;
            pending->abandoned = true;
            return storageFailure();
        }
    }
}

} // namespace StravaCredentialPublisher
