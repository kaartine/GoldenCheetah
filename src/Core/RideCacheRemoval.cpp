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

bool
RideCache::removeCurrentRide()
{
    if (!context->ride) return false;
    return removeRide(context->ride->fileName);
}

bool
RideCache::removeRide(const QString &filenameToDelete)
{
    return removeRideEntry(
        filenameToDelete, RideFileDisposition::Archive);
}

bool
RideCache::removeArchivedRide(const QString &filenameToDelete)
{
    return removeRideEntry(
        filenameToDelete, RideFileDisposition::AlreadyArchived);
}

bool
RideCache::removeRideEntry(
    const QString &filenameToDelete,
    RideFileDisposition disposition)
{

    // if there is no file activity to delete then return
    if (filenameToDelete.isEmpty()) return false;

    RideItem* select = NULL; // ride to select once its gone
    RideItem* todelete = NULL;
    int index = 0; // index to wipe out

    // find the filenameToDelete in the list and if it happens to be the
    // the current ride then select another one immediately after it, but
    // if it is the last one on the list select the one before
    for (index = 0; index < rides_.count(); index++) {

        RideItem* rideI = rides_[index];

        if (rideI->fileName == filenameToDelete) {

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
        qDebug()<<"ERROR: delete not found.";
        return false;
    }

    // If this activity is linked, unlink it first
    if (todelete->hasLinkedActivity()) {
        QString linkedFileName = todelete->getLinkedFileName();
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

    // remove any other derived/additional files; notes, cpi etc (they can only exist in /cache )
    QStringList extras;
    extras << "notes" << "cpi" << "cpx";
    foreach (QString extension, extras) {

        QString deleteMe = QFileInfo(filenameToDelete).baseName() + "." + extension;
        QFile::remove(context->athlete->home->cache().canonicalPath() + "/" + deleteMe);
    }

    if (select) {

        // we don't want the whole delete, select next flicker
        context->mainWindow->setUpdatesEnabled(false);

        // select a different ride
        context->ride = select;

        // notify after removed from list
        context->notifyRideDeleted(todelete);

        // now we can update
        context->mainWindow->setUpdatesEnabled(true);
        QApplication::processEvents();

        // now select another ride
        context->notifyRideSelected(select);

    } else {
        // re-select the context ride (if it exists) when deleting a non current ride
        context->notifyRideSelected(context->ride);
    }

    refresh();
    // model estimates (lazy refresh)
    estimator->refresh();

    return true;

}
