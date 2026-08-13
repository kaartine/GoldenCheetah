/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_CREDENTIAL_PUBLISHER_H
#define GC_STRAVA_CREDENTIAL_PUBLISHER_H

#include "StravaCredentialDurability.h"
#include "StravaTokenPublication.h"

#include <QString>

#include <functional>
#include <memory>

namespace StravaCredentialPublisher {

constexpr int CredentialSnapshotTimeoutMs = 60000;

struct Request
{
    QString accountKey;
    QString expectedRefreshToken;
    StravaTokenPublication::TokenPair replacement;
    QString refreshedAt;
    StravaTokenPublication::PublicationMode mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;
    bool activatesAuthorization = false;
    bool clearsRemoteGrantUncertainty = false;
    std::shared_ptr<StravaCredentialDurability::Mutation> mutation;

    bool isValid() const;
};

struct RemovalRequest
{
    QString accountKey;
    QString expectedRefreshToken;
    StravaTokenPublication::PublicationMode mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;
    bool remoteRevocationMayHaveBeenDispatched = false;
    std::shared_ptr<StravaCredentialDurability::Mutation> mutation;

    bool isValid() const;
};

using CancellationCheck = std::function<bool()>;

enum class StateCommitStatus
{
    Saved,
    NotStarted,
    Pending,
    StorageFailure
};

struct StateCommitResult
{
    StateCommitStatus status = StateCommitStatus::NotStarted;

    bool isSuccess() const
    {
        return status == StateCommitStatus::Saved;
    }

    bool canDiscardMutation() const
    {
        return status == StateCommitStatus::NotStarted;
    }
};

struct StoredAuthorization
{
    StravaTokenPublication::TokenPair credentials;
    QString refreshedAt;
    QString state;
    QString revision;
    bool remoteGrantUncertain = true;
    bool readable = false;
};

struct StoredAuthorizationMetadata
{
    QString state;
    QString revision;
    bool readable = false;
};

std::shared_ptr<StravaCredentialDurability::Mutation>
beginMutation(
    const QString &accountKey,
    StravaCredentialDurability::MutationKind kind,
    int timeoutMs,
    QString &error);
StravaCredentialDurability::RecoveryResult recover(
    const QString &accountKey,
    int timeoutMs = CredentialSnapshotTimeoutMs);
StoredAuthorization readStoredAuthorization(
    const QString &accountKey,
    int timeoutMs = CredentialSnapshotTimeoutMs);
StoredAuthorizationMetadata readStoredAuthorizationMetadata(
    const QString &accountKey,
    int timeoutMs = CredentialSnapshotTimeoutMs);
bool finishNoChange(
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs = 30000);
StoredAuthorization readStoredAuthorization(
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs = 30000);

StravaTokenPublication::PublicationResult publish(
    const Request &request,
    int timeoutMs = 30000,
    const CancellationCheck &cancelled = {});
StateCommitResult markAuthorizationPendingTracked(
    const QString &accountKey,
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs = 30000,
    const CancellationCheck &cancelled = {});
StateCommitResult markRevocationPendingTracked(
    const QString &accountKey,
    const std::shared_ptr<StravaCredentialDurability::Mutation> &mutation,
    int timeoutMs = 30000,
    const CancellationCheck &cancelled = {});
StravaTokenPublication::RemovalResult remove(
    const RemovalRequest &request,
    int timeoutMs = 30000,
    const CancellationCheck &cancelled = {});

} // namespace StravaCredentialPublisher

#endif
