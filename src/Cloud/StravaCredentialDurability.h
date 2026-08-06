/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_CREDENTIAL_DURABILITY_H
#define GC_STRAVA_CREDENTIAL_DURABILITY_H

#include "StravaTokenPublication.h"

#include <QString>

#include <functional>
#include <memory>

namespace StravaCredentialDurability {

enum class MutationKind
{
    Refresh,
    Authorization,
    Revocation
};

enum class RecoveryStatus
{
    NoWork,
    Recovered,
    Pending,
    Conflict,
    StorageFailure,
    Invalid
};

struct RecoveryResult
{
    RecoveryStatus status = RecoveryStatus::Invalid;
    QString error;
    quint64 generation = 0;

    bool isSuccess() const
    {
        return status == RecoveryStatus::NoWork
            || status == RecoveryStatus::Recovered;
    }
};

struct Publication
{
    QString expectedRefreshToken;
    StravaTokenPublication::TokenPair replacement;
    QString refreshedAt;
    StravaTokenPublication::PublicationMode mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;
    bool activatesAuthorization = false;
    bool clearsRemoteGrantUncertainty = false;

    bool isValid() const;
};

struct StoredState
{
    StravaTokenPublication::TokenPair credentials;
    QString refreshedAt;
    QString authorizationState;
    QString authorizationRevision;
    bool remoteGrantUncertain = true;
    bool readable = false;
};

struct TokenPairReadResult
{
    bool readable = false;
    StravaTokenPublication::TokenPair value;
};

struct PendingTransactionReadResult
{
    bool readable = false;
    QString value;
};

struct StorageCallbacks
{
    std::function<TokenPairReadResult()> readCurrent;
    std::function<QString()> readTimestamp;
    std::function<QString()> readAuthorizationState;
    std::function<bool()> readRemoteGrantUncertain;
    std::function<QString()> readPendingTransaction;
    std::function<bool(const QString &)> writePendingTransaction;
    std::function<bool()> clearPendingTransaction;
    std::function<bool(const QString &)> writeRefreshToken;
    std::function<bool(const QString &)> writeAccessToken;
    std::function<bool(const QString &)> writeTimestamp;
    std::function<bool(const QString &)> writeAuthorizationState;
    std::function<bool(bool)> writeRemoteGrantUncertain;
    std::function<bool()> sync;
    std::function<bool()> refresh;
    std::function<bool()> completeRevocationCleanup;
    std::function<bool()> touchAuthorizationRevision = [] {
        return true;
    };
    std::function<QString()> readAuthorizationRevision = [] {
        return QString();
    };
    std::function<PendingTransactionReadResult()>
        readPendingTransactionForRecovery;

    bool isValid() const;
};

namespace Detail {
struct CoordinatorState;
struct MutationState;
}

class Mutation final
{
public:
    ~Mutation();

    Mutation(const Mutation &) = delete;
    Mutation &operator=(const Mutation &) = delete;

    quint64 generation() const;
    QString transactionId() const;
    bool isCurrent(QString &error) const;
    bool readStoredState(StoredState &stored, QString &error) const;
    bool markPendingState(QString &error);
    bool markLocalCommitStarted(
        const QString &expectedRefreshToken,
        StravaTokenPublication::PublicationMode mode,
        QString &error);
    bool isFinished() const;

    bool armPublication(
        const Publication &publication,
        QString &error);
    StravaTokenPublication::PublicationResult
    commitArmedPublication();
    StravaTokenPublication::PublicationResult publish(
        const Publication &publication);
    bool markCommitUnknown(QString &error);
    bool abortBeforeRemoteDispatch(QString &error);
    bool finishCommit(QString &error);
    bool finishNoChange(QString &error);

private:
    explicit Mutation(
        std::shared_ptr<Detail::MutationState> state);

    std::shared_ptr<Detail::MutationState> state_;

    friend class Coordinator;
};

class Coordinator final
{
public:
    Coordinator(
        QString accountKey,
        QString transactionParent,
        StorageCallbacks storage);

    std::shared_ptr<Mutation> begin(
        MutationKind kind,
        int timeoutMs,
        QString &error,
        bool recoverPending = true);
    RecoveryResult recover(int timeoutMs);
    bool readStoredState(
        StoredState &stored,
        int timeoutMs,
        QString &error);

private:
    std::shared_ptr<Detail::CoordinatorState> state_;
};

} // namespace StravaCredentialDurability

#endif
