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
#include "StravaSettingsCommit.h"

#include <QUuid>

#include <memory>
#include <stdexcept>

namespace StravaCredentialPublisher {

namespace {

using StravaTokenPublication::PublicationResult;
using StravaTokenPublication::PublicationStatus;
using StravaTokenPublication::RemovalResult;
using StravaTokenPublication::RemovalStatus;

PublicationResult invalidInput()
{
    return {
        PublicationStatus::InvalidInput,
        QStringLiteral("Strava credential publication inputs are invalid.")
    };
}

PublicationResult publicationPending()
{
    return {
        PublicationStatus::Pending,
        QStringLiteral(
            "Strava credential publication is pending recovery.")
    };
}

PublicationResult storageFailure()
{
    return {
        PublicationStatus::StorageFailure,
        QStringLiteral("Strava credentials could not be stored securely.")
    };
}

RemovalResult removalPending()
{
    return {
        RemovalStatus::Pending,
        QStringLiteral(
            "Strava credential removal has an unknown outcome and is pending recovery.")
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

bool completeRevocationCleanup(const QString &accountKey)
{
    if (!appsettings
        || StravaSettingsCommit::credentialThreadShutdownRequested()) {
        return false;
    }
    const bool stateSaved = appsettings->setCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("revoked"));
    const bool uncertaintyCleared = appsettings->setCValueChecked(
        accountKey,
        GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
        false);
    const bool activeDisabled = appsettings->setCValueChecked(
        accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "/Strava/active"),
        false);
    const bool startupDisabled = appsettings->setCValueChecked(
        accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "/Strava/syncstartup"),
        false);
    const bool importDisabled = appsettings->setCValueChecked(
        accountKey,
        QStringLiteral(
            GC_QSETTINGS_ATHLETE_PRIVATE "/Strava/syncimport"),
        false);
    const bool revisionSaved = appsettings->setCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_REVISION,
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const bool synced = appsettings->syncCValueChecked(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE);
    return stateSaved
        && uncertaintyCleared
        && activeDisabled
        && startupDisabled
        && importDisabled
        && revisionSaved
        && synced;
}

StravaCredentialDurability::TokenPairReadResult
readCurrentCredentials(const QString &accountKey)
{
    if (!appsettings
        || StravaSettingsCommit::credentialThreadShutdownRequested()) {
        return {};
    }
    const GSettings::CredentialReadResult access =
        appsettings->credentialCValueChecked(
            accountKey, GC_STRAVA_TOKEN);
    const GSettings::CredentialReadResult refresh =
        appsettings->credentialCValueChecked(
            accountKey, GC_STRAVA_REFRESH_TOKEN);
    const bool readable = access.readable() && refresh.readable();
    return {
        readable,
        readable
            ? StravaTokenPublication::TokenPair{
                access.status == GSettings::CredentialReadStatus::Present
                    ? access.value.toString() : QString(),
                refresh.status == GSettings::CredentialReadStatus::Present
                    ? refresh.value.toString() : QString()
            }
            : StravaTokenPublication::TokenPair()
    };
}

StravaCredentialDurability::StorageCallbacks storageFor(
    const QString &accountKey)
{
    StravaCredentialDurability::StorageCallbacks callbacks;
    callbacks.readCurrent = [accountKey] {
        return readCurrentCredentials(accountKey);
    };
    callbacks.readTimestamp = [accountKey] {
        if (!appsettings
            || StravaSettingsCommit::
                credentialThreadShutdownRequested()) {
            return QString();
        }
        return appsettings->cvalue(
            accountKey,
            GC_STRAVA_LAST_REFRESH,
            QString()).toString();
    };
    callbacks.readAuthorizationState = [accountKey] {
        if (!appsettings
            || StravaSettingsCommit::
                credentialThreadShutdownRequested()) {
            return QStringLiteral("authorization_pending");
        }
        return appsettings->cvalue(
            accountKey,
            GC_STRAVA_AUTHORIZATION_STATE,
            QStringLiteral("active")).toString();
    };
    callbacks.readRemoteGrantUncertain = [accountKey] {
        if (!appsettings
            || StravaSettingsCommit::
                credentialThreadShutdownRequested()) {
            return true;
        }
        return appsettings->cvalue(
            accountKey,
            GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
            true).toBool();
    };
    callbacks.readPendingTransaction = [accountKey] {
        if (!appsettings
            || StravaSettingsCommit::
                credentialThreadShutdownRequested()) {
            return QString();
        }
        return appsettings->cvalue(
            accountKey,
            GC_STRAVA_PENDING_TRANSACTION,
            QString()).toString();
    };
    callbacks.writePendingTransaction =
        [accountKey](const QString &value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey,
                GC_STRAVA_PENDING_TRANSACTION,
                value);
        };
    callbacks.clearPendingTransaction = [accountKey] {
        return appsettings
            && !StravaSettingsCommit::
                credentialThreadShutdownRequested()
            && appsettings->setCValueChecked(
            accountKey,
            GC_STRAVA_PENDING_TRANSACTION,
            QVariant());
    };
    callbacks.writeRefreshToken =
        [accountKey](const QString &value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey, GC_STRAVA_REFRESH_TOKEN, value);
        };
    callbacks.writeAccessToken =
        [accountKey](const QString &value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey, GC_STRAVA_TOKEN, value);
        };
    callbacks.writeTimestamp =
        [accountKey](const QString &value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey, GC_STRAVA_LAST_REFRESH, value);
        };
    callbacks.writeAuthorizationState =
        [accountKey](const QString &value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey,
                GC_STRAVA_AUTHORIZATION_STATE,
                value);
        };
    callbacks.writeRemoteGrantUncertain =
        [accountKey](bool value) {
            return appsettings
                && !StravaSettingsCommit::
                    credentialThreadShutdownRequested()
                && appsettings->setCValueChecked(
                accountKey,
                GC_STRAVA_REMOTE_GRANT_UNCERTAIN,
                value);
        };
    callbacks.sync = [accountKey] {
        return appsettings
            && !StravaSettingsCommit::
                credentialThreadShutdownRequested()
            && appsettings->syncCValueChecked(
            accountKey,
            GC_STRAVA_AUTHORIZATION_STATE);
    };
    callbacks.refresh = [accountKey] {
        return appsettings
            && !StravaSettingsCommit::
                credentialThreadShutdownRequested()
            && appsettings->syncCValueChecked(
            accountKey,
            GC_STRAVA_AUTHORIZATION_STATE);
    };
    callbacks.completeRevocationCleanup = [accountKey] {
        return completeRevocationCleanup(accountKey);
    };
    callbacks.touchAuthorizationRevision = [accountKey] {
        return appsettings
            && !StravaSettingsCommit::
                credentialThreadShutdownRequested()
            && appsettings->setCValueChecked(
                accountKey,
                GC_STRAVA_AUTHORIZATION_REVISION,
                QUuid::createUuid().toString(
                    QUuid::WithoutBraces));
    };
    callbacks.readPendingTransactionForRecovery =
        [accountKey] {
            if (!appsettings
                || StravaSettingsCommit::
                    credentialThreadShutdownRequested()) {
                return StravaCredentialDurability::
                    PendingTransactionReadResult{};
            }
            const GSettings::CredentialReadResult pending =
                appsettings->credentialCValueChecked(
                    accountKey,
                    GC_STRAVA_PENDING_TRANSACTION);
            return StravaCredentialDurability::PendingTransactionReadResult{
                pending.readable(),
                pending.status
                        == GSettings::CredentialReadStatus::Present
                    ? pending.value.toString() : QString()
            };
        };
    callbacks.readAuthorizationRevision = [accountKey] {
        if (!appsettings
            || StravaSettingsCommit::
                credentialThreadShutdownRequested()) {
            return QString();
        }
        return appsettings->cvalue(
            accountKey,
            GC_STRAVA_AUTHORIZATION_REVISION,
            QString()).toString();
    };
    return callbacks;
}

RemovalResult removeOnSettingsThread(
    const RemovalRequest &request)
{
    if (!appsettings) return removalStorageFailure();

    const StravaCredentialDurability::StorageCallbacks storage =
        storageFor(request.accountKey);
    RemovalResult result = StravaTokenPublication::remove(
        request.expectedRefreshToken,
        request.mode,
        {
            [storage] {
                const StravaCredentialDurability::TokenPairReadResult
                    current = storage.readCurrent();
                if (!current.readable) {
                    throw std::runtime_error(
                        "credential read unavailable");
                }
                return current.value;
            },
            storage.writeRefreshToken,
            storage.writeAccessToken,
            storage.writeTimestamp
        });
    if (!result.isSuccess()) return result;

    if (!storage.completeRevocationCleanup()) {
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
    const Result &pending,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    auto result = std::make_shared<Result>(failure);
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [result, operation] {
                try {
                    *result = operation();
                } catch (...) {
                }
            },
            timeoutMs,
            cancelled);
    if (dispatched.status
        == StravaSettingsCommit::DispatchStatus::Completed) {
        return *result;
    }
    return dispatched.status
            == StravaSettingsCommit::DispatchStatus::Pending
        ? pending : failure;
}

StateCommitResult runPendingStateCommit(
    const std::function<bool()> &operation,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    auto saved = std::make_shared<bool>(false);
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [saved, operation] {
                try {
                    *saved = operation();
                } catch (...) {
                    *saved = false;
                }
            },
            timeoutMs,
            cancelled);
    if (dispatched.status
        == StravaSettingsCommit::DispatchStatus::Pending) {
        return {StateCommitStatus::Pending};
    }
    if (dispatched.status
        == StravaSettingsCommit::DispatchStatus::NotStarted) {
        return {StateCommitStatus::NotStarted};
    }
    return {*saved
            ? StateCommitStatus::Saved
            : StateCommitStatus::StorageFailure};
}

void retireIncompleteStateCommit(
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    StateCommitStatus status)
{
    if (!mutation
        || (status != StateCommitStatus::Pending
            && status != StateCommitStatus::StorageFailure)) {
        return;
    }
    StravaSettingsCommit::runOnCredentialThreadAsync(
        [mutation] {
            QString error;
            mutation->finishNoChange(error);
        },
        {});
}

} // namespace

bool Request::isValid() const
{
    const bool supportedMode = mode
            == StravaTokenPublication::PublicationMode::Authoritative
        || mode
            == StravaTokenPublication::PublicationMode::CompareAndSwap;
    return !accountKey.trimmed().isEmpty()
        && replacement.isValid()
        && !refreshedAt.isEmpty()
        && supportedMode
        && (mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || !expectedRefreshToken.isEmpty());
}

bool RemovalRequest::isValid() const
{
    const bool supportedMode = mode
            == StravaTokenPublication::PublicationMode::Authoritative
        || mode
            == StravaTokenPublication::PublicationMode::CompareAndSwap;
    return !accountKey.trimmed().isEmpty()
        && supportedMode
        && (mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || !expectedRefreshToken.isEmpty());
}

std::shared_ptr<StravaCredentialDurability::Mutation>
beginMutation(
    const QString &accountKey,
    StravaCredentialDurability::MutationKind kind,
    int timeoutMs,
    QString &error)
{
    error.clear();
    if (!appsettings || accountKey.trimmed().isEmpty()
        || timeoutMs <= 0) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return {};
    }

    const QString transactionParent = runOnSettingsThread<QString>(
        [accountKey] {
            return appsettings
                ? appsettings->athleteConfigDirectory(accountKey)
                : QString();
        },
        QString(),
        QString(),
        timeoutMs,
        {});
    if (transactionParent.isEmpty()) {
        error = QStringLiteral(
            "The athlete credential transaction directory is unavailable");
        return {};
    }
    const auto coordinator =
        std::make_shared<StravaCredentialDurability::Coordinator>(
            accountKey, transactionParent, storageFor(accountKey));
    struct BeginResult
    {
        std::shared_ptr<StravaCredentialDurability::Mutation> mutation;
        QString error;
    };
    const auto started = std::make_shared<BeginResult>();
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [coordinator, kind, timeoutMs, started] {
                started->mutation = coordinator->begin(
                    kind, timeoutMs, started->error, false);
            },
            timeoutMs,
            {});
    if (dispatched.status
        == StravaSettingsCommit::DispatchStatus::Completed) {
        error = started->error;
        return started->mutation;
    }
    if (dispatched.status
        == StravaSettingsCommit::DispatchStatus::Pending) {
        StravaSettingsCommit::runOnCredentialThreadAsync(
            [started] {
                if (!started->mutation) return;
                QString finishError;
                started->mutation->finishNoChange(finishError);
            },
            {});
        error = QStringLiteral(
            "The Strava credential transaction is still starting");
    }
    return {};
}

StravaCredentialDurability::RecoveryResult recover(
    const QString &accountKey,
    int timeoutMs)
{
    if (!appsettings || accountKey.trimmed().isEmpty()
        || timeoutMs <= 0) {
        return {
            StravaCredentialDurability::RecoveryStatus::Invalid,
            QStringLiteral(
                "The Strava credential recovery is unavailable"),
            0
        };
    }
    const QString transactionParent = runOnSettingsThread<QString>(
        [accountKey] {
            return appsettings
                ? appsettings->athleteConfigDirectory(accountKey)
                : QString();
        },
        QString(),
        QString(),
        timeoutMs,
        {});
    if (transactionParent.isEmpty()) {
        return {
            StravaCredentialDurability::RecoveryStatus::StorageFailure,
            QStringLiteral(
                "The athlete credential transaction directory is unavailable"),
            0
        };
    }
    const auto coordinator =
        std::make_shared<StravaCredentialDurability::Coordinator>(
            accountKey, transactionParent, storageFor(accountKey));
    const StravaCredentialDurability::RecoveryResult failure = {
        StravaCredentialDurability::RecoveryStatus::StorageFailure,
        QStringLiteral(
            "Strava credential recovery could not be started"),
        0
    };
    const StravaCredentialDurability::RecoveryResult pending = {
        StravaCredentialDurability::RecoveryStatus::Pending,
        QStringLiteral(
            "Strava credential recovery is still running"),
        0
    };
    return runOnSettingsThread<
        StravaCredentialDurability::RecoveryResult>(
            [coordinator, timeoutMs] {
                return coordinator->recover(timeoutMs);
            },
            failure,
            pending,
            timeoutMs,
            {});
}

StoredAuthorization readStoredAuthorization(
    const QString &accountKey,
    int timeoutMs)
{
    if (!appsettings || accountKey.trimmed().isEmpty()
        || timeoutMs <= 0) {
        return {};
    }
    const QString transactionParent = runOnSettingsThread<QString>(
        [accountKey] {
            return appsettings
                ? appsettings->athleteConfigDirectory(accountKey)
                : QString();
        },
        QString(),
        QString(),
        timeoutMs,
        {});
    if (transactionParent.isEmpty()) return {};
    const auto coordinator =
        std::make_shared<StravaCredentialDurability::Coordinator>(
            accountKey, transactionParent, storageFor(accountKey));
    return runOnSettingsThread<StoredAuthorization>(
        [coordinator, timeoutMs] {
            StravaCredentialDurability::StoredState durable;
            QString error;
            if (!coordinator->readStoredState(
                    durable, timeoutMs, error)) {
                return StoredAuthorization();
            }
            StoredAuthorization stored;
            stored.credentials = durable.credentials;
            stored.refreshedAt = durable.refreshedAt;
            stored.state = durable.authorizationState;
            stored.revision = durable.authorizationRevision;
            stored.remoteGrantUncertain = durable.remoteGrantUncertain;
            stored.readable = durable.readable;
            return stored;
        },
        StoredAuthorization(),
        StoredAuthorization(),
        timeoutMs,
        {});
}

StoredAuthorizationMetadata readStoredAuthorizationMetadata(
    const QString &accountKey,
    int timeoutMs)
{
    const StoredAuthorization stored =
        readStoredAuthorization(accountKey, timeoutMs);
    return {
        stored.state,
        stored.revision,
        stored.readable
    };
}

bool finishNoChange(
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs)
{
    if (!mutation || timeoutMs <= 0) return false;
    return runOnSettingsThread<bool>(
        [mutation] {
            QString error;
            return mutation->finishNoChange(error);
        },
        false,
        false,
        timeoutMs,
        {});
}

StoredAuthorization readStoredAuthorization(
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs)
{
    if (!mutation || timeoutMs <= 0) return {};
    return runOnSettingsThread<StoredAuthorization>(
        [mutation] {
            StravaCredentialDurability::StoredState durable;
            QString error;
            if (!mutation->readStoredState(durable, error))
                return StoredAuthorization();
            StoredAuthorization stored;
            stored.credentials = durable.credentials;
            stored.refreshedAt = durable.refreshedAt;
            stored.state = durable.authorizationState;
            stored.revision = durable.authorizationRevision;
            stored.remoteGrantUncertain =
                durable.remoteGrantUncertain;
            stored.readable = durable.readable;
            return stored;
        },
        StoredAuthorization(),
        StoredAuthorization(),
        timeoutMs,
        {});
}

PublicationResult publish(
    const Request &request,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!request.isValid())
        return invalidInput();
    std::shared_ptr<StravaCredentialDurability::Mutation> mutation =
        request.mutation;
    QString error;
    if (!mutation) {
        mutation = beginMutation(
            request.accountKey,
            StravaCredentialDurability::MutationKind::Authorization,
            timeoutMs,
            error);
    }
    if (!mutation)
        return storageFailure();

    StravaCredentialDurability::Publication publication;
    publication.expectedRefreshToken =
        request.expectedRefreshToken;
    publication.replacement = request.replacement;
    publication.refreshedAt = request.refreshedAt;
    publication.mode = request.mode;
    publication.activatesAuthorization =
        request.activatesAuthorization;
    publication.clearsRemoteGrantUncertainty =
        request.clearsRemoteGrantUncertainty;
    return runOnSettingsThread<PublicationResult>(
        [mutation, publication] {
            return mutation->publish(publication);
        },
        publicationPending(),
        publicationPending(),
        timeoutMs,
        cancelled);
}

StateCommitResult markRevocationPendingTracked(
    const QString &accountKey,
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (accountKey.trimmed().isEmpty() || !mutation)
        return {StateCommitStatus::NotStarted};
    const StateCommitResult result = runPendingStateCommit(
        [mutation] {
            QString error;
            return mutation->markPendingState(error);
        },
        timeoutMs,
        cancelled);
    retireIncompleteStateCommit(mutation, result.status);
    return result;
}

StateCommitResult markAuthorizationPendingTracked(
    const QString &accountKey,
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (accountKey.trimmed().isEmpty() || !mutation)
        return {StateCommitStatus::NotStarted};
    const StateCommitResult result = runPendingStateCommit(
        [mutation] {
            QString error;
            return mutation->markPendingState(error);
        },
        timeoutMs,
        cancelled);
    retireIncompleteStateCommit(mutation, result.status);
    return result;
}

RemovalResult remove(
    const RemovalRequest &request,
    int timeoutMs,
    const CancellationCheck &cancelled)
{
    if (!request.isValid())
        return invalidRemovalInput();
    std::shared_ptr<StravaCredentialDurability::Mutation> mutation =
        request.mutation;
    QString error;
    if (!mutation) {
        mutation = beginMutation(
            request.accountKey,
            StravaCredentialDurability::MutationKind::Revocation,
            timeoutMs,
            error);
    }
    if (!mutation)
        return removalPending();
    if (!mutation->markLocalCommitStarted(
            request.expectedRefreshToken,
            request.mode,
            error)) {
        return removalPending();
    }
    return runOnSettingsThread<RemovalResult>(
        [request, mutation] {
            RemovalResult result = removeOnSettingsThread(request);
            if (result.status == RemovalStatus::Cleared) {
                QString finishError;
                if (!mutation->finishCommit(finishError)) {
                    result = removalPending();
                    if (!finishError.isEmpty())
                        result.error = finishError;
                }
            }
            return result;
        },
        removalPending(),
        removalPending(),
        timeoutMs,
        cancelled);
}

} // namespace StravaCredentialPublisher
