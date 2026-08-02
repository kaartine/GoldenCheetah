/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCache.h"

#include "Context.h"
#include "RideCacheMutationScope.h"
#include "RideItem.h"
#include "RideMetadata.h"

#include <QFile>
#include <QFileInfo>
#include <QPointer>

#include <algorithm>
#include <utility>

RideCache::OperationResult
RideCache::moveActivity
(RideItem *item, const QDateTime &newDateTime)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(item)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }
    QPointer<RideItem> guardedItem(item);
    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
        return result;
    }
    if (!guardedItem || !ownsLiveRide(guardedItem.data())) {
        result.error = tr(
            "The activity changed while background work was being stopped");
        return result;
    }
    item = guardedItem.data();

    QString oldFileName = item->fileName;
    QDateTime oldDateTime = item->dateTime;

    QFileInfo oldInfo(oldFileName);
    QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();

    RideFile *ride = item->ride(true);
    if (! ride) {
        result.error = tr("Failed to open activity file");
        return result;
    }

    QDate originalDate = QDate::fromString(ride->getTag("Original Date", ""), "yyyy/MM/dd");
    if (! originalDate.isValid()) {
        ride->setTag("Original Date", oldDateTime.date().toString("yyyy/MM/dd"));
    }
    item->setStartTime(newDateTime);
    ride->setTag("Year", newDateTime.toString("yyyy"));
    ride->setTag("Month", newDateTime.toString("MMMM"));
    ride->setTag("Weekday", newDateTime.toString("ddd"));
    ride->setTag("Filename", newFileName);
    RideMetadata *rideMetadata = GlobalContext::context()->rideMetadata;
    if (rideMetadata) {
        item->metadata_.insert(
            "Calendar Text", rideMetadata->calendarText(item));
    }

    QString renameError;
    if (! renameRideFiles(oldFileName, newFileName, item->planned, renameError)) {
        item->dateTime = oldDateTime;
        item->fileName = oldFileName;
        result.error = tr("Failed to rename files: %1").arg(renameError);
        item->close();
        return result;
    }

    QString newPath = (item->planned ? plannedDirectory : directory).canonicalPath() + "/" + newFileName;
    QFile outFile(newPath);
    if (! RideFileFactory::instance().writeRideFile(context, ride, outFile, QFileInfo(newFileName).suffix())) {
        renameRideFiles(newFileName, oldFileName, item->planned, renameError);
        item->dateTime = oldDateTime;
        item->fileName = oldFileName;
        result.error = tr("Failed to save activity file after rename");
        item->close();
        return result;
    }
    item->close();

    item->setFileName((item->planned ? plannedDirectory : directory).canonicalPath(), newFileName);

    QString publishError;
    if (!mutation.resetAndSort(
            publishError, [] { return true; })) {
        result.error = publishError;
        return result;
    }
    if (!guardedItem || !ownsLiveRide(guardedItem.data())) {
        result.error = tr(
            "The renamed activity was removed while it was being published");
        return result;
    }
    item = guardedItem.data();

    item->isstale = true;

    RideItem *linkedItem = getLinkedActivity(item);
    if (linkedItem) {
        const QPointer<RideItem> guardedLinkedItem(linkedItem);
        linkedItem->setLinkedFileName(newFileName);
        if (!mutation.ownersStable() || !guardedItem
            || !guardedLinkedItem) {
            result.error = tr(
                "A linked activity changed while the rename was being published");
            return result;
        }
        emit itemChanged(linkedItem);
        result.affectedCount = 2;
    } else {
        result.affectedCount = 1;
    }

    if (item->planned) {
        updateFromWorkout(item, false);
        if (!mutation.ownersStable() || !guardedItem) {
            result.error = tr(
                "The planned activity changed while its workout was being updated");
            return result;
        }
    }

    item->refresh();
    if (!mutation.ownersStable() || !guardedItem) {
        result.error = tr(
            "The activity changed while its metrics were being refreshed");
        return result;
    }
    context->notifyRideChanged(item);
    if (!mutation.ownersStable() || !guardedItem) {
        result.error = tr(
            "The activity changed while the rename was being announced");
        return result;
    }
    if (context->ride == item) {
        context->notifyRideSelected(item);
    }

    result.success = true;

    return result;
}

RideCache::OperationResult
RideCache::copyPlannedActivity
(RideItem *sourceItem, const QDate &newDate, QTime newTime)
{
    OperationResult result;

    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(sourceItem)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }
    QPointer<RideItem> guardedSource(sourceItem);
    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
        return result;
    }
    if (!guardedSource
        || !ownsLiveRide(guardedSource.data())) {
        result.error = tr(
            "The source activity changed while background work was being stopped");
        return result;
    }
    sourceItem = guardedSource.data();

    QString error;
    QTime time(sourceItem->dateTime.time());
    if (newTime.isValid()) {
        time = newTime;
    }
    RideItem *newItem = copyPlannedRideFile(sourceItem, newDate, time, error);

    if (! newItem) {
        result.error = error;
        return result;
    }
    QPointer<RideItem> guardedNewItem(newItem);
    bool published = false;
    const auto discardUnpublished = [&] {
        if (!guardedNewItem || published) return;
        if (!mutation.ownersStable()) {
            guardedNewItem->context = nullptr;
        }
        delete guardedNewItem.data();
    };

    if (!mutation.ownersStable() || !guardedSource
        || !ownsLiveRide(guardedSource.data())) {
        discardUnpublished();
        result.error = tr(
            "The source activity changed while its copy was being prepared");
        return result;
    }

    if (!mutation.resetAndSort(
            error,
            [this, &guardedNewItem, &published] {
                if (!guardedNewItem) return false;
                rides_ << guardedNewItem.data();
                published = true;
                return true;
            })) {
        discardUnpublished();
        result.error = error;
        return result;
    }
    if (!mutation.ownersStable() || !guardedNewItem
        || !ownsLiveRide(guardedNewItem.data())) {
        result.error = tr(
            "The copied activity changed while it was being published");
        return result;
    }

    result.success = true;
    result.affectedCount = 1;

    return result;
}

RideCache::OperationResult
RideCache::copyPlannedActivities
(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets)
{
    OperationResult result;

    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (sourceItemsAndTargets.isEmpty()) {
        result.error = tr("No files specified");
        return result;
    }
    QList<std::pair<QPointer<RideItem>, QDate>> guardedSources;
    guardedSources.reserve(sourceItemsAndTargets.size());
    for (const std::pair<RideItem*, QDate> &pair : sourceItemsAndTargets) {
        if (!ownsLiveRide(pair.first)) {
            result.error = tr(
                "A source activity is no longer in the activity list");
            return result;
        }
        guardedSources.append({QPointer<RideItem>(pair.first), pair.second});
    }

    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
        return result;
    }
    for (const auto &pair : std::as_const(guardedSources)) {
        if (!pair.first || !ownsLiveRide(pair.first.data())) {
            result.error = tr(
                "A source activity changed while background work was being stopped");
            return result;
        }
    }

    QVector<QPointer<RideItem>> newItems;
    bool published = false;
    const auto discardUnpublished = [&] {
        if (published) return;
        const bool ownersStable = mutation.ownersStable();
        for (const QPointer<RideItem> &item :
             std::as_const(newItems)) {
            if (!item) continue;
            if (!ownersStable) item->context = nullptr;
            delete item.data();
        }
        newItems.clear();
    };

    QStringList failedFiles;
    for (const auto &pair : std::as_const(guardedSources)) {
        if (!pair.first || !ownsLiveRide(pair.first.data())) {
            discardUnpublished();
            result.error = tr(
                "A source activity changed while copies were being prepared");
            return result;
        }
        const QString sourceFileName = pair.first->fileName;
        QString error;
        RideItem *newItem = copyPlannedRideFile(
            pair.first.data(), pair.second,
            pair.first->dateTime.time(), error);
        if (newItem) {
            newItems.append(QPointer<RideItem>(newItem));
        } else {
            failedFiles << sourceFileName;
        }
        if (!mutation.ownersStable()) {
            discardUnpublished();
            result.error = tr(
                "The activity collection changed while copies were being prepared");
            return result;
        }
    }

    if (! newItems.isEmpty()) {
        QString publishError;
        if (!mutation.resetAndSort(
                publishError,
                [this, &newItems, &published] {
                    for (const QPointer<RideItem> &item :
                         std::as_const(newItems)) {
                        if (!item) return false;
                    }
                    for (const QPointer<RideItem> &item :
                         std::as_const(newItems)) {
                        rides_ << item.data();
                    }
                    published = true;
                    return true;
                })) {
            discardUnpublished();
            result.error = publishError;
            return result;
        }
    }
    if (! failedFiles.isEmpty()) {
        result.error = tr("Failed to copy %1 of %2 activities: %3")
                         .arg(failedFiles.count())
                         .arg(sourceItemsAndTargets.count())
                         .arg(failedFiles.join(", "));
    }

    int publishedCount = 0;
    for (const QPointer<RideItem> &item : std::as_const(newItems)) {
        if (item && ownsLiveRide(item.data())) ++publishedCount;
    }
    result.success = publishedCount > 0;
    result.affectedCount = publishedCount;

    return result;
}

RideCache::OperationResult
RideCache::shiftPlannedActivities
(const QDate &fromDate, int dayOffset)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }

    if (dayOffset == 0) {
        result.success = true;
        result.affectedCount = 0;
        return result;
    }

    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
        return result;
    }

    QList<QPointer<RideItem>> itemsToShift;
    for (RideItem *item : rides()) {
        if (item->planned && item->dateTime.date() >= fromDate) {
            itemsToShift.append(QPointer<RideItem>(item));
        }
    }
    if (itemsToShift.isEmpty()) {
        result.success = true;
        result.affectedCount = 0;
        return result;
    }

    // Prevent shifting any activity to before fromDate.
    int effectiveOffset = dayOffset;
    if (dayOffset < 0) {
        QDate earliestDate = itemsToShift.constFirst()->dateTime.date();
        for (const QPointer<RideItem> &guardedItem :
             std::as_const(itemsToShift)) {
            RideItem *item = guardedItem.data();
            if (!item) continue;
            if (item->dateTime.date() < earliestDate) {
                earliestDate = item->dateTime.date();
            }
        }
        int maxBackwardShift = fromDate.daysTo(earliestDate);
        if (-dayOffset > maxBackwardShift) {
            effectiveOffset = -maxBackwardShift;
        }
        if (effectiveOffset == 0) {
            result.success = true;
            result.affectedCount = 0;
            return result;
        }
    }

    // Avoid filename collisions by moving the farthest item first.
    if (effectiveOffset > 0) {
        std::sort(
            itemsToShift.begin(), itemsToShift.end(),
            [](const QPointer<RideItem> &left,
               const QPointer<RideItem> &right) {
                return left && right
                    ? left->dateTime > right->dateTime
                    : bool(left);
            });
    } else {
        std::sort(
            itemsToShift.begin(), itemsToShift.end(),
            [](const QPointer<RideItem> &left,
               const QPointer<RideItem> &right) {
                return left && right
                    ? left->dateTime < right->dateTime
                    : bool(left);
            });
    }

    QStringList failedFiles;
    int successCount = 0;
    for (const QPointer<RideItem> &guardedItem :
         std::as_const(itemsToShift)) {
        RideItem *item = guardedItem.data();
        if (!item || !ownsLiveRide(item)) continue;
        QString oldFileName = item->fileName;
        QDate newDate = item->dateTime.date().addDays(effectiveOffset);
        QDateTime newDateTime(newDate, item->dateTime.time());

        QFileInfo oldInfo(oldFileName);
        QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();

        RideFile *ride = item->ride(true);
        if (! ride) {
            failedFiles << oldFileName;
            continue;
        }

        QDate originalDate = QDate::fromString(ride->getTag("Original Date", ""), "yyyy/MM/dd");
        if (! originalDate.isValid()) {
            ride->setTag("Original Date", item->dateTime.date().toString("yyyy/MM/dd"));
        }
        item->setStartTime(newDateTime);
        ride->setTag("Year", newDateTime.toString("yyyy"));
        ride->setTag("Month", newDateTime.toString("MMMM"));
        ride->setTag("Weekday", newDateTime.toString("ddd"));
        ride->setTag("Filename", newFileName);
        RideMetadata *rideMetadata = GlobalContext::context()->rideMetadata;
        if (rideMetadata) {
            item->metadata_.insert(
                "Calendar Text", rideMetadata->calendarText(item));
        }

        QString renameError;
        if (! renameRideFiles(oldFileName, newFileName, true, renameError)) {
            failedFiles << oldFileName;
            item->close();
            continue;
        }

        QString newPath = plannedDirectory.canonicalPath() + "/" + newFileName;
        QFile outFile(newPath);
        if (! RideFileFactory::instance().writeRideFile(context, ride, outFile, QFileInfo(newFileName).suffix())) {
            renameRideFiles(newFileName, oldFileName, true, renameError);
            failedFiles << oldFileName;
            item->close();
            continue;
        }
        item->close();
        item->setFileName(plannedDirectory.canonicalPath(), newFileName);
        updateFromWorkout(item, true);
        if (!mutation.ownersStable() || !guardedItem) {
            result.error = tr(
                "A planned activity changed while its workout was being updated");
            return result;
        }
        item = guardedItem.data();
        item->isstale = true;

        RideItem *linkedItem = getLinkedActivity(item);
        if (linkedItem) {
            const QPointer<RideItem> guardedLinkedItem(linkedItem);
            linkedItem->setLinkedFileName(item->fileName);
            if (!mutation.ownersStable() || !guardedItem
                || !guardedLinkedItem) {
                result.error = tr(
                    "A linked activity changed while planned activities were being shifted");
                return result;
            }
            emit itemChanged(linkedItem);
        }

        successCount++;
    }

    if (successCount > 0) {
        QString publishError;
        if (!mutation.resetAndSort(
                publishError, [] { return true; })) {
            result.error = publishError;
            return result;
        }
    }

    if (! failedFiles.isEmpty()) {
        result.error = tr("Failed to shift %1 of %2 activities: %3")
                         .arg(failedFiles.count())
                         .arg(itemsToShift.count())
                         .arg(failedFiles.join(", "));
    }

    result.success = true;
    result.affectedCount = successCount;

    return result;
}
