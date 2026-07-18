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
#include "RideItem.h"

#include <utility>

RideCacheItemSnapshot::RideCacheItemSnapshot() = default;
RideCacheItemSnapshot::~RideCacheItemSnapshot() = default;
RideCacheItemSnapshot::RideCacheItemSnapshot(
    RideCacheItemSnapshot &&) noexcept = default;
RideCacheItemSnapshot &RideCacheItemSnapshot::operator=(
    RideCacheItemSnapshot &&) noexcept = default;

RideCacheItemSnapshot
RideCacheItemSnapshot::takeFrom(RideItem &source)
{
    RideCacheItemSnapshot snapshot;
    snapshot.metrics_ = source.metrics_;
    snapshot.counts_ = source.count_;
    snapshot.stdmeans_ = source.stdmean_;
    snapshot.stdvariances_ = source.stdvariance_;
    snapshot.metadata_ = source.metadata_;
    snapshot.xdata_ = source.xdata_;
    snapshot.errors_ = source.errors_;

    snapshot.intervals_.reserve(
        static_cast<std::size_t>(source.intervals_.size()));
    for (IntervalItem *interval : source.intervals_) {
        snapshot.intervals_.emplace_back(interval);
    }
    source.intervals_.clear();

    snapshot.dirty_ = source.isdirty;
    snapshot.stale_ = source.isstale;
    snapshot.edit_ = source.isedit;
    snapshot.skipSave_ = source.skipsave;
    snapshot.fileName_ = source.fileName;
    snapshot.dateTime_ = source.dateTime;
    snapshot.zoneRange_ = source.zoneRange;
    snapshot.hrZoneRange_ = source.hrZoneRange;
    snapshot.paceZoneRange_ = source.paceZoneRange;
    snapshot.fingerprint_ = source.fingerprint;
    snapshot.metadataCrc_ = source.metacrc;
    snapshot.crc_ = source.crc;
    snapshot.timestamp_ = source.timestamp;
    snapshot.databaseVersion_ = source.dbversion;
    snapshot.userDatabaseVersion_ = source.udbversion;
    snapshot.color_ = source.color;
    snapshot.present_ = source.present;
    snapshot.sport_ = source.sport;
    snapshot.bike_ = source.isBike;
    snapshot.run_ = source.isRun;
    snapshot.swim_ = source.isSwim;
    snapshot.crossTraining_ = source.isXtrain;
    snapshot.aero_ = source.isAero;
    snapshot.weight_ = source.weight;
    snapshot.overrides_ = source.overrides_;
    snapshot.samples_ = source.samples;
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

    target.metrics_ = std::move(metrics_);
    target.count_ = std::move(counts_);
    target.stdmean_ = std::move(stdmeans_);
    target.stdvariance_ = std::move(stdvariances_);
    target.metadata_ = std::move(metadata_);
    target.xdata_ = std::move(xdata_);
    target.errors_ = std::move(errors_);

    for (std::unique_ptr<IntervalItem> &interval : intervals_) {
        interval->rideItem_ = &target;
        target.intervals_.append(interval.release());
    }
    intervals_.clear();

    target.isdirty = dirty_;
    target.isstale = stale_;
    target.isedit = edit_;
    target.skipsave = skipSave_;
    target.zoneRange = zoneRange_;
    target.hrZoneRange = hrZoneRange_;
    target.paceZoneRange = paceZoneRange_;
    target.fingerprint = fingerprint_;
    target.metacrc = metadataCrc_;
    target.crc = crc_;
    target.timestamp = timestamp_;
    target.dbversion = databaseVersion_;
    target.udbversion = userDatabaseVersion_;
    target.color = color_;
    target.present = present_;
    target.sport = sport_;
    target.isBike = bike_;
    target.isRun = run_;
    target.isSwim = swim_;
    target.isXtrain = crossTraining_;
    target.isAero = aero_;
    target.weight = weight_;
    target.overrides_ = std::move(overrides_);
    target.samples = samples_;
    return true;
}

void
RideCacheSnapshotBatch::append(RideCacheItemSnapshot &&snapshot)
{
    snapshots_.push_back(std::move(snapshot));
}
