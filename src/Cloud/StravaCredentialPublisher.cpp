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
using StravaTokenPublication::RemovalResult;
using StravaTokenPublication::RemovalStatus;

template<typename Result>
struct PendingOperation
{
    std::mutex mutex;
    std::condition_variable condition;
    bool abandoned = false;
    bool complete = false;
    bool started = false;
    Result result;
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

RemovalResult invalidRemovalInput()
{
    return {
        RemovalStatus::InvalidInput,
        QStringLiteral(
            "Strava credential removal inputs are invalid.")
    };
}

RemovalResult removalStorageFailure()
{
    return {
        RemovalStatus::StorageFailure,
        QStringLiteral(
            "Strava credentials could not be removed securely.")
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

StravaTokenPublication::PublicationCallbacks callbacksFor(
    const QString &accountKey)
{
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
    return callbacks;
}

bool saveAuthorizationState(
    const QString &accountKey,
    const QString &state)
{
    return appsettings
        && appsettings->setCValueChecked(
            accountKey,
            GC_STRAVA_AUTHORIZATION_STATE,
            state)
        && appsettings->syncCValueChecked(
            accountKey,
            GC_STRAVA_AUTHORIZATION_STATE);
}

bool saveAuthorizationStateAndUncertainty(
    const QString &accountKey,
    const QString &state,
    bool remoteGrantUncertain)
{
    if (!appsettings) return false;

    const bool stateSaved = appsettings->setCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE,
        state);
    const bool uncertaintySaved =
        appsettings->setCValueChecked(
            accountKey,
            GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
            remoteGrantUncertain);
    const bool synced = appsettings->syncCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE);
    return stateSaved && uncertaintySaved && synced;
}

PublicationResult publishOnSettingsThread(const Request &request)
{
    if (!appsettings) return storageFailure();

    const QString accountKey = request.accountKey;
    if (request.activatesAuthorization) {
        if (!saveAuthorizationState(
                accountKey,
                QStringLiteral("authorization_pending"))) {
            return storageFailure();
        }
    }

    const PublicationResult result =
        StravaTokenPublication::publish(
        request.expectedRefreshToken,
        request.replacement,
        request.refreshedAt,
        request.mode,
        callbacksFor(accountKey));
    if (!result.isSuccess()) {
        if (request.activatesAuthorization) {
            saveAuthorizationStateAndUncertainty(
                accountKey,
                QStringLiteral("authorization_pending"),
                true);
        }
        return result;
    }
    if (!request.activatesAuthorization) {
        return result;
    }

    const bool stateSaved = appsettings->setCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("active"));
    const bool uncertaintySaved =
        !request.clearsRemoteGrantUncertainty
        || appsettings->setCValueChecked(
            accountKey,
            GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
            false);
    const bool synced = appsettings->syncCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE);
    if (!stateSaved || !uncertaintySaved || !synced) {
        saveAuthorizationStateAndUncertainty(
            accountKey,
            QStringLiteral("authorization_pending"),
            true);
        return storageFailure();
    }
    return result;
}

bool markAuthorizationPendingOnSettingsThread(
    const QString &accountKey)
{
    return saveAuthorizationStateAndUncertainty(
        accountKey,
        QStringLiteral("authorization_pending"),
        true);
}

bool markRevocationPendingOnSettingsThread(
    const QString &accountKey)
{
    return saveAuthorizationState(
        accountKey,
        QStringLiteral("revocation_pending"));
}

RemovalResult removeOnSettingsThread(
    const RemovalRequest &request)
{
    if (!appsettings) return removalStorageFailure();

    RemovalResult result = StravaTokenPublication::remove(
        request.expectedRefreshToken,
        request.mode,
        callbacksFor(request.accountKey));
    if (!result.isSuccess()) return result;

    const bool stateSaved = appsettings->setCValueChecked(
        request.accountKey,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("revoked"));
    const bool uncertaintyCleared =
        appsettings->setCValueChecked(
            request.accountKey,
            GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
            false);
    const bool activeDisabled = appsettings->setCValueChecked(
        request.accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "/Strava/active"),
        false);
    const bool startupDisabled = appsettings->setCValueChecked(
        request.accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE
            "/Strava/syncstartup"),
        false);
    const bool importDisabled = appsettings->setCValueChecked(
        request.accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE
            "/Strava/syncimport"),
        false);
    const bool synced = appsettings->syncCValueChecked(
        request.accountKey,
        GC_STRAVA_AUTHORIZATION_STATE);

    if ((!stateSaved
         || !uncertaintyCleared
         || !activeDisabled
         || !startupDisabled
         || !importDisabled
         || !synced)
        && result.isSuccess()) {
        result.status = RemovalStatus::CleanupPending;
        result.error = QStringLiteral(
            "Strava credentials were removed, but local "
            "account cleanup is still pending.");
    }
    return result;
}

template<typename Result, typename Operation>
Result runOnSettingsThread(
    Operation operation,
    const Result &failure,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (timeoutMs <= 0 || cancellationRequested(cancelled))
        return failure;

    QCoreApplication *application = QCoreApplication::instance();
    if (!application) return failure;
    if (QThread::currentThread() == application->thread()) {
        try {
            return operation();
        } catch (...) {
            return failure;
        }
    }

    const auto pending =
        std::make_shared<PendingOperation<Result>>();
    const bool queued = QMetaObject::invokeMethod(
        application,
        [pending, operation, failure]() {
            {
                const std::lock_guard<std::mutex> lock(
                    pending->mutex);
                if (pending->abandoned) {
                    pending->complete = true;
                    pending->condition.notify_all();
                    return;
                }
                pending->started = true;
            }

            Result result = failure;
            try {
                result = operation();
            } catch (...) {
            }
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
    if (!queued) return failure;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            if (pending->complete) return pending->result;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (!pending->started) {
                    pending->abandoned = true;
                    return failure;
                }
                pending->condition.wait_for(
                    lock, std::chrono::milliseconds(10));
                if (pending->complete)
                    return pending->result;
                continue;
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
            if (!pending->started) {
                pending->abandoned = true;
                return failure;
            }
        }
    }
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

bool RemovalRequest::isValid() const
{
    return !accountKey.trimmed().isEmpty()
        && (mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || !expectedRefreshToken.isEmpty());
}

PublicationResult publish(
    const Request &request,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!request.isValid())
        return invalidInput();
    return runOnSettingsThread<PublicationResult>(
        [request] {
            return publishOnSettingsThread(request);
        },
        storageFailure(),
        timeoutMs,
        cancelled);
}

bool markRevocationPending(
    const QString &accountKey,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (accountKey.trimmed().isEmpty())
        return false;
    return runOnSettingsThread<bool>(
        [accountKey] {
            return markRevocationPendingOnSettingsThread(
                accountKey);
        },
        false,
        timeoutMs,
        cancelled);
}

bool markAuthorizationPending(
    const QString &accountKey,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (accountKey.trimmed().isEmpty())
        return false;
    return runOnSettingsThread<bool>(
        [accountKey] {
            return markAuthorizationPendingOnSettingsThread(
                accountKey);
        },
        false,
        timeoutMs,
        cancelled);
}

RemovalResult remove(
    const RemovalRequest &request,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!request.isValid())
        return invalidRemovalInput();
    return runOnSettingsThread<RemovalResult>(
        [request] {
            return removeOnSettingsThread(request);
        },
        removalStorageFailure(),
        timeoutMs,
        cancelled);
}

} // namespace StravaCredentialPublisher
