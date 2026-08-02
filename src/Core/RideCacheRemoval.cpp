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
#include "RideCacheMutationScope.h"
#include "RideFileCacheIntegrity.h"
#include "LinkedActivityRemovalJournal.h"
#include "PlanReplacementJournal.h"
#include "PlannedActivityFileStager.h"

#include <QCoreApplication>
#include <QEvent>
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
void rideCacheRemovalTransitionReached(
    const char *transition);
bool rideCacheRemovalLinkedSaveKeepsPath(
    RideItem *item);
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
        release();
    }

    bool acquired() const { return acquired_; }

    void release()
    {
        if (!acquired_) return;
        *inProgress_ = false;
        acquired_ = false;
    }

private:
    std::shared_ptr<bool> inProgress_;
    bool acquired_ = false;
};

class RemovalScopeExit final
{
public:
    explicit RemovalScopeExit(std::function<void()> action)
        : action_(std::move(action))
    {
    }

    ~RemovalScopeExit()
    {
        if (action_) action_();
    }

    RemovalScopeExit(const RemovalScopeExit &) = delete;
    RemovalScopeExit &operator=(const RemovalScopeExit &) = delete;

private:
    std::function<void()> action_;
};

class ScopedRideFiles final
{
public:
    ~ScopedRideFiles()
    {
        for (const QPointer<RideFile> &ride :
             std::as_const(files_)) {
            if (ride) delete ride.data();
        }
    }

    void append(RideFile *ride)
    {
        files_.append(QPointer<RideFile>(ride));
    }

    const QVector<QPointer<RideFile>> &files() const
    {
        return files_;
    }

private:
    QVector<QPointer<RideFile>> files_;
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

bool copyRemovalSourceToPath(
    const RemovalFileSnapshot &source,
    const QString &stagingPath,
    QString &error)
{
    if (stagingPath.isEmpty()
        || removalEntryExists(stagingPath)) {
        error = QObject::tr(
            "The activity backup staging path is unavailable");
        return false;
    }

    QFile input(source.path);
    if (!input.open(QIODevice::ReadOnly)) {
        error = QObject::tr(
            "Cannot read the activity source: %1")
                    .arg(input.errorString());
        return false;
    }

    NewAtomicFileWriter output(stagingPath);
    if (!output.open()) {
        error = QObject::tr(
            "Cannot create the activity backup staging file: %1")
                    .arg(output.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty()
            && input.error()
                != QFileDevice::NoError) {
            error = QObject::tr(
                "Cannot read the activity source: %1")
                        .arg(input.errorString());
            output.cancelWriting();
            return false;
        }
        if (output.write(chunk) != chunk.size()) {
            error = QObject::tr(
                "Cannot write the activity backup staging file: %1")
                        .arg(output.errorString());
            output.cancelWriting();
            return false;
        }
        copied += chunk.size();
        hash.addData(chunk);
    }
    if (!output.flush()) {
        error = QObject::tr(
            "Cannot flush the activity backup staging file: %1")
                    .arg(output.errorString());
        output.cancelWriting();
        return false;
    }
    if (copied != source.contents.size
        || hash.result() != source.contents.digest) {
        error = QObject::tr(
            "The copied activity backup does not match the source");
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        error = QObject::tr(
            "Cannot publish the activity backup staging file: %1")
                    .arg(output.errorString());
        return false;
    }
    if (!syncRemovalDirectory(stagingPath, error))
        return false;

    QString verifyError;
    if (!atomicFileMatchesSnapshot(
            stagingPath, source.contents,
            verifyError)
        || !atomicFileMatchesSnapshot(
            source.path, source.contents,
            verifyError)) {
        error = QObject::tr(
            "The activity source changed while it was being copied: %1")
                    .arg(verifyError);
        return false;
    }
    return true;
}

bool removeRemovalFile(
    const QString &path,
    const QString &hookPath,
    QString &error)
{
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (rideCacheRemovalShouldFailCleanup(
            hookPath)) {
        error = QObject::tr(
            "Injected activity removal cleanup failure");
        return false;
    }
#else
    Q_UNUSED(hookPath)
#endif
    return removeFileDurably(
        path, error, syncRemovalDirectory);
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
        RemovalPrepareFunction prepare,
        std::shared_ptr<
            LinkedActivityRemoval::Journal> journal = {})
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
          journal_(std::move(journal)),
          transactionId_(
              journal_
              ? journal_->transactionId()
              : QUuid::createUuid().toString(
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
            sourceTombstone_ = journal_
                ? journal_->sourceTombstonePath()
                : source_.path
                    + QStringLiteral(".gc-remove-")
                    + transactionId_;
            previousBackup_ = journal_
                ? journal_->previousBackupPath()
                : backup_.path
                    + QStringLiteral(".gc-previous-")
                    + transactionId_;
            if (journal_) {
                backupStaging_ =
                    journal_->backupStagingPath();
            }
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

        const bool sourceCopied = journal_
            ? copyRemovalSourceToPath(
                  source_, backupStaging_,
                  result.error)
            : copyRemovalSource(
                  source_, backup_.path,
                  backupStaging_, result.error);
        if (!sourceCopied) {
            cleanupUnpublishedStaging(result);
            return result;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "backup-staged");
#endif

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
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
            rideCacheRemovalTransitionReached(
                "previous-backup-preserved");
#endif
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
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "backup-published");
#endif

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
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "source-tombstoned");
#endif

        if (journal_) {
            QString commitError;
            if (!journal_->markCommitted(
                    commitError)) {
                result.error = commitError.isEmpty()
                    ? QObject::tr(
                        "Cannot commit the linked activity deletion journal")
                    : commitError;
                if (journal_->hasCommitMarker()) {
                    result.status = RideCache::RemovalStatus::
                        CommittedCleanupPending;
                    result.recoveryPaths =
                        journal_->recoveryPaths();
                    return result;
                }
                return rollbackResult(result.error);
            }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
            rideCacheRemovalTransitionReached(
                "commit-marker");
#endif
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
        QString removalError;
        if (!removeRemovalFile(
                actualPath, hookPath,
                removalError)) {
            appendRemovalError(
                result.error,
                QObject::tr(
                    "Cannot remove an activity cleanup file: %1 (%2)")
                        .arg(actualPath, removalError));
            addRecoveryPath(
                result.recoveryPaths,
                actualPath);
            return false;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "cleanup-file");
#endif
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
        QString removalError;
        if (!removeFileDurably(
                backupStaging_, removalError,
                syncRemovalDirectory)) {
            result.status = RideCache::RemovalStatus::RecoveryRequired;
            appendRemovalError(
                result.error,
                QObject::tr(
                    "Cannot remove the unpublished activity backup: %1 (%2)")
                    .arg(backupStaging_, removalError));
            addRecoveryPath(
                result.recoveryPaths,
                backupStaging_);
            return;
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
        QString removalError;
        if (!removeFileDurably(
                path, removalError,
                syncRemovalDirectory)) {
            appendRemovalError(
                error,
                QObject::tr(
                    "Cannot remove the %1 during rollback: %2 (%3)")
                    .arg(description, path, removalError));
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
    std::shared_ptr<
        LinkedActivityRemoval::Journal> journal_;
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
    RemovalPrepareFunction prepare,
    std::shared_ptr<
        LinkedActivityRemoval::Journal> journal = {})
{
    return ActivityRemovalTransaction(
        sourcePath,
        backupPath,
        derivedPaths,
        archiveSource,
        std::move(prepare),
        std::move(journal)).execute();
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

bool linkedRemovalPeerSaveKeepsPath(
    RideItem *item,
    QString &error)
{
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (rideCacheRemovalLinkedSaveKeepsPath(item))
        return true;
#else
    if (item) {
        RideFile *const ride = item->ride();
        const QFileInfo source(
            QDir(item->path).filePath(item->fileName));
        if (ride
            && source.completeSuffix().compare(
                   QStringLiteral("json"),
                   Qt::CaseInsensitive) == 0) {
            const QDateTime dateTime = ride->startTime();
            const QChar zero = QLatin1Char('0');
            const QString expectedBaseName =
                QStringLiteral("%1_%2_%3_%4_%5_%6")
                    .arg(dateTime.date().year(), 4, 10, zero)
                    .arg(dateTime.date().month(), 2, 10, zero)
                    .arg(dateTime.date().day(), 2, 10, zero)
                    .arg(dateTime.time().hour(), 2, 10, zero)
                    .arg(dateTime.time().minute(), 2, 10, zero)
                    .arg(dateTime.time().second(), 2, 10, zero);
            if (source.baseName() == expectedBaseName)
                return true;
        }
    }
#endif
    error = QObject::tr(
        "The linked activity must be saved as a correctly named JSON activity before deletion");
    return false;
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
        if (!ownsLiveRide(ride) || ride == rideToDelete)
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
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
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
        if (!ride || deletelist.contains(ride)
            || !rides_.contains(ride)) {
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
    guardedEstimator->stop();
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
        if (!guardedCache->ownsLiveRide(ride)
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
                false,
                true);
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
    const bool resumeBackgroundWork =
        result.affectedCount > 0
        && (triggerRefresh
            || (guardedCache
                && guardedCache->removalRefreshPending_));
    if (resumeBackgroundWork
        && operationOwnersAreStable()) {
        guardedCache->removalRefreshPending_ = false;
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

RideCache::PlannedReplacementResult
RideCache::replacePlannedActivities(
    const QList<RideItem*> &activitiesToReplace,
    const QList<std::pair<RideItem*, QDate>>
        &sourceItemsAndTargets)
{
    QList<PlannedActivityCopyRequest> copies;
    copies.reserve(sourceItemsAndTargets.size());
    for (const auto &sourceAndTarget : sourceItemsAndTargets) {
        copies.append({
            sourceAndTarget.first,
            sourceAndTarget.second,
            QTime()});
    }
    return replacePlannedActivityCopies(
        activitiesToReplace, copies);
}

RideCache::PlannedReplacementResult
RideCache::replacePlannedActivityCopies(
    const QList<RideItem*> &activitiesToReplace,
    const QList<PlannedActivityCopyRequest> &copies)
{
    PlannedReplacementResult result;
    if (QThread::currentThread() != thread()) {
        result.error = tr(
            "Planned activities can only be replaced on the cache thread");
        return result;
    }
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (copies.isEmpty()) {
        result.error = tr(
            "A planned activity replacement requires source activities");
        return result;
    }

    const QPointer<RideCache> guardedCache(this);
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    if (!guardedCache || !guardedContext || !guardedAthlete
        || guardedCache->context != guardedContext.data()
        || guardedContext->athlete != guardedAthlete.data()
        || guardedAthlete->rideCache != guardedCache.data()) {
        result.error = tr(
            "The planned activity collection is no longer available");
        return result;
    }

    const QString plannedRoot =
        guardedAthlete->home->planned().absolutePath();
    const QString canonicalPlannedRoot =
        guardedAthlete->home->planned().canonicalPath();
    if (canonicalPlannedRoot.isEmpty()) {
        result.error = tr(
            "The planned activity directory is unavailable");
        return result;
    }

    QStringList inputPaths;
    QSet<QString> inputPathKeys;
    QSet<QString> targetNames;
    QList<PlannedActivityTarget> targets;
    targets.reserve(copies.size());

    for (const PlannedActivityCopyRequest &copy : copies) {
        RideItem *const source = copy.source;
        if (!guardedCache->ownsLiveRide(source)
            || !source->planned || source->fileName.isEmpty()
            || !source->dateTime.isValid()
            || !copy.targetDate.isValid()) {
            result.error = tr(
                "A planned activity copy source or target is invalid");
            return result;
        }

        const QString sourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                plannedRoot, source->fileName);
        const QString itemSourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                source->path, source->fileName);
        const QString canonicalItemRoot =
            QDir(source->path).canonicalPath();
        if (sourcePath.isEmpty() || itemSourcePath.isEmpty()
            || canonicalItemRoot.isEmpty()
            || atomicFilePathKey(canonicalItemRoot)
                != atomicFilePathKey(canonicalPlannedRoot)
            || atomicFilePathKey(sourcePath)
                != atomicFilePathKey(itemSourcePath)) {
            result.error = tr(
                "A planned activity copy source does not match its cache namespace: %1")
                               .arg(source->fileName);
            return result;
        }

        int identityMatches = 0;
        for (RideItem *candidate :
             std::as_const(guardedCache->rides_)) {
            if (!guardedCache->ownsLiveRide(candidate)
                || !candidate->planned
                || candidate->fileName != source->fileName) {
                continue;
            }
            const QString candidatePath =
                RideFileCacheIntegrity::activitySourcePath(
                    candidate->path, candidate->fileName);
            if (!candidatePath.isEmpty()
                && atomicFilePathKey(candidatePath)
                    == atomicFilePathKey(sourcePath)) {
                ++identityMatches;
            }
        }
        if (identityMatches != 1) {
            result.error = tr(
                "A planned activity copy source identity is not unique: %1")
                               .arg(source->fileName);
            return result;
        }

        PlannedActivityFile::CopyTarget copyTarget;
        QString targetError;
        if (!PlannedActivityFile::resolveCopyTarget(
                source->fileName, source->dateTime,
                copy.targetDate, copy.targetTime,
                copyTarget,
                targetError)) {
            result.error = targetError;
            return result;
        }
        if (targetNames.contains(copyTarget.fileName)) {
            result.error = tr(
                "A planned activity copy target is duplicated: %1")
                               .arg(copyTarget.fileName);
            return result;
        }
        targetNames.insert(copyTarget.fileName);

        const QString inputKey =
            atomicFilePathKey(sourcePath);
        if (!inputPathKeys.contains(inputKey)) {
            inputPathKeys.insert(inputKey);
            inputPaths.append(sourcePath);
        }

        const QPointer<RideItem> guardedSource(source);
        const QString sourceFileName = source->fileName;
        const QString sourceItemPath = source->path;
        const QDateTime sourceDateTime = source->dateTime;
        const QDateTime targetDateTime = copyTarget.dateTime;
        targets.append({
            copyTarget.fileName,
            [guardedCache, guardedContext, guardedAthlete,
             guardedSource, sourcePath, sourceFileName,
             sourceItemPath, sourceDateTime, targetDateTime]
            (const QString &stagingPath, QString &error) {
                const auto sourceIsStable = [&] {
                    return guardedCache && guardedContext
                        && guardedAthlete && guardedSource
                        && guardedCache->context
                            == guardedContext.data()
                        && guardedContext->athlete
                            == guardedAthlete.data()
                        && guardedAthlete->rideCache
                            == guardedCache.data()
                        && guardedCache->ownsLiveRide(
                            guardedSource.data())
                        && guardedSource->planned
                        && guardedSource->fileName
                            == sourceFileName
                        && guardedSource->path
                            == sourceItemPath
                        && guardedSource->dateTime
                            == sourceDateTime;
                };
                if (!sourceIsStable()) {
                    error = QObject::tr(
                        "A planned activity copy source changed before staging");
                    return false;
                }
                if (!guardedCache->stagePlannedActivityCopy(
                        sourcePath, sourceFileName,
                        targetDateTime, stagingPath,
                        error)) {
                    return false;
                }
                if (!sourceIsStable()) {
                    error = QObject::tr(
                        "A planned activity copy source changed during staging");
                    return false;
                }
                return true;
            }});
    }

    return replacePlannedActivityFiles(
        activitiesToReplace, inputPaths, targets);
}

RideCache::PlannedReplacementResult
RideCache::replacePlannedActivityFiles(
    const QList<RideItem*> &activitiesToReplace,
    const QStringList &inputPaths,
    const QList<PlannedActivityTarget> &targets,
    bool notifyAdded,
    const PlannedReplacementCoordinator &coordinator)
{
    PlannedReplacementResult result;
    if (QThread::currentThread() != thread()) {
        result.error = tr(
            "Planned activities can only be replaced on the cache thread");
        return result;
    }
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    RemovalOperationGuard replacementRefreshGuard(
        replacementRefreshBlocked_);
    if (!replacementRefreshGuard.acquired()) {
        result.error = tr(
            "Another planned activity refresh is already blocked");
        return result;
    }
    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
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
            && guardedCache->context == guardedContext.data()
            && guardedContext->athlete == guardedAthlete.data()
            && guardedAthlete->context == guardedContext.data()
            && guardedAthlete->rideCache == guardedCache.data()
            && guardedCache->model_ == guardedModel.data()
            && guardedCache->estimator == guardedEstimator.data();
    };
    if (!operationOwnersAreStable()) {
        result.error = tr(
            "The planned activity collection is no longer available");
        return result;
    }
    if (targets.isEmpty()) {
        result.error = tr(
            "A planned activity replacement requires target activities");
        return result;
    }
    if (bool(coordinator.commit) != bool(coordinator.complete)) {
        result.error = tr(
            "A planned activity replacement coordinator is incomplete");
        return result;
    }

    if (!operationOwnersAreStable()) {
        result.error = tr(
            "The planned activity collection disappeared while its mutation was being reserved");
        return result;
    }

    const QString athleteRoot =
        guardedAthlete->home->root().absolutePath();
    const QString plannedRoot =
        guardedAthlete->home->planned().absolutePath();
    const QString canonicalPlannedRoot =
        guardedAthlete->home->planned().canonicalPath();
    if (canonicalPlannedRoot.isEmpty()) {
        result.error = tr(
            "The planned activity directory is unavailable");
        return result;
    }

    struct PendingReplacement
    {
        RideItem *address = nullptr;
        QPointer<RideItem> item;
        QString fileName;
        QString path;
        QString sourcePath;
        QStringList derivedPaths;
        QSet<QString> existingDerivedPathKeys;
    };
    QList<PendingReplacement> pending;
    pending.reserve(activitiesToReplace.size());
    QSet<RideItem*> requestedItems;
    QSet<QString> removalPathKeys;
    QStringList removalPaths;
    for (RideItem *item : activitiesToReplace) {
        if (!guardedCache->ownsLiveRide(item)) {
            result.error = tr(
                "A planned activity replacement target is no longer in the cache");
            return result;
        }
        if (requestedItems.contains(item)) continue;
        requestedItems.insert(item);
        if (!item->planned || item->fileName.isEmpty()) {
            result.error = tr(
                "Only named planned activities can be replaced");
            return result;
        }
        if (item->isdirty) {
            result.error = tr(
                "A modified planned activity must be saved or reverted before replacement: %1")
                               .arg(item->fileName);
            return result;
        }
        const QString sourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                plannedRoot, item->fileName);
        const QString itemSourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                item->path, item->fileName);
        const QString canonicalItemRoot =
            QDir(item->path).canonicalPath();
        if (sourcePath.isEmpty() || itemSourcePath.isEmpty()
            || canonicalItemRoot.isEmpty()
            || atomicFilePathKey(canonicalItemRoot)
                != atomicFilePathKey(canonicalPlannedRoot)
            || atomicFilePathKey(sourcePath)
                != atomicFilePathKey(itemSourcePath)) {
            result.error = tr(
                "A planned activity source does not match its cache namespace: %1")
                               .arg(item->fileName);
            return result;
        }
        const QString sourceKey = atomicFilePathKey(sourcePath);
        if (removalPathKeys.contains(sourceKey)) {
            result.error = tr(
                "A planned activity source identity is duplicated: %1")
                               .arg(item->fileName);
            return result;
        }
        int identityMatches = 0;
        for (RideItem *candidate : std::as_const(guardedCache->rides_)) {
            if (!guardedCache->ownsLiveRide(candidate)
                || !candidate->planned
                || candidate->fileName != item->fileName) {
                continue;
            }
            const QString candidatePath =
                RideFileCacheIntegrity::activitySourcePath(
                    candidate->path, candidate->fileName);
            if (!candidatePath.isEmpty()
                && atomicFilePathKey(candidatePath) == sourceKey) {
                ++identityMatches;
            }
        }
        if (identityMatches != 1) {
            result.error = tr(
                "A planned activity source identity is not unique: %1")
                               .arg(item->fileName);
            return result;
        }
        if (item->hasLinkedActivity()) {
            result.error = tr(
                "A linked planned activity must be unlinked before plan replacement: %1")
                               .arg(item->fileName);
            return result;
        }
        for (RideItem *candidate : std::as_const(guardedCache->rides_)) {
            if (guardedCache->ownsLiveRide(candidate)
                && candidate != item
                && candidate->getLinkedFileName() == item->fileName) {
                result.error = tr(
                    "A planned activity referenced by another activity cannot be replaced: %1")
                                   .arg(item->fileName);
                return result;
            }
        }
        removalPathKeys.insert(sourceKey);
        removalPaths.append(sourcePath);
        const QStringList derivedPaths =
            guardedCache->derivedFilePathsForRemoval(item);
        QSet<QString> existingDerivedPathKeys;
        for (const QString &derivedPath : derivedPaths) {
            const QFileInfo derivedInfo(derivedPath);
            if (derivedInfo.exists() || derivedInfo.isSymLink()) {
                existingDerivedPathKeys.insert(
                    atomicFilePathKey(derivedPath));
            }
        }
        pending.append({
            item, QPointer<RideItem>(item), item->fileName,
            item->path, sourcePath, derivedPaths,
            existingDerivedPathKeys});
    }

    QStringList targetPaths;
    targetPaths.reserve(targets.size());
    QStringList replacementFileNames;
    replacementFileNames.reserve(targets.size());
    QList<std::function<bool(const QString &, QString &)>>
        targetStagers;
    targetStagers.reserve(targets.size() + pending.size());
    QStringList targetDescriptions;
    targetDescriptions.reserve(targets.size() + pending.size());
    QSet<QString> targetNames;
    QSet<QString> targetPathKeys;
    for (const PlannedActivityTarget &target : targets) {
        QDateTime dateTime;
        const QString targetPath =
            RideFileCacheIntegrity::activitySourcePath(
                plannedRoot, target.fileName);
        if (targetPath.isEmpty()
            || !RideFile::parseRideFileName(
                target.fileName, &dateTime)
            || !target.stage) {
            result.error = tr(
                "A planned activity replacement target is invalid: %1")
                               .arg(target.fileName);
            return result;
        }
        const QString targetKey = atomicFilePathKey(targetPath);
        if (targetNames.contains(target.fileName)
            || targetPathKeys.contains(targetKey)) {
            result.error = tr(
                "A planned activity replacement target is duplicated: %1")
                               .arg(target.fileName);
            return result;
        }
        for (RideItem *candidate : std::as_const(guardedCache->rides_)) {
            if (guardedCache->ownsLiveRide(candidate)
                && candidate->fileName == target.fileName
                && !requestedItems.contains(candidate)) {
                result.error = tr(
                    "A planned activity replacement target already exists in the cache: %1")
                                   .arg(target.fileName);
                return result;
            }
        }
        targetNames.insert(target.fileName);
        targetPathKeys.insert(targetKey);
        targetPaths.append(targetPath);
        replacementFileNames.append(target.fileName);
        targetStagers.append(target.stage);
        targetDescriptions.append(target.fileName);
    }
    const int replacementTargetCount =
        replacementFileNames.size();

    const QString backupRoot =
        guardedAthlete->home->fileBackup().absolutePath();
    QFileInfo backupRootInfo(backupRoot);
    const bool backupRootExisted =
        backupRootInfo.exists() || backupRootInfo.isSymLink();
    if (backupRootExisted
        && (backupRootInfo.isSymLink()
            || !backupRootInfo.isDir())) {
        result.error = tr(
            "The activity backup path is not a regular directory");
        return result;
    }
    if (!backupRootExisted) {
        QString createError;
        if (!createDirectoryDurably(
                backupRoot, createError,
                syncRemovalDirectory)) {
            result.error = createError.isEmpty()
                ? tr("Cannot create the activity backup directory")
                : createError;
            return result;
        }
    }
    backupRootInfo.refresh();
    const QString canonicalAthleteRoot =
        QDir(athleteRoot).canonicalPath();
    const QString canonicalBackupRoot =
        backupRootInfo.canonicalFilePath();
    const QString relativeBackupRoot =
        QDir(canonicalAthleteRoot).relativeFilePath(
            canonicalBackupRoot);
    if (backupRootInfo.isSymLink()
        || !backupRootInfo.isDir()
        || canonicalAthleteRoot.isEmpty()
        || canonicalBackupRoot.isEmpty()
        || QDir::isAbsolutePath(relativeBackupRoot)
        || relativeBackupRoot == QStringLiteral("..")
        || relativeBackupRoot.startsWith(
            QStringLiteral("../"))) {
        result.error = tr(
            "The activity backup path escapes the athlete directory");
        return result;
    }
    if (backupRootExisted) {
        QString backupRootSyncError;
        if (!syncRemovalDirectory(
                backupRoot, backupRootSyncError)) {
            result.error = backupRootSyncError;
            return result;
        }
    }

    const QString plannedBackupRoot =
        QDir(canonicalBackupRoot).filePath(
            QStringLiteral("planned"));
    QFileInfo plannedBackupInfo(plannedBackupRoot);
    const bool plannedBackupExisted =
        plannedBackupInfo.exists() || plannedBackupInfo.isSymLink();
    if (plannedBackupExisted
        && (plannedBackupInfo.isSymLink()
            || !plannedBackupInfo.isDir())) {
        result.error = tr(
            "The planned activity backup path is not a regular directory");
        return result;
    }
    if (!plannedBackupExisted) {
        QString createError;
        if (!createDirectoryDurably(
                plannedBackupRoot, createError,
                syncRemovalDirectory)) {
            result.error = createError.isEmpty()
                ? tr(
                    "Cannot create the planned activity backup directory")
                : createError;
            return result;
        }
    }
    plannedBackupInfo.refresh();
    if (plannedBackupInfo.isSymLink()
        || !plannedBackupInfo.isDir()) {
        result.error = tr(
            "The planned activity backup path is not a regular directory");
        return result;
    }
    if (plannedBackupExisted) {
        QString backupSyncError;
        if (!syncRemovalDirectory(
                plannedBackupRoot, backupSyncError)) {
            result.error = backupSyncError;
            return result;
        }
    }

    for (const PendingReplacement &entry : std::as_const(pending)) {
        const QString backupPath =
            RideFileCacheIntegrity::activitySourcePath(
                plannedBackupRoot,
                entry.fileName + QStringLiteral(".bak"));
        if (backupPath.isEmpty()) {
            result.error = tr(
                "A planned activity backup path is unsafe: %1")
                               .arg(entry.fileName);
            return result;
        }
        const QString backupKey = atomicFilePathKey(backupPath);
        if (targetPathKeys.contains(backupKey)) {
            result.error = tr(
                "A planned activity backup conflicts with a replacement target: %1")
                               .arg(entry.fileName);
            return result;
        }
        const QFileInfo backupInfo(backupPath);
        if (backupInfo.exists() || backupInfo.isSymLink()) {
            if (removalPathKeys.contains(backupKey)) {
                result.error = tr(
                    "A planned activity backup path is duplicated: %1")
                                   .arg(entry.fileName);
                return result;
            }
            removalPathKeys.insert(backupKey);
            removalPaths.append(backupPath);
        }
        targetPathKeys.insert(backupKey);
        targetPaths.append(backupPath);
        const QString sourcePath = entry.sourcePath;
        targetStagers.append(
            [sourcePath](const QString &stagingPath,
                         QString &error) {
                if (QFile::copy(sourcePath, stagingPath))
                    return true;
                error = QObject::tr(
                    "Cannot stage a planned activity backup: %1")
                                .arg(sourcePath);
                return false;
            });
        targetDescriptions.append(
            tr("backup for %1").arg(entry.fileName));
    }

    for (const PendingReplacement &entry : std::as_const(pending)) {
        RideItem *const item = entry.item.data();
        if (!item) {
            result.error = tr(
                "A planned activity disappeared before derived files were inspected");
            return result;
        }
        for (const QString &derivedPath : entry.derivedPaths) {
            const QFileInfo derivedInfo(derivedPath);
            if (!derivedInfo.exists() && !derivedInfo.isSymLink())
                continue;
            const QString derivedKey =
                atomicFilePathKey(derivedPath);
            if (removalPathKeys.contains(derivedKey))
                continue;
            if (targetPathKeys.contains(derivedKey)) {
                result.error = tr(
                    "A derived activity file conflicts with a plan replacement target: %1")
                                   .arg(derivedPath);
                return result;
            }
            removalPathKeys.insert(derivedKey);
            removalPaths.append(derivedPath);
        }
    }

    struct GuardedCacheEntry
    {
        RideItem *address = nullptr;
        QPointer<RideItem> item;
        bool replacementTarget = false;
    };
    QVector<GuardedCacheEntry> cacheSnapshot;
    cacheSnapshot.reserve(guardedCache->rides_.size());
    QSet<RideItem*> cacheSnapshotAddresses;
    for (RideItem *item : std::as_const(guardedCache->rides_)) {
        if (!guardedCache->ownsLiveRide(item)) continue;
        cacheSnapshot.append({
            item, QPointer<RideItem>(item),
            requestedItems.contains(item)});
        cacheSnapshotAddresses.insert(item);
    }
    const auto flushOriginalDeferredDeletes = [&] {
        for (int pass = 0; pass < 2; ++pass) {
            for (const GuardedCacheEntry &entry : cacheSnapshot) {
                if (entry.item) {
                    QCoreApplication::sendPostedEvents(
                        entry.item.data(),
                        QEvent::DeferredDelete);
                }
            }
        }
    };
    QSet<int> retiredCacheEntries;
    const auto cacheSnapshotIsStable = [&] {
        flushOriginalDeferredDeletes();
        if (!operationOwnersAreStable()
            || guardedCache->rides_.size()
                != cacheSnapshot.size()) {
            return false;
        }
        for (RideItem *address :
             std::as_const(guardedCache->rides_)) {
            if (!cacheSnapshotAddresses.contains(address))
                return false;
        }
        for (const GuardedCacheEntry &entry : cacheSnapshot) {
            if (!entry.item) return false;
        }
        return true;
    };
    struct CacheRepairResult
    {
        bool ownersStable = true;
        bool unexpectedDeletion = false;
    };
    const auto repairDestroyedCacheEntries =
        [&](bool modelResetActive) {
            CacheRepairResult repair;
            if (!operationOwnersAreStable()) {
                repair.ownersStable = false;
                return repair;
            }
            const auto hasDestroyedEntries = [&] {
                for (int index = 0;
                     index < cacheSnapshot.size(); ++index) {
                    if (!retiredCacheEntries.contains(index)
                        && !cacheSnapshot.at(index).item) {
                        return true;
                    }
                }
                return false;
            };
            const auto hasDestroyedModelEntries = [&] {
                for (int index = 0;
                     index < cacheSnapshot.size(); ++index) {
                    const GuardedCacheEntry &entry =
                        cacheSnapshot.at(index);
                    if (!retiredCacheEntries.contains(index)
                        && !entry.item
                        && guardedCache->rides_.contains(
                            entry.address)) {
                        return true;
                    }
                }
                return false;
            };
            const auto purgeDestroyedEntries = [&] {
                for (int index = 0;
                     index < cacheSnapshot.size(); ++index) {
                    if (retiredCacheEntries.contains(index))
                        continue;
                    const GuardedCacheEntry &entry =
                        cacheSnapshot.at(index);
                    if (entry.item) continue;
                    if (!entry.replacementTarget)
                        repair.unexpectedDeletion = true;
                    guardedCache->rides_.removeOne(entry.address);
                    guardedCache->delete_.removeOne(entry.address);
                    guardedCache->deletelist.remove(entry.address);
                    if (guardedContext->ride == entry.address)
                        guardedContext->ride = nullptr;
                    retiredCacheEntries.insert(index);
                }
            };

            if (modelResetActive) {
                purgeDestroyedEntries();
                repair.ownersStable =
                    operationOwnersAreStable();
                return repair;
            }

            while (hasDestroyedModelEntries()) {
                QPointer<RideItem> previousSelection;
                for (const GuardedCacheEntry &entry : cacheSnapshot) {
                    if (entry.address == guardedContext->ride
                        && entry.item) {
                        previousSelection = entry.item;
                        break;
                    }
                }
                guardedContext->ride = nullptr;
                QString resetError;
                if (!mutation.beginReset(resetError)) {
                    repair.ownersStable = false;
                    return repair;
                }
                flushOriginalDeferredDeletes();
                if (!operationOwnersAreStable()) {
                    mutation.endReset();
                    repair.ownersStable = false;
                    return repair;
                }
                purgeDestroyedEntries();
                mutation.endReset();
                flushOriginalDeferredDeletes();
                if (!operationOwnersAreStable()) {
                    repair.ownersStable = false;
                    return repair;
                }
                if (previousSelection
                    && guardedCache->ownsLiveRide(
                        previousSelection.data())) {
                    guardedContext->ride =
                        previousSelection.data();
                }
            }
            if (hasDestroyedEntries())
                purgeDestroyedEntries();
            return repair;
        };

    const auto removalPathKeySet = [](const QStringList &paths) {
        QSet<QString> keys;
        for (const QString &path : paths)
            keys.insert(atomicFilePathKey(path));
        return keys;
    };
    const auto existingPathKeySet = [](const QStringList &paths) {
        QSet<QString> keys;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            if (info.exists() || info.isSymLink())
                keys.insert(atomicFilePathKey(path));
        }
        return keys;
    };
    const auto pendingIsStable = [=](
        bool requireOriginalDerivedFiles) {
        if (!cacheSnapshotIsStable()) return false;
        for (const PendingReplacement &entry : pending) {
            RideItem *const item = entry.item.data();
            if (!guardedCache->ownsLiveRide(item)
                || !item->planned
                || item->isdirty
                || item->hasLinkedActivity()
                || item->fileName != entry.fileName
                || item->path != entry.path
                || removalPathKeySet(
                       guardedCache->derivedFilePathsForRemoval(item))
                    != removalPathKeySet(entry.derivedPaths)
                || (requireOriginalDerivedFiles
                    && existingPathKeySet(entry.derivedPaths)
                        != entry.existingDerivedPathKeys)) {
                return false;
            }
            int matches = 0;
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (guardedCache->ownsLiveRide(candidate)
                    && candidate != item
                    && candidate->getLinkedFileName()
                        == entry.fileName) {
                    return false;
                }
                if (!guardedCache->ownsLiveRide(candidate)
                    || !candidate->planned
                    || candidate->fileName != entry.fileName) {
                    continue;
                }
                const QString candidatePath =
                    RideFileCacheIntegrity::activitySourcePath(
                        candidate->path, candidate->fileName);
                if (!candidatePath.isEmpty()
                    && atomicFilePathKey(candidatePath)
                        == atomicFilePathKey(entry.sourcePath)) {
                    ++matches;
                }
            }
            if (matches != 1) return false;
        }
        for (RideItem *candidate :
             std::as_const(guardedCache->rides_)) {
            if (guardedCache->ownsLiveRide(candidate)
                && targetNames.contains(candidate->fileName)
                && !requestedItems.contains(candidate)) {
                return false;
            }
        }
        return true;
    };
    if (!pendingIsStable(true)) {
        result.error = tr(
            "The planned activity collection changed before replacement");
        return result;
    }

    PlanReplacement::Specification specification;
    specification.athleteRoot = athleteRoot;
    specification.scopeRoot = athleteRoot;
    specification.inputPaths = inputPaths;
    specification.removalPaths = removalPaths;
    specification.targetPaths = targetPaths;
    QString journalError;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            specification, journalError);
    if (!journal) {
        result.error = journalError.isEmpty()
            ? tr("Cannot prepare the plan replacement")
            : journalError;
        return result;
    }

    const auto rollback = [&](const QString &detail) {
        appendRemovalError(result.error, detail);
        const CacheRepairResult cacheRepair =
            repairDestroyedCacheEntries(false);
        if (!cacheRepair.ownersStable) {
            appendRemovalError(
                result.error,
                tr("The planned activity collection disappeared while repairing a failed replacement"));
        } else if (cacheRepair.unexpectedDeletion) {
            appendRemovalError(
                result.error,
                tr("A cached activity was destroyed during the failed replacement"));
        }
        bool committed = false;
        QString commitStateError;
        if (!journal->commitState(
                committed, commitStateError)) {
            appendRemovalError(
                result.error,
                commitStateError.isEmpty()
                    ? tr("The plan replacement commit state is unreadable; recovery is required")
                    : commitStateError);
            return;
        }
        if (committed) {
            result.committed = true;
            return;
        }
        QString cleanupError;
        if (!journal->cleanupAfterRollback(cleanupError)) {
            appendRemovalError(
                result.error,
                cleanupError.isEmpty()
                    ? tr("The incomplete plan replacement could not be cleaned up")
                    : cleanupError);
        }
    };

    for (int index = 0; index < targetStagers.size(); ++index) {
        QString stageError;
        bool staged = false;
        try {
            staged = targetStagers.at(index)(
                journal->stagingPath(index), stageError);
        } catch (const QString &detail) {
            stageError = detail;
        } catch (const std::exception &exception) {
            stageError = QString::fromLocal8Bit(exception.what());
        } catch (...) {
            stageError = tr(
                "A planned activity staging operation failed");
        }
        if (!staged) {
            rollback(stageError.isEmpty()
                ? tr("A planned activity could not be staged: %1")
                      .arg(targetDescriptions.at(index))
                : stageError);
            return result;
        }
        if (!pendingIsStable(true)) {
            rollback(tr(
                "The planned activity collection changed during staging"));
            return result;
        }
        if (!journal->recordStaged(index, stageError)) {
            rollback(stageError);
            return result;
        }
        if (index < replacementTargetCount
            && !guardedCache->validatePlannedActivityStage(
                journal->stagingPath(index),
                replacementFileNames.at(index),
                stageError)) {
            rollback(stageError.isEmpty()
                ? tr("A staged planned activity is invalid: %1")
                      .arg(replacementFileNames.at(index))
                : stageError);
            return result;
        }
        if (!pendingIsStable(true)) {
            rollback(tr(
                "The planned activity collection changed during staged activity validation"));
            return result;
        }
    }

    ScopedRideFiles processorRides;
    for (const PendingReplacement &entry : std::as_const(pending)) {
        if (!pendingIsStable(true)) {
            rollback(tr(
                "The planned activity collection changed before publication"));
            return result;
        }
        QString processorError;
        RideFile *processorRide =
            guardedCache->openPlannedActivityForDeleteProcessor(
                entry.sourcePath, processorError);
        if (!processorRide) {
            rollback(processorError.isEmpty()
                ? tr("A planned activity could not be opened for delete processing")
                : processorError);
            return result;
        }
        processorRides.append(processorRide);
        if (!pendingIsStable(true)) {
            rollback(tr(
                "The planned activity collection changed while delete processing was prepared"));
            return result;
        }
    }

    bool coordinatorCommitted = false;
    if (coordinator.commit) {
        bool proceed = false;
        QString coordinatorError;
        try {
            proceed = coordinator.commit(
                journal->directoryPath(),
                coordinatorCommitted,
                coordinatorError);
        } catch (const QString &detail) {
            coordinatorError = detail;
        } catch (const std::exception &exception) {
            coordinatorError = QString::fromLocal8Bit(
                exception.what());
        } catch (...) {
            coordinatorError = tr(
                "The planned activity coordinator failed");
        }
        if (!proceed || !coordinatorCommitted) {
            const QString detail = coordinatorError.isEmpty()
                ? (coordinatorCommitted
                    ? tr(
                        "The coordinated plan import requires recovery")
                    : tr(
                        "The planned activity coordinator did not commit"))
                : coordinatorError;
            if (coordinatorCommitted) {
                result.committed = true;
                result.error = detail;
            } else {
                rollback(detail);
            }
            return result;
        }
    }

    const auto completeCoordinator = [&] {
        if (!coordinator.complete) return true;
        bool completed = false;
        QString coordinatorError;
        try {
            completed = coordinator.complete(
                coordinatorError);
        } catch (const QString &detail) {
            coordinatorError = detail;
        } catch (const std::exception &exception) {
            coordinatorError = QString::fromLocal8Bit(
                exception.what());
        } catch (...) {
            coordinatorError = tr(
                "The coordinated plan import could not be completed");
        }
        if (completed) return true;
        appendRemovalError(
            result.error,
            coordinatorError.isEmpty()
                ? tr(
                    "The coordinated plan import requires recovery")
                : coordinatorError);
        return false;
    };

    QString publishError;
    if (!journal->publishAndCommit(publishError)) {
        bool committed = false;
        QString commitStateError;
        if (!journal->commitState(
                committed, commitStateError)) {
            if (coordinatorCommitted)
                result.committed = true;
            appendRemovalError(result.error, publishError);
            appendRemovalError(
                result.error,
                commitStateError.isEmpty()
                    ? tr("The plan replacement commit state is unreadable; recovery is required")
                    : commitStateError);
            return result;
        }
        if (!committed) {
            if (coordinatorCommitted) {
                result.committed = true;
                appendRemovalError(result.error, publishError);
                appendRemovalError(
                    result.error,
                    tr(
                        "The coordinated plan import requires recovery"));
                return result;
            }
            rollback(publishError);
            return result;
        }
        result.committed = true;
        if (!completeCoordinator()) return result;
        QString recoveryError;
        if (!journal->cleanupAfterCommit(recoveryError)) {
            appendRemovalError(result.error, publishError);
            appendRemovalError(result.error, recoveryError);
            return result;
        }
    } else {
        bool committed = false;
        QString commitStateError;
        if (!journal->commitState(
                committed, commitStateError)
            || !committed) {
            if (coordinatorCommitted)
                result.committed = true;
            appendRemovalError(
                result.error,
                commitStateError.isEmpty()
                    ? tr("The plan replacement commit marker is missing; recovery is required")
                    : commitStateError);
            return result;
        }
        result.committed = true;
        if (!completeCoordinator()) return result;
    }

    bool cacheStateDegraded = false;
    const auto recordCacheRepair =
        [&](const CacheRepairResult &repair,
            const QString &ownerDetail,
            const QString &deletionDetail) {
            if (!repair.ownersStable) {
                result.cacheUpdated = false;
                appendRemovalError(result.error, ownerDetail);
                return false;
            }
            if (repair.unexpectedDeletion) {
                cacheStateDegraded = true;
                result.cacheUpdated = false;
                appendRemovalError(result.error, deletionDetail);
            }
            return true;
        };
    const auto postCommitOwnersAreStable =
        [&](const QString &detail) {
            if (operationOwnersAreStable()) return true;
            result.cacheUpdated = false;
            appendRemovalError(result.error, detail);
            return false;
        };

    if (!pendingIsStable(false)) {
        result.error = tr(
            "The planned activity collection changed after its files were committed");
        recordCacheRepair(
            repairDestroyedCacheEntries(false),
            tr("The planned activity collection disappeared after its files were committed"),
            tr("A cached activity was destroyed while planned activity files were committed"));
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }

    guardedCache->invalidateStartupSnapshots();
    if (!operationOwnersAreStable()) {
        result.error = tr(
            "The planned activity collection disappeared before model replacement");
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }

    QVector<RideItem*> incoming;
    incoming.reserve(replacementTargetCount);
    QVector<QPointer<RideItem>> guardedIncoming;
    guardedIncoming.reserve(replacementTargetCount);
    QVector<QDateTime> replacementDateTimes;
    replacementDateTimes.reserve(replacementTargetCount);
    for (const QString &fileName : replacementFileNames) {
        QDateTime dateTime;
        RideFile::parseRideFileName(fileName, &dateTime);
        RideItem *item = new RideItem(nullptr, guardedContext.data());
        item->path = canonicalPlannedRoot;
        item->fileName = fileName;
        item->dateTime = dateTime;
        item->planned = true;
        item->isdirty = false;
        item->isstale = true;
        connect(item, SIGNAL(rideDataChanged()),
                guardedCache.data(), SLOT(itemChanged()));
        connect(item, SIGNAL(rideMetadataChanged()),
                guardedCache.data(), SLOT(itemChanged()));
        incoming.append(item);
        guardedIncoming.append(QPointer<RideItem>(item));
        replacementDateTimes.append(dateTime);
    }
    const auto flushDeferredRideItemDeletes = [&] {
        flushOriginalDeferredDeletes();
        for (int pass = 0; pass < 2; ++pass) {
            for (const QPointer<RideItem> &item :
                 std::as_const(guardedIncoming)) {
                if (item) {
                    QCoreApplication::sendPostedEvents(
                        item.data(),
                        QEvent::DeferredDelete);
                }
            }
        }
    };

    QPointer<RideItem> previousSelection;
    if (guardedCache->ownsLiveRide(guardedContext->ride)) {
        previousSelection = guardedContext->ride;
    }
    QString selectedReplacementName;
    if (previousSelection
        && requestedItems.contains(previousSelection.data())) {
        selectedReplacementName = previousSelection->fileName;
    }
    guardedContext->ride = nullptr;

    QString modelResetError;
    if (!mutation.beginReset(modelResetError)) {
        for (const QPointer<RideItem> &item :
             std::as_const(guardedIncoming)) {
            if (!item) continue;
            item->context = nullptr;
            delete item.data();
        }
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        result.error = modelResetError.isEmpty()
            ? tr("The activity model was busy after the plan files were replaced")
            : modelResetError;
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    flushDeferredRideItemDeletes();
    if (!operationOwnersAreStable()) {
        mutation.endReset();
        for (const QPointer<RideItem> &item :
             std::as_const(guardedIncoming)) {
            if (!item) continue;
            item->context = nullptr;
            delete item.data();
        }
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        result.error = tr(
            "The planned activity collection disappeared during model replacement");
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    const CacheRepairResult resetRepair =
        repairDestroyedCacheEntries(true);
    if (!recordCacheRepair(
            resetRepair,
            tr("The planned activity collection disappeared during model replacement"),
            tr("A cached activity was destroyed during model replacement"))) {
        mutation.endReset();
        for (const QPointer<RideItem> &item :
             std::as_const(guardedIncoming)) {
            if (!item) continue;
            item->context = nullptr;
            delete item.data();
        }
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    for (const PendingReplacement &entry : std::as_const(pending)) {
        guardedCache->rides_.removeOne(entry.address);
    }
    guardedCache->rides_.append(incoming);
    std::sort(
        guardedCache->rides_.begin(),
        guardedCache->rides_.end(),
        [](const RideItem *left, const RideItem *right) {
            return left->dateTime < right->dateTime;
        });
    for (const PendingReplacement &entry : std::as_const(pending)) {
        if (entry.item) guardedCache->delete_.append(entry.item.data());
    }
    mutation.endReset();
    flushDeferredRideItemDeletes();
    if (!postCommitOwnersAreStable(tr(
            "The planned activity collection disappeared during model replacement"))) {
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    if (!recordCacheRepair(
            repairDestroyedCacheEntries(false),
            tr("The planned activity collection disappeared while repairing model replacement"),
            tr("A cached activity was destroyed during model replacement"))) {
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    result.removedCount = pending.size();

    QSet<int> retiredIncoming;
    const auto guardedCacheItemAtAddress =
        [&](RideItem *address) {
            if (!address) return QPointer<RideItem>();
            for (const GuardedCacheEntry &entry : cacheSnapshot) {
                if (entry.address == address && entry.item)
                    return entry.item;
            }
            for (const QPointer<RideItem> &item :
                 std::as_const(guardedIncoming)) {
                if (item.data() == address) return item;
            }
            return QPointer<RideItem>();
        };
    const auto incomingItemIsValid = [&](int index) {
        RideItem *const item =
            guardedIncoming.at(index).data();
        return guardedCache->ownsLiveRide(item)
            && item->context == guardedContext.data()
            && item->planned && !item->isdirty
            && item->path == canonicalPlannedRoot
            && item->fileName
                == replacementFileNames.at(index)
            && item->dateTime
                == replacementDateTimes.at(index);
    };
    const auto incomingIsStable = [&](const QString &detail) {
        flushDeferredRideItemDeletes();
        if (!operationOwnersAreStable()) return false;
        bool stable = true;
        int resetPasses = 0;
        while (true) {
            flushDeferredRideItemDeletes();
            bool hasInvalidItem = false;
            int validCount = 0;
            for (int index = 0;
                 index < guardedIncoming.size(); ++index) {
                if (retiredIncoming.contains(index)) continue;
                if (incomingItemIsValid(index)) {
                    ++validCount;
                } else {
                    hasInvalidItem = true;
                }
            }
            result.addedCount = validCount;
            if (!hasInvalidItem) break;

            stable = false;
            if (++resetPasses
                > guardedIncoming.size() + 1) {
                break;
            }
            const QPointer<RideItem> resetSelection =
                guardedCacheItemAtAddress(
                    guardedContext->ride);
            guardedContext->ride = nullptr;
            QString repairResetError;
            if (!mutation.beginReset(repairResetError)) {
                result.cacheUpdated = false;
                appendRemovalError(
                    result.error,
                    repairResetError.isEmpty()
                        ? tr("The activity model became busy while published activities were repaired")
                        : repairResetError);
                return false;
            }
            flushDeferredRideItemDeletes();
            if (!operationOwnersAreStable()) {
                mutation.endReset();
                result.cacheUpdated = false;
                appendRemovalError(result.error, detail);
                return false;
            }
            if (!recordCacheRepair(
                    repairDestroyedCacheEntries(true),
                    tr("The planned activity collection disappeared while repairing published activities"),
                    tr("A cached activity was destroyed while published activities were repaired"))) {
                mutation.endReset();
                appendRemovalError(result.error, detail);
                return false;
            }

            for (int index = 0;
                 index < guardedIncoming.size(); ++index) {
                if (retiredIncoming.contains(index)
                    || incomingItemIsValid(index)) {
                    continue;
                }
                RideItem *const address = incoming.at(index);
                RideItem *const item =
                    guardedIncoming.at(index).data();
                guardedCache->rides_.removeOne(address);
                guardedCache->delete_.removeOne(address);
                guardedCache->deletelist.remove(address);
                if (guardedContext->ride == address)
                    guardedContext->ride = nullptr;
                if (item) {
                    if (!guardedCache->delete_.contains(item))
                        guardedCache->delete_.append(item);
                }
                retiredIncoming.insert(index);
            }
            mutation.endReset();
            flushDeferredRideItemDeletes();
            if (!operationOwnersAreStable()) {
                result.cacheUpdated = false;
                appendRemovalError(result.error, detail);
                return false;
            }
            if (!recordCacheRepair(
                    repairDestroyedCacheEntries(false),
                    tr("The planned activity collection disappeared after published activities were repaired"),
                    tr("A cached activity was destroyed while published activities were repaired"))) {
                appendRemovalError(result.error, detail);
                return false;
            }
            if (resetSelection
                && guardedCache->ownsLiveRide(
                    resetSelection.data())) {
                guardedContext->ride = resetSelection.data();
            }
        }
        if (!stable) {
            result.cacheUpdated = false;
            appendRemovalError(result.error, detail);
        }
        return stable;
    };

    if (!incomingIsStable(tr(
            "A newly published planned activity changed during model replacement"))) {
        QString cleanupError;
        result.cleanupComplete =
            journal->cleanupAfterCommit(cleanupError);
        appendRemovalError(result.error, cleanupError);
        return result;
    }
    result.cacheUpdated = !cacheStateDegraded;

    QString cleanupError;
    result.cleanupComplete =
        journal->cleanupAfterCommit(cleanupError);
    appendRemovalError(result.error, cleanupError);
    if (!postCommitOwnersAreStable(tr(
            "The planned activity collection disappeared after replacement cleanup"))) {
        return result;
    }

    for (const QPointer<RideFile> &processorRide :
         processorRides.files()) {
        if (!processorRide) {
            result.warnings.append(tr(
                "An activity delete processor input disappeared before processing"));
            continue;
        }
        QString processorError;
        const QPointer<RideFile> guardedProcessorRide(
            processorRide);
        if (!runRemovalDeleteProcessor(
                guardedProcessorRide.data(), processorError)) {
            result.warnings.append(
                processorError.isEmpty()
                    ? tr("An activity delete processor failed")
                    : processorError);
        }
        if (guardedProcessorRide) {
            QCoreApplication::sendPostedEvents(
                guardedProcessorRide.data(),
                QEvent::DeferredDelete);
        }
        flushDeferredRideItemDeletes();
        if (!guardedProcessorRide) {
            result.warnings.append(tr(
                "An activity delete processor destroyed its input"));
        }
        if (!postCommitOwnersAreStable(tr(
                "The planned activity collection disappeared during delete processing"))) {
            return result;
        }
        if (!recordCacheRepair(
                repairDestroyedCacheEntries(false),
                tr("The planned activity collection disappeared after delete processing"),
                tr("A cached activity was destroyed during delete processing"))) {
            return result;
        }
        if (!incomingIsStable(tr(
                "A newly published planned activity changed during delete processing"))) {
            return result;
        }
    }

    RideItem *selection = previousSelection.data();
    if (!guardedCache->ownsLiveRide(selection)) {
        selection = nullptr;
        if (!selectedReplacementName.isEmpty()) {
            for (const QPointer<RideItem> &item :
                 std::as_const(guardedIncoming)) {
                if (item && item->fileName
                        == selectedReplacementName) {
                    selection = item.data();
                    break;
                }
            }
        }
        if (!selection) {
            for (const QPointer<RideItem> &item :
                 std::as_const(guardedIncoming)) {
                if (item) {
                    selection = item.data();
                    break;
                }
            }
        }
        if (!selection) {
            const QVector<RideItem*> liveRides = guardedCache->rides();
            if (!liveRides.isEmpty()) selection = liveRides.constFirst();
        }
    }
    RideItem *const selectionAddress = selection;
    const QPointer<RideItem> guardedSelection(selection);
    guardedContext->ride = guardedSelection.data();

    const auto selectionIsStable = [&](const QString &detail) {
        if (!postCommitOwnersAreStable(tr(
                "The planned activity collection disappeared while validating its selection"))) {
            return false;
        }
        const CacheRepairResult selectionRepair =
            repairDestroyedCacheEntries(false);
        if (!recordCacheRepair(
                selectionRepair,
                tr("The planned activity collection disappeared while repairing its selection"),
                detail)) {
            return false;
        }
        if (!incomingIsStable(tr(
                "A newly published planned activity changed while the selection was repaired"))) {
            return false;
        }
        if (!selectionAddress) return true;
        if (guardedSelection
            && guardedCache->ownsLiveRide(
                guardedSelection.data())) {
            return true;
        }
        guardedContext->ride = nullptr;
        result.cacheUpdated = false;
        if (!selectionRepair.unexpectedDeletion)
            appendRemovalError(result.error, detail);
        return false;
    };

    for (const PendingReplacement &entry : std::as_const(pending)) {
        RideItem *const removed = entry.item.data();
        if (removed) guardedContext->notifyRideDeleted(removed);
        flushDeferredRideItemDeletes();
        if (!postCommitOwnersAreStable(tr(
                "The planned activity collection disappeared during deletion notification"))) {
            return result;
        }
        if (!entry.item) {
            guardedCache->delete_.removeOne(entry.address);
            guardedCache->deletelist.remove(entry.address);
        }
        if (!incomingIsStable(tr(
                "A newly published planned activity changed during deletion notification"))
            || !selectionIsStable(tr(
                "The selected activity changed during deletion notification"))) {
            return result;
        }
    }
    if (notifyAdded) {
        for (const QPointer<RideItem> &item :
             std::as_const(guardedIncoming)) {
            const QPointer<RideItem> guardedItem(item);
            if (guardedItem)
                guardedContext->notifyRideAdded(guardedItem.data());
            flushDeferredRideItemDeletes();
            if (!postCommitOwnersAreStable(tr(
                    "The planned activity collection disappeared during addition notification"))) {
                return result;
            }
            if (!incomingIsStable(tr(
                    "A newly published planned activity changed during addition notification"))
                || !selectionIsStable(tr(
                    "The selected activity changed during addition notification"))) {
                return result;
            }
        }
    }
    if (!postCommitOwnersAreStable(tr(
            "The planned activity collection disappeared before selection notification"))) {
        return result;
    }
    if (!incomingIsStable(tr(
            "A newly published planned activity changed before selection notification"))
        || !selectionIsStable(tr(
            "The selected activity changed before selection notification"))) {
        return result;
    }
    guardedContext->ride = guardedSelection.data();
    guardedContext->notifyRideSelected(guardedSelection.data());
    flushDeferredRideItemDeletes();
    if (!postCommitOwnersAreStable(tr(
            "The planned activity collection disappeared during selection notification"))) {
        return result;
    }
    if (!incomingIsStable(tr(
            "A newly published planned activity changed during selection notification"))
        || !selectionIsStable(tr(
            "The selected activity changed during selection notification"))) {
        return result;
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
        if (!ownsLiveRide(ride)
            || ride->fileName != fileName)
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
        if (!ownsLiveRide(ride)
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
    if (!ownsLiveRide(item)) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The activity is no longer in the activity list");
        return check;
    }

    QList<RideItem*> incomingLinkedItems;
    for (RideItem *candidate : std::as_const(rides_)) {
        if (!ownsLiveRide(candidate)
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
            if (!ownsLiveRide(candidate)
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
    bool triggerRefresh,
    bool workersQuiesced)
{
    RemovalResult result;
    result.requestedCount = 1;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        appendRemovalItemResult(
            result, {}, false,
            RemovalStatus::Rejected,
            result.error);
        return result;
    }
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

    if (index < 0 || deletelist.contains(todelete)) {
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
    if (!workersQuiesced) {
        guardedCache->cancel();
        if (!operationOwnersAreStable()
            || !deletionItem
            || !guardedCache->ownsLiveRide(
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
        guardedEstimator->stop();
        if (!operationOwnersAreStable()
            || !deletionItem
            || !guardedCache->ownsLiveRide(
                deletionItem.data())) {
            result.error = tr(
                "The activity deletion target changed while estimates were being stopped");
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
            if (!plannedBackupExisted) {
                QString createError;
                if (!createDirectoryDurably(
                        plannedBackupPath,
                        createError,
                        syncRemovalDirectory)) {
                    result.error = createError.isEmpty()
                        ? tr(
                            "Cannot create the planned activity backup directory")
                        : createError;
                    appendRemovalItemResult(
                        result, filenameToDelete,
                        todelete->planned,
                        RemovalStatus::Rejected,
                        result.error);
                    return result;
                }
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
            if (plannedBackupExisted) {
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
        [guardedCache, filenameToDelete, plannedToDelete,
         activeDirectoryIdentity](RideItem *candidate) {
            if (!guardedCache->ownsLiveRide(candidate)
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
            if (!guardedCache->ownsLiveRide(item)
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
                if (!guardedCache->ownsLiveRide(candidate)
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
    std::shared_ptr<
        LinkedActivityRemoval::Journal> linkedJournal;

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
            return guardedCache->ownsLiveRide(item)
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
            if (!guardedCache->ownsLiveRide(item)) {
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
                if (!guardedCache->ownsLiveRide(candidate)
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
            if (!guardedCache->ownsLiveRide(item))
                return QString();
            return RideFileCacheIntegrity::activitySourcePath(
                item->path, item->fileName);
        };
    const auto appendLinkedRecoveryPaths =
        [&appendRecoveryPath,
         &currentLinkedSourcePath,
         &linkedOriginalSourcePath,
         &sourcePath,
         &linkedJournal] {
            appendRecoveryPath(sourcePath);
            appendRecoveryPath(
                linkedOriginalSourcePath);
            appendRecoveryPath(
                currentLinkedSourcePath());
            if (linkedJournal) {
                for (const QString &path :
                     linkedJournal->recoveryPaths()) {
                    appendRecoveryPath(path);
                }
            }
        };
    const auto restoreLinkedPeer =
        [guardedCache, operationOwnersAreStable,
         deletionTargetIsStable,
         &linkedItem, &linkedFileName,
         &linkedIdentityMatches,
         &acceptLinkedIdentity,
         &linkedJournal](QString &error) {
            if (linkedJournal) {
                QString journalError;
                if (!linkedJournal->cleanupAfterRollback(
                        journalError)) {
                    error = journalError.isEmpty()
                        ? QObject::tr(
                            "The linked activity deletion journal could not be rolled back")
                        : journalError;
                    return false;
                }
            }
            if (!operationOwnersAreStable()) {
                error = QObject::tr(
                    "The activity collection disappeared before the linked activity could be restored");
                return false;
            }
            RideItem *item = linkedItem.data();
            if (!guardedCache->ownsLiveRide(item)) {
                error = QObject::tr(
                    "The linked activity disappeared before it could be restored");
                return false;
            }
            item->setLinkedFileName(
                linkedFileName);
            item = linkedItem.data();
            if (!operationOwnersAreStable()
                || !guardedCache->ownsLiveRide(item)
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
                || !guardedCache->ownsLiveRide(item)
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
        if (!archiveSource) {
            result.error = tr(
                "A linked activity cannot be removed after its source has already been archived");
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        linkedItem =
            linkCheck.affectedItems.value(1);
        if (!guardedCache->ownsLiveRide(linkedItem.data())) {
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
        QString linkedSavePathError;
        if (!linkedRemovalPeerSaveKeepsPath(
                linkedItem.data(),
                linkedSavePathError)) {
            result.error = linkedSavePathError;
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                RemovalStatus::Rejected,
                result.error);
            return result;
        }
        QString journalError;
        linkedJournal =
            LinkedActivityRemoval::Journal::prepare(
                {
                    guardedAthlete->home->root()
                        .absolutePath(),
                    sourcePath,
                    backupPath,
                    linkedOriginalSourcePath,
                    derivedPaths
                },
                journalError);
        if (!linkedJournal
            || !linkedJournal->validateOriginalStorage(
                journalError)) {
            result.error = journalError.isEmpty()
                ? tr(
                    "The linked activity deletion journal could not be prepared")
                : journalError;
            if (linkedJournal) {
                QString cleanupError;
                if (!linkedJournal->cleanupAfterRollback(
                        cleanupError)) {
                    requireLinkedRecovery(
                        cleanupError.isEmpty()
                        ? tr(
                            "The incomplete linked activity deletion journal requires recovery")
                        : cleanupError);
                }
            }
            appendRemovalItemResult(
                result, filenameToDelete,
                plannedToDelete,
                result.status,
                result.error,
                result.recoveryPaths);
            return result;
        }
        linkedFileName =
            linkedItem->getLinkedFileName();
        linkedItem->clearLinkedFileName();
        if (!operationOwnersAreStable()
            || !guardedCache->ownsLiveRide(linkedItem.data())
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
            const bool linkRestored =
                restoreLinkedPeer(restoreError);
            QString journalCleanupError;
            const bool journalCleaned =
                linkRestored
                && deletionTargetIsStable()
                && !linkedIdentityChanged
                && linkedJournal->cleanupAfterRollback(
                    journalCleanupError);
            requireLinkedRecovery(tr(
                "The deletion target changed while its linked activity was being updated"));
            if (!journalCleaned) {
                appendRemovalResultError(
                    result,
                    journalCleanupError.isEmpty()
                    ? tr(
                        "The linked activity deletion journal requires recovery")
                    : journalCleanupError);
            }
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
            QString journalCleanupError;
            const bool journalCleaned =
                linkRestored
                && deletionTargetIsStable()
                && !linkedIdentityChanged
                && linkedJournal->cleanupAfterRollback(
                    journalCleanupError);
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged
                || !journalCleaned) {
                requireLinkedRecovery(tr(
                    "The linked activity could not be restored after its reciprocal link changed"));
                appendRemovalResultError(
                    result, restoreError);
                appendRemovalResultError(
                    result, journalCleanupError);
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
                linkedItem.data(), linkError,
                linkedJournal->peerWriterFactory(
                    qSaveFileWriterFactory()))) {
            result.error = linkError.isEmpty()
                ? tr("The linked activity could not be updated")
                : linkError;
            QString linkRollbackError;
            const bool linkRestored =
                restoreLinkedPeer(
                    linkRollbackError);
            bool journalCleaned = false;
            if (linkRestored
                && deletionTargetIsStable()
                && !linkedIdentityChanged) {
                QString journalCleanupError;
                journalCleaned =
                    linkedJournal->cleanupAfterRollback(
                        journalCleanupError);
                appendRemovalError(
                    linkRollbackError,
                    journalCleanupError);
            }
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged
                || !journalCleaned) {
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
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "peer-published");
#endif
        linkedIdentityError.clear();
        const bool linkedIdentityAccepted =
            acceptLinkedIdentity(
                linkedIdentityError);
        if (!linkedIdentityAccepted
            || !operationOwnersAreStable()
            || !guardedCache->ownsLiveRide(linkedItem.data())
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
            const bool linkRestored =
                restoreLinkedPeer(restoreError);
            bool journalCleaned = false;
            if (linkRestored
                && deletionTargetIsStable()
                && !linkedIdentityChanged) {
                QString journalCleanupError;
                journalCleaned =
                    linkedJournal->cleanupAfterRollback(
                        journalCleanupError);
                appendRemovalError(
                    restoreError,
                    journalCleanupError);
            }
            requireLinkedRecovery(tr(
                "The deletion target changed while its linked activity was being saved"));
            if (!journalCleaned) {
                appendRemovalResultError(
                    result,
                    tr("The linked activity deletion journal requires recovery"));
            }
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

    if (archiveSource && !linkedJournal) {
        QString journalError;
        linkedJournal =
            LinkedActivityRemoval::Journal::prepare(
                {
                    guardedAthlete->home->root()
                        .absolutePath(),
                    sourcePath,
                    backupPath,
                    QString(),
                    derivedPaths
                },
                journalError);
        if (!linkedJournal
            || !linkedJournal->validateOriginalStorage(
                journalError)) {
            result.error = journalError.isEmpty()
                ? tr(
                    "The activity deletion journal could not be prepared")
                : journalError;
            if (linkedJournal) {
                QString cleanupError;
                if (!linkedJournal->cleanupAfterRollback(
                        cleanupError)) {
                    result.status =
                        RemovalStatus::RecoveryRequired;
                    appendRemovalResultError(
                        result,
                        cleanupError.isEmpty()
                        ? tr(
                            "The incomplete activity deletion journal requires recovery")
                        : cleanupError);
                    for (const QString &path :
                         linkedJournal->recoveryPaths()) {
                        appendRecoveryPath(path);
                    }
                }
            }
            appendRemovalItemResult(
                result,
                filenameToDelete,
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
            if (!guardedCache->ownsLiveRide(peer)
                || !peer->getLinkedFileName()
                        .isEmpty()) {
                return false;
            }
            for (RideItem *candidate :
                 std::as_const(guardedCache->rides_)) {
                if (!guardedCache->ownsLiveRide(candidate)
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

    StorageRemovalResult storage =
        removeRideFilesFromStorage(
            sourcePath,
            backupPath,
            derivedPaths,
            archiveSource,
            prepareRemoval,
            linkedJournal);
    if (storage.status == RemovalStatus::Committed
        && linkedJournal) {
        QString cleanupError;
        if (!linkedJournal->cleanupAfterCommit(
                cleanupError)) {
            storage.status = RemovalStatus::
                CommittedCleanupPending;
            appendRemovalError(
                storage.error, cleanupError);
            for (const QString &path :
                 linkedJournal->recoveryPaths()) {
                addRecoveryPath(
                    storage.recoveryPaths, path);
            }
        }
    } else if (storage.status
                   == RemovalStatus::CommittedCleanupPending
               && linkedJournal) {
        for (const QString &path :
             linkedJournal->recoveryPaths()) {
            addRecoveryPath(
                storage.recoveryPaths, path);
        }
    }
    if (!storage.committed()
        && linkedJournal
        && !linkedItemWasSaved) {
        if (storage.status
            == RemovalStatus::RecoveryRequired) {
            for (const QString &path :
                 linkedJournal->recoveryPaths()) {
                addRecoveryPath(
                    storage.recoveryPaths, path);
            }
        } else {
            QString cleanupError;
            if (!linkedJournal->cleanupAfterRollback(
                    cleanupError)) {
                storage.status =
                    RemovalStatus::RecoveryRequired;
                appendRemovalError(
                    storage.error,
                    cleanupError.isEmpty()
                    ? tr(
                        "The activity deletion journal requires recovery")
                    : cleanupError);
                for (const QString &path :
                     linkedJournal->recoveryPaths()) {
                    addRecoveryPath(
                        storage.recoveryPaths, path);
                }
            }
        }
    }
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
            bool journalCleaned = false;
            if (linkRestored
                && storage.status
                    != RemovalStatus::RecoveryRequired
                && deletionTargetIsStable()
                && !linkedIdentityChanged) {
                QString journalCleanupError;
                journalCleaned = linkedJournal
                    && linkedJournal->cleanupAfterRollback(
                        journalCleanupError);
                appendRemovalError(
                    linkRollbackError,
                    journalCleanupError);
            }
            if (!linkRestored
                || !deletionTargetIsStable()
                || linkedIdentityChanged
                || (storage.status
                        != RemovalStatus::RecoveryRequired
                    && !journalCleaned)) {
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
    if (guardedCache->ownsLiveRide(
            previousSelectionCandidate)) {
        previousSelection = previousSelectionCandidate;
    }
    QPointer<RideItem> nextSelection;
    if (previousSelection == todelete) {
        for (int row = index + 1;
             row < guardedCache->rides_.size(); ++row) {
            RideItem *candidate = guardedCache->rides_.at(row);
            if (guardedCache->ownsLiveRide(candidate)) {
                nextSelection = candidate;
                break;
            }
        }
        for (int row = index - 1;
             !nextSelection && row >= 0; --row) {
            RideItem *candidate = guardedCache->rides_.at(row);
            if (guardedCache->ownsLiveRide(candidate)) {
                nextSelection = candidate;
                break;
            }
        }
    } else if (previousSelection
               && guardedCache->ownsLiveRide(
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
    if (!guardedModel->startRemove(index)) {
        result.status = RemovalStatus::RecoveryRequired;
        appendRemovalError(
            result.error,
            tr("The activity model became busy after its files were archived"));
        if (!result.items.isEmpty()) {
            result.items.last().status = result.status;
            result.items.last().error = result.error;
        }
        return result;
    }
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
    if (!deletionItem) {
        guardedCache->delete_.removeOne(todelete);
        guardedCache->discardDetachedTombstones();
    }
    if (!guardedCache->purgeDestroyedModelRows()) {
        if (!operationOwnersAreStable()) return result;
        result.status = RemovalStatus::RecoveryRequired;
        appendRemovalError(
            result.error,
            tr("Destroyed activity rows could not be removed from the activity model"));
        if (!result.items.isEmpty()) {
            result.items.last().status = result.status;
            result.items.last().error = result.error;
        }
        return result;
    }
    if (!operationOwnersAreStable()) return result;
    QObject::disconnect(selectionDestroyedConnection);

    RideItem *stableSelection =
        nextSelection.data();
    if (stableSelection
        && !guardedCache->ownsLiveRide(
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
    guardedCache->discardDetachedTombstones();
    stableSelection = nextSelection.data();
    if (stableSelection
        && !guardedCache->ownsLiveRide(
            stableSelection)) {
        stableSelection = nullptr;
    }
    guardedContext->notifyRideSelected(
        stableSelection);
    if (guardedMainWindow)
        guardedMainWindow->setUpdatesEnabled(true);
    if (!operationOwnersAreStable()) return result;

    if (triggerRefresh) {
        guardedCache->removalRefreshPending_ = false;
        removalGuard.release();
        guardedCache->refresh();
        if (!operationOwnersAreStable()) return result;
        // model estimates (lazy refresh)
        guardedEstimator->refresh();
    } else {
        removalGuard.release();
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
