/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_TOKEN_REFRESH_H
#define GC_STRAVA_TOKEN_REFRESH_H

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct StravaTokenRefreshResult
{
    bool success = false;
    QString accessToken;
    QString refreshToken;
    QString error;
    QString sourceRefreshToken;
    QString refreshedAt;
    bool remoteGrantMayHaveRotated = false;

    bool isValid() const;
};

enum class StravaAuthorizationStatus
{
    Active,
    RevocationPending,
    Revoked
};

struct StravaAuthorizationRemovalResult
{
    bool removed = false;
    bool cleanupPending = false;
    QString error;
    bool remoteAuthorizationMayRemain = false;

    bool isSuccess() const
    {
        return removed && error.isEmpty();
    }
};

struct StravaAuthorizationSnapshot
{
    StravaAuthorizationStatus status =
        StravaAuthorizationStatus::Revoked;
    QString accessToken;
    QString refreshToken;
    std::uint64_t epoch = 0;
    bool remoteGrantMayHaveRotated = false;
};

struct StravaAuthorizedRequestLease;

class StravaAuthorizedRequest final
{
public:
    StravaAuthorizedRequest();
    ~StravaAuthorizedRequest();
    StravaAuthorizedRequest(
        StravaAuthorizedRequest &&other) noexcept;
    StravaAuthorizedRequest &operator=(
        StravaAuthorizedRequest &&other) noexcept;

    StravaAuthorizedRequest(
        const StravaAuthorizedRequest &) = delete;
    StravaAuthorizedRequest &operator=(
        const StravaAuthorizedRequest &) = delete;

    bool isValid() const;
    QString accessToken() const;
    bool authorizeDispatch();
    void setAbortOperation(
        const std::function<void()> &operation);
    void release();

private:
    StravaAuthorizedRequest(
        std::shared_ptr<StravaAuthorizedRequestLease> lease,
        QString accessToken);

    std::shared_ptr<StravaAuthorizedRequestLease> lease;
    QString token;

    friend class StravaTokenRefreshCoordinator;
};

class StravaTokenRefreshCoordinator final
{
public:
    using RefreshOperation =
        std::function<StravaTokenRefreshResult()>;
    using CanonicalRefreshOperation =
        std::function<StravaTokenRefreshResult(const QString &)>;
    using CancellationCheck = std::function<bool()>;
    using AuthorizationRemovalOperation =
        std::function<StravaAuthorizationRemovalResult(
            const QString &effectiveRefreshToken)>;
    using AuthorizationRemovalTransactionOperation =
        std::function<StravaAuthorizationRemovalResult(
            const QString &remoteRefreshToken,
            const QString &durableRefreshToken)>;
    using AuthorizationInstallationOperation =
        std::function<bool()>;
    using AuthorizationPendingOperation =
        std::function<bool()>;

    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaTokenRefreshResult refresh(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

    static StravaTokenRefreshResult
    refreshAfterRejectedAccessToken(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const QString &rejectedAccessToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaTokenRefreshResult
    refreshAfterRejectedAccessToken(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const QString &rejectedAccessToken,
        const CanonicalRefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime);

    static bool installAuthorization(
        const QString &accountKey,
        const StravaTokenRefreshResult &authorization);
    static bool installAuthorizationDurably(
        const QString &accountKey,
        const StravaTokenRefreshResult &authorization,
        const AuthorizationInstallationOperation &operation);
    static bool installAuthorizationDurably(
        const QString &accountKey,
        const StravaTokenRefreshResult &authorization,
        std::uint64_t expectedAuthorizationEpoch,
        const AuthorizationInstallationOperation &operation);
    static StravaAuthorizationRemovalResult
    removeAuthorization(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const AuthorizationRemovalOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaAuthorizationRemovalResult
    removeAuthorization(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const AuthorizationRemovalOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds waitTimeout);
    static StravaAuthorizationRemovalResult
    removeAuthorization(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const AuthorizationPendingOperation &persistPending,
        const AuthorizationRemovalOperation &operation,
        const CancellationCheck &cancelled = {});
    static StravaAuthorizationRemovalResult
    removeAuthorization(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const AuthorizationPendingOperation &persistPending,
        const AuthorizationRemovalOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds waitTimeout,
        const std::optional<std::uint64_t>
            &expectedAuthorizationEpoch = std::nullopt);
    static StravaAuthorizationRemovalResult
    removeAuthorizationTransaction(
        const QString &accountKey,
        const QString &inputRefreshToken,
        const AuthorizationPendingOperation &persistPending,
        const AuthorizationRemovalTransactionOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds waitTimeout,
        const std::optional<std::uint64_t>
            &expectedAuthorizationEpoch = std::nullopt);
    static void initializeAuthorization(
        const QString &accountKey,
        StravaAuthorizationStatus status,
        const QString &accessToken,
        const QString &refreshToken,
        bool remoteGrantMayHaveRotated = false);
    static void initializeAuthorizationStatus(
        const QString &accountKey,
        StravaAuthorizationStatus status);
    static StravaAuthorizationStatus authorizationStatus(
        const QString &accountKey);
    static StravaAuthorizationStatus
    authorizationStatusFromStorage(const QString &stored);
    static bool authorizationUsable(
        const QString &accountKey);
    static std::uint64_t authorizationEpoch(
        const QString &accountKey);
    static StravaAuthorizationSnapshot authorizationSnapshot(
        const QString &accountKey);
    static StravaAuthorizedRequest beginAuthorizedRequest(
        const QString &accountKey);
    static void invalidate(const QString &accountKey);

private:
    StravaTokenRefreshCoordinator() = delete;
};

#endif
