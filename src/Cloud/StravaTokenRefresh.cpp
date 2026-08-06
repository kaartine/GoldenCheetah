/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "StravaTokenRefresh.h"

#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

struct StravaAuthorizedRequestLease
{
    QString accountKey;
    std::uint64_t requestId = 0;
    std::uint64_t generation = 0;
};

namespace {

constexpr int MaximumRememberedTokens = 16;

struct RefreshFlight
{
    QString requestedToken;
    QString effectiveToken;
    std::uint64_t generation = 0;
    bool complete = false;
    bool superseded = false;
    StravaTokenRefreshResult result;
    std::condition_variable condition;
};

struct CachedGrant
{
    bool present = false;
    bool authoritative = false;
    std::uint64_t generation = 0;
    StravaTokenRefreshResult result;
    std::chrono::steady_clock::time_point storedAt;
};

struct ActiveRequest
{
    std::uint64_t generation = 0;
    bool dispatchAuthorized = false;
    bool abortIssued = false;
    std::function<void()> abortOperation;
};

struct AccountState
{
    std::uint64_t generation = 0;
    std::uint64_t authorizationEpoch = 0;
    std::uint64_t nextRequestId = 0;
    std::shared_ptr<RefreshFlight> activeFlight;
    QString canonicalToken;
    QStringList knownTokens;
    QString durableAccessToken;
    QString durableRefreshToken;
    QString latestObservedAccessToken;
    QString latestObservedRefreshToken;
    CachedGrant cache;
    StravaAuthorizationStatus authorizationStatus =
        StravaAuthorizationStatus::Active;
    bool authorizationStatusInitialized = false;
    bool storageInitializationProvisional = false;
    std::uint64_t provisionalGeneration = 0;
    std::uint64_t provisionalAuthorizationEpoch = 0;
    bool authorizationInstallInFlight = false;
    bool removalInFlight = false;
    bool remoteGrantMayHaveRotated = false;
    bool remoteGrantLatestTokenKnown = false;
    QHash<std::uint64_t, ActiveRequest> activeRequests;
    std::condition_variable requestCondition;
};

struct RefreshRegistry
{
    std::mutex mutex;
    QHash<QString, std::shared_ptr<AccountState>> accounts;
};

RefreshRegistry &registry()
{
    static RefreshRegistry value;
    return value;
}

class ThreadBoundAbortRelay final : public QObject
{
public:
    ThreadBoundAbortRelay(
        QObject *target,
        std::function<void(QObject *)> operation)
        : target_(target), operation_(std::move(operation))
    {
    }

    void dispatch()
    {
        QObject *target = target_.data();
        if (target && operation_) operation_(target);
    }

private:
    QPointer<QObject> target_;
    std::function<void(QObject *)> operation_;
};

#ifdef GC_STRAVA_TOKEN_REFRESH_TEST_HOOKS
std::mutex threadBoundDispatchHookMutex;
std::function<void()> threadBoundDispatchHook;

void runThreadBoundDispatchHook()
{
    std::function<void()> hook;
    {
        const std::lock_guard<std::mutex> lock(
            threadBoundDispatchHookMutex);
        hook = threadBoundDispatchHook;
    }
    if (hook) hook();
}
#endif

StravaTokenRefreshResult failure(const QString &error)
{
    return {
        false, QString(), QString(), error, QString(), QString()
    };
}

StravaTokenRefreshResult cancelledResult()
{
    return failure(QStringLiteral("Strava token refresh was cancelled."));
}

StravaTokenRefreshResult supersededResult()
{
    return failure(
        QStringLiteral("Strava token refresh was superseded."));
}

StravaAuthorizationRemovalResult removalFailure(
    const QString &error)
{
    return {
        false, false, error, true
    };
}

StravaAuthorizationRemovalResult removalCancelled()
{
    return removalFailure(QStringLiteral(
        "Strava authorization removal was cancelled."));
}

bool cancellationRequested(
    const StravaTokenRefreshCoordinator::CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

StravaTokenRefreshResult normalized(
    StravaTokenRefreshResult result)
{
    if (!result.success) {
        result.accessToken.clear();
        result.refreshToken.clear();
        result.sourceRefreshToken.clear();
        result.refreshedAt.clear();
        if (result.error.isEmpty()) {
            result.error =
                QStringLiteral("Strava token refresh failed.");
        }
        return result;
    }

    if (result.accessToken.isEmpty()
        || result.refreshToken.isEmpty()
        || !result.error.isEmpty()) {
        return failure(
            QStringLiteral(
                "Strava token refresh returned incomplete credentials."));
    }
    return result;
}

std::shared_ptr<AccountState> accountState(
    RefreshRegistry &value,
    const QString &accountKey)
{
    auto found = value.accounts.find(accountKey);
    if (found != value.accounts.end()) return found.value();

    auto state = std::make_shared<AccountState>();
    value.accounts.insert(accountKey, state);
    return state;
}

void rememberToken(AccountState &state, const QString &token)
{
    if (token.isEmpty()) return;
    state.knownTokens.removeAll(token);
    state.knownTokens.append(token);
    while (state.knownTokens.size() > MaximumRememberedTokens)
        state.knownTokens.removeFirst();
}

void clearCache(AccountState &state)
{
    state.cache = {};
}

void clearProvisionalStorageInitialization(AccountState &state)
{
    state.storageInitializationProvisional = false;
    state.provisionalGeneration = 0;
    state.provisionalAuthorizationEpoch = 0;
}

void applyStoredAuthorization(
    AccountState &state,
    StravaAuthorizationStatus status,
    const QString &accessToken,
    const QString &refreshToken,
    bool remoteGrantMayHaveRotated)
{
    state.authorizationStatus = status;
    state.authorizationStatusInitialized = true;
    state.remoteGrantMayHaveRotated =
        remoteGrantMayHaveRotated;
    state.remoteGrantLatestTokenKnown = false;
    state.durableAccessToken = accessToken;
    state.durableRefreshToken = refreshToken;
    state.latestObservedAccessToken = accessToken;
    state.latestObservedRefreshToken = refreshToken;
    state.canonicalToken.clear();
    state.knownTokens.clear();
    clearCache(state);
    if (!refreshToken.isEmpty()) {
        state.canonicalToken = refreshToken;
        rememberToken(state, refreshToken);
    }
}

void reconcileActiveAuthorization(AccountState &state)
{
    if (state.authorizationStatus
            != StravaAuthorizationStatus::Active
        || state.durableRefreshToken.isEmpty()) {
        return;
    }

    state.canonicalToken = state.durableRefreshToken;
    rememberToken(state, state.durableRefreshToken);
    if (state.cache.present
        && (state.cache.result.accessToken
                != state.durableAccessToken
            || state.cache.result.refreshToken
                != state.durableRefreshToken)) {
        clearCache(state);
    }
}

void releaseRequestLease(
    const std::shared_ptr<StravaAuthorizedRequestLease> &lease)
{
    if (!lease) return;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const auto found = value.accounts.find(lease->accountKey);
    if (found == value.accounts.end()) return;
    const std::shared_ptr<AccountState> state = found.value();
    if (state->activeRequests.remove(lease->requestId) > 0)
        state->requestCondition.notify_all();
}

bool authorizeRequestDispatch(
    const std::shared_ptr<StravaAuthorizedRequestLease> &lease)
{
    if (!lease) return false;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const auto found = value.accounts.find(lease->accountKey);
    if (found == value.accounts.end()) return false;
    const std::shared_ptr<AccountState> state = found.value();
    auto request = state->activeRequests.find(
        lease->requestId);
    if (request == state->activeRequests.end())
        return false;

    const bool authorized =
        request->generation == lease->generation
        && lease->generation == state->generation
        && state->authorizationStatus
            == StravaAuthorizationStatus::Active
        && !state->authorizationInstallInFlight
        && !state->removalInFlight;
    if (!authorized) {
        state->activeRequests.erase(request);
        state->requestCondition.notify_all();
        return false;
    }
    request->dispatchAuthorized = true;
    return true;
}

void registerRequestAbort(
    const std::shared_ptr<StravaAuthorizedRequestLease> &lease,
    const std::function<void()> &operation)
{
    if (!lease || !operation) return;

    std::function<void()> abortNow;
    {
        RefreshRegistry &value = registry();
        const std::lock_guard<std::mutex> lock(value.mutex);
        const auto found = value.accounts.find(
            lease->accountKey);
        if (found == value.accounts.end()) {
            abortNow = operation;
        } else {
            const std::shared_ptr<AccountState> state =
                found.value();
            auto request = state->activeRequests.find(
                lease->requestId);
            if (request == state->activeRequests.end()
                || !request->dispatchAuthorized) {
                abortNow = operation;
            } else {
                request->abortOperation = operation;
                if (state->authorizationStatus
                        != StravaAuthorizationStatus::Active
                    || state->removalInFlight
                    || request->generation
                        != state->generation) {
                    request->abortIssued = true;
                    abortNow = operation;
                }
            }
        }
    }
    if (abortNow) {
        try {
            abortNow();
        } catch (...) {
        }
    }
}

bool cacheIsFresh(
    const CachedGrant &cache,
    std::chrono::milliseconds cacheLifetime)
{
    return cache.present
        && cacheLifetime.count() > 0
        && std::chrono::steady_clock::now() - cache.storedAt
            < cacheLifetime;
}

bool cacheMatches(
    const AccountState &state,
    const QString &inputRefreshToken)
{
    return state.cache.authoritative
        || state.knownTokens.contains(inputRefreshToken);
}

StravaTokenRefreshResult coordinatedRefresh(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const QString &rejectedAccessToken,
    const StravaTokenRefreshCoordinator::
        CanonicalRefreshOperation &operation,
    const StravaTokenRefreshCoordinator::
        CancellationCheck &cancelled,
    std::chrono::milliseconds cacheLifetime)
{
    const bool followsRejectedAccess =
        !rejectedAccessToken.isEmpty();
    if (accountKey.trimmed().isEmpty()
        || inputRefreshToken.isEmpty()
        || !operation) {
        return failure(
            QStringLiteral("Strava token refresh inputs are invalid."));
    }
    if (cancellationRequested(cancelled))
        return cancelledResult();

    RefreshRegistry &value = registry();
    std::shared_ptr<AccountState> state;
    std::shared_ptr<RefreshFlight> flight;

    for (;;) {
        std::unique_lock<std::mutex> lock(value.mutex);
        state = accountState(value, accountKey);

        if (state->authorizationStatus
                != StravaAuthorizationStatus::Active
            || state->authorizationInstallInFlight
            || state->removalInFlight) {
            return failure(QStringLiteral(
                "Strava authorization is not active."));
        }
        if (state->cache.present
            && state->cache.generation != state->generation) {
            clearCache(*state);
        }
        if (state->cache.present
            && !cacheIsFresh(state->cache, cacheLifetime)) {
            clearCache(*state);
        }
        if (state->cache.present
            && cacheMatches(*state, inputRefreshToken)
            && (!followsRejectedAccess
                || state->cache.result.accessToken
                    != rejectedAccessToken)) {
            return state->cache.result;
        }

        if (!state->activeFlight) {
            flight = std::make_shared<RefreshFlight>();
            flight->requestedToken = inputRefreshToken;
            flight->effectiveToken =
                state->canonicalToken.isEmpty()
                    ? inputRefreshToken
                    : state->canonicalToken;
            flight->generation = state->generation;
            state->activeFlight = flight;
            if (state->canonicalToken.isEmpty()) {
                state->canonicalToken = inputRefreshToken;
                rememberToken(*state, inputRefreshToken);
            }
            break;
        }

        flight = state->activeFlight;
        while (!flight->complete && !flight->superseded) {
            flight->condition.wait_for(
                lock, std::chrono::milliseconds(10));
            if (flight->complete || flight->superseded)
                break;
            lock.unlock();
            const bool shouldCancel =
                cancellationRequested(cancelled);
            lock.lock();
            if (shouldCancel) return cancelledResult();
        }
        if (state->authorizationStatus
                != StravaAuthorizationStatus::Active
            || state->authorizationInstallInFlight
            || state->removalInFlight) {
            return failure(QStringLiteral(
                "Strava authorization is not active."));
        }
        if (flight->superseded && !flight->complete)
            return supersededResult();
        if (followsRejectedAccess)
            return flight->result;
        if (flight->requestedToken == inputRefreshToken
            || flight->effectiveToken == inputRefreshToken) {
            return flight->result;
        }
    }

    StravaTokenRefreshResult observed;
    try {
        observed = operation(flight->effectiveToken);
    } catch (...) {
        observed = failure(
            QStringLiteral("Strava token refresh failed."));
    }
    StravaTokenRefreshResult result = normalized(observed);
    if (result.isValid()
        && result.sourceRefreshToken.isEmpty()) {
        result.sourceRefreshToken =
            flight->effectiveToken;
    }
    const StravaTokenRefreshResult publishedResult = result;

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        if (flight->generation == state->generation
            && observed.remoteGrantMayHaveRotated) {
            const bool priorRotationIsUnknown =
                state->remoteGrantMayHaveRotated
                && !state->remoteGrantLatestTokenKnown;
            state->remoteGrantMayHaveRotated = true;
            state->remoteGrantLatestTokenKnown =
                !priorRotationIsUnknown
                && !observed.accessToken.isEmpty()
                && !observed.refreshToken.isEmpty();
            if (!publishedResult.isValid()) {
                ++state->authorizationEpoch;
                state->authorizationStatus =
                    StravaAuthorizationStatus::
                        RevocationPending;
                state->authorizationStatusInitialized = true;
                clearProvisionalStorageInitialization(*state);
                clearCache(*state);
            }
        }
        if (flight->generation == state->generation
            && !observed.accessToken.isEmpty()
            && !observed.refreshToken.isEmpty()) {
            state->latestObservedAccessToken =
                observed.accessToken;
            state->latestObservedRefreshToken =
                observed.refreshToken;
        }
        if (flight->generation == state->generation
            && publishedResult.isValid()) {
            state->durableAccessToken =
                publishedResult.accessToken;
            state->durableRefreshToken =
                publishedResult.refreshToken;
        }
        if (state->authorizationStatus
                != StravaAuthorizationStatus::Active
            || state->authorizationInstallInFlight
            || state->removalInFlight) {
            result = failure(QStringLiteral(
                "Strava authorization is not active."));
        } else if (flight->superseded
            || flight->generation != state->generation) {
            result = supersededResult();
        } else if (result.isValid()) {
            rememberToken(*state, flight->requestedToken);
            rememberToken(*state, flight->effectiveToken);
            rememberToken(*state, result.refreshToken);
            state->canonicalToken = result.refreshToken;
            state->latestObservedAccessToken =
                result.accessToken;
            state->latestObservedRefreshToken =
                result.refreshToken;
            state->remoteGrantMayHaveRotated = false;
            state->remoteGrantLatestTokenKnown = false;
            state->authorizationStatusInitialized = true;
            clearProvisionalStorageInitialization(*state);
            state->cache.present = cacheLifetime.count() > 0;
            state->cache.authoritative = false;
            state->cache.generation = state->generation;
            state->cache.result = result;
            state->cache.storedAt =
                std::chrono::steady_clock::now();
        }
        flight->result = result;
        flight->complete = true;
        if (state->activeFlight == flight)
            state->activeFlight.reset();
    }
    flight->condition.notify_all();
    return result;
}

} // namespace

StravaAuthorizedRequest::StravaAuthorizedRequest() = default;

StravaAuthorizedRequest::StravaAuthorizedRequest(
    std::shared_ptr<StravaAuthorizedRequestLease> requestLease,
    QString accessToken)
    : lease(std::move(requestLease)),
      token(std::move(accessToken))
{
}

StravaAuthorizedRequest::~StravaAuthorizedRequest()
{
    release();
}

StravaAuthorizedRequest::StravaAuthorizedRequest(
    StravaAuthorizedRequest &&other) noexcept
    : lease(std::move(other.lease)),
      token(std::move(other.token))
{
}

StravaAuthorizedRequest &StravaAuthorizedRequest::operator=(
    StravaAuthorizedRequest &&other) noexcept
{
    if (this == &other) return *this;
    release();
    lease = std::move(other.lease);
    token = std::move(other.token);
    return *this;
}

bool StravaAuthorizedRequest::isValid() const
{
    return lease && !token.isEmpty();
}

QString StravaAuthorizedRequest::accessToken() const
{
    return isValid() ? token : QString();
}

bool StravaAuthorizedRequest::authorizeDispatch()
{
    if (!isValid()) return false;
    if (authorizeRequestDispatch(lease))
        return true;
    lease.reset();
    token.clear();
    return false;
}

void StravaAuthorizedRequest::setAbortOperation(
    const std::function<void()> &operation)
{
    registerRequestAbort(lease, operation);
}

void StravaAuthorizedRequest::release()
{
    if (!lease) return;
    releaseRequestLease(lease);
    lease.reset();
    token.clear();
}

bool StravaTokenRefreshResult::isValid() const
{
    return success
        && !accessToken.isEmpty()
        && !refreshToken.isEmpty()
        && error.isEmpty();
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::refresh(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const RefreshOperation &operation,
    const CancellationCheck &cancelled)
{
    return refresh(
        accountKey, inputRefreshToken, operation, cancelled,
        std::chrono::minutes(1));
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::refresh(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const RefreshOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds cacheLifetime)
{
    if (!operation) {
        return failure(
            QStringLiteral("Strava token refresh operation is missing."));
    }
    return refresh(
        accountKey, inputRefreshToken,
        [&operation](const QString &) {
            return operation();
        },
        cancelled, cacheLifetime);
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::refresh(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const CanonicalRefreshOperation &operation,
    const CancellationCheck &cancelled)
{
    return refresh(
        accountKey, inputRefreshToken, operation, cancelled,
        std::chrono::minutes(1));
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::refresh(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const CanonicalRefreshOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds cacheLifetime)
{
    return coordinatedRefresh(
        accountKey,
        inputRefreshToken,
        QString(),
        operation,
        cancelled,
        cacheLifetime);
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::
refreshAfterRejectedAccessToken(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const QString &rejectedAccessToken,
    const CanonicalRefreshOperation &operation,
    const CancellationCheck &cancelled)
{
    return refreshAfterRejectedAccessToken(
        accountKey,
        inputRefreshToken,
        rejectedAccessToken,
        operation,
        cancelled,
        std::chrono::minutes(1));
}

StravaTokenRefreshResult
StravaTokenRefreshCoordinator::
refreshAfterRejectedAccessToken(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const QString &rejectedAccessToken,
    const CanonicalRefreshOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds cacheLifetime)
{
    if (rejectedAccessToken.isEmpty()) {
        return failure(QStringLiteral(
            "The rejected Strava access token is missing."));
    }
    return coordinatedRefresh(
        accountKey,
        inputRefreshToken,
        rejectedAccessToken,
        operation,
        cancelled,
        cacheLifetime);
}

namespace {

bool installAuthorizationDurablyImpl(
    const QString &accountKey,
    const StravaTokenRefreshResult &authorization,
    const std::optional<std::uint64_t> &expectedEpoch,
    const StravaTokenRefreshCoordinator::
        AuthorizationInstallationOperation &operation)
{
    StravaTokenRefreshResult result =
        normalized(authorization);
    if (accountKey.trimmed().isEmpty()
        || !result.isValid()
        || !operation) {
        return false;
    }
    if (result.sourceRefreshToken.isEmpty())
        result.sourceRefreshToken = result.refreshToken;

    RefreshRegistry &value = registry();
    std::shared_ptr<RefreshFlight> active;
    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const std::shared_ptr<AccountState> state =
            accountState(value, accountKey);
        if ((expectedEpoch
                && state->authorizationEpoch
                    != *expectedEpoch)
            || state->authorizationInstallInFlight
            || state->removalInFlight
            || !state->activeRequests.isEmpty()) {
            return false;
        }
        ++state->authorizationEpoch;
        ++state->generation;
        clearProvisionalStorageInitialization(*state);
        state->authorizationInstallInFlight = true;
        active = state->activeFlight;
        const bool refreshMayStillRotate =
            active && !active->complete;
        state->canonicalToken.clear();
        state->knownTokens.clear();
        state->durableAccessToken.clear();
        state->durableRefreshToken.clear();
        state->latestObservedAccessToken.clear();
        state->latestObservedRefreshToken.clear();
        state->remoteGrantMayHaveRotated =
            refreshMayStillRotate;
        state->remoteGrantLatestTokenKnown = false;
        clearCache(*state);
        if (active) active->superseded = true;
    }
    if (active) active->condition.notify_all();

    bool installed = false;
    try {
        installed = operation();
    } catch (...) {
        installed = false;
    }

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const std::shared_ptr<AccountState> state =
            accountState(value, accountKey);
        state->authorizationInstallInFlight = false;
        if (installed) {
            state->authorizationStatus =
                StravaAuthorizationStatus::Active;
            state->authorizationStatusInitialized = true;
            clearProvisionalStorageInitialization(*state);
            state->canonicalToken = result.refreshToken;
            state->durableAccessToken =
                result.accessToken;
            state->durableRefreshToken =
                result.refreshToken;
            state->latestObservedAccessToken =
                result.accessToken;
            state->latestObservedRefreshToken =
                result.refreshToken;
            state->knownTokens.clear();
            rememberToken(*state, result.refreshToken);
            state->cache.present = true;
            state->cache.authoritative = true;
            state->cache.generation = state->generation;
            state->cache.result = result;
            state->cache.storedAt =
                std::chrono::steady_clock::now();
        } else {
            state->authorizationStatus =
                StravaAuthorizationStatus::RevocationPending;
            state->authorizationStatusInitialized = true;
            clearProvisionalStorageInitialization(*state);
        }
    }
    return installed;
}

} // namespace

bool StravaTokenRefreshCoordinator::installAuthorization(
    const QString &accountKey,
    const StravaTokenRefreshResult &authorization)
{
    return installAuthorizationDurably(
        accountKey, authorization, [] { return true; });
}

bool StravaTokenRefreshCoordinator::
installAuthorizationDurably(
    const QString &accountKey,
    const StravaTokenRefreshResult &authorization,
    const AuthorizationInstallationOperation &operation)
{
    return installAuthorizationDurablyImpl(
        accountKey,
        authorization,
        std::nullopt,
        operation);
}

bool StravaTokenRefreshCoordinator::
installAuthorizationDurably(
    const QString &accountKey,
    const StravaTokenRefreshResult &authorization,
    std::uint64_t expectedAuthorizationEpoch,
    const AuthorizationInstallationOperation &operation)
{
    return installAuthorizationDurablyImpl(
        accountKey,
        authorization,
        expectedAuthorizationEpoch,
        operation);
}

StravaAuthorizationRemovalResult
StravaTokenRefreshCoordinator::removeAuthorization(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const AuthorizationRemovalOperation &operation,
    const CancellationCheck &cancelled)
{
    return removeAuthorization(
        accountKey,
        inputRefreshToken,
        [] { return true; },
        operation,
        cancelled,
        std::chrono::seconds(35));
}

StravaAuthorizationRemovalResult
StravaTokenRefreshCoordinator::removeAuthorization(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const AuthorizationRemovalOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds waitTimeout)
{
    return removeAuthorization(
        accountKey,
        inputRefreshToken,
        [] { return true; },
        operation,
        cancelled,
        waitTimeout);
}

StravaAuthorizationRemovalResult
StravaTokenRefreshCoordinator::removeAuthorization(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const AuthorizationPendingOperation &persistPending,
    const AuthorizationRemovalOperation &operation,
    const CancellationCheck &cancelled)
{
    return removeAuthorization(
        accountKey,
        inputRefreshToken,
        persistPending,
        operation,
        cancelled,
        std::chrono::seconds(35));
}

StravaAuthorizationRemovalResult
StravaTokenRefreshCoordinator::removeAuthorization(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const AuthorizationPendingOperation &persistPending,
    const AuthorizationRemovalOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds waitTimeout,
    const std::optional<std::uint64_t>
        &expectedAuthorizationEpoch)
{
    AuthorizationRemovalTransactionOperation transaction;
    if (operation) {
        transaction =
            [operation](
                const QString &remoteRefreshToken,
                const QString &) {
                return operation(remoteRefreshToken);
            };
    }
    return removeAuthorizationTransaction(
        accountKey,
        inputRefreshToken,
        persistPending,
        transaction,
        cancelled,
        waitTimeout,
        expectedAuthorizationEpoch);
}

StravaAuthorizationRemovalResult
StravaTokenRefreshCoordinator::
removeAuthorizationTransaction(
    const QString &accountKey,
    const QString &inputRefreshToken,
    const AuthorizationPendingOperation &persistPending,
    const AuthorizationRemovalTransactionOperation &operation,
    const CancellationCheck &cancelled,
    std::chrono::milliseconds waitTimeout,
    const std::optional<std::uint64_t>
        &expectedAuthorizationEpoch)
{
    if (accountKey.trimmed().isEmpty()
        || !persistPending
        || !operation
        || waitTimeout.count() <= 0) {
        return removalFailure(QStringLiteral(
            "Strava authorization removal inputs are invalid."));
    }
    if (cancellationRequested(cancelled))
        return removalCancelled();

    RefreshRegistry &value = registry();
    std::shared_ptr<AccountState> state;
    QString effectiveRefreshToken;
    QString durableRefreshToken;
    StravaAuthorizationStatus previousAuthorizationStatus =
        StravaAuthorizationStatus::Active;
    bool previousAuthorizationStatusInitialized = false;
    const auto deadline = std::chrono::steady_clock::now()
        + waitTimeout;

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        state = accountState(value, accountKey);
        previousAuthorizationStatus = state->authorizationStatus;
        previousAuthorizationStatusInitialized =
            state->authorizationStatusInitialized;
        if (expectedAuthorizationEpoch
            && state->authorizationEpoch
                != *expectedAuthorizationEpoch) {
            return removalFailure(QStringLiteral(
                "The Strava authorization changed before "
                "disconnect started."));
        }
        if (state->authorizationInstallInFlight) {
            return removalFailure(QStringLiteral(
                "A Strava authorization update is already "
                "in progress."));
        }
        if (state->removalInFlight) {
            return removalFailure(QStringLiteral(
                "Strava authorization removal is already in progress."));
        }
        ++state->authorizationEpoch;
        clearProvisionalStorageInitialization(*state);
        state->removalInFlight = true;
    }

    for (;;) {
        std::vector<std::function<void()>> abortOperations;
        bool drained = false;
        {
            const std::lock_guard<std::mutex> lock(value.mutex);
            for (auto request = state->activeRequests.begin();
                 request != state->activeRequests.end();
                 ++request) {
                if (request->dispatchAuthorized
                    && request->abortOperation
                    && !request->abortIssued) {
                    request->abortIssued = true;
                    abortOperations.push_back(
                        request->abortOperation);
                }
            }
            const std::shared_ptr<RefreshFlight> active =
                state->activeFlight;
            const bool refreshActive =
                active
                && !active->complete
                && !active->superseded;
            drained = !refreshActive
                && state->activeRequests.isEmpty();
        }

        for (const auto &abortOperation : abortOperations) {
            try {
                abortOperation();
            } catch (...) {
            }
        }
        if (drained) break;

        if (cancellationRequested(cancelled)) {
            const std::lock_guard<std::mutex> lock(value.mutex);
            state->removalInFlight = false;
            reconcileActiveAuthorization(*state);
            state->requestCondition.notify_all();
            return removalCancelled();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            const std::lock_guard<std::mutex> lock(value.mutex);
            state->removalInFlight = false;
            reconcileActiveAuthorization(*state);
            state->requestCondition.notify_all();
            return removalFailure(QStringLiteral(
                "Timed out waiting for active Strava requests."));
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }

    if (cancellationRequested(cancelled)) {
        const std::lock_guard<std::mutex> lock(value.mutex);
        state->removalInFlight = false;
        reconcileActiveAuthorization(*state);
        state->requestCondition.notify_all();
        return removalCancelled();
    }

    bool pendingStored = false;
    try {
        pendingStored = persistPending();
    } catch (...) {
        pendingStored = false;
    }
    if (!pendingStored) {
        const std::lock_guard<std::mutex> lock(value.mutex);
        state->removalInFlight = false;
        reconcileActiveAuthorization(*state);
        state->requestCondition.notify_all();
        return removalFailure(QStringLiteral(
            "The pending Strava disconnect could not be stored."));
    }

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        state->authorizationStatus =
            StravaAuthorizationStatus::RevocationPending;
        state->authorizationStatusInitialized = true;
        clearProvisionalStorageInitialization(*state);
    }

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const bool inputIsKnown =
            inputRefreshToken.isEmpty()
            || state->knownTokens.contains(inputRefreshToken);
        if (inputIsKnown
            && !state->durableRefreshToken.isEmpty()) {
            durableRefreshToken =
                state->durableRefreshToken;
        } else {
            durableRefreshToken = inputRefreshToken;
        }
        if (inputIsKnown
            && !state->latestObservedRefreshToken.isEmpty()) {
            effectiveRefreshToken =
                state->latestObservedRefreshToken;
        } else if (inputIsKnown
                   && !state->canonicalToken.isEmpty()) {
            effectiveRefreshToken =
                state->canonicalToken;
        } else {
            effectiveRefreshToken = inputRefreshToken;
        }
    }

    StravaAuthorizationRemovalResult result;
    try {
        result = operation(
            effectiveRefreshToken,
            durableRefreshToken);
    } catch (...) {
        result = removalFailure(QStringLiteral(
            "Strava authorization removal failed."));
    }
    if (result.removed && !result.error.isEmpty()) {
        result = removalFailure(QStringLiteral(
            "Strava authorization removal returned "
            "an invalid result."));
    } else if (!result.removed) {
        result.cleanupPending = false;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "Strava authorization removal failed.");
        }
    }

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        state->removalInFlight = false;
        result.remoteAuthorizationMayRemain =
            result.remoteAuthorizationMayRemain
            || (state->remoteGrantMayHaveRotated
                && !state->remoteGrantLatestTokenKnown);
        if (result.isSuccess()) {
            ++state->generation;
            state->authorizationStatus =
                StravaAuthorizationStatus::Revoked;
            state->authorizationStatusInitialized = true;
            clearProvisionalStorageInitialization(*state);
            state->canonicalToken.clear();
            state->knownTokens.clear();
            state->durableAccessToken.clear();
            state->durableRefreshToken.clear();
            state->latestObservedAccessToken.clear();
            state->latestObservedRefreshToken.clear();
            state->remoteGrantMayHaveRotated = false;
            state->remoteGrantLatestTokenKnown = false;
            clearCache(*state);
            state->activeFlight.reset();
        } else if (result.authorizationUnchanged) {
            state->authorizationStatus = previousAuthorizationStatus;
            state->authorizationStatusInitialized =
                previousAuthorizationStatusInitialized;
            clearProvisionalStorageInitialization(*state);
        } else {
            state->authorizationStatus =
                StravaAuthorizationStatus::RevocationPending;
            state->authorizationStatusInitialized = true;
            clearProvisionalStorageInitialization(*state);
        }
        state->requestCondition.notify_all();
    }
    return result;
}

void StravaTokenRefreshCoordinator::
initializeAuthorization(
    const QString &accountKey,
    StravaAuthorizationStatus status,
    const QString &accessToken,
    const QString &refreshToken,
    bool remoteGrantMayHaveRotated)
{
    if (accountKey.trimmed().isEmpty()) return;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const std::shared_ptr<AccountState> state =
        accountState(value, accountKey);
    if (state->authorizationStatusInitialized)
        return;

    applyStoredAuthorization(
        *state, status, accessToken, refreshToken,
        remoteGrantMayHaveRotated);
    clearProvisionalStorageInitialization(*state);
}

bool StravaTokenRefreshCoordinator::
reconcileAuthorizationFromStorage(
    const QString &accountKey,
    StravaAuthorizationStatus status,
    const QString &accessToken,
    const QString &refreshToken,
    bool remoteGrantMayHaveRotated,
    bool authoritative)
{
    if (accountKey.trimmed().isEmpty()) return false;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const std::shared_ptr<AccountState> state =
        accountState(value, accountKey);
    if (!state->authorizationStatusInitialized) {
        applyStoredAuthorization(
            *state, status, accessToken, refreshToken,
            remoteGrantMayHaveRotated);
        state->storageInitializationProvisional = !authoritative;
        state->provisionalGeneration = state->generation;
        state->provisionalAuthorizationEpoch =
            state->authorizationEpoch;
        return true;
    }
    if (!state->storageInitializationProvisional)
        return false;
    if (!authoritative)
        return true;
    if (state->generation != state->provisionalGeneration
        || state->authorizationEpoch
            != state->provisionalAuthorizationEpoch
        || state->authorizationInstallInFlight
        || state->removalInFlight
        || state->activeFlight
        || !state->activeRequests.isEmpty()) {
        return false;
    }

    ++state->generation;
    ++state->authorizationEpoch;
    applyStoredAuthorization(
        *state, status, accessToken, refreshToken,
        remoteGrantMayHaveRotated);
    clearProvisionalStorageInitialization(*state);
    return true;
}

bool StravaTokenRefreshCoordinator::
adoptAuthoritativeAuthorizationFromStorage(
    const QString &accountKey,
    const StravaAuthorizationSnapshot &expected,
    StravaAuthorizationStatus status,
    const QString &accessToken,
    const QString &refreshToken,
    bool remoteGrantMayHaveRotated)
{
    const bool active =
        status == StravaAuthorizationStatus::Active;
    if (accountKey.trimmed().isEmpty()
        || (active
            && (accessToken.isEmpty()
                || refreshToken.isEmpty()))) {
        return false;
    }

    RefreshRegistry &value = registry();
    std::shared_ptr<RefreshFlight> supersededFlight;
    std::vector<std::function<void()>> abortOperations;
    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const std::shared_ptr<AccountState> state =
            accountState(value, accountKey);
        const QString authoritativeAccess =
            active ? accessToken : QString();
        const QString authoritativeRefresh =
            active ? refreshToken : QString();
        const bool expectedStillCurrent =
            state->authorizationStatusInitialized
            && !state->storageInitializationProvisional
            && state->authorizationStatus == expected.status
            && state->durableAccessToken == expected.accessToken
            && state->durableRefreshToken == expected.refreshToken
            && state->authorizationEpoch == expected.epoch
            && state->remoteGrantMayHaveRotated
                == expected.remoteGrantMayHaveRotated;
        if (!expectedStillCurrent) return false;
        if (state->authorizationStatus == status
            && state->durableAccessToken == authoritativeAccess
            && state->durableRefreshToken == authoritativeRefresh
            && state->remoteGrantMayHaveRotated
                == remoteGrantMayHaveRotated) {
            return true;
        }
        if (state->authorizationInstallInFlight
            || state->removalInFlight
            || state->activeFlight
            || (active && !state->activeRequests.isEmpty())) {
            return false;
        }

        ++state->generation;
        ++state->authorizationEpoch;
        if (!active) {
            supersededFlight = state->activeFlight;
            if (supersededFlight)
                supersededFlight->superseded = true;
            for (auto request = state->activeRequests.begin();
                 request != state->activeRequests.end();
                 ++request) {
                if (request->dispatchAuthorized
                    && request->abortOperation
                    && !request->abortIssued) {
                    request->abortIssued = true;
                    abortOperations.push_back(
                        request->abortOperation);
                }
            }
        }
        applyStoredAuthorization(
            *state, status,
            authoritativeAccess, authoritativeRefresh,
            remoteGrantMayHaveRotated);
        clearProvisionalStorageInitialization(*state);
    }
    if (supersededFlight)
        supersededFlight->condition.notify_all();
    for (const auto &abortOperation : abortOperations) {
        try {
            abortOperation();
        } catch (...) {
        }
    }
    return true;
}

bool StravaTokenRefreshCoordinator::
reconcileAuthoritativeAuthorizationFromStorage(
    const QString &accountKey,
    StravaAuthorizationStatus status,
    const QString &accessToken,
    const QString &refreshToken,
    bool remoteGrantMayHaveRotated,
    bool *authorizationActive)
{
    if (authorizationActive) *authorizationActive = false;
    StravaAuthorizationStatus effectiveStatus = status;
    QString effectiveAccessToken = accessToken;
    QString effectiveRefreshToken = refreshToken;
    bool effectiveUncertainty = remoteGrantMayHaveRotated;
    if (status == StravaAuthorizationStatus::Active
        && (accessToken.isEmpty() || refreshToken.isEmpty())) {
        effectiveStatus = StravaAuthorizationStatus::RevocationPending;
        effectiveAccessToken.clear();
        effectiveRefreshToken.clear();
        effectiveUncertainty = true;
    }

    const StravaAuthorizationSnapshot expected =
        authorizationSnapshot(accountKey);
    const bool reconciled =
        adoptAuthoritativeAuthorizationFromStorage(
            accountKey,
            expected,
            effectiveStatus,
            effectiveAccessToken,
            effectiveRefreshToken,
            effectiveUncertainty);
    if (reconciled && authorizationActive) {
        *authorizationActive =
            effectiveStatus == StravaAuthorizationStatus::Active;
    }
    return reconciled;
}

std::function<void()>
StravaTokenRefreshCoordinator::threadBoundAbortOperation(
    QObject *target,
    const std::function<void(QObject *)> &operation)
{
    if (!target || !operation
        || QThread::currentThread() != target->thread()) {
        return {};
    }

    auto relay = std::shared_ptr<ThreadBoundAbortRelay>(
        new ThreadBoundAbortRelay(target, operation),
        [](ThreadBoundAbortRelay *value) {
            if (value) value->deleteLater();
        });
    return [relay] {
#ifdef GC_STRAVA_TOKEN_REFRESH_TEST_HOOKS
        runThreadBoundDispatchHook();
#endif
        QMetaObject::invokeMethod(
            relay.get(),
            [relay] { relay->dispatch(); },
            Qt::QueuedConnection);
    };
}

#ifdef GC_STRAVA_TOKEN_REFRESH_TEST_HOOKS
void StravaTokenRefreshCoordinator::
setThreadBoundDispatchHookForTest(std::function<void()> hook)
{
    const std::lock_guard<std::mutex> lock(
        threadBoundDispatchHookMutex);
    threadBoundDispatchHook = std::move(hook);
}
#endif

void StravaTokenRefreshCoordinator::
initializeAuthorizationStatus(
    const QString &accountKey,
    StravaAuthorizationStatus status)
{
    initializeAuthorization(
        accountKey,
        status,
        QString(),
        QString());
}

StravaAuthorizationStatus
StravaTokenRefreshCoordinator::authorizationStatus(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty())
        return StravaAuthorizationStatus::Revoked;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    return accountState(value, accountKey)
        ->authorizationStatus;
}

StravaAuthorizationStatus
StravaTokenRefreshCoordinator::authorizationStatusFromStorage(
    const QString &stored)
{
    if (stored.isEmpty()
        || stored == QStringLiteral("active")) {
        return StravaAuthorizationStatus::Active;
    }
    if (stored == QStringLiteral("revoked"))
        return StravaAuthorizationStatus::Revoked;
    return StravaAuthorizationStatus::RevocationPending;
}

bool StravaTokenRefreshCoordinator::authorizationUsable(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty())
        return false;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const std::shared_ptr<AccountState> state =
        accountState(value, accountKey);
    return state->authorizationStatus
            == StravaAuthorizationStatus::Active
        && !state->authorizationInstallInFlight
        && !state->removalInFlight;
}

std::uint64_t
StravaTokenRefreshCoordinator::authorizationEpoch(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty())
        return 0;

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    return accountState(value, accountKey)
        ->authorizationEpoch;
}

StravaAuthorizationSnapshot
StravaTokenRefreshCoordinator::authorizationSnapshot(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty())
        return {};

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const std::shared_ptr<AccountState> state =
        accountState(value, accountKey);
    StravaAuthorizationSnapshot snapshot;
    snapshot.status = state->authorizationStatus;
    snapshot.accessToken = state->durableAccessToken;
    snapshot.refreshToken = state->durableRefreshToken;
    snapshot.epoch = state->authorizationEpoch;
    snapshot.remoteGrantMayHaveRotated =
        state->remoteGrantMayHaveRotated;
    return snapshot;
}

StravaAuthorizedRequest
StravaTokenRefreshCoordinator::beginAuthorizedRequest(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty())
        return {};

    RefreshRegistry &value = registry();
    const std::lock_guard<std::mutex> lock(value.mutex);
    const std::shared_ptr<AccountState> state =
        accountState(value, accountKey);
    if (state->authorizationStatus
            != StravaAuthorizationStatus::Active
        || state->authorizationInstallInFlight
        || state->removalInFlight
        || state->durableAccessToken.isEmpty()) {
        return {};
    }

    const std::uint64_t requestId =
        ++state->nextRequestId;
    ActiveRequest request;
    request.generation = state->generation;
    state->activeRequests.insert(requestId, request);

    auto requestLease =
        std::make_shared<StravaAuthorizedRequestLease>();
    requestLease->accountKey = accountKey;
    requestLease->requestId = requestId;
    requestLease->generation = state->generation;
    return StravaAuthorizedRequest(
        std::move(requestLease),
        state->durableAccessToken);
}

void StravaTokenRefreshCoordinator::invalidate(
    const QString &accountKey)
{
    if (accountKey.trimmed().isEmpty()) return;

    RefreshRegistry &value = registry();
    std::shared_ptr<RefreshFlight> active;
    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const std::shared_ptr<AccountState> state =
            accountState(value, accountKey);
        ++state->generation;
        clearProvisionalStorageInitialization(*state);
        state->canonicalToken.clear();
        state->knownTokens.clear();
        state->durableAccessToken.clear();
        state->durableRefreshToken.clear();
        state->latestObservedAccessToken.clear();
        state->latestObservedRefreshToken.clear();
        clearCache(*state);
        active = state->activeFlight;
        if (active) active->superseded = true;
    }
    if (active) active->condition.notify_all();
}
