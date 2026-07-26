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
    const CancellationCheck &cancelled)
{
    StravaNetworkReply::Result result;
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
    StravaRevocationClient client(
        performRevocationRequest);
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
        revoked.error
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
    return (!refreshToken.isEmpty() || !accessToken.isEmpty())
        && StravaOAuthPolicy::hasUsableCredentials(
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

    bool irreversibleStarted = false;
    const CancellationCheck operationCancelled =
        [&cancelled, &irreversibleStarted] {
            return !irreversibleStarted
                && cancellationRequested(cancelled);
        };
    const auto persistPending =
        [&request,
         &operationCancelled,
         &irreversible,
         &irreversibleStarted] {
            const bool stored =
                StravaCredentialPublisher::
                markRevocationPending(
                    request.accountKey,
                    NetworkTimeoutMs,
                    operationCancelled);
            if (!stored)
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
         &remoteAuthorizationRevoked](
            const QString &effectiveRefreshToken,
            const QString &durableRefreshToken) {
            if (request.mode == Mode::RevokeRemote) {
                const bool useRefreshToken =
                    !effectiveRefreshToken.isEmpty();
                const QString token = useRefreshToken
                    ? effectiveRefreshToken
                    : request.accessToken;
                const RevocationToken tokenType =
                    useRefreshToken
                        ? RevocationToken::RefreshToken
                        : RevocationToken::AccessToken;
                const RemoteRevocationResult revoked =
                    revokeRemote(
                        token,
                        tokenType,
                        operationCancelled);
                if (!revoked.isSuccess()) {
                    return StravaAuthorizationRemovalResult{
                        false, false, revoked.error, true
                    };
                }
                remoteAuthorizationRevoked = true;
            }

            StravaCredentialPublisher::RemovalRequest removal;
            removal.accountKey = request.accountKey;
            removal.expectedRefreshToken =
                durableRefreshToken;
            removal.mode =
                request.mode == Mode::LocalOnly
                    || durableRefreshToken.isEmpty()
                ? StravaTokenPublication::PublicationMode::
                    Authoritative
                : StravaTokenPublication::PublicationMode::
                    CompareAndSwap;
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
