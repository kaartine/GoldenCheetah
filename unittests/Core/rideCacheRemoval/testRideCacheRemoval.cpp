#include <QtTest>

#include "Athlete.h"
#include "Context.h"
#include "AtomicFileWriter.h"
#include "LinkedActivityRemovalJournal.h"
#include "LinkedActivitySaveJournal.h"
#include "RideCache.h"
#include "RideCacheModel.h"
#include "RideItem.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

void resetRideCacheRemovalRefreshCounts();
int rideCacheRemovalRefreshCount();
int rideCacheRemovalEstimatorRefreshCount();
void setRideCacheRemovalCleanupFailurePath(const QString &path);
void setRideCacheRemovalMoveFailurePath(const QString &path);
void setRideCacheRemovalMoveFailureTargetPath(
    const QString &path);
void setRideCacheRemovalPartialMoveFailurePath(
    const QString &path,
    bool removeSource);
void setRideCacheRemovalPartialMoveFailureTargetPath(
    const QString &path,
    bool removeSource);
void setRideCacheRemovalMoveMutation(
    const QString &path,
    const QByteArray &contents);
void setRideCacheRemovalMoveAction(
    const QString &path,
    const std::function<void()> &action);
void setRideCacheRemovalSyncFailurePath(const QString &path);
void setRideCacheRemovalSyncFailureCount(
    const QString &path,
    int count);
void setRideCacheRemovalSaveFailureFileName(
    const QString &fileName);
void setRideCacheRemovalSaveFailureOnCall(
    const QString &fileName,
    int call);
void setRideCacheRemovalSaveFailureCalls(
    const QString &fileName,
    const QSet<int> &calls);
int rideCacheRemovalSaveCallCount();
int rideCacheRemovalCancelCount();
void setRideCacheRemovalSaveActionOnCall(
    const QString &fileName,
    int call,
    const std::function<void()> &action);
void setRideCacheRemovalSuccessfulSaveRename(
    const QString &fileName,
    const QString &targetDirectory,
    const QString &targetFileName);
void setRideCacheRemovalPersistedSaveContents(
    const QString &fileName,
    const QByteArray &contents);
void setRideCacheRemovalPersistedSaveContentsOnCall(
    const QString &fileName,
    int call,
    const QByteArray &contents);
RideFile *rideCacheRemovalLastProcessedRide();
void setRideCacheRemovalProcessorFailure(bool fail);
void setRideCacheRemovalProcessorAction(
    const std::function<void()> &action);
void setRideCacheRemovalLinkMutationActionOnCall(
    int call,
    const std::function<void()> &action);

namespace {

QString firstName()
{
    return QStringLiteral("2026_07_06_08_00_00.json");
}

QString secondName()
{
    return QStringLiteral("2026_07_06_09_00_00.json");
}

QString thirdName()
{
    return QStringLiteral("2026_07_06_10_00_00.json");
}

void writeFixture(const QString &path, const QByteArray &contents)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY2(
        file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
    QVERIFY(file.flush());
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool cacheContains(const RideCache &cache, const QString &fileName)
{
    for (const RideItem *item :
         cache.rides()) {
        if (item && item->fileName == fileName) return true;
    }
    return false;
}

QStringList stagedFilesFor(const QString &originalPath)
{
    const QFileInfo original(originalPath);
    const QDir directory(original.absolutePath());
    const QStringList names = directory.entryList(
        {original.fileName()
             + QStringLiteral(".gc-remove-*")},
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QStringList paths;
    paths.reserve(names.size());
    for (const QString &name : names) {
        paths.append(directory.filePath(name));
    }
    return paths;
}

QStringList recoveryFilesFor(
    const QString &originalPath,
    const QString &marker)
{
    const QFileInfo original(originalPath);
    const QDir directory(original.absolutePath());
    const QStringList names = directory.entryList(
        {original.fileName() + marker
             + QStringLiteral("*")},
        QDir::Files | QDir::Hidden
            | QDir::NoDotAndDotDot);
    QStringList paths;
    paths.reserve(names.size());
    for (const QString &name : names)
        paths.append(directory.filePath(name));
    return paths;
}

void dismissNextMessageBox()
{
    QTimer::singleShot(0, [] {
        QMessageBox *messageBox =
            qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
        if (messageBox) messageBox->accept();
    });
}

struct Fixture
{
    bool initialize()
    {
        if (!temporary.isValid()) return false;
        context.reset(new Context(nullptr));
        athlete.reset(new Athlete(context.get(), QDir(temporary.path())));
        cache.reset(new RideCache(context.get()));
        athlete->rideCache = cache.get();
        resetRideCacheRemovalRefreshCounts();
        return athlete->home->activities().exists()
            && athlete->home->fileBackup().exists()
            && athlete->home->cache().exists();
    }

    RideItem *addRide(
        const QString &fileName,
        bool current,
        RideFile *ride = nullptr)
    {
        RideItem *item = new RideItem(ride, context.get());
        item->fileName = fileName;
        item->path = athlete->home->activities().absolutePath();
        cache->mutableRidesForRemovalTest().append(item);
        if (current) context->ride = item;
        return item;
    }

    RideItem *addPlannedRide(
        const QString &fileName,
        bool current,
        RideFile *ride = nullptr)
    {
        RideItem *item =
            addRide(fileName, current, ride);
        item->planned = true;
        item->path =
            athlete->home->planned().absolutePath();
        return item;
    }

    QString activityPath(const QString &fileName) const
    {
        return athlete->home->activities().filePath(fileName);
    }

    QString plannedActivityPath(const QString &fileName) const
    {
        return athlete->home->planned().filePath(fileName);
    }

    QString backupPath(const QString &fileName) const
    {
        return athlete->home->fileBackup().filePath(
            fileName + QStringLiteral(".bak"));
    }

    QString plannedBackupPath(const QString &fileName) const
    {
        return QDir(
            athlete->home->fileBackup().filePath(
                QStringLiteral("planned")))
            .filePath(
                fileName + QStringLiteral(".bak"));
    }

    QString cachePath(const QString &fileName, const QString &extension) const
    {
        return athlete->home->cache().filePath(
            QFileInfo(fileName).baseName()
            + QLatin1Char('.') + extension);
    }

    QString plannedCachePath(
        const QString &fileName,
        const QString &extension) const
    {
        return QDir(
            athlete->home->cache().filePath(
                QStringLiteral("planned")))
            .filePath(
                QFileInfo(fileName).baseName()
                + QLatin1Char('.') + extension);
    }

    QTemporaryDir temporary;
    std::unique_ptr<Context> context;
    std::unique_ptr<Athlete> athlete;
    std::unique_ptr<RideCache> cache;
};

} // namespace

class TestRideCacheRemoval : public QObject
{
    Q_OBJECT

private slots:
    void archivedRemovalEvictsNamedRideWithoutMovingFiles();
    void ordinaryRemovalArchivesFileAndReplacesBackup();
    void archiveFailurePreservesCacheAndFiles();
    void backupReplacementFailurePreservesCacheAndFiles();
    void backupStagingFailureRollsBackFiles();
    void sourceArchiveFailureRollsBackFiles();
    void sourceMutationBeforeArchiveRollsBackFiles();
    void unsafeFilenameFailsClosed();
    void sourceDirectoryMismatchIsRejected();
    void duplicateFullIdentityFailsClosed();
    void plannedAndCompletedBackupsUseSeparateNamespaces();
    void plannedBackupDirectorySyncFailurePreservesActivity();
    void plannedBackupDirectorySyncFailureIsRetried();
    void plannedBackupSymlinkIsRejected();
    void linkedSaveFailureRejectsRemoval();
    void linkedArchivedRemovalRejectsBeforePeerMutation();
    void unsafeLinkedPeerIdentityRejectsBeforeSave_data();
    void unsafeLinkedPeerIdentityRejectsBeforeSave();
    void incomingReferenceToLinkedPeerRejectsBeforeSave();
    void linkedSaveRenameIsRejectedBeforeMutation();
    void linkedSaveCompensationFailureRequiresRecovery();
    void linkedPeerDeletionDuringMetadataChangeRequiresRecovery();
    void linkedPeerDeletionDuringCompensationRequiresRecovery_data();
    void linkedPeerDeletionDuringCompensationRequiresRecovery();
    void targetDeletionDuringLinkedMetadataChangeRequiresRecovery();
    void targetMutationDuringStorageRollbackRequiresRecovery_data();
    void targetMutationDuringStorageRollbackRequiresRecovery();
    void linkedRenameBeforeStorageRollbackRequiresRecovery();
    void linkedReplacementDuringClearIsRestored();
    void dirtyLinkedPeerRequiresPreflight();
    void brokenLinkedPairRejectsRemoval();
    void incomingLinkedReferenceRejectsRemoval();
    void sameNamespaceIncomingLinkRejectsRemoval();
    void removalLinkPreflightAllowsUnlinkedActivity();
    void dataProcessorReceivesLoadedRideBeforeArchive();
    void linkedRideClosedDuringMetadataChangeIsNotReused();
    void sourceSymlinkIsRejected();
    void lockedSourceIsRejected();
    void derivedFilesAreNotMovedBeforeCommit_data();
    void derivedFilesAreNotMovedBeforeCommit();
    void derivedMutationBeforeStagingRollsBackFiles();
    void directorySyncFailureRollsBackFiles();
    void rollbackFailureRequiresRecovery();
    void sourceRestoreSyncFailureRetainsPublishedBackup();
    void failedPreviousBackupMovePartialStateRollsBack_data();
    void failedPreviousBackupMovePartialStateRollsBack();
    void failedBackupPublishPartialMoveRollsBack_data();
    void failedBackupPublishPartialMoveRollsBack();
    void failedSourceMovePartialStateRollsBack_data();
    void failedSourceMovePartialStateRollsBack();
    void batchStopsAfterRecoveryRequired();
    void derivedCleanupFailurePreservesCacheAndFiles_data();
    void derivedCleanupFailurePreservesCacheAndFiles();
    void committedCleanupFailureLeavesRecoveryFile_data();
    void committedCleanupFailureLeavesRecoveryFile();
    void cleanupPendingBoolReportsLogicalRemoval();
    void previousBackupCleanupFailureIsExplicit();
    void sourceCleanupFailureIsExplicit();
    void archivedCleanupFailurePreservesCacheAndFiles();
    void batchRemovalReportsPartialFailure();
    void stagingCleanupFailureRequiresRecoveryAndStopsBatch();
    void partialBatchBoolPreservesAnySuccessContract();
    void sharedLegacySidecarsSurviveNamespaceRemoval();
    void linkedRecoveryRequiredRestoresPair();
    void linkedRollbackSaveFailureReportsRecoveryPath();
    void linkedRollbackPrecedesCompensationSerialization();
    void concurrentLinkedRemovalJournalsAreSerialized();
    void activeLinkedSaveAndRemovalShareAthleteLease();
    void abandonedLinkedRemovalJournalBlocksNextTransaction();
    void pendingLinkedSaveJournalBlocksDeletionTransaction();
    void startupReconcilesAbandonedLinkedSaveJournal();
    void startupReportsCorruptLinkedSaveJournal();
    void prepareRestrictsExistingTransactionDirectories();
    void startupRestrictsExistingTransactionDirectories();
    void oversizedUnreadableJournalControlFileFailsBeforeRead_data();
    void oversizedUnreadableJournalControlFileFailsBeforeRead();
    void startupWithoutJournalAllowsSymlinkedAthleteRoot();
    void linkedDeletionCrashAfterPeerSaveRecoversOnRestart_data();
    void linkedDeletionCrashAfterPeerSaveRecoversOnRestart();
    void processorFailureIsReportedWithoutRemoval();
    void processorReorderingDoesNotRemoveWrongRow();
    void sidecarOwnerIntroducedBeforeCommitRejectsRemoval();
    void modelRemovalSignalSiblingRemovalDoesNotUseInvalidIndex();
    void modelRemovalSignalKeepsSelectionValid_data();
    void modelRemovalSignalKeepsSelectionValid();
    void modelRemovalSignalDestructionDoesNotExposeDanglingItem_data();
    void modelRemovalSignalDestructionDoesNotExposeDanglingItem();
    void modelSignalOwnerDestructionDoesNotContinue_data();
    void modelSignalOwnerDestructionDoesNotContinue();
    void batchModelSignalOwnerDestructionDoesNotContinue_data();
    void batchModelSignalOwnerDestructionDoesNotContinue();
    void currentRideRemovalUsesOrdinaryArchivePath();
    void nonCurrentRemovalNotifiesDeletionAndPreservesSelection();
    void batchRemovingAllRidesClearsSelection();
    void missingRideIsRejectedWithoutTouchingFiles();
    void plannedRemovalDeletesOnlyPlannedCpx();
    void plannedBatchRemovalDeletesOnlyPlannedCpx();
    void batchRemovalRefreshesOnce();
    void singleRemovalCancelsRefreshBeforeProcessing();
    void batchRemovalSnapshotsAliasedRideList();
    void batchRejectsReentrantIdentityMutation();
    void batchSurvivesReentrantPendingDeletion();
    void stalePointerOverloadsRejectWithoutDereference_data();
    void stalePointerOverloadsRejectWithoutDereference();
    void currentRemovalUsesSelectedNamespace();
    void ambiguousFilenameRemovalFailsClosed();
    void explicitNamespaceRemovalUsesIdentity();
    void explicitBatchRemovalUsesItemIdentity();
    void plannedRenameMovesOnlyPlannedCpx();
    void completedRenameMovesOnlyCompletedCpx();
};

void TestRideCacheRemoval::
archivedRemovalEvictsNamedRideWithoutMovingFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *archivedItem =
        fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);

    const QByteArray decoy("new file at the old activity path");
    const QByteArray archived("already archived original");
    writeFixture(fixture.activityPath(firstName()), decoy);
    writeFixture(fixture.backupPath(firstName()), archived);
    writeFixture(
        fixture.cachePath(firstName(), QStringLiteral("notes")),
        QByteArray("derived notes"));

    QVERIFY(fixture.cache->removeArchivedRide(archivedItem));

    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QCOMPARE(fixture.context->ride->fileName, secondName());
    QCOMPARE(readBytes(fixture.activityPath(firstName())), decoy);
    QCOMPARE(readBytes(fixture.backupPath(firstName())), archived);
    QVERIFY(!QFileInfo::exists(
        fixture.cachePath(firstName(), QStringLiteral("notes"))));
}

void TestRideCacheRemoval::ordinaryRemovalArchivesFileAndReplacesBackup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);

    const QByteArray original("live original");
    writeFixture(fixture.activityPath(firstName()), original);
    writeFixture(
        fixture.backupPath(firstName()),
        QByteArray("previous backup"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(firstName());
    QVERIFY2(
        result.cleanlyCompleted(),
        qPrintable(result.error));
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(result.error.isEmpty());

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), original);
}

void TestRideCacheRemoval::archiveFailurePreservesCacheAndFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray previousBackup("previous backup");
    const QByteArray notes("derived notes");
    const QByteArray cpi("derived cpi");
    const QByteArray cpx("derived cpx");
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("notes")),
        notes);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("cpi")),
        cpi);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("cpx")),
        cpx);

    dismissNextMessageBox();
    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);
    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("notes"))),
        notes);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("cpi"))),
        cpi);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("cpx"))),
        cpx);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::
backupReplacementFailurePreservesCacheAndFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray notes("derived notes");
    const QString backupDirectory =
        fixture.backupPath(firstName());
    const QString backupSentinel =
        QDir(backupDirectory).filePath(
            QStringLiteral("sentinel"));
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(backupSentinel, QByteArray("keep backup directory"));
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("notes")),
        notes);

    dismissNextMessageBox();
    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);
    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QVERIFY(QFileInfo(backupDirectory).isDir());
    QCOMPARE(
        readBytes(backupSentinel),
        QByteArray("keep backup directory"));
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("notes"))),
        notes);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::backupStagingFailureRollsBackFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QByteArray notes("derived notes");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("notes")),
        notes);
    setRideCacheRemovalMoveFailurePath(
        fixture.backupPath(firstName()));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("notes"))),
        notes);
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
    QVERIFY(stagedFilesFor(
        fixture.cachePath(
            firstName(), QStringLiteral("notes"))).isEmpty());
}

void TestRideCacheRemoval::sourceArchiveFailureRollsBackFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QByteArray cpi("derived cpi");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("cpi")),
        cpi);
    setRideCacheRemovalMoveFailurePath(
        fixture.activityPath(firstName()));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("cpi"))),
        cpi);
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
    QVERIFY(stagedFilesFor(
        fixture.cachePath(
            firstName(), QStringLiteral("cpi"))).isEmpty());
}

void TestRideCacheRemoval::
sourceMutationBeforeArchiveRollsBackFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray concurrentActivity(
        "concurrently replaced activity");
    const QByteArray previousBackup("previous backup");
    const QByteArray notes("derived notes");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("notes")),
        notes);
    setRideCacheRemovalMoveMutation(
        fixture.activityPath(firstName()),
        concurrentActivity);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        concurrentActivity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("notes"))),
        notes);
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
    QVERIFY(stagedFilesFor(
        fixture.cachePath(
            firstName(), QStringLiteral("notes"))).isEmpty());
}

void TestRideCacheRemoval::unsafeFilenameFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString unsafeName =
        QStringLiteral("../outside.json");
    RideItem *item =
        fixture.addRide(unsafeName, true);
    const QString outsidePath =
        QDir(fixture.temporary.path())
            .filePath(QStringLiteral("outside.json"));
    const QByteArray contents("outside activity");
    writeFixture(outsidePath, contents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(readBytes(outsidePath), contents);
    QVERIFY(!QFileInfo::exists(
        QDir(fixture.temporary.path())
            .filePath(QStringLiteral("outside.json.bak"))));
}

void TestRideCacheRemoval::sourceDirectoryMismatchIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QString alternateDirectory =
        QDir(fixture.temporary.path()).filePath(
            QStringLiteral("alternate-activities"));
    const QString itemSourcePath =
        QDir(alternateDirectory).filePath(firstName());
    const QString namespaceSourcePath =
        fixture.activityPath(firstName());
    const QByteArray itemContents("item source activity");
    const QByteArray namespaceContents("namespace activity");
    item->path = alternateDirectory;
    writeFixture(itemSourcePath, itemContents);
    writeFixture(namespaceSourcePath, namespaceContents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(readBytes(itemSourcePath), itemContents);
    QCOMPARE(readBytes(namespaceSourcePath), namespaceContents);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::duplicateFullIdentityFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *duplicate = fixture.addRide(firstName(), false);
    const QByteArray contents("shared source activity");
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, contents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 2);
    QVERIFY(fixture.cache->rides().contains(target));
    QVERIFY(fixture.cache->rides().contains(duplicate));
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(readBytes(sourcePath), contents);
    QVERIFY(!QFileInfo::exists(fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
plannedAndCompletedBackupsUseSeparateNamespaces()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), false);
    RideItem *planned =
        fixture.addPlannedRide(firstName(), true);
    planned->path =
        fixture.athlete->home->planned().absolutePath();

    const QByteArray completedContents(
        "completed activity");
    const QByteArray plannedContents(
        "planned activity");
    writeFixture(
        fixture.activityPath(firstName()),
        completedContents);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedContents);

    QVERIFY(fixture.cache->removeRide(planned));
    QVERIFY(fixture.cache->removeRide(completed));

    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        completedContents);
    QCOMPARE(
        readBytes(fixture.plannedBackupPath(firstName())),
        plannedContents);
}

void TestRideCacheRemoval::
plannedBackupDirectorySyncFailurePreservesActivity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *planned =
        fixture.addRide(firstName(), true);
    planned->planned = true;
    planned->path =
        fixture.athlete->home->planned().absolutePath();

    const QByteArray contents("planned activity");
    const QString sourcePath =
        fixture.plannedActivityPath(firstName());
    const QString backupDirectory =
        QFileInfo(
            fixture.plannedBackupPath(firstName()))
            .absolutePath();
    QVERIFY(!QFileInfo::exists(backupDirectory));
    writeFixture(sourcePath, contents);
    setRideCacheRemovalSyncFailurePath(
        backupDirectory);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(planned);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, planned);
    QCOMPARE(readBytes(sourcePath), contents);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedBackupPath(firstName())));
}

void TestRideCacheRemoval::
plannedBackupDirectorySyncFailureIsRetried()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *planned =
        fixture.addPlannedRide(firstName(), true);
    const QString sourcePath =
        fixture.plannedActivityPath(firstName());
    const QString backupPath =
        fixture.plannedBackupPath(firstName());
    const QString backupDirectory =
        QFileInfo(backupPath).absolutePath();
    writeFixture(sourcePath, QByteArray("planned activity"));
    setRideCacheRemovalSyncFailureCount(
        backupDirectory, 2);

    const RideCache::RemovalResult first =
        fixture.cache->removeRideResult(planned);
    QCOMPARE(
        first.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(readBytes(sourcePath), QByteArray("planned activity"));
    QVERIFY(!QFileInfo::exists(backupPath));

    const RideCache::RemovalResult second =
        fixture.cache->removeRideResult(planned);
    QCOMPARE(
        second.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(readBytes(sourcePath), QByteArray("planned activity"));
    QVERIFY(!QFileInfo::exists(backupPath));

    const RideCache::RemovalResult third =
        fixture.cache->removeRideResult(planned);
    QCOMPARE(
        third.status,
        RideCache::RemovalStatus::Committed);
    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(sourcePath));
    QCOMPARE(readBytes(backupPath), QByteArray("planned activity"));
}

void TestRideCacheRemoval::plannedBackupSymlinkIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *planned =
        fixture.addRide(firstName(), true);
    planned->planned = true;
    planned->path =
        fixture.athlete->home->planned().absolutePath();

    const QByteArray contents("planned activity");
    const QString sourcePath =
        fixture.plannedActivityPath(firstName());
    const QString backupDirectory =
        QFileInfo(
            fixture.plannedBackupPath(firstName()))
            .absolutePath();
    const QString outsideDirectory =
        QDir(fixture.temporary.path()).filePath(
            QStringLiteral("outside-backups"));
    QVERIFY(QDir().mkpath(outsideDirectory));
    if (!QFile::link(
            outsideDirectory, backupDirectory)) {
        QSKIP("Directory symbolic links are unavailable");
    }
    writeFixture(sourcePath, contents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(planned);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, planned);
    QCOMPARE(readBytes(sourcePath), contents);
    QVERIFY(!QFileInfo::exists(
        QDir(outsideDirectory).filePath(
            firstName() + QStringLiteral(".bak"))));
}

void TestRideCacheRemoval::linkedSaveFailureRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QByteArray targetContents(
        "target activity");
    const QByteArray linkedContents(
        "linked activity");
    writeFixture(
        fixture.activityPath(firstName()),
        targetContents);
    writeFixture(
        fixture.plannedActivityPath(secondName()),
        linkedContents);
    setRideCacheRemovalSaveFailureFileName(
        secondName());

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        targetContents);
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        linkedContents);
    QCOMPARE(rideCacheRemovalSaveCallCount(), 2);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
linkedArchivedRemovalRejectsBeforePeerMutation()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *peer =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    peer->setLinkedFileName(firstName());

    const QByteArray targetContents("target activity");
    const QByteArray peerContents("peer activity");
    const QString targetPath =
        fixture.activityPath(firstName());
    const QString peerPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, targetContents);
    writeFixture(peerPath, peerContents);

    const RideCache::RemovalResult result =
        fixture.cache->removeArchivedRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(result.error.contains(
        QStringLiteral("already been archived")));
    QCOMPARE(rideCacheRemovalSaveCallCount(), 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(target->getLinkedFileName(), secondName());
    QCOMPARE(peer->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), targetContents);
    QCOMPARE(readBytes(peerPath), peerContents);
}

void TestRideCacheRemoval::
unsafeLinkedPeerIdentityRejectsBeforeSave_data()
{
    QTest::addColumn<QString>("identityCase");

    QTest::newRow("unsafe-name")
        << QStringLiteral("unsafe-name");
    QTest::newRow("outside-path")
        << QStringLiteral("outside-path");
    QTest::newRow("symlink-source")
        << QStringLiteral("symlink-source");
    QTest::newRow("non-regular-source")
        << QStringLiteral("non-regular-source");
    QTest::newRow("wrong-namespace")
        << QStringLiteral("wrong-namespace");
}

void TestRideCacheRemoval::
unsafeLinkedPeerIdentityRejectsBeforeSave()
{
    QFETCH(QString, identityCase);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);

    const QByteArray targetContents("target activity");
    const QByteArray sentinelContents("linked sentinel");
    const QString targetPath =
        fixture.activityPath(firstName());
    QString linkedSourcePath =
        fixture.plannedActivityPath(secondName());
    QString sentinelPath = linkedSourcePath;

    if (identityCase == QStringLiteral("unsafe-name")) {
        linked->fileName = QStringLiteral("../outside-linked.json");
        linkedSourcePath = QDir(linked->path).filePath(linked->fileName);
        sentinelPath = QDir::cleanPath(linkedSourcePath);
        writeFixture(sentinelPath, sentinelContents);
    } else if (identityCase == QStringLiteral("outside-path")) {
        linked->path = QDir(fixture.temporary.path()).filePath(
            QStringLiteral("outside"));
        linkedSourcePath = QDir(linked->path).filePath(linked->fileName);
        sentinelPath = linkedSourcePath;
        writeFixture(sentinelPath, sentinelContents);
    } else if (identityCase == QStringLiteral("symlink-source")) {
        sentinelPath = QDir(fixture.temporary.path()).filePath(
            QStringLiteral("outside/symlink-sentinel.json"));
        writeFixture(sentinelPath, sentinelContents);
        QVERIFY(QFile::link(sentinelPath, linkedSourcePath));
        QVERIFY(QFileInfo(linkedSourcePath).isSymLink());
    } else if (identityCase
               == QStringLiteral("non-regular-source")) {
        QVERIFY(QDir().mkpath(linkedSourcePath));
        sentinelPath = QDir(linkedSourcePath).filePath(
            QStringLiteral("sentinel"));
        writeFixture(sentinelPath, sentinelContents);
    } else {
        linked->path = fixture.athlete->home
            ->activities().absolutePath();
        linkedSourcePath =
            fixture.activityPath(secondName());
        sentinelPath = linkedSourcePath;
        writeFixture(sentinelPath, sentinelContents);
    }

    target->setLinkedFileName(linked->fileName);
    linked->setLinkedFileName(firstName());
    writeFixture(targetPath, targetContents);
    setRideCacheRemovalSaveFailureFileName(linked->fileName);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(rideCacheRemovalSaveCallCount(), 0);
    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(target->getLinkedFileName(), linked->fileName);
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), targetContents);
    QCOMPARE(readBytes(sentinelPath), sentinelContents);
    QVERIFY(!QFileInfo::exists(fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
incomingReferenceToLinkedPeerRejectsBeforeSave()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    RideItem *incoming = fixture.addRide(thirdName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());
    incoming->setLinkedFileName(secondName());

    const QByteArray targetContents("target activity");
    const QByteArray linkedContents("linked activity");
    const QByteArray incomingContents("incoming activity");
    writeFixture(fixture.activityPath(firstName()), targetContents);
    writeFixture(
        fixture.plannedActivityPath(secondName()), linkedContents);
    writeFixture(fixture.activityPath(thirdName()), incomingContents);
    setRideCacheRemovalSaveFailureFileName(secondName());

    const RideCache::OperationPreCheck check =
        fixture.cache->checkRemovalLinks(target);
    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QVERIFY(!check.canProceed);
    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(rideCacheRemovalSaveCallCount(), 0);
    QCOMPARE(fixture.cache->count(), 3);
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(incoming->getLinkedFileName(), secondName());
    QCOMPARE(readBytes(fixture.activityPath(firstName())), targetContents);
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        linkedContents);
    QCOMPARE(readBytes(fixture.activityPath(thirdName())), incomingContents);
}

void TestRideCacheRemoval::
linkedSaveRenameIsRejectedBeforeMutation()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QByteArray targetContents("target activity");
    const QByteArray linkedContents("linked activity");
    const QString targetPath =
        fixture.activityPath(firstName());
    const QString oldLinkedPath =
        fixture.plannedActivityPath(secondName());
    const QString newLinkedPath =
        fixture.plannedActivityPath(thirdName());
    const QString linkedDirectory =
        QFileInfo(newLinkedPath).absolutePath();
    writeFixture(targetPath, targetContents);
    writeFixture(oldLinkedPath, linkedContents);
    setRideCacheRemovalSuccessfulSaveRename(
        secondName(), linkedDirectory, thirdName());

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(result.error.contains(
        QStringLiteral("correctly named JSON")));
    QVERIFY(result.recoveryPaths.isEmpty());
    QCOMPARE(rideCacheRemovalSaveCallCount(), 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(linked->fileName, secondName());
    QCOMPARE(linked->path,
             QFileInfo(oldLinkedPath).absolutePath());
    QVERIFY(linked->planned);
    QCOMPARE(target->getLinkedFileName(), secondName());
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), targetContents);
    QCOMPARE(readBytes(oldLinkedPath), linkedContents);
    QVERIFY(!QFileInfo::exists(newLinkedPath));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
linkedSaveCompensationFailureRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    setRideCacheRemovalSaveFailureCalls(
        secondName(), QSet<int>{1, 2});

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
    QCOMPARE(rideCacheRemovalSaveCallCount(), 2);
    QVERIFY(result.recoveryPaths.contains(linkedPath));
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
}

void TestRideCacheRemoval::
linkedPeerDeletionDuringMetadataChangeRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    setRideCacheRemovalLinkMutationActionOnCall(
        1,
        [&fixture, linked] {
            fixture.cache->mutableRidesForRemovalTest().removeOne(linked);
            delete linked;
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), target);
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
    QVERIFY(result.recoveryPaths.contains(linkedPath));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
linkedPeerDeletionDuringCompensationRequiresRecovery_data()
{
    QTest::addColumn<bool>("storageFailure");

    QTest::newRow("initial-save-compensation") << false;
    QTest::newRow("storage-rollback-compensation") << true;
}

void TestRideCacheRemoval::
linkedPeerDeletionDuringCompensationRequiresRecovery()
{
    QFETCH(bool, storageFailure);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    if (storageFailure) {
        setRideCacheRemovalMoveFailurePath(targetPath);
    } else {
        setRideCacheRemovalSaveFailureOnCall(
            secondName(), 1);
    }
    setRideCacheRemovalLinkMutationActionOnCall(
        2,
        [&fixture, linked] {
            fixture.cache->mutableRidesForRemovalTest().removeOne(linked);
            delete linked;
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), target);
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
    QVERIFY(result.recoveryPaths.contains(linkedPath));
}

void TestRideCacheRemoval::
targetDeletionDuringLinkedMetadataChangeRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    setRideCacheRemovalLinkMutationActionOnCall(
        1,
        [&fixture, target] {
            fixture.cache->mutableRidesForRemovalTest().removeOne(target);
            delete target;
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), linked);
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
    QVERIFY(result.recoveryPaths.contains(targetPath));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
targetMutationDuringStorageRollbackRequiresRecovery_data()
{
    QTest::addColumn<bool>("mutateInProcessor");

    QTest::newRow("delete-processor") << true;
    QTest::newRow("link-compensation") << false;
}

void TestRideCacheRemoval::
targetMutationDuringStorageRollbackRequiresRecovery()
{
    QFETCH(bool, mutateInProcessor);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide = mutateInProcessor
        ? reinterpret_cast<RideFile *>(quintptr(1))
        : nullptr;
    RideItem *target =
        fixture.addRide(firstName(), true, loadedRide);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));

    const auto mutateTarget = [target] {
        target->fileName = thirdName();
    };
    if (mutateInProcessor) {
        setRideCacheRemovalProcessorAction(mutateTarget);
    } else {
        setRideCacheRemovalMoveFailurePath(targetPath);
        setRideCacheRemovalLinkMutationActionOnCall(
            2, mutateTarget);
    }

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(target->fileName, thirdName());
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
    QVERIFY(result.recoveryPaths.contains(targetPath));
    QVERIFY(result.recoveryPaths.contains(linkedPath));
}

void TestRideCacheRemoval::
linkedRenameBeforeStorageRollbackRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString oldLinkedPath =
        fixture.plannedActivityPath(secondName());
    const QString newLinkedPath =
        fixture.plannedActivityPath(thirdName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(oldLinkedPath, QByteArray("linked activity"));
    setRideCacheRemovalSaveActionOnCall(
        secondName(), 1,
        [linked, oldLinkedPath, newLinkedPath] {
            QVERIFY(QFile::rename(
                oldLinkedPath, newLinkedPath));
            linked->fileName = thirdName();
        });
    setRideCacheRemovalMoveFailurePath(targetPath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(oldLinkedPath), QByteArray("linked activity"));
    QCOMPARE(readBytes(newLinkedPath), QByteArray("linked activity"));
    QVERIFY(result.recoveryPaths.contains(targetPath));
    QVERIFY(result.recoveryPaths.contains(newLinkedPath));
}

void TestRideCacheRemoval::linkedReplacementDuringClearIsRestored()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    setRideCacheRemovalLinkMutationActionOnCall(
        1,
        [linked] {
            linked->setLinkedFileName(thirdName());
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(target->getLinkedFileName(), secondName());
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(linkedPath), QByteArray("linked activity"));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::dirtyLinkedPeerRequiresPreflight()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());
    linked->isdirty = true;

    const QByteArray targetContents("target activity");
    const QByteArray linkedContents("linked activity");
    writeFixture(
        fixture.activityPath(firstName()),
        targetContents);
    writeFixture(
        fixture.plannedActivityPath(secondName()),
        linkedContents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        targetContents);
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        linkedContents);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
incomingLinkedReferenceRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *incoming =
        fixture.addPlannedRide(secondName(), false);
    incoming->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString incomingPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(incomingPath, QByteArray("incoming activity"));

    const RideCache::OperationPreCheck check =
        fixture.cache->checkRemovalLinks(target);
    QVERIFY(!check.canProceed);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        incoming->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(incomingPath), QByteArray("incoming activity"));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
sameNamespaceIncomingLinkRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *incoming =
        fixture.addRide(secondName(), false);
    incoming->setLinkedFileName(firstName());

    const QString targetPath =
        fixture.activityPath(firstName());
    const QString incomingPath =
        fixture.activityPath(secondName());
    writeFixture(targetPath, QByteArray("target activity"));
    writeFixture(incomingPath, QByteArray("incoming activity"));

    const RideCache::OperationPreCheck check =
        fixture.cache->checkRemovalLinks(target);
    QVERIFY(!check.canProceed);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        incoming->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(targetPath), QByteArray("target activity"));
    QCOMPARE(readBytes(incomingPath), QByteArray("incoming activity"));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
removalLinkPreflightAllowsUnlinkedActivity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item =
        fixture.addRide(firstName(), true);

    const RideCache::OperationPreCheck check =
        fixture.cache->checkRemovalLinks(item);

    QVERIFY(check.canProceed);
    QVERIFY(!check.requiresUserDecision);
    QCOMPARE(
        check.affectedItems,
        QList<RideItem*>({item}));
    QVERIFY(check.dirtyItems.isEmpty());
}

void TestRideCacheRemoval::brokenLinkedPairRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target =
        fixture.addRide(
            firstName(), true, loadedRide);
    target->setLinkedFileName(secondName());
    const QByteArray contents("target activity");
    writeFixture(
        fixture.activityPath(firstName()),
        contents);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        contents);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
    QVERIFY(
        rideCacheRemovalLastProcessedRide()
        == nullptr);
}

void TestRideCacheRemoval::
dataProcessorReceivesLoadedRideBeforeArchive()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    fixture.addRide(
        firstName(), true, loadedRide);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("activity"));

    QVERIFY(fixture.cache->removeRide(firstName()));

    QVERIFY(
        rideCacheRemovalLastProcessedRide()
        == loadedRide);
}

void TestRideCacheRemoval::
linkedRideClosedDuringMetadataChangeIsNotReused()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const originallyLoadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target = fixture.addRide(
        firstName(), true, originallyLoadedRide);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    writeFixture(
        fixture.plannedActivityPath(secondName()),
        QByteArray("linked activity"));
    setRideCacheRemovalLinkMutationActionOnCall(
        1, [target] { target->close(); });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(
        rideCacheRemovalLastProcessedRide()
        != originallyLoadedRide);
}

void TestRideCacheRemoval::sourceSymlinkIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item =
        fixture.addRide(firstName(), true);
    const QString outsidePath =
        QDir(fixture.temporary.path())
            .filePath(QStringLiteral("outside-source"));
    const QByteArray contents("outside contents");
    writeFixture(outsidePath, contents);
    if (!QFile::link(
            outsidePath,
            fixture.activityPath(firstName()))) {
        QSKIP("Symbolic links are unavailable");
    }

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(QFileInfo(
        fixture.activityPath(firstName())).isSymLink());
    QCOMPARE(readBytes(outsidePath), contents);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::lockedSourceIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item =
        fixture.addRide(firstName(), true);
    const QByteArray contents("activity");
    const QString sourcePath =
        fixture.activityPath(firstName());
    writeFixture(sourcePath, contents);
    QLockFile lock(atomicFileLockPath(sourcePath));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), contents);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
derivedFilesAreNotMovedBeforeCommit_data()
{
    QTest::addColumn<QString>("failingExtension");

    QTest::newRow("cpx") << QStringLiteral("cpx");
    QTest::newRow("cpi") << QStringLiteral("cpi");
    QTest::newRow("notes") << QStringLiteral("notes");
}

void TestRideCacheRemoval::
derivedFilesAreNotMovedBeforeCommit()
{
    QFETCH(QString, failingExtension);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);

    const QStringList extensions = {
        QStringLiteral("cpx"),
        QStringLiteral("cpi"),
        QStringLiteral("notes")
    };
    for (const QString &extension : extensions) {
        writeFixture(
            fixture.cachePath(firstName(), extension),
            extension.toLatin1() + " contents");
    }
    setRideCacheRemovalMoveFailurePath(
        fixture.cachePath(
            firstName(), failingExtension));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY2(
        result.cleanlyCompleted(),
        qPrintable(result.error));
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        activity);
    for (const QString &extension : extensions) {
        const QString path =
            fixture.cachePath(firstName(), extension);
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(stagedFilesFor(path).isEmpty());
    }
    Q_UNUSED(item);
    Q_UNUSED(previousBackup);
}

void TestRideCacheRemoval::
derivedMutationBeforeStagingRollsBackFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QByteArray notes("derived notes");
    const QByteArray concurrentNotes(
        "concurrently replaced notes");
    const QString notesPath =
        fixture.cachePath(
            firstName(), QStringLiteral("notes"));
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(notesPath, notes);
    setRideCacheRemovalMoveMutation(
        notesPath, concurrentNotes);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(readBytes(notesPath), concurrentNotes);
    QVERIFY(stagedFilesFor(notesPath).isEmpty());
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
}

void TestRideCacheRemoval::
directorySyncFailureRollsBackFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    setRideCacheRemovalSyncFailurePath(
        fixture.activityPath(firstName()));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
}

void TestRideCacheRemoval::rollbackFailureRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QString sourcePath =
        fixture.activityPath(firstName());
    writeFixture(sourcePath, activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    setRideCacheRemovalSyncFailurePath(sourcePath);
    setRideCacheRemovalMoveFailureTargetPath(
        sourcePath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, item);
    QVERIFY(!QFileInfo::exists(sourcePath));
    const QStringList sourceRecovery =
        stagedFilesFor(sourcePath);
    QCOMPARE(sourceRecovery.size(), 1);
    QCOMPARE(
        readBytes(sourceRecovery.constFirst()),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        activity);
    const QStringList previousRecovery =
        recoveryFilesFor(
            fixture.backupPath(firstName()),
            QStringLiteral(".gc-previous-"));
    QCOMPARE(previousRecovery.size(), 1);
    QCOMPARE(
        readBytes(previousRecovery.constFirst()),
        previousBackup);
    QVERIFY(result.recoveryPaths.contains(
        sourceRecovery.constFirst()));
    QVERIFY(result.recoveryPaths.contains(
        fixture.backupPath(firstName())));
    QVERIFY(result.recoveryPaths.contains(
        previousRecovery.constFirst()));
}

void TestRideCacheRemoval::
sourceRestoreSyncFailureRetainsPublishedBackup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(sourcePath, activity);
    writeFixture(backupPath, previousBackup);
    setRideCacheRemovalSyncFailureCount(
        sourcePath, 2);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), activity);
    const QStringList previousRecovery =
        recoveryFilesFor(
            backupPath,
            QStringLiteral(".gc-previous-"));
    QCOMPARE(previousRecovery.size(), 1);
    QCOMPARE(
        readBytes(previousRecovery.constFirst()),
        previousBackup);
    QVERIFY(result.recoveryPaths.contains(backupPath));
    QVERIFY(result.recoveryPaths.contains(
        previousRecovery.constFirst()));
}

void TestRideCacheRemoval::
failedPreviousBackupMovePartialStateRollsBack_data()
{
    QTest::addColumn<bool>("removeSource");

    QTest::newRow("target-created") << false;
    QTest::newRow("target-created-source-removed") << true;
}

void TestRideCacheRemoval::
failedPreviousBackupMovePartialStateRollsBack()
{
    QFETCH(bool, removeSource);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(sourcePath, activity);
    writeFixture(backupPath, previousBackup);
    setRideCacheRemovalPartialMoveFailurePath(
        backupPath, removeSource);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RolledBack);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), previousBackup);
    QVERIFY(recoveryFilesFor(
        backupPath,
        QStringLiteral(".gc-previous-")).isEmpty());
    QVERIFY(result.recoveryPaths.isEmpty());
}

void TestRideCacheRemoval::
failedBackupPublishPartialMoveRollsBack_data()
{
    QTest::addColumn<bool>("removeSource");

    QTest::newRow("target-created") << false;
    QTest::newRow("target-created-source-removed") << true;
}

void TestRideCacheRemoval::
failedBackupPublishPartialMoveRollsBack()
{
    QFETCH(bool, removeSource);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(sourcePath, activity);
    writeFixture(backupPath, previousBackup);
    setRideCacheRemovalPartialMoveFailureTargetPath(
        backupPath, removeSource);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RolledBack);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), previousBackup);
    QVERIFY(recoveryFilesFor(
        backupPath,
        QStringLiteral(".gc-previous-")).isEmpty());
    QVERIFY(result.recoveryPaths.isEmpty());
}

void TestRideCacheRemoval::
failedSourceMovePartialStateRollsBack_data()
{
    QTest::addColumn<bool>("removeSource");

    QTest::newRow("target-created") << false;
    QTest::newRow("target-created-source-removed") << true;
}

void TestRideCacheRemoval::
failedSourceMovePartialStateRollsBack()
{
    QFETCH(bool, removeSource);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(sourcePath, activity);
    writeFixture(backupPath, previousBackup);
    setRideCacheRemovalPartialMoveFailurePath(
        sourcePath, removeSource);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RolledBack);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), previousBackup);
    QVERIFY(stagedFilesFor(sourcePath).isEmpty());
    QVERIFY(recoveryFilesFor(
        backupPath,
        QStringLiteral(".gc-previous-")).isEmpty());
    QVERIFY(result.recoveryPaths.isEmpty());
}

void TestRideCacheRemoval::batchStopsAfterRecoveryRequired()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first =
        fixture.addRide(firstName(), true);
    RideItem *second =
        fixture.addRide(secondName(), false);
    const QString firstPath =
        fixture.activityPath(firstName());
    const QString secondPath =
        fixture.activityPath(secondName());
    writeFixture(firstPath, QByteArray("first activity"));
    writeFixture(secondPath, QByteArray("second activity"));
    setRideCacheRemovalSyncFailurePath(firstPath);
    setRideCacheRemovalMoveFailureTargetPath(firstPath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{first, second});

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(result.requestedCount, 2);
    QCOMPARE(result.items.size(), 2);
    QCOMPARE(
        result.items.at(0).status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(
        result.items.at(1).status,
        RideCache::RemovalStatus::NotAttempted);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(readBytes(secondPath), QByteArray("second activity"));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(secondName())));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::linkedRecoveryRequiredRestoresPair()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString sourcePath =
        fixture.activityPath(firstName());
    writeFixture(sourcePath, QByteArray("target activity"));
    writeFixture(
        fixture.plannedActivityPath(secondName()),
        QByteArray("linked activity"));
    setRideCacheRemovalSyncFailurePath(sourcePath);
    setRideCacheRemovalMoveFailureTargetPath(sourcePath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
}

void TestRideCacheRemoval::
linkedRollbackSaveFailureReportsRecoveryPath()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("target activity"));
    writeFixture(linkedPath, QByteArray("linked activity"));
    setRideCacheRemovalSaveFailureOnCall(
        secondName(), 2);
    setRideCacheRemovalSyncFailurePath(sourcePath);
    setRideCacheRemovalMoveFailureTargetPath(sourcePath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        target->getLinkedFileName(), secondName());
    QCOMPARE(
        linked->getLinkedFileName(), firstName());
    QVERIFY(result.recoveryPaths.contains(linkedPath));
}

void TestRideCacheRemoval::
linkedRollbackPrecedesCompensationSerialization()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *linked =
        fixture.addPlannedRide(secondName(), false);
    target->setLinkedFileName(secondName());
    linked->setLinkedFileName(firstName());

    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString linkedPath =
        fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("target-linked"));
    writeFixture(linkedPath, QByteArray("peer-linked"));
    setRideCacheRemovalPersistedSaveContents(
        secondName(), QByteArray("peer-unlinked"));
    setRideCacheRemovalPersistedSaveContentsOnCall(
        secondName(), 2,
        QByteArray("peer-linked-with-save-history"));
    setRideCacheRemovalMoveFailurePath(sourcePath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RolledBack);
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(result.recoveryPaths.isEmpty());
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(target->getLinkedFileName(), secondName());
    QCOMPARE(linked->getLinkedFileName(), firstName());
    QCOMPARE(readBytes(sourcePath), QByteArray("target-linked"));
    QCOMPARE(
        readBytes(linkedPath),
        QByteArray("peer-linked-with-save-history"));
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
concurrentLinkedRemovalJournalsAreSerialized()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath =
        fixture.activityPath(firstName());
    const QString peerPath =
        fixture.plannedActivityPath(secondName());
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    const LinkedActivityRemoval::Specification specification{
        fixture.athlete->home->root().absolutePath(),
        sourcePath,
        backupPath,
        peerPath,
        {}};
    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> first =
        LinkedActivityRemoval::Journal::prepare(
            specification, error);
    QVERIFY2(first, qPrintable(error));

    error.clear();
    const std::shared_ptr<LinkedActivityRemoval::Journal> concurrent =
        LinkedActivityRemoval::Journal::prepare(
            specification, error);
    QVERIFY(!concurrent);
    QVERIFY(error.contains(
        QStringLiteral("already"),
        Qt::CaseInsensitive));

    error.clear();
    QVERIFY2(first->cleanupAfterRollback(error), qPrintable(error));
    first.reset();

    error.clear();
    const std::shared_ptr<LinkedActivityRemoval::Journal> next =
        LinkedActivityRemoval::Journal::prepare(
            specification, error);
    QVERIFY2(next, qPrintable(error));
    QVERIFY2(next->cleanupAfterRollback(error), qPrintable(error));
}

void TestRideCacheRemoval::
activeLinkedSaveAndRemovalShareAthleteLease()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));
    LinkedActivitySave::Specification saveSpecification;
    saveSpecification.athleteRoot = fixture.temporary.path();
    saveSpecification.entries = {
        {sourcePath,
         fixture.activityPath(thirdName()),
         sourcePath + QStringLiteral(".save-bak"),
         false},
        {peerPath,
         fixture.plannedActivityPath(thirdName()),
         peerPath + QStringLiteral(".save-bak"),
         false}};
    const LinkedActivityRemoval::Specification removalSpecification{
        fixture.athlete->home->root().absolutePath(),
        sourcePath,
        fixture.backupPath(firstName()),
        peerPath,
        {}};

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> save =
        LinkedActivitySave::Journal::prepare(
            saveSpecification, error);
    QVERIFY2(save, qPrintable(error));

    error.clear();
    const std::shared_ptr<LinkedActivityRemoval::Journal> blockedRemoval =
        LinkedActivityRemoval::Journal::prepare(
            removalSpecification, error);
    QVERIFY(!blockedRemoval);
    QVERIFY2(
        error.contains(QStringLiteral("already"), Qt::CaseInsensitive),
        qPrintable(error));
    error.clear();
    QVERIFY2(save->cleanupAfterRollback(error), qPrintable(error));
    save.reset();

    std::shared_ptr<LinkedActivityRemoval::Journal> removal =
        LinkedActivityRemoval::Journal::prepare(
            removalSpecification, error);
    QVERIFY2(removal, qPrintable(error));

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> blockedSave =
        LinkedActivitySave::Journal::prepare(
            saveSpecification, error);
    QVERIFY(!blockedSave);
    QVERIFY2(
        error.contains(QStringLiteral("already"), Qt::CaseInsensitive),
        qPrintable(error));
    error.clear();
    QVERIFY2(removal->cleanupAfterRollback(error), qPrintable(error));
}

void TestRideCacheRemoval::
abandonedLinkedRemovalJournalBlocksNextTransaction()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QString backupPath = fixture.backupPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    const LinkedActivityRemoval::Specification specification{
        fixture.athlete->home->root().absolutePath(),
        sourcePath,
        backupPath,
        peerPath,
        {}};
    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> abandoned =
        LinkedActivityRemoval::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString abandonedPath = abandoned->directoryPath();
    abandoned.reset();
    QVERIFY(QFileInfo::exists(abandonedPath));

    error.clear();
    const std::shared_ptr<LinkedActivityRemoval::Journal> blocked =
        LinkedActivityRemoval::Journal::prepare(specification, error);
    QVERIFY(!blocked);
    QVERIFY2(
        error.contains(QStringLiteral("recovery"), Qt::CaseInsensitive),
        qPrintable(error));

    error.clear();
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(
            fixture.athlete->home->root().absolutePath(), error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(abandonedPath));

    error.clear();
    const std::shared_ptr<LinkedActivityRemoval::Journal> next =
        LinkedActivityRemoval::Journal::prepare(specification, error);
    QVERIFY2(next, qPrintable(error));
    QVERIFY2(next->cleanupAfterRollback(error), qPrintable(error));
}

void TestRideCacheRemoval::
pendingLinkedSaveJournalBlocksDeletionTransaction()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    const QString pending = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions/linked-save/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    QVERIFY(QDir().mkpath(pending));

    const LinkedActivityRemoval::Specification specification = {
        fixture.temporary.path(),
        sourcePath,
        fixture.backupPath(firstName()),
        peerPath,
        {}};
    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(specification, error);
    QVERIFY(!journal);
    QVERIFY2(
        error.contains(QStringLiteral("recovery"), Qt::CaseInsensitive),
        qPrintable(error));
}

void TestRideCacheRemoval::
startupReconcilesAbandonedLinkedSaveJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::unique_ptr<Context> context(new Context(nullptr));
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), QDir(temporary.path())));

    const QString firstPath = athlete->home->activities().filePath(
        firstName());
    const QString secondPath = athlete->home->planned().filePath(
        secondName());
    writeFixture(firstPath, QByteArray("first-old"));
    writeFixture(secondPath, QByteArray("second-old"));
    LinkedActivitySave::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.entries = {
        {firstPath,
         athlete->home->activities().filePath(thirdName()),
         firstPath + QStringLiteral(".bak"),
         false},
        {secondPath,
         athlete->home->planned().filePath(thirdName()),
         secondPath + QStringLiteral(".bak"),
         false}};
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> abandoned =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString journalPath = abandoned->directoryPath();
    abandoned.reset();
    QVERIFY(QFileInfo::exists(journalPath));

    std::unique_ptr<RideCache> cache(new RideCache(context.get()));
    athlete->rideCache = cache.get();
    QVERIFY2(
        cache->startupRecoveryError().isEmpty(),
        qPrintable(cache->startupRecoveryError()));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(readBytes(firstPath), QByteArray("first-old"));
    QCOMPARE(readBytes(secondPath), QByteArray("second-old"));
}

void TestRideCacheRemoval::
startupReportsCorruptLinkedSaveJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::unique_ptr<Context> context(new Context(nullptr));
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), QDir(temporary.path())));

    const QString firstPath = athlete->home->activities().filePath(
        firstName());
    const QString secondPath = athlete->home->planned().filePath(
        secondName());
    writeFixture(firstPath, QByteArray("first-old"));
    writeFixture(secondPath, QByteArray("second-old"));
    LinkedActivitySave::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.entries = {
        {firstPath,
         athlete->home->activities().filePath(thirdName()),
         firstPath + QStringLiteral(".bak"),
         false},
        {secondPath,
         athlete->home->planned().filePath(thirdName()),
         secondPath + QStringLiteral(".bak"),
         false}};
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> abandoned =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString journalPath = abandoned->directoryPath();
    abandoned.reset();
    writeFixture(
        QDir(journalPath).filePath(QStringLiteral("manifest.json")),
        QByteArray("{}\n"));

    std::unique_ptr<RideCache> cache(new RideCache(context.get()));
    athlete->rideCache = cache.get();
    QVERIFY2(
        !cache->startupRecoveryError().isEmpty(),
        "Corrupt linked-save recovery must stop athlete startup");
    QVERIFY2(
        cache->startupRecoveryError().contains(
            QStringLiteral("journal"), Qt::CaseInsensitive),
        qPrintable(cache->startupRecoveryError()));
    QVERIFY(QFileInfo::exists(journalPath));
    QCOMPARE(readBytes(firstPath), QByteArray("first-old"));
    QCOMPARE(readBytes(secondPath), QByteArray("second-old"));
}

void TestRideCacheRemoval::
prepareRestrictsExistingTransactionDirectories()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    const QString transactionsPath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions"));
    const QString namespacePath = QDir(transactionsPath).filePath(
        QStringLiteral("linked-removal"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QFileDevice::Permissions broadPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner | QFileDevice::ReadGroup
        | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    QVERIFY(QFile::setPermissions(transactionsPath, broadPermissions));
    QVERIFY(QFile::setPermissions(namespacePath, broadPermissions));

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther
        | QFileDevice::WriteOther | QFileDevice::ExeOther;
    for (const QString &path :
         {transactionsPath, namespacePath, journal->directoryPath()}) {
        const QFileDevice::Permissions permissions =
            QFileInfo(path).permissions();
        QCOMPARE(
            permissions & nonOwnerPermissions,
            QFileDevice::Permissions());
        QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(permissions.testFlag(QFileDevice::ExeOwner));
    }

    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
#endif
}

void TestRideCacheRemoval::
startupRestrictsExistingTransactionDirectories()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    journal.reset();

    const QString transactionsPath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions"));
    const QString namespacePath = QDir(transactionsPath).filePath(
        QStringLiteral("linked-removal"));
    const QFileDevice::Permissions broadPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner | QFileDevice::ReadGroup
        | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    for (const QString &path :
         {transactionsPath, namespacePath, journalPath}) {
        QVERIFY(QFile::setPermissions(path, broadPermissions));
    }
    writeFixture(
        QDir(journalPath).filePath(QStringLiteral("manifest.json")),
        QByteArray("{}\n"));

    error.clear();
    QVERIFY(!LinkedActivityRemoval::Journal::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY(!error.isEmpty());

    const QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther
        | QFileDevice::WriteOther | QFileDevice::ExeOther;
    for (const QString &path :
         {transactionsPath, namespacePath, journalPath}) {
        const QFileDevice::Permissions permissions =
            QFileInfo(path).permissions();
        QCOMPARE(
            permissions & nonOwnerPermissions,
            QFileDevice::Permissions());
        QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(permissions.testFlag(QFileDevice::ExeOwner));
    }
#endif
}

void TestRideCacheRemoval::
oversizedUnreadableJournalControlFileFailsBeforeRead_data()
{
    QTest::addColumn<QString>("relativePath");
    QTest::addColumn<bool>("removeManifest");

    QTest::newRow("manifest")
        << QStringLiteral("manifest.json") << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("COMMITTED") << false;
    QTest::newRow("manifest-temporary")
        << QStringLiteral(".manifest.json.attack.tmp") << false;
    QTest::newRow("pre-manifest-commit-marker")
        << QStringLiteral("COMMITTED") << true;
    QTest::newRow("pre-manifest-temporary")
        << QStringLiteral(".manifest.json.attack.tmp") << true;
}

void TestRideCacheRemoval::
oversizedUnreadableJournalControlFileFailsBeforeRead()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix file permissions are required");
#else
    QFETCH(QString, relativePath);
    QFETCH(bool, removeManifest);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    writeFixture(sourcePath, QByteArray("source"));
    writeFixture(peerPath, QByteArray("peer"));

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString controlPath = QDir(journal->directoryPath()).filePath(
        relativePath);
    const QString manifestPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral("manifest.json"));
    journal.reset();
    if (removeManifest) QVERIFY(QFile::remove(manifestPath));

    QFile control(controlPath);
    QVERIFY2(
        control.open(QIODevice::WriteOnly | QIODevice::Truncate),
        qPrintable(control.errorString()));
    QVERIFY(control.resize(4 * 1024 * 1024 + 1));
    control.close();
    QVERIFY(QFile::setPermissions(
        controlPath, QFileDevice::Permissions()));

    QFile readabilityProbe(controlPath);
    if (readabilityProbe.open(QIODevice::ReadOnly)) {
        readabilityProbe.close();
        QFile::setPermissions(
            controlPath,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QSKIP("The test process can bypass Unix file permissions");
    }

    error.clear();
    QVERIFY(!LinkedActivityRemoval::Journal::reconcileAll(
        fixture.temporary.path(), error));
    QVERIFY2(
        error.contains(QStringLiteral("unexpectedly large")),
        qPrintable(error));

    QFile::setPermissions(
        controlPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

void TestRideCacheRemoval::
startupWithoutJournalAllowsSymlinkedAthleteRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString actualRoot =
        QDir(temporary.path()).filePath(
            QStringLiteral("actual-athlete"));
    const QString linkedRoot =
        QDir(temporary.path()).filePath(
            QStringLiteral("linked-athlete"));
    QVERIFY(QDir().mkpath(actualRoot));
    if (!QFile::link(actualRoot, linkedRoot)) {
        QSKIP("Directory symbolic links are unavailable");
    }

    QString error;
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(
            linkedRoot, error),
        qPrintable(error));
}

void TestRideCacheRemoval::
linkedDeletionCrashAfterPeerSaveRecoversOnRestart_data()
{
    QTest::addColumn<QString>("crashPhase");
    QTest::addColumn<int>("crashOccurrence");
    QTest::addColumn<QString>("recoveryCrashPhase");
    QTest::addColumn<bool>("committed");

    QTest::newRow("journal-directory-created")
        << QStringLiteral("journal-directory-created")
        << 1 << QString() << false;
    QTest::newRow("journal-peer-old-published")
        << QStringLiteral("journal-peer-old-published")
        << 1 << QString() << false;
    QTest::newRow("journal-initial-manifest-published")
        << QStringLiteral("journal-initial-manifest-published")
        << 1 << QString() << false;
    QTest::newRow("peer-published")
        << QStringLiteral("peer-published") << 1 << QString() << false;
    QTest::newRow("peer-staging-published")
        << QStringLiteral("peer-staging-published")
        << 1 << QString() << false;
    QTest::newRow("peer-manifest-published")
        << QStringLiteral("peer-manifest-published")
        << 1 << QString() << false;
    QTest::newRow("peer-file-committed")
        << QStringLiteral("peer-file-committed")
        << 1 << QString() << false;
    QTest::newRow("backup-staged")
        << QStringLiteral("backup-staged") << 1 << QString() << false;
    QTest::newRow("previous-backup-preserved")
        << QStringLiteral("previous-backup-preserved") << 1 << QString() << false;
    QTest::newRow("backup-published")
        << QStringLiteral("backup-published") << 1 << QString() << false;
    QTest::newRow("source-tombstoned")
        << QStringLiteral("source-tombstoned") << 1 << QString() << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("commit-marker") << 1 << QString() << true;
    for (int occurrence = 1; occurrence <= 5; ++occurrence) {
        QTest::newRow(
            qPrintable(QStringLiteral("cleanup-%1")
                .arg(occurrence)))
            << QStringLiteral("cleanup-file")
            << occurrence << QString() << true;
    }
    for (const QString &phase :
         {QStringLiteral("journal-commit-marker-removed"),
          QStringLiteral("journal-peer-staging-removed"),
          QStringLiteral("journal-peer-old-removed"),
          QStringLiteral("journal-manifest-removed"),
          QStringLiteral("journal-directory-removed")}) {
        QTest::newRow(qPrintable(phase))
            << phase << 1 << QString() << true;
    }
    QTest::newRow("rollback-journal-peer-old-removed")
        << QStringLiteral("peer-published")
        << 1
        << QStringLiteral("journal-peer-old-removed")
        << false;
    QTest::newRow("rollback-journal-manifest-removed")
        << QStringLiteral("peer-published")
        << 1
        << QStringLiteral("journal-manifest-removed")
        << false;
    QTest::newRow("rollback-peer-staging-removed")
        << QStringLiteral("peer-published")
        << 1
        << QStringLiteral("rollback-peer-staging-removed")
        << false;
    QTest::newRow("rollback-backup-staging-removed")
        << QStringLiteral("backup-staged")
        << 1
        << QStringLiteral("rollback-backup-staging-removed")
        << false;
    QTest::newRow("rollback-source-tombstone-removed")
        << QStringLiteral("source-tombstoned")
        << 1
        << QStringLiteral("rollback-source-tombstone-removed")
        << false;
    QTest::newRow("rollback-previous-backup-removed")
        << QStringLiteral("source-tombstoned")
        << 1
        << QStringLiteral("rollback-previous-backup-removed")
        << false;
}

void TestRideCacheRemoval::
linkedDeletionCrashAfterPeerSaveRecoversOnRestart()
{
    QFETCH(QString, crashPhase);
    QFETCH(int, crashOccurrence);
    QFETCH(QString, recoveryCrashPhase);
    QFETCH(bool, committed);

    static const char RootEnvironment[] =
        "GC_RIDE_CACHE_REMOVAL_CRASH_ROOT";
    static const char ModeEnvironment[] =
        "GC_RIDE_CACHE_REMOVAL_CRASH_MODE";
    const QString root = qEnvironmentVariable(
        RootEnvironment);
    const QString mode = qEnvironmentVariable(
        ModeEnvironment);

    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(
            new Context(nullptr));
        std::unique_ptr<Athlete> athlete(
            new Athlete(context.get(), QDir(root)));
        std::unique_ptr<RideCache> cache(
            new RideCache(context.get()));
        athlete->rideCache = cache.get();

        if (mode == QStringLiteral("recover")) {
            if (!cache->startupRecoveryError().isEmpty())
                std::_Exit(87);
            return;
        }

        RideItem *target = new RideItem(
            nullptr, context.get());
        target->fileName = firstName();
        target->path =
            athlete->home->activities().absolutePath();
        RideItem *peer = new RideItem(
            nullptr, context.get());
        peer->fileName = secondName();
        peer->path =
            athlete->home->planned().absolutePath();
        peer->planned = true;
        cache->mutableRidesForRemovalTest().append(target);
        cache->mutableRidesForRemovalTest().append(peer);
        context->ride = target;
        target->setLinkedFileName(secondName());
        peer->setLinkedFileName(firstName());

        const QString targetPath =
            athlete->home->activities().filePath(
                firstName());
        const QString peerPath =
            athlete->home->planned().filePath(
                secondName());
        writeFixture(
            targetPath,
            QByteArray("target-linked-to-peer"));
        writeFixture(
            peerPath,
            QByteArray("peer-linked-to-target"));
        writeFixture(
            athlete->home->fileBackup().filePath(
                firstName() + QStringLiteral(".bak")),
            QByteArray("previous-backup"));
        const QString targetBaseName =
            QFileInfo(firstName()).baseName();
        for (const QString &extension :
             {QStringLiteral("cpx"),
              QStringLiteral("cpi"),
              QStringLiteral("notes")}) {
            writeFixture(
                athlete->home->cache().filePath(
                    targetBaseName + QLatin1Char('.')
                    + extension),
                extension.toLatin1() + "-derived");
        }
        setRideCacheRemovalPersistedSaveContents(
            secondName(), QByteArray("peer-unlinked"));

        cache->removeRideResult(target);
        QFAIL("linked deletion child did not stop at the requested transition");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild =
        [&](const QString &childMode,
            const QString &requestedCrashPhase) {
            QProcess child;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.remove(QStringLiteral(
                "GC_RIDE_CACHE_REMOVAL_CRASH_PHASE"));
            environment.remove(QStringLiteral(
                "GC_RIDE_CACHE_REMOVAL_CRASH_OCCURRENCE"));
            environment.insert(
                RootEnvironment, temporary.path());
            environment.insert(
                ModeEnvironment, childMode);
            environment.insert(
                QStringLiteral("QT_QPA_PLATFORM"),
                QStringLiteral("offscreen"));
            if (!requestedCrashPhase.isEmpty()) {
                environment.insert(
                    QStringLiteral(
                        "GC_RIDE_CACHE_REMOVAL_CRASH_PHASE"),
                    requestedCrashPhase);
                environment.insert(
                    QStringLiteral(
                        "GC_RIDE_CACHE_REMOVAL_CRASH_OCCURRENCE"),
                    QString::number(crashOccurrence));
            }
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral(
                    "linkedDeletionCrashAfterPeerSaveRecoversOnRestart:%1")
                    .arg(QString::fromLatin1(
                        QTest::currentDataTag()))});
            if (!child.waitForStarted(5000)) {
                return qMakePair(
                    -1, child.errorString());
            }
            if (!child.waitForFinished(10000)) {
                child.kill();
                child.waitForFinished();
                return qMakePair(
                    -2, QStringLiteral("child timed out"));
            }
            return qMakePair(
                child.exitCode(),
                QString::fromUtf8(child.readAll()));
        };

    const auto crashed = runChild(
        QStringLiteral("crash"), crashPhase);
    QCOMPARE(crashed.first, 86);

    if (!recoveryCrashPhase.isEmpty()) {
        const auto recoveryCrashed = runChild(
            QStringLiteral("recover"),
            recoveryCrashPhase);
        QCOMPARE(recoveryCrashed.first, 86);
    }

    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QByteArray targetAfterRecovery =
        readBytes(
            QDir(temporary.path())
                .filePath(QStringLiteral(
                    "activities/2026_07_06_08_00_00.json")));
    const QByteArray peerAfterRecovery =
        readBytes(
            QDir(temporary.path())
                .filePath(QStringLiteral(
                    "planned/2026_07_06_09_00_00.json")));

    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);

    const QString targetPath =
        QDir(temporary.path()).filePath(
            QStringLiteral(
                "activities/2026_07_06_08_00_00.json"));
    const QString peerPath =
        QDir(temporary.path()).filePath(
            QStringLiteral(
                "planned/2026_07_06_09_00_00.json"));
    const QString backupPath =
        QDir(temporary.path()).filePath(
            QStringLiteral(
                "bak/2026_07_06_08_00_00.json.bak"));
    if (committed) {
        QVERIFY(!QFileInfo::exists(targetPath));
        QCOMPARE(
            readBytes(peerPath),
            QByteArray("peer-unlinked"));
        QCOMPARE(
            readBytes(backupPath),
            QByteArray("target-linked-to-peer"));
    } else {
        QCOMPARE(
            readBytes(targetPath),
            QByteArray("target-linked-to-peer"));
        QCOMPARE(
            readBytes(peerPath),
            QByteArray("peer-linked-to-target"));
        QCOMPARE(
            readBytes(backupPath),
            QByteArray("previous-backup"));
    }
    QCOMPARE(
        readBytes(
            QDir(temporary.path())
                .filePath(QStringLiteral(
                    "activities/2026_07_06_08_00_00.json"))),
        targetAfterRecovery);
    QCOMPARE(
        readBytes(
            QDir(temporary.path())
                .filePath(QStringLiteral(
                    "planned/2026_07_06_09_00_00.json"))),
        peerAfterRecovery);

    const QString targetBaseName =
        QFileInfo(firstName()).baseName();
    for (const QString &extension :
         {QStringLiteral("cpx"),
          QStringLiteral("cpi"),
          QStringLiteral("notes")}) {
        const QString derivedPath =
            QDir(temporary.path()).filePath(
                QStringLiteral("cache/")
                + targetBaseName + QLatin1Char('.')
                + extension);
        if (committed) {
            QVERIFY(!QFileInfo::exists(derivedPath));
        } else {
            QCOMPARE(
                readBytes(derivedPath),
                extension.toLatin1() + "-derived");
        }
    }

    const QDir transactionRoot(
        QDir(temporary.path()).filePath(
            QStringLiteral(
                ".gc-transactions/linked-removal")));
    QVERIFY(!transactionRoot.exists()
        || transactionRoot.entryList(
            QDir::Dirs | QDir::Hidden
                | QDir::NoDotAndDotDot).isEmpty());
    QDirIterator artifacts(
        temporary.path(),
        QDir::Files | QDir::Hidden
            | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (artifacts.hasNext()) {
        const QString path = artifacts.next();
        const QString name = QFileInfo(path).fileName();
        const bool transactionSidecar =
            name.contains(QStringLiteral(".gc-linked-new-"))
            || name.contains(QStringLiteral(".gc-copy-"))
            || name.contains(QStringLiteral(".gc-remove-"))
            || name.contains(QStringLiteral(".gc-previous-"));
        QVERIFY2(
            !transactionSidecar
                || name.endsWith(QStringLiteral(".lock")),
            qPrintable(path));
    }
}

void TestRideCacheRemoval::
processorFailureIsReportedWithoutRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *item = fixture.addRide(
        firstName(), true, loadedRide);
    const QByteArray activity("activity");
    const QString sourcePath =
        fixture.activityPath(firstName());
    writeFixture(sourcePath, activity);
    setRideCacheRemovalProcessorFailure(true);

    RideCache::RemovalResult result;
    bool exceptionEscaped = false;
    try {
        result = fixture.cache->removeRideResult(item);
    } catch (...) {
        exceptionEscaped = true;
    }

    QVERIFY(!exceptionEscaped);
    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(readBytes(sourcePath), activity);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
processorReorderingDoesNotRemoveWrongRow()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target = fixture.addRide(
        firstName(), true, loadedRide);
    RideItem *survivor =
        fixture.addRide(secondName(), false);
    RideItem *intruder =
        fixture.addRide(thirdName(), false);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    setRideCacheRemovalProcessorAction(
        [&fixture] {
            fixture.cache->mutableRidesForRemovalTest().move(2, 0);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    const int countAfterRemoval =
        fixture.cache->count();
    const bool targetStillCached =
        fixture.cache->rides().contains(target);
    const bool survivorStillCached =
        fixture.cache->rides().contains(survivor);
    const bool intruderStillCached =
        fixture.cache->rides().contains(intruder);
    if (targetStillCached)
        fixture.cache->mutableRidesForRemovalTest().removeOne(target);
    if (!intruderStillCached)
        delete intruder;

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Committed);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(countAfterRemoval, 2);
    QVERIFY(!targetStillCached);
    QVERIFY(survivorStillCached);
    QVERIFY(intruderStillCached);
    QCOMPARE(fixture.context->ride, survivor);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        QByteArray("target activity"));
}

void TestRideCacheRemoval::
sidecarOwnerIntroducedBeforeCommitRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target = fixture.addRide(
        firstName(), true, loadedRide);
    const QString peerName =
        QFileInfo(firstName()).completeBaseName()
        + QStringLiteral(".fit");
    const QString targetPath = fixture.activityPath(firstName());
    const QString peerPath = fixture.activityPath(peerName);
    const QString notesPath =
        fixture.cachePath(firstName(), QStringLiteral("notes"));
    const QString cpiPath =
        fixture.cachePath(firstName(), QStringLiteral("cpi"));
    const QByteArray targetContents("target activity");
    const QByteArray peerContents("peer activity");
    const QByteArray notesContents("shared notes");
    const QByteArray cpiContents("shared cache");
    writeFixture(targetPath, targetContents);
    writeFixture(peerPath, peerContents);
    writeFixture(notesPath, notesContents);
    writeFixture(cpiPath, cpiContents);
    QPointer<RideItem> introducedOwner;
    setRideCacheRemovalProcessorAction(
        [&fixture, &introducedOwner, &peerName] {
            introducedOwner = fixture.addRide(peerName, false);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(introducedOwner);
    QCOMPARE(fixture.cache->count(), 2);
    QVERIFY(fixture.cache->rides().contains(target));
    QVERIFY(fixture.cache->rides().contains(introducedOwner.data()));
    QCOMPARE(fixture.context->ride, target);
    QCOMPARE(readBytes(targetPath), targetContents);
    QCOMPARE(readBytes(peerPath), peerContents);
    QCOMPARE(readBytes(notesPath), notesContents);
    QCOMPARE(readBytes(cpiPath), cpiContents);
    QVERIFY(!QFileInfo::exists(fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
modelRemovalSignalSiblingRemovalDoesNotUseInvalidIndex()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *survivor = fixture.addRide(firstName(), false);
    RideItem *target = fixture.addRide(secondName(), true);
    QAbstractItemModelTester modelTester(
        fixture.cache->model(),
        QAbstractItemModelTester::FailureReportingMode::QtTest);
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("target activity"));
    bool reentered = false;
    RideCache::RemovalResult nestedResult;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [&] {
            if (reentered) return;
            reentered = true;
            nestedResult =
                fixture.cache->removeArchivedRideResult(survivor);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(
        nestedResult.status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(nestedResult.affectedCount, 0);
    QVERIFY(!fixture.cache->rides().contains(target));
    QVERIFY(fixture.cache->rides().contains(survivor));
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.context->ride, survivor);
    QCOMPARE(
        readBytes(fixture.backupPath(secondName())),
        QByteArray("target activity"));
}

void TestRideCacheRemoval::
modelRemovalSignalKeepsSelectionValid_data()
{
    QTest::addColumn<bool>("destroyBeforeRowRemoval");

    QTest::newRow("rows-about-to-be-removed") << true;
    QTest::newRow("rows-removed") << false;
}

void TestRideCacheRemoval::
modelRemovalSignalKeepsSelectionValid()
{
    QFETCH(bool, destroyBeforeRowRemoval);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *survivor = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedTarget(target);
    bool observerRan = false;
    QString observedSelection;
    const auto destroyTarget = [target] { delete target; };
    const auto observeSelection = [&] {
        observerRan = true;
        RideItem *const selected = fixture.context->ride;
        QVERIFY(selected != nullptr);
        observedSelection = selected->fileName;
    };
    if (destroyBeforeRowRemoval) {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            destroyTarget);
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            observeSelection);
    } else {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsRemoved,
            destroyTarget);
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsRemoved,
            observeSelection);
    }
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(observerRan);
    QCOMPARE(observedSelection, secondName());
    QVERIFY(guardedTarget.isNull());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), survivor);
    QCOMPARE(fixture.context->ride, survivor);
}

void TestRideCacheRemoval::
modelRemovalSignalDestructionDoesNotExposeDanglingItem_data()
{
    QTest::addColumn<bool>("destroyBeforeRowRemoval");

    QTest::newRow("rows-about-to-be-removed") << true;
    QTest::newRow("rows-removed") << false;
}

void TestRideCacheRemoval::
modelRemovalSignalDestructionDoesNotExposeDanglingItem()
{
    QFETCH(bool, destroyBeforeRowRemoval);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target =
        fixture.addRide(firstName(), true);
    RideItem *survivor =
        fixture.addRide(secondName(), false);
    RideItem *const targetAddress = target;
    const QPointer<RideItem> guardedTarget(target);
    bool danglingNotification = false;

    const auto destroyTarget =
        [target] { delete target; };
    if (destroyBeforeRowRemoval) {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            destroyTarget);
    } else {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsRemoved,
            destroyTarget);
    }
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [&](RideItem *deleted) {
            danglingNotification =
                guardedTarget.isNull()
                && deleted == targetAddress;
        });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Committed);
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(guardedTarget.isNull());
    QVERIFY(!danglingNotification);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), survivor);
    QCOMPARE(fixture.context->ride, survivor);
}

void TestRideCacheRemoval::
modelSignalOwnerDestructionDoesNotContinue_data()
{
    QTest::addColumn<bool>("destroyCache");
    QTest::addColumn<bool>("destroyBeforeRowRemoval");

    QTest::newRow("cache-rows-about-to-be-removed")
        << true << true;
    QTest::newRow("cache-rows-removed")
        << true << false;
    QTest::newRow("context-rows-about-to-be-removed")
        << false << true;
    QTest::newRow("context-rows-removed")
        << false << false;
}

void TestRideCacheRemoval::
modelSignalOwnerDestructionDoesNotContinue()
{
    QFETCH(bool, destroyCache);
    QFETCH(bool, destroyBeforeRowRemoval);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *survivor = fixture.addRide(secondName(), false);
    RideCache *const cache = fixture.cache.get();
    QPointer<RideCache> cacheGuard(cache);
    QPointer<Context> contextGuard(fixture.context.get());
    QPointer<RideItem> targetGuard(target);
    QPointer<RideItem> survivorGuard(survivor);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const auto destroyOwner = [&fixture, destroyCache, target, survivor] {
        if (destroyCache) {
            fixture.athlete->rideCache = nullptr;
            fixture.context->ride = nullptr;
            delete fixture.cache.release();
            return;
        }
        target->context = nullptr;
        survivor->context = nullptr;
        fixture.athlete->context = nullptr;
        delete fixture.context.release();
    };
    if (destroyBeforeRowRemoval) {
        connect(
            cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            destroyOwner);
    } else {
        connect(
            cache->model(),
            &QAbstractItemModel::rowsRemoved,
            destroyOwner);
    }

    const RideCache::RemovalResult result =
        cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(result.affectedCount, 1);
    if (destroyCache) {
        QVERIFY(cacheGuard.isNull());
        QVERIFY(contextGuard);
        QVERIFY(targetGuard.isNull());
        QVERIFY(survivorGuard.isNull());
        QCOMPARE(contextGuard->ride, nullptr);
    } else {
        QVERIFY(contextGuard.isNull());
        QVERIFY(cacheGuard);
        QCOMPARE(cacheGuard->count(), 1);
        QCOMPARE(cacheGuard->rides().constFirst(), survivor);
        QVERIFY(!cacheGuard->rides().contains(target));
    }
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        QByteArray("target activity"));
}

void TestRideCacheRemoval::
batchModelSignalOwnerDestructionDoesNotContinue_data()
{
    QTest::addColumn<bool>("destroyCache");
    QTest::addColumn<bool>("destroyBeforeRowRemoval");

    QTest::newRow("cache-rows-about-to-be-removed")
        << true << true;
    QTest::newRow("cache-rows-removed")
        << true << false;
    QTest::newRow("context-rows-about-to-be-removed")
        << false << true;
    QTest::newRow("context-rows-removed")
        << false << false;
}

void TestRideCacheRemoval::
batchModelSignalOwnerDestructionDoesNotContinue()
{
    QFETCH(bool, destroyCache);
    QFETCH(bool, destroyBeforeRowRemoval);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *survivor = fixture.addRide(secondName(), false);
    RideCache *const cache = fixture.cache.get();
    QPointer<RideCache> cacheGuard(cache);
    QPointer<Context> contextGuard(fixture.context.get());
    QPointer<RideItem> targetGuard(target);
    QPointer<RideItem> survivorGuard(survivor);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("survivor activity"));

    const auto destroyOwner =
        [&fixture, destroyCache, target, survivor] {
            if (destroyCache) {
                fixture.athlete->rideCache = nullptr;
                fixture.context->ride = nullptr;
                delete fixture.cache.release();
                return;
            }
            target->context = nullptr;
            survivor->context = nullptr;
            fixture.athlete->context = nullptr;
            delete fixture.context.release();
        };
    if (destroyBeforeRowRemoval) {
        connect(
            cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            destroyOwner);
    } else {
        connect(
            cache->model(),
            &QAbstractItemModel::rowsRemoved,
            destroyOwner);
    }

    const RideCache::RemovalResult result =
        cache->removeRidesResult(
            QList<RideItem*>{target, survivor});

    QCOMPARE(result.requestedCount, 2);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::PartiallyCommitted);
    QCOMPARE(result.items.size(), 2);
    QVERIFY(result.items.at(0).logicallyRemoved());
    QCOMPARE(
        result.items.at(1).status,
        RideCache::RemovalStatus::NotAttempted);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        QByteArray("target activity"));
    QCOMPARE(
        readBytes(fixture.activityPath(secondName())),
        QByteArray("survivor activity"));
    if (destroyCache) {
        QVERIFY(cacheGuard.isNull());
        QVERIFY(contextGuard);
        QVERIFY(targetGuard.isNull());
        QVERIFY(survivorGuard.isNull());
    } else {
        QVERIFY(contextGuard.isNull());
        QVERIFY(cacheGuard);
        QCOMPARE(cacheGuard->count(), 1);
        QCOMPARE(cacheGuard->rides().constFirst(), survivor);
    }
}

void TestRideCacheRemoval::
derivedCleanupFailurePreservesCacheAndFiles_data()
{
    QTest::addColumn<QString>("failingExtension");

    QTest::newRow("notes") << QStringLiteral("notes");
    QTest::newRow("cpi") << QStringLiteral("cpi");
    QTest::newRow("cpx") << QStringLiteral("cpx");
}

void TestRideCacheRemoval::
derivedCleanupFailurePreservesCacheAndFiles()
{
    QFETCH(QString, failingExtension);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);

    const QStringList extensions = {
        QStringLiteral("notes"),
        QStringLiteral("cpi"),
        QStringLiteral("cpx")
    };
    for (const QString &extension : extensions) {
        const QString path =
            fixture.cachePath(firstName(), extension);
        if (extension == failingExtension) {
            writeFixture(
                QDir(path).filePath(
                    QStringLiteral("sentinel")),
                QByteArray("keep derived directory"));
        } else {
            writeFixture(
                path,
                extension.toLatin1() + " contents");
        }
    }

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);
    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    for (const QString &extension : extensions) {
        const QString path =
            fixture.cachePath(firstName(), extension);
        if (extension == failingExtension) {
            QVERIFY(QFileInfo(path).isDir());
            QCOMPARE(
                readBytes(QDir(path).filePath(
                    QStringLiteral("sentinel"))),
                QByteArray("keep derived directory"));
        } else {
            QCOMPARE(
                readBytes(path),
                extension.toLatin1() + " contents");
        }
    }
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::
committedCleanupFailureLeavesRecoveryFile_data()
{
    QTest::addColumn<QString>("failingExtension");

    QTest::newRow("notes") << QStringLiteral("notes");
    QTest::newRow("cpi") << QStringLiteral("cpi");
    QTest::newRow("cpx") << QStringLiteral("cpx");
}

void TestRideCacheRemoval::
committedCleanupFailureLeavesRecoveryFile()
{
    QFETCH(QString, failingExtension);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(
        fixture.activityPath(firstName()),
        activity);
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);

    const QStringList extensions = {
        QStringLiteral("notes"),
        QStringLiteral("cpi"),
        QStringLiteral("cpx")
    };
    for (const QString &extension : extensions) {
        writeFixture(
            fixture.cachePath(firstName(), extension),
            extension.toLatin1() + " contents");
    }
    const QString failingPath =
        fixture.cachePath(
            firstName(), failingExtension);
    setRideCacheRemovalCleanupFailurePath(failingPath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);
    QVERIFY(result.allLogicallyRemoved());
    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::
            CommittedCleanupPending);
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(result.error.contains(failingPath));

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        activity);
    QCOMPARE(
        readBytes(failingPath),
        failingExtension.toLatin1() + " contents");
    QVERIFY(result.recoveryPaths.contains(
        failingPath));
    QVERIFY(stagedFilesFor(
        fixture.backupPath(firstName())).isEmpty());
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
cleanupPendingBoolReportsLogicalRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), true);
    const QString notesPath =
        fixture.cachePath(
            firstName(), QStringLiteral("notes"));
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("activity"));
    writeFixture(notesPath, QByteArray("notes"));
    setRideCacheRemovalCleanupFailurePath(notesPath);

    QVERIFY(fixture.cache->removeRide(firstName()));

    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(readBytes(notesPath), QByteArray("notes"));
}

void TestRideCacheRemoval::
previousBackupCleanupFailureIsExplicit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);
    const QByteArray activity("activity");
    const QByteArray previousBackup("previous backup");
    const QString backupPath =
        fixture.backupPath(firstName());
    writeFixture(
        fixture.activityPath(firstName()), activity);
    writeFixture(backupPath, previousBackup);
    setRideCacheRemovalCleanupFailurePath(backupPath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::
            CommittedCleanupPending);
    QVERIFY(result.allLogicallyRemoved());
    QCOMPARE(readBytes(backupPath), activity);
    const QStringList recovery =
        recoveryFilesFor(
            backupPath,
            QStringLiteral(".gc-previous-"));
    QCOMPARE(recovery.size(), 1);
    QCOMPARE(
        readBytes(recovery.constFirst()),
        previousBackup);
    QVERIFY(result.recoveryPaths.contains(
        recovery.constFirst()));
}

void TestRideCacheRemoval::sourceCleanupFailureIsExplicit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);
    const QByteArray activity("activity");
    const QString sourcePath =
        fixture.activityPath(firstName());
    writeFixture(sourcePath, activity);
    setRideCacheRemovalCleanupFailurePath(sourcePath);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::
            CommittedCleanupPending);
    QVERIFY(result.allLogicallyRemoved());
    QVERIFY(!QFileInfo::exists(sourcePath));
    const QStringList recovery =
        stagedFilesFor(sourcePath);
    QCOMPARE(recovery.size(), 1);
    QCOMPARE(
        readBytes(recovery.constFirst()),
        activity);
    QVERIFY(result.recoveryPaths.contains(
        recovery.constFirst()));
}

void TestRideCacheRemoval::
archivedCleanupFailurePreservesCacheAndFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray archived("archived activity");
    const QString cpxDirectory =
        fixture.cachePath(
            firstName(), QStringLiteral("cpx"));
    writeFixture(
        fixture.backupPath(firstName()),
        archived);
    writeFixture(
        QDir(cpxDirectory).filePath(
            QStringLiteral("sentinel")),
        QByteArray("keep cpx directory"));

    QVERIFY(!fixture.cache->removeArchivedRide(item));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        archived);
    QVERIFY(QFileInfo(cpxDirectory).isDir());
    QCOMPARE(
        readBytes(QDir(cpxDirectory).filePath(
            QStringLiteral("sentinel"))),
        QByteArray("keep cpx directory"));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::batchRemovalReportsPartialFailure()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *failed = fixture.addRide(firstName(), false);
    RideItem *removed = fixture.addRide(secondName(), true);

    const QByteArray previousBackup("previous backup");
    const QByteArray removedActivity("removable activity");
    writeFixture(
        fixture.backupPath(firstName()),
        previousBackup);
    writeFixture(
        fixture.activityPath(secondName()),
        removedActivity);

    dismissNextMessageBox();
    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{failed, removed});
    QVERIFY(!result.allLogicallyRemoved());
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(!result.error.isEmpty());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), failed);
    QCOMPARE(
        readBytes(fixture.backupPath(firstName())),
        previousBackup);
    QCOMPARE(
        readBytes(fixture.backupPath(secondName())),
        removedActivity);
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
stagingCleanupFailureRequiresRecoveryAndStopsBatch()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addRide(firstName(), true);
    RideItem *second = fixture.addRide(secondName(), false);
    const QString firstPath = fixture.activityPath(firstName());
    const QString secondPath = fixture.activityPath(secondName());
    const QString firstBackup = fixture.backupPath(firstName());
    const QByteArray firstContents("first activity");
    const QByteArray changedContents("concurrent replacement");
    const QByteArray secondContents("second activity");
    const QByteArray retainedContents("retained staging sentinel");
    writeFixture(firstPath, firstContents);
    writeFixture(secondPath, secondContents);

    QString retainedStagingPath;
    setRideCacheRemovalMoveAction(
        firstPath,
        [&] {
            const QFileInfo backupInfo(firstBackup);
            QDir backupDirectory(backupInfo.absolutePath());
            const QStringList stagingNames = backupDirectory.entryList(
                {QStringLiteral(".%1.gc-copy-*")
                     .arg(backupInfo.fileName())},
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
            QCOMPARE(stagingNames.size(), 1);
            retainedStagingPath =
                backupDirectory.filePath(stagingNames.constFirst());
            QVERIFY(QFile::remove(retainedStagingPath));
            QVERIFY(QDir().mkdir(retainedStagingPath));
            writeFixture(
                QDir(retainedStagingPath).filePath(
                    QStringLiteral("sentinel")),
                retainedContents);
            writeFixture(firstPath, changedContents);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{first, second}, false);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(result.requestedCount, 2);
    QCOMPARE(result.items.size(), 2);
    QCOMPARE(
        result.items.at(0).status,
        RideCache::RemovalStatus::RecoveryRequired);
    QCOMPARE(
        result.items.at(1).status,
        RideCache::RemovalStatus::NotAttempted);
    QVERIFY(!retainedStagingPath.isEmpty());
    QVERIFY(result.recoveryPaths.contains(retainedStagingPath));
    QCOMPARE(
        readBytes(QDir(retainedStagingPath).filePath(
            QStringLiteral("sentinel"))),
        retainedContents);
    QCOMPARE(readBytes(firstPath), changedContents);
    QCOMPARE(readBytes(secondPath), secondContents);
    QVERIFY(!QFileInfo::exists(fixture.backupPath(secondName())));
    QCOMPARE(fixture.cache->count(), 2);
}

void TestRideCacheRemoval::
partialBatchBoolPreservesAnySuccessContract()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *missing =
        fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);
    const QByteArray activity("removable activity");
    writeFixture(
        fixture.activityPath(secondName()),
        activity);

    QVERIFY(fixture.cache->removeRides(
        QStringList{firstName(), secondName()}));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), missing);
    QCOMPARE(
        readBytes(fixture.backupPath(secondName())),
        activity);
}

void TestRideCacheRemoval::
sharedLegacySidecarsSurviveNamespaceRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), false);
    RideItem *planned =
        fixture.addPlannedRide(firstName(), true);

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray plannedActivity(
        "planned activity");
    const QByteArray notes("shared notes");
    const QByteArray cpi("shared cpi");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedActivity);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("notes")),
        notes);
    writeFixture(
        fixture.cachePath(
            firstName(), QStringLiteral("cpi")),
        cpi);

    QVERIFY(fixture.cache->removeRide(planned));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstName())));
    QCOMPARE(
        readBytes(
            fixture.plannedBackupPath(firstName())),
        plannedActivity);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("notes"))),
        notes);
    QCOMPARE(
        readBytes(fixture.cachePath(
            firstName(), QStringLiteral("cpi"))),
        cpi);
}

void TestRideCacheRemoval::currentRideRemovalUsesOrdinaryArchivePath()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item =
        fixture.addRide(firstName(), true);

    QStringList events;
    RideItem *deletedItem = nullptr;
    RideItem *selectedItem =
        reinterpret_cast<RideItem *>(quintptr(1));
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [&](RideItem *deleted) {
            events.append(QStringLiteral("deleted"));
            deletedItem = deleted;
            QVERIFY(fixture.context->ride == nullptr);
        });
    connect(
        fixture.context.get(),
        &Context::rideSelected,
        [&](RideItem *selected) {
            events.append(QStringLiteral("selected"));
            selectedItem = selected;
        });

    const QByteArray original("current original");
    writeFixture(fixture.activityPath(firstName()), original);

    QVERIFY(fixture.cache->removeCurrentRide());

    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(fixture.context->ride, nullptr);
    QCOMPARE(deletedItem, item);
    QCOMPARE(selectedItem, nullptr);
    const QStringList expectedEvents = {
        QStringLiteral("deleted"),
        QStringLiteral("selected")
    };
    QCOMPARE(events, expectedEvents);
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), original);
}

void TestRideCacheRemoval::
nonCurrentRemovalNotifiesDeletionAndPreservesSelection()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *selected =
        fixture.addRide(firstName(), true);
    RideItem *target =
        fixture.addRide(secondName(), false);

    QStringList events;
    RideItem *deletedItem = nullptr;
    RideItem *selectedItem = nullptr;
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [&](RideItem *deleted) {
            events.append(QStringLiteral("deleted"));
            deletedItem = deleted;
            QCOMPARE(fixture.context->ride, selected);
        });
    connect(
        fixture.context.get(),
        &Context::rideSelected,
        [&](RideItem *ride) {
            events.append(QStringLiteral("selected"));
            selectedItem = ride;
        });
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::Committed);
    QCOMPARE(fixture.context->ride, selected);
    QCOMPARE(deletedItem, target);
    QCOMPARE(selectedItem, selected);
    const QStringList expectedEvents = {
        QStringLiteral("deleted"),
        QStringLiteral("selected")
    };
    QCOMPARE(events, expectedEvents);
}

void TestRideCacheRemoval::batchRemovingAllRidesClearsSelection()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first =
        fixture.addRide(firstName(), true);
    RideItem *second =
        fixture.addRide(secondName(), false);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("first activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second activity"));

    QStringList events;
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [&](RideItem *) {
            events.append(QStringLiteral("deleted"));
        });
    connect(
        fixture.context.get(),
        &Context::rideSelected,
        [&](RideItem *) {
            events.append(QStringLiteral("selected"));
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{first, second});

    QVERIFY(result.allLogicallyRemoved());
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(fixture.context->ride, nullptr);
    const QStringList expectedEvents = {
        QStringLiteral("deleted"),
        QStringLiteral("selected"),
        QStringLiteral("deleted"),
        QStringLiteral("selected")
    };
    QCOMPARE(events, expectedEvents);
}

void TestRideCacheRemoval::missingRideIsRejectedWithoutTouchingFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(secondName(), true);

    const QByteArray contents("unrelated activity");
    writeFixture(fixture.activityPath(secondName()), contents);

    QVERIFY(!fixture.cache->removeArchivedRide(firstName()));

    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QCOMPARE(readBytes(fixture.activityPath(secondName())), contents);
}

void TestRideCacheRemoval::plannedRemovalDeletesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addPlannedRide(firstName(), false);
    fixture.addRide(secondName(), true);

    const QString completedCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QByteArray completedContents(
        "completed cache with matching basename");
    writeFixture(completedCpx, completedContents);
    writeFixture(
        plannedCpx,
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeArchivedRide(firstName()));

    QCOMPARE(readBytes(completedCpx), completedContents);
    QVERIFY(!QFileInfo::exists(plannedCpx));
}

void TestRideCacheRemoval::
plannedBatchRemovalDeletesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addPlannedRide(firstName(), false);
    fixture.addRide(secondName(), true);

    const QString plannedActivity =
        fixture.plannedActivityPath(
            firstName());
    const QString completedActivity =
        fixture.activityPath(
            secondName());
    const QString completedDecoyCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedTargetCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedTargetCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedDecoyCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QByteArray completedDecoyContents(
        "completed cache with matching basename");
    const QByteArray plannedDecoyContents(
        "planned cache with matching basename");
    writeFixture(
        plannedActivity,
        QByteArray("planned activity"));
    writeFixture(
        completedActivity,
        QByteArray("completed activity"));
    writeFixture(
        completedDecoyCpx,
        completedDecoyContents);
    writeFixture(
        plannedTargetCpx,
        QByteArray("planned target cache"));
    writeFixture(
        completedTargetCpx,
        QByteArray("completed target cache"));
    writeFixture(
        plannedDecoyCpx,
        plannedDecoyContents);

    QVERIFY(fixture.cache->removeRides(
        {firstName(), secondName()}, false));

    QVERIFY(!QFileInfo::exists(
        plannedActivity));
    QVERIFY(!QFileInfo::exists(
        completedActivity));
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(
        readBytes(completedDecoyCpx),
        completedDecoyContents);
    QVERIFY(!QFileInfo::exists(
        plannedTargetCpx));
    QVERIFY(!QFileInfo::exists(
        completedTargetCpx));
    QCOMPARE(
        readBytes(plannedDecoyCpx),
        plannedDecoyContents);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::batchRemovalRefreshesOnce()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), true);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("first activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second activity"));
    QVERIFY(fixture.cache->removeRides(
        {firstName(), secondName()}));

    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        1);
}

void TestRideCacheRemoval::
singleRemovalCancelsRefreshBeforeProcessing()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target =
        fixture.addRide(firstName(), true, loadedRide);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    bool processorObservedCancellation = false;
    setRideCacheRemovalProcessorAction(
        [&] {
            processorObservedCancellation =
                rideCacheRemovalCancelCount() == 1;
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QVERIFY(processorObservedCancellation);
}

void TestRideCacheRemoval::
batchRemovalSnapshotsAliasedRideList()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.addRide(firstName(), false);
    fixture.addRide(secondName(), false);
    fixture.addRide(thirdName(), true);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("first activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second activity"));
    writeFixture(
        fixture.activityPath(thirdName()),
        QByteArray("third activity"));

    QVERIFY(fixture.cache->removeRides(
        fixture.cache->rides(), false));

    QCOMPARE(fixture.cache->count(), 0);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(secondName())));
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(thirdName())));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::
batchRejectsReentrantIdentityMutation()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first =
        fixture.addRide(firstName(), true);
    RideItem *second =
        fixture.addRide(secondName(), false);
    const QString firstPath =
        fixture.activityPath(firstName());
    const QString secondPath =
        fixture.activityPath(secondName());
    const QString replacementPath =
        fixture.activityPath(thirdName());
    writeFixture(firstPath, QByteArray("first activity"));
    writeFixture(secondPath, QByteArray("second activity"));
    writeFixture(replacementPath, QByteArray("replacement activity"));
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [first, second](RideItem *deleted) {
            if (deleted == first)
                second->fileName = thirdName();
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{first, second});

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::PartiallyCommitted);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(result.requestedCount, 2);
    QCOMPARE(result.items.size(), 2);
    QCOMPARE(
        result.items.at(1).status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), second);
    QCOMPARE(readBytes(secondPath), QByteArray("second activity"));
    QCOMPARE(
        readBytes(replacementPath),
        QByteArray("replacement activity"));
}

void TestRideCacheRemoval::
batchSurvivesReentrantPendingDeletion()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first =
        fixture.addRide(firstName(), true);
    RideItem *second =
        fixture.addRide(secondName(), false);
    const QString firstPath =
        fixture.activityPath(firstName());
    const QString secondPath =
        fixture.activityPath(secondName());
    writeFixture(firstPath, QByteArray("first activity"));
    writeFixture(secondPath, QByteArray("second activity"));
    connect(
        fixture.context.get(),
        &Context::rideDeleted,
        [&fixture, first, second](RideItem *deleted) {
            if (deleted != first) return;
            fixture.cache->mutableRidesForRemovalTest().removeOne(second);
            delete second;
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRidesResult(
            QList<RideItem*>{first, second});

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::PartiallyCommitted);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(result.requestedCount, 2);
    QCOMPARE(result.items.size(), 2);
    QCOMPARE(
        result.items.at(1).status,
        RideCache::RemovalStatus::Rejected);
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(readBytes(secondPath), QByteArray("second activity"));
}

void TestRideCacheRemoval::
stalePointerOverloadsRejectWithoutDereference_data()
{
    QTest::addColumn<bool>("batch");

    QTest::newRow("single") << false;
    QTest::newRow("batch") << true;
}

void TestRideCacheRemoval::
stalePointerOverloadsRejectWithoutDereference()
{
    QFETCH(bool, batch);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *stale = new RideItem(nullptr, nullptr);
    delete stale;

    const RideCache::RemovalResult result = batch
        ? fixture.cache->removeRidesResult(
            QList<RideItem*>{stale}, false)
        : fixture.cache->removeRideResult(stale);

    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(result.requestedCount, 1);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(result.items.size(), 1);
    QCOMPARE(fixture.cache->count(), 0);
}

void TestRideCacheRemoval::
currentRemovalUsesSelectedNamespace()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), false);
    fixture.addPlannedRide(firstName(), true);

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray plannedActivity(
        "planned activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedActivity);
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeCurrentRide());

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(fixture.context->ride, completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(
            fixture.plannedBackupPath(firstName())),
        plannedActivity);
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
}

void TestRideCacheRemoval::
ambiguousFilenameRemovalFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    RideItem *planned =
        fixture.addPlannedRide(firstName(), false);
    fixture.context->ride = nullptr;

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray plannedActivity(
        "planned activity");
    const QByteArray completedCache(
        "completed cache");
    const QByteArray plannedCache(
        "planned cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        plannedActivity);
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        plannedCache);

    QVERIFY(!fixture.cache->removeRide(firstName()));
    QVERIFY(!fixture.cache->removeRides(
        {firstName()}, false));

    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(fixture.cache->rides().at(0), completed);
    QCOMPARE(fixture.cache->rides().at(1), planned);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QCOMPARE(
        readBytes(
            fixture.plannedActivityPath(
                firstName())),
        plannedActivity);
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QCOMPARE(
        readBytes(
            fixture.plannedCachePath(
                firstName(),
                QStringLiteral("cpx"))),
        plannedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
}

void TestRideCacheRemoval::
explicitNamespaceRemovalUsesIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    fixture.addPlannedRide(firstName(), false);

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned activity"));
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeRide(
        firstName(), true));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
}

void TestRideCacheRemoval::
explicitBatchRemovalUsesItemIdentity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *completed =
        fixture.addRide(firstName(), true);
    RideItem *planned =
        fixture.addPlannedRide(firstName(), false);

    const QByteArray completedActivity(
        "completed activity");
    const QByteArray completedCache(
        "completed cache");
    writeFixture(
        fixture.activityPath(firstName()),
        completedActivity);
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned activity"));
    writeFixture(
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx")),
        completedCache);
    writeFixture(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx")),
        QByteArray("planned cache"));

    QVERIFY(fixture.cache->removeRides(
        QList<RideItem*>{planned}, false));

    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), completed);
    QCOMPARE(
        readBytes(
            fixture.activityPath(firstName())),
        completedActivity);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(
            firstName())));
    QCOMPARE(
        readBytes(
            fixture.cachePath(
                firstName(),
                QStringLiteral("cpx"))),
        completedCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"))));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(
        rideCacheRemovalEstimatorRefreshCount(),
        0);
}

void TestRideCacheRemoval::plannedRenameMovesOnlyPlannedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString oldActivity =
        fixture.plannedActivityPath(firstName());
    const QString newActivity =
        fixture.plannedActivityPath(secondName());
    const QString completedOldCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedNewCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedOldCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedNewCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString oldNotes =
        fixture.cachePath(
            firstName(),
            QStringLiteral("notes"));
    const QString newNotes =
        fixture.cachePath(
            secondName(),
            QStringLiteral("notes"));
    const QByteArray activityContents(
        "planned activity");
    const QByteArray completedContents(
        "completed cache with matching basename");
    const QByteArray plannedContents(
        "planned cache");
    const QByteArray notesContents(
        "shared notes");
    writeFixture(
        oldActivity, activityContents);
    writeFixture(
        completedOldCpx,
        completedContents);
    writeFixture(
        plannedOldCpx,
        plannedContents);
    writeFixture(
        oldNotes, notesContents);

    QString error;
    QVERIFY2(
        fixture.cache->renameRideFilesForTest(
            firstName(),
            secondName(),
            true,
            error),
        qPrintable(error));

    QVERIFY(!QFileInfo::exists(oldActivity));
    QCOMPARE(
        readBytes(newActivity),
        activityContents);
    QCOMPARE(
        readBytes(completedOldCpx),
        completedContents);
    QVERIFY(!QFileInfo::exists(
        completedNewCpx));
    QVERIFY(!QFileInfo::exists(
        plannedOldCpx));
    QCOMPARE(
        readBytes(plannedNewCpx),
        plannedContents);
    QVERIFY(!QFileInfo::exists(oldNotes));
    QCOMPARE(
        readBytes(newNotes),
        notesContents);
}

void TestRideCacheRemoval::
completedRenameMovesOnlyCompletedCpx()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString oldActivity =
        fixture.activityPath(firstName());
    const QString newActivity =
        fixture.activityPath(secondName());
    const QString completedOldCpx =
        fixture.cachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString completedNewCpx =
        fixture.cachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QString plannedOldCpx =
        fixture.plannedCachePath(
            firstName(),
            QStringLiteral("cpx"));
    const QString plannedNewCpx =
        fixture.plannedCachePath(
            secondName(),
            QStringLiteral("cpx"));
    const QByteArray activityContents(
        "completed activity");
    const QByteArray completedContents(
        "completed cache");
    const QByteArray plannedContents(
        "planned cache with matching basename");
    writeFixture(
        oldActivity, activityContents);
    writeFixture(
        completedOldCpx,
        completedContents);
    writeFixture(
        plannedOldCpx,
        plannedContents);

    QString error;
    QVERIFY2(
        fixture.cache->renameRideFilesForTest(
            firstName(),
            secondName(),
            false,
            error),
        qPrintable(error));

    QVERIFY(!QFileInfo::exists(oldActivity));
    QCOMPARE(
        readBytes(newActivity),
        activityContents);
    QVERIFY(!QFileInfo::exists(
        completedOldCpx));
    QCOMPARE(
        readBytes(completedNewCpx),
        completedContents);
    QCOMPARE(
        readBytes(plannedOldCpx),
        plannedContents);
    QVERIFY(!QFileInfo::exists(
        plannedNewCpx));
}

QTEST_MAIN(TestRideCacheRemoval)
#include "testRideCacheRemoval.moc"
