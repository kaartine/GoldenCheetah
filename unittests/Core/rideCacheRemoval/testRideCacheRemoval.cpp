#include <QtTest>

#include "Athlete.h"
#include "AnchoredFileSystem.h"
#include "Context.h"
#include "AtomicFileWriter.h"
#include "LinkedActivityRemovalJournal.h"
#include "LinkedActivitySaveJournal.h"
#include "PlanReplacementJournal.h"
#include "RideCache.h"
#include "RideCacheModel.h"
#include "RideCacheMutationScope.h"
#include "RideItem.h"
#include "NavigationModel.h"
#include "TrainDB.h"

#define private public
#include "PlanBundle.h"
#undef private

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <functional>
#include <cstdlib>
#include <memory>
#include <thread>

void resetRideCacheRemovalRefreshCounts();
int rideCacheRemovalRefreshCount();
int rideCacheRemovalEstimatorRefreshCount();
int rideCacheRemovalEstimatorStopCount();
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
bool rideCacheRemovalPartialMoveWasTriggered();
QString rideCacheRemovalPartialMovePublishedLinkPath();
QString rideCacheRemovalPartialMoveFailureError();
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
int rideCacheRemovalStartupInvalidationCount();
int rideCacheRemovalRideItemDestructionCount();
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
void setRideCacheRemovalTransitionAction(
    const QByteArray &transition,
    const std::function<void()> &action);
void setAnchoredFilesystemTransitionAction(
    const QByteArray &transition,
    const std::function<void(
        const QString &, const QString &)> &action);
void setRideCacheRemovalValidationMutation(
    const QByteArray &contents);
void setRideCacheRemovalLinkMutationActionOnCall(
    int call,
    const std::function<void()> &action);
void setRideCacheCalendarStorageAction(
    const std::function<void()> &action);
void setRideCacheCalendarCopyFailureOnCall(int call);
void setRideCacheActivityIdentityFailureOnCall(int call);
void setPlanBundleWorkoutRootForTest(const QString &path);

namespace {

constexpr int PlannedCopyCrashExitCode = 85;
const char PlannedCopyCrashRootEnvironment[] =
    "GC_RIDE_CACHE_COPY_CRASH_ROOT";
const char PlannedCopyCrashModeEnvironment[] =
    "GC_RIDE_CACHE_COPY_CRASH_MODE";
const char ActivityMoveCrashRootEnvironment[] =
    "GC_RIDE_CACHE_MOVE_CRASH_ROOT";
const char ActivityMoveCrashModeEnvironment[] =
    "GC_RIDE_CACHE_MOVE_CRASH_MODE";
const char ActivityShiftCrashRootEnvironment[] =
    "GC_RIDE_CACHE_SHIFT_CRASH_ROOT";
const char ActivityShiftCrashModeEnvironment[] =
    "GC_RIDE_CACHE_SHIFT_CRASH_MODE";
const char PlanReplacementCrashPhaseEnvironment[] =
    "GC_PLAN_REPLACEMENT_CRASH_PHASE";
const char PlanReplacementCrashOccurrenceEnvironment[] =
    "GC_PLAN_REPLACEMENT_CRASH_OCCURRENCE";

enum class JournalNamespaceEntryKind
{
    RegularFile,
    SymbolicLink,
    Directory
};

QString qlockRemovalGuardName(
    const QString &lockName, int suffixCount = 1)
{
    return lockName + QStringLiteral(".rmlock").repeated(suffixCount);
}

QString qlockRemovalGuardName(int suffixCount = 1)
{
    return qlockRemovalGuardName(
        QStringLiteral(".01234567-89ab-cdef-8123-456789abcdef.lock"),
        suffixCount);
}

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

QString nextDayName()
{
    return QStringLiteral("2026_07_07_08_00_00.json");
}

QString twoDaysLaterName()
{
    return QStringLiteral("2026_07_08_08_00_00.json");
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

void replaceFixtureWhilePinned(
    const QString &path,
    const QByteArray &contents)
{
#ifdef Q_OS_WIN
    const QString retainedPath =
        path + QStringLiteral(".gc-test-retained-original");
    QVERIFY(!QFileInfo::exists(retainedPath));
    QFile original(path);
    QVERIFY2(
        original.rename(retainedPath),
        qPrintable(original.errorString()));
#endif
    writeFixture(path, contents);
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool createTestSymbolicLink(
    const QString &targetPath,
    const QString &linkPath,
    bool directory)
{
#ifdef Q_OS_WIN
    const QString nativeTarget =
        QDir::toNativeSeparators(targetPath);
    const QString nativeLink =
        QDir::toNativeSeparators(linkPath);
    const DWORD typeFlag = directory
        ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (::CreateSymbolicLinkW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
            reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
            typeFlag
                | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        return true;
    }
    if (::GetLastError() != ERROR_INVALID_PARAMETER)
        return false;
#endif
    return ::CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
        typeFlag);
#else
    Q_UNUSED(directory)
    return QFile::link(targetPath, linkPath);
#endif
}

bool nativeIdentityForPath(
    const QString &path,
    AnchoredFileSystem::NativeIdentity &identity,
    QString &error)
{
    const QFileInfo info(path);
    AnchoredFileSystem::DirectoryAnchor parent;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            info.absolutePath(), parent, error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef entry =
        parent.entry(info.fileName(), error);
    if (!entry.isValid()) return false;

    AnchoredFileSystem::PinnedFile file;
    if (!AnchoredFileSystem::pinRegularFile(
            entry, file, error)) {
        return false;
    }
    identity = file.identity();
    return identity.isValid();
}

QStringList removalDataArtifactsUnder(const QString &rootPath)
{
    QStringList paths;
    QDirIterator entries(
        rootPath,
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next();
        const QString name = QFileInfo(path).fileName();
        if (!name.endsWith(QStringLiteral(".lock"))
            && (name.contains(QStringLiteral(".gc-copy-"))
                || name.contains(QStringLiteral(".gc-remove-"))
                || name.contains(QStringLiteral(".gc-previous-")))) {
            paths.append(path);
        }
    }
    return paths;
}

bool cacheContains(const RideCache &cache, const QString &fileName)
{
    for (const RideItem *item :
         cache.rides()) {
        if (item && item->fileName == fileName) return true;
    }
    return false;
}

RideItem *cacheItem(RideCache &cache, const QString &fileName)
{
    for (RideItem *item : cache.rides()) {
        if (item && item->fileName == fileName) return item;
    }
    return nullptr;
}

bool stageBytes(
    const QString &path,
    const QByteArray &contents,
    QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(contents) != contents.size()
        || !file.flush()) {
        error = file.errorString();
        return false;
    }
    return true;
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
        setPlanBundleWorkoutRootForTest(QString());
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

bool preparePlanBundleReader(
    PlanBundleReader &reader,
    const QDateTime &sourceDateTime,
    const QDateTime &targetDateTime,
    const QString &workoutReference = QString())
{
    reader.tempDir = new QTemporaryDir();
    if (!reader.tempDir->isValid()) return false;
    const QDir root(reader.tempDir->path());
    if (!root.mkpath(QStringLiteral("planned"))
        || !root.mkpath(QStringLiteral("workouts"))) {
        return false;
    }
    reader.plannedDir = QDir(
        root.filePath(QStringLiteral("planned")));
    reader.workoutDir = QDir(
        root.filePath(QStringLiteral("workouts")));
    reader.metadata.name = QStringLiteral("Atomic test plan");
    reader.targetRangeStart = targetDateTime.date();
    reader.targetRangeEnd = targetDateTime.date();
    reader.duration = 1;

    const QString sourceName = sourceDateTime.toString(
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
        + QStringLiteral(".json");
    QByteArray sourceContents(
        "plan bundle activity\n");
    if (!workoutReference.isEmpty()) {
        sourceContents += "workout="
            + workoutReference.toUtf8() + '\n';
    }
    const QString sourcePath =
        reader.plannedDir.filePath(sourceName);
    writeFixture(sourcePath, sourceContents);
    if (readBytes(sourcePath) != sourceContents)
        return false;
    RideFile *preview = new RideFile(sourceDateTime, 1.0);
    if (!workoutReference.isEmpty()) {
        preview->setTag(
            QStringLiteral("WorkoutFilename"),
            workoutReference);
    }
    reader.rideFiles.append({
        preview, true, targetDateTime, sourceName});
    return true;
}

} // namespace

void planReplacementTransitionReached(const char *transition)
{
    const QByteArray requested = qgetenv(
        PlanReplacementCrashPhaseEnvironment);
    if (requested.isEmpty() || requested != transition) return;

    static QHash<QByteArray, int> occurrences;
    const int occurrence = ++occurrences[requested];
    bool valid = false;
    const int requestedOccurrence =
        qEnvironmentVariableIntValue(
            PlanReplacementCrashOccurrenceEnvironment,
            &valid);
    if (occurrence == (valid ? requestedOccurrence : 1))
        std::_Exit(PlannedCopyCrashExitCode);
}

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
    void sourceParentReplacementDoesNotTouchSubstituteFile();
    void sourceParentReplacementDuringRollbackRequiresRecovery();
    void backupReplacementBeforeCommitRequiresRecovery();
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
    void linkValidationRejectsNullAndDestroyedItems();
    void dataProcessorReceivesLoadedRideBeforeArchive();
    void linkedRideClosedDuringMetadataChangeIsNotReused();
    void sourceSymlinkIsRejected();
    void lockedSourceIsRejected();
    void derivedFilesAreNotMovedBeforeCommit_data();
    void derivedFilesAreNotMovedBeforeCommit();
    void derivedMutationBeforeStagingRollsBackFiles();
    void missingDerivedParentCreatedDuringPrepareRejectsRemoval();
    void directorySyncFailureRollsBackFiles();
    void rollbackFailureRequiresRecovery();
    void sourceRestoreSyncFailureRetainsPublishedBackup();
    void failedPreviousBackupMovePartialStateRequiresRecovery();
    void failedBackupPublishPartialMoveRequiresRecovery();
    void failedSourceMovePartialStateRequiresRecovery();
    void committedCleanupPartialRemovalRejectsUnverifiedRecoveryPath();
    void committedCleanupParentReplacementDoesNotReportSubstitute();
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
    void recoveryPathsRejectReplacedJournalDirectory();
    void recoveryPathsRejectCandidateReplacedAfterCollection();
    void recoveryPathsRejectCandidateReplacedAfterFinalJournalCheck();
    void staleQLockFileRemovalGuardDoesNotPoisonReconcile_data();
    void staleQLockFileRemovalGuardDoesNotPoisonReconcile();
    void staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile_data();
    void staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile();
    void unsafeJournalQLockFileRemovalGuardEntriesRemainRejected_data();
    void unsafeJournalQLockFileRemovalGuardEntriesRemainRejected();
    void unsafeQLockFileRemovalGuardEntriesRemainRejected_data();
    void unsafeQLockFileRemovalGuardEntriesRemainRejected();
    void pendingLinkedSaveJournalBlocksDeletionTransaction();
    void abandonedPlanJournalBlocksLinkedTransaction_data();
    void abandonedPlanJournalBlocksLinkedTransaction();
    void activeRemovalJournalRejectsDirectorySubstitute();
    void journalCleanupRejectsNamespaceRedirect();
    void journalCleanupDoesNotHideNonEmptyQuarantine_data();
    void journalCleanupDoesNotHideNonEmptyQuarantine();
    void activeRemovalJournalRejectsPeerOldSubstitute();
    void peerOldCleanupRejectsSubstitute();
    void journalTemporaryCleanupRejectsSubstitute();
    void preManifestTemporaryCleanupRejectsSubstitute();
    void activeRemovalJournalRejectsManifestSubstitute();
    void peerManifestPublicationRejectsManifestSubstitute();
    void commitMarkerReadRejectsMarkerSubstitute();
    void startupReconcilesAbandonedLinkedSaveJournal();
    void startupReportsCorruptLinkedSaveJournal();
    void startupReconcilesAbandonedPlanReplacementJournal();
    void startupReportsCorruptPlanReplacementJournal();
    void prepareRestrictsExistingTransactionDirectories();
    void startupRestrictsExistingTransactionDirectories();
    void oversizedUnreadableJournalControlFileFailsBeforeRead_data();
    void oversizedUnreadableJournalControlFileFailsBeforeRead();
    void startupWithoutJournalAllowsSymlinkedAthleteRoot();
    void legacyLinkedRemovalManifestRecovers();
    void linkedDeletionCrashAfterPeerSaveRecoversOnRestart_data();
    void linkedDeletionCrashAfterPeerSaveRecoversOnRestart();
    void ordinaryDeletionCrashRecoversOnRestart_data();
    void ordinaryDeletionCrashRecoversOnRestart();
    void processorFailureIsReportedWithoutRemoval();
    void processorReorderingDoesNotRemoveWrongRow();
    void preexistingDestroyedSiblingDoesNotBreakRemoval();
    void processorDestroyedSelectionNeighborIsPurged();
    void sidecarOwnerIntroducedBeforeCommitRejectsRemoval();
    void modelRemovalSignalSiblingRemovalDoesNotUseInvalidIndex();
    void modelRemovalSignalKeepsSelectionValid_data();
    void modelRemovalSignalKeepsSelectionValid();
    void modelRemovalSignalDestructionDoesNotExposeDanglingItem_data();
    void modelRemovalSignalDestructionDoesNotExposeDanglingItem();
    void modelRemovalSignalReaderSkipsDestroyedItem();
    void modelRemovalPublishesOnlyLiveSnapshot();
    void ordinaryRemovalStopsEstimatorBeforeRowsAbout();
    void refreshRequestedDuringRemovalIsDeferred();
    void rowsRemovedGarbageCollectSkipsDestroyedAddress();
    void modelRemovalPurgesDestroyedSibling_data();
    void modelRemovalPurgesDestroyedSibling();
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
    void temporaryRideItemDestructionDoesNotCreateCacheTombstone();
    void tombstonedCacheEntryIsNotDereferencedOrRemoved();
    void removalPrecheckRejectsDestroyedCacheAddress();
    void navigationRideEventDoesNotRetainDestroyedItem();
    void currentRemovalUsesSelectedNamespace();
    void ambiguousFilenameRemovalFailsClosed();
    void explicitNamespaceRemovalUsesIdentity();
    void explicitBatchRemovalUsesItemIdentity();
    void plannedRenameMovesOnlyPlannedCpx();
    void plannedReplacementPurgesPreexistingDestroyedRow();
    void plannedReplacementStageFailurePreservesOldGeneration();
    void plannedReplacementStageFailureResumesBackgroundWork();
    void plannedReplacementCommitsOneCompleteGeneration();
    void planBundleImportPublishesOneCompleteGeneration();
    void planBundleImportCommitsWorkoutFileAndDatabaseRow();
    void planBundleImportStageFailurePreservesOldGeneration();
    void plannedReplacementQuiescesWorkersAndDefersRefresh();
    void plannedReplacementBlocksRefreshRestartDuringCallbacks();
    void plannedReplacementRefreshDoesNotLeakIntoNextRemoval();
    void addRidePurgesPreexistingDestroyedRow();
    void addRidesPurgesPreexistingDestroyedRow();
    void addRidePurgesRowDestroyedDuringReset();
    void duplicateSingleImportRetiresReplacedItem();
    void duplicateSingleImportRetiresOldWhenReplacementIsDestroyed();
    void duplicateBatchImportRetiresEveryDisplacedItem();
    void consecutiveAddsCoalesceBackgroundResume();
    void mutationScopePublishesOnlyLiveSortedRows();
    void mutationScopeDefersConfigAndReleasesModelReservation();
    void moveActivityPurgesDestroyedRowsBeforeReorder();
    void copyPlannedActivityPurgesDestroyedRowsBeforePublish();
    void copyPlannedActivityUsesRequestedTime();
    void copyPlannedActivitiesPurgesDestroyedRowsBeforePublish();
    void shiftPlannedActivitiesPurgesDestroyedRowsBeforeReorder();
    void moveActivityReservesModelBeforeStorageCommit();
    void moveActivityCrashRecoveryIsGenerationAtomic();
    void moveActivityPostCommitNotificationLossIsReportedCommitted();
    void moveLinkedActivityPersistsReciprocalGeneration();
    void moveLinkedActivityStageFailureRollsBackGeneration();
    void moveLinkedActivityCrashRecoveryIsGenerationAtomic_data();
    void moveLinkedActivityCrashRecoveryIsGenerationAtomic();
    void copyPlannedActivityReservesModelBeforeStorageCommit();
    void copyPlannedActivitiesReserveModelBeforeStorageCommit();
    void copyPlannedActivitiesStageFailureRollsBackGeneration();
    void copyPlannedActivitiesCrashRecoveryIsGenerationAtomic_data();
    void copyPlannedActivitiesCrashRecoveryIsGenerationAtomic();
    void shiftPlannedActivitiesReservesModelBeforeStorageCommit();
    void shiftPlannedActivitiesCommitOverlappingGeneration();
    void shiftPlannedActivitiesStageFailureRollsBackGeneration();
    void shiftLinkedPlannedActivityPersistsReciprocalGeneration();
    void shiftPlannedActivitiesCrashRecoveryIsGenerationAtomic_data();
    void shiftPlannedActivitiesCrashRecoveryIsGenerationAtomic();
    void plannedReplacementDropsStaleResumeDuringNewReplacement();
    void plannedReplacementCoalescesSupersededResumes();
    void plannedReplacementProcessorFailureIsReportedAfterPublication();
    void plannedReplacementProcessorRunsAfterPublication();
    void plannedReplacementProcessorUsesIsolatedRide();
    void plannedReplacementArchivesOldGenerationAndInvalidatesDerivedFiles();
    void plannedReplacementFailurePreservesBackupAndDerivedFiles();
    void plannedReplacementSupportsImportWithoutRemovals();
    void plannedReplacementCoordinatorWrapsDurableCommit();
    void plannedReplacementRetainsJournalAfterExternalCommitFailure();
    void plannedReplacementRejectsInvalidTargets_data();
    void plannedReplacementRejectsInvalidTargets();
    void plannedReplacementCacheMutationDuringStageRollsBack();
    void plannedReplacementSourceMutationDuringStageFailsClosed();
    void plannedReplacementRejectsLinkedActivity();
    void plannedReplacementSnapshotsTargetsBeforeCallbacks();
    void plannedReplacementRejectsLinkIntroducedDuringStage();
    void plannedReplacementRejectsSidecarOwnerIntroducedDuringStage();
    void plannedReplacementRejectsDerivedFileIntroducedDuringStage();
    void plannedReplacementStageMutationAfterValidationRollsBack();
    void plannedReplacementStageCallbackDeletionIsContained();
    void plannedReplacementStageCallbackDeferredDeletionIsContained();
    void plannedReplacementRejectsUnreadableStagedActivity();
    void plannedReplacementRejectsDirtyTarget();
    void plannedReplacementRejectsWrongThreadBeforeCallbacks();
    void plannedReplacementCorruptCommitMarkerRequiresRecovery();
    void plannedReplacementBackupParentSymlinkIsRejected();
    void plannedReplacementBackupRootSyncFailurePreservesGeneration();
    void plannedReplacementInvalidatesStartupSnapshots();
    void plannedReplacementModelSignalDeletesOldItemSafely_data();
    void plannedReplacementModelSignalDeletesOldItemSafely();
    void plannedReplacementModelSignalDeletesUnrelatedItemSafely_data();
    void plannedReplacementModelSignalDeletesUnrelatedItemSafely();
    void plannedReplacementModelResetDeletesIncomingItemSafely();
    void plannedReplacementDefersNestedConfigReset_data();
    void plannedReplacementDefersNestedConfigReset();
    void modelResetRemainsBalancedAcrossReentrantInsert();
    void modelInsertRejectsReentrantReset();
    void plannedReplacementNotificationDeletionIsContained_data();
    void plannedReplacementNotificationDeletionIsContained();
    void plannedReplacementDeletionNotificationCanDestroyOldItem();
    void plannedReplacementDeletionNotificationCanDeferOldItemDeletion();
    void plannedReplacementMarksDestroyedAddressDuringCallback();
    void plannedReplacementAdditionNotificationCanDeferIncomingDeletion();
    void plannedReplacementNotificationOwnerLossIsReported();
    void plannedReplacementModelSignalOwnerDestructionDoesNotContinue_data();
    void plannedReplacementModelSignalOwnerDestructionDoesNotContinue();
    void plannedCopyReplacementCommitsOneCompleteGeneration();
    void plannedCopyReplacementMissingSourcePreservesOldGeneration();
    void plannedCopyReplacementRejectsStaleSourceWithoutDereference();
    void plannedCopyReplacementCanReplaceSameTargetPath();
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
    const QString activityPath =
        fixture.activityPath(firstName());
    setRideCacheRemovalMoveAction(
        activityPath,
        [activityPath, concurrentActivity] {
            replaceFixtureWhilePinned(
                activityPath, concurrentActivity);
        });

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

void TestRideCacheRemoval::
sourceParentReplacementDoesNotTouchSubstituteFile()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("same-content identity sentinel");
    const QString sourcePath = fixture.activityPath(firstName());
    const QString sourceDirectory = QFileInfo(sourcePath).absolutePath();
    const QString displacedDirectory =
        sourceDirectory + QStringLiteral(".displaced");
    writeFixture(sourcePath, activity);

    AnchoredFileSystem::NativeIdentity originalIdentity;
    QString identityError;
    QVERIFY2(
        nativeIdentityForPath(
            sourcePath, originalIdentity, identityError),
        qPrintable(identityError));

    bool actionReached = false;
    bool parentReplaced = false;
    AnchoredFileSystem::NativeIdentity substituteIdentity;
    setRideCacheRemovalMoveAction(
        sourcePath,
        [&] {
            actionReached = true;
            if (!QDir().rename(
                    sourceDirectory,
                    displacedDirectory)) {
                return;
            }
            parentReplaced = true;
            QVERIFY(QDir().mkdir(sourceDirectory));
            writeFixture(sourcePath, activity);
            QString error;
            QVERIFY2(
                nativeIdentityForPath(
                    sourcePath, substituteIdentity, error),
                qPrintable(error));
            QVERIFY(substituteIdentity != originalIdentity);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(actionReached);
    if (!parentReplaced) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows directory handles prevent this parent replacement race");
#else
        QFAIL("The source parent could not be replaced");
#endif
    }

    const QString displacedSourcePath =
        QDir(displacedDirectory).filePath(
            QFileInfo(sourcePath).fileName());
    QVERIFY2(
        QFileInfo::exists(sourcePath),
        "The byte-identical substitute was removed");
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(displacedSourcePath), activity);

    AnchoredFileSystem::NativeIdentity canonicalIdentity;
    identityError.clear();
    QVERIFY2(
        nativeIdentityForPath(
            sourcePath, canonicalIdentity, identityError),
        qPrintable(identityError));
    QCOMPARE(canonicalIdentity, substituteIdentity);
    QVERIFY(canonicalIdentity != originalIdentity);

    AnchoredFileSystem::NativeIdentity displacedIdentity;
    identityError.clear();
    QVERIFY2(
        nativeIdentityForPath(
            displacedSourcePath,
            displacedIdentity,
            identityError),
        qPrintable(identityError));
    QCOMPARE(displacedIdentity, originalIdentity);

    QVERIFY(!result.allLogicallyRemoved());
    QVERIFY(!result.cleanlyCompleted());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.recoveryPaths.contains(sourcePath));
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QVERIFY(!QFileInfo::exists(
        fixture.backupPath(firstName())));
    QVERIFY2(
        removalDataArtifactsUnder(
            fixture.temporary.path()).isEmpty(),
        qPrintable(
            removalDataArtifactsUnder(
                fixture.temporary.path()).join(
                    QStringLiteral(", "))));
}

void TestRideCacheRemoval::
sourceParentReplacementDuringRollbackRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("same-content rollback sentinel");
    const QString sourcePath = fixture.activityPath(firstName());
    const QString sourceDirectory = QFileInfo(sourcePath).absolutePath();
    const QString displacedDirectory =
        sourceDirectory + QStringLiteral(".rollback-displaced");
    writeFixture(sourcePath, activity);

    AnchoredFileSystem::NativeIdentity originalIdentity;
    QString identityError;
    QVERIFY2(
        nativeIdentityForPath(
            sourcePath, originalIdentity, identityError),
        qPrintable(identityError));

    bool actionReached = false;
    bool parentReplaced = false;
    AnchoredFileSystem::NativeIdentity substituteIdentity;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("backup-published"),
        [&] {
            actionReached = true;
            if (!QDir().rename(
                    sourceDirectory,
                    displacedDirectory)) {
                return;
            }
            parentReplaced = true;
            QVERIFY(QDir().mkdir(sourceDirectory));
            writeFixture(sourcePath, activity);
            QString error;
            QVERIFY2(
                nativeIdentityForPath(
                    sourcePath, substituteIdentity, error),
                qPrintable(error));
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(actionReached);
    if (!parentReplaced) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows directory handles prevent this rollback race");
#else
        QFAIL("The source parent could not be replaced during rollback");
#endif
    }

    const QString displacedSourcePath =
        QDir(displacedDirectory).filePath(
            QFileInfo(sourcePath).fileName());
    QCOMPARE(result.status,
             RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(displacedSourcePath), activity);

    AnchoredFileSystem::NativeIdentity canonicalIdentity;
    identityError.clear();
    QVERIFY2(
        nativeIdentityForPath(
            sourcePath, canonicalIdentity, identityError),
        qPrintable(identityError));
    QCOMPARE(canonicalIdentity, substituteIdentity);
    QVERIFY(canonicalIdentity != originalIdentity);

    AnchoredFileSystem::NativeIdentity displacedIdentity;
    identityError.clear();
    QVERIFY2(
        nativeIdentityForPath(
            displacedSourcePath,
            displacedIdentity,
            identityError),
        qPrintable(identityError));
    QCOMPARE(displacedIdentity, originalIdentity);
    QVERIFY(!result.recoveryPaths.contains(sourcePath));
}

void TestRideCacheRemoval::
backupReplacementBeforeCommitRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("same-content backup sentinel");
    const QString sourcePath = fixture.activityPath(firstName());
    const QString backupPath = fixture.backupPath(firstName());
    const QString retainedBackupPath =
        backupPath + QStringLiteral(".retained");
    writeFixture(sourcePath, activity);

    bool actionReached = false;
    bool backupReplaced = false;
    AnchoredFileSystem::NativeIdentity publishedIdentity;
    AnchoredFileSystem::NativeIdentity substituteIdentity;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("source-tombstoned"),
        [&] {
            actionReached = true;
            QString error;
            QVERIFY2(
                nativeIdentityForPath(
                    backupPath, publishedIdentity, error),
                qPrintable(error));
            if (!QFile::rename(
                    backupPath, retainedBackupPath)) {
                return;
            }
            backupReplaced = true;
            writeFixture(backupPath, activity);
            error.clear();
            QVERIFY2(
                nativeIdentityForPath(
                    backupPath, substituteIdentity, error),
                qPrintable(error));
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(actionReached);
    if (!backupReplaced) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows file handles prevent this backup replacement race");
#else
        QFAIL("The published backup could not be replaced");
#endif
    }

    QCOMPARE(result.status,
             RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), activity);
    QCOMPARE(readBytes(retainedBackupPath), activity);

    AnchoredFileSystem::NativeIdentity canonicalIdentity;
    QString identityError;
    QVERIFY2(
        nativeIdentityForPath(
            backupPath, canonicalIdentity, identityError),
        qPrintable(identityError));
    QCOMPARE(canonicalIdentity, substituteIdentity);
    QVERIFY(canonicalIdentity != publishedIdentity);

    AnchoredFileSystem::NativeIdentity retainedIdentity;
    identityError.clear();
    QVERIFY2(
        nativeIdentityForPath(
            retainedBackupPath,
            retainedIdentity,
            identityError),
        qPrintable(identityError));
    QCOMPARE(retainedIdentity, publishedIdentity);
    QVERIFY(!result.recoveryPaths.contains(backupPath));
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

    const RideCache::RemovalResult plannedResult =
        fixture.cache->removeRideResult(planned);
    QVERIFY2(
        plannedResult.allLogicallyRemoved(),
        qPrintable(plannedResult.error));
    const RideCache::RemovalResult completedResult =
        fixture.cache->removeRideResult(completed);
    QVERIFY2(
        completedResult.allLogicallyRemoved(),
        qPrintable(completedResult.error));

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
    QVERIFY2(
        third.status == RideCache::RemovalStatus::Committed,
        qPrintable(third.error));
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
    if (!createTestSymbolicLink(
            outsideDirectory, backupDirectory, true)) {
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
        if (!createTestSymbolicLink(
                sentinelPath, linkedSourcePath, false)) {
            QSKIP("File symbolic links are unavailable");
        }
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

void TestRideCacheRemoval::
linkValidationRejectsNullAndDestroyedItems()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const RideCache::OperationPreCheck nullCheck =
        fixture.cache->checkLinkActivities(nullptr, nullptr);
    QVERIFY(!nullCheck.canProceed);
    QCOMPARE(
        nullCheck.blockingReason,
        QStringLiteral("Invalid activities for linking"));

    const RideCache::OperationResult nullResult =
        fixture.cache->linkActivities(nullptr, nullptr);
    QVERIFY(!nullResult.success);
    QCOMPARE(
        nullResult.error,
        QStringLiteral("Invalid activities for linking"));

    RideItem *destroyed = fixture.addRide(firstName(), false);
    RideItem *planned = fixture.addPlannedRide(secondName(), false);
    delete destroyed;

    const RideCache::OperationPreCheck destroyedCheck =
        fixture.cache->checkLinkActivities(destroyed, planned);
    QVERIFY(!destroyedCheck.canProceed);
    QCOMPARE(
        destroyedCheck.blockingReason,
        QStringLiteral(
            "An activity is no longer in the activity list"));

    fixture.cache->mutableRidesForRemovalTest().removeOne(destroyed);
    fixture.cache->garbageCollect();
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
    if (!createTestSymbolicLink(
            outsidePath,
            fixture.activityPath(firstName()),
            false)) {
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
    setRideCacheRemovalMoveAction(
        notesPath,
        [notesPath, concurrentNotes] {
            replaceFixtureWhilePinned(
                notesPath, concurrentNotes);
        });

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
missingDerivedParentCreatedDuringPrepareRejectsRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *item = fixture.addPlannedRide(
        firstName(), true, loadedRide);

    const QByteArray activity("planned activity");
    const QByteArray concurrentCache("concurrent planned cache");
    const QString sourcePath =
        fixture.plannedActivityPath(firstName());
    const QString cachePath = fixture.plannedCachePath(
        firstName(), QStringLiteral("cpx"));
    writeFixture(sourcePath, activity);
    QVERIFY(!QFileInfo::exists(QFileInfo(cachePath).absolutePath()));
    setRideCacheRemovalProcessorAction([&] {
        writeFixture(cachePath, concurrentCache);
    });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(!result.allLogicallyRemoved());
    QVERIFY(!result.cleanlyCompleted());
    QCOMPARE(result.affectedCount, 0);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->rides().constFirst(), item);
    QCOMPARE(fixture.context->ride, item);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(cachePath), concurrentCache);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedBackupPath(firstName())));
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
failedPreviousBackupMovePartialStateRequiresRecovery()
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
    setRideCacheRemovalPartialMoveFailurePath(
        backupPath, false);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QVERIFY(rideCacheRemovalPartialMoveWasTriggered());
    QVERIFY2(
        rideCacheRemovalPartialMoveFailureError().isEmpty(),
        qPrintable(rideCacheRemovalPartialMoveFailureError()));
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QVERIFY(!QFileInfo::exists(backupPath));
    const QString linkedPath =
        rideCacheRemovalPartialMovePublishedLinkPath();
    QVERIFY(linkedPath.endsWith(QStringLiteral(".partial-link")));
    const QString previousPath = linkedPath.left(
        linkedPath.size() - QStringLiteral(".partial-link").size());
    QCOMPARE(readBytes(previousPath), previousBackup);
    QCOMPARE(readBytes(linkedPath), previousBackup);
    QVERIFY(result.recoveryPaths.contains(sourcePath));
    QVERIFY(!result.recoveryPaths.contains(previousPath));
    QVERIFY(!result.recoveryPaths.contains(linkedPath));
    QVERIFY(result.error.contains(
        QStringLiteral("uncertain final location")));
    QVERIFY(recoveryFilesFor(
        backupPath,
        QStringLiteral(".gc-copy-")).isEmpty());
}

void TestRideCacheRemoval::
failedBackupPublishPartialMoveRequiresRecovery()
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
    setRideCacheRemovalPartialMoveFailureTargetPath(
        backupPath, false);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QVERIFY(rideCacheRemovalPartialMoveWasTriggered());
    QVERIFY2(
        rideCacheRemovalPartialMoveFailureError().isEmpty(),
        qPrintable(rideCacheRemovalPartialMoveFailureError()));
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(readBytes(sourcePath), activity);
    QCOMPARE(readBytes(backupPath), activity);
    const QString linkedPath =
        rideCacheRemovalPartialMovePublishedLinkPath();
    QCOMPARE(readBytes(linkedPath), activity);
    const QStringList previousRecovery =
        recoveryFilesFor(
            backupPath,
            QStringLiteral(".gc-previous-"));
    QCOMPARE(previousRecovery.size(), 1);
    QCOMPARE(
        readBytes(previousRecovery.constFirst()),
        previousBackup);
    QVERIFY(result.recoveryPaths.contains(sourcePath));
    QVERIFY(!result.recoveryPaths.contains(backupPath));
    QVERIFY(!result.recoveryPaths.contains(linkedPath));
    QVERIFY(result.recoveryPaths.contains(
        previousRecovery.constFirst()));
    QVERIFY(result.error.contains(
        QStringLiteral("uncertain final location")));
    QVERIFY(recoveryFilesFor(
        backupPath,
        QStringLiteral(".gc-copy-")).isEmpty());
}

void TestRideCacheRemoval::
failedSourceMovePartialStateRequiresRecovery()
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
    setRideCacheRemovalPartialMoveFailurePath(
        sourcePath, false);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::RecoveryRequired);
    QVERIFY(result.requiresRecovery());
    QVERIFY(rideCacheRemovalPartialMoveWasTriggered());
    QVERIFY2(
        rideCacheRemovalPartialMoveFailureError().isEmpty(),
        qPrintable(rideCacheRemovalPartialMoveFailureError()));
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(!QFileInfo::exists(sourcePath));
    const QString linkedPath =
        rideCacheRemovalPartialMovePublishedLinkPath();
    QVERIFY(linkedPath.endsWith(QStringLiteral(".partial-link")));
    const QString tombstonePath = linkedPath.left(
        linkedPath.size() - QStringLiteral(".partial-link").size());
    QCOMPARE(readBytes(tombstonePath), activity);
    QCOMPARE(readBytes(linkedPath), activity);
    QCOMPARE(readBytes(backupPath), activity);
    const QStringList previousRecovery =
        recoveryFilesFor(
            backupPath,
            QStringLiteral(".gc-previous-"));
    QCOMPARE(previousRecovery.size(), 1);
    QCOMPARE(
        readBytes(previousRecovery.constFirst()),
        previousBackup);
    QVERIFY(!result.recoveryPaths.contains(tombstonePath));
    QVERIFY(!result.recoveryPaths.contains(linkedPath));
    QVERIFY(result.recoveryPaths.contains(backupPath));
    QVERIFY(result.recoveryPaths.contains(
        previousRecovery.constFirst()));
    QVERIFY(result.error.contains(
        QStringLiteral("uncertain final location")));
}

void TestRideCacheRemoval::
committedCleanupPartialRemovalRejectsUnverifiedRecoveryPath()
{
#ifndef Q_OS_UNIX
    QSKIP("Exact quarantine recovery paths require Unix removal semantics");
#endif
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray notes("derived notes");
    const QString sourcePath = fixture.activityPath(firstName());
    const QString notesPath = fixture.cachePath(
        firstName(), QStringLiteral("notes"));
    writeFixture(sourcePath, activity);
    writeFixture(notesPath, notes);
    setRideCacheRemovalPartialMoveFailurePath(
        notesPath, false);

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::CommittedCleanupPending);
    QVERIFY(result.allLogicallyRemoved());
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(rideCacheRemovalPartialMoveWasTriggered());
    QVERIFY2(
        rideCacheRemovalPartialMoveFailureError().isEmpty(),
        qPrintable(rideCacheRemovalPartialMoveFailureError()));
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(fixture.cache->count(), 0);
    const QString linkedPath =
        rideCacheRemovalPartialMovePublishedLinkPath();
    QVERIFY(linkedPath.endsWith(QStringLiteral(".partial-link")));
    const QString quarantinePath = linkedPath.left(
        linkedPath.size() - QStringLiteral(".partial-link").size());
    QCOMPARE(readBytes(quarantinePath), notes);
    QCOMPARE(readBytes(linkedPath), notes);
    QVERIFY(!result.recoveryPaths.contains(quarantinePath));
    QVERIFY(!result.recoveryPaths.contains(linkedPath));
    QVERIFY(!result.recoveryPaths.contains(notesPath));
    QCOMPARE(result.recoveryPaths.size(), 1);
    QVERIFY(QFileInfo(result.recoveryPaths.constFirst()).isDir());
    QVERIFY(result.error.contains(
        QStringLiteral("destination changed after publication")));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), activity);
    QVERIFY(!QFileInfo::exists(sourcePath));
}

void TestRideCacheRemoval::
committedCleanupParentReplacementDoesNotReportSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), true);

    const QByteArray activity("live activity");
    const QByteArray notes("derived notes");
    const QByteArray substitute("substitute notes");
    const QString sourcePath = fixture.activityPath(firstName());
    const QString notesPath = fixture.cachePath(
        firstName(), QStringLiteral("notes"));
    const QString cacheDirectory =
        QFileInfo(notesPath).absolutePath();
    const QString displacedDirectory =
        cacheDirectory + QStringLiteral(".displaced");
    writeFixture(sourcePath, activity);
    writeFixture(notesPath, notes);

    bool actionReached = false;
    bool parentReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("commit-marker"),
        [&] {
            actionReached = true;
            if (!QDir().rename(
                    cacheDirectory,
                    displacedDirectory)) {
                return;
            }
            parentReplaced = true;
            QVERIFY(QDir().mkdir(cacheDirectory));
            writeFixture(notesPath, substitute);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(actionReached);
    if (!parentReplaced) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows directory handles prevent this cleanup race");
#else
        QFAIL("The cleanup parent could not be replaced");
#endif
    }

    const QString displacedNotesPath =
        QDir(displacedDirectory).filePath(
            QFileInfo(notesPath).fileName());
    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::CommittedCleanupPending);
    QVERIFY(result.allLogicallyRemoved());
    QVERIFY(!result.cleanlyCompleted());
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(readBytes(notesPath), substitute);
    QCOMPARE(readBytes(displacedNotesPath), notes);
    QVERIFY(!result.recoveryPaths.contains(notesPath));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), activity);
    QVERIFY(!QFileInfo::exists(sourcePath));
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
recoveryPathsRejectReplacedJournalDirectory()
{
#ifndef Q_OS_UNIX
    QSKIP("Live journal directory replacement is blocked on Windows");
#else
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString displacedPath =
        journalPath + QStringLiteral(".displaced");
    bool hookReached = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-recovery-manifest-verified"),
        [&] {
            hookReached = true;
            QVERIFY(QDir().rename(journalPath, displacedPath));
            QVERIFY(QDir().mkpath(journalPath));
            writeFixture(
                QDir(journalPath).filePath(
                    QStringLiteral("manifest.json")),
                QByteArray("substitute manifest"));
        });

    const QStringList recovery = journal->recoveryPaths();

    QVERIFY(hookReached);
    QVERIFY2(!recovery.contains(journalPath),
             "A replacement journal directory was reported for recovery");
#endif
}

void TestRideCacheRemoval::
recoveryPathsRejectCandidateReplacedAfterCollection()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray sourceContents("source");
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, sourceContents);

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString firstCandidate = journal->backupStagingPath();
    const QString secondCandidate = journal->sourceTombstonePath();
    const QString displaced =
        firstCandidate + QStringLiteral(".displaced");
    writeFixture(firstCandidate, sourceContents);
    writeFixture(secondCandidate, sourceContents);
    bool hookReached = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-recovery-candidates-collected"),
        [&] {
            hookReached = true;
            QVERIFY(QFile::rename(firstCandidate, displaced));
            writeFixture(firstCandidate, QByteArray("other!"));
        });

    const QStringList recovery = journal->recoveryPaths();

    QVERIFY(hookReached);
    QVERIFY2(!recovery.contains(firstCandidate),
             "A replaced recovery candidate remained in the result");
    QVERIFY(recovery.contains(secondCandidate));
    QCOMPARE(readBytes(displaced), sourceContents);
    QCOMPARE(readBytes(firstCandidate), QByteArray("other!"));
}

void TestRideCacheRemoval::
recoveryPathsRejectCandidateReplacedAfterFinalJournalCheck()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray sourceContents("source");
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, sourceContents);

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString candidate = journal->backupStagingPath();
    const QString displaced = candidate + QStringLiteral(".displaced");
    writeFixture(candidate, sourceContents);
    bool hookReached = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-recovery-final-journal-verified"),
        [&] {
            hookReached = true;
            QVERIFY(QFile::rename(candidate, displaced));
            writeFixture(candidate, QByteArray("other!"));
        });

    const QStringList recovery = journal->recoveryPaths();

    QVERIFY(hookReached);
    QVERIFY2(!recovery.contains(candidate),
             "A candidate replaced after the journal check was returned");
    QCOMPARE(readBytes(displaced), sourceContents);
    QCOMPARE(readBytes(candidate), QByteArray("other!"));
}

void TestRideCacheRemoval::
staleQLockFileRemovalGuardDoesNotPoisonReconcile_data()
{
    QTest::addColumn<int>("suffixCount");
    QTest::newRow("single-rmlock") << 1;
    QTest::newRow("nested-rmlock") << 2;
}

void TestRideCacheRemoval::
staleQLockFileRemovalGuardDoesNotPoisonReconcile()
{
    QFETCH(int, suffixCount);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString root = fixture.temporary.path();
    const QString namespacePath = QDir(root).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
    QVERIFY(QDir().mkpath(namespacePath));
    writeFixture(
        QDir(namespacePath).filePath(
            qlockRemovalGuardName(suffixCount)),
        QByteArray("stale QLockFile removal guard"));

    QString error;
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(root, error),
        qPrintable(error));
    error.clear();
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(root, error),
        qPrintable(error));

    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {root, sourcePath, fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
}

void TestRideCacheRemoval::
staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile_data()
{
    QTest::addColumn<int>("suffixCount");
    QTest::addColumn<bool>("removeManifest");
    QTest::newRow("single-rmlock") << 1 << false;
    QTest::newRow("nested-rmlock") << 2 << false;
    QTest::newRow("pre-manifest-rmlock") << 1 << true;
}

void TestRideCacheRemoval::
staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile()
{
    QFETCH(int, suffixCount);
    QFETCH(bool, removeManifest);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString root = fixture.temporary.path();
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {root, sourcePath, fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    writeFixture(
        QDir(journalPath).filePath(qlockRemovalGuardName(
            QStringLiteral(".manifest.json.lock"), suffixCount)),
        QByteArray("stale journal QLockFile removal guard"));
    journal.reset();
    if (removeManifest) {
        QVERIFY(QFile::remove(
            QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    }

    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(root, error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    error.clear();
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(root, error),
        qPrintable(error));
}

void TestRideCacheRemoval::
unsafeJournalQLockFileRemovalGuardEntriesRemainRejected_data()
{
    QTest::addColumn<QString>("entryName");
    QTest::addColumn<int>("entryKind");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<QString>("errorFragment");
    QTest::newRow("unknown-lock-target")
        << QStringLiteral(".peer.old.lock.rmlock")
        << int(JournalNamespaceEntryKind::RegularFile)
        << qint64(9) << QString();
    QTest::newRow("suffix-lookalike")
        << QStringLiteral(".manifest.json.lock.rmlock.tmp")
        << int(JournalNamespaceEntryKind::RegularFile)
        << qint64(9) << QString();
    QTest::newRow("recognized-symbolic-link")
        << QStringLiteral(".manifest.json.lock.rmlock")
        << int(JournalNamespaceEntryKind::SymbolicLink)
        << qint64(0) << QStringLiteral("unsafe");
    QTest::newRow("recognized-directory")
        << QStringLiteral(".manifest.json.lock.rmlock")
        << int(JournalNamespaceEntryKind::Directory)
        << qint64(0) << QStringLiteral("unsafe");
    QTest::newRow("recognized-oversized-file")
        << QStringLiteral(".manifest.json.lock.rmlock")
        << int(JournalNamespaceEntryKind::RegularFile)
        << qint64(64 * 1024 + 1) << QStringLiteral("large");
}

void TestRideCacheRemoval::
unsafeJournalQLockFileRemovalGuardEntriesRemainRejected()
{
    QFETCH(QString, entryName);
    QFETCH(int, entryKind);
    QFETCH(qint64, size);
    QFETCH(QString, errorFragment);
    const JournalNamespaceEntryKind kind =
        JournalNamespaceEntryKind(entryKind);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString root = fixture.temporary.path();
    const QString sourcePath = fixture.activityPath(firstName());
    writeFixture(sourcePath, QByteArray("source"));

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {root, sourcePath, fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString entryPath = QDir(journalPath).filePath(entryName);
    const QByteArray contents(int(size), 'x');
    if (kind == JournalNamespaceEntryKind::Directory) {
        QVERIFY(QDir().mkpath(entryPath));
    } else if (kind == JournalNamespaceEntryKind::SymbolicLink) {
        const QString targetPath = QDir(root).filePath(
            QStringLiteral("journal-qlock-removal-guard-target"));
        writeFixture(targetPath, QByteArray("target"));
        if (!createTestSymbolicLink(targetPath, entryPath, false))
            QSKIP("File symbolic links are unavailable");
    } else {
        writeFixture(entryPath, contents);
    }
    journal.reset();

    for (int attempt = 0; attempt < 2; ++attempt) {
        error.clear();
        QVERIFY(!LinkedActivityRemoval::Journal::reconcileAll(root, error));
        QVERIFY2(!error.isEmpty(), qPrintable(entryName));
        if (!errorFragment.isEmpty()) {
            QVERIFY2(
                error.contains(errorFragment, Qt::CaseInsensitive),
                qPrintable(error));
        }
        if (kind == JournalNamespaceEntryKind::RegularFile)
            QCOMPARE(readBytes(entryPath), contents);
    }
}

void TestRideCacheRemoval::
unsafeQLockFileRemovalGuardEntriesRemainRejected_data()
{
    QTest::addColumn<QString>("entryName");
    QTest::addColumn<int>("entryKind");
    QTest::newRow("invalid-uuid")
        << QStringLiteral(".not-a-uuid.lock.rmlock")
        << int(JournalNamespaceEntryKind::RegularFile);
    QTest::newRow("suffix-lookalike")
        << qlockRemovalGuardName() + QStringLiteral(".tmp")
        << int(JournalNamespaceEntryKind::RegularFile);
    QTest::newRow("symbolic-link")
        << qlockRemovalGuardName()
        << int(JournalNamespaceEntryKind::SymbolicLink);
    QTest::newRow("directory")
        << qlockRemovalGuardName()
        << int(JournalNamespaceEntryKind::Directory);
}

void TestRideCacheRemoval::
unsafeQLockFileRemovalGuardEntriesRemainRejected()
{
    QFETCH(QString, entryName);
    QFETCH(int, entryKind);
    const JournalNamespaceEntryKind kind =
        JournalNamespaceEntryKind(entryKind);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString root = fixture.temporary.path();
    const QString namespacePath = QDir(root).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QString entryPath = QDir(namespacePath).filePath(entryName);

    if (kind == JournalNamespaceEntryKind::Directory) {
        QVERIFY(QDir().mkpath(entryPath));
    } else if (kind == JournalNamespaceEntryKind::SymbolicLink) {
        const QString targetPath = QDir(root).filePath(
            QStringLiteral("qlock-removal-guard-target"));
        writeFixture(targetPath, QByteArray("target"));
        if (!createTestSymbolicLink(targetPath, entryPath, false))
            QSKIP("File symbolic links are unavailable");
    } else {
        writeFixture(entryPath, QByteArray("lookalike"));
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        QString error;
        QVERIFY(!LinkedActivityRemoval::Journal::reconcileAll(root, error));
        QVERIFY2(error.contains(entryName), qPrintable(error));
    }
}

void TestRideCacheRemoval::
activeRemovalJournalRejectsDirectorySubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("source");
    const QByteArray peerContents("peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    const QString namespacePath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
    QString journalPath;
    QString displacedPath;
    QByteArray substituteManifest;
    QByteArray substitutePeerOld;
    bool actionReached = false;
    bool directoryReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-initial-manifest-published"),
        [&] {
            actionReached = true;
            const QStringList journals = QDir(namespacePath).entryList(
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                QDir::Name);
            QCOMPARE(journals.size(), 1);
            journalPath = QDir(namespacePath).filePath(
                journals.constFirst());
            displacedPath = journalPath + QStringLiteral(".displaced");
            substituteManifest = readBytes(
                QDir(journalPath).filePath(QStringLiteral("manifest.json")));
            substitutePeerOld = readBytes(
                QDir(journalPath).filePath(QStringLiteral("peer.old")));
            if (!QDir().rename(journalPath, displacedPath)) return;
            directoryReplaced = true;
            QVERIFY(QDir().mkdir(journalPath));
            writeFixture(
                QDir(journalPath).filePath(QStringLiteral("manifest.json")),
                substituteManifest);
            writeFixture(
                QDir(journalPath).filePath(QStringLiteral("peer.old")),
                substitutePeerOld);
        });

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);

    QVERIFY(actionReached);
    if (!directoryReplaced) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows directory handles prevent this journal race");
#else
        QFAIL("The active journal directory could not be replaced");
#endif
    }
    if (journal) {
        error.clear();
        QVERIFY2(
            !journal->cleanupAfterRollback(error),
            "Rollback accepted a substituted journal directory");
    }

    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(displacedPath));
    QCOMPARE(
        readBytes(QDir(journalPath).filePath(QStringLiteral("manifest.json"))),
        substituteManifest);
    QCOMPARE(
        readBytes(QDir(journalPath).filePath(QStringLiteral("peer.old"))),
        substitutePeerOld);
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(peerPath), peerContents);
}

void TestRideCacheRemoval::journalCleanupRejectsNamespaceRedirect()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    QTemporaryDir outside;
    QVERIFY(outside.isValid());

    const QString sourcePath = fixture.activityPath(firstName());
    const QByteArray sourceContents("source");
    writeFixture(sourcePath, sourceContents);

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), QString(), {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QString journalPath = journal->directoryPath();
    const QString id = QFileInfo(journalPath).fileName();
    const QString namespacePath = QFileInfo(journalPath).absolutePath();
    const QString displacedNamespace =
        namespacePath + QStringLiteral(".displaced");
    const QString displacedJournal =
        QDir(displacedNamespace).filePath(id);
    const QString outsideJournal = QDir(outside.path()).filePath(id);
    QVERIFY(QDir().mkdir(outsideJournal));

    bool actionReached = false;
    bool namespaceRedirected = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-directory-finally-inspected"),
        [&] {
            actionReached = true;
            if (!QDir().rename(
                    namespacePath, displacedNamespace)) {
                return;
            }
            namespaceRedirected = createTestSymbolicLink(
                outside.path(), namespacePath, true);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);

    QVERIFY(actionReached);
    if (!namespaceRedirected) {
#ifdef Q_OS_WIN
        QSKIP("Open Windows directory handles prevent this namespace race");
#else
        QFAIL("The journal namespace could not be redirected");
#endif
    }
    QVERIFY2(
        QFileInfo(outsideJournal).isDir(),
        "Cleanup removed a directory outside the anchored namespace");
    QVERIFY2(!cleaned, "Cleanup accepted a redirected journal namespace");
    QVERIFY(QFileInfo(displacedJournal).isDir());
    QCOMPARE(readBytes(sourcePath), sourceContents);
}

void TestRideCacheRemoval::
journalCleanupDoesNotHideNonEmptyQuarantine_data()
{
    QTest::addColumn<bool>("committed");
    QTest::newRow("rollback") << false;
    QTest::newRow("commit") << true;
}

void TestRideCacheRemoval::
journalCleanupDoesNotHideNonEmptyQuarantine()
{
    QFETCH(bool, committed);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString sourcePath = fixture.activityPath(firstName());
    const QString backupPath = fixture.backupPath(firstName());
    const QByteArray sourceContents("source");
    writeFixture(sourcePath, sourceContents);

    QString error;
    const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             backupPath, QString(), {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    if (committed) {
        QVERIFY(QFile::copy(
            sourcePath, journal->backupStagingPath()));
        QVERIFY(QFile::rename(
            journal->backupStagingPath(), backupPath));
        QVERIFY(QFile::rename(
            sourcePath, journal->sourceTombstonePath()));
        QVERIFY2(journal->markCommitted(error), qPrintable(error));
    }

    QString quarantinePath;
    setAnchoredFilesystemTransitionAction(
        QByteArrayLiteral("remove-directory-finally-verified"),
        [&](const QString &target, const QString &) {
            quarantinePath = target;
            writeFixture(
                QDir(target).filePath(QStringLiteral("unexpected")),
                QByteArray("unexpected"));
        });

    error.clear();
    const bool cleaned = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY(!cleaned);
    QVERIFY(!error.isEmpty());
    QVERIFY(!quarantinePath.isEmpty());
    QVERIFY(QFileInfo(QDir(quarantinePath).filePath(
        QStringLiteral("unexpected"))).isFile());

    error.clear();
    const bool retryCleaned = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY2(
        !retryCleaned,
        "A retry hid a nonempty quarantined journal directory");
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo(QDir(quarantinePath).filePath(
        QStringLiteral("unexpected"))).isFile());
}

void TestRideCacheRemoval::
activeRemovalJournalRejectsPeerOldSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("source");
    const QByteArray peerContents("peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    const QString namespacePath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
    QString peerOldPath;
    QString displacedPath;
    QByteArray peerOldContents;
    bool actionReached = false;
    bool peerOldReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-peer-old-published"),
        [&] {
            actionReached = true;
            const QStringList journals = QDir(namespacePath).entryList(
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                QDir::Name);
            QCOMPARE(journals.size(), 1);
            const QString journalPath = QDir(namespacePath).filePath(
                journals.constFirst());
            peerOldPath = QDir(journalPath).filePath(
                QStringLiteral("peer.old"));
            displacedPath = QDir(journalPath).filePath(
                QStringLiteral(".peer.old.attack.tmp"));
            peerOldContents = readBytes(peerOldPath);
            if (!QFile::rename(peerOldPath, displacedPath)) return;
            peerOldReplaced = true;
            writeFixture(peerOldPath, peerOldContents);
        });

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);

    QVERIFY(actionReached);
    if (!peerOldReplaced) {
        QSKIP("The active journal peer copy could not be replaced");
    }
    const bool accepted = bool(journal);
    journal.reset();
    QVERIFY2(
        !accepted,
        "Preparation accepted a substituted journal peer copy");
    QCOMPARE(readBytes(peerOldPath), peerOldContents);
    QCOMPARE(readBytes(displacedPath), peerOldContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(peerPath), peerContents);
}

void TestRideCacheRemoval::
peerOldCleanupRejectsSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("source");
    const QByteArray peerContents("peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QString peerOldPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral("peer.old"));
    const QString displacedPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral(".peer.old.attack.tmp"));
    const QByteArray peerOldContents = readBytes(peerOldPath);
    bool actionReached = false;
    bool peerOldReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-manifest-removed"),
        [&] {
            actionReached = true;
            if (!QFile::rename(peerOldPath, displacedPath)) return;
            peerOldReplaced = true;
            writeFixture(peerOldPath, peerOldContents);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);

    QVERIFY(actionReached);
    if (!peerOldReplaced) {
        QSKIP("The journal peer copy could not be replaced during cleanup");
    }
    QVERIFY2(
        !cleaned,
        "Cleanup removed a substituted journal peer copy");
    QCOMPARE(readBytes(peerOldPath), peerOldContents);
    QCOMPARE(readBytes(displacedPath), peerOldContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(peerPath), peerContents);
}

void TestRideCacheRemoval::
journalTemporaryCleanupRejectsSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QByteArray sourceContents("source");
    const QByteArray temporaryContents("temporary control data");
    writeFixture(sourcePath, sourceContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QString temporaryPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral(".manifest.json.attack.tmp"));
    const QString displacedPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral(".manifest.json.displaced.tmp"));
    writeFixture(temporaryPath, temporaryContents);

    bool actionReached = false;
    bool temporaryReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-temporary-files-inspected"),
        [&] {
            actionReached = true;
            if (!QFile::rename(temporaryPath, displacedPath)) return;
            temporaryReplaced = true;
            writeFixture(temporaryPath, temporaryContents);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);

    QVERIFY(actionReached);
    if (!temporaryReplaced) {
        QSKIP("The journal temporary file could not be replaced during cleanup");
    }
    QVERIFY2(
        !cleaned,
        "Cleanup removed a substituted journal temporary file");
    QCOMPARE(readBytes(temporaryPath), temporaryContents);
    QCOMPARE(readBytes(displacedPath), temporaryContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
}

void TestRideCacheRemoval::
preManifestTemporaryCleanupRejectsSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QByteArray sourceContents("source");
    const QByteArray temporaryContents("temporary control data");
    writeFixture(sourcePath, sourceContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), {}, {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    journal.reset();

    QVERIFY(QFile::remove(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    const QString temporaryPath = QDir(journalPath).filePath(
        QStringLiteral(".manifest.json.attack.tmp"));
    const QString displacedPath = QDir(journalPath).filePath(
        QStringLiteral(".manifest.json.displaced.tmp"));
    writeFixture(temporaryPath, temporaryContents);

    bool actionReached = false;
    bool temporaryReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-pre-manifest-files-inspected"),
        [&] {
            actionReached = true;
            if (!QFile::rename(temporaryPath, displacedPath)) return;
            temporaryReplaced = true;
            writeFixture(temporaryPath, temporaryContents);
        });

    error.clear();
    const bool reconciled = LinkedActivityRemoval::Journal::reconcileAll(
        fixture.temporary.path(), error);

    QVERIFY(actionReached);
    if (!temporaryReplaced) {
        QSKIP("The pre-manifest temporary file could not be replaced");
    }
    QVERIFY2(
        !reconciled,
        "Recovery removed a substituted pre-manifest temporary file");
    QCOMPARE(readBytes(temporaryPath), temporaryContents);
    QCOMPARE(readBytes(displacedPath), temporaryContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
}

void TestRideCacheRemoval::
activeRemovalJournalRejectsManifestSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("source");
    const QByteArray peerContents("peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QString manifestPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral("manifest.json"));
    const QString displacedPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral(".manifest.json.attack.tmp"));
    const QByteArray manifestContents = readBytes(manifestPath);
    QVERIFY(!manifestContents.isEmpty());
    bool actionReached = false;
    bool manifestReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-manifest-inspected"),
        [&] {
            actionReached = true;
            if (!QFile::rename(manifestPath, displacedPath)) return;
            manifestReplaced = true;
            writeFixture(manifestPath, manifestContents);
        });

    error.clear();
    const bool validated = journal->validateOriginalStorage(error);

    QVERIFY(actionReached);
    if (!manifestReplaced) {
        QSKIP("The active journal manifest could not be replaced");
    }
    QVERIFY2(
        !validated,
        "Validation accepted a substituted journal manifest");
    QCOMPARE(readBytes(manifestPath), manifestContents);
    QCOMPARE(readBytes(displacedPath), manifestContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(peerPath), peerContents);
}

void TestRideCacheRemoval::
peerManifestPublicationRejectsManifestSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("source");
    const QByteArray peerContents("peer");
    const QByteArray updatedPeerContents("updated peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(), sourcePath,
             fixture.backupPath(firstName()), peerPath, {}},
            error);
    QVERIFY2(journal, qPrintable(error));

    const QString manifestPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral("manifest.json"));
    const QString displacedPath = QDir(journal->directoryPath()).filePath(
        QStringLiteral(".manifest.json.attack.tmp"));
    const QByteArray manifestContents = readBytes(manifestPath);
    QVERIFY(!manifestContents.isEmpty());

    const AtomicFileWriterFactory factory =
        journal->peerWriterFactory(qSaveFileWriterFactory());
    std::unique_ptr<AtomicFileWriter> writer = factory(
        peerPath, AtomicFileMode::ReplaceExisting);
    QVERIFY(writer);
    QVERIFY2(writer->open(), qPrintable(writer->errorString()));
    QCOMPARE(
        writer->write(updatedPeerContents),
        static_cast<qint64>(updatedPeerContents.size()));
    QVERIFY2(writer->flush(), qPrintable(writer->errorString()));

    bool actionReached = false;
    bool manifestReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("peer-staging-published"),
        [&] {
            actionReached = true;
            if (!QFile::rename(manifestPath, displacedPath)) return;
            manifestReplaced = true;
            writeFixture(manifestPath, manifestContents);
        });

    const bool committed = writer->commit();

    QVERIFY(actionReached);
    if (!manifestReplaced) {
        QSKIP("The active journal manifest could not be replaced");
    }
    QVERIFY2(
        !committed,
        "Peer manifest publication replaced a substituted manifest");
    QVERIFY(!writer->errorString().isEmpty());
    QCOMPARE(readBytes(manifestPath), manifestContents);
    QCOMPARE(readBytes(peerPath), peerContents);

    writer->cancelWriting();
    writer.reset();
    journal.reset();
    QCOMPARE(readBytes(displacedPath), manifestContents);
    QCOMPARE(readBytes(sourcePath), sourceContents);
}

void TestRideCacheRemoval::
commitMarkerReadRejectsMarkerSubstitute()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *item = fixture.addRide(firstName(), true);
    const QString sourcePath = fixture.activityPath(firstName());
    const QByteArray sourceContents("source");
    writeFixture(sourcePath, sourceContents);

    const QString namespacePath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
    QString journalPath;
    QString markerPath;
    QString displacedPath;
    QByteArray markerContents;
    bool actionReached = false;
    bool markerReplaced = false;
    setRideCacheRemovalTransitionAction(
        QByteArrayLiteral("journal-commit-marker-inspected"),
        [&] {
            actionReached = true;
            const QStringList journals = QDir(namespacePath).entryList(
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                QDir::Name);
            QCOMPARE(journals.size(), 1);
            journalPath = QDir(namespacePath).filePath(journals.constFirst());
            markerPath = QDir(journalPath).filePath(
                QStringLiteral("COMMITTED"));
            displacedPath = QDir(journalPath).filePath(
                QStringLiteral(".COMMITTED.attack.tmp"));
            markerContents = readBytes(markerPath);
            if (markerContents.isEmpty()
                || !QFile::rename(markerPath, displacedPath)) {
                return;
            }
            markerReplaced = true;
            writeFixture(markerPath, markerContents);
        });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(item);

    QVERIFY(actionReached);
    QVERIFY(!journalPath.isEmpty());
    if (!markerReplaced) {
        QSKIP("The active journal commit marker could not be replaced");
    }
    QCOMPARE(
        result.status,
        RideCache::RemovalStatus::CommittedCleanupPending);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.recoveryPaths.contains(journalPath));
    QCOMPARE(readBytes(markerPath), markerContents);
    QCOMPARE(readBytes(displacedPath), markerContents);
    QVERIFY(!QFileInfo::exists(sourcePath));
    QCOMPARE(readBytes(fixture.backupPath(firstName())), sourceContents);
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
abandonedPlanJournalBlocksLinkedTransaction_data()
{
    QTest::addColumn<bool>("startSave");
    QTest::newRow("linked-save") << true;
    QTest::newRow("linked-removal") << false;
}

void TestRideCacheRemoval::
abandonedPlanJournalBlocksLinkedTransaction()
{
    QFETCH(bool, startSave);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString completedPath = fixture.activityPath(firstName());
    const QString plannedPath = fixture.plannedActivityPath(secondName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(completedPath, QByteArray("completed source"));
    writeFixture(plannedPath, QByteArray("planned source"));

    PlanReplacement::Specification planSpecification;
    planSpecification.athleteRoot = fixture.temporary.path();
    planSpecification.scopeRoot = fixture.temporary.path();
    planSpecification.inputPaths = {plannedPath};
    planSpecification.removalPaths = {plannedPath};
    planSpecification.targetPaths = {targetPath};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> abandoned =
        PlanReplacement::Journal::prepare(
            planSpecification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString abandonedPath = abandoned->directoryPath();
    abandoned.reset();
    QVERIFY(QFileInfo::exists(abandonedPath));

    if (startSave) {
        LinkedActivitySave::Specification saveSpecification;
        saveSpecification.athleteRoot = fixture.temporary.path();
        saveSpecification.entries = {
            {completedPath,
             fixture.activityPath(thirdName()),
             completedPath + QStringLiteral(".save-bak"),
             false},
            {plannedPath,
             fixture.plannedActivityPath(firstName()),
             plannedPath + QStringLiteral(".save-bak"),
             false}};
        const std::shared_ptr<LinkedActivitySave::Journal> journal =
            LinkedActivitySave::Journal::prepare(
                saveSpecification, error);
        QVERIFY(!journal);
    } else {
        const LinkedActivityRemoval::Specification removalSpecification{
            fixture.temporary.path(),
            completedPath,
            fixture.backupPath(firstName()),
            plannedPath,
            {}};
        const std::shared_ptr<LinkedActivityRemoval::Journal> journal =
            LinkedActivityRemoval::Journal::prepare(
                removalSpecification, error);
        QVERIFY(!journal);
    }
    QVERIFY2(
        error.contains(QStringLiteral("recovery"), Qt::CaseInsensitive),
        qPrintable(error));

    error.clear();
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(
            fixture.temporary.path(), error),
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
startupReconcilesAbandonedPlanReplacementJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::unique_ptr<Context> context(new Context(nullptr));
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), QDir(temporary.path())));
    const QString oldPath = athlete->home->planned().filePath(firstName());
    const QString newPath = athlete->home->planned().filePath(thirdName());
    writeFixture(oldPath, QByteArray("old plan"));

    PlanReplacement::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.scopeRoot = temporary.path();
    specification.inputPaths = {oldPath};
    specification.removalPaths = {oldPath};
    specification.targetPaths = {newPath};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> abandoned =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    writeFixture(
        abandoned->stagingPath(0), QByteArray("new plan"));
    QVERIFY2(abandoned->recordStaged(0, error), qPrintable(error));
    const QString journalPath = abandoned->directoryPath();
    abandoned.reset();

    std::unique_ptr<RideCache> cache(new RideCache(context.get()));
    athlete->rideCache = cache.get();
    QVERIFY2(
        cache->startupRecoveryError().isEmpty(),
        qPrintable(cache->startupRecoveryError()));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(readBytes(oldPath), QByteArray("old plan"));
    QVERIFY(!QFileInfo::exists(newPath));
}

void TestRideCacheRemoval::
startupReportsCorruptPlanReplacementJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::unique_ptr<Context> context(new Context(nullptr));
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), QDir(temporary.path())));
    const QString oldPath = athlete->home->planned().filePath(firstName());
    const QString newPath = athlete->home->planned().filePath(thirdName());
    writeFixture(oldPath, QByteArray("old plan"));

    PlanReplacement::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.scopeRoot = temporary.path();
    specification.inputPaths = {oldPath};
    specification.removalPaths = {oldPath};
    specification.targetPaths = {newPath};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> abandoned =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString journalPath = abandoned->directoryPath();
    abandoned.reset();
    writeFixture(
        QDir(journalPath).filePath(QStringLiteral("manifest.json")),
        QByteArray("{}\n"));

    std::unique_ptr<RideCache> cache(new RideCache(context.get()));
    athlete->rideCache = cache.get();
    QVERIFY(!cache->startupRecoveryError().isEmpty());
    QVERIFY(QFileInfo::exists(journalPath));
    QCOMPARE(readBytes(oldPath), QByteArray("old plan"));
    QVERIFY(!QFileInfo::exists(newPath));
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
    if (!createTestSymbolicLink(
            actualRoot, linkedRoot, true)) {
        QSKIP("Directory symbolic links are unavailable");
    }

    QString error;
    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(
            linkedRoot, error),
        qPrintable(error));
}

void TestRideCacheRemoval::legacyLinkedRemovalManifestRecovers()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString sourcePath = fixture.activityPath(firstName());
    const QString peerPath = fixture.plannedActivityPath(secondName());
    const QByteArray sourceContents("legacy source");
    const QByteArray peerContents("legacy peer");
    writeFixture(sourcePath, sourceContents);
    writeFixture(peerPath, peerContents);

    QString error;
    std::shared_ptr<LinkedActivityRemoval::Journal> journal =
        LinkedActivityRemoval::Journal::prepare(
            {fixture.temporary.path(),
             sourcePath,
             fixture.backupPath(firstName()),
             peerPath,
             {}},
            error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    journal.reset();

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(
        readBytes(manifestPath), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    QJsonObject manifest = document.object();
    QCOMPARE(manifest.value(QStringLiteral("version")).toInt(), 2);
    QVERIFY(manifest.value(QStringLiteral("hasPeer")).toBool());
    manifest.insert(QStringLiteral("version"), 1);
    manifest.remove(QStringLiteral("hasPeer"));
    QByteArray legacyManifest =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    legacyManifest.append('\n');
    writeFixture(manifestPath, legacyManifest);

    QVERIFY2(
        LinkedActivityRemoval::Journal::reconcileAll(
            fixture.temporary.path(), error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(peerPath), peerContents);
    QVERIFY(!QFileInfo::exists(fixture.backupPath(firstName())));
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
#ifdef Q_OS_WIN
    constexpr int ChildFinishTimeoutMs = 30000;
#else
    constexpr int ChildFinishTimeoutMs = 10000;
#endif
    constexpr int ChildTerminationTimeoutMs = 5000;
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
            child.setProcessChannelMode(QProcess::MergedChannels);
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
            if (!child.waitForFinished(ChildFinishTimeoutMs)) {
                child.kill();
                const bool terminated = child.waitForFinished(
                    ChildTerminationTimeoutMs);
                QString diagnostic = QStringLiteral(
                    "child timed out after %1 ms; mode=%2; phase=%3; "
                    "row=%4; terminated=%5; process-error=%6")
                    .arg(ChildFinishTimeoutMs)
                    .arg(childMode)
                    .arg(requestedCrashPhase)
                    .arg(QString::fromLatin1(QTest::currentDataTag()))
                    .arg(terminated ? QStringLiteral("yes")
                                    : QStringLiteral("no"))
                    .arg(child.errorString());
                const QByteArray output = child.readAll();
                if (!output.isEmpty()) {
                    diagnostic.append(QStringLiteral("; output=%1")
                        .arg(QString::fromUtf8(output)));
                }
                return qMakePair(-2, diagnostic);
            }
            return qMakePair(
                child.exitCode(),
                QString::fromUtf8(child.readAll()));
        };

    const auto crashed = runChild(
        QStringLiteral("crash"), crashPhase);
    QVERIFY2(
        crashed.first == 86,
        qPrintable(QStringLiteral("Expected exit 86, got %1: %2")
            .arg(crashed.first).arg(crashed.second)));

    if (!recoveryCrashPhase.isEmpty()) {
        const auto recoveryCrashed = runChild(
            QStringLiteral("recover"),
            recoveryCrashPhase);
        QVERIFY2(
            recoveryCrashed.first == 86,
            qPrintable(QStringLiteral("Expected exit 86, got %1: %2")
                .arg(recoveryCrashed.first)
                .arg(recoveryCrashed.second)));
    }

    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QVERIFY2(
        recovered.first == 0,
        qPrintable(QStringLiteral("Expected exit 0, got %1: %2")
            .arg(recovered.first).arg(recovered.second)));

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
    QVERIFY2(
        recoveredAgain.first == 0,
        qPrintable(QStringLiteral("Expected exit 0, got %1: %2")
            .arg(recoveredAgain.first)
            .arg(recoveredAgain.second)));

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
ordinaryDeletionCrashRecoversOnRestart_data()
{
    QTest::addColumn<QString>("crashPhase");
    QTest::addColumn<int>("crashOccurrence");
    QTest::addColumn<bool>("committed");

    QTest::newRow("journal-directory-created")
        << QStringLiteral("journal-directory-created") << 1 << false;
    QTest::newRow("journal-initial-manifest-published")
        << QStringLiteral("journal-initial-manifest-published")
        << 1 << false;
    QTest::newRow("backup-staged")
        << QStringLiteral("backup-staged") << 1 << false;
    QTest::newRow("previous-backup-preserved")
        << QStringLiteral("previous-backup-preserved") << 1 << false;
    QTest::newRow("backup-published")
        << QStringLiteral("backup-published") << 1 << false;
    QTest::newRow("source-tombstoned")
        << QStringLiteral("source-tombstoned") << 1 << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("commit-marker") << 1 << true;
    for (int occurrence = 1; occurrence <= 5; ++occurrence) {
        QTest::newRow(
            qPrintable(QStringLiteral("cleanup-%1").arg(occurrence)))
            << QStringLiteral("cleanup-file") << occurrence << true;
    }
    for (const QString &phase :
         {QStringLiteral("journal-manifest-removed"),
          QStringLiteral("journal-commit-marker-removed"),
          QStringLiteral("journal-directory-removed")}) {
        QTest::newRow(qPrintable(phase)) << phase << 1 << true;
    }
}

void TestRideCacheRemoval::
ordinaryDeletionCrashRecoversOnRestart()
{
    QFETCH(QString, crashPhase);
    QFETCH(int, crashOccurrence);
    QFETCH(bool, committed);

    static const char RootEnvironment[] =
        "GC_RIDE_CACHE_ORDINARY_REMOVAL_CRASH_ROOT";
    static const char ModeEnvironment[] =
        "GC_RIDE_CACHE_ORDINARY_REMOVAL_CRASH_MODE";
    const QString root = qEnvironmentVariable(RootEnvironment);
    const QString mode = qEnvironmentVariable(ModeEnvironment);

    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(new Context(nullptr));
        std::unique_ptr<Athlete> athlete(
            new Athlete(context.get(), QDir(root)));
        std::unique_ptr<RideCache> cache(new RideCache(context.get()));
        athlete->rideCache = cache.get();

        if (mode == QStringLiteral("recover")) {
            if (!cache->startupRecoveryError().isEmpty())
                std::_Exit(87);
            return;
        }

        RideItem *target = new RideItem(nullptr, context.get());
        target->fileName = firstName();
        target->path = athlete->home->activities().absolutePath();
        cache->mutableRidesForRemovalTest().append(target);
        context->ride = target;

        writeFixture(
            athlete->home->activities().filePath(firstName()),
            QByteArray("newest-activity"));
        writeFixture(
            athlete->home->fileBackup().filePath(
                firstName() + QStringLiteral(".bak")),
            QByteArray("previous-backup"));
        const QString baseName = QFileInfo(firstName()).baseName();
        for (const QString &extension :
             {QStringLiteral("cpx"),
              QStringLiteral("cpi"),
              QStringLiteral("notes")}) {
            writeFixture(
                athlete->home->cache().filePath(
                    baseName + QLatin1Char('.') + extension),
                extension.toLatin1() + "-derived");
        }

        cache->removeRideResult(target);
        QFAIL("ordinary deletion child did not stop at the requested transition");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild = [&](const QString &childMode,
                              const QString &requestedCrashPhase) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral(
            "GC_RIDE_CACHE_REMOVAL_CRASH_PHASE"));
        environment.remove(QStringLiteral(
            "GC_RIDE_CACHE_REMOVAL_CRASH_OCCURRENCE"));
        environment.insert(RootEnvironment, temporary.path());
        environment.insert(ModeEnvironment, childMode);
        environment.insert(
            QStringLiteral("QT_QPA_PLATFORM"),
            QStringLiteral("offscreen"));
        if (!requestedCrashPhase.isEmpty()) {
            environment.insert(
                QStringLiteral("GC_RIDE_CACHE_REMOVAL_CRASH_PHASE"),
                requestedCrashPhase);
            environment.insert(
                QStringLiteral("GC_RIDE_CACHE_REMOVAL_CRASH_OCCURRENCE"),
                QString::number(crashOccurrence));
        }
        child.setProcessEnvironment(environment);
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("ordinaryDeletionCrashRecoversOnRestart:%1")
                 .arg(QString::fromLatin1(QTest::currentDataTag()))});
        if (!child.waitForStarted(5000))
            return qMakePair(-1, child.errorString());
        if (!child.waitForFinished(10000)) {
            child.kill();
            child.waitForFinished();
            return qMakePair(-2, QStringLiteral("child timed out"));
        }
        return qMakePair(
            child.exitCode(), QString::fromUtf8(child.readAll()));
    };

    const auto crashed = runChild(QStringLiteral("delete"), crashPhase);
    QCOMPARE(crashed.first, 86);

    const auto recovered = runChild(QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QDir rootDirectory(temporary.path());
    const QString sourcePath = rootDirectory.filePath(
        QStringLiteral("activities/") + firstName());
    const QString backupPath = rootDirectory.filePath(
        QStringLiteral("bak/") + firstName() + QStringLiteral(".bak"));
    const QString baseName = QFileInfo(firstName()).baseName();
    if (committed) {
        QVERIFY(!QFileInfo::exists(sourcePath));
        QCOMPARE(readBytes(backupPath), QByteArray("newest-activity"));
    } else {
        QCOMPARE(readBytes(sourcePath), QByteArray("newest-activity"));
        QCOMPARE(readBytes(backupPath), QByteArray("previous-backup"));
    }
    for (const QString &extension :
         {QStringLiteral("cpx"),
          QStringLiteral("cpi"),
          QStringLiteral("notes")}) {
        const QString derivedPath = rootDirectory.filePath(
            QStringLiteral("cache/") + baseName + QLatin1Char('.')
            + extension);
        if (committed) {
            QVERIFY(!QFileInfo::exists(derivedPath));
        } else {
            QCOMPARE(
                readBytes(derivedPath),
                extension.toLatin1() + "-derived");
        }
    }

    const bool sourceExists = QFileInfo::exists(sourcePath);
    const QByteArray sourceContents = readBytes(sourcePath);
    const QByteArray backupContents = readBytes(backupPath);
    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);
    QCOMPARE(QFileInfo::exists(sourcePath), sourceExists);
    QCOMPARE(readBytes(sourcePath), sourceContents);
    QCOMPARE(readBytes(backupPath), backupContents);

    const QDir transactionRoot(rootDirectory.filePath(
        QStringLiteral(".gc-transactions/linked-removal")));
    QVERIFY(!transactionRoot.exists()
        || transactionRoot.entryList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QDirIterator artifacts(
        temporary.path(),
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (artifacts.hasNext()) {
        const QString path = artifacts.next();
        const QString name = QFileInfo(path).fileName();
        const bool transactionSidecar =
            name.contains(QStringLiteral(".gc-copy-"))
            || name.contains(QStringLiteral(".gc-remove-"))
            || name.contains(QStringLiteral(".gc-previous-"));
        QVERIFY2(
            !transactionSidecar || name.endsWith(QStringLiteral(".lock")),
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
preexistingDestroyedSiblingDoesNotBreakRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *sibling = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedSibling(sibling);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("sibling activity"));

    delete sibling;
    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedSibling.isNull());
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(fixture.cache->model()->rowCount(), 0);
    QCOMPARE(
        readBytes(fixture.activityPath(secondName())),
        QByteArray("sibling activity"));
}

void TestRideCacheRemoval::
processorDestroyedSelectionNeighborIsPurged()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideFile *const loadedRide =
        reinterpret_cast<RideFile *>(quintptr(1));
    RideItem *target = fixture.addRide(
        firstName(), true, loadedRide);
    RideItem *sibling = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedSibling(sibling);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("sibling activity"));
    setRideCacheRemovalProcessorAction(
        [sibling] { delete sibling; });

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedSibling.isNull());
    QVERIFY(fixture.context->ride == nullptr);
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(fixture.cache->model()->rowCount(), 0);
    QCOMPARE(
        readBytes(fixture.activityPath(secondName())),
        QByteArray("sibling activity"));
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
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        targetAddress));
}

void TestRideCacheRemoval::
modelRemovalSignalReaderSkipsDestroyedItem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    fixture.addRide(secondName(), false);
    QStringList observedFilenames;

    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [target] { delete target; });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [&fixture, &observedFilenames] {
            observedFilenames = fixture.cache->getAllFilenames();
        });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(observedFilenames, QStringList{secondName()});
}

void TestRideCacheRemoval::modelRemovalPublishesOnlyLiveSnapshot()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *survivor = fixture.addRide(secondName(), false);
    const int targetRow = fixture.cache->rides().indexOf(target);
    QVERIFY(targetRow >= 0);
    QVector<RideItem*> observedRides;
    int observedCount = -1;
    int observedModelRows = -1;
    QVariant observedTargetData;

    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [target] { delete target; });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [&] {
            observedRides = fixture.cache->rides();
            observedCount = fixture.cache->count();
            observedModelRows = fixture.cache->model()->rowCount();
            observedTargetData = fixture.cache->model()->data(
                fixture.cache->model()->index(targetRow, 1));
        });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QCOMPARE(observedRides, QVector<RideItem*>{survivor});
    QCOMPARE(observedCount, 1);
    QCOMPARE(observedModelRows, 2);
    QVERIFY(!observedTargetData.isValid());
}

void TestRideCacheRemoval::
ordinaryRemovalStopsEstimatorBeforeRowsAbout()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    bool estimatorWasStopped = false;

    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [&] {
            estimatorWasStopped =
                rideCacheRemovalEstimatorStopCount() == 1;
        });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QVERIFY(estimatorWasStopped);
}

void TestRideCacheRemoval::refreshRequestedDuringRemovalIsDeferred()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    bool refreshWasDeferred = false;

    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsAboutToBeRemoved,
        [&] {
            fixture.cache->refresh();
            refreshWasDeferred =
                rideCacheRemovalRefreshCount() == 0;
        });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QVERIFY(refreshWasDeferred);
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
rowsRemovedGarbageCollectSkipsDestroyedAddress()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    const QPointer<RideItem> guardedTarget(target);

    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsRemoved,
        [target] { delete target; });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::rowsRemoved,
        [&] { fixture.cache->garbageCollect(); });
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QVERIFY(guardedTarget.isNull());
    QCOMPARE(fixture.cache->pendingDeletionCountForRemovalTest(), 0);
}

void TestRideCacheRemoval::modelRemovalPurgesDestroyedSibling_data()
{
    QTest::addColumn<bool>("destroyBeforeRowRemoval");

    QTest::newRow("rows-about-to-be-removed") << true;
    QTest::newRow("rows-removed") << false;
}

void TestRideCacheRemoval::modelRemovalPurgesDestroyedSibling()
{
    QFETCH(bool, destroyBeforeRowRemoval);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *target = fixture.addRide(firstName(), true);
    RideItem *sibling = fixture.addRide(secondName(), false);
    RideItem *survivor = fixture.addRide(thirdName(), false);
    RideItem *const siblingAddress = sibling;
    const QPointer<RideItem> guardedSibling(sibling);
    const auto destroySibling = [sibling] { delete sibling; };
    if (destroyBeforeRowRemoval) {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            destroySibling);
    } else {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::rowsRemoved,
            destroySibling);
    }
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("target activity"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("sibling activity"));

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(target);

    const QVector<RideItem*> remainingRides = fixture.cache->rides();
    const int remainingModelRows = fixture.cache->model()->rowCount();
    const int remainingCount = fixture.cache->count();
    const bool rememberedSibling =
        fixture.cache->remembersDeletedAddressForRemovalTest(
            siblingAddress);
    fixture.cache->mutableRidesForRemovalTest().removeOne(
        siblingAddress);

    QCOMPARE(result.status, RideCache::RemovalStatus::Committed);
    QVERIFY(guardedSibling.isNull());
    QCOMPARE(remainingRides, QVector<RideItem*>{survivor});
    QCOMPARE(remainingModelRows, 1);
    QCOMPARE(remainingCount, 1);
    QVERIFY(!rememberedSibling);
    QCOMPARE(
        readBytes(fixture.activityPath(secondName())),
        QByteArray("sibling activity"));
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

    const auto destroyOwner = [
        &fixture, destroyCache, destroyBeforeRowRemoval,
        target, survivor] {
        if (destroyCache) {
            fixture.athlete->rideCache = nullptr;
            fixture.context->ride = nullptr;
            RideCache *const owner = fixture.cache.release();
            if (destroyBeforeRowRemoval)
                owner->deleteLater();
            else
                delete owner;
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
    if (destroyCache && destroyBeforeRowRemoval) {
        QCoreApplication::sendPostedEvents(
            nullptr, QEvent::DeferredDelete);
    }

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
        [&fixture, destroyCache, destroyBeforeRowRemoval,
         target, survivor] {
            if (destroyCache) {
                fixture.athlete->rideCache = nullptr;
                fixture.context->ride = nullptr;
                RideCache *const owner = fixture.cache.release();
                if (destroyBeforeRowRemoval)
                    owner->deleteLater();
                else
                    delete owner;
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
    if (destroyCache && destroyBeforeRowRemoval) {
        QCoreApplication::sendPostedEvents(
            nullptr, QEvent::DeferredDelete);
    }

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
                     .arg(backupInfo.fileName()),
                 backupInfo.fileName()
                     + QStringLiteral(".gc-copy-*")},
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
            replaceFixtureWhilePinned(
                firstPath, changedContents);
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
    QVERIFY(!result.recoveryPaths.contains(retainedStagingPath));
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

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(planned);
    QVERIFY2(
        result.allLogicallyRemoved(),
        qPrintable(result.error));

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
temporaryRideItemDestructionDoesNotCreateCacheTombstone()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *temporary = new RideItem(nullptr, fixture.context.get());
    RideItem *const address = temporary;

    delete temporary;

    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        address));
    QCOMPARE(rideCacheRemovalStartupInvalidationCount(), 0);
}

void TestRideCacheRemoval::
tombstonedCacheEntryIsNotDereferencedOrRemoved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), false);
    fixture.cache->markDeletedAddressForRemovalTest(item);

    const QModelIndex fileNameIndex =
        fixture.cache->model()->index(0, 1);
    QVERIFY(fileNameIndex.isValid());
    QVERIFY(!fixture.cache->model()->data(
        fileNameIndex, Qt::DisplayRole).isValid());

    const RideCache::RemovalResult result =
        fixture.cache->removeRideResult(firstName());
    QCOMPARE(result.status, RideCache::RemovalStatus::Rejected);
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(fixture.cache->model()->rowCount(), 1);
    QCOMPARE(
        fixture.cache->mutableRidesForRemovalTest().size(),
        1);
}

void TestRideCacheRemoval::
removalPrecheckRejectsDestroyedCacheAddress()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *const item = fixture.addRide(firstName(), false);
    RideItem *const destroyedAddress = item;
    delete item;
    QVERIFY(
        fixture.cache->remembersDeletedAddressForRemovalTest(
            destroyedAddress));
    QCOMPARE(rideCacheRemovalStartupInvalidationCount(), 1);

    const RideCache::OperationPreCheck check =
        fixture.cache->checkRemovalLinks(destroyedAddress);
    QVERIFY(!check.canProceed);
    QCOMPARE(
        check.blockingReason,
        QStringLiteral(
            "The activity is no longer in the activity list"));

    fixture.cache->mutableRidesForRemovalTest().removeOne(
        destroyedAddress);
    fixture.cache->garbageCollect();
}

void TestRideCacheRemoval::
navigationRideEventDoesNotRetainDestroyedItem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *item = fixture.addRide(firstName(), false);
    fixture.cache->mutableRidesForRemovalTest().removeOne(item);
    NavigationEvent event(NavigationEvent::RIDE, item);

    QCOMPARE(
        event.after.metaType(),
        QMetaType::fromType<QPointer<RideItem>>());
    delete item;

    QVERIFY(event.after.value<QPointer<RideItem>>().isNull());
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

void TestRideCacheRemoval::
plannedReplacementPurgesPreexistingDestroyedRow()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *unrelated = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedUnrelated(unrelated);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString unrelatedPath = fixture.activityPath(secondName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(unrelatedPath, QByteArray("unrelated activity"));
    delete unrelated;
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedUnrelated.isNull());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->model()->rowCount(), 1);
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(targetPath), QByteArray("new third"));
    QCOMPARE(readBytes(unrelatedPath), QByteArray("unrelated activity"));
}

void TestRideCacheRemoval::
plannedReplacementStageFailurePreservesOldGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString secondPath = fixture.plannedActivityPath(secondName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(secondPath, QByteArray("old second"));

    QList<RideCache::PlannedActivityTarget> targets;
    targets.append({
        secondName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new second"), error);
        }});
    targets.append({
        thirdName(),
        [](const QString &, QString &error) {
            error = QStringLiteral("injected second-stage failure");
            return false;
        }});

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first, second}, {firstPath, secondPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QCOMPARE(readBytes(secondPath), QByteArray("old second"));
    QVERIFY(!QFileInfo::exists(thirdPath));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QVERIFY(!cacheContains(*fixture.cache, thirdName()));
}

void TestRideCacheRemoval::
plannedReplacementStageFailureResumesBackgroundWork()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &, QString &error) {
            error = QStringLiteral("injected staging failure");
            return false;
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QCoreApplication::processEvents();

    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementCommitsOneCompleteGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString secondPath = fixture.plannedActivityPath(secondName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(secondPath, QByteArray("old second"));

    QList<RideCache::PlannedActivityTarget> targets;
    targets.append({
        secondName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new second"), error);
        }});
    targets.append({
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }});

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first, second}, {firstPath, secondPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(result.removedCount, 2);
    QCOMPARE(result.addedCount, 2);
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(secondPath), QByteArray("new second"));
    QCOMPARE(readBytes(thirdPath), QByteArray("new third"));
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
}

void TestRideCacheRemoval::
planBundleImportPublishesOneCompleteGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QDateTime sourceDateTime(
        QDate(2026, 1, 5), QTime(8, 30));
    const QDateTime targetDateTime(
        QDate(2026, 3, 9), QTime(8, 30));
    PlanBundleReader reader(
        fixture.context.get(), targetDateTime.date());
    QVERIFY(preparePlanBundleReader(
        reader, sourceDateTime, targetDateTime));

    const PlanResult result = reader.importBundle();

    QVERIFY2(result.ok(), qPrintable(result.errors.join(';')));
    QVERIFY(result.committed);
    const QString targetName = targetDateTime.toString(
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
        + QStringLiteral(".json");
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(targetName)));
    QVERIFY(cacheContains(*fixture.cache, targetName));
    QCOMPARE(fixture.cache->count(), 1);
}

void TestRideCacheRemoval::
planBundleImportCommitsWorkoutFileAndDatabaseRow()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QString workoutRoot = QDir(
        fixture.temporary.path()).filePath(
            QStringLiteral("workout-library"));
    const QString databaseRoot = QDir(
        fixture.temporary.path()).filePath(
            QStringLiteral("workout-database"));
    QVERIFY(QDir().mkpath(workoutRoot));
    QVERIFY(QDir().mkpath(databaseRoot));
    setPlanBundleWorkoutRootForTest(workoutRoot);

    const QByteArray workoutContents(
        "[COURSE HEADER]\nVERSION = 2\n[END COURSE HEADER]\n");
    const QString workoutHash = QString::fromLatin1(
        QCryptographicHash::hash(
            workoutContents,
            QCryptographicHash::Md5).toHex());
    const QString packedWorkout = workoutHash
        + QStringLiteral("-threshold.erg");
    const QDateTime sourceDateTime(
        QDate(2026, 1, 5), QTime(8, 30));
    const QDateTime targetDateTime(
        QDate(2026, 3, 9), QTime(8, 30));
    PlanBundleReader reader(
        fixture.context.get(), targetDateTime.date());
    QVERIFY(preparePlanBundleReader(
        reader, sourceDateTime, targetDateTime,
        packedWorkout));
    writeFixture(
        reader.workoutDir.filePath(packedWorkout),
        workoutContents);

    TrainDB database{QDir(databaseRoot)};
    TrainDB *const previousDatabase = trainDB;
    trainDB = &database;
    const PlanResult result = reader.importBundle();
    trainDB = previousDatabase;
    setPlanBundleWorkoutRootForTest(QString());

    QVERIFY2(result.ok(), qPrintable(result.errors.join(';')));
    QVERIFY(result.committed);
    const QString targetWorkout = QDir(workoutRoot).filePath(
        QStringLiteral("threshold.erg"));
    QCOMPARE(readBytes(targetWorkout), workoutContents);
    QVERIFY(database.hasWorkout(targetWorkout));
    const QString targetActivityName = targetDateTime.toString(
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
        + QStringLiteral(".json");
    const QByteArray activityContents = readBytes(
        fixture.plannedActivityPath(targetActivityName));
    QVERIFY(activityContents.contains(
        targetWorkout.toUtf8()));
    TrainDB::PlanImportJournal pending;
    bool found = false;
    QString journalError;
    QVERIFY2(database.loadPlanImportJournal(
                 fixture.athlete->home->root().canonicalPath(),
                 pending, found, journalError),
             qPrintable(journalError));
    QVERIFY(!found);
}

void TestRideCacheRemoval::
planBundleImportStageFailurePreservesOldGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QDateTime sourceDateTime(
        QDate(2026, 1, 5), QTime(8, 30));
    const QDateTime targetDateTime(
        QDate(2026, 3, 9), QTime(8, 30));
    const QString targetName = targetDateTime.toString(
        QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
        + QStringLiteral(".json");
    RideItem *old = fixture.addPlannedRide(
        targetName, false);
    old->dateTime = targetDateTime;
    const QString oldPath =
        fixture.plannedActivityPath(targetName);
    writeFixture(
        oldPath, QByteArray("old plan generation"));
    QCOMPARE(
        readBytes(oldPath),
        QByteArray("old plan generation"));

    PlanBundleReader reader(
        fixture.context.get(), targetDateTime.date());
    QVERIFY(preparePlanBundleReader(
        reader, sourceDateTime, targetDateTime));
    setRideCacheCalendarStorageAction([old] {
        old->isdirty = true;
    });

    const PlanResult result = reader.importBundle();
    old->isdirty = false;

    QVERIFY(!result.ok());
    QVERIFY(!result.committed);
    QCOMPARE(
        readBytes(oldPath),
        QByteArray("old plan generation"));
    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(fixture.cache->rides().contains(old));
    QVERIFY(cacheContains(*fixture.cache, targetName));
}

void TestRideCacheRemoval::
plannedReplacementQuiescesWorkersAndDefersRefresh()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    bool estimatorWasStoppedBeforeStaging = false;
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [&estimatorWasStoppedBeforeStaging]
        (const QString &path, QString &error) {
            estimatorWasStoppedBeforeStaging =
                rideCacheRemovalEstimatorStopCount() == 1;
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(estimatorWasStoppedBeforeStaging);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QCoreApplication::processEvents();

    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementBlocksRefreshRestartDuringCallbacks()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    bool refreshWasBlocked = false;
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [&fixture, &refreshWasBlocked]
        (const QString &path, QString &error) {
            fixture.cache->refresh();
            refreshWasBlocked =
                rideCacheRemovalRefreshCount() == 0;
            return stageBytes(
                path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(refreshWasBlocked);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QCoreApplication::processEvents();

    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementRefreshDoesNotLeakIntoNextRemoval()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [&fixture](const QString &path, QString &error) {
            fixture.cache->refresh();
            return stageBytes(
                path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult replacement =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(
        replacement.cleanlyCompleted(),
        qPrintable(replacement.error));
    QCoreApplication::processEvents();
    resetRideCacheRemovalRefreshCounts();

    RideItem *second = fixture.addRide(secondName(), false);
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("second"));
    QVERIFY(fixture.cache->removeRides({second}, false));

    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::addRidePurgesPreexistingDestroyedRow()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *destroyed = fixture.addRide(firstName(), false);
    RideItem *survivor = fixture.addRide(thirdName(), false);
    QVERIFY(RideFile::parseRideFileName(
        survivor->fileName, &survivor->dateTime));
    const RideItem *const destroyedAddress = destroyed;
    delete destroyed;
    QVERIFY(fixture.cache->remembersDeletedAddressForRemovalTest(
        const_cast<RideItem *>(destroyedAddress)));

    fixture.cache->addRide(
        secondName(), false, false, false, false);

    const bool tombstoneWasPurged =
        !fixture.cache->remembersDeletedAddressForRemovalTest(
            const_cast<RideItem *>(destroyedAddress));
    if (!tombstoneWasPurged) {
        fixture.cache->mutableRidesForRemovalTest().removeAll(
            const_cast<RideItem *>(destroyedAddress));
    }
    QVERIFY(tombstoneWasPurged);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        fixture.cache->getAllFilenames(),
        QStringList({secondName(), thirdName()}));
}

void TestRideCacheRemoval::addRidesPurgesPreexistingDestroyedRow()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *destroyed = fixture.addRide(firstName(), false);
    RideItem *survivor = fixture.addRide(thirdName(), false);
    QVERIFY(RideFile::parseRideFileName(
        survivor->fileName, &survivor->dateTime));
    RideItem *const destroyedAddress = destroyed;
    delete destroyed;

    const QVector<RideItem *> added = fixture.cache->addRides(
        QStringList{secondName()}, {}, false, false, false, false);

    const bool tombstoneWasPurged =
        !fixture.cache->remembersDeletedAddressForRemovalTest(
            destroyedAddress);
    if (!tombstoneWasPurged) {
        fixture.cache->mutableRidesForRemovalTest().removeAll(
            destroyedAddress);
    }
    QVERIFY(tombstoneWasPurged);
    QCOMPARE(added.size(), 1);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        fixture.cache->getAllFilenames(),
        QStringList({secondName(), thirdName()}));
}

void TestRideCacheRemoval::addRidePurgesRowDestroyedDuringReset()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *destroyed = fixture.addRide(firstName(), false);
    RideItem *survivor = fixture.addRide(thirdName(), false);
    QVERIFY(RideFile::parseRideFileName(
        survivor->fileName, &survivor->dateTime));
    RideItem *const destroyedAddress = destroyed;
    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [destroyed, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete destroyed;
        });

    fixture.cache->addRide(
        secondName(), false, false, false, false);

    const bool tombstoneWasPurged =
        !fixture.cache->remembersDeletedAddressForRemovalTest(
            destroyedAddress);
    if (!tombstoneWasPurged) {
        fixture.cache->mutableRidesForRemovalTest().removeAll(
            destroyedAddress);
    }
    QVERIFY(tombstoneWasPurged);
    QCOMPARE(fixture.cache->count(), 2);
    QCOMPARE(
        fixture.cache->getAllFilenames(),
        QStringList({secondName(), thirdName()}));
}

void TestRideCacheRemoval::duplicateSingleImportRetiresReplacedItem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.cache->addRide(
        firstName(), false, true, false, false);
    RideItem *const replaced =
        cacheItem(*fixture.cache, firstName());
    QVERIFY(replaced);
    const QPointer<RideItem> guardedReplaced(replaced);

    int aboutToResetCount = 0;
    int resetCount = 0;
    bool aliveDuringAboutToReset = false;
    bool aliveDuringReset = false;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        [&] {
            ++aboutToResetCount;
            aliveDuringAboutToReset = guardedReplaced;
        });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&] {
            ++resetCount;
            aliveDuringReset = guardedReplaced;
        });
    QSignalSpy changedSpy(
        fixture.cache.get(),
        QOverload<RideItem *>::of(&RideCache::itemChanged));
    RideItem *announcedSelection = nullptr;
    connect(
        fixture.context.get(), &Context::rideSelected,
        [&](RideItem *item) { announcedSelection = item; });
    resetRideCacheRemovalRefreshCounts();

    fixture.cache->addRide(
        firstName(), false, false, false, false);

    RideItem *const replacement =
        cacheItem(*fixture.cache, firstName());
    QVERIFY(replacement);
    QVERIFY(replacement != replaced);
    QCOMPARE(aboutToResetCount, 1);
    QCOMPARE(resetCount, 1);
    QVERIFY(aliveDuringAboutToReset);
    QVERIFY(aliveDuringReset);
    QVERIFY(guardedReplaced);
    QCOMPARE(fixture.context->ride, replacement);
    QCOMPARE(announcedSelection, replacement);
    QCOMPARE(
        fixture.cache->pendingDeletionCountForRemovalTest(),
        qsizetype(1));

    guardedReplaced->notifyRideMetadataChanged();
    QCOMPARE(changedSpy.count(), 0);

    fixture.cache->garbageCollect();
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedReplaced.isNull());
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 1);
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 1);
}

void TestRideCacheRemoval::
duplicateSingleImportRetiresOldWhenReplacementIsDestroyed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.cache->addRide(
        firstName(), false, true, false, false);
    RideItem *const replaced =
        cacheItem(*fixture.cache, firstName());
    QVERIFY(replaced);
    const QPointer<RideItem> guardedReplaced(replaced);

    bool replacementDestroyed = false;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&] {
            RideItem *const replacement =
                cacheItem(*fixture.cache, firstName());
            if (replacement && replacement != replaced) {
                replacementDestroyed = true;
                delete replacement;
            }
        });
    QSignalSpy changedSpy(
        fixture.cache.get(),
        QOverload<RideItem *>::of(&RideCache::itemChanged));
    resetRideCacheRemovalRefreshCounts();

    fixture.cache->addRide(
        firstName(), false, false, false, false);

    QVERIFY(replacementDestroyed);
    QVERIFY(guardedReplaced);
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 1);
    QCOMPARE(fixture.context->ride, nullptr);
    QCOMPARE(
        fixture.cache->pendingDeletionCountForRemovalTest(),
        qsizetype(1));
    guardedReplaced->notifyRideMetadataChanged();
    QCOMPARE(changedSpy.count(), 0);

    fixture.cache->garbageCollect();
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedReplaced.isNull());
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 2);
}

void TestRideCacheRemoval::
duplicateBatchImportRetiresEveryDisplacedItem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    fixture.cache->addRide(
        firstName(), false, true, false, false);
    fixture.cache->addRide(
        secondName(), false, false, false, false);
    RideItem *const replacedFirst =
        cacheItem(*fixture.cache, firstName());
    RideItem *const replacedSecond =
        cacheItem(*fixture.cache, secondName());
    QVERIFY(replacedFirst);
    QVERIFY(replacedSecond);
    const QPointer<RideItem> guardedFirst(replacedFirst);
    const QPointer<RideItem> guardedSecond(replacedSecond);

    int aboutToResetCount = 0;
    int resetCount = 0;
    bool displacedAliveDuringAboutToReset = false;
    bool displacedAliveDuringReset = false;
    int destroyedDuringReset = -1;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        [&] {
            ++aboutToResetCount;
            displacedAliveDuringAboutToReset =
                guardedFirst && guardedSecond;
        });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&] {
            ++resetCount;
            displacedAliveDuringReset =
                guardedFirst && guardedSecond;
            destroyedDuringReset =
                rideCacheRemovalRideItemDestructionCount();
        });
    QSignalSpy changedSpy(
        fixture.cache.get(),
        QOverload<RideItem *>::of(&RideCache::itemChanged));
    RideItem *announcedSelection = nullptr;
    connect(
        fixture.context.get(), &Context::rideSelected,
        [&](RideItem *item) { announcedSelection = item; });
    resetRideCacheRemovalRefreshCounts();

    const QVector<RideItem *> imported =
        fixture.cache->addRides(
            {firstName(), firstName(), secondName(), thirdName()},
            {}, false, false, false, false);

    RideItem *const firstReplacement =
        cacheItem(*fixture.cache, firstName());
    QVERIFY(firstReplacement);
    QVERIFY(firstReplacement != replacedFirst);
    QVERIFY(cacheItem(*fixture.cache, secondName()) != replacedSecond);
    QCOMPARE(imported.size(), 3);
    QCOMPARE(fixture.cache->count(), 3);
    QCOMPARE(aboutToResetCount, 1);
    QCOMPARE(resetCount, 1);
    QVERIFY(displacedAliveDuringAboutToReset);
    QVERIFY(displacedAliveDuringReset);
    QCOMPARE(destroyedDuringReset, 0);
    QVERIFY(guardedFirst);
    QVERIFY(guardedSecond);
    QCOMPARE(fixture.context->ride, firstReplacement);
    QCOMPARE(announcedSelection, firstReplacement);
    QCOMPARE(
        fixture.cache->pendingDeletionCountForRemovalTest(),
        qsizetype(3));

    guardedFirst->notifyRideMetadataChanged();
    guardedSecond->notifyRideMetadataChanged();
    QCOMPARE(changedSpy.count(), 0);

    fixture.cache->garbageCollect();
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedFirst.isNull());
    QVERIFY(guardedSecond.isNull());
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 3);
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QCOMPARE(rideCacheRemovalRideItemDestructionCount(), 3);
}

void TestRideCacheRemoval::consecutiveAddsCoalesceBackgroundResume()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    fixture.cache->addRide(
        firstName(), false, false, false, false);
    fixture.cache->addRide(
        secondName(), false, false, false, false);

    QCOMPARE(rideCacheRemovalCancelCount(), 2);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 2);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QTRY_COMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::mutationScopePublishesOnlyLiveSortedRows()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    RideItem *first = fixture.addRide(firstName(), false);
    RideItem *third = fixture.addRide(thirdName(), false);
    QVERIFY(RideFile::parseRideFileName(
        first->fileName, &first->dateTime));
    QVERIFY(RideFile::parseRideFileName(
        third->fileName, &third->dateTime));

    QString error;
    RideCacheMutationScope mutation(fixture.cache.get(), error);
    QVERIFY2(mutation.ready(), qPrintable(error));
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);

    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [first, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete first;
        });

    QVERIFY2(
        mutation.resetAndSort(error, [] { return true; }),
        qPrintable(error));
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(first));
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(fixture.cache->getAllFilenames(), QStringList{thirdName()});
}

void TestRideCacheRemoval::
mutationScopeDefersConfigAndReleasesModelReservation()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideCacheModel *const model = fixture.cache->model();
    int resetCount = 0;
    connect(
        model, &QAbstractItemModel::modelReset,
        [&resetCount] { ++resetCount; });

    {
        QString error;
        RideCacheMutationScope mutation(fixture.cache.get(), error);
        QVERIFY2(mutation.ready(), qPrintable(error));
        QVERIFY(!model->startInsert(0, 0));
        fixture.context->configChanged(0x20);
        QCOMPARE(resetCount, 0);
    }

    QCoreApplication::processEvents();
    QCOMPARE(resetCount, 1);
    QVERIFY(model->startInsert(0, 0));
    model->endInsert();
}

void TestRideCacheRemoval::moveActivityPurgesDestroyedRowsBeforeReorder()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime originalDateTime;
    QDateTime targetDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &originalDateTime));
    QVERIFY(RideFile::parseRideFileName(thirdName(), &targetDateTime));
    RideFile *ride = new RideFile(originalDateTime, 1.0);
    RideItem *target = fixture.addRide(firstName(), true, ride);
    target->dateTime = originalDateTime;
    writeFixture(fixture.activityPath(firstName()), QByteArray("activity"));
    RideItem *destroyed = fixture.addRide(secondName(), false);
    RideItem *const destroyedAddress = destroyed;
    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [destroyed, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete destroyed;
        });

    const RideCache::OperationResult result =
        fixture.cache->moveActivity(target, targetDateTime);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QVERIFY(result.cleanupComplete);
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(target->fileName, thirdName());
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QVERIFY(QFileInfo::exists(fixture.activityPath(thirdName())));
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        destroyedAddress));
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);
    delete ride;
}

void TestRideCacheRemoval::copyPlannedActivityPurgesDestroyedRowsBeforePublish()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &sourceDateTime));
    RideItem *source = fixture.addPlannedRide(firstName(), false);
    source->dateTime = sourceDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned source"));
    RideItem *destroyed = fixture.addRide(secondName(), false);
    RideItem *const destroyedAddress = destroyed;
    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [destroyed, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete destroyed;
        });

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivity(
            source, sourceDateTime.date().addDays(1));

    QVERIFY(!result.success);
    QVERIFY(result.committed);
    QVERIFY(!result.cacheUpdated);
    QVERIFY(result.cleanupComplete);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(fixture.cache->count(), 2);
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        destroyedAddress));
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);
}

void TestRideCacheRemoval::copyPlannedActivityUsesRequestedTime()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &sourceDateTime));
    RideItem *source = fixture.addPlannedRide(firstName(), false);
    source->dateTime = sourceDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned source"));
    const QTime requestedTime(12, 34, 56);

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivity(
            source, sourceDateTime.date().addDays(1),
            requestedTime);

    const QString targetName =
        QStringLiteral("2026_07_07_12_34_56.json");
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QVERIFY(result.cleanupComplete);
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(cacheContains(*fixture.cache, targetName));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(targetName)),
        QByteArray("planned source"));
}

void TestRideCacheRemoval::copyPlannedActivitiesPurgesDestroyedRowsBeforePublish()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime firstDateTime;
    QDateTime secondDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &firstDateTime));
    QVERIFY(RideFile::parseRideFileName(secondName(), &secondDateTime));
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    first->dateTime = firstDateTime;
    second->dateTime = secondDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()), QByteArray("first"));
    writeFixture(
        fixture.plannedActivityPath(secondName()), QByteArray("second"));
    RideItem *destroyed = fixture.addRide(thirdName(), false);
    RideItem *const destroyedAddress = destroyed;
    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [destroyed, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete destroyed;
        });

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivities({
            {first, firstDateTime.date().addDays(1)},
            {second, secondDateTime.date().addDays(2)}});

    QVERIFY(!result.success);
    QVERIFY(result.committed);
    QVERIFY(!result.cacheUpdated);
    QVERIFY(result.cleanupComplete);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.affectedCount, 2);
    QCOMPARE(fixture.cache->count(), 4);
    const QStringList filenames =
        fixture.cache->getAllFilenames();
    QVERIFY(filenames.contains(
        QStringLiteral("2026_07_07_08_00_00.json")));
    QVERIFY(filenames.contains(
        QStringLiteral("2026_07_08_09_00_00.json")));
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        destroyedAddress));
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);
}

void TestRideCacheRemoval::shiftPlannedActivitiesPurgesDestroyedRowsBeforeReorder()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime originalDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &originalDateTime));
    RideFile *ride = new RideFile(originalDateTime, 1.0);
    RideItem *target = fixture.addPlannedRide(firstName(), false, ride);
    target->dateTime = originalDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()), QByteArray("planned"));
    RideItem *destroyed = fixture.addRide(secondName(), false);
    RideItem *const destroyedAddress = destroyed;
    QMetaObject::Connection resetConnection;
    resetConnection = connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        fixture.cache.get(),
        [destroyed, &resetConnection] {
            QObject::disconnect(resetConnection);
            delete destroyed;
        });

    const RideCache::OperationResult result =
        fixture.cache->shiftPlannedActivities(
            originalDateTime.date(), 1);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.affectedCount, 1);
    QCOMPARE(
        target->fileName,
        QStringLiteral("2026_07_07_08_00_00.json"));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstName())));
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(target->fileName)));
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        destroyedAddress));
    QCOMPARE(rideCacheRemovalCancelCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorStopCount(), 1);
    delete ride;
}

void TestRideCacheRemoval::moveActivityReservesModelBeforeStorageCommit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime originalDateTime;
    QDateTime targetDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &originalDateTime));
    QVERIFY(RideFile::parseRideFileName(thirdName(), &targetDateTime));
    RideFile *ride = new RideFile(originalDateTime, 1.0);
    RideItem *target = fixture.addRide(firstName(), true, ride);
    target->dateTime = originalDateTime;
    writeFixture(
        fixture.activityPath(firstName()), QByteArray("activity"));
    bool insertAllowed = true;
    setRideCacheCalendarStorageAction([&] {
        insertAllowed = fixture.cache->model()->startInsert(0, 0);
    });

    const RideCache::OperationResult result =
        fixture.cache->moveActivity(target, targetDateTime);
    if (insertAllowed) fixture.cache->model()->endInsert();
    delete ride;

    QVERIFY(!insertAllowed);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(target->fileName, thirdName());
    QVERIFY(!QFileInfo::exists(fixture.activityPath(firstName())));
    QVERIFY(QFileInfo::exists(fixture.activityPath(thirdName())));
}

void TestRideCacheRemoval::
moveActivityCrashRecoveryIsGenerationAtomic()
{
    const QString root = qEnvironmentVariable(
        ActivityMoveCrashRootEnvironment);
    const QString mode = qEnvironmentVariable(
        ActivityMoveCrashModeEnvironment);
    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(new Context(nullptr));
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

        QDateTime sourceDateTime;
        QDateTime targetDateTime;
        if (!RideFile::parseRideFileName(
                firstName(), &sourceDateTime)
            || !RideFile::parseRideFileName(
                thirdName(), &targetDateTime)) {
            std::_Exit(88);
        }
        RideFile *ride = new RideFile(sourceDateTime, 1.0);
        RideItem *item = new RideItem(ride, context.get());
        item->fileName = firstName();
        item->path = athlete->home->activities().absolutePath();
        item->dateTime = sourceDateTime;
        cache->mutableRidesForRemovalTest().append(item);
        context->ride = item;
        writeFixture(
            athlete->home->activities().filePath(firstName()),
            QByteArray("old activity"));
        const QString oldBase = QFileInfo(firstName()).baseName();
        for (const QString &extension :
             {QStringLiteral("cpx"), QStringLiteral("cpi"),
              QStringLiteral("notes")}) {
            writeFixture(
                athlete->home->cache().filePath(
                    oldBase + QLatin1Char('.') + extension),
                extension.toLatin1() + QByteArray(" old"));
        }

        const RideCache::OperationResult childResult =
            cache->moveActivity(item, targetDateTime);
        QFAIL(qPrintable(QStringLiteral(
            "activity move child did not stop at the requested transition: %1")
                             .arg(childResult.error)));
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild =
        [&](const QString &childMode,
            const QString &phase) {
            QProcess child;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashPhaseEnvironment));
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashOccurrenceEnvironment));
            environment.insert(
                QString::fromLatin1(
                    ActivityMoveCrashRootEnvironment),
                temporary.path());
            environment.insert(
                QString::fromLatin1(
                    ActivityMoveCrashModeEnvironment),
                childMode);
            environment.insert(
                QStringLiteral("QT_QPA_PLATFORM"),
                QStringLiteral("offscreen"));
            if (!phase.isEmpty()) {
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashPhaseEnvironment),
                    phase);
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashOccurrenceEnvironment),
                    QStringLiteral("1"));
            }
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral(
                    "moveActivityCrashRecoveryIsGenerationAtomic")});
            if (!child.waitForStarted(5000))
                return qMakePair(-1, child.errorString());
            if (!child.waitForFinished(20000)) {
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
        QStringLiteral("crash"),
        QStringLiteral("plan-replacement-target-published"));
    QCOMPARE(crashed.first, PlannedCopyCrashExitCode);
    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QDir activities(
        QDir(temporary.path()).filePath(
            QStringLiteral("activities")));
    const QDir cache(
        QDir(temporary.path()).filePath(
            QStringLiteral("cache")));
    QCOMPARE(
        readBytes(activities.filePath(firstName())),
        QByteArray("old activity"));
    QVERIFY(!QFileInfo::exists(
        activities.filePath(thirdName())));
    const QString oldBase = QFileInfo(firstName()).baseName();
    const QString newBase = QFileInfo(thirdName()).baseName();
    for (const QString &extension :
         {QStringLiteral("cpx"), QStringLiteral("cpi"),
          QStringLiteral("notes")}) {
        QCOMPARE(
            readBytes(cache.filePath(
                oldBase + QLatin1Char('.') + extension)),
            extension.toLatin1() + QByteArray(" old"));
        QVERIFY(!QFileInfo::exists(
            cache.filePath(
                newBase + QLatin1Char('.') + extension)));
    }
}

void TestRideCacheRemoval::
moveActivityPostCommitNotificationLossIsReportedCommitted()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QDateTime targetDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &sourceDateTime));
    QVERIFY(RideFile::parseRideFileName(
        thirdName(), &targetDateTime));
    RideItem *item = fixture.addRide(firstName(), true);
    item->dateTime = sourceDateTime;
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("activity old"));
    connect(
        fixture.context.get(), &Context::rideChanged,
        fixture.cache.get(),
        [context = fixture.context.get(), item](RideItem *changed) {
            if (changed != item) return;
            context->ride = nullptr;
            delete item;
        });

    const RideCache::OperationResult result =
        fixture.cache->moveActivity(item, targetDateTime);

    QVERIFY(result.committed);
    QVERIFY(!result.success);
    QVERIFY(!result.cacheUpdated);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.affectedCount, 1);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QVERIFY(QFileInfo::exists(
        fixture.activityPath(thirdName())));
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(item));
}

void TestRideCacheRemoval::
moveLinkedActivityPersistsReciprocalGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QDateTime peerDateTime;
    QDateTime targetDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &sourceDateTime));
    QVERIFY(RideFile::parseRideFileName(
        secondName(), &peerDateTime));
    QVERIFY(RideFile::parseRideFileName(
        thirdName(), &targetDateTime));
    RideItem *completed = fixture.addRide(firstName(), true);
    RideItem *planned = fixture.addPlannedRide(secondName(), false);
    completed->dateTime = sourceDateTime;
    planned->dateTime = peerDateTime;
    completed->setLinkedFileName(secondName());
    planned->setLinkedFileName(firstName());
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("completed old"));
    writeFixture(
        fixture.plannedActivityPath(secondName()),
        QByteArray("planned old"));

    const RideCache::OperationResult result =
        fixture.cache->moveActivity(completed, targetDateTime);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QCOMPARE(result.affectedCount, 2);
    QCOMPARE(completed->fileName, thirdName());
    QCOMPARE(completed->getLinkedFileName(), secondName());
    QCOMPARE(planned->fileName, secondName());
    QCOMPARE(planned->getLinkedFileName(), thirdName());
    QVERIFY(!completed->isDirty());
    QVERIFY(!planned->isDirty());
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.activityPath(thirdName())),
        QByteArray("completed old\nidentity=")
            + thirdName().toUtf8()
            + QByteArray("\nlinked=")
            + secondName().toUtf8() + '\n');
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        QByteArray("planned old\nidentity=")
            + secondName().toUtf8()
            + QByteArray("\nlinked=")
            + thirdName().toUtf8() + '\n');
}

void TestRideCacheRemoval::
moveLinkedActivityStageFailureRollsBackGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QDateTime peerDateTime;
    QDateTime targetDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &sourceDateTime));
    QVERIFY(RideFile::parseRideFileName(
        secondName(), &peerDateTime));
    QVERIFY(RideFile::parseRideFileName(
        thirdName(), &targetDateTime));
    RideItem *completed = fixture.addRide(firstName(), true);
    RideItem *planned = fixture.addPlannedRide(secondName(), false);
    completed->dateTime = sourceDateTime;
    planned->dateTime = peerDateTime;
    completed->setLinkedFileName(secondName());
    planned->setLinkedFileName(firstName());
    const QByteArray completedContents("completed old");
    const QByteArray plannedContents("planned old");
    writeFixture(
        fixture.activityPath(firstName()), completedContents);
    writeFixture(
        fixture.plannedActivityPath(secondName()), plannedContents);
    setRideCacheActivityIdentityFailureOnCall(2);

    const RideCache::OperationResult result =
        fixture.cache->moveActivity(completed, targetDateTime);

    QVERIFY(!result.success);
    QVERIFY(!result.committed);
    QVERIFY(result.error.contains(
        QStringLiteral("injected identity staging failure")));
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(completed->fileName, firstName());
    QCOMPARE(completed->getLinkedFileName(), secondName());
    QCOMPARE(planned->fileName, secondName());
    QCOMPARE(planned->getLinkedFileName(), firstName());
    QCOMPARE(
        readBytes(fixture.activityPath(firstName())),
        completedContents);
    QVERIFY(!QFileInfo::exists(
        fixture.activityPath(thirdName())));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        plannedContents);
}

void TestRideCacheRemoval::
moveLinkedActivityCrashRecoveryIsGenerationAtomic_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("occurrence");
    QTest::addColumn<bool>("committed");

    QTest::newRow("directory-created")
        << QStringLiteral("plan-replacement-directory-created")
        << 1 << false;
    for (int occurrence = 1; occurrence <= 5; ++occurrence) {
        const QByteArray name = QByteArray("old-copy-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-old-copy-published")
            << occurrence << false;
    }
    QTest::newRow("initial-manifest")
        << QStringLiteral("plan-replacement-initial-manifest-published")
        << 1 << false;
    for (int occurrence = 1; occurrence <= 5; ++occurrence) {
        const QByteArray name = QByteArray("stage-recorded-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-stage-recorded")
            << occurrence << false;
    }
    for (int occurrence = 1; occurrence <= 5; ++occurrence) {
        const QByteArray name = QByteArray("target-published-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-target-published")
            << occurrence << false;
    }
    for (int occurrence = 1; occurrence <= 4; ++occurrence) {
        const QByteArray name = QByteArray("old-removed-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-old-removed")
            << occurrence << false;
    }
    QTest::newRow("commit-marker")
        << QStringLiteral("plan-replacement-commit-marker")
        << 1 << true;
    QTest::newRow("manifest-removed")
        << QStringLiteral("plan-replacement-manifest-removed")
        << 1 << true;
    for (int occurrence = 1; occurrence <= 10; ++occurrence) {
        const QByteArray name = QByteArray("cleanup-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-cleanup-file")
            << occurrence << true;
    }
    QTest::newRow("commit-marker-removed")
        << QStringLiteral("plan-replacement-commit-marker-removed")
        << 1 << true;
    QTest::newRow("journal-directory-removed")
        << QStringLiteral("plan-replacement-directory-removed")
        << 1 << true;
}

void TestRideCacheRemoval::
moveLinkedActivityCrashRecoveryIsGenerationAtomic()
{
    QFETCH(QString, phase);
    QFETCH(int, occurrence);
    QFETCH(bool, committed);

    const QString root = qEnvironmentVariable(
        ActivityMoveCrashRootEnvironment);
    const QString mode = qEnvironmentVariable(
        ActivityMoveCrashModeEnvironment);
    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(new Context(nullptr));
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

        QDateTime sourceDateTime;
        QDateTime peerDateTime;
        QDateTime targetDateTime;
        if (!RideFile::parseRideFileName(
                firstName(), &sourceDateTime)
            || !RideFile::parseRideFileName(
                secondName(), &peerDateTime)
            || !RideFile::parseRideFileName(
                thirdName(), &targetDateTime)) {
            std::_Exit(88);
        }
        RideItem *completed = new RideItem(nullptr, context.get());
        completed->fileName = firstName();
        completed->path =
            athlete->home->activities().absolutePath();
        completed->dateTime = sourceDateTime;
        RideItem *planned = new RideItem(nullptr, context.get());
        planned->fileName = secondName();
        planned->path = athlete->home->planned().absolutePath();
        planned->dateTime = peerDateTime;
        planned->planned = true;
        completed->setLinkedFileName(secondName());
        planned->setLinkedFileName(firstName());
        cache->mutableRidesForRemovalTest().append(completed);
        cache->mutableRidesForRemovalTest().append(planned);
        context->ride = completed;
        writeFixture(
            athlete->home->activities().filePath(firstName()),
            QByteArray("completed old"));
        writeFixture(
            athlete->home->planned().filePath(secondName()),
            QByteArray("planned old"));
        const QString oldBase = QFileInfo(firstName()).baseName();
        for (const QString &extension :
             {QStringLiteral("cpx"), QStringLiteral("cpi"),
              QStringLiteral("notes")}) {
            writeFixture(
                athlete->home->cache().filePath(
                    oldBase + QLatin1Char('.') + extension),
                extension.toLatin1() + QByteArray(" old"));
        }

        const RideCache::OperationResult childResult =
            cache->moveActivity(completed, targetDateTime);
        QFAIL(qPrintable(QStringLiteral(
            "linked move child did not stop at the requested transition: %1")
                             .arg(childResult.error)));
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild =
        [&](const QString &childMode,
            const QString &phase) {
            QProcess child;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashPhaseEnvironment));
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashOccurrenceEnvironment));
            environment.insert(
                QString::fromLatin1(
                    ActivityMoveCrashRootEnvironment),
                temporary.path());
            environment.insert(
                QString::fromLatin1(
                    ActivityMoveCrashModeEnvironment),
                childMode);
            environment.insert(
                QStringLiteral("QT_QPA_PLATFORM"),
                QStringLiteral("offscreen"));
            if (!phase.isEmpty()) {
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashPhaseEnvironment),
                    phase);
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashOccurrenceEnvironment),
                    QString::number(occurrence));
            }
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral(
                    "moveLinkedActivityCrashRecoveryIsGenerationAtomic:%1")
                     .arg(QString::fromLatin1(
                         QTest::currentDataTag()))});
            if (!child.waitForStarted(5000))
                return qMakePair(-1, child.errorString());
            if (!child.waitForFinished(20000)) {
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
        QStringLiteral("crash"), phase);
    QCOMPARE(crashed.first, PlannedCopyCrashExitCode);
    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QDir activities(
        QDir(temporary.path()).filePath(
            QStringLiteral("activities")));
    const QDir planned(
        QDir(temporary.path()).filePath(
            QStringLiteral("planned")));
    const QDir cache(
        QDir(temporary.path()).filePath(
            QStringLiteral("cache")));
    const QString oldBase = QFileInfo(firstName()).baseName();
    const QString newBase = QFileInfo(thirdName()).baseName();
    const QStringList extensions = {
        QStringLiteral("cpx"), QStringLiteral("cpi"),
        QStringLiteral("notes")};
    if (committed) {
        QVERIFY(!QFileInfo::exists(
            activities.filePath(firstName())));
        QCOMPARE(
            readBytes(activities.filePath(thirdName())),
            QByteArray("completed old\nidentity=")
                + thirdName().toUtf8()
                + QByteArray("\nlinked=")
                + secondName().toUtf8() + '\n');
        QCOMPARE(
            readBytes(planned.filePath(secondName())),
            QByteArray("planned old\nidentity=")
                + secondName().toUtf8()
                + QByteArray("\nlinked=")
                + thirdName().toUtf8() + '\n');
        for (const QString &extension : extensions) {
            QVERIFY(!QFileInfo::exists(
                cache.filePath(
                    oldBase + QLatin1Char('.') + extension)));
            QCOMPARE(
                readBytes(cache.filePath(
                    newBase + QLatin1Char('.') + extension)),
                extension.toLatin1() + QByteArray(" old"));
        }
    } else {
        QCOMPARE(
            readBytes(activities.filePath(firstName())),
            QByteArray("completed old"));
        QVERIFY(!QFileInfo::exists(
            activities.filePath(thirdName())));
        QCOMPARE(
            readBytes(planned.filePath(secondName())),
            QByteArray("planned old"));
        for (const QString &extension : extensions) {
            QCOMPARE(
                readBytes(cache.filePath(
                    oldBase + QLatin1Char('.') + extension)),
                extension.toLatin1() + QByteArray(" old"));
            QVERIFY(!QFileInfo::exists(
                cache.filePath(
                    newBase + QLatin1Char('.') + extension)));
        }
    }

    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);
}

void TestRideCacheRemoval::
copyPlannedActivityReservesModelBeforeStorageCommit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime sourceDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &sourceDateTime));
    RideItem *source = fixture.addPlannedRide(firstName(), false);
    source->dateTime = sourceDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned source"));
    bool insertAllowed = true;
    setRideCacheCalendarStorageAction([&] {
        insertAllowed = fixture.cache->model()->startInsert(0, 0);
    });

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivity(
            source, sourceDateTime.date().addDays(1));
    if (insertAllowed) fixture.cache->model()->endInsert();

    const QString targetName =
        QStringLiteral("2026_07_07_08_00_00.json");
    QVERIFY(!insertAllowed);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(cacheContains(*fixture.cache, targetName));
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(targetName)));
}

void TestRideCacheRemoval::
copyPlannedActivitiesReserveModelBeforeStorageCommit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime firstDateTime;
    QDateTime secondDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &firstDateTime));
    QVERIFY(RideFile::parseRideFileName(secondName(), &secondDateTime));
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    first->dateTime = firstDateTime;
    second->dateTime = secondDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()), QByteArray("first"));
    writeFixture(
        fixture.plannedActivityPath(secondName()), QByteArray("second"));
    bool insertAllowed = true;
    setRideCacheCalendarStorageAction([&] {
        insertAllowed = fixture.cache->model()->startInsert(0, 0);
    });

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivities({
            {first, firstDateTime.date().addDays(1)},
            {second, secondDateTime.date().addDays(2)}});
    if (insertAllowed) fixture.cache->model()->endInsert();

    const QString firstTarget =
        QStringLiteral("2026_07_07_08_00_00.json");
    const QString secondTarget =
        QStringLiteral("2026_07_08_09_00_00.json");
    QVERIFY(!insertAllowed);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(cacheContains(*fixture.cache, firstTarget));
    QVERIFY(cacheContains(*fixture.cache, secondTarget));
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(firstTarget)));
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(secondTarget)));
}

void TestRideCacheRemoval::
copyPlannedActivitiesStageFailureRollsBackGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime firstDateTime;
    QDateTime secondDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &firstDateTime));
    QVERIFY(RideFile::parseRideFileName(secondName(), &secondDateTime));
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    first->dateTime = firstDateTime;
    second->dateTime = secondDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()), QByteArray("first"));
    writeFixture(
        fixture.plannedActivityPath(secondName()), QByteArray("second"));
    setRideCacheCalendarCopyFailureOnCall(2);

    const RideCache::OperationResult result =
        fixture.cache->copyPlannedActivities({
            {first, firstDateTime.date().addDays(1)},
            {second, secondDateTime.date().addDays(2)}});

    const QString firstTarget =
        QStringLiteral("2026_07_07_08_00_00.json");
    const QString secondTarget =
        QStringLiteral("2026_07_08_09_00_00.json");
    QVERIFY(!result.success);
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(fixture.cache->count(), 2);
    QVERIFY(!cacheContains(*fixture.cache, firstTarget));
    QVERIFY(!cacheContains(*fixture.cache, secondTarget));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstTarget)));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(secondTarget)));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(firstName())),
        QByteArray("first"));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(secondName())),
        QByteArray("second"));
}

void TestRideCacheRemoval::
copyPlannedActivitiesCrashRecoveryIsGenerationAtomic_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("occurrence");
    QTest::addColumn<bool>("committed");

    QTest::newRow("directory-created")
        << QStringLiteral("plan-replacement-directory-created")
        << 1 << false;
    QTest::newRow("initial-manifest")
        << QStringLiteral("plan-replacement-initial-manifest-published")
        << 1 << false;
    QTest::newRow("first-stage-recorded")
        << QStringLiteral("plan-replacement-stage-recorded")
        << 1 << false;
    QTest::newRow("second-stage-recorded")
        << QStringLiteral("plan-replacement-stage-recorded")
        << 2 << false;
    QTest::newRow("first-target-published")
        << QStringLiteral("plan-replacement-target-published")
        << 1 << false;
    QTest::newRow("second-target-published")
        << QStringLiteral("plan-replacement-target-published")
        << 2 << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("plan-replacement-commit-marker")
        << 1 << true;
    QTest::newRow("manifest-removed")
        << QStringLiteral("plan-replacement-manifest-removed")
        << 1 << true;
    QTest::newRow("commit-marker-removed")
        << QStringLiteral("plan-replacement-commit-marker-removed")
        << 1 << true;
    QTest::newRow("journal-directory-removed")
        << QStringLiteral("plan-replacement-directory-removed")
        << 1 << true;
}

void TestRideCacheRemoval::
copyPlannedActivitiesCrashRecoveryIsGenerationAtomic()
{
    QFETCH(QString, phase);
    QFETCH(int, occurrence);
    QFETCH(bool, committed);

    const QString root = qEnvironmentVariable(
        PlannedCopyCrashRootEnvironment);
    const QString mode = qEnvironmentVariable(
        PlannedCopyCrashModeEnvironment);
    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(new Context(nullptr));
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

        QDateTime firstDateTime;
        QDateTime secondDateTime;
        if (!RideFile::parseRideFileName(
                firstName(), &firstDateTime)
            || !RideFile::parseRideFileName(
                secondName(), &secondDateTime)) {
            std::_Exit(88);
        }
        RideItem *first = new RideItem(nullptr, context.get());
        first->fileName = firstName();
        first->path = athlete->home->planned().absolutePath();
        first->dateTime = firstDateTime;
        first->planned = true;
        RideItem *second = new RideItem(nullptr, context.get());
        second->fileName = secondName();
        second->path = athlete->home->planned().absolutePath();
        second->dateTime = secondDateTime;
        second->planned = true;
        cache->mutableRidesForRemovalTest().append(first);
        cache->mutableRidesForRemovalTest().append(second);
        writeFixture(
            athlete->home->planned().filePath(firstName()),
            QByteArray("first"));
        writeFixture(
            athlete->home->planned().filePath(secondName()),
            QByteArray("second"));

        const RideCache::OperationResult childResult =
            cache->copyPlannedActivities({
            {first, firstDateTime.date().addDays(1)},
            {second, secondDateTime.date().addDays(2)}});
        QFAIL(qPrintable(QStringLiteral(
            "planned copy child did not stop at the requested transition: %1")
                             .arg(childResult.error)));
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild =
        [&](const QString &childMode,
            const QString &requestedPhase) {
            QProcess child;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashPhaseEnvironment));
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashOccurrenceEnvironment));
            environment.insert(
                QString::fromLatin1(
                    PlannedCopyCrashRootEnvironment),
                temporary.path());
            environment.insert(
                QString::fromLatin1(
                    PlannedCopyCrashModeEnvironment),
                childMode);
            environment.insert(
                QStringLiteral("QT_QPA_PLATFORM"),
                QStringLiteral("offscreen"));
            if (!requestedPhase.isEmpty()) {
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashPhaseEnvironment),
                    requestedPhase);
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashOccurrenceEnvironment),
                    QString::number(occurrence));
            }
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral(
                    "copyPlannedActivitiesCrashRecoveryIsGenerationAtomic:%1")
                     .arg(QString::fromLatin1(
                         QTest::currentDataTag()))});
            if (!child.waitForStarted(5000)) {
                return qMakePair(-1, child.errorString());
            }
            if (!child.waitForFinished(20000)) {
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
        QStringLiteral("crash"), phase);
    QCOMPARE(crashed.first, PlannedCopyCrashExitCode);

    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QDir planned(
        QDir(temporary.path()).filePath(
            QStringLiteral("planned")));
    const QString firstTarget =
        QStringLiteral("2026_07_07_08_00_00.json");
    const QString secondTarget =
        QStringLiteral("2026_07_08_09_00_00.json");
    QCOMPARE(readBytes(planned.filePath(firstName())), QByteArray("first"));
    QCOMPARE(readBytes(planned.filePath(secondName())), QByteArray("second"));
    if (committed) {
        QCOMPARE(
            readBytes(planned.filePath(firstTarget)),
            QByteArray("first"));
        QCOMPARE(
            readBytes(planned.filePath(secondTarget)),
            QByteArray("second"));
    } else {
        QVERIFY(!QFileInfo::exists(planned.filePath(firstTarget)));
        QVERIFY(!QFileInfo::exists(planned.filePath(secondTarget)));
    }

    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);
    const QDir journalRoot(
        QDir(temporary.path()).filePath(
            QStringLiteral(
                ".gc-transactions/plan-replacement")));
    QVERIFY(!journalRoot.exists()
        || journalRoot.entryInfoList(
               QDir::Dirs | QDir::Hidden
                   | QDir::NoDotAndDotDot)
               .isEmpty());
}

void TestRideCacheRemoval::
shiftPlannedActivitiesReservesModelBeforeStorageCommit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime originalDateTime;
    QVERIFY(RideFile::parseRideFileName(firstName(), &originalDateTime));
    RideFile *ride = new RideFile(originalDateTime, 1.0);
    RideItem *target = fixture.addPlannedRide(firstName(), false, ride);
    target->dateTime = originalDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()), QByteArray("planned"));
    bool insertAllowed = true;
    setRideCacheCalendarStorageAction([&] {
        insertAllowed = fixture.cache->model()->startInsert(0, 0);
    });

    const RideCache::OperationResult result =
        fixture.cache->shiftPlannedActivities(
            originalDateTime.date(), 1);
    if (insertAllowed) fixture.cache->model()->endInsert();
    delete ride;

    const QString targetName =
        QStringLiteral("2026_07_07_08_00_00.json");
    QVERIFY(!insertAllowed);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(target->fileName, targetName);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstName())));
    QVERIFY(QFileInfo::exists(
        fixture.plannedActivityPath(targetName)));
}

void TestRideCacheRemoval::
shiftPlannedActivitiesCommitOverlappingGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime firstDateTime;
    QDateTime secondDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &firstDateTime));
    QVERIFY(RideFile::parseRideFileName(
        nextDayName(), &secondDateTime));
    RideFile *firstRide = new RideFile(firstDateTime, 1.0);
    RideFile *secondRide = new RideFile(secondDateTime, 1.0);
    RideItem *first = fixture.addPlannedRide(
        firstName(), false, firstRide);
    RideItem *second = fixture.addPlannedRide(
        nextDayName(), false, secondRide);
    first->dateTime = firstDateTime;
    second->dateTime = secondDateTime;
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("first old"));
    writeFixture(
        fixture.plannedActivityPath(nextDayName()),
        QByteArray("second old"));

    const RideCache::OperationResult result =
        fixture.cache->shiftPlannedActivities(
            firstDateTime.date(), 1);
    delete firstRide;
    delete secondRide;

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QCOMPARE(result.affectedCount, 2);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(nextDayName())),
        QByteArray("first old\nidentity=")
            + nextDayName().toUtf8()
            + QByteArray("\nlinked=\n"));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(twoDaysLaterName())),
        QByteArray("second old\nidentity=")
            + twoDaysLaterName().toUtf8()
            + QByteArray("\nlinked=\n"));
    QCOMPARE(first->fileName, nextDayName());
    QCOMPARE(second->fileName, twoDaysLaterName());
}

void TestRideCacheRemoval::
shiftPlannedActivitiesStageFailureRollsBackGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime firstDateTime;
    QDateTime secondDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &firstDateTime));
    QVERIFY(RideFile::parseRideFileName(
        nextDayName(), &secondDateTime));
    RideFile *firstRide = new RideFile(firstDateTime, 1.0);
    RideFile *secondRide = new RideFile(secondDateTime, 1.0);
    RideItem *first = fixture.addPlannedRide(
        firstName(), false, firstRide);
    RideItem *second = fixture.addPlannedRide(
        nextDayName(), false, secondRide);
    first->dateTime = firstDateTime;
    second->dateTime = secondDateTime;
    const QByteArray firstContents("first old");
    const QByteArray secondContents("second old");
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        firstContents);
    writeFixture(
        fixture.plannedActivityPath(nextDayName()),
        secondContents);
    setRideCacheActivityIdentityFailureOnCall(2);

    const RideCache::OperationResult result =
        fixture.cache->shiftPlannedActivities(
            firstDateTime.date(), 1);
    delete firstRide;
    delete secondRide;

    QVERIFY(!result.success);
    QVERIFY(!result.committed);
    QVERIFY(result.error.contains(
        QStringLiteral("injected identity staging failure")));
    QCOMPARE(result.affectedCount, 0);
    QCOMPARE(first->fileName, firstName());
    QCOMPARE(second->fileName, nextDayName());
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(firstName())),
        firstContents);
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(nextDayName())),
        secondContents);
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(twoDaysLaterName())));
}

void TestRideCacheRemoval::
shiftLinkedPlannedActivityPersistsReciprocalGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());

    QDateTime plannedDateTime;
    QDateTime completedDateTime;
    QVERIFY(RideFile::parseRideFileName(
        firstName(), &plannedDateTime));
    QVERIFY(RideFile::parseRideFileName(
        secondName(), &completedDateTime));
    RideFile *plannedRide = new RideFile(plannedDateTime, 1.0);
    RideItem *planned = fixture.addPlannedRide(
        firstName(), true, plannedRide);
    RideItem *completed = fixture.addRide(secondName(), false);
    planned->dateTime = plannedDateTime;
    completed->dateTime = completedDateTime;
    planned->setLinkedFileName(secondName());
    completed->setLinkedFileName(firstName());
    writeFixture(
        fixture.plannedActivityPath(firstName()),
        QByteArray("planned old"));
    writeFixture(
        fixture.activityPath(secondName()),
        QByteArray("completed old"));

    const RideCache::OperationResult result =
        fixture.cache->shiftPlannedActivities(
            plannedDateTime.date(), 1);
    delete plannedRide;

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QCOMPARE(result.affectedCount, 2);
    QCOMPARE(planned->fileName, nextDayName());
    QCOMPARE(planned->getLinkedFileName(), secondName());
    QCOMPARE(completed->fileName, secondName());
    QCOMPARE(completed->getLinkedFileName(), nextDayName());
    QVERIFY(!planned->isDirty());
    QVERIFY(!completed->isDirty());
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(firstName())));
    QCOMPARE(
        readBytes(fixture.plannedActivityPath(nextDayName())),
        QByteArray("planned old\nidentity=")
            + nextDayName().toUtf8()
            + QByteArray("\nlinked=")
            + secondName().toUtf8() + '\n');
    QCOMPARE(
        readBytes(fixture.activityPath(secondName())),
        QByteArray("completed old\nidentity=")
            + secondName().toUtf8()
            + QByteArray("\nlinked=")
            + nextDayName().toUtf8() + '\n');
}

void TestRideCacheRemoval::
shiftPlannedActivitiesCrashRecoveryIsGenerationAtomic_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("occurrence");
    QTest::addColumn<bool>("committed");

    QTest::newRow("directory-created")
        << QStringLiteral("plan-replacement-directory-created")
        << 1 << false;
    QTest::newRow("first-old-copy")
        << QStringLiteral("plan-replacement-old-copy-published")
        << 1 << false;
    QTest::newRow("second-old-copy")
        << QStringLiteral("plan-replacement-old-copy-published")
        << 2 << false;
    QTest::newRow("initial-manifest")
        << QStringLiteral("plan-replacement-initial-manifest-published")
        << 1 << false;
    QTest::newRow("first-stage-recorded")
        << QStringLiteral("plan-replacement-stage-recorded")
        << 1 << false;
    QTest::newRow("second-stage-recorded")
        << QStringLiteral("plan-replacement-stage-recorded")
        << 2 << false;
    QTest::newRow("first-target-published")
        << QStringLiteral("plan-replacement-target-published")
        << 1 << false;
    QTest::newRow("second-target-published")
        << QStringLiteral("plan-replacement-target-published")
        << 2 << false;
    QTest::newRow("old-only-removed")
        << QStringLiteral("plan-replacement-old-removed")
        << 1 << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("plan-replacement-commit-marker")
        << 1 << true;
    QTest::newRow("manifest-removed")
        << QStringLiteral("plan-replacement-manifest-removed")
        << 1 << true;
    for (int occurrence = 1; occurrence <= 4; ++occurrence) {
        const QByteArray name = QByteArray("cleanup-")
            + QByteArray::number(occurrence);
        QTest::newRow(name.constData())
            << QStringLiteral("plan-replacement-cleanup-file")
            << occurrence << true;
    }
    QTest::newRow("commit-marker-removed")
        << QStringLiteral("plan-replacement-commit-marker-removed")
        << 1 << true;
    QTest::newRow("journal-directory-removed")
        << QStringLiteral("plan-replacement-directory-removed")
        << 1 << true;
}

void TestRideCacheRemoval::
shiftPlannedActivitiesCrashRecoveryIsGenerationAtomic()
{
    QFETCH(QString, phase);
    QFETCH(int, occurrence);
    QFETCH(bool, committed);

    const QString root = qEnvironmentVariable(
        ActivityShiftCrashRootEnvironment);
    const QString mode = qEnvironmentVariable(
        ActivityShiftCrashModeEnvironment);
    if (!root.isEmpty()) {
        std::unique_ptr<Context> context(new Context(nullptr));
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

        QDateTime firstDateTime;
        QDateTime secondDateTime;
        if (!RideFile::parseRideFileName(
                firstName(), &firstDateTime)
            || !RideFile::parseRideFileName(
                nextDayName(), &secondDateTime)) {
            std::_Exit(88);
        }
        RideItem *first = new RideItem(nullptr, context.get());
        first->fileName = firstName();
        first->path = athlete->home->planned().absolutePath();
        first->dateTime = firstDateTime;
        first->planned = true;
        RideItem *second = new RideItem(nullptr, context.get());
        second->fileName = nextDayName();
        second->path = athlete->home->planned().absolutePath();
        second->dateTime = secondDateTime;
        second->planned = true;
        cache->mutableRidesForRemovalTest().append(first);
        cache->mutableRidesForRemovalTest().append(second);
        writeFixture(
            athlete->home->planned().filePath(firstName()),
            QByteArray("first old"));
        writeFixture(
            athlete->home->planned().filePath(nextDayName()),
            QByteArray("second old"));

        const RideCache::OperationResult childResult =
            cache->shiftPlannedActivities(firstDateTime.date(), 1);
        QFAIL(qPrintable(QStringLiteral(
            "planned shift child did not stop at the requested transition: %1")
                             .arg(childResult.error)));
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto runChild =
        [&](const QString &childMode,
            const QString &requestedPhase) {
            QProcess child;
            QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashPhaseEnvironment));
            environment.remove(QString::fromLatin1(
                PlanReplacementCrashOccurrenceEnvironment));
            environment.insert(
                QString::fromLatin1(
                    ActivityShiftCrashRootEnvironment),
                temporary.path());
            environment.insert(
                QString::fromLatin1(
                    ActivityShiftCrashModeEnvironment),
                childMode);
            environment.insert(
                QStringLiteral("QT_QPA_PLATFORM"),
                QStringLiteral("offscreen"));
            if (!requestedPhase.isEmpty()) {
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashPhaseEnvironment),
                    requestedPhase);
                environment.insert(
                    QString::fromLatin1(
                        PlanReplacementCrashOccurrenceEnvironment),
                    QString::number(occurrence));
            }
            child.setProcessEnvironment(environment);
            child.start(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral(
                    "shiftPlannedActivitiesCrashRecoveryIsGenerationAtomic:%1")
                     .arg(QString::fromLatin1(
                         QTest::currentDataTag()))});
            if (!child.waitForStarted(5000))
                return qMakePair(-1, child.errorString());
            if (!child.waitForFinished(20000)) {
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
        QStringLiteral("crash"), phase);
    QCOMPARE(crashed.first, PlannedCopyCrashExitCode);
    const auto recovered = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recovered.first, 0);

    const QDir planned(
        QDir(temporary.path()).filePath(
            QStringLiteral("planned")));
    if (committed) {
        QVERIFY(!QFileInfo::exists(
            planned.filePath(firstName())));
        QCOMPARE(
            readBytes(planned.filePath(nextDayName())),
            QByteArray("first old\nidentity=")
                + nextDayName().toUtf8()
                + QByteArray("\nlinked=\n"));
        QCOMPARE(
            readBytes(planned.filePath(twoDaysLaterName())),
            QByteArray("second old\nidentity=")
                + twoDaysLaterName().toUtf8()
                + QByteArray("\nlinked=\n"));
    } else {
        QCOMPARE(
            readBytes(planned.filePath(firstName())),
            QByteArray("first old"));
        QCOMPARE(
            readBytes(planned.filePath(nextDayName())),
            QByteArray("second old"));
        QVERIFY(!QFileInfo::exists(
            planned.filePath(twoDaysLaterName())));
    }

    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);
}

void TestRideCacheRemoval::
plannedReplacementDropsStaleResumeDuringNewReplacement()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> failingTargets = {{
        thirdName(),
        [](const QString &, QString &error) {
            error = QStringLiteral("injected staging failure");
            return false;
        }}};

    const RideCache::PlannedReplacementResult firstResult =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, failingTargets);
    QVERIFY(!firstResult.committed);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    bool staleResumeWasBlocked = false;
    const QList<RideCache::PlannedActivityTarget> succeedingTargets = {{
        thirdName(),
        [&staleResumeWasBlocked]
        (const QString &path, QString &error) {
            QCoreApplication::processEvents();
            staleResumeWasBlocked =
                rideCacheRemovalEstimatorRefreshCount() == 0;
            return stageBytes(path, QByteArray("new third"), error);
        }}};
    const RideCache::PlannedReplacementResult secondResult =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, succeedingTargets);

    QVERIFY2(secondResult.cleanlyCompleted(), qPrintable(secondResult.error));
    QVERIFY(staleResumeWasBlocked);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QCoreApplication::processEvents();

    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementCoalescesSupersededResumes()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> failingTargets = {{
        thirdName(),
        [](const QString &, QString &error) {
            error = QStringLiteral("injected staging failure");
            return false;
        }}};

    const RideCache::PlannedReplacementResult firstResult =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, failingTargets);
    const RideCache::PlannedReplacementResult secondResult =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, failingTargets);

    QVERIFY(!firstResult.committed);
    QVERIFY(!secondResult.committed);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);

    QCoreApplication::processEvents();

    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementProcessorFailureIsReportedAfterPublication()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    std::unique_ptr<RideFile> ride(new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 0)), 1.0));
    RideItem *first = fixture.addPlannedRide(
        firstName(), false, ride.get());
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    setRideCacheRemovalProcessorFailure(true);

    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};
    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);
    setRideCacheRemovalProcessorFailure(false);

    QVERIFY(result.committed);
    QVERIFY(result.cacheUpdated);
    QVERIFY(result.cleanupComplete);
    QVERIFY(result.cleanlyCompleted());
    QVERIFY(result.error.isEmpty());
    QVERIFY(!result.warnings.isEmpty());
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(thirdPath), QByteArray("new third"));
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
}

void TestRideCacheRemoval::
plannedReplacementProcessorRunsAfterPublication()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    bool sawPublishedGeneration = false;
    bool replacementGuardWasActive = false;
    bool activityMutationWasBlocked = false;
    setRideCacheRemovalProcessorAction([&] {
        sawPublishedGeneration =
            !QFileInfo::exists(firstPath)
            && readBytes(thirdPath) == QByteArray("new third");
        replacementGuardWasActive =
            fixture.cache->replacementOperationInProgressForTest();
        activityMutationWasBlocked =
            fixture.cache->activityMutationBlockedForTest();
    });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(sawPublishedGeneration);
    QVERIFY(replacementGuardWasActive);
    QVERIFY(activityMutationWasBlocked);
}

void TestRideCacheRemoval::
plannedReplacementProcessorUsesIsolatedRide()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    std::unique_ptr<RideFile> liveRide(new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 0)), 1.0));
    RideItem *first = fixture.addPlannedRide(
        firstName(), false, liveRide.get());
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(rideCacheRemovalLastProcessedRide());
    QVERIFY(rideCacheRemovalLastProcessedRide() != liveRide.get());
}

void TestRideCacheRemoval::
plannedReplacementArchivesOldGenerationAndInvalidatesDerivedFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString secondPath = fixture.plannedActivityPath(secondName());
    const QString firstPlannedCpx = fixture.plannedCachePath(
        firstName(), QStringLiteral("cpx"));
    const QString secondPlannedCpx = fixture.plannedCachePath(
        secondName(), QStringLiteral("cpx"));
    const QString firstNotes = fixture.cachePath(
        firstName(), QStringLiteral("notes"));
    const QString completedCpx = fixture.cachePath(
        firstName(), QStringLiteral("cpx"));
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(secondPath, QByteArray("old second"));
    writeFixture(
        fixture.plannedBackupPath(firstName()),
        QByteArray("previous first backup"));
    writeFixture(firstPlannedCpx, QByteArray("stale first plan cache"));
    writeFixture(secondPlannedCpx, QByteArray("stale second plan cache"));
    writeFixture(firstNotes, QByteArray("stale first notes"));
    writeFixture(completedCpx, QByteArray("completed cache decoy"));

    const QList<RideCache::PlannedActivityTarget> targets = {
        {secondName(),
         [](const QString &path, QString &error) {
             return stageBytes(path, QByteArray("new second"), error);
         }},
        {thirdName(),
         [](const QString &path, QString &error) {
             return stageBytes(path, QByteArray("new third"), error);
         }}};
    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first, second}, {firstPath, secondPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(
        readBytes(fixture.plannedBackupPath(firstName())),
        QByteArray("old first"));
    QCOMPARE(
        readBytes(fixture.plannedBackupPath(secondName())),
        QByteArray("old second"));
    QVERIFY(!QFileInfo::exists(firstPlannedCpx));
    QVERIFY(!QFileInfo::exists(secondPlannedCpx));
    QVERIFY(!QFileInfo::exists(firstNotes));
    QCOMPARE(readBytes(completedCpx), QByteArray("completed cache decoy"));
}

void TestRideCacheRemoval::
plannedReplacementFailurePreservesBackupAndDerivedFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *second = fixture.addPlannedRide(secondName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString secondPath = fixture.plannedActivityPath(secondName());
    const QString firstBackup = fixture.plannedBackupPath(firstName());
    const QString secondBackup = fixture.plannedBackupPath(secondName());
    const QString firstPlannedCpx = fixture.plannedCachePath(
        firstName(), QStringLiteral("cpx"));
    const QString secondNotes = fixture.cachePath(
        secondName(), QStringLiteral("notes"));
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(secondPath, QByteArray("old second"));
    writeFixture(firstBackup, QByteArray("previous first backup"));
    writeFixture(secondBackup, QByteArray("previous second backup"));
    writeFixture(firstPlannedCpx, QByteArray("first plan cache"));
    writeFixture(secondNotes, QByteArray("second notes"));

    const QList<RideCache::PlannedActivityTarget> targets = {
        {secondName(),
         [](const QString &path, QString &error) {
             return stageBytes(path, QByteArray("new second"), error);
         }},
        {thirdName(),
         [](const QString &, QString &error) {
             error = QStringLiteral("injected backup rollback failure point");
             return false;
         }}};
    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first, second}, {firstPath, secondPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QCOMPARE(readBytes(secondPath), QByteArray("old second"));
    QCOMPARE(readBytes(firstBackup), QByteArray("previous first backup"));
    QCOMPARE(readBytes(secondBackup), QByteArray("previous second backup"));
    QCOMPARE(readBytes(firstPlannedCpx), QByteArray("first plan cache"));
    QCOMPARE(readBytes(secondNotes), QByteArray("second notes"));
}

void TestRideCacheRemoval::
plannedReplacementSupportsImportWithoutRemovals()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *selected = fixture.addRide(firstName(), true);
    writeFixture(
        fixture.activityPath(firstName()),
        QByteArray("selected completed activity"));
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("imported plan"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles({}, {}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(result.removedCount, 0);
    QCOMPARE(result.addedCount, 1);
    QCOMPARE(readBytes(targetPath), QByteArray("imported plan"));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
    QCOMPARE(fixture.context->ride, selected);
}

void TestRideCacheRemoval::
plannedReplacementCoordinatorWrapsDurableCommit()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    QString journalPath;
    bool decisionCalled = false;
    bool completionCalled = false;
    bool commitMarkerWasVisible = false;
    RideCache::PlannedReplacementCoordinator coordinator;
    coordinator.commit =
        [&](const QString &path, bool &committed, QString &) {
            decisionCalled = true;
            journalPath = path;
            committed = true;
            return QFileInfo::exists(
                QDir(path).filePath(
                    QStringLiteral("manifest.json")));
        };
    coordinator.complete = [&](QString &) {
        completionCalled = true;
        commitMarkerWasVisible = QFileInfo::exists(
            QDir(journalPath).filePath(
                QStringLiteral("COMMITTED")));
        return commitMarkerWasVisible;
    };

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets, false,
            coordinator);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(decisionCalled);
    QVERIFY(completionCalled);
    QVERIFY(commitMarkerWasVisible);
    QVERIFY(!journalPath.isEmpty());
    QVERIFY(!QFileInfo::exists(journalPath));
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(thirdPath), QByteArray("new third"));
}

void TestRideCacheRemoval::
plannedReplacementRetainsJournalAfterExternalCommitFailure()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    QString journalPath;
    RideCache::PlannedReplacementCoordinator coordinator;
    coordinator.commit =
        [&](const QString &path, bool &committed, QString &error) {
            journalPath = path;
            committed = true;
            error = QStringLiteral(
                "injected failure after durable decision");
            return false;
        };
    coordinator.complete = [](QString &) { return true; };

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets, false,
            coordinator);

    QVERIFY(result.committed);
    QVERIFY(!result.cacheUpdated);
    QVERIFY(!result.cleanupComplete);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(thirdPath));
    QVERIFY(QFileInfo::exists(journalPath));

    QString recoveryError;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::openPrepared(
            fixture.athlete->home->root().absolutePath(),
            QFileInfo(journalPath).fileName(),
            recoveryError);
    QVERIFY2(journal, qPrintable(recoveryError));
    QVERIFY2(journal->publishAndCommit(recoveryError),
             qPrintable(recoveryError));
    QVERIFY2(journal->cleanupAfterCommit(recoveryError),
             qPrintable(recoveryError));
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(thirdPath), QByteArray("new third"));
}

void TestRideCacheRemoval::
plannedReplacementRejectsInvalidTargets_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("unsafe-name") << 0;
    QTest::newRow("malformed-date") << 1;
    QTest::newRow("duplicate-target") << 2;
    QTest::newRow("existing-untracked-file") << 3;
    QTest::newRow("existing-cache-item") << 4;
    QTest::newRow("missing-stager") << 5;
}

void TestRideCacheRemoval::
plannedReplacementRejectsInvalidTargets()
{
    QFETCH(int, scenario);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString outsidePath = QDir(fixture.temporary.path()).filePath(
        QStringLiteral("outside.json"));
    writeFixture(firstPath, QByteArray("old first"));

    QString targetName = thirdName();
    QList<RideCache::PlannedActivityTarget> targets;
    const auto validStager = [](const QString &path, QString &error) {
        return stageBytes(path, QByteArray("new plan"), error);
    };
    switch (scenario) {
    case 0:
        targetName = QStringLiteral("../outside.json");
        targets.append({targetName, validStager});
        break;
    case 1:
        targetName = QStringLiteral("not-a-date.json");
        targets.append({targetName, validStager});
        break;
    case 2:
        targets.append({targetName, validStager});
        targets.append({targetName, validStager});
        break;
    case 3:
        writeFixture(
            fixture.plannedActivityPath(targetName),
            QByteArray("untracked existing plan"));
        targets.append({targetName, validStager});
        break;
    case 4:
        fixture.addPlannedRide(targetName, false);
        writeFixture(
            fixture.plannedActivityPath(targetName),
            QByteArray("existing cached plan"));
        targets.append({targetName, validStager});
        break;
    case 5:
        targets.append({targetName, {}});
        break;
    default:
        QFAIL("unknown invalid-target scenario");
    }

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
    QVERIFY(!QFileInfo::exists(outsidePath));
    if (scenario == 3) {
        QCOMPARE(
            readBytes(fixture.plannedActivityPath(targetName)),
            QByteArray("untracked existing plan"));
    } else if (scenario == 4) {
        QCOMPARE(
            readBytes(fixture.plannedActivityPath(targetName)),
            QByteArray("existing cached plan"));
    }
}

void TestRideCacheRemoval::
plannedReplacementCacheMutationDuringStageRollsBack()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [first](const QString &path, QString &error) {
            first->path = QStringLiteral("concurrently-mutated-path");
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);
    first->path = fixture.athlete->home->planned().absolutePath();

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(targetPath));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
}

void TestRideCacheRemoval::
plannedReplacementSourceMutationDuringStageFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [firstPath](const QString &path, QString &error) {
            if (!stageBytes(path, QByteArray("new third"), error))
                return false;
            return stageBytes(
                firstPath, QByteArray("concurrent first"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("concurrent first"));
    QVERIFY(!QFileInfo::exists(targetPath));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
}

void TestRideCacheRemoval::
plannedReplacementRejectsLinkedActivity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    first->setLinkedFileName(secondName());
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("linked old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("linked old first"));
    QVERIFY(!QFileInfo::exists(targetPath));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
}

void TestRideCacheRemoval::
plannedReplacementSnapshotsTargetsBeforeCallbacks()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));

    QList<RideCache::PlannedActivityTarget> targets;
    targets.append({
        secondName(),
        [&targets](const QString &path, QString &error) {
            const bool staged = stageBytes(
                path, QByteArray("new second"), error);
            targets.clear();
            return staged;
        }});
    targets.append({
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }});

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(result.addedCount, 2);
    QVERIFY(cacheContains(*fixture.cache, secondName()));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
}

void TestRideCacheRemoval::
plannedReplacementRejectsLinkIntroducedDuringStage()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [first](const QString &path, QString &error) {
            first->setLinkedFileName(secondName());
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    first->setLinkedFileName(QString());
    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(thirdName())));
}

void TestRideCacheRemoval::
plannedReplacementRejectsSidecarOwnerIntroducedDuringStage()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString notes = fixture.cachePath(
        firstName(), QStringLiteral("notes"));
    writeFixture(firstPath, QByteArray("old first"));
    writeFixture(notes, QByteArray("shared notes"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [&fixture](const QString &path, QString &error) {
            fixture.addRide(firstName(), false);
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QCOMPARE(readBytes(notes), QByteArray("shared notes"));
}

void TestRideCacheRemoval::
plannedReplacementRejectsDerivedFileIntroducedDuringStage()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString notes = fixture.cachePath(
        firstName(), QStringLiteral("notes"));
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [notes](const QString &path, QString &error) {
            return stageBytes(notes, QByteArray("late notes"), error)
                && stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QCOMPARE(readBytes(notes), QByteArray("late notes"));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(thirdName())));
}

void TestRideCacheRemoval::
plannedReplacementStageMutationAfterValidationRollsBack()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    setRideCacheRemovalValidationMutation(
        QByteArray("changed after semantic validation"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(thirdPath));
}

void TestRideCacheRemoval::
plannedReplacementStageCallbackDeletionIsContained()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *unrelated = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedUnrelated(unrelated);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [guardedUnrelated](const QString &path, QString &error) {
            if (guardedUnrelated)
                delete guardedUnrelated.data();
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(guardedUnrelated.isNull());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(modelResetCount, 1);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
}

void TestRideCacheRemoval::
plannedReplacementStageCallbackDeferredDeletionIsContained()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *unrelated = fixture.addRide(secondName(), false);
    RideItem *const unrelatedAddress = unrelated;
    const QPointer<RideItem> guardedUnrelated(unrelated);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [guardedUnrelated](const QString &path, QString &error) {
            if (guardedUnrelated)
                guardedUnrelated->deleteLater();
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    if (!guardedUnrelated) {
        fixture.cache->mutableRidesForRemovalTest()
            .removeOne(unrelatedAddress);
    }
    QVERIFY(!result.committed);
    QVERIFY(guardedUnrelated.isNull());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(modelResetCount, 1);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
}

void TestRideCacheRemoval::
plannedReplacementRejectsUnreadableStagedActivity()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray(), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(
        fixture.plannedActivityPath(thirdName())));
}

void TestRideCacheRemoval::plannedReplacementRejectsDirtyTarget()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    first->isdirty = true;
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
}

void TestRideCacheRemoval::
plannedReplacementRejectsWrongThreadBeforeCallbacks()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    int stageCalls = 0;
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [&stageCalls](const QString &, QString &) {
            ++stageCalls;
            return true;
        }}};
    RideCache::PlannedReplacementResult directResult;
    RideCache::PlannedReplacementResult copyResult;

    std::thread worker([&] {
        directResult = fixture.cache->replacePlannedActivityFiles(
            {first}, {}, targets);
        copyResult = fixture.cache->replacePlannedActivities(
            {first}, {{first, QDate(2026, 7, 7)}});
    });
    worker.join();

    QVERIFY(!directResult.committed);
    QVERIFY(!copyResult.committed);
    QVERIFY(directResult.error.contains(
        QStringLiteral("cache thread")));
    QVERIFY(copyResult.error.contains(
        QStringLiteral("cache thread")));
    QCOMPARE(stageCalls, 0);
    QCOMPARE(rideCacheRemovalCancelCount(), 0);
}

void TestRideCacheRemoval::
plannedReplacementCorruptCommitMarkerRequiresRecovery()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            const QString markerPath =
                QFileInfo(path).absoluteDir().filePath(
                    QStringLiteral("COMMITTED"));
            QFile marker(markerPath);
            if (!marker.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                || marker.write("invalid-marker\n") != 15
                || !marker.flush()) {
                error = marker.errorString();
                return false;
            }
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(targetPath));
    const QDir journalRoot(QDir(fixture.temporary.path()).filePath(
        QStringLiteral(".gc-transactions/plan-replacement")));
    QVERIFY(!journalRoot.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void TestRideCacheRemoval::
plannedReplacementBackupParentSymlinkIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QString backupRoot =
        fixture.athlete->home->fileBackup().absolutePath();
    QVERIFY(QDir(backupRoot).removeRecursively());
    const QString external = QDir(fixture.temporary.path()).filePath(
        QStringLiteral("external-backups"));
    QVERIFY(QDir().mkpath(external));
    if (!createTestSymbolicLink(
            external, backupRoot, true)) {
        QSKIP("Directory symbolic links are unavailable");
    }
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!QFileInfo::exists(
        QDir(external).filePath(QStringLiteral("planned"))));
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
}

void TestRideCacheRemoval::
plannedReplacementBackupRootSyncFailurePreservesGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString thirdPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const QString backupRoot =
        fixture.athlete->home->fileBackup().absolutePath();
    QVERIFY(QDir(backupRoot).removeRecursively());
    setRideCacheRemovalSyncFailurePath(backupRoot);
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(firstPath), QByteArray("old first"));
    QVERIFY(!QFileInfo::exists(thirdPath));
}

void TestRideCacheRemoval::
plannedReplacementInvalidatesStartupSnapshots()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(rideCacheRemovalStartupInvalidationCount(), 1);
}

void TestRideCacheRemoval::
plannedReplacementModelSignalDeletesOldItemSafely_data()
{
    QTest::addColumn<bool>("deleteBeforeReset");
    QTest::newRow("model-about-to-reset") << true;
    QTest::newRow("model-reset") << false;
}

void TestRideCacheRemoval::
plannedReplacementModelSignalDeletesOldItemSafely()
{
    QFETCH(bool, deleteBeforeReset);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), true);
    const QPointer<RideItem> guardedFirst(first);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    const auto deleteOldItem = [guardedFirst] {
        if (guardedFirst) delete guardedFirst.data();
    };
    bool replacementGuardWasActive = false;
    bool activityMutationWasBlocked = false;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        [&fixture, &replacementGuardWasActive,
         &activityMutationWasBlocked] {
            replacementGuardWasActive =
                fixture.cache->replacementOperationInProgressForTest();
            activityMutationWasBlocked =
                fixture.cache->activityMutationBlockedForTest();
        });
    bool selectionWasSafeDuringReset = false;
    if (deleteBeforeReset) {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelAboutToBeReset,
            deleteOldItem);
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelAboutToBeReset,
            [&fixture, &selectionWasSafeDuringReset] {
                selectionWasSafeDuringReset =
                    fixture.context->ride == nullptr;
            });
    } else {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelReset,
            deleteOldItem);
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelReset,
            [&fixture, &selectionWasSafeDuringReset] {
                selectionWasSafeDuringReset =
                    fixture.context->ride == nullptr;
            });
    }
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedFirst.isNull());
    QVERIFY(selectionWasSafeDuringReset);
    QVERIFY(replacementGuardWasActive);
    QVERIFY(activityMutationWasBlocked);
    QCOMPARE(fixture.cache->count(), 1);
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(targetPath), QByteArray("new third"));
}

void TestRideCacheRemoval::
plannedReplacementModelSignalDeletesUnrelatedItemSafely_data()
{
    QTest::addColumn<bool>("deleteBeforeReset");
    QTest::addColumn<int>("expectedResetCount");
    QTest::newRow("model-about-to-reset") << true << 1;
    QTest::newRow("model-reset") << false << 2;
}

void TestRideCacheRemoval::
plannedReplacementModelSignalDeletesUnrelatedItemSafely()
{
    QFETCH(bool, deleteBeforeReset);
    QFETCH(int, expectedResetCount);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *unrelated = fixture.addRide(secondName(), false);
    const QPointer<RideItem> guardedUnrelated(unrelated);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    const auto deleteUnrelated = [guardedUnrelated] {
        if (guardedUnrelated)
            delete guardedUnrelated.data();
    };
    if (deleteBeforeReset) {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelAboutToBeReset,
            deleteUnrelated);
    } else {
        connect(
            fixture.cache->model(),
            &QAbstractItemModel::modelReset,
            deleteUnrelated);
    }
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(result.committed);
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(guardedUnrelated.isNull());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(modelResetCount, expectedResetCount);
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
}

void TestRideCacheRemoval::
plannedReplacementModelResetDeletesIncomingItemSafely()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), true);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&fixture] {
            for (RideItem *item : fixture.cache->rides()) {
                if (item && item->fileName == thirdName()) {
                    delete item;
                    return;
                }
            }
        });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(result.committed);
    QVERIFY(!result.cacheUpdated);
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(modelResetCount, 2);
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(targetPath), QByteArray("new third"));
}

void TestRideCacheRemoval::
plannedReplacementDefersNestedConfigReset_data()
{
    QTest::addColumn<qint32>("changes");
    QTest::newRow("zero-mask") << qint32(0);
    QTest::newRow("field-mask") << qint32(1);
}

void TestRideCacheRemoval::
plannedReplacementDefersNestedConfigReset()
{
    QFETCH(qint32, changes);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    int resetDepth = 0;
    int maximumResetDepth = 0;
    int resetCount = 0;
    bool configChangeSent = false;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        [&fixture, changes, &resetDepth, &maximumResetDepth,
         &configChangeSent] {
            ++resetDepth;
            maximumResetDepth = qMax(maximumResetDepth, resetDepth);
            if (!configChangeSent) {
                configChangeSent = true;
                fixture.context->configChanged(changes);
            }
        });
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&resetDepth, &resetCount] {
            --resetDepth;
            ++resetCount;
        });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);
    QCoreApplication::processEvents();

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(maximumResetDepth, 1);
    QCOMPARE(resetDepth, 0);
    QCOMPARE(resetCount, 2);
}

void TestRideCacheRemoval::
modelResetRemainsBalancedAcrossReentrantInsert()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideCacheModel *const model = fixture.cache->model();
    int resetStarted = 0;
    int resetFinished = 0;
    int insertStarted = 0;
    int insertFinished = 0;
    bool inserted = false;

    connect(
        model, &QAbstractItemModel::modelAboutToBeReset,
        [model, &resetStarted, &inserted] {
            ++resetStarted;
            if (inserted) return;
            inserted = true;
            model->startInsert(0, 0);
            model->endInsert();
        });
    connect(
        model, &QAbstractItemModel::modelReset,
        [&resetFinished] { ++resetFinished; });
    connect(
        model, &QAbstractItemModel::rowsAboutToBeInserted,
        [&insertStarted] { ++insertStarted; });
    connect(
        model, &QAbstractItemModel::rowsInserted,
        [&insertFinished] { ++insertFinished; });

    model->beginReset();
    model->endReset();

    QCOMPARE(resetStarted, 1);
    QCOMPARE(resetFinished, 1);
    QCOMPARE(insertStarted, 0);
    QCOMPARE(insertFinished, 0);
}

void TestRideCacheRemoval::
modelInsertRejectsReentrantReset()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideCacheModel *const model = fixture.cache->model();
    bool resetWasAllowed = true;
    int resetStarted = 0;
    int resetFinished = 0;

    connect(
        model, &QAbstractItemModel::rowsAboutToBeInserted,
        [model, &resetWasAllowed] {
            resetWasAllowed = model->beginReset();
            if (resetWasAllowed) model->endReset();
        });
    connect(
        model, &QAbstractItemModel::modelAboutToBeReset,
        [&resetStarted] { ++resetStarted; });
    connect(
        model, &QAbstractItemModel::modelReset,
        [&resetFinished] { ++resetFinished; });

    QVERIFY(model->startInsert(0, 0));
    model->endInsert();

    QVERIFY(!resetWasAllowed);
    QCOMPARE(resetStarted, 0);
    QCOMPARE(resetFinished, 0);
}

void TestRideCacheRemoval::
plannedReplacementNotificationDeletionIsContained_data()
{
    QTest::addColumn<QString>("phase");
    QTest::newRow("ride-deleted") << QStringLiteral("ride-deleted");
    QTest::newRow("ride-added") << QStringLiteral("ride-added");
    QTest::newRow("ride-selected") << QStringLiteral("ride-selected");
}

void TestRideCacheRemoval::
plannedReplacementNotificationDeletionIsContained()
{
    QFETCH(QString, phase);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), true);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    bool correctiveResetSelectionWasSafe = false;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelAboutToBeReset,
        [&fixture, &modelResetCount,
         &correctiveResetSelectionWasSafe] {
            if (modelResetCount == 1) {
                correctiveResetSelectionWasSafe =
                    fixture.context->ride == nullptr;
            }
        });
    bool deleted = false;
    const auto deleteNamed = [&fixture, &deleted](const QString &name) {
        if (deleted) return;
        for (RideItem *item : fixture.cache->rides()) {
            if (item && item->fileName == name) {
                deleted = true;
                delete item;
                return;
            }
        }
    };
    if (phase == QStringLiteral("ride-deleted")) {
        connect(
            fixture.context.get(), &Context::rideDeleted,
            [&deleteNamed](RideItem *) {
                deleteNamed(thirdName());
            });
    } else if (phase == QStringLiteral("ride-added")) {
        connect(
            fixture.context.get(), &Context::rideAdded,
            [&deleteNamed](RideItem *item) {
                if (item && item->fileName == secondName())
                    deleteNamed(thirdName());
            });
    } else {
        connect(
            fixture.context.get(), &Context::rideSelected,
            [&deleted](RideItem *item) {
                if (!deleted && item) {
                    deleted = true;
                    delete item;
                }
            });
    }
    const QList<RideCache::PlannedActivityTarget> targets = {
        {secondName(),
         [](const QString &path, QString &error) {
             return stageBytes(path, QByteArray("new second"), error);
         }},
        {thirdName(),
         [](const QString &path, QString &error) {
             return stageBytes(path, QByteArray("new third"), error);
         }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets, true);

    QVERIFY(deleted);
    QVERIFY(result.committed);
    QVERIFY(!result.cleanlyCompleted());
    QCOMPARE(fixture.cache->count(), 1);
    QCOMPARE(modelResetCount, 2);
    QVERIFY(correctiveResetSelectionWasSafe);
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
}

void TestRideCacheRemoval::
plannedReplacementDeletionNotificationCanDestroyOldItem()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *const firstAddress = first;
    const QPointer<RideItem> guardedFirst(first);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    connect(
        fixture.context.get(), &Context::rideDeleted,
        [](RideItem *item) { delete item; });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    const qsizetype pendingDeletionCount =
        fixture.cache->pendingDeletionCountForRemovalTest();
    if (pendingDeletionCount != 0) {
        fixture.cache->discardPendingDeletionAddressForRemovalTest(
            firstAddress);
    }
    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedFirst.isNull());
    QCOMPARE(pendingDeletionCount, 0);
}

void TestRideCacheRemoval::
plannedReplacementDeletionNotificationCanDeferOldItemDeletion()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *const firstAddress = first;
    const QPointer<RideItem> guardedFirst(first);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    connect(
        fixture.context.get(), &Context::rideDeleted,
        [](RideItem *item) { item->deleteLater(); });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    const qsizetype pendingDeletionCount =
        fixture.cache->pendingDeletionCountForRemovalTest();
    if (pendingDeletionCount != 0) {
        fixture.cache->discardPendingDeletionAddressForRemovalTest(
            firstAddress);
    }
    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedFirst.isNull());
    QCOMPARE(pendingDeletionCount, 0);
}

void TestRideCacheRemoval::
plannedReplacementMarksDestroyedAddressDuringCallback()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    RideItem *const firstAddress = first;
    const QPointer<RideItem> guardedFirst(first);
    bool tombstoneVisibleDuringDestruction = false;
    connect(
        first, &QObject::destroyed,
        [&fixture, firstAddress,
         &tombstoneVisibleDuringDestruction] {
            tombstoneVisibleDuringDestruction =
                fixture.cache->remembersDeletedAddressForRemovalTest(
                    firstAddress);
        });
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    connect(
        fixture.context.get(), &Context::rideDeleted,
        [](RideItem *item) { delete item; });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QVERIFY(guardedFirst.isNull());
    QVERIFY(tombstoneVisibleDuringDestruction);
    QVERIFY(!fixture.cache->remembersDeletedAddressForRemovalTest(
        firstAddress));
}

void TestRideCacheRemoval::
plannedReplacementAdditionNotificationCanDeferIncomingDeletion()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), true);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    RideItem *incomingAddress = nullptr;
    QPointer<RideItem> guardedIncoming;
    connect(
        fixture.context.get(), &Context::rideAdded,
        [&incomingAddress, &guardedIncoming](RideItem *item) {
            incomingAddress = item;
            guardedIncoming = item;
            item->deleteLater();
        });
    int modelResetCount = 0;
    connect(
        fixture.cache->model(),
        &QAbstractItemModel::modelReset,
        [&modelResetCount] { ++modelResetCount; });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets, true);

    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    if (!guardedIncoming && incomingAddress) {
        fixture.cache->mutableRidesForRemovalTest()
            .removeOne(incomingAddress);
        if (fixture.context->ride == incomingAddress)
            fixture.context->ride = nullptr;
    }
    QVERIFY(result.committed);
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(guardedIncoming.isNull());
    QCOMPARE(fixture.cache->count(), 0);
    QCOMPARE(modelResetCount, 2);
}

void TestRideCacheRemoval::
plannedReplacementNotificationOwnerLossIsReported()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), false);
    const QPointer<RideItem> guardedFirst(first);
    RideCache *const cache = fixture.cache.get();
    const QString firstPath = fixture.plannedActivityPath(firstName());
    writeFixture(firstPath, QByteArray("old first"));
    connect(
        fixture.context.get(), &Context::rideDeleted,
        [&fixture, cache, guardedFirst](RideItem *) {
            for (RideItem *item : cache->rides()) {
                if (item) item->context = nullptr;
            }
            if (guardedFirst) guardedFirst->context = nullptr;
            fixture.athlete->context = nullptr;
            delete fixture.context.release();
        });
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(result.committed);
    QVERIFY(!result.cleanlyCompleted());
    QVERIFY(!result.error.isEmpty());
}

void TestRideCacheRemoval::
plannedReplacementModelSignalOwnerDestructionDoesNotContinue_data()
{
    QTest::addColumn<bool>("destroyCache");
    QTest::addColumn<bool>("destroyBeforeReset");
    QTest::newRow("cache-model-about-to-reset") << true << true;
    QTest::newRow("cache-model-reset") << true << false;
    QTest::newRow("context-model-about-to-reset") << false << true;
    QTest::newRow("context-model-reset") << false << false;
}

void TestRideCacheRemoval::
plannedReplacementModelSignalOwnerDestructionDoesNotContinue()
{
    QFETCH(bool, destroyCache);
    QFETCH(bool, destroyBeforeReset);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *first = fixture.addPlannedRide(firstName(), true);
    RideCache *const cache = fixture.cache.get();
    const QPointer<RideCache> guardedCache(cache);
    const QPointer<Context> guardedContext(fixture.context.get());
    const QPointer<RideItem> guardedFirst(first);
    const QString firstPath = fixture.plannedActivityPath(firstName());
    const QString targetPath = fixture.plannedActivityPath(thirdName());
    writeFixture(firstPath, QByteArray("old first"));

    bool destroyed = false;
    const auto destroyOwner = [&] {
        if (destroyed) return;
        destroyed = true;
        if (destroyCache) {
            fixture.athlete->rideCache = nullptr;
            fixture.context->ride = nullptr;
            delete fixture.cache.release();
            return;
        }
        for (RideItem *item : cache->rides()) {
            if (item) item->context = nullptr;
        }
        if (guardedFirst) guardedFirst->context = nullptr;
        fixture.athlete->context = nullptr;
        delete fixture.context.release();
    };
    if (destroyBeforeReset) {
        connect(
            cache->model(),
            &QAbstractItemModel::modelAboutToBeReset,
            destroyOwner);
    } else {
        connect(
            cache->model(),
            &QAbstractItemModel::modelReset,
            destroyOwner);
    }
    const QList<RideCache::PlannedActivityTarget> targets = {{
        thirdName(),
        [](const QString &path, QString &error) {
            return stageBytes(path, QByteArray("new third"), error);
        }}};

    const RideCache::PlannedReplacementResult result =
        cache->replacePlannedActivityFiles(
            {first}, {firstPath}, targets);

    QVERIFY(result.committed);
    QVERIFY(result.cleanupComplete);
    QVERIFY(!QFileInfo::exists(firstPath));
    QCOMPARE(readBytes(targetPath), QByteArray("new third"));
    if (destroyCache) {
        QVERIFY(guardedCache.isNull());
        QVERIFY(guardedContext);
        QCOMPARE(guardedContext->ride, nullptr);
    } else {
        QVERIFY(guardedContext.isNull());
        QVERIFY(guardedCache);
    }
}

void TestRideCacheRemoval::
plannedCopyReplacementCommitsOneCompleteGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *oldTarget =
        fixture.addPlannedRide(firstName(), true);
    const QString sourceName =
        QStringLiteral("2026_07_05_10_00_00.json");
    RideItem *source =
        fixture.addPlannedRide(sourceName, false);
    source->dateTime = QDateTime(
        QDate(2026, 7, 5), QTime(10, 0));
    const QString oldPath =
        fixture.plannedActivityPath(firstName());
    const QString sourcePath =
        fixture.plannedActivityPath(sourceName);
    const QString targetPath =
        fixture.plannedActivityPath(thirdName());
    writeFixture(oldPath, QByteArray("old target"));
    writeFixture(sourcePath, QByteArray("repeat source"));

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivities(
            {oldTarget}, {{source, QDate(2026, 7, 6)}});

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(result.removedCount, 1);
    QCOMPARE(result.addedCount, 1);
    QVERIFY(!QFileInfo::exists(oldPath));
    QCOMPARE(readBytes(sourcePath), QByteArray("repeat source"));
    QCOMPARE(readBytes(targetPath), QByteArray("repeat source"));
    QVERIFY(!cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, sourceName));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
    QCoreApplication::processEvents();
    QCOMPARE(rideCacheRemovalRefreshCount(), 1);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 1);
}

void TestRideCacheRemoval::
plannedCopyReplacementMissingSourcePreservesOldGeneration()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *oldTarget =
        fixture.addPlannedRide(firstName(), true);
    const QString sourceName =
        QStringLiteral("2026_07_05_10_00_00.json");
    RideItem *source =
        fixture.addPlannedRide(sourceName, false);
    source->dateTime = QDateTime(
        QDate(2026, 7, 5), QTime(10, 0));
    const QString oldPath =
        fixture.plannedActivityPath(firstName());
    const QString targetPath =
        fixture.plannedActivityPath(thirdName());
    writeFixture(oldPath, QByteArray("old target"));

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivities(
            {oldTarget}, {{source, QDate(2026, 7, 6)}});

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readBytes(oldPath), QByteArray("old target"));
    QVERIFY(!QFileInfo::exists(targetPath));
    QVERIFY(cacheContains(*fixture.cache, firstName()));
    QVERIFY(cacheContains(*fixture.cache, sourceName));
    QCOMPARE(rideCacheRemovalRefreshCount(), 0);
    QCOMPARE(rideCacheRemovalEstimatorRefreshCount(), 0);
}

void TestRideCacheRemoval::
plannedCopyReplacementRejectsStaleSourceWithoutDereference()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *const stale =
        reinterpret_cast<RideItem *>(quintptr(1));

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivities(
            {}, {{stale, QDate(2026, 7, 6)}});

    QVERIFY(!result.committed);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(fixture.cache->count(), 0);
}

void TestRideCacheRemoval::
plannedCopyReplacementCanReplaceSameTargetPath()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    RideItem *oldTarget =
        fixture.addPlannedRide(thirdName(), true);
    const QString sourceName =
        QStringLiteral("2026_07_05_10_00_00.json");
    RideItem *source =
        fixture.addPlannedRide(sourceName, false);
    source->dateTime = QDateTime(
        QDate(2026, 7, 5), QTime(10, 0));
    const QString oldTargetPath =
        fixture.plannedActivityPath(thirdName());
    const QString sourcePath =
        fixture.plannedActivityPath(sourceName);
    writeFixture(oldTargetPath, QByteArray("old target"));
    writeFixture(sourcePath, QByteArray("repeat source"));

    const RideCache::PlannedReplacementResult result =
        fixture.cache->replacePlannedActivities(
            {oldTarget}, {{source, QDate(2026, 7, 6)}});

    QVERIFY2(result.cleanlyCompleted(), qPrintable(result.error));
    QCOMPARE(readBytes(oldTargetPath), QByteArray("repeat source"));
    QCOMPARE(readBytes(sourcePath), QByteArray("repeat source"));
    QCOMPARE(
        readBytes(fixture.plannedBackupPath(thirdName())),
        QByteArray("old target"));
    QVERIFY(cacheContains(*fixture.cache, sourceName));
    QVERIFY(cacheContains(*fixture.cache, thirdName()));
    QCOMPARE(fixture.cache->count(), 2);
}

QTEST_MAIN(TestRideCacheRemoval)
#include "testRideCacheRemoval.moc"
