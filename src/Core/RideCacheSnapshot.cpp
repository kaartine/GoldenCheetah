/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCacheSnapshot.h"
#include "RideCacheStartup.h"

#include "IntervalItem.h"
#include "RideFile.h"
#include "RideItem.h"

#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_swappable_v<QColor>);
static_assert(std::is_nothrow_swappable_v<QString>);
static_assert(std::is_nothrow_swappable_v<QStringList>);
static_assert(std::is_nothrow_swappable_v<QVector<double>>);
static_assert(std::is_nothrow_swappable_v<QMap<int, double>>);
static_assert(std::is_nothrow_swappable_v<QMap<QString, QString>>);
static_assert(
    std::is_nothrow_swappable_v<QMap<QString, QStringList>>);
static_assert(std::is_nothrow_swappable_v<QList<IntervalItem *>>);

#ifdef GC_RIDE_ITEM_REFRESH_TEST_HOOKS
static int materializationFailureCountdown = -1;

void
RideItemComputedState::failMaterializationAfterForTest(int intervals)
{
    materializationFailureCountdown = intervals;
}
#endif

RideItemComputedState::RideItemComputedState() = default;
RideItemComputedState::~RideItemComputedState() = default;
RideItemComputedState::RideItemComputedState(
    RideItemComputedState &&) noexcept = default;
RideItemComputedState &RideItemComputedState::operator=(
    RideItemComputedState &&) noexcept = default;

RideCacheItemSnapshot::RideCacheItemSnapshot() = default;
RideCacheItemSnapshot::~RideCacheItemSnapshot() = default;
RideCacheItemSnapshot::RideCacheItemSnapshot(
    RideCacheItemSnapshot &&) noexcept = default;
RideCacheItemSnapshot &RideCacheItemSnapshot::operator=(
    RideCacheItemSnapshot &&) noexcept = default;

RideItemComputedState
RideItemComputedState::takeFrom(RideItem &source)
{
    RideItemComputedState state;
    // The startup parser reuses its RideItem and resets these containers
    // in place after each snapshot.  Keep the source containers allocated;
    // only the detached state is move-only.
    state.metrics_ = source.metrics_;
    state.counts_ = source.count_;
    state.stdmeans_ = source.stdmean_;
    state.stdvariances_ = source.stdvariance_;
    state.metadata_ = source.metadata_;
    state.xdata_ = source.xdata_;
    state.errors_ = source.errors_;

    state.intervals_.reserve(
        static_cast<std::size_t>(source.intervals_.size()));
    for (IntervalItem *interval : source.intervals_) {
        if (!interval) continue;
        RideItemIntervalState value;
        value.name = std::move(interval->name);
        value.type = static_cast<int>(interval->type);
        value.start = interval->start;
        value.stop = interval->stop;
        value.startKm = interval->startKM;
        value.stopKm = interval->stopKM;
        value.displaySequence = interval->displaySequence;
        value.color = std::move(interval->color);
        value.route = std::move(interval->route);
        value.test = interval->test;
        value.selected = interval->selected;
        value.metrics = std::move(interval->metrics_);
        value.counts = std::move(interval->count_);
        value.stdmeans = std::move(interval->stdmean_);
        value.stdvariances = std::move(interval->stdvariance_);
        if (source.ride_
            && interval->type == RideFileInterval::USER
            && interval->rideInterval) {
            value.userIntervalOrdinal =
                source.ride_->intervals().indexOf(
                    interval->rideInterval);
        }
        state.intervals_.push_back(std::move(value));
        delete interval;
    }
    source.intervals_.clear();

    state.zoneRange_ = source.zoneRange;
    state.hrZoneRange_ = source.hrZoneRange;
    state.paceZoneRange_ = source.paceZoneRange;
    state.fingerprint_ = source.fingerprint;
    state.metadataCrc_ = source.metacrc;
    state.crc_ = source.crc;
    state.timestamp_ = source.timestamp;
    state.databaseVersion_ = source.dbversion;
    state.userDatabaseVersion_ = source.udbversion;
    state.color_ = source.color;
    state.present_ = source.present;
    state.sport_ = source.sport;
    state.bike_ = source.isBike;
    state.run_ = source.isRun;
    state.swim_ = source.isSwim;
    state.crossTraining_ = source.isXtrain;
    state.aero_ = source.isAero;
    state.weight_ = source.weight;
    state.overrides_ = source.overrides_;
    state.samples_ = source.samples;
    return state;
}

void
RideItemComputedState::prepareFor(RideItem &target)
{
    Q_ASSERT(!prepared_);
    preparedIntervals_.reserve(
        static_cast<qsizetype>(intervals_.size()));
    preparedIntervalOwners_.reserve(intervals_.size());
    retiredIntervalOwners_.reserve(
        static_cast<std::size_t>(target.intervals_.size()));
    for (RideItemIntervalState &value : intervals_) {
#ifdef GC_RIDE_ITEM_REFRESH_TEST_HOOKS
        if (materializationFailureCountdown == 0)
            throw std::bad_alloc();
        if (materializationFailureCountdown > 0)
            --materializationFailureCountdown;
#endif
        auto interval = std::make_unique<IntervalItem>();
        interval->rideItem_ = &target;
        interval->name = std::move(value.name);
        interval->type = static_cast<RideFileInterval::IntervalType>(
            value.type);
        interval->start = value.start;
        interval->stop = value.stop;
        interval->startKM = value.startKm;
        interval->stopKM = value.stopKm;
        interval->displaySequence = value.displaySequence;
        interval->color = std::move(value.color);
        interval->route = std::move(value.route);
        interval->test = value.test;
        interval->selected = value.selected;
        interval->metrics_ = std::move(value.metrics);
        interval->count_ = std::move(value.counts);
        interval->stdmean_ = std::move(value.stdmeans);
        interval->stdvariance_ = std::move(value.stdvariances);
        interval->rideInterval = nullptr;
        if (target.ride_
            && interval->type == RideFileInterval::USER
            && value.userIntervalOrdinal >= 0
            && value.userIntervalOrdinal
                < target.ride_->intervals().size()) {
            interval->rideInterval = target.ride_->intervals().at(
                value.userIntervalOrdinal);
        }
        preparedIntervals_.append(interval.get());
        preparedIntervalOwners_.push_back(std::move(interval));
    }
    prepared_ = true;
}

void
RideItemComputedState::applyPreparedTo(RideItem &target) noexcept
{
    Q_ASSERT(prepared_);
    Q_ASSERT(retiredIntervalOwners_.empty());
    for (IntervalItem *interval : target.intervals_)
        retiredIntervalOwners_.emplace_back(interval);

    target.metrics_.swap(metrics_);
    target.count_.swap(counts_);
    target.stdmean_.swap(stdmeans_);
    target.stdvariance_.swap(stdvariances_);
    target.metadata_.swap(metadata_);
    target.xdata_.swap(xdata_);
    target.errors_.swap(errors_);

    target.intervals_.swap(preparedIntervals_);
    for (std::unique_ptr<IntervalItem> &interval : preparedIntervalOwners_)
        interval.release();
    preparedIntervalOwners_.clear();
    intervals_.clear();

    target.zoneRange = zoneRange_;
    target.hrZoneRange = hrZoneRange_;
    target.paceZoneRange = paceZoneRange_;
    target.fingerprint = fingerprint_;
    target.metacrc = metadataCrc_;
    target.crc = crc_;
    target.timestamp = timestamp_;
    target.dbversion = databaseVersion_;
    target.udbversion = userDatabaseVersion_;
    std::swap(target.color, color_);
    target.present.swap(present_);
    target.sport.swap(sport_);
    target.isBike = bike_;
    target.isRun = run_;
    target.isSwim = swim_;
    target.isXtrain = crossTraining_;
    target.isAero = aero_;
    target.weight = weight_;
    target.overrides_.swap(overrides_);
    target.samples = samples_;
    prepared_ = false;
}

void
RideItemComputedState::applyTo(RideItem &target)
{
    prepareFor(target);
    applyPreparedTo(target);
    retiredIntervalOwners_.clear();
}

RideCacheItemSnapshot
RideCacheItemSnapshot::takeFrom(RideItem &source)
{
    RideCacheItemSnapshot snapshot;
    snapshot.computed_ = RideItemComputedState::takeFrom(source);

    snapshot.dirty_ = source.isdirty;
    snapshot.stale_ = source.isstale;
    snapshot.edit_ = source.isedit;
    snapshot.skipSave_ = source.skipsave;
    snapshot.fileName_ = source.fileName;
    snapshot.dateTime_ = source.dateTime;
    return snapshot;
}

bool
RideCacheItemSnapshot::applyTo(RideItem &target)
{
    if (!RideCacheStartup::canApplySnapshot(
            fileName_,
            dateTime_,
            {
                target.fileName, target.dateTime,
                target.isstale, target.isdirty,
                target.isedit,
                target.isOpen(),
                !target.intervals_.isEmpty()
            })) {
        return false;
    }

    if (!target.prepareForRefreshRelevantMutation()) return false;

    computed_.applyTo(target);

    target.isdirty = dirty_;
    target.isstale = stale_;
    target.isedit = edit_;
    target.skipsave = skipSave_;
    return true;
}

void
RideCacheSnapshotBatch::append(RideCacheItemSnapshot &&snapshot)
{
    snapshots_.push_back(std::move(snapshot));
}
