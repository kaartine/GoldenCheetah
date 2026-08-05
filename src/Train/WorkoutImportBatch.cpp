/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WorkoutImportBatch.h"
#include "LibraryImportFileStager.h"
#include "TrainDB.h"

#ifdef GC_LIBRARY_TRANSACTION_TEST_HOOKS
#include "LibraryTransactionTestStubs.h"
#else
#include "Athlete.h"
#include "Context.h"
#include "ErgFile.h"
#include "Library.h"
#include "LibraryParser.h"
#include "Settings.h"
#include "VideoSyncFile.h"
#endif

#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>

namespace {

class MetadataSnapshot
{
public:
    bool capture(const QString &target, QString &error)
    {
        path = target;
        const QFileInfo info(path);
        existed = info.exists();
        permissions = info.permissions();
        if (!existed) return true;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
            return false;
        }
        contents = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            error = file.errorString();
            return false;
        }
        return true;
    }

    bool restore(QString &error) const
    {
        if (!existed) {
            if (!QFileInfo::exists(path)) return true;
            QFile file(path);
            if (file.remove()) return true;
            error = file.errorString();
            return false;
        }

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            error = file.errorString();
            return false;
        }
        if (file.write(contents) != contents.size()) {
            error = file.errorString();
            file.cancelWriting();
            return false;
        }
        if (!file.setPermissions(permissions)) {
            error = file.errorString();
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            error = file.errorString();
            return false;
        }
        return true;
    }

private:
    QString path;
    QByteArray contents;
    QFileDevice::Permissions permissions;
    bool existed = false;
};

QString metadataPath(Context *context)
{
    QDir root = context->athlete->home->root();
    root.cdUp();
    return root.filePath(QStringLiteral("library.xml"));
}

void appendUnique(QStringList &values, const QString &value)
{
    if (!values.contains(value)) values.append(value);
}

} // namespace

WorkoutImportBatchResult runWorkoutImportDialogBatch(
    Context *context,
    const QStringList &videos,
    const QStringList &workouts,
    const QStringList &videoSyncs,
    bool overwrite,
    const WorkoutImportBatchSuccess &onSuccess)
{
    WorkoutImportBatchResult result;
    const QStringList requestedFiles = videos + workouts + videoSyncs;

    auto fail = [&](const QString &title, const QString &message) {
        result.errorTitle = title;
        result.errorMessage = message;
        for (const QString &file : requestedFiles) {
            appendUnique(result.failedFiles, file);
        }
    };

    if (requestedFiles.isEmpty()) {
        fail(QObject::tr("Import Failed"),
             QObject::tr("There are no files to import."));
        return result;
    }
    if (context == nullptr
        || context->athlete == nullptr
        || context->athlete->home == nullptr
        || appsettings == nullptr
        || trainDB == nullptr
        || (trainDB->schemaStatus() != TrainDB::SchemaStatus::current
            && trainDB->schemaStatus() != TrainDB::SchemaStatus::migrationReady)) {
        fail(QObject::tr("Import Failed"),
             QObject::tr("The workout database is not ready for imports."));
        return result;
    }

    Library *library = Library::findLibrary(QStringLiteral("Media Library"));
    if (!videos.isEmpty() && library == nullptr) {
        fail(QObject::tr("Import Failed"),
             QObject::tr("The media library is not initialized."));
        return result;
    }

    TrainDB::ScopedLUW transaction(*trainDB);
    if (!transaction.isActive()) {
        fail(QObject::tr("Import Failed"),
             QObject::tr("Could not start the workout database transaction."));
        return result;
    }

    QString libraryPath = appsettings->value(nullptr, GC_WORKOUTDIR).toString();
    if (libraryPath.isEmpty()) {
        QDir root = context->athlete->home->root();
        root.cdUp();
        libraryPath = root.absolutePath();
    }

    LibraryImportFileStager fileStager;
    MetadataSnapshot metadataSnapshot;
    bool metadataTouched = false;
    QStringList pendingRefs;
    QString selectedVideo;
    QString selectedWorkout;
    QString selectedVideoSync;

    auto rollback = [&]() {
        transaction.rollback();
        if (metadataTouched) {
            QString restoreError;
            if (!metadataSnapshot.restore(restoreError)) {
                result.rollbackFailures.append(metadataPath(context));
                result.errorMessage += QObject::tr(
                    " The previous library metadata could not be restored: %1")
                                           .arg(restoreError);
            }
        }
        const QStringList fileFailures = fileStager.rollback();
        for (const QString &path : fileFailures) {
            appendUnique(result.rollbackFailures, path);
        }
        if (!fileFailures.isEmpty()) {
            result.errorMessage += QObject::tr(
                " Some copied files could not be restored or removed: %1")
                                       .arg(fileFailures.join(
                                           QStringLiteral(", ")));
        }
        return result;
    };

    for (const QString &video : videos) {
        if (!QFileInfo(video).isFile()) {
            fail(QObject::tr("Import Video Failed"),
                 QObject::tr("%1 is not a readable file.")
                     .arg(QFileInfo(video).fileName()));
            return rollback();
        }
        if (!library->refs.contains(video)) {
            if (!trainDB->importVideo(video, ImportMode::insert)) {
                fail(QObject::tr("Import Video Failed"),
                     QObject::tr("%1 could not be written to the workout database.")
                         .arg(QFileInfo(video).fileName()));
                return rollback();
            }
            pendingRefs.append(video);
        }
        if (selectedVideo.isEmpty()) selectedVideo = video;
    }

    const LibraryImportStageMode stageMode = overwrite
        ? LibraryImportStageMode::replaceExisting
        : LibraryImportStageMode::preserveExisting;
    const ImportMode importMode = overwrite
        ? ImportMode::insertOrUpdate
        : ImportMode::insert;

    for (const QString &source : videoSyncs) {
        const QString target =
            QDir(libraryPath).filePath(QFileInfo(source).fileName());
        const LibraryImportStageResult staged =
            fileStager.stage(source, target, stageMode);
        if (!staged.succeeded()) {
            fail(QObject::tr("Copy VideoSync Failed"),
                 QObject::tr("%1 could not be copied to the workout library. %2")
                     .arg(QFileInfo(source).fileName(), staged.error));
            return rollback();
        }

        int mode = 0;
        VideoSyncFile videoSync(target, mode, context);
        if (!videoSync.isValid()
            || !trainDB->importVideoSync(target, videoSync, importMode)) {
            fail(QObject::tr("Import VideoSync Failed"),
                 QObject::tr("%1 could not be written to the workout database.")
                     .arg(QFileInfo(source).fileName()));
            return rollback();
        }
        selectedVideoSync = target;
    }

    for (const QString &source : workouts) {
        const QString target =
            QDir(libraryPath).filePath(QFileInfo(source).fileName());
        const LibraryImportStageResult staged =
            fileStager.stage(source, target, stageMode);
        if (!staged.succeeded()) {
            fail(QObject::tr("Copy Workout Failed"),
                 QObject::tr("%1 could not be copied to the workout library. %2")
                     .arg(QFileInfo(source).fileName(), staged.error));
            return rollback();
        }

        ErgFile workout(target, ErgFileFormat::unknown, context);
        if (!workout.isValid()
            || !trainDB->importWorkout(target, workout, importMode)) {
            fail(QObject::tr("Import Workout Failed"),
                 QObject::tr("%1 could not be written to the workout database.")
                     .arg(QFileInfo(source).fileName()));
            return rollback();
        }
        selectedWorkout = target;
    }

    if (!pendingRefs.isEmpty()) {
        QString snapshotError;
        if (!metadataSnapshot.capture(metadataPath(context), snapshotError)) {
            fail(QObject::tr("Import Video Failed"),
                 QObject::tr("The existing library metadata could not be read: %1")
                     .arg(snapshotError));
            return rollback();
        }

        for (const QString &ref : pendingRefs) library->refs.append(ref);
        QString serializeError;
        metadataTouched = true;
        const bool serialized = LibraryParser::serialize(
            context->athlete->home->root(), &serializeError);
        for (const QString &ref : pendingRefs) library->refs.removeOne(ref);
        if (!serialized) {
            fail(QObject::tr("Import Video Failed"),
                 QObject::tr("The media library references could not be saved. %1")
                     .arg(serializeError));
            return rollback();
        }
    }

    if (!transaction.commit()) {
        fail(QObject::tr("Import Failed"),
             QObject::tr("Could not commit the workout database transaction."));
        return rollback();
    }

    for (const QString &ref : pendingRefs) library->refs.append(ref);
    if (!selectedVideo.isEmpty()) context->notifySelectVideo(selectedVideo);
    if (!selectedWorkout.isEmpty()) context->notifySelectWorkout(selectedWorkout);
    if (!selectedVideoSync.isEmpty()) {
        context->notifySelectVideoSync(selectedVideoSync);
    }

    for (const QString &path : fileStager.finalize()) {
        qWarning() << "Workout import could not remove backup" << path;
    }

    result.succeeded = true;
    if (onSuccess) onSuccess();
    return result;
}
