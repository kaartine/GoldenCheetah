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
#include <QStringList>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

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

struct AccountState
{
    std::uint64_t generation = 0;
    std::shared_ptr<RefreshFlight> activeFlight;
    QString canonicalToken;
    QStringList knownTokens;
    CachedGrant cache;
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

StravaTokenRefreshResult failure(const QString &error)
{
    return {
        false, QString(), QString(), error, QString()
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

} // namespace

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
    if (accountKey.trimmed().isEmpty()
        || inputRefreshToken.isEmpty()
        || !operation) {
        return failure(
            QStringLiteral("Strava token refresh inputs are invalid."));
    }
    if (cancellationRequested(cancelled)) return cancelledResult();

    RefreshRegistry &value = registry();
    std::shared_ptr<AccountState> state;
    std::shared_ptr<RefreshFlight> flight;

    for (;;) {
        std::unique_lock<std::mutex> lock(value.mutex);
        state = accountState(value, accountKey);

        if (state->cache.present
            && state->cache.generation != state->generation) {
            clearCache(*state);
        }
        if (state->cache.present
            && !cacheIsFresh(state->cache, cacheLifetime)) {
            clearCache(*state);
        }
        if (state->cache.present
            && cacheMatches(*state, inputRefreshToken)) {
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
        if (flight->superseded && !flight->complete)
            return supersededResult();
        if (flight->requestedToken == inputRefreshToken
            || flight->effectiveToken == inputRefreshToken) {
            return flight->result;
        }
    }

    StravaTokenRefreshResult result;
    try {
        result = normalized(operation(flight->effectiveToken));
    } catch (...) {
        result = failure(
            QStringLiteral("Strava token refresh failed."));
    }

    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        if (flight->superseded
            || flight->generation != state->generation) {
            result = supersededResult();
        } else if (result.isValid()) {
            if (result.sourceRefreshToken.isEmpty()) {
                result.sourceRefreshToken =
                    flight->effectiveToken;
            }
            rememberToken(*state, flight->requestedToken);
            rememberToken(*state, flight->effectiveToken);
            rememberToken(*state, result.refreshToken);
            state->canonicalToken = result.refreshToken;
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

bool StravaTokenRefreshCoordinator::installAuthorization(
    const QString &accountKey,
    const StravaTokenRefreshResult &authorization)
{
    StravaTokenRefreshResult result =
        normalized(authorization);
    if (accountKey.trimmed().isEmpty() || !result.isValid())
        return false;
    if (result.sourceRefreshToken.isEmpty())
        result.sourceRefreshToken = result.refreshToken;

    RefreshRegistry &value = registry();
    std::shared_ptr<RefreshFlight> active;
    {
        const std::lock_guard<std::mutex> lock(value.mutex);
        const std::shared_ptr<AccountState> state =
            accountState(value, accountKey);
        ++state->generation;
        state->canonicalToken = result.refreshToken;
        state->knownTokens.clear();
        rememberToken(*state, result.refreshToken);
        state->cache.present = true;
        state->cache.authoritative = true;
        state->cache.generation = state->generation;
        state->cache.result = result;
        state->cache.storedAt =
            std::chrono::steady_clock::now();
        active = state->activeFlight;
        if (active) active->superseded = true;
    }
    if (active) active->condition.notify_all();
    return true;
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
        state->canonicalToken.clear();
        state->knownTokens.clear();
        clearCache(*state);
        active = state->activeFlight;
        if (active) active->superseded = true;
    }
    if (active) active->condition.notify_all();
}
