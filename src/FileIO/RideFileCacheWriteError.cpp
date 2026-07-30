/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFileCacheWriteError.h"

#include <QObject>

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

struct RideFileCacheWriteErrorCoordinator::SharedState
{
    enum class DeliveryState {
        Idle,
        Dispatching,
        Scheduled,
        Delivered
    };

    std::mutex mutex;
    DeliveryState deliveryState = DeliveryState::Idle;
    std::uint64_t generation = 0;
    std::optional<QString> pendingMessage;
};

RideFileCacheWriteErrorCoordinator::RideFileCacheWriteErrorCoordinator()
    : state_(std::make_shared<SharedState>())
{
}

RideFileCacheWriteErrorCoordinator::~RideFileCacheWriteErrorCoordinator() =
    default;

bool
RideFileCacheWriteErrorCoordinator::queueForOwner(
    QObject *owner,
    Delivery delivery)
{
    if (!owner || !delivery)
        return false;
    return QMetaObject::invokeMethod(
        owner,
        std::move(delivery),
        Qt::QueuedConnection);
}

RideFileCacheWriteErrorCoordinator::ReportResult
RideFileCacheWriteErrorCoordinator::report(
    const QString &cachePath,
    const QString &detail,
    const Dispatch &dispatch,
    const Notify &notify)
{
    if (!dispatch || !notify)
        return ReportResult::DispatchFailed;

    const QString path = cachePath.isEmpty()
        ? QStringLiteral("<unknown>")
        : cachePath;
    const QString reason = detail.isEmpty()
        ? QStringLiteral("Unknown error")
        : detail;
    const QString message =
        QStringLiteral("Cannot create cache file %1: %2.")
            .arg(path, reason);
    const Dispatch activeDispatch = dispatch;
    const Notify activeNotify = notify;
    const std::shared_ptr<SharedState> state = state_;
    QString currentMessage = message;
    std::uint64_t generation = 0;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->deliveryState
            == SharedState::DeliveryState::Dispatching) {
            if (!state->pendingMessage)
                state->pendingMessage.emplace(
                    std::move(currentMessage));
            return ReportResult::Coalesced;
        }
        if (state->deliveryState != SharedState::DeliveryState::Idle)
            return ReportResult::Coalesced;
        state->deliveryState = SharedState::DeliveryState::Dispatching;
        generation = ++state->generation;
    }

    constexpr int MaximumDispatchAttempts = 2;
    int dispatchAttempts = 0;
    for (;;) {
        ++dispatchAttempts;
        Delivery delivery;
        bool accepted = false;
        try {
            delivery = [
                state,
                generation,
                message = currentMessage,
                notify = activeNotify
            ]() {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->generation != generation
                        || (state->deliveryState
                                != SharedState::DeliveryState::Dispatching
                            && state->deliveryState
                                != SharedState::DeliveryState::Scheduled)) {
                        return;
                    }
                    state->deliveryState =
                        SharedState::DeliveryState::Delivered;
                }
                notify(message);
            };
            accepted = activeDispatch(std::move(delivery));
        } catch (...) {
            std::optional<QString> pendingToDestroy;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->generation == generation) {
                    pendingToDestroy =
                        std::move(state->pendingMessage);
                    state->pendingMessage.reset();
                    if (state->deliveryState
                        == SharedState::DeliveryState::Dispatching) {
                        state->deliveryState =
                            SharedState::DeliveryState::Idle;
                    }
                }
            }
            pendingToDestroy.reset();
            throw;
        }

        std::optional<QString> retryMessage;
        std::optional<QString> pendingToDestroy;
        ReportResult result = ReportResult::Scheduled;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->generation == generation) {
                if (state->deliveryState
                    == SharedState::DeliveryState::Delivered) {
                    pendingToDestroy =
                        std::move(state->pendingMessage);
                    state->pendingMessage.reset();
                } else if (accepted) {
                    state->deliveryState =
                        SharedState::DeliveryState::Scheduled;
                    pendingToDestroy =
                        std::move(state->pendingMessage);
                    state->pendingMessage.reset();
                } else if (state->pendingMessage
                           && dispatchAttempts
                               < MaximumDispatchAttempts) {
                    retryMessage =
                        std::move(state->pendingMessage);
                    state->pendingMessage.reset();
                    generation = ++state->generation;
                } else {
                    state->deliveryState =
                        SharedState::DeliveryState::Idle;
                    pendingToDestroy =
                        std::move(state->pendingMessage);
                    state->pendingMessage.reset();
                    result = ReportResult::DispatchFailed;
                }
            }
        }
        pendingToDestroy.reset();

        if (!retryMessage)
            return result;
        currentMessage = std::move(*retryMessage);
    }
}
