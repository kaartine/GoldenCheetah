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
#include "RideItem.h"

#include <QPointer>

#include <utility>

bool
RideCache::saveActivity
(Context *context, RideItem *item, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved,
 QObject *operationOwner)
{
    error.clear();
    const bool contextRequired = context != nullptr;
    const QPointer<Context> guardedContext(context);
    const bool ownerRequired = operationOwner != nullptr;
    const QPointer<QObject> guardedOwner(operationOwner);
    QPointer<RideItem> guardedItem(item);
    const auto operationIsAvailable = [&] {
        return (!contextRequired || guardedContext)
            && (!ownerRequired || guardedOwner);
    };
    const auto rejectUnavailableOperation = [&] {
        if (operationIsAvailable()) return false;
        if (error.isEmpty()) {
            error = QObject::tr(
                "The activity collection disappeared while saving");
        }
        return true;
    };

    if (rejectUnavailableOperation()) return false;
    if (!guardedItem) {
        error = QObject::tr("No activity given");
        return false;
    }
    if (!guardedItem->isDirty()) {
        return true;
    }
    if (!save) {
        error = QObject::tr("No activity save operation available");
        return false;
    }
    const bool saved = save(
        guardedContext.data(), guardedItem.data(), &error);
    if (rejectUnavailableOperation()) return false;
    if (!guardedItem) {
        error = QObject::tr(
            "The activity disappeared while it was being saved");
        return false;
    }
    if (!saved) {
        if (error.isEmpty()) {
            error = QObject::tr("The activity could not be saved");
        }
        return false;
    }
    if (notifySaved) {
        const QString savedFileName = guardedItem->fileName;
        const QString savedPath = guardedItem->path;
        const bool savedPlanned = guardedItem->planned;
        notifySaved(guardedItem.data());
        if (rejectUnavailableOperation()) return false;
        if (!guardedItem) {
            error = QObject::tr(
                "The activity disappeared while announcing its save");
            return false;
        }
        if (guardedItem->fileName != savedFileName
            || guardedItem->path != savedPath
            || guardedItem->planned != savedPlanned) {
            error = QObject::tr(
                "The activity identity changed while announcing its save");
            return false;
        }
    }
    return true;
}

bool
RideCache::saveActivities
(Context *context, const QList<RideItem *> &items, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved,
 QObject *operationOwner)
{
    error.clear();
    QStringList failed;
    const bool contextRequired = context != nullptr;
    const QPointer<Context> guardedContext(context);
    const bool ownerRequired = operationOwner != nullptr;
    const QPointer<QObject> guardedOwner(operationOwner);
    const auto operationIsAvailable = [&] {
        return (!contextRequired || guardedContext)
            && (!ownerRequired || guardedOwner);
    };
    const auto unavailableFailure = [] {
        return QObject::tr(
            "<activity collection> (collection disappeared while saving)");
    };

    if (!operationIsAvailable()) {
        error = QObject::tr(
            "The activity collection is no longer available");
        return false;
    }

    struct SaveRequest
    {
        QPointer<RideItem> item;
        QString fileName;
        QString path;
        bool planned = false;
    };
    QList<SaveRequest> requests;
    requests.reserve(items.size());
    QSet<RideItem*> requestedItems;
    for (RideItem *item : items) {
        if (!item) {
            requests.append(SaveRequest{});
            continue;
        }
        if (requestedItems.contains(item)) continue;
        requestedItems.insert(item);
        requests.append({
            QPointer<RideItem>(item),
            item->fileName,
            item->path,
            item->planned});
    }

    for (const SaveRequest &request :
         std::as_const(requests)) {
        if (!operationIsAvailable()) {
            failed << unavailableFailure();
            break;
        }
        RideItem *const item = request.item.data();
        if (!item
            || item->fileName != request.fileName
            || item->path != request.path
            || item->planned != request.planned) {
            failed << (request.fileName.isEmpty()
                ? QObject::tr("<unknown activity>")
                : QStringLiteral("%1 (%2)")
                    .arg(
                        request.fileName,
                        QObject::tr(
                            "activity changed before it could be saved")));
            continue;
        }
        QString itemError;
        if (!RideCache::saveActivity(
                guardedContext.data(), item, itemError,
                save, notifySaved, guardedOwner.data())) {
            const QString fileName = request.fileName.isEmpty()
                ? QObject::tr("<unknown activity>")
                : request.fileName;
            if (itemError.isEmpty()) {
                failed << fileName;
            } else {
                failed << QStringLiteral("%1 (%2)").arg(fileName, itemError);
            }
            if (!operationIsAvailable()) break;
        }
    }
    if (!failed.isEmpty()) {
        error = QObject::tr("Failed to save: %1")
                    .arg(failed.join(QStringLiteral(", ")));
        return false;
    }

    return true;
}
