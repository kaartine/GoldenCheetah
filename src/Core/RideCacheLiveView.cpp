/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "RideCache.h"
#include "RideCacheModel.h"

#include <QPointer>

int RideCache::modelRowCount() const
{
    return rides_.count();
}

RideItem *RideCache::modelRideAt(int row) const
{
    return row >= 0 && row < rides_.size()
        ? rides_.at(row)
        : nullptr;
}

int RideCache::modelIndexOf(RideItem *item) const
{
    return rides_.indexOf(item);
}

void RideCache::discardDetachedTombstones()
{
    for (auto tombstone = deletelist.begin();
         tombstone != deletelist.end();) {
        RideItem *address = *tombstone;
        if (!rides_.contains(address)
            && !reverse_.contains(address)
            && !delete_.contains(address)) {
            tombstone = deletelist.erase(tombstone);
        } else {
            ++tombstone;
        }
    }
}

bool RideCache::purgeDestroyedModelRows()
{
    while (true) {
        bool hasDestroyedRow = false;
        for (RideItem *item : rides_) {
            if (!item || deletelist.contains(item)) {
                hasDestroyedRow = true;
                break;
            }
        }
        if (!hasDestroyedRow) {
            discardDetachedTombstones();
            return true;
        }

        const QPointer<RideCache> guardedCache(this);
        const QPointer<RideCacheModel> guardedModel(model_);
        if (!guardedModel || !guardedModel->beginReset()) return false;
        if (!guardedCache || !guardedModel
            || guardedCache->model_ != guardedModel.data()) {
            return false;
        }

        for (qsizetype row = guardedCache->rides_.size();
             row > 0; --row) {
            RideItem *item = guardedCache->rides_.at(row - 1);
            if (!item || guardedCache->deletelist.contains(item))
                guardedCache->rides_.remove(row - 1, 1);
        }
        guardedModel->endReset();
        if (!guardedCache || !guardedModel
            || guardedCache->model_ != guardedModel.data()) {
            return false;
        }
        guardedCache->discardDetachedTombstones();
    }
}
