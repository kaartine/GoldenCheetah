/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaAccountRemoval.h"

#include "StravaCredentialPublisher.h"
#include "StravaNetworkReply.h"
#include "StravaOAuthPolicy.h"
#include "StravaRevocationClient.h"
#include "StravaTokenRefresh.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>

#include <chrono>

namespace StravaAccountRemoval {

namespace {

constexpr int NetworkTimeoutMs = 30000;
constexpr int SettingsTimeoutMs = 60000;

bool cancellationRequested(
    const CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

Result failure(
    const QString &error,
    bool remoteAuthorizationMayRemain = true)
{
    Result result;
    result.error = error.left(1024);
    result.remoteAuthorizationMayRemain =
        remoteAuthorizationMayRemain;
    return result;
}

StravaNetworkReply::Result performRevocationRequest(
    const StravaOAuthPolicy::RevocationRequest &request,
    qsizetype maximumBytes,
    const CancellationCheck &cancelled,
    bool *mayHaveBeenDispatched)
{
    StravaNetworkReply::Result result;
    if (mayHaveBeenDispatched)
        *mayHaveBeenDispatched = false;
    if (!request.isValid()
        || maximumBytes <= 0
        || cancellationRequested(cancelled)) {
        result.failure =
            cancellationRequested(cancelled)
                ? StravaNetworkReply::Failure::Cancelled
                : StravaNetworkReply::Failure::Invalid;
        result.networkError =
            QNetworkReply::UnknownNetworkError;
        result.networkErrorString = QStringLiteral(
            "The Strava revocation request is invalid.");
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest networkRequest(request.endpoint);
    networkRequest.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral(
            "application/x-www-form-urlencoded"));
    networkRequest.setRawHeader(
        QByteArrayLiteral("Authorization"),
        request.authorizationHeader);
    networkRequest.setTransferTimeout(NetworkTimeoutMs);
    networkRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setAttribute(
        QNetworkRequest::AutoDeleteReplyOnFinishAttribute,
        false);

    QNetworkReply *reply =
        manager.post(networkRequest, request.body);
    if (!reply) {
        result.failure =
            StravaNetworkReply::Failure::Invalid;
        result.networkError =
            QNetworkReply::UnknownNetworkError;
        result.networkErrorString = QStringLiteral(
            "The Strava revocation request could not be started.");
        return result;
    }
    if (mayHaveBeenDispatched)
        *mayHaveBeenDispatched = true;
    return StravaNetworkReply::collect(
        reply,
        maximumBytes,
        NetworkTimeoutMs,
        cancelled);
}

RemoteRevocationResult revokeRemoteAuthorization(
    const QString &clientId,
    const QString &clientSecret,
    const QString &token,
    RevocationToken tokenType,
    const CancellationCheck &cancelled)
{
    bool mayHaveBeenDispatched = false;
    StravaRevocationClient client(
        [&mayHaveBeenDispatched](
            const StravaOAuthPolicy::RevocationRequest &request,
            qsizetype maximumBytes,
            const CancellationCheck &operationCancelled) {
            return performRevocationRequest(
                request,
                maximumBytes,
                operationCancelled,
                &mayHaveBeenDispatched);
        });
    const StravaRevocationClient::Result revoked =
        client.revoke(
            clientId,
            clientSecret,
            token,
            tokenType == RevocationToken::RefreshToken
                ? StravaOAuthPolicy::RevocationTokenType::
                    RefreshToken
                : StravaOAuthPolicy::RevocationTokenType::
                    AccessToken,
            cancelled);
    return {
        revoked.isSuccess(),
        revoked.error,
        mayHaveBeenDispatched
    };
}

} // namespace

bool Request::isValid() const
{
    if (accountKey.trimmed().isEmpty())
        return false;
    if (mode != Mode::RevokeRemote
        && mode != Mode::LocalOnly) {
        return false;
    }
    if (mode == Mode::LocalOnly)
        return true;
    return StravaOAuthPolicy::hasUsableCredentials(
            clientId, clientSecret);
}

Result execute(
    const Request &request,
    const CancellationCheck &cancelled,
    const IrreversibleCallback &irreversible)
{
    return execute(
        request,
        cancelled,
        [&request](
            const QString &token,
            RevocationToken tokenType,
            const CancellationCheck &operationCancelled) {
            return revokeRemoteAuthorization(
                request.clientId,
                request.clientSecret,
                token,
                tokenType,
                operationCancelled);
        },
        irreversible);
}

Result execute(
    const Request &request,
    const CancellationCheck &cancelled,
    const RemoteRevocationOperation &revokeRemote,
    const IrreversibleCallback &irreversible)
{
    if (!request.isValid())
        return failure(QStringLiteral(
            "Strava account removal inputs are invalid."));
    if (request.mode == Mode::RevokeRemote
        && !revokeRemote) {
        return failure(QStringLiteral(
            "The Strava remote revocation operation is unavailable."));
    }
    if (cancellationRequested(cancelled))
        return failure(QStringLiteral(
            "Strava account removal was cancelled."));

    QString mutationError;
    const std::shared_ptr<StravaCredentialDurability::Mutation> mutation =
        StravaCredentialPublisher::beginMutation(
            request.accountKey,
            StravaCredentialDurability::MutationKind::Revocation,
            NetworkTimeoutMs,
            mutationError);
    if (!mutation) {
        return failure(
            mutationError.isEmpty()
                ? QStringLiteral(
                      "The Strava credential transaction could not be started.")
                : mutationError);
    }

    const StravaCredentialPublisher::StoredAuthorization
        fencedAuthorization = StravaCredentialPublisher::
            readStoredAuthorization(mutation);
    if (!fencedAuthorization.readable) {
        StravaCredentialPublisher::finishNoChange(mutation);
        return failure(
            mutationError.isEmpty()
                ? QStringLiteral(
                      "The current Strava authorization could not be read securely.")
                : mutationError,
            true);
    }

    bool irreversibleStarted = false;
    bool mutationOperationStarted = false;
    bool pendingStateCommitOwnedByWorker = false;
    const CancellationCheck operationCancelled =
        [&cancelled, &irreversibleStarted] {
            return !irreversibleStarted
                && cancellationRequested(cancelled);
        };
    const auto persistPending =
        [&request,
         &operationCancelled,
         &irreversible,
         &irreversibleStarted,
         &pendingStateCommitOwnedByWorker,
         mutation] {
            const StravaCredentialPublisher::StateCommitResult stored =
                StravaCredentialPublisher::
                markRevocationPendingTracked(
                    request.accountKey,
                    mutation,
                    NetworkTimeoutMs,
                    operationCancelled);
            pendingStateCommitOwnedByWorker =
                stored.status
                    == StravaCredentialPublisher::StateCommitStatus::Pending
                || stored.status
                    == StravaCredentialPublisher::StateCommitStatus::StorageFailure;
            if (!stored.isSuccess())
                return false;

            irreversibleStarted = true;
            if (irreversible) {
                try {
                    irreversible();
                } catch (...) {
                }
            }
            return true;
        };

    bool remoteAuthorizationRevoked = false;
    const auto operation =
        [&request,
         &operationCancelled,
         &revokeRemote,
         &remoteAuthorizationRevoked,
         &mutationOperationStarted,
         fencedAuthorization,
         mutation](
            const QString &,
            const QString &) {
            QString mutationError;
            if (!mutation->isCurrent(mutationError)) {
                return StravaAuthorizationRemovalResult{
                    false, false, mutationError, true
                };
            }
            QString token;
            RevocationToken tokenType = RevocationToken::RefreshToken;
            if (request.mode == Mode::RevokeRemote) {
                const bool useRefreshToken =
                    !fencedAuthorization.credentials
                        .refreshToken.isEmpty();
                token = useRefreshToken
                    ? fencedAuthorization.credentials.refreshToken
                    : fencedAuthorization.credentials.accessToken;
                if (token.isEmpty()) {
                    return StravaAuthorizationRemovalResult{
                        false,
                        false,
                        QStringLiteral(
                            "The current Strava revocation token is unavailable."),
                        true
                    };
                }
                tokenType =
                    useRefreshToken
                        ? RevocationToken::RefreshToken
                        : RevocationToken::AccessToken;
            }
            StravaCredentialPublisher::RemovalRequest removal;
            removal.accountKey = request.accountKey;
            removal.expectedRefreshToken =
                fencedAuthorization.credentials.refreshToken;
            removal.mode =
                request.mode == Mode::LocalOnly
                    || fencedAuthorization.credentials
                        .refreshToken.isEmpty()
                ? StravaTokenPublication::PublicationMode::Authoritative
                : StravaTokenPublication::PublicationMode::CompareAndSwap;
            removal.mutation = mutation;
            if (request.mode == Mode::RevokeRemote) {
                if (!mutation->markCommitUnknown(mutationError)) {
                    return StravaAuthorizationRemovalResult{
                        false, false, mutationError, true
                    };
                }
                mutationOperationStarted = true;
                const RemoteRevocationResult revoked =
                    revokeRemote(
                        token,
                        tokenType,
                        operationCancelled);
                if (!mutation->isCurrent(mutationError)) {
                    return StravaAuthorizationRemovalResult{
                        false, false, mutationError, true
                    };
                }
                if (!revoked.isSuccess()) {
                    if (!revoked.mayHaveBeenDispatched) {
                        QString abortError;
                        if (mutation->abortBeforeRemoteDispatch(
                                abortError)) {
                            mutationOperationStarted = false;
                            return StravaAuthorizationRemovalResult{
                                false,
                                false,
                                revoked.error,
                                true,
                                true
                            };
                        } else if (!abortError.isEmpty()) {
                            return StravaAuthorizationRemovalResult{
                                false, false, abortError, true
                            };
                        }
                    }
                    return StravaAuthorizationRemovalResult{
                        false, false, revoked.error, true
                    };
                }
                if (!mutation->markLocalCommitStarted(
                        removal.expectedRefreshToken,
                        removal.mode,
                        mutationError)) {
                    return StravaAuthorizationRemovalResult{
                        false, false, mutationError, true
                    };
                }
                remoteAuthorizationRevoked = true;
            }

            mutationOperationStarted = true;
            removal.remoteRevocationMayHaveBeenDispatched =
                remoteAuthorizationRevoked;
            const StravaTokenPublication::RemovalResult removed =
                StravaCredentialPublisher::remove(
                    removal,
                    SettingsTimeoutMs,
                    {});
            if (!removed.isSuccess()) {
                return StravaAuthorizationRemovalResult{
                    false,
                    false,
                    removed.error,
                    request.mode == Mode::LocalOnly
                        || !remoteAuthorizationRevoked
                        || removed.status
                            == StravaTokenPublication::RemovalStatus::Conflict
                };
            }
            return StravaAuthorizationRemovalResult{
                true,
                removed.status
                    == StravaTokenPublication::RemovalStatus::
                        CleanupPending,
                QString(),
                request.mode == Mode::LocalOnly
                    || !remoteAuthorizationRevoked
            };
        };

    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::
            removeAuthorizationTransaction(
                request.accountKey,
                request.refreshToken,
                persistPending,
                operation,
                operationCancelled,
                std::chrono::seconds(75),
                request.expectedAuthorizationEpoch);
    if (!removed.isSuccess()) {
        if (!mutationOperationStarted
            && !pendingStateCommitOwnedByWorker
            && !mutation->isFinished()) {
            StravaCredentialPublisher::finishNoChange(mutation);
        }
        return failure(
            removed.error,
            removed.remoteAuthorizationMayRemain);
    }

    Result result;
    result.disconnected = true;
    result.cleanupPending = removed.cleanupPending;
    result.remoteAuthorizationMayRemain =
        removed.remoteAuthorizationMayRemain;
    return result;
}

} // namespace StravaAccountRemoval
