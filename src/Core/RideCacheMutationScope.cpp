/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCacheMutationScope.h"

#include "Athlete.h"
#include "Context.h"
#include "Estimator.h"
#include "RideCache.h"
#include "RideCacheModel.h"
#include "RideItem.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <algorithm>

struct RideCacheMutationScope::State
{
    QPointer<RideCache> cache;
    QPointer<Context> context;
    QPointer<Athlete> athlete;
    QPointer<RideCacheModel> model;
    QPointer<Estimator> estimator;
    std::shared_ptr<bool> inProgress;
    quint64 resumeGeneration = 0;
    quint64 modelReservation = 0;
    bool acquired = false;
    bool workersQuiesced = false;
    bool resetActive = false;
    bool ready = false;
};

RideCacheMutationScope::RideCacheMutationScope(
    RideCache *cache,
    QString &error)
    : state_(new State)
{
    error.clear();
    if (!cache || QThread::currentThread() != cache->thread()) {
        error = QObject::tr(
            "Activities can only be changed on the cache thread");
        return;
    }
    if (cache->activityMutationIsBlocked()) {
        error = QObject::tr(
            "Another activity operation is already in progress");
        return;
    }

    state_->cache = cache;
    state_->context = cache->context;
    state_->athlete = state_->context
        ? state_->context->athlete : nullptr;
    state_->model = cache->model_;
    state_->estimator = cache->estimator;
    state_->inProgress = cache->removalInProgress_;
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection is no longer available");
        return;
    }
    if (!state_->inProgress || *state_->inProgress) {
        error = QObject::tr(
            "Another activity operation is already in progress");
        return;
    }

    *state_->inProgress = true;
    state_->acquired = true;
    state_->resumeGeneration =
        ++cache->mutationResumeGeneration_;

    cache->cancel();
    state_->workersQuiesced = true;
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection disappeared while refresh was being cancelled");
        return;
    }

    state_->estimator->stop();
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection disappeared while estimates were being stopped");
        return;
    }

    if (!state_->cache->purgeDestroyedModelRows()) {
        error = ownersStable()
            ? QObject::tr(
                "Destroyed activity rows could not be removed before the operation")
            : QObject::tr(
                "The activity collection disappeared while destroyed rows were being removed");
        return;
    }
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection disappeared while destroyed rows were being removed");
        return;
    }

    state_->modelReservation =
        state_->model->reserveCacheMutation();
    if (state_->modelReservation == 0) {
        error = QObject::tr(
            "The activity list is busy and cannot reserve this operation");
        return;
    }
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection disappeared while its model was being reserved");
        return;
    }

    state_->ready = true;
}

RideCacheMutationScope::~RideCacheMutationScope()
{
    if (!state_ || !state_->acquired) return;

    if (state_->resetActive) endReset();
    if (state_->model && state_->modelReservation != 0) {
        state_->model->releaseCacheMutation(
            state_->modelReservation);
    }
    state_->modelReservation = 0;

    if (ownersStable()
        && !state_->cache->purgeDestroyedModelRows()) {
        qWarning().noquote() << QObject::tr(
            "Destroyed activity rows could not be removed after the operation");
    }
    if (ownersStable()) {
        state_->cache->removalRefreshPending_ = false;
    }
    if (state_->inProgress) *state_->inProgress = false;
    state_->acquired = false;

    if (!state_->workersQuiesced || !ownersStable())
        return;

    const QPointer<RideCache> cache = state_->cache;
    const QPointer<Context> context = state_->context;
    const QPointer<Athlete> athlete = state_->athlete;
    const QPointer<RideCacheModel> model = state_->model;
    const QPointer<Estimator> estimator = state_->estimator;
    const std::shared_ptr<bool> inProgress = state_->inProgress;
    const quint64 generation = state_->resumeGeneration;
    const bool queued = QMetaObject::invokeMethod(
        cache.data(),
        [cache, context, athlete, model, estimator,
         inProgress, generation] {
            const auto resumeIsCurrent = [&] {
                return cache && context && athlete && model
                    && estimator
                    && cache->context == context.data()
                    && context->athlete == athlete.data()
                    && athlete->context == context.data()
                    && athlete->rideCache == cache.data()
                    && cache->model_ == model.data()
                    && cache->estimator == estimator.data()
                    && cache->mutationResumeGeneration_
                        == generation
                    && (!inProgress || !*inProgress)
                    && (!cache->replacementRefreshBlocked_
                        || !*cache->replacementRefreshBlocked_);
            };
            if (!resumeIsCurrent()) return;
            cache->refresh();
            if (!resumeIsCurrent()) return;
            estimator->refresh();
        },
        Qt::QueuedConnection);
    if (!queued) {
        qWarning().noquote() << QObject::tr(
            "Activity background refresh could not be resumed");
    }
}

bool RideCacheMutationScope::ready() const
{
    return state_ && state_->ready
        && state_->modelReservation != 0
        && ownersStable();
}

bool RideCacheMutationScope::ownersStable() const
{
    return state_ && state_->cache && state_->context
        && state_->athlete && state_->model && state_->estimator
        && state_->cache->context == state_->context.data()
        && state_->context->athlete == state_->athlete.data()
        && state_->athlete->context == state_->context.data()
        && state_->athlete->rideCache == state_->cache.data()
        && state_->cache->model_ == state_->model.data()
        && state_->cache->estimator == state_->estimator.data();
}

bool RideCacheMutationScope::beginReset(QString &error)
{
    error.clear();
    if (!ready()) {
        error = QObject::tr(
            "The activity collection is no longer available");
        return false;
    }
    if (state_->resetActive) {
        error = QObject::tr(
            "The activity model reset is already active");
        return false;
    }

    const QPointer<RideCacheModel> model = state_->model;
    if (!model || !model->beginReservedReset(
            state_->modelReservation)) {
        error = QObject::tr(
            "The reserved activity model cannot begin its reset");
        return false;
    }
    state_->resetActive = true;
    if (!ownersStable() || state_->cache->model_ != model.data()) {
        endReset();
        error = QObject::tr(
            "The activity collection disappeared while reordering began");
        return false;
    }
    return true;
}

void RideCacheMutationScope::endReset()
{
    if (!state_ || !state_->resetActive) return;
    const QPointer<RideCacheModel> model = state_->model;
    state_->resetActive = false;
    if (model) model->endReset();
}

bool RideCacheMutationScope::resetAndSort(
    QString &error,
    const std::function<bool()> &publish)
{
    error.clear();
    const QPointer<RideCacheModel> model = state_->model;
    if (!beginReset(error)) return false;
    const auto finishReset = [this] { endReset(); };

    state_->cache->purgeDestroyedRowsInsideModelReset();
    if (!ownersStable() || state_->cache->model_ != model.data()) {
        finishReset();
        error = QObject::tr(
            "The activity collection disappeared while destroyed rows were being removed");
        return false;
    }
    if (publish && !publish()) {
        finishReset();
        if (error.isEmpty()) {
            error = QObject::tr(
                "The activity changes could not be published");
        }
        return false;
    }
    if (!ownersStable() || state_->cache->model_ != model.data()) {
        finishReset();
        error = QObject::tr(
            "The activity collection disappeared while changes were being published");
        return false;
    }

    state_->cache->purgeDestroyedRowsInsideModelReset();
    if (!ownersStable() || state_->cache->model_ != model.data()) {
        finishReset();
        error = QObject::tr(
            "The activity collection disappeared before changes were reordered");
        return false;
    }
    std::sort(
        state_->cache->rides_.begin(),
        state_->cache->rides_.end(),
        [](const RideItem *left, const RideItem *right) {
            return left->dateTime < right->dateTime;
        });
    finishReset();
    if (!ownersStable()) {
        error = QObject::tr(
            "The activity collection disappeared while reordering finished");
        return false;
    }
    return true;
}
