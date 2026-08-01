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
#include "AtomicFileWriter.h"
#include "RideCacheModel.h"
#include "RideFileCacheIntegrity.h"

#include <QPointer>
#include <QTemporaryFile>

#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
bool rideCacheRemovalShouldFailCleanup(
    const QString &path);
bool rideCacheRemovalShouldFailMove(
    const QString &sourcePath,
    const QString &targetPath);
void rideCacheRemovalMutateBeforeMove(
    const QString &sourcePath,
    const QString &targetPath);
bool rideCacheRemovalShouldFailSync(
    const QString &path);
#endif

namespace {

struct RemovalFileSnapshot
{
    QString path;
    QString description;
    bool exists = false;
    AtomicFileSnapshot contents;
};

struct StorageRemovalResult
{
    RideCache::RemovalStatus status =
        RideCache::RemovalStatus::Rejected;
    QString error;
    QStringList recoveryPaths;

    bool committed() const
    {
        return status == RideCache::RemovalStatus::Committed
            || status
                == RideCache::RemovalStatus::CommittedCleanupPending;
    }
};

struct PendingRideRemoval
{
    QPointer<RideItem> item;
    QString fileName;
    QString path;
    bool planned = false;
};

class RemovalOperationGuard final
{
public:
    explicit RemovalOperationGuard(
        const std::shared_ptr<bool> &inProgress)
        : inProgress_(inProgress)
    {
        if (inProgress_ && !*inProgress_) {
            *inProgress_ = true;
            acquired_ = true;
        }
    }

    ~RemovalOperationGuard()
    {
        if (acquired_) *inProgress_ = false;
    }

    bool acquired() const { return acquired_; }

private:
    std::shared_ptr<bool> inProgress_;
    bool acquired_ = false;
};

using RemovalPrepareFunction =
    std::function<bool(QString &error)>;

bool removalEntryExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool moveRemovalFile(
    const QString &sourcePath,
    const QString &targetPath,
    QString &error)
{
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (rideCacheRemovalShouldFailMove(
            sourcePath, targetPath)) {
        error = QObject::tr(
            "Injected activity removal move failure");
        return false;
    }
    rideCacheRemovalMutateBeforeMove(
        sourcePath, targetPath);
#endif
    return moveAtomicFile(
        sourcePath, targetPath, error);
}

void appendRemovalError(
    QString &error,
    const QString &detail)
{
    if (detail.isEmpty()) return;
    if (!error.isEmpty()) error += QStringLiteral("; ");
    error += detail;
}

bool syncRemovalDirectory(
    const QString &path,
    QString &error)
{
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (rideCacheRemovalShouldFailSync(path)) {
        error = QObject::tr(
            "Injected activity directory sync failure: %1")
                    .arg(path);
        return false;
    }
#endif
    return syncParentDirectory(path, error);
}

bool validateAndSnapshotRemovalEntry(
    RemovalFileSnapshot &entry,
    bool required,
    QString &error)
{
    const QFileInfo info(entry.path);
    const bool exists =
        info.exists() || info.isSymLink();
    entry.exists = exists;
    if (!exists) {
        if (required) {
            error = QObject::tr(
                "The %1 is missing: %2")
                        .arg(
                            entry.description,
                            entry.path);
            return false;
        }
        return true;
    }
    if (info.isSymLink() || !info.isFile()) {
        error = QObject::tr(
            "The %1 is not a regular file: %2")
                    .arg(
                        entry.description,
                        entry.path);
        return false;
    }
    QString snapshotError;
    if (!captureAtomicFileSnapshot(
            entry.path,
            entry.contents,
            snapshotError)) {
        error = QObject::tr(
            "Cannot snapshot the %1: %2")
                    .arg(
                        entry.description,
                        snapshotError);
        return false;
    }
    return true;
}

bool removalSnapshotMatches(
    const RemovalFileSnapshot &entry,
    QString &error)
{
    if (!entry.exists) {
        if (!removalEntryExists(entry.path))
            return true;
        error = QObject::tr(
            "The %1 was created concurrently: %2")
                    .arg(
                        entry.description,
                        entry.path);
        return false;
    }

    QString snapshotError;
    if (atomicFileMatchesSnapshot(
            entry.path,
            entry.contents,
            snapshotError)) {
        return true;
    }
    error = QObject::tr(
        "The %1 changed during activity removal: %2")
                .arg(
                    entry.description,
                    snapshotError);
    return false;
}

bool copyRemovalSource(
    const RemovalFileSnapshot &source,
    const QString &backupPath,
    QString &stagingPath,
    QString &error)
{
    QFile input(source.path);
    if (!input.open(QIODevice::ReadOnly)) {
        error = QObject::tr(
            "Cannot read the activity source: %1")
                    .arg(input.errorString());
        return false;
    }

    QTemporaryFile output(
        QDir(QFileInfo(backupPath).absolutePath())
            .filePath(
                QStringLiteral(".%1.gc-copy-XXXXXX")
                    .arg(QFileInfo(backupPath).fileName())));
    if (!output.open()) {
        error = QObject::tr(
            "Cannot create the activity backup staging file: %1")
                    .arg(output.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!input.atEnd()) {
        const QByteArray chunk =
            input.read(1024 * 1024);
        if (chunk.isEmpty()
            && input.error()
                != QFileDevice::NoError) {
            error = QObject::tr(
                "Cannot read the activity source: %1")
                        .arg(input.errorString());
            return false;
        }
        if (output.write(chunk) != chunk.size()) {
            error = QObject::tr(
                "Cannot write the activity backup staging file: %1")
                        .arg(output.errorString());
            return false;
        }
        copied += chunk.size();
        hash.addData(chunk);
    }
    if (!output.flush()
        || !syncFileDevice(output, error)) {
        if (error.isEmpty()) {
            error = QObject::tr(
                "Cannot flush the activity backup staging file: %1")
                        .arg(output.errorString());
        }
        return false;
    }
    if (copied != source.contents.size
        || hash.result()
            != source.contents.digest) {
        error = QObject::tr(
            "The copied activity backup does not match the source");
        return false;
    }
    QString snapshotError;
    if (!atomicFileMatchesSnapshot(
            source.path,
            source.contents,
            snapshotError)) {
        error = QObject::tr(
            "The activity source changed while it was being copied: %1")
                    .arg(snapshotError);
        return false;
    }

    stagingPath = output.fileName();
    output.setAutoRemove(false);
    output.close();
    return true;
}

bool removeRemovalFile(
    const QString &path,
    const QString &hookPath)
{
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (rideCacheRemovalShouldFailCleanup(
            hookPath)) {
        return false;
    }
#else
    Q_UNUSED(hookPath)
#endif
    return QFile::remove(path);
}

void addRecoveryPath(
    QStringList &paths,
    const QString &path)
{
    if (!path.isEmpty()
        && removalEntryExists(path)
        && !paths.contains(path)) {
        paths.append(path);
    }
}

class ActivityRemovalTransaction final
{
public:
    ActivityRemovalTransaction(
        QString sourcePath,
        QString backupPath,
        QStringList derivedPaths,
        bool archiveSource,
        RemovalPrepareFunction prepare)
        : source_({
              std::move(sourcePath),
              QObject::tr("activity source"),
              false,
              {}}),
          backup_({
              std::move(backupPath),
              QObject::tr("previous activity backup"),
              false,
              {}}),
          archiveSource_(archiveSource),
          prepare_(std::move(prepare)),
          transactionId_(
              QUuid::createUuid().toString(
                  QUuid::WithoutBraces))
    {
        derived_.reserve(derivedPaths.size());
        for (QString &path : derivedPaths) {
            derived_.append({
                std::move(path),
                QObject::tr("derived activity file"),
                false,
                {}});
        }
        if (archiveSource_) {
            sourceTombstone_ =
                source_.path
                + QStringLiteral(".gc-remove-")
                + transactionId_;
            previousBackup_ =
                backup_.path
                + QStringLiteral(".gc-previous-")
                + transactionId_;
        }
    }

    StorageRemovalResult execute()
    {
        StorageRemovalResult result;
        if (!lockAndSnapshot(result.error))
            return result;

        if (!archiveSource_) {
            if (!prepareRemoval(result.error)
                || !revalidateBeforeCommit(
                    result.error)) {
                return result;
            }
            result.status =
                RideCache::RemovalStatus::Committed;
            cleanupCommittedFiles(result);
            return result;
        }

        if (!copyRemovalSource(
                source_, backup_.path,
                backupStaging_, result.error)) {
            cleanupUnpublishedStaging(result);
            return result;
        }

        if (!revalidateBeforeCommit(result.error)) {
            cleanupUnpublishedStaging(result);
            return result;
        }
        if (!prepareRemoval(result.error)
            || !revalidateBeforeCommit(
                result.error)) {
            cleanupUnpublishedStaging(result);
            return result;
        }

        if (backup_.exists) {
            QString moveError;
            if (!moveRemovalFile(
                    backup_.path,
                    previousBackup_,
                    moveError)) {
                previousBackupMoved_ =
                    removalEntryExists(
                        previousBackup_);
                result.error = QObject::tr(
                    "Cannot preserve the previous activity backup: %1")
                                   .arg(moveError);
                return rollbackResult(result.error);
            }
            previousBackupMoved_ = true;
            QString verifyError;
            if (!atomicFileMatchesSnapshot(
                    previousBackup_,
                    backup_.contents,
                    verifyError)) {
                result.error = QObject::tr(
                    "The preserved activity backup is not intact: %1")
                                   .arg(verifyError);
                return rollbackResult(result.error);
            }
            QString syncError;
            if (!syncRemovalDirectory(
                    backup_.path, syncError)) {
                result.error = syncError;
                return rollbackResult(result.error);
            }
        }

        QString moveError;
        if (!moveRemovalFile(
                backupStaging_,
                backup_.path,
                moveError)) {
            backupPublished_ =
                removalEntryExists(backup_.path);
            result.error = QObject::tr(
                "Cannot publish the activity backup: %1")
                               .arg(moveError);
            return rollbackResult(result.error);
        }
        backupPublished_ = true;
        backupStaging_.clear();
        QString verifyError;
        if (!atomicFileMatchesSnapshot(
                backup_.path,
                source_.contents,
                verifyError)) {
            result.error = QObject::tr(
                "The published activity backup is not intact: %1")
                               .arg(verifyError);
            return rollbackResult(result.error);
        }
        QString syncError;
        if (!syncRemovalDirectory(
                backup_.path, syncError)) {
            result.error = syncError;
            return rollbackResult(result.error);
        }

        if (!moveRemovalFile(
                source_.path,
                sourceTombstone_,
                moveError)) {
            sourceTombstoned_ =
                removalEntryExists(
                    sourceTombstone_);
            result.error = QObject::tr(
                "Cannot stage the activity source for removal: %1")
                               .arg(moveError);
            return rollbackResult(result.error);
        }
        sourceTombstoned_ = true;
        if (!atomicFileMatchesSnapshot(
                sourceTombstone_,
                source_.contents,
                verifyError)) {
            result.error = QObject::tr(
                "The activity source changed before archival: %1")
                               .arg(verifyError);
            return rollbackResult(result.error);
        }
        if (!syncRemovalDirectory(
                source_.path, syncError)) {
            result.error = syncError;
            return rollbackResult(result.error);
        }

        result.status =
            RideCache::RemovalStatus::Committed;
        cleanupCommittedFiles(result);
        return result;
    }

private:
    bool lockAndSnapshot(QString &error)
    {
        QStringList paths;
        if (archiveSource_) {
            paths.append(source_.path);
            paths.append(backup_.path);
            paths.append(sourceTombstone_);
            paths.append(previousBackup_);
        }
        for (const RemovalFileSnapshot &entry :
             std::as_const(derived_)) {
            if (QFileInfo(entry.path)
                    .absoluteDir().exists()) {
                paths.append(entry.path);
            }
        }
        if (!locks_.lock(paths, error))
            return false;

        if (archiveSource_
            && (!validateAndSnapshotRemovalEntry(
                    source_, true, error)
                || !validateAndSnapshotRemovalEntry(
                    backup_, false, error))) {
            return false;
        }
        for (RemovalFileSnapshot &entry : derived_) {
            if (!validateAndSnapshotRemovalEntry(
                    entry, false, error)) {
                return false;
            }
        }
        if (archiveSource_
            && (removalEntryExists(sourceTombstone_)
                || removalEntryExists(previousBackup_))) {
            error = QObject::tr(
                "An activity removal recovery path already exists");
            return false;
        }
        return true;
    }

    bool revalidateBeforeCommit(QString &error)
    {
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalMutateBeforeMove(
            source_.path, backup_.path);
        if (backup_.exists) {
            rideCacheRemovalMutateBeforeMove(
                backup_.path, previousBackup_);
        }
        for (const RemovalFileSnapshot &entry :
             std::as_const(derived_)) {
            if (entry.exists) {
                rideCacheRemovalMutateBeforeMove(
                    entry.path, entry.path);
            }
        }
#endif
        if (archiveSource_) {
            if (!removalSnapshotMatches(
                    source_, error)
                || !removalSnapshotMatches(
                    backup_, error)) {
                return false;
            }
        }
        for (const RemovalFileSnapshot &entry :
             std::as_const(derived_)) {
            if (!removalSnapshotMatches(entry, error))
                return false;
        }
        return true;
    }

    bool prepareRemoval(QString &error)
    {
        if (!prepare_) return true;
        return prepare_(error);
    }

    bool cleanupPath(
        const QString &actualPath,
        const QString &hookPath,
        const AtomicFileSnapshot &expected,
        StorageRemovalResult &result)
    {
        if (!removalEntryExists(actualPath))
            return true;

        QString verifyError;
        if (!atomicFileMatchesSnapshot(
                actualPath, expected,
                verifyError)) {
            appendRemovalError(
                result.error,
                QObject::tr(
                    "A cleanup file changed and was retained: %1 (%2)")
                        .arg(actualPath, verifyError));
            addRecoveryPath(
                result.recoveryPaths,
                actualPath);
            return false;
        }
        if (!removeRemovalFile(
                actualPath, hookPath)) {
            appendRemovalError(
                result.error,
                QObject::tr(
                    "Cannot remove an activity cleanup file: %1")
                        .arg(actualPath));
            addRecoveryPath(
                result.recoveryPaths,
                actualPath);
            return false;
        }
        QString syncError;
        if (!syncRemovalDirectory(
                actualPath, syncError)) {
            appendRemovalError(
                result.error, syncError);
            return false;
        }
        return true;
    }

    void cleanupCommittedFiles(
        StorageRemovalResult &result)
    {
        bool complete = true;
        for (const RemovalFileSnapshot &entry :
             std::as_const(derived_)) {
            if (entry.exists
                && !cleanupPath(
                    entry.path,
                    entry.path,
                    entry.contents,
                    result)) {
                complete = false;
            }
        }
        if (archiveSource_
            && backup_.exists
            && !cleanupPath(
                previousBackup_,
                backup_.path,
                backup_.contents,
                result)) {
            complete = false;
        }
        if (archiveSource_
            && !cleanupPath(
                sourceTombstone_,
                source_.path,
                source_.contents,
                result)) {
            complete = false;
        }
        if (!complete) {
            result.status = RideCache::RemovalStatus::
                CommittedCleanupPending;
            if (!result.recoveryPaths.isEmpty()) {
                appendRemovalError(
                    result.error,
                    QObject::tr(
                        "Cleanup files were retained at: %1")
                        .arg(result.recoveryPaths.join(
                            QStringLiteral(", "))));
            }
        }
    }

    void cleanupUnpublishedStaging(
        StorageRemovalResult &result)
    {
        if (backupStaging_.isEmpty()
            || !removalEntryExists(
                backupStaging_)) {
            return;
        }
        if (!QFile::remove(backupStaging_)) {
            result.status = RideCache::RemovalStatus::RecoveryRequired;
            appendRemovalError(
                result.error,
                QObject::tr(
                    "Cannot remove the unpublished activity backup: %1")
                    .arg(backupStaging_));
            addRecoveryPath(
                result.recoveryPaths,
                backupStaging_);
            return;
        }
        QString syncError;
        if (!syncRemovalDirectory(
                backupStaging_, syncError)) {
            result.status = RideCache::RemovalStatus::RecoveryRequired;
            appendRemovalError(
                result.error, syncError);
        }
        backupStaging_.clear();
    }

    bool removeVerifiedRollbackFile(
        const QString &path,
        const AtomicFileSnapshot &expected,
        const QString &description,
        QString &error)
    {
        if (!removalEntryExists(path)) return true;

        QString verifyError;
        if (!atomicFileMatchesSnapshot(
                path, expected, verifyError)) {
            appendRemovalError(
                error,
                QObject::tr(
                    "Cannot remove the unverified %1 during rollback: %2")
                    .arg(description, verifyError));
            return false;
        }
        if (!QFile::remove(path)) {
            appendRemovalError(
                error,
                QObject::tr(
                    "Cannot remove the %1 during rollback: %2")
                    .arg(description, path));
            return false;
        }
        QString syncError;
        if (!syncRemovalDirectory(path, syncError)) {
            appendRemovalError(error, syncError);
            return false;
        }
        return true;
    }

    bool restorePreviousBackup(QString &error)
    {
        if (!backup_.exists) {
            if (removalEntryExists(previousBackup_)
                || removalEntryExists(backup_.path)) {
                appendRemovalError(
                    error,
                    QObject::tr(
                        "An unexpected activity backup remains after rollback"));
                return false;
            }
            previousBackupMoved_ = false;
            return true;
        }

        if (!previousBackupMoved_) {
            QString verifyError;
            if (atomicFileMatchesSnapshot(
                    backup_.path,
                    backup_.contents,
                    verifyError)) {
                return true;
            }
            appendRemovalError(
                error,
                QObject::tr(
                    "The previous activity backup is not intact after rollback: %1")
                    .arg(verifyError));
            return false;
        }

        if (!removalEntryExists(backup_.path)
            && removalEntryExists(previousBackup_)) {
            QString moveError;
            if (!moveRemovalFile(
                    previousBackup_,
                    backup_.path,
                    moveError)) {
                appendRemovalError(
                    error,
                    QObject::tr(
                        "Cannot restore the previous activity backup: %1")
                        .arg(moveError));
            }
        }

        QString verifyError;
        if (!atomicFileMatchesSnapshot(
                backup_.path,
                backup_.contents,
                verifyError)) {
            appendRemovalError(
                error,
                QObject::tr(
                    "The restored previous activity backup is not intact: %1")
                    .arg(verifyError));
            return false;
        }
        QString syncError;
        if (!syncRemovalDirectory(
                backup_.path, syncError)) {
            appendRemovalError(error, syncError);
            return false;
        }
        if (removalEntryExists(previousBackup_)
            && !removeVerifiedRollbackFile(
                previousBackup_,
                backup_.contents,
                QObject::tr(
                    "duplicate previous activity backup"),
                error)) {
            return false;
        }
        previousBackupMoved_ = false;
        return true;
    }

    bool rollback(QString &error)
    {
        bool restored = true;
        bool sourceDurable = false;

        if (sourceTombstoned_) {
            if (removalEntryExists(source_.path)) {
                QString sourceError;
                if (atomicFileMatchesSnapshot(
                        source_.path,
                        source_.contents,
                        sourceError)) {
                    sourceDurable = true;
                    if (removalEntryExists(
                            sourceTombstone_)) {
                        if (removeVerifiedRollbackFile(
                                sourceTombstone_,
                                source_.contents,
                                QObject::tr(
                                    "duplicate activity source"),
                                error)) {
                            sourceTombstoned_ = false;
                        } else {
                            restored = false;
                        }
                    } else {
                        sourceTombstoned_ = false;
                    }
                } else {
                    appendRemovalError(
                        error,
                        QObject::tr(
                            "The activity source path is occupied by a changed file: %1")
                            .arg(sourceError));
                    restored = false;
                }
            } else if (removalEntryExists(
                           sourceTombstone_)) {
                QString moveError;
                if (!moveRemovalFile(
                        sourceTombstone_,
                        source_.path,
                        moveError)) {
                    appendRemovalError(
                        error,
                        QObject::tr(
                            "Cannot restore the activity source: %1")
                            .arg(moveError));
                    restored = false;
                } else {
                    sourceTombstoned_ = false;
                    QString verifyError;
                    if (!atomicFileMatchesSnapshot(
                            source_.path,
                            source_.contents,
                            verifyError)) {
                        appendRemovalError(
                            error,
                            QObject::tr(
                                "The restored activity source is not intact: %1")
                                .arg(verifyError));
                        restored = false;
                    } else {
                        QString syncError;
                        if (!syncRemovalDirectory(
                                source_.path,
                                syncError)) {
                            appendRemovalError(
                                error, syncError);
                            restored = false;
                        } else {
                            sourceDurable = true;
                        }
                    }
                }
            } else {
                appendRemovalError(
                    error,
                    QObject::tr(
                        "The activity source and its recovery file are both missing"));
                restored = false;
            }
        } else {
            QString verifyError;
            if (atomicFileMatchesSnapshot(
                    source_.path,
                    source_.contents,
                    verifyError)) {
                sourceDurable = true;
            } else {
                appendRemovalError(
                    error,
                    QObject::tr(
                        "The activity source is not intact after rollback: %1")
                        .arg(verifyError));
                restored = false;
            }
        }

        if (sourceDurable) {
            bool publishedBackupRemoved = true;
            if (backupPublished_) {
                publishedBackupRemoved =
                    removeVerifiedRollbackFile(
                        backup_.path,
                        source_.contents,
                        QObject::tr(
                            "new activity backup"),
                        error);
                if (publishedBackupRemoved)
                    backupPublished_ = false;
                else
                    restored = false;
            }
            if (publishedBackupRemoved
                && !restorePreviousBackup(error)) {
                restored = false;
            }
        } else {
            restored = false;
            appendRemovalError(
                error,
                QObject::tr(
                    "The published activity backup was retained because the source was not durably restored"));
        }

        bool verifiedCurrentBackup = false;
        if (removalEntryExists(backup_.path)) {
            QString verifyError;
            verifiedCurrentBackup =
                atomicFileMatchesSnapshot(
                    backup_.path,
                    source_.contents,
                    verifyError);
        }
        if (sourceDurable || verifiedCurrentBackup) {
            StorageRemovalResult stagingCleanup;
            cleanupUnpublishedStaging(
                stagingCleanup);
            appendRemovalError(
                error, stagingCleanup.error);
            if (!stagingCleanup.error.isEmpty()
                || !stagingCleanup.recoveryPaths.isEmpty()) {
                restored = false;
            }
        } else if (!backupStaging_.isEmpty()
                   && removalEntryExists(
                       backupStaging_)) {
            appendRemovalError(
                error,
                QObject::tr(
                    "The unpublished activity backup was retained for recovery"));
            restored = false;
        }

        if (!sourceDurable)
            restored = false;
        if (restored
            && (removalEntryExists(sourceTombstone_)
                || removalEntryExists(previousBackup_)
                || removalEntryExists(backupStaging_))) {
            appendRemovalError(
                error,
                QObject::tr(
                    "Activity recovery files remain after rollback"));
            restored = false;
        }
        return restored;
    }

    StorageRemovalResult rollbackResult(
        const QString &primaryError)
    {
        StorageRemovalResult result;
        result.error = primaryError;
        QString rollbackError;
        const bool restored = rollback(
            rollbackError);
        appendRemovalError(
            result.error, rollbackError);
        result.status = restored
            ? RideCache::RemovalStatus::RolledBack
            : RideCache::RemovalStatus::RecoveryRequired;
        addRecoveryPath(
            result.recoveryPaths,
            sourceTombstone_);
        addRecoveryPath(
            result.recoveryPaths,
            previousBackup_);
        addRecoveryPath(
            result.recoveryPaths,
            backupStaging_);
        if (!restored) {
            addRecoveryPath(
                result.recoveryPaths,
                source_.path);
            addRecoveryPath(
                result.recoveryPaths,
                backup_.path);
        }
        if (!result.recoveryPaths.isEmpty()) {
            appendRemovalError(
                result.error,
                QObject::tr(
                    "Recovery files are available at: %1")
                    .arg(result.recoveryPaths.join(
                        QStringLiteral(", "))));
        }
        return result;
    }

    RemovalFileSnapshot source_;
    RemovalFileSnapshot backup_;
    QVector<RemovalFileSnapshot> derived_;
    bool archiveSource_ = false;
    RemovalPrepareFunction prepare_;
    QString transactionId_;
    QString backupStaging_;
    QString sourceTombstone_;
    QString previousBackup_;
    bool previousBackupMoved_ = false;
    bool backupPublished_ = false;
    bool sourceTombstoned_ = false;
    AtomicFileLockSet locks_;
};

StorageRemovalResult removeRideFilesFromStorage(
    const QString &sourcePath,
    const QString &backupPath,
    const QStringList &derivedPaths,
    bool archiveSource,
    RemovalPrepareFunction prepare)
{
    return ActivityRemovalTransaction(
        sourcePath,
        backupPath,
        derivedPaths,
        archiveSource,
        std::move(prepare)).execute();
}

bool runRemovalDeleteProcessor(
    RideFile *ride,
    QString &error)
{
    if (!ride) return true;
    try {
        DataProcessorFactory::instance()
            .autoProcess(
                ride,
                QStringLiteral("Save"),
                QStringLiteral("DELETE"));
    } catch (const QString &detail) {
        error = detail;
        return false;
    } catch (const std::exception &exception) {
        error = QString::fromLocal8Bit(
            exception.what());
        return false;
    } catch (...) {
        error = QObject::tr(
            "An activity delete processor failed");
        return false;
    }
    return true;
}

void appendRemovalResultError(
    RideCache::RemovalResult &result,
    const QString &detail)
{
    appendRemovalError(result.error, detail);
}

void appendRemovalItemResult(
    RideCache::RemovalResult &result,
    const QString &fileName,
    bool planned,
    RideCache::RemovalStatus status,
    const QString &error = {},
    const QStringList &recoveryPaths = {})
{
    result.items.append({
        fileName,
        planned,
        status,
        error,
        recoveryPaths});
}

void finalizeRemovalBatchStatus(
    RideCache::RemovalResult &result)
{
    result.requestedCount = result.items.size();
    bool rolledBack = false;
    bool cleanupPending = false;
    for (const RideCache::RemovalItemResult &item :
         std::as_const(result.items)) {
        if (item.status
            == RideCache::RemovalStatus::RecoveryRequired) {
            result.status =
                RideCache::RemovalStatus::RecoveryRequired;
            return;
        }
        rolledBack = rolledBack
            || item.status
                == RideCache::RemovalStatus::RolledBack;
        cleanupPending = cleanupPending
            || item.status
                == RideCache::RemovalStatus::
                    CommittedCleanupPending;
    }

    if (result.requestedCount > 0
        && result.affectedCount
            == result.requestedCount) {
        result.status = cleanupPending
            ? RideCache::RemovalStatus::
                CommittedCleanupPending
            : RideCache::RemovalStatus::Committed;
    } else if (result.affectedCount > 0) {
        result.status =
            RideCache::RemovalStatus::PartiallyCommitted;
    } else if (rolledBack) {
        result.status =
            RideCache::RemovalStatus::RolledBack;
    } else {
        result.status =
            RideCache::RemovalStatus::Rejected;
    }
}

} // namespace

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

QStringList
RideCache::derivedFilePathsForRemoval(
    RideItem *rideToDelete) const
{
    if (!rideToDelete) return {};
    const QString &fileName =
        rideToDelete->fileName;
    const QString baseName =
        QFileInfo(fileName).baseName();
    if (baseName.isEmpty())
        return {};

    QStringList paths;
    const QString cpxPath =
        cpxCachePathForActivity(
            fileName,
            rideToDelete->planned);
    if (!cpxPath.isEmpty())
        paths.append(cpxPath);

    const QString sidecarKey =
        baseName.normalized(
            QString::NormalizationForm_C)
            .toCaseFolded();
    bool legacySidecarsAreShared = false;
    for (const RideItem *ride : rides_) {
        if (!ride || ride == rideToDelete)
            continue;
        if (QFileInfo(ride->fileName)
                .baseName()
                .normalized(
                    QString::NormalizationForm_C)
                .toCaseFolded()
            == sidecarKey) {
            legacySidecarsAreShared = true;
            break;
        }
    }
    if (!legacySidecarsAreShared) {
        for (const QString &extension :
             {QStringLiteral("cpi"),
              QStringLiteral("notes")}) {
            paths.append(
                context->athlete->home
                    ->cache()
                    .filePath(
                        baseName
                        + QLatin1Char('.')
                        + extension));
        }
    }
    return paths;
}

bool
RideCache::removeCurrentRide()
{
    return removeCurrentRideResult()
        .allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeCurrentRideResult()
{
    if (!context->ride) {
        RemovalResult result;
        result.requestedCount = 1;
        result.error = tr("No activity is selected");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    return removeRideResult(context->ride);
}

bool
RideCache::removeRide(const QString &filenameToDelete)
{
    return removeRideResult(
        filenameToDelete).allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeRideResult(
    const QString &filenameToDelete)
{
    RideItem *ride =
        uniqueRideForFileName(filenameToDelete);
    if (!ride) {
        RemovalResult result;
        result.requestedCount = 1;
        result.error = tr(
            "Activity deletion target was not found or is ambiguous: %1")
                           .arg(filenameToDelete);
        appendRemovalItemResult(
            result, filenameToDelete, false,
            RemovalStatus::Rejected,
            result.error);
        qWarning().noquote() << result.error;
        return result;
    }
    return removeRideResult(ride);
}

bool
RideCache::removeRide(
    const QString &filenameToDelete,
    bool planned)
{
    return removeRideResult(
        filenameToDelete,
        planned).allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeRideResult(
    const QString &filenameToDelete,
    bool planned)
{
    RideItem *ride =
        uniqueRideForIdentity(
            filenameToDelete, planned);
    if (!ride) {
        RemovalResult result;
        result.requestedCount = 1;
        result.error = tr(
            "Activity deletion target was not found or is ambiguous: %1")
                           .arg(filenameToDelete);
        appendRemovalItemResult(
            result, filenameToDelete, planned,
            RemovalStatus::Rejected,
            result.error);
        qWarning().noquote() << result.error;
        return result;
    }
    return removeRideResult(ride);
}

bool
RideCache::removeRide(RideItem *rideToDelete)
{
    return removeRideResult(
        rideToDelete).allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeRideResult(
    RideItem *rideToDelete)
{
    return removeRideEntry(
        rideToDelete, RideFileDisposition::Archive);
}

bool
RideCache::removeArchivedRide(const QString &filenameToDelete)
{
    return removeArchivedRideResult(
        filenameToDelete).allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeArchivedRideResult(
    const QString &filenameToDelete)
{
    RideItem *ride =
        uniqueRideForFileName(filenameToDelete);
    if (!ride) {
        RemovalResult result;
        result.requestedCount = 1;
        result.error = tr(
            "Archived activity deletion target was not found or is ambiguous: %1")
                           .arg(filenameToDelete);
        appendRemovalItemResult(
            result, filenameToDelete, false,
            RemovalStatus::Rejected,
            result.error);
        qWarning().noquote() << result.error;
        return result;
    }
    return removeArchivedRideResult(ride);
}

bool
RideCache::removeArchivedRide(RideItem *rideToDelete)
{
    return removeArchivedRideResult(
        rideToDelete).allLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeArchivedRideResult(
    RideItem *rideToDelete)
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
    return removeRidesResult(
        filenamesToDelete,
        triggerRefresh).anyLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeRidesResult(
    const QStringList &filenamesToDelete,
    bool triggerRefresh)
{
    RemovalResult result;
    if (filenamesToDelete.isEmpty()) {
        result.error = tr(
            "No activities were requested for deletion");
        return result;
    }

    QList<RideItem*> ridesToDelete;
    ridesToDelete.reserve(
        filenamesToDelete.size());
    QSet<QString> requestedNames;
    for (const QString &fileName :
         filenamesToDelete) {
        if (requestedNames.contains(fileName))
            continue;
        requestedNames.insert(fileName);
        RideItem *ride =
            uniqueRideForFileName(fileName);
        if (!ride) {
            const QString detail = tr(
                "Activity deletion target was not found or is ambiguous: %1")
                    .arg(fileName);
            appendRemovalResultError(
                result,
                detail);
            appendRemovalItemResult(
                result, fileName, false,
                RemovalStatus::Rejected,
                detail);
            continue;
        }
        ridesToDelete.append(ride);
    }
    if (ridesToDelete.isEmpty()) {
        finalizeRemovalBatchStatus(result);
        qWarning().noquote() << result.error;
        return result;
    }

    RemovalResult removed =
        removeRidesResult(
            ridesToDelete, triggerRefresh);
    result.affectedCount = removed.affectedCount;
    result.recoveryPaths.append(
        removed.recoveryPaths);
    result.items.append(removed.items);
    appendRemovalResultError(
        result, removed.error);
    finalizeRemovalBatchStatus(result);
    if (!result.allLogicallyRemoved()
        && !result.error.isEmpty()) {
        qWarning().noquote() << result.error;
    }
    return result;
}

bool
RideCache::removeRides(
    const QList<RideItem*> &ridesToDelete,
    bool triggerRefresh)
{
    return removeRidesResult(
        ridesToDelete,
        triggerRefresh).anyLogicallyRemoved();
}

RideCache::RemovalResult
RideCache::removeRidesResult(
    const QList<RideItem*> &ridesToDelete,
    bool triggerRefresh)
{
    RemovalResult result;
    const QPointer<RideCache> guardedCache(this);
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    const QPointer<RideCacheModel> guardedModel(model_);
    const QPointer<Estimator> guardedEstimator(estimator);
    const auto operationOwnersAreStable = [=] {
        return guardedCache && guardedContext
            && guardedAthlete && guardedModel
            && guardedEstimator
            && guardedCache->context
                == guardedContext.data()
            && guardedContext->athlete
                == guardedAthlete.data()
            && guardedAthlete->context
                == guardedContext.data()
            && guardedAthlete->rideCache
                == guardedCache.data()
            && guardedCache->model_
                == guardedModel.data()
            && guardedCache->estimator
                == guardedEstimator.data();
    };
    QList<PendingRideRemoval> pending;
    pending.reserve(ridesToDelete.size());
    QSet<RideItem*> requested;
    for (RideItem *ride : ridesToDelete) {
        if (!ride || !rides_.contains(ride)) {
            pending.append(PendingRideRemoval{});
            continue;
        }
        if (requested.contains(ride)) continue;
        requested.insert(ride);
        pending.append({
            QPointer<RideItem>(ride),
            ride->fileName,
            ride->path,
            ride->planned});
    }
    if (pending.isEmpty()) {
        result.error = tr(
            "No activities were requested for deletion");
        return result;
    }

    const auto appendNotAttempted =
        [&result, &pending](int first, const QString &detail) {
            appendRemovalResultError(result, detail);
            for (int remaining = first;
                 remaining < pending.size();
                 ++remaining) {
                const PendingRideRemoval &notAttempted =
                    pending.at(remaining);
                appendRemovalItemResult(
                    result,
                    notAttempted.fileName,
                    notAttempted.planned,
                    RideCache::RemovalStatus::NotAttempted,
                    detail);
            }
        };
    const QString ownersUnavailable = QObject::tr(
        "The activity collection disappeared during batch deletion");

    if (!operationOwnersAreStable()) {
        appendNotAttempted(0, ownersUnavailable);
        finalizeRemovalBatchStatus(result);
        return result;
    }
    guardedCache->cancel();
    if (!operationOwnersAreStable()) {
        appendNotAttempted(0, ownersUnavailable);
        finalizeRemovalBatchStatus(result);
        return result;
    }
    for (int pendingIndex = 0;
         pendingIndex < pending.size();
         ++pendingIndex) {
        if (!operationOwnersAreStable()) {
            appendNotAttempted(
                pendingIndex, ownersUnavailable);
            break;
        }
        const PendingRideRemoval &request =
            pending.at(pendingIndex);
        RideItem *const ride = request.item.data();
        if (!ride
            || !guardedCache->rides_.contains(ride)
            || ride->fileName != request.fileName
            || ride->path != request.path
            || ride->planned != request.planned) {
            const QString detail = tr(
                "The activity deletion target changed before it could be removed");
            appendRemovalResultError(
                result,
                detail);
            appendRemovalItemResult(
                result, request.fileName,
                request.planned,
                RemovalStatus::Rejected,
                detail);
            continue;
        }

        RemovalResult removed =
            guardedCache->removeRideEntry(
                ride,
                RideFileDisposition::Archive,
                false);
        result.affectedCount +=
            removed.affectedCount;
        result.recoveryPaths.append(
            removed.recoveryPaths);
        result.items.append(removed.items);
        if (!removed.cleanlyCompleted()) {
            const QString detail = removed.error.isEmpty()
                ? tr("Activity deletion did not complete cleanly")
                : removed.error;
            appendRemovalResultError(
                result,
                tr("%1: %2")
                    .arg(request.fileName, detail));
        }
        if (!operationOwnersAreStable()) {
            appendNotAttempted(
                pendingIndex + 1,
                ownersUnavailable);
            break;
        }
        if (removed.requiresRecovery()) {
            for (int remaining = pendingIndex + 1;
                 remaining < pending.size();
                 ++remaining) {
                const PendingRideRemoval &notAttempted =
                    pending.at(remaining);
                appendRemovalItemResult(
                    result,
                    notAttempted.fileName,
                    notAttempted.planned,
                    RemovalStatus::NotAttempted,
                    tr("Not attempted because storage recovery is required"));
            }
            break;
        }
    }

    finalizeRemovalBatchStatus(result);
    if (result.affectedCount > 0
        && triggerRefresh
        && operationOwnersAreStable()) {
        guardedCache->refresh();
        if (operationOwnersAreStable())
            guardedEstimator->refresh();
    }
    if (!result.allLogicallyRemoved()
        && !result.error.isEmpty()) {
        qWarning().noquote() << result.error;
    }
    return result;
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

RideCache::OperationPreCheck
RideCache::checkRemovalLinks(RideItem *item)
{
    OperationPreCheck check;
    if (!item || !rides_.contains(item)) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The activity is no longer in the activity list");
        return check;
    }

    QList<RideItem*> incomingLinkedItems;
    for (RideItem *candidate : std::as_const(rides_)) {
        if (!candidate
            || candidate == item
            || candidate->getLinkedFileName()
                != item->fileName) {
            continue;
        }
        incomingLinkedItems.append(candidate);
    }

    check.affectedItems.append(item);
    if (!item->hasLinkedActivity()) {
        if (!incomingLinkedItems.isEmpty()) {
            check.canProceed = false;
            check.blockingReason = tr(
                "Another activity has an inconsistent link to this activity");
        }
        return check;
    }

    RideItem *const linkedItem =
        uniqueRideForIdentity(
            item->getLinkedFileName(),
            !item->planned);
    QList<RideItem*> incomingToLinkedItem;
    if (linkedItem) {
        for (RideItem *candidate : std::as_const(rides_)) {
            if (!candidate
                || candidate == linkedItem
                || candidate->getLinkedFileName()
                    != linkedItem->fileName) {
                continue;
            }
            incomingToLinkedItem.append(candidate);
        }
    }
    if (!linkedItem
        || linkedItem->getLinkedFileName()
            != item->fileName
        || incomingLinkedItems.size() != 1
        || incomingLinkedItems.constFirst()
            != linkedItem
        || incomingToLinkedItem.size() != 1
        || incomingToLinkedItem.constFirst()
            != item) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The linked activity pair is missing or inconsistent");
        return check;
    }

    check.affectedItems.append(linkedItem);
    if (item->isDirty())
        check.dirtyItems.append(item);
    if (linkedItem->isDirty())
        check.dirtyItems.append(linkedItem);
    if (!check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *dirtyItem :
             std::as_const(check.dirtyItems)) {
            dirtyNames.append(
                dirtyItem->fileName);
        }
        check.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Deleting will modify the linked activity. You must save or discard changes first.")
                .arg(dirtyNames.join(
                    QLatin1Char('\n')));
    }
    return check;
}

RideCache::RemovalResult
RideCache::removeRideEntry(
    RideItem *rideToDelete,
    RideFileDisposition disposition,
    bool triggerRefresh)
{
    RemovalResult result;
    result.requestedCount = 1;
    RemovalOperationGuard removalGuard(removalInProgress_);
    if (!removalGuard.acquired()) {
        result.error = tr(
            "Another activity deletion is already in progress");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    const QPointer<RideCache> guardedCache(this);
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    const QPointer<RideCacheModel> guardedModel(model_);
    const QPointer<Estimator> guardedEstimator(estimator);
    const auto operationOwnersAreStable = [=] {
        return guardedCache && guardedContext
            && guardedAthlete && guardedModel
            && guardedEstimator
            && guardedCache->context
                == guardedContext.data()
            && guardedContext->athlete
                == guardedAthlete.data()
            && guardedAthlete->rideCache
                == guardedCache.data()
            && guardedCache->model_
                == guardedModel.data()
            && guardedCache->estimator
                == guardedEstimator.data();
    };

    if (!operationOwnersAreStable()) {
        result.error = QObject::tr(
            "The activity collection is no longer available");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }

    if (!rideToDelete) {
        result.error = tr(
            "Cannot delete a null activity");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }

    RideItem *todelete = rideToDelete;
    int index = rides_.indexOf(todelete);

    if (index < 0) {
        result.error = tr(
            "The activity deletion target is no longer in the cache");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        qWarning().noquote() << result.error;
        return result;
    }
    const QPointer<RideItem> deletionItem(todelete);
    if (triggerRefresh) {
        guardedCache->cancel();
        if (!operationOwnersAreStable()
            || !deletionItem
            || !guardedCache->rides_.contains(
                deletionItem.data())) {
            result.error = tr(
                "The activity deletion target changed while refresh was being cancelled");
            appendRemovalItemResult(
                result, {}, false,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        todelete = deletionItem.data();
        index = guardedCache->rides_.indexOf(todelete);
    }
    if (todelete->fileName.isEmpty()) {
        result.error = tr(
            "Cannot delete an activity without a file name");
        appendRemovalItemResult(
            result, {}, todelete->planned,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    const QString filenameToDelete =
        todelete->fileName;
    const bool plannedToDelete =
        todelete->planned;
    const QString pathToDelete =
        todelete->path;
    const bool archiveSource =
        disposition == RideFileDisposition::Archive;
    const QDir activeDirectory =
        todelete->planned
        ? plannedDirectory
        : directory;
    const QString validatedSourcePath =
        RideFileCacheIntegrity::activitySourcePath(
            activeDirectory.absolutePath(),
            filenameToDelete);
    if (validatedSourcePath.isEmpty()) {
        result.error = tr(
            "The activity file name is unsafe: %1")
                           .arg(filenameToDelete);
        appendRemovalItemResult(
            result, filenameToDelete,
            todelete->planned,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    const QString itemSourcePath =
        RideFileCacheIntegrity::activitySourcePath(
            pathToDelete, filenameToDelete);
    const QDir itemDirectory(pathToDelete);
    const QString canonicalItemDirectory =
        itemDirectory.canonicalPath();
    const QString canonicalActiveDirectory =
        activeDirectory.canonicalPath();
    const QString itemDirectoryIdentity =
        canonicalItemDirectory.isEmpty()
        ? QDir::cleanPath(itemDirectory.absolutePath())
        : canonicalItemDirectory;
    const QString activeDirectoryIdentity =
        canonicalActiveDirectory.isEmpty()
        ? QDir::cleanPath(activeDirectory.absolutePath())
        : canonicalActiveDirectory;
    if (itemSourcePath.isEmpty()
        || itemDirectoryIdentity
            != activeDirectoryIdentity) {
        result.error = tr(
            "The activity source does not match its cache namespace: %1")
                           .arg(filenameToDelete);
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }

    QString sourcePath = validatedSourcePath;
    QString backupPath;
    if (archiveSource) {
        QDir backupDirectory =
            context->athlete->home
                ->fileBackup();
        if (todelete->planned) {
            const QString plannedBackupPath =
                backupDirectory.filePath(
                    QStringLiteral("planned"));
            QFileInfo plannedBackupInfo(
                plannedBackupPath);
            const bool plannedBackupExisted =
                plannedBackupInfo.exists()
                || plannedBackupInfo.isSymLink();
            if (plannedBackupExisted
                && (plannedBackupInfo.isSymLink()
                    || !plannedBackupInfo.isDir())) {
                result.error = tr(
                    "The planned activity backup path is not a regular directory");
                appendRemovalItemResult(
                    result, filenameToDelete,
                    todelete->planned,
                    RemovalStatus::Rejected,
                    result.error);
                return result;
            }
            if (!plannedBackupExisted
                && !QDir().mkpath(
                    plannedBackupPath)) {
                result.error = tr(
                    "Cannot create the planned activity backup directory");
                appendRemovalItemResult(
                    result, filenameToDelete,
                    todelete->planned,
                    RemovalStatus::Rejected,
                    result.error);
                return result;
            }
            plannedBackupInfo.refresh();
            if (plannedBackupInfo.isSymLink()
                || !plannedBackupInfo.isDir()) {
                result.error = tr(
                    "The planned activity backup path is not a regular directory");
                appendRemovalItemResult(
                    result, filenameToDelete,
                    todelete->planned,
                    RemovalStatus::Rejected,
                    result.error);
                return result;
            }
            QString syncError;
            if (!syncRemovalDirectory(
                    plannedBackupPath,
                    syncError)) {
                result.error = syncError;
                appendRemovalItemResult(
                    result, filenameToDelete,
                    todelete->planned,
                    RemovalStatus::Rejected,
                    result.error);
                return result;
            }
            backupDirectory =
                QDir(plannedBackupPath);
        }
        backupPath =
            RideFileCacheIntegrity::activitySourcePath(
                backupDirectory.absolutePath(),
                filenameToDelete
                    + QStringLiteral(".bak"));
        if (backupPath.isEmpty()) {
            result.error = tr(
                "The activity backup path is unsafe");
            appendRemovalItemResult(
                result, filenameToDelete,
                todelete->planned,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
    }

    const auto hasDeletionSourceIdentity =
        [filenameToDelete, plannedToDelete,
         activeDirectoryIdentity](RideItem *candidate) {
            if (!candidate
                || candidate->fileName != filenameToDelete
                || candidate->planned != plannedToDelete) {
                return false;
            }
            const QDir candidateDirectory(candidate->path);
            const QString canonicalCandidateDirectory =
                candidateDirectory.canonicalPath();
            const QString candidateDirectoryIdentity =
                canonicalCandidateDirectory.isEmpty()
                ? QDir::cleanPath(
                    candidateDirectory.absolutePath())
                : canonicalCandidateDirectory;
            return candidateDirectoryIdentity
                == activeDirectoryIdentity;
        };
    const auto deletionTargetIsStable =
        [guardedCache, deletionItem, filenameToDelete,
         plannedToDelete, pathToDelete,
         operationOwnersAreStable,
         hasDeletionSourceIdentity] {
            if (!operationOwnersAreStable()) return false;
            RideItem *const item = deletionItem.data();
            if (!item
                || !guardedCache->rides_.contains(item)
                || item->fileName != filenameToDelete
                || item->planned != plannedToDelete
                || item->path != pathToDelete) {
                return false;
            }
            int identityMatches = 0;
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (hasDeletionSourceIdentity(candidate))
                    ++identityMatches;
            }
            return identityMatches == 1;
        };
    if (!deletionTargetIsStable()) {
        result.error = QObject::tr(
            "The activity source identity is not unique in the cache");
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    const QStringList derivedPaths =
        guardedCache->derivedFilePathsForRemoval(todelete);
    const QString sidecarKey =
        QFileInfo(filenameToDelete)
            .baseName()
            .normalized(QString::NormalizationForm_C)
            .toCaseFolded();
    const QString legacyCpiPath =
        guardedAthlete->home->cache().filePath(
            QFileInfo(filenameToDelete).baseName()
            + QStringLiteral(".cpi"));
    const QString legacyNotesPath =
        guardedAthlete->home->cache().filePath(
            QFileInfo(filenameToDelete).baseName()
            + QStringLiteral(".notes"));
    const bool removesLegacySidecars =
        derivedPaths.contains(legacyCpiPath)
        || derivedPaths.contains(legacyNotesPath);
    const auto sidecarOwnershipIsStable =
        [guardedCache, deletionItem, sidecarKey,
         removesLegacySidecars,
         operationOwnersAreStable] {
            if (!removesLegacySidecars) return true;
            if (!operationOwnersAreStable()) return false;
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (!candidate
                    || candidate == deletionItem.data()) {
                    continue;
                }
                const QString candidateKey =
                    QFileInfo(candidate->fileName)
                        .baseName()
                        .normalized(
                            QString::NormalizationForm_C)
                        .toCaseFolded();
                if (candidateKey == sidecarKey)
                    return false;
            }
            return true;
        };
    QPointer<RideItem> linkedItem;
    QString linkedFileName;
    QString linkedOriginalFileName;
    QString linkedOriginalPath;
    QString linkedOriginalSourcePath;
    bool linkedOriginalPlanned = false;
    QString linkedExpectedFileName;
    QString linkedExpectedPath;
    bool linkedExpectedPlanned = false;
    bool linkedItemWasSaved = false;
    bool linkedIdentityChanged = false;

    const auto appendRecoveryPath =
        [&result](const QString &path) {
            if (!path.isEmpty()
                && !result.recoveryPaths.contains(path)) {
                result.recoveryPaths.append(path);
            }
        };
    const auto linkedIdentityMatches =
        [guardedCache, operationOwnersAreStable,
         &linkedItem,
         &linkedExpectedFileName,
         &linkedExpectedPath,
         &linkedExpectedPlanned] {
            if (!operationOwnersAreStable()) return false;
            RideItem *const item = linkedItem.data();
            return item
                && guardedCache->rides_.contains(item)
                && item->fileName
                    == linkedExpectedFileName
                && item->path
                    == linkedExpectedPath
                && item->planned
                    == linkedExpectedPlanned;
        };
    const auto directoryIdentity =
        [](const QString &path) {
            const QDir directory(path);
            const QString canonicalPath =
                directory.canonicalPath();
            return canonicalPath.isEmpty()
                ? QDir::cleanPath(directory.absolutePath())
                : canonicalPath;
        };
    const auto acceptLinkedIdentity =
        [guardedCache, operationOwnersAreStable,
         directoryIdentity, &linkedItem,
         &linkedOriginalFileName,
         &linkedOriginalPath,
         &linkedOriginalPlanned,
         &linkedExpectedFileName,
         &linkedExpectedPath,
         &linkedExpectedPlanned,
         &linkedIdentityChanged](QString &error) {
            if (!operationOwnersAreStable()) {
                error = QObject::tr(
                    "The activity collection disappeared while checking the linked activity");
                return false;
            }
            RideItem *const item = linkedItem.data();
            if (!item
                || !guardedCache->rides_.contains(item)) {
                error = QObject::tr(
                    "The linked activity disappeared while its identity was being checked");
                return false;
            }
            if (item->planned != linkedOriginalPlanned) {
                error = QObject::tr(
                    "The linked activity changed cache namespaces during deletion");
                return false;
            }

            const QDir cacheDirectory = item->planned
                ? guardedCache->plannedDirectory
                : guardedCache->directory;
            const QString savedSourcePath =
                RideFileCacheIntegrity::activitySourcePath(
                    item->path, item->fileName);
            const QFileInfo savedSource(savedSourcePath);
            if (savedSourcePath.isEmpty()
                || directoryIdentity(item->path)
                    != directoryIdentity(
                        cacheDirectory.absolutePath())
                || !savedSource.exists()
                || savedSource.isSymLink()
                || !savedSource.isFile()) {
                error = QObject::tr(
                    "The linked activity does not have a safe cache identity");
                return false;
            }

            int identityMatches = 0;
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (!candidate
                    || candidate->fileName != item->fileName
                    || candidate->planned != item->planned
                    || directoryIdentity(candidate->path)
                        != directoryIdentity(item->path)) {
                    continue;
                }
                ++identityMatches;
            }
            if (identityMatches != 1) {
                error = QObject::tr(
                    "The linked activity identity is not unique in the cache");
                return false;
            }

            linkedIdentityChanged =
                linkedIdentityChanged
                || item->fileName
                    != linkedOriginalFileName
                || item->path != linkedOriginalPath
                || item->planned
                    != linkedOriginalPlanned;
            linkedExpectedFileName = item->fileName;
            linkedExpectedPath = item->path;
            linkedExpectedPlanned = item->planned;
            return true;
        };
    const auto currentLinkedSourcePath =
        [guardedCache, &linkedItem] {
            if (!guardedCache) return QString();
            RideItem *const item = linkedItem.data();
            if (!item
                || !guardedCache->rides_.contains(item))
                return QString();
            return RideFileCacheIntegrity::activitySourcePath(
                item->path, item->fileName);
        };
    const auto appendLinkedRecoveryPaths =
        [&appendRecoveryPath,
         &currentLinkedSourcePath,
         &linkedOriginalSourcePath,
         &sourcePath] {
            appendRecoveryPath(sourcePath);
            appendRecoveryPath(
                linkedOriginalSourcePath);
            appendRecoveryPath(
                currentLinkedSourcePath());
        };
    const auto restoreLinkedPeer =
        [guardedCache, operationOwnersAreStable,
         deletionTargetIsStable,
         &linkedItem, &linkedFileName,
         &linkedIdentityMatches,
         &acceptLinkedIdentity](QString &error) {
            if (!operationOwnersAreStable()) {
                error = QObject::tr(
                    "The activity collection disappeared before the linked activity could be restored");
                return false;
            }
            RideItem *item = linkedItem.data();
            if (!item
                || !guardedCache->rides_.contains(item)) {
                error = QObject::tr(
                    "The linked activity disappeared before it could be restored");
                return false;
            }
            item->setLinkedFileName(
                linkedFileName);
            item = linkedItem.data();
            if (!operationOwnersAreStable()
                || !item
                || !guardedCache->rides_.contains(item)
                || !linkedIdentityMatches()
                || !deletionTargetIsStable()) {
                error = QObject::tr(
                    "The linked activity disappeared while restoring its link");
                return false;
            }
            if (item->getLinkedFileName()
                    != linkedFileName) {
                error = QObject::tr(
                    "The linked activity metadata could not be restored");
                return false;
            }
            QString saveError;
            if (!guardedCache->saveActivity(
                    item, saveError)) {
                error = saveError.isEmpty()
                    ? QObject::tr(
                        "The linked activity could not be restored")
                    : saveError;
                return false;
            }
            QString identityError;
            if (!acceptLinkedIdentity(
                    identityError)) {
                error = identityError;
                return false;
            }
            item = linkedItem.data();
            if (!operationOwnersAreStable()
                || !item
                || !guardedCache->rides_.contains(item)
                || !deletionTargetIsStable()) {
                error = QObject::tr(
                    "The linked activity disappeared while saving its restored link");
                return false;
            }
            if (!linkedIdentityMatches()) {
                error = QObject::tr(
                    "The linked activity identity changed and its original reciprocal link could not be restored safely");
                return false;
            }
            if (item->getLinkedFileName()
                    != linkedFileName) {
                error = QObject::tr(
                    "The linked activity link changed while it was being restored");
                return false;
            }
            return true;
        };
    const auto requireLinkedRecovery =
        [&result, &appendLinkedRecoveryPaths](
            const QString &detail) {
            result.status =
                RideCache::RemovalStatus::RecoveryRequired;
            appendRemovalResultError(result, detail);
            appendLinkedRecoveryPaths();
        };

    const OperationPreCheck linkCheck =
        guardedCache->checkRemovalLinks(todelete);
    if (!deletionTargetIsStable()
        || !sidecarOwnershipIsStable()) {
        result.error = QObject::tr(
            "The activity cache changed while checking linked activities");
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    if (!linkCheck.canProceed) {
        result.error =
            linkCheck.blockingReason;
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
    if (todelete->hasLinkedActivity()) {
        linkedItem =
            linkCheck.affectedItems.value(1);
        if (!linkedItem
            || !guardedCache->rides_.contains(
                linkedItem.data())) {
            result.error = tr(
                "The linked activity changed before deletion");
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        linkedOriginalFileName =
            linkedItem->fileName;
        linkedOriginalPath = linkedItem->path;
        linkedOriginalPlanned = linkedItem->planned;
        linkedOriginalSourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                linkedOriginalPath,
                linkedOriginalFileName);
        linkedExpectedFileName =
            linkedOriginalFileName;
        linkedExpectedPath = linkedOriginalPath;
        linkedExpectedPlanned =
            linkedOriginalPlanned;
        QString linkedIdentityError;
        if (!acceptLinkedIdentity(linkedIdentityError)) {
            result.error = linkedIdentityError.isEmpty()
                ? tr("The linked activity identity is unsafe")
                : linkedIdentityError;
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        if (linkedItem->isDirty()) {
            result.error = tr(
                "The linked activity has unsaved changes that must be saved or discarded first");
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        linkedFileName =
            linkedItem->getLinkedFileName();
        linkedItem->clearLinkedFileName();
        if (!operationOwnersAreStable()
            || !linkedItem
            || !guardedCache->rides_.contains(
                linkedItem.data())
            || !linkedIdentityMatches()) {
            requireLinkedRecovery(tr(
                "The linked activity disappeared while its link was being updated"));
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        linkedIdentityChanged = false;
        if (!deletionTargetIsStable()
            || !sidecarOwnershipIsStable()) {
            QString restoreError;
            restoreLinkedPeer(restoreError);
            requireLinkedRecovery(tr(
                "The deletion target changed while its linked activity was being updated"));
            appendRemovalResultError(
                result, restoreError);
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        if (!linkedItem->getLinkedFileName().isEmpty()) {
            result.error = tr(
                "The linked activity could not be loaded for update");
            QString restoreError;
            const bool linkRestored =
                restoreLinkedPeer(restoreError);
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged) {
                requireLinkedRecovery(tr(
                    "The linked activity could not be restored after its reciprocal link changed"));
                appendRemovalResultError(
                    result, restoreError);
            }
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        QString linkError;
        if (!guardedCache->saveActivity(
                linkedItem.data(), linkError)) {
            result.error = linkError.isEmpty()
                ? tr("The linked activity could not be updated")
                : linkError;
            QString linkRollbackError;
            const bool linkRestored =
                restoreLinkedPeer(
                    linkRollbackError);
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged) {
                requireLinkedRecovery(
                    linkRollbackError.isEmpty()
                    ? tr("The linked activity could not be restored")
                    : linkRollbackError);
            }
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        linkedItemWasSaved = true;
        linkedIdentityError.clear();
        const bool linkedIdentityAccepted =
            acceptLinkedIdentity(
                linkedIdentityError);
        if (!linkedIdentityAccepted
            || !operationOwnersAreStable()
            || !linkedItem
            || !guardedCache->rides_.contains(
                linkedItem.data())
            || !linkedIdentityMatches()
            || !linkedItem->getLinkedFileName()
                    .isEmpty()) {
            requireLinkedRecovery(
                linkedIdentityError.isEmpty()
                ? tr(
                    "The linked activity changed while its update was being saved")
                : linkedIdentityError);
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        if (!deletionTargetIsStable()
            || !sidecarOwnershipIsStable()) {
            QString restoreError;
            restoreLinkedPeer(restoreError);
            requireLinkedRecovery(tr(
                "The deletion target changed while its linked activity was being saved"));
            appendRemovalResultError(
                result, restoreError);
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
    }

    const QString originalPeerReference =
        linkedOriginalFileName;
    const QString expectedPeerReference =
        linkedExpectedFileName;
    const auto linkedStateIsStable =
        [guardedCache, operationOwnersAreStable,
         linkedIdentityMatches,
         linkedItem, linkedItemWasSaved,
         deletionItem, filenameToDelete,
         originalPeerReference,
         expectedPeerReference] {
            if (!linkedItemWasSaved) return true;
            if (!operationOwnersAreStable()
                || !linkedIdentityMatches()) {
                return false;
            }
            RideItem *const peer = linkedItem.data();
            if (!peer
                || !guardedCache->rides_.contains(peer)
                || !peer->getLinkedFileName()
                        .isEmpty()) {
                return false;
            }
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (!candidate
                    || candidate
                        == deletionItem.data()
                    || candidate == peer) {
                    continue;
                }
                const QString linkedFileName =
                    candidate->getLinkedFileName();
                if (linkedFileName == filenameToDelete
                    || (!originalPeerReference.isEmpty()
                        && linkedFileName
                            == originalPeerReference)
                    || (!expectedPeerReference.isEmpty()
                        && linkedFileName
                            == expectedPeerReference)) {
                    return false;
                }
            }
            return true;
        };
    const RemovalPrepareFunction prepareRemoval =
        [deletionItem,
         operationOwnersAreStable,
         deletionTargetIsStable,
         linkedStateIsStable,
         sidecarOwnershipIsStable](
            QString &prepareError) {
            const auto stateIsStable = [&] {
                return operationOwnersAreStable()
                    && deletionTargetIsStable()
                    && linkedStateIsStable()
                    && sidecarOwnershipIsStable();
            };
            if (!stateIsStable()) {
                prepareError = QObject::tr(
                    "The activity cache changed during deletion");
                return false;
            }
            RideItem *const item =
                deletionItem.data();
            RideFile *const deletionRide =
                item ? item->ride() : nullptr;
            RideItem *const loadedItem =
                deletionItem.data();
            if (!stateIsStable()
                || !loadedItem
                || loadedItem->ride(false)
                    != deletionRide) {
                prepareError = QObject::tr(
                    "The activity cache changed while loading the deletion target");
                return false;
            }
            if (!runRemovalDeleteProcessor(
                    deletionRide,
                    prepareError)) {
                return false;
            }
            RideItem *const processedItem =
                deletionItem.data();
            if (!stateIsStable()
                || !processedItem
                || processedItem->ride(false)
                    != deletionRide) {
                prepareError = QObject::tr(
                    "The activity cache changed during deletion");
                return false;
            }
            return true;
        };

    const StorageRemovalResult storage =
        removeRideFilesFromStorage(
            sourcePath,
            backupPath,
            derivedPaths,
            archiveSource,
            prepareRemoval);
    if (!storage.committed()) {
        result.status = storage.status;
        result.error = storage.error.isEmpty()
            ? tr("The activity files could not be removed")
            : storage.error;
        result.recoveryPaths =
            storage.recoveryPaths;

        if (linkedItemWasSaved) {
            QString linkRollbackError;
            const bool linkRestored =
                restoreLinkedPeer(
                    linkRollbackError);
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged) {
                requireLinkedRecovery(
                    linkRollbackError.isEmpty()
                    ? tr("The linked activity could not be restored")
                    : linkRollbackError);
            }
        }
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            result.status,
            result.error,
            result.recoveryPaths);
        qWarning().noquote()
            << filenameToDelete << result.error;
        return result;
    }
    if (!operationOwnersAreStable()) {
        result.status = RemovalStatus::RecoveryRequired;
        result.error = QObject::tr(
            "The activity collection disappeared after its files were archived");
        result.recoveryPaths = storage.recoveryPaths;
        if (!backupPath.isEmpty()
            && !result.recoveryPaths.contains(backupPath)) {
            result.recoveryPaths.append(backupPath);
        }
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            result.status,
            result.error,
            result.recoveryPaths);
        return result;
    }
    todelete = deletionItem.data();
    index = todelete
        ? guardedCache->rides_.indexOf(todelete)
        : -1;
    if (index < 0 || !deletionTargetIsStable()) {
        result.status =
            RemovalStatus::RecoveryRequired;
        result.error = tr(
            "The activity cache changed after its files were archived");
        result.recoveryPaths =
            storage.recoveryPaths;
        if (!backupPath.isEmpty()
            && !result.recoveryPaths.contains(
                backupPath)) {
            result.recoveryPaths.append(
                backupPath);
        }
        appendRemovalItemResult(
            result, filenameToDelete,
            plannedToDelete,
            result.status,
            result.error,
            result.recoveryPaths);
        qWarning().noquote()
            << filenameToDelete << result.error;
        return result;
    }

    QPointer<RideItem> previousSelection;
    RideItem *const previousSelectionCandidate =
        guardedContext->ride;
    if (previousSelectionCandidate
        && guardedCache->rides_.contains(
            previousSelectionCandidate)) {
        previousSelection = previousSelectionCandidate;
    }
    QPointer<RideItem> nextSelection;
    if (previousSelection == todelete) {
        if (index + 1 < guardedCache->rides_.size())
            nextSelection = guardedCache->rides_.at(index + 1);
        else if (index > 0)
            nextSelection = guardedCache->rides_.at(index - 1);
    } else if (previousSelection
               && guardedCache->rides_.contains(
                   previousSelection.data())) {
        nextSelection = previousSelection;
    }

    result.status = storage.status;
    result.error = storage.error;
    result.recoveryPaths =
        storage.recoveryPaths;
    result.affectedCount = 1;
    appendRemovalItemResult(
        result, filenameToDelete,
        plannedToDelete,
        result.status,
        result.error,
        result.recoveryPaths);

    RideItem *const signalSelection =
        nextSelection.data();
    QMetaObject::Connection selectionDestroyedConnection;
    if (signalSelection) {
        selectionDestroyedConnection = QObject::connect(
            signalSelection,
            &QObject::destroyed,
            guardedContext.data(),
            [guardedContext, signalSelection] {
                if (guardedContext
                    && guardedContext->ride
                        == signalSelection) {
                    guardedContext->ride = nullptr;
                }
            },
            Qt::DirectConnection);
    }
    guardedContext->ride = signalSelection;
    guardedModel->startRemove(index);
    if (!guardedCache || !guardedModel
        || guardedCache->model_
            != guardedModel.data()) {
        return result;
    }
    const int reentrantIndex =
        guardedCache->rides_.indexOf(todelete);
    if (reentrantIndex >= 0) {
        int removalIndex = reentrantIndex;
        if (reentrantIndex != index
            && index >= 0
            && index < guardedCache->rides_.size()) {
            guardedCache->rides_.move(
                reentrantIndex, index);
            removalIndex = index;
        }
        guardedCache->rides_.remove(
            removalIndex, 1);
    }
    if (deletionItem)
        guardedCache->delete_ << todelete;
    guardedModel->endRemove(index);
    if (!guardedCache || !guardedModel
        || guardedCache->model_
            != guardedModel.data()) {
        return result;
    }
    QObject::disconnect(selectionDestroyedConnection);
    if (!deletionItem)
        guardedCache->delete_.removeOne(todelete);

    RideItem *stableSelection =
        nextSelection.data();
    if (stableSelection
        && !guardedCache->rides_.contains(
            stableSelection)) {
        stableSelection = nullptr;
    }
    if (!operationOwnersAreStable()) return result;
    guardedContext->ride = stableSelection;
    const QPointer<MainWindow> guardedMainWindow(
        guardedContext->mainWindow);
    if (guardedMainWindow)
        guardedMainWindow->setUpdatesEnabled(false);
    if (!operationOwnersAreStable()) {
        if (guardedMainWindow)
            guardedMainWindow->setUpdatesEnabled(true);
        return result;
    }
    RideItem *stableDeletedItem =
        deletionItem.data();
    if (stableDeletedItem)
        guardedContext->notifyRideDeleted(
            stableDeletedItem);
    if (!operationOwnersAreStable()) {
        if (guardedMainWindow)
            guardedMainWindow->setUpdatesEnabled(true);
        return result;
    }
    if (!deletionItem)
        guardedCache->delete_.removeOne(todelete);
    stableSelection = nextSelection.data();
    if (stableSelection
        && !guardedCache->rides_.contains(
            stableSelection)) {
        stableSelection = nullptr;
    }
    guardedContext->notifyRideSelected(
        stableSelection);
    if (guardedMainWindow)
        guardedMainWindow->setUpdatesEnabled(true);
    if (!operationOwnersAreStable()) return result;

    if (triggerRefresh) {
        guardedCache->refresh();
        if (!operationOwnersAreStable()) return result;
        // model estimates (lazy refresh)
        guardedEstimator->refresh();
    }

    if (!result.cleanlyCompleted()) {
        if (result.error.isEmpty()) {
            result.error = tr(
                "The activity was removed with a recovery warning");
        }
        qWarning().noquote()
            << filenameToDelete << result.error;
    }
    return result;

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
