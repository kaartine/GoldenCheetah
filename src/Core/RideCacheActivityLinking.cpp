/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include "RideCache.h"

#include "RideItem.h"

RideCache::OperationPreCheck
RideCache::checkLinkActivities(RideItem *item1, RideItem *item2)
{
    OperationPreCheck check;

    if (!isValidLink(item1, item2, check.blockingReason)) {
        check.canProceed = false;
        return check;
    }
    if (item1->hasLinkedActivity()) {
        check.canProceed = false;
        check.blockingReason = tr("%1 is already linked to %2")
            .arg(item1->fileName)
            .arg(item1->getLinkedFileName());
        return check;
    }
    if (item2->hasLinkedActivity()) {
        check.canProceed = false;
        check.blockingReason = tr("%1 is already linked to %2")
            .arg(item2->fileName)
            .arg(item2->getLinkedFileName());
        return check;
    }

    check.affectedItems << item1 << item2;
    if (item1->isDirty()) check.dirtyItems << item1;
    if (item2->isDirty()) check.dirtyItems << item2;
    if (!check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *item : check.dirtyItems)
            dirtyNames << item->fileName;
        check.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Linking will modify both activities. You must save or discard "
            "changes first.")
            .arg(dirtyNames.join("\n"));
    }

    return check;
}

RideCache::OperationResult
RideCache::linkActivities(RideItem *item1, RideItem *item2)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!isValidLink(item1, item2, result.error)) return result;

    item1->setLinkedFileName(item2->fileName);
    item2->setLinkedFileName(item1->fileName);

    result.success = true;
    result.affectedCount = 2;

    emit itemChanged(item1);
    emit itemChanged(item2);

    return result;
}

bool
RideCache::isValidLink(
    RideItem *item1,
    RideItem *item2,
    QString &error)
{
    error.clear();
    if (!item1 || !item2) {
        error = tr("Invalid activities for linking");
        return false;
    }
    if (!ownsLiveRide(item1) || !ownsLiveRide(item2)) {
        error = tr(
            "An activity is no longer in the activity list");
        return false;
    }
    if (item1 == item2) {
        error = tr("Can't link to self");
        return false;
    }
    if (item1->planned == item2->planned) {
        error = tr(
            "Cannot link two activities of the same type. One must be "
            "planned, one actual.");
        return false;
    }
    return true;
}
