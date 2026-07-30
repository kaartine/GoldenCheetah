/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCache.h"

#include "LTMSettings.h"
#include "Athlete.h"
#include "Context.h"
#include "DataProcessor.h"
#include "Estimator.h"
#include "RideCacheModel.h"
#include "RideFileCacheIntegrity.h"

QString
RideCache::cpxCachePathForActivity(
    const QString &fileName,
    bool isPlanned) const
{
    if (!context
        || !context->athlete
        || !context->athlete->home) {
        return {};
    }

    AthleteDirectoryStructure *home =
        context->athlete->home;
    const QString sourcePath =
        RideFileCacheIntegrity::activitySourcePath(
            (isPlanned
                 ? home->planned()
                 : home->activities())
                .absolutePath(),
            fileName);
    return RideFileCacheIntegrity::
        cachePathForActivity(
            home->cache().absolutePath(),
            home->activities().absolutePath(),
            home->planned().absolutePath(),
            sourcePath);
}

void
RideCache::removeDerivedFiles(
    const QString &fileName,
    bool isPlanned)
{
    const QString baseName =
        QFileInfo(fileName).baseName();
    if (baseName.isEmpty())
        return;

    for (const QString &extension :
         {QStringLiteral("notes"),
          QStringLiteral("cpi")}) {
        QFile::remove(
            context->athlete->home
                ->cache()
                .filePath(
                    baseName
                    + QLatin1Char('.')
                    + extension));
    }

    const QString cpxPath =
        cpxCachePathForActivity(
            fileName, isPlanned);
    if (!cpxPath.isEmpty())
        QFile::remove(cpxPath);
}

bool
RideCache::removeCurrentRide()
{
    if (!context->ride) return false;
    return removeRide(context->ride);
}

bool
RideCache::removeRide(const QString &filenameToDelete)
{
    RideItem *ride =
        uniqueRideForFileName(filenameToDelete);
    if (!ride) {
        qDebug()
            << "ERROR: delete not found or ambiguous:"
            << filenameToDelete;
        return false;
    }
    return removeRide(ride);
}

bool
RideCache::removeRide(
    const QString &filenameToDelete,
    bool planned)
{
    RideItem *ride =
        uniqueRideForIdentity(
            filenameToDelete, planned);
    if (!ride) {
        qDebug()
            << "ERROR: delete identity not found or ambiguous:"
            << filenameToDelete << planned;
        return false;
    }
    return removeRide(ride);
}

bool
RideCache::removeRide(RideItem *rideToDelete)
{
    return removeRideEntry(
        rideToDelete, RideFileDisposition::Archive);
}

bool
RideCache::removeArchivedRide(const QString &filenameToDelete)
{
    RideItem *ride =
        uniqueRideForFileName(filenameToDelete);
    if (!ride) {
        qDebug()
            << "ERROR: archived delete not found or ambiguous:"
            << filenameToDelete;
        return false;
    }
    return removeArchivedRide(ride);
}

bool
RideCache::removeArchivedRide(RideItem *rideToDelete)
{
    return removeRideEntry(
        rideToDelete,
        RideFileDisposition::AlreadyArchived);
}

bool
RideCache::removeRides(
    const QStringList &filenamesToDelete,
    bool triggerRefresh)
{
    QList<RideItem*> ridesToDelete;
    ridesToDelete.reserve(
        filenamesToDelete.size());
    for (const QString &fileName :
         filenamesToDelete) {
        RideItem *ride =
            uniqueRideForFileName(fileName);
        if (!ride) {
            qDebug()
                << "ERROR: delete not found or ambiguous:"
                << fileName;
            continue;
        }
        if (!ridesToDelete.contains(ride))
            ridesToDelete.append(ride);
    }
    return removeRides(
        ridesToDelete, triggerRefresh);
}

bool
RideCache::removeRides(
    const QList<RideItem*> &ridesToDelete,
    bool triggerRefresh)
{
    const QList<RideItem*> pending =
        ridesToDelete;
    if (pending.isEmpty()) return false;

    cancel();
    bool anyDeleted = false;
    for (RideItem *ride : pending) {
        anyDeleted =
            removeRideEntry(
                ride,
                RideFileDisposition::Archive,
                false)
            || anyDeleted;
    }

    if (anyDeleted && triggerRefresh) {
        refresh();
        estimator->refresh();
    }
    return anyDeleted;
}

RideItem *
RideCache::uniqueRideForFileName(
    const QString &fileName) const
{
    if (fileName.isEmpty()) return nullptr;

    RideItem *match = nullptr;
    for (RideItem *ride : rides_) {
        if (!ride || ride->fileName != fileName)
            continue;
        if (match) return nullptr;
        match = ride;
    }
    return match;
}

RideItem *
RideCache::uniqueRideForIdentity(
    const QString &fileName,
    bool planned) const
{
    if (fileName.isEmpty()) return nullptr;

    RideItem *match = nullptr;
    for (RideItem *ride : rides_) {
        if (!ride
            || ride->fileName != fileName
            || ride->planned != planned) {
            continue;
        }
        if (match) return nullptr;
        match = ride;
    }
    return match;
}

bool
RideCache::removeRideEntry(
    RideItem *rideToDelete,
    RideFileDisposition disposition,
    bool triggerRefresh)
{

    if (!rideToDelete) return false;

    RideItem* select = NULL; // ride to select once its gone
    RideItem* todelete = NULL;
    int index = 0; // index to wipe out

    // find the filenameToDelete in the list and if it happens to be the
    // the current ride then select another one immediately after it, but
    // if it is the last one on the list select the one before
    for (index = 0; index < rides_.count(); index++) {

        RideItem* rideI = rides_[index];

        if (rideI == rideToDelete) {

            // bingo!
            todelete = rideI;

            // if the ride to be deleted happens to be the current ride, then select another
            if (context->ride == todelete) {
                if (rides_.count() - index > 1) select = rides_[index + 1];
                else if (index > 0) select = rides_[index - 1];
            }
            break;
        }
    }

    // WTAF!?
    if (!todelete) {
        qDebug()
            << "ERROR: delete item not found:"
            << rideToDelete;
        return false;
    }
    if (todelete->fileName.isEmpty())
        return false;
    const QString filenameToDelete =
        todelete->fileName;

    // If this activity is linked, unlink it first
    if (todelete->hasLinkedActivity()) {
        RideItem *linkedItem = getLinkedActivity(todelete);
        if (linkedItem) {
            linkedItem->clearLinkedFileName();
            QString error;
            saveActivity(linkedItem, error);
        }
    }

    // dataprocessor runs on "save" which is a short
    // hand for add, update, delete
    DataProcessorFactory::instance().autoProcess(todelete->ride(), "Save", "DELETE");

    // remove from the cache, before deleting it this is so
    // any aggregating functions no longer see it, when recalculating
    // during aride deleted operation
    // but model needs to know about this!
    model_->startRemove(index);
    rides_.remove(index, 1);
    delete_<<todelete;
    model_->endRemove(index);

    // delete the file by renaming it
    if (disposition == RideFileDisposition::Archive) {
        QFile file((todelete->planned ? plannedDirectory : directory).canonicalPath() + "/" + filenameToDelete);

        // purposefully don't remove the old ext so the user wouldn't have to figure out what the old file type was
        QString strNewName = filenameToDelete + ".bak";

        // in case there was an existing bak file, delete it
        // ignore errors since it probably isn't there.
        QFile::remove(context->athlete->home->fileBackup().canonicalPath() + "/" + strNewName);

        if (!file.rename(context->athlete->home->fileBackup().canonicalPath() + "/" + strNewName)) {
            QMessageBox::critical(NULL, "Rename Error", tr("Can't rename %1 to %2 in %3")
                .arg(filenameToDelete).arg(strNewName).arg(context->athlete->home->fileBackup().canonicalPath()));
        }
    }

    removeDerivedFiles(
        filenameToDelete,
        todelete->planned);

    if (select) {

        // we don't want the whole delete, select next flicker
        if (context->mainWindow)
            context->mainWindow->setUpdatesEnabled(false);

        // select a different ride
        context->ride = select;

        // notify after removed from list
        context->notifyRideDeleted(todelete);

        // now we can update
        if (context->mainWindow)
            context->mainWindow->setUpdatesEnabled(true);
        QApplication::processEvents();

        // now select another ride
        context->notifyRideSelected(select);

    } else {
        // re-select the context ride (if it exists) when deleting a non current ride
        context->notifyRideSelected(context->ride);
    }

    if (triggerRefresh) {
        refresh();
        // model estimates (lazy refresh)
        estimator->refresh();
    }

    return true;

}

bool
RideCache::renameRideFiles(
    const QString &oldFileName,
    const QString &newFileName,
    bool isPlanned,
    QString &error)
{
    const QFileInfo oldInfo(oldFileName);
    const QFileInfo newInfo(newFileName);

    const QDir activeDir =
        isPlanned
        ? plannedDirectory
        : directory;
    const QString oldPath =
        activeDir.filePath(oldFileName);
    const QString newPath =
        activeDir.filePath(newFileName);

    if (!QFile::rename(oldPath, newPath)) {
        error = tr(
            "Failed to rename activity file from %1 to %2")
                    .arg(oldFileName, newFileName);
        return false;
    }

    for (const QString &extension :
         {QStringLiteral("notes"),
          QStringLiteral("cpi")}) {
        const QString oldExtPath =
            context->athlete->home
                ->cache()
                .filePath(
                    oldInfo.baseName()
                    + QLatin1Char('.')
                    + extension);
        const QString newExtPath =
            context->athlete->home
                ->cache()
                .filePath(
                    newInfo.baseName()
                    + QLatin1Char('.')
                    + extension);
        if (oldExtPath != newExtPath
            && QFile::exists(oldExtPath)) {
            QFile::rename(
                oldExtPath, newExtPath);
        }
    }

    const QString oldCpxPath =
        cpxCachePathForActivity(
            oldFileName, isPlanned);
    const QString newCpxPath =
        cpxCachePathForActivity(
            newFileName, isPlanned);
    if (!oldCpxPath.isEmpty()
        && !newCpxPath.isEmpty()
        && oldCpxPath != newCpxPath
        && QFile::exists(oldCpxPath)) {
        QFile::rename(
            oldCpxPath, newCpxPath);
    }

    return true;
}
