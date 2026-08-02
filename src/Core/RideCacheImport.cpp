/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCache.h"

#include "Athlete.h"
#include "Context.h"
#include "Estimator.h"
#include "RideCacheBulkMerge.h"
#include "RideCacheCallbackGuard.h"
#include "RideCacheModel.h"
#include "RideCacheMutationScope.h"
#include "RideFile.h"
#include "RideItem.h"

#include <QPointer>

#include <algorithm>

namespace {

bool rideImportLessThan(const RideItem *left, const RideItem *right)
{
    return left->dateTime < right->dateTime;
}

} // namespace

void RideCache::addRide(
    QString name,
    bool dosignal,
    bool select,
    bool useTempActivities,
    bool planned)
{
    QDateTime dt;
    if (!RideFile::parseRideFileName(name, &dt)) return;

    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        if (!mutationError.isEmpty())
            qWarning().noquote() << mutationError;
        return;
    }
    QPointer<RideItem> prior(context->ride);

    RideItem *last;
    if (useTempActivities) {
        last = new RideItem(
            context->athlete->home->tmpActivities().canonicalPath(),
            name, dt, context, false);
    } else if (planned) {
        last = new RideItem(
            plannedDirectory.canonicalPath(),
            name, dt, context, planned);
    } else {
        last = new RideItem(
            directory.canonicalPath(),
            name, dt, context, planned);
    }

    connect(last, SIGNAL(rideDataChanged()), this, SLOT(itemChanged()));
    connect(last, SIGNAL(rideMetadataChanged()), this, SLOT(itemChanged()));
    QPointer<RideItem> guardedLast(last);
    const RideCacheCallbackGuard callbackGuard(
        this, context, last, estimator);

    bool added = false;
    for (int index = 0; index < rides_.count(); ++index) {
        RideItem *const current = rides_.at(index);
        if (current && !deletelist.contains(current)
            && current->fileName == last->fileName) {
            invalidateStartupSnapshots();
            rides_[index] = last;
            added = true;
            break;
        }
    }

    if (!added) {
        bool appended = false;
        QString publishError;
        const bool published = mutation.resetAndSort(
            publishError,
            [this, &guardedLast, &appended] {
                if (!guardedLast) return false;
                rides_ << guardedLast.data();
                appended = true;
                return true;
            });
        if (!published) {
            if (guardedLast && !appended) {
                if (!mutation.ownersStable())
                    guardedLast->context = nullptr;
                delete guardedLast.data();
            }
            if (!publishError.isEmpty())
                qWarning().noquote() << publishError;
            return;
        }
        if (!mutation.ownersStable() || !guardedLast) return;
    }

    last = guardedLast.data();
    if (!last) return;
    last->refresh();
    if (!mutation.ownersStable() || !guardedLast)
        return;

    if (dosignal) {
        context->notifyRideAdded(last);
        if (!callbackGuard.allAlive()
            || !mutation.ownersStable()) return;
    }

    if (prior) prior->close();

    if (select) {
        context->ride = last;
        context->notifyRideSelected(last);
    } else {
        context->notifyRideSelected(prior.data());
    }

    if (!callbackGuard.allAlive()
        || !mutation.ownersStable()) return;
}

QVector<RideItem*> RideCache::addRides(
    const QStringList &names,
    const QVector<RideFile*> &preparedRides,
    bool dosignal,
    bool select,
    bool useTempActivities,
    bool planned)
{
    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        if (!mutationError.isEmpty())
            qWarning().noquote() << mutationError;
        qDeleteAll(preparedRides);
        return {};
    }
    QPointer<RideItem> prior(context->ride);
    QVector<RideItem*> incoming;
    QVector<QPointer<RideItem>> guardedIncoming;
    incoming.reserve(names.size());
    guardedIncoming.reserve(names.size());

    for (qsizetype index = 0; index < names.size(); ++index) {
        const QString &name = names[index];
        RideFile *prepared =
            index < preparedRides.size() ? preparedRides[index] : nullptr;

        QDateTime dt;
        if (!RideFile::parseRideFileName(name, &dt)) {
            delete prepared;
            continue;
        }

        QString path;
        bool itemPlanned = planned;
        if (useTempActivities) {
            path = context->athlete->home->tmpActivities().canonicalPath();
            itemPlanned = false;
        } else if (planned) {
            path = plannedDirectory.canonicalPath();
        } else {
            path = directory.canonicalPath();
        }

        RideItem *item = nullptr;
        if (prepared) {
            item = new RideItem(prepared, dt, context);
            item->path = path;
            item->fileName = name;
            item->planned = itemPlanned;
            item->isdirty = false;
        } else {
            item = new RideItem(
                path, name, dt, context, itemPlanned);
        }

        connect(item, SIGNAL(rideDataChanged()), this, SLOT(itemChanged()));
        connect(item, SIGNAL(rideMetadataChanged()), this, SLOT(itemChanged()));
        incoming.append(item);
        guardedIncoming.append(QPointer<RideItem>(item));
    }

    for (qsizetype index = names.size();
         index < preparedRides.size(); ++index) {
        delete preparedRides[index];
    }
    if (incoming.isEmpty()) return incoming;

    bool mergeStarted = false;
    bool mergePrepared = false;
    QString mergeResetError;
    const QPointer<RideCacheModel> guardedModel(model_);
    const QVector<RideItem*> replaced =
        RideCacheBulkMerge::mergeItems(
            rides_,
            incoming,
            [](const RideItem *item) { return item->fileName; },
            rideImportLessThan,
            [&mutation, &mergeStarted, &mergeResetError]() {
                mergeStarted = mutation.beginReset(
                    mergeResetError);
                return mergeStarted;
            },
            [&mutation]() {
                mutation.endReset();
            },
            [this, guardedModel, &mutation, &mergePrepared]() {
                if (!mutation.ownersStable() || !guardedModel
                    || model_ != guardedModel.data()) {
                    return false;
                }
                purgeDestroyedRowsInsideModelReset();
                mergePrepared = true;
                return true;
            });
    if (!mergeStarted || !mergePrepared) {
        if (!mergeResetError.isEmpty())
            qWarning().noquote() << mergeResetError;
        if (!mutation.ownersStable()) {
            for (const QPointer<RideItem> &item : guardedIncoming) {
                if (item) item->context = nullptr;
            }
        }
        qDeleteAll(incoming);
        return {};
    }
    Q_UNUSED(replaced);
    if (!mutation.ownersStable()) return {};

    for (const QPointer<RideItem> &guardedItem : guardedIncoming) {
        RideItem *item = guardedItem.data();
        if (!item) continue;
        item->refresh();
        if (!mutation.ownersStable() || !guardedItem) return {};
        item->close();
        if (dosignal) {
            context->notifyRideAdded(item);
            if (!mutation.ownersStable()) return {};
        }
    }

    if (prior) prior->close();

    if (select) {
        RideItem *selection = nullptr;
        for (auto item = guardedIncoming.crbegin();
             item != guardedIncoming.crend(); ++item) {
            if (*item) {
                selection = item->data();
                break;
            }
        }
        context->ride = selection;
        context->notifyRideSelected(context->ride);
    } else {
        context->notifyRideSelected(prior.data());
    }
    if (!mutation.ownersStable()) return {};

    QVector<RideItem *> liveIncoming;
    liveIncoming.reserve(guardedIncoming.size());
    for (const QPointer<RideItem> &item : guardedIncoming) {
        if (item) liveIncoming.append(item.data());
    }
    return liveIncoming;
}
