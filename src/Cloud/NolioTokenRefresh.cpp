/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "NolioTokenRefresh.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace {

struct RefreshFlight
{
    QString inputToken;
    bool complete = false;
    NolioTokenRefreshResult result;
    std::condition_variable condition;
};

std::mutex refreshMutex;
std::shared_ptr<RefreshFlight> activeFlight;
QString cachedInputToken;
NolioTokenRefreshResult cachedResult;
bool hasCachedResult = false;
std::chrono::steady_clock::time_point cachedAt;

} // namespace

NolioTokenRefreshResult
NolioTokenRefreshCoordinator::refresh(
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled)
{
    return refresh(
        inputRefreshToken, operation, cancelled,
        std::chrono::minutes(1));
}

NolioTokenRefreshResult
NolioTokenRefreshCoordinator::refresh(
        const QString &inputRefreshToken,
        const RefreshOperation &operation,
        const CancellationCheck &cancelled,
        std::chrono::milliseconds cacheLifetime)
{
    std::shared_ptr<RefreshFlight> flight;
    bool leader = false;

    for (;;) {
        std::unique_lock<std::mutex> lock(refreshMutex);
        if (hasCachedResult) {
            const bool cacheMatches =
                    cachedInputToken == inputRefreshToken;
            const bool cacheIsFresh =
                    cacheLifetime.count() > 0
                    && std::chrono::steady_clock::now() - cachedAt
                        < cacheLifetime;
            if (cacheMatches && cacheIsFresh)
                return cachedResult;

            cachedInputToken.clear();
            cachedResult = {};
            hasCachedResult = false;
        }

        if (!activeFlight) {
            flight = std::make_shared<RefreshFlight>();
            flight->inputToken = inputRefreshToken;
            activeFlight = flight;
            leader = true;
            break;
        }

        flight = activeFlight;
        while (!flight->complete) {
            if (cancelled && cancelled()) {
                NolioTokenRefreshResult result;
                result.error = QStringLiteral("Token refresh cancelled.");
                return result;
            }
            flight->condition.wait_for(
                lock, std::chrono::milliseconds(10));
        }
        if (flight->inputToken == inputRefreshToken)
            return flight->result;
    }

    NolioTokenRefreshResult result;
    if (leader) {
        try {
            result = operation();
        } catch (...) {
            result.error = QStringLiteral("Token refresh failed.");
        }
    }

    {
        const std::lock_guard<std::mutex> lock(refreshMutex);
        flight->result = result;
        flight->complete = true;
        if (result.success && cacheLifetime.count() > 0) {
            cachedInputToken = inputRefreshToken;
            cachedResult = result;
            hasCachedResult = true;
            cachedAt = std::chrono::steady_clock::now();
        }
        if (activeFlight == flight)
            activeFlight.reset();
    }
    flight->condition.notify_all();
    return result;
}
