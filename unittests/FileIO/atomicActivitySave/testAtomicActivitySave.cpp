#include <QtTest>

#include "AnchoredFileSystem.h"
#include "AtomicFileWriter.h"
#include "JsonRideFile.h"
#include "LinkedActivitySaveJournal.h"
#include "RideCache.h"
#include "SaveDialogs.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdlib>
#include <functional>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

void resetAtomicActivitySaveProcessorStub();
void setAtomicActivitySaveProcessorFailure(bool enabled);
int atomicActivitySaveProcessorCalls();
void setAtomicActivitySaveSetDirtyAction(
    RideItem *rideItem, std::function<void()> action);
void setAtomicActivitySaveSetRideAction(
    RideItem *rideItem, std::function<void()> action);
void setAtomicActivitySaveCloseAction(
    RideItem *rideItem, std::function<void()> action);

namespace {

QByteArray linkedActivitySaveActionTransition;
std::function<void()> linkedActivitySaveAction;
QString anchoredFilesystemSyncFailurePath;
std::function<void(const char *, const QString &, const QString &)>
    anchoredFilesystemAction;
bool forceLegacyWindowsDelete = false;

#ifdef Q_OS_WIN
class WindowsTestHandle
{
public:
    explicit WindowsTestHandle(
        HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsTestHandle() { reset(); }

    bool isValid() const
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset()
    {
        if (isValid()) ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

} // namespace

void setLinkedActivitySaveTransitionAction(
    const QByteArray &transition,
    const std::function<void()> &action)
{
    linkedActivitySaveActionTransition = transition;
    linkedActivitySaveAction = action;
}

void clearLinkedActivitySaveTransitionAction()
{
    linkedActivitySaveActionTransition.clear();
    linkedActivitySaveAction = {};
}

void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary)
{
    if (anchoredFilesystemAction) {
        anchoredFilesystemAction(transition, primary, secondary);
    }
}

bool anchoredFilesystemSyncFailureRequested(const QString &path)
{
    return !anchoredFilesystemSyncFailurePath.isEmpty()
        && path == anchoredFilesystemSyncFailurePath;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return forceLegacyWindowsDelete;
}

void linkedActivitySaveTransitionReached(const char *transition)
{
    if (linkedActivitySaveAction
        && linkedActivitySaveActionTransition == transition) {
        std::function<void()> action = std::move(linkedActivitySaveAction);
        linkedActivitySaveActionTransition.clear();
        action();
    }
    const QByteArray requested = qgetenv(
        "GC_LINKED_ACTIVITY_SAVE_CRASH_PHASE");
    if (requested.isEmpty() || requested != transition) return;
    static QHash<QByteArray, int> occurrences;
    const int occurrence = ++occurrences[requested];
    bool valid = false;
    const int requestedOccurrence = qEnvironmentVariableIntValue(
        "GC_LINKED_ACTIVITY_SAVE_CRASH_OCCURRENCE", &valid);
    if (occurrence == (valid ? requestedOccurrence : 1)) {
        std::_Exit(86);
    }
}

namespace {

enum class FailurePoint {
    None,
    Open,
    ShortWrite,
    Flush,
    Commit,
    CorruptCommit,
    MissingCommit
};

enum class LockArtifactEntryKind
{
    RegularFile,
    SymbolicLink,
    Directory
};

class FaultInjectingWriter final : public AtomicFileWriter
{
public:
    FaultInjectingWriter(
        const QString &path, FailurePoint failure,
        std::function<void()> afterCommit = std::function<void()>())
        : path_(path), failure_(failure),
          afterCommit_(std::move(afterCommit))
    {
    }

    bool open() override
    {
        return failure_ != FailurePoint::Open;
    }

    qint64 write(const QByteArray &data) override
    {
        staged_ = data;
        if (failure_ == FailurePoint::ShortWrite) {
            return data.isEmpty() ? -1 : data.size() - 1;
        }
        return data.size();
    }

    bool flush() override
    {
        return failure_ != FailurePoint::Flush;
    }

    bool commit() override
    {
        if (failure_ == FailurePoint::Commit) {
            return false;
        }
        if (failure_ == FailurePoint::MissingCommit) {
            QFile::remove(path_);
            return true;
        }

        QFile target(path_);
        if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        QByteArray committed = staged_;
        if (failure_ == FailurePoint::CorruptCommit) {
            committed.append("corrupt");
        }
        const bool success =
            target.write(committed) == committed.size()
            && target.flush();
        if (success && afterCommit_) {
            afterCommit_();
        }
        return success;
    }

    void cancelWriting() override
    {
        staged_.clear();
    }

    QString errorString() const override
    {
        return QStringLiteral("injected failure");
    }

private:
    QString path_;
    FailurePoint failure_;
    QByteArray staged_;
    std::function<void()> afterCommit_;
};

class ObservableRideFile final : public RideFile
{
public:
    using RideFile::RideFile;
    void markSaved() { emitSaved(); }
};

class TestSaveSingleDialog final : public SaveSingleDialogWidget
{
public:
    explicit TestSaveSingleDialog(RideItem *rideItem)
        : SaveSingleDialogWidget(nullptr, nullptr, rideItem)
    {
    }

    bool saveResult = true;
    int saveCalls = 0;
    int errorReports = 0;
    std::function<void()> saveAction;

protected:
    bool saveRide(QString &error) override
    {
        ++saveCalls;
        const bool result = saveResult;
        const std::function<void()> action = saveAction;
        saveAction = {};
        if (action) action();
        if (!result) {
            error = QStringLiteral("injected dialog save failure");
        }
        return result;
    }

    void reportSaveError(const QString &) override
    {
        ++errorReports;
    }
};

class TestSaveOnExitDialog final : public SaveOnExitDialogWidget
{
public:
    explicit TestSaveOnExitDialog(const QList<RideItem *> &rides)
        : SaveOnExitDialogWidget(nullptr, nullptr, rides)
    {
        for (RideItem *ride : rides) {
            trackedActivities.append(QPointer<RideItem>(ride));
        }
    }

    QHash<RideItem *, bool> results;
    QHash<RideItem *, std::function<void()>> saveActions;
    QHash<RideItem *, RideItem *> linkedActivities;
    QList<RideItem *> calls;
    std::function<void()> saveAction;
    int errorReports = 0;

    void trackActivity(RideItem *rideItem)
    {
        trackedActivities.append(QPointer<RideItem>(rideItem));
    }

protected:
    QList<RideItem *> currentDirtyActivities() const override
    {
        QList<RideItem *> dirty;
        for (const QPointer<RideItem> &activity : trackedActivities) {
            if (activity && activity->isDirty()) {
                dirty.append(activity.data());
            }
        }
        return dirty;
    }

    bool saveRide(RideItem *rideItem) override
    {
        calls.append(rideItem);
        QPointer<RideItem> guardedRide(rideItem);
        const bool result = results.value(rideItem, true);
        std::function<void()> action = saveActions.take(rideItem);
        if (!action) {
            action = saveAction;
            saveAction = {};
        }
        if (action) action();
        if (result && guardedRide) {
            guardedRide->setDirty(false);
        }
        return result;
    }

    void reportSaveError(const QString &) override
    {
        ++errorReports;
    }

    RideItem *linkedActivityForSaveGroup(
        RideItem *rideItem) const override
    {
        return linkedActivities.value(rideItem, nullptr);
    }

private:
    QList<QPointer<RideItem>> trackedActivities;
};

QString activityFileName(const QDateTime &startTime, const QString &suffix)
{
    return startTime.toString(QStringLiteral("yyyy_MM_dd_hh_mm_ss"))
        + QLatin1Char('.') + suffix;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

void writeFixture(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    QVERIFY(file.flush());
}

bool createHardLink(const QString &source, const QString &target)
{
#ifdef Q_OS_UNIX
    return ::link(
        QFile::encodeName(source).constData(),
        QFile::encodeName(target).constData()) == 0;
#elif defined(Q_OS_WIN)
    return ::CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(target.utf16()),
        reinterpret_cast<LPCWSTR>(source.utf16()),
        nullptr);
#else
    Q_UNUSED(source)
    Q_UNUSED(target)
    return false;
#endif
}

bool replaceAndPinFixture(
    const QString &path,
    const QString &retainedPath,
    const QByteArray &bytes,
    AnchoredFileSystem::DirectoryAnchor &parent,
    AnchoredFileSystem::EntryRef &entry,
    AnchoredFileSystem::PinnedFile &file,
    QString &error)
{
    error.clear();
    if (!QFile::rename(path, retainedPath)) {
        error = QStringLiteral("Cannot retain the transaction-owned fixture");
        return false;
    }
    QFile replacement(path);
    if (!replacement.open(QIODevice::WriteOnly)
        || replacement.write(bytes) != bytes.size()
        || !replacement.flush()) {
        error = replacement.errorString();
        return false;
    }
    replacement.close();
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            QFileInfo(path).absolutePath(), parent, error)) {
        return false;
    }
    entry = parent.entry(QFileInfo(path).fileName(), error);
    return entry.isValid()
        && AnchoredFileSystem::pinRegularFile(entry, file, error);
}

LinkedActivitySave::Specification linkedSaveJournalSpecification(
    const QString &root)
{
    LinkedActivitySave::Specification specification;
    specification.athleteRoot = root;
    specification.entries = {
        {QDir(root).filePath(QStringLiteral("first-old.json")),
         QDir(root).filePath(QStringLiteral("first-new.json")),
         QDir(root).filePath(QStringLiteral("first-old.json.bak")),
         false},
        {QDir(root).filePath(QStringLiteral("second-old.json")),
         QDir(root).filePath(QStringLiteral("second-new.json")),
         QDir(root).filePath(QStringLiteral("second-old.json.bak")),
         false}};
    return specification;
}

void writeLinkedSaveJournalSources(const QString &root)
{
    writeFixture(
        QDir(root).filePath(QStringLiteral("first-old.json")),
        QByteArray("first old generation"));
    writeFixture(
        QDir(root).filePath(QStringLiteral("second-old.json")),
        QByteArray("second old generation"));
}

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

} // namespace

class TestAtomicActivitySave : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void failurePreservesOriginalAndDirty_data();
    void failurePreservesOriginalAndDirty();
    void successReplacesOriginalAndMarksClean();
    void finalizeFailureKeepsDirty();
    void qSaveFileWriterCommitsReplacement();
    void conversionMovesOriginalToBackup();
    void jsonRenameRemovesSupersededSource();
    void finalizationFailurePreservesTargetAndKeepsDirty();
    void finalizationFailurePreservesPreviousBackup();
    void finalizationSyncFailureRestoresSource_data();
    void finalizationSyncFailureRestoresSource();
    void atomicWriterRejectsSymlinkTarget();
    void atomicWriterRejectsCollisionAtCommit();
    void atomicWriterRejectsLockedTarget();
    void newWriterRollsBackPartialPublishFailure();
    void newWriterPreservesReplacementAfterPartialPublishFailure();
    void newWriterRejectsSuccessfulNoPublish();
    void stagedFileSetPublishesAll();
    void publicationFailureSkipsCacheUpdate();
    void publicationSuccessUpdatesCacheAfterPublish();
    void stagedFileSetRollsBackOnMiddleFailure();
    void stagedFileSetRollsBackPartiallyPublishedFailure();
    void stagedFileSetCleansStagingOnCollision();
    void stagedFileSetRejectsUnsafePathGraphs();
    void stagedFileSetRejectsInvalidAndDuplicateStages();
    void stagedFileSetReleasesLocksAfterLockFailure();
    void stagedFileSetFinalizesWhileTargetsAreLocked();
    void stagedFileSetReturnsSuccessfulFinalizerWarning();
    void stagedFileSetRollsBackWhenFinalizerFails();
    void atomicFileLockTargetName_data();
    void atomicFileLockTargetName();
    void lockSetKeepsCaseDistinctPaths();
    void atomicWriterPreservesConcurrentNewTarget();
    void jsonWriterFailurePreservesOriginal_data();
    void jsonWriterFailurePreservesOriginal();
    void jsonWriterKeepsUtf8BomAndRoundTrips();
    void jsonReaderRejectsMalformedPayload();
    void jsonWriterRejectsOpenTarget();
    void jsonWriterRejectsExistingTargetWhenReplacementDisabled();
    void jsonReaderKeepsLegacyLatin1Compatibility();
    void jsonReaderPreservesUtf8ReplacementCharacter();
    void jsonReaderPreservesLargeInteger();
    void jsonReaderPreservesPositiveExponent();
    void jsonRoundTripsReferenceAndSampleState();
    void saveHelpersRejectInvalidOperations();
    void successfulTransactionPreservesLiveObjectState();
    void transactionRemainsCommittedWhenWriterCommitDestroysRide();
    void transactionRollsBackPathChangeWhenWriterCommitDestroysRide();
    void transactionRemainsCommittedWhenMarkCleanDestroysRide();
    void transactionRemainsCommittedWhenFinalizeDestroysRide();
    void transactionPreservesZeroValuedSeriesPresence();
    void processorFailureSkipsWriteAndKeepsDirty();
    void failedSaveRestoresHistoryUntilCommit();
    void candidateSaveTransfersOnlyAfterSuccess();
    void candidateSaveRejectsCurrentDestroyedBySave();
    void candidateSaveRejectsCandidateDestroyedBySave();
    void candidateSaveRejectsReplacementDestroyedBySave();
    void candidateSaveRejectsCandidateDestroyedBySetDirty();
    void candidateSaveRejectsCurrentDestroyedBySetRide();
    void defaultProcessorFailurePreservesFileAndDirtyState();
    void mainWindowConvertsSourceAfterCommit();
    void mainWindowRenamesJsonAfterCommit();
    void linkedFilenameSaveFailureNeverPublishesPrefix();
    void linkedFilenameSavePublishesCompletePair();
    void linkedConversionPublishesCompletePairAndBackup();
    void linkedBatchProcessesAllBeforeSerializing();
    void linkedBatchProcessorFailureLeavesCompleteOldGeneration();
    void linkedBatchStopsWhenPeerDestroyedDuringProcessing();
    void linkedSaveRequirementRejectsUnpreparedRenames();
    void pendingLinkedRemovalJournalBlocksLinkedSaveTransaction();
    void concurrentLinkedSaveJournalsAreSerialized();
    void abandonedLinkedSaveJournalBlocksNextTransaction();
    void linkedSaveQLockFileRemovalGuardDoesNotPoisonReconcile_data();
    void linkedSaveQLockFileRemovalGuardDoesNotPoisonReconcile();
    void linkedSaveUnsafeQLockFileRemovalGuardsRemainRejected_data();
    void linkedSaveUnsafeQLockFileRemovalGuardsRemainRejected();
    void linkedSaveJournalLocksCompleteProductionPathSet_data();
    void linkedSaveJournalLocksCompleteProductionPathSet();
    void linkedSaveJournalRejectsUnsafePathGraphs_data();
    void linkedSaveJournalRejectsUnsafePathGraphs();
    void linkedSaveRejectsHardLinkedRetirementSource();
    void linkedSaveRetirementSkipsInPlaceEntry_data();
    void linkedSaveRetirementSkipsInPlaceEntry();
    void linkedSaveCleanupReleasesTransactionResources_data();
    void linkedSaveCleanupReleasesTransactionResources();
    void linkedSaveNondurableJournalRemovalStaysRetryable_data();
    void linkedSaveNondurableJournalRemovalStaysRetryable();
    void linkedSaveNondurableJournalRemovalRejectsReplacement_data();
    void linkedSaveNondurableJournalRemovalRejectsReplacement();
    void linkedSaveNondurableJournalFileRemovalStaysRetryable_data();
    void linkedSaveNondurableJournalFileRemovalStaysRetryable();
    void linkedSaveTrackedJournalReplacementStopsRetry_data();
    void linkedSaveTrackedJournalReplacementStopsRetry();
    void linkedSaveNondurableRetirementStaysRetryable();
    void linkedSavePartialRetirementKeepsRecoveryJournal();
    void linkedSavePublishRetryPreservesIncompleteRetirementIntent();
    void linkedSaveIncompleteRetirementBlocksFreshRecovery();
    void linkedSaveRetirementIntentControlFiles_data();
    void linkedSaveRetirementIntentControlFiles();
    void linkedSaveRetirementIntentMustRemainNamed();
    void linkedSaveIntentRemovalPartialStaysRetryable();
    void linkedSaveIntentRemovalNondurableStaysRetryable();
#ifdef Q_OS_WIN
    void linkedSavePendingWindowsRetirementRetriesAfterHandleClose();
    void linkedSaveWindowsJournalRemovalRetriesAfterHandleClose();
    void linkedSaveLegacyWindowsJournalFileRemovalRetries_data();
    void linkedSaveLegacyWindowsJournalFileRemovalRetries();
#endif
    void linkedSavePreparedSourceIdentityReplacementIsRejected_data();
    void linkedSavePreparedSourceIdentityReplacementIsRejected();
    void linkedSaveSourceRetirementSubstitutionIsRejected_data();
    void linkedSaveSourceRetirementSubstitutionIsRejected();
    void linkedSaveSourceRepopulationStopsFurtherRetirement();
    void linkedSaveFinalSourceRepopulationIsRejected();
    void linkedSaveCommitBoundarySourceRepopulationIsRejected_data();
    void linkedSaveCommitBoundarySourceRepopulationIsRejected();
    void linkedSaveRollbackPreservesRecreatedRetiredSource_data();
    void linkedSaveRollbackPreservesRecreatedRetiredSource();
    void linkedSaveRollbackPublicationRacePreservesNewSource();
    void linkedSaveRollbackProductionSubstitutionIsRejected_data();
    void linkedSaveRollbackProductionSubstitutionIsRejected();
    void linkedSaveRollbackProductionParentSubstitutionIsRejected_data();
    void linkedSaveRollbackProductionParentSubstitutionIsRejected();
    void linkedSaveRollbackPreservesLateCommitMarker();
    void linkedSaveMovedLiveJournalCannotClaimCleanup();
    void linkedSaveMovedJournalCannotReceiveCommitMarker();
    void linkedSaveNondurableCommitMarkerStaysRecoverable();
    void linkedSaveCommitMarkerIdentityLossIsRejected_data();
    void linkedSaveCommitMarkerIdentityLossIsRejected();
    void linkedSavePartialCommitMarkerPublicationBlocksRollback();
#ifdef Q_OS_WIN
    void anchoredFilesDenyConcurrentWindowsWrites_data();
    void anchoredFilesDenyConcurrentWindowsWrites();
    void anchoredOutputFilesCanBeRepinned_data();
    void anchoredOutputFilesCanBeRepinned();
#endif
    void linkedSaveRecoveryRejectsStagedSourceAtOldName();
    void linkedSaveRecoveryRejectsRecreatedOriginalSource_data();
    void linkedSaveRecoveryRejectsRecreatedOriginalSource();
    void linkedSaveCommittedRecoverySubstitutionIsRejected_data();
    void linkedSaveCommittedRecoverySubstitutionIsRejected();
    void linkedSaveJournalCleanupSubstitutionPreservesReplacement_data();
    void linkedSaveJournalCleanupSubstitutionPreservesReplacement();
    void linkedSavePreManifestCleanupSubstitutionPreservesReplacement();
    void linkedSavePreManifestRemovalDurabilityStaysRetryable();
    void linkedSaveRecoverySourceRetirementSubstitutionIsRejected_data();
    void linkedSaveRecoverySourceRetirementSubstitutionIsRejected();
    void linkedSaveRecoveryRepopulationStopsFurtherRetirement();
    void linkedSavePublicationPreservesExternalChanges_data();
    void linkedSavePublicationPreservesExternalChanges();
    void linkedSaveRecoveryRejectsUnsafeJournalEntries_data();
    void linkedSaveRecoveryRejectsUnsafeJournalEntries();
    void linkedSavePreManifestRecoveryRejectsUnknownEntries_data();
    void linkedSavePreManifestRecoveryRejectsUnknownEntries();
    void linkedSaveOversizedControlFileFailsBeforeRead_data();
    void linkedSaveOversizedControlFileFailsBeforeRead();
    void linkedSaveTransactionDirectoriesArePrivate();
    void linkedSaveRecoveryRestrictsExistingDirectories();
    void linkedSaveRecoveryWithoutJournalAllowsSymlinkedRoot();
    void linkedFilenameSaveCrashRecoversCompleteGeneration_data();
    void linkedFilenameSaveCrashRecoversCompleteGeneration();
    void mainWindowRejectsTargetCollision();
    void mainWindowHoldsSourceAndTargetLocks();
    void mainWindowFinalizeFailureRemainsRetryable();
    void mainWindowRejectsSourceChangedDuringSave();
    void mainWindowRejectsIdentityChangedByProcessor_data();
    void mainWindowRejectsIdentityChangedByProcessor();
    void mainWindowSurvivesItemDestroyedByProcessor();
    void mainWindowSaveSilentPropagatesFailure();
    void mainWindowSaveSilentPreservesUppercaseJsonPath();
    void mainWindowSaveRideSingleDialogPropagatesResult();
    void preflightSaveRelinksCompleteActivitySetBeforeSaving();
    void preflightFindsLinkedPeerByPredictedFilename();
    void preflightItemsRejectDestroyedActivity();
    void preflightRelinkRejectsDestroyedActivity();
    void preflightRejectsIdentityMutationDuringLaterRelink();
    void preflightRejectsDestroyedOwnerDuringRelink();
    void preflightRejectsDestroyedOwnerDuringSave();
    void discardReloadFailureIsRejected();
    void discardReloadSurvivesActivityDestroyedByClose();
    void saveSingleDialogSaveAndAbandon();
    void saveSingleDialogRejectsDestroyedActivity();
    void saveSingleDialogRejectsIdentityMutation_data();
    void saveSingleDialogRejectsIdentityMutation();
    void saveSingleDialogCommitsWhenActivityDestroyedDuringSave();
    void saveSingleDialogRejectsActivityDestroyedDuringFailedSave();
    void saveSingleDialogSurvivesParentDestroyedDuringSave();
    void saveOnExitDialogStopsUntilAllSelectedSave();
    void saveOnExitCompletesRenamedLinkedSaveGroup();
    void saveOnExitDialogDoesNotAcceptRedirtiedCompletedActivity();
    void saveOnExitDialogDoesNotAcceptNewDirtyActivity();
    void saveOnExitDialogDefersSkippedStateUntilSuccess();
    void saveOnExitAbandonMarksRedirtiedCompletedActivity();
    void saveOnExitAbandonMarksNewDirtyActivity();
    void saveOnExitDialogRejectsIdentityMutation_data();
    void saveOnExitDialogRejectsIdentityMutation();
    void saveOnExitDialogRejectsDestroyedActivity();
    void saveOnExitDialogCommitsWhenActivityDestroyedDuringSave();
    void saveOnExitDialogSurvivesParentDestroyedDuringSave();
    void rideCacheSaveActivityPropagatesFailure();
    void rideCacheSaveActivityRejectsDestructionDuringSave();
    void rideCacheSaveActivitiesAggregatesFailures();
    void rideCacheSaveActivitiesSurviveReentrantNextDeletion_data();
    void rideCacheSaveActivitiesSurviveReentrantNextDeletion();
    void rideCacheSaveActivitiesStopWhenOwnerDestroyed_data();
    void rideCacheSaveActivitiesStopWhenOwnerDestroyed();
};

void TestAtomicActivitySave::cleanup()
{
    clearLinkedActivitySaveTransitionAction();
    anchoredFilesystemSyncFailurePath.clear();
    anchoredFilesystemAction = {};
    forceLegacyWindowsDelete = false;
    resetAtomicActivitySaveProcessorStub();
}

void TestAtomicActivitySave::failurePreservesOriginalAndDirty_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("open")
        << static_cast<int>(FailurePoint::Open) << QStringLiteral("open");
    QTest::newRow("short-write")
        << static_cast<int>(FailurePoint::ShortWrite) << QStringLiteral("write");
    QTest::newRow("flush")
        << static_cast<int>(FailurePoint::Flush) << QStringLiteral("flush");
    QTest::newRow("commit")
        << static_cast<int>(FailurePoint::Commit) << QStringLiteral("commit");
    QTest::newRow("corrupt-commit")
        << static_cast<int>(FailurePoint::CorruptCommit) << QStringLiteral("match");
    QTest::newRow("missing-commit")
        << static_cast<int>(FailurePoint::MissingCommit) << QStringLiteral("verify");
}

void TestAtomicActivitySave::failurePreservesOriginalAndDirty()
{
    QFETCH(int, failurePoint);
    const FailurePoint failure = static_cast<FailurePoint>(failurePoint);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const AtomicFileWriterFactory factory = [failure](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(new FaultInjectingWriter(target, failure));
    };

    const bool saved = completeActivitySave(
        [&](QString &stepError) {
            return writeFileAtomically(path, replacement, factory, stepError);
        },
        [](QString &) { return true; },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), original);
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::successReplacesOriginalAndMarksClean()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const AtomicFileWriterFactory factory = [](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(
            new FaultInjectingWriter(target, FailurePoint::None));
    };

    QVERIFY(completeActivitySave(
        [&](QString &stepError) {
            return writeFileAtomically(path, replacement, factory, stepError);
        },
        [](QString &) { return true; },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error));

    QVERIFY(error.isEmpty());
    QCOMPARE(readAll(path), replacement);
    QVERIFY(!rideFileDirty);
    QVERIFY(!rideItemDirty);
}

void TestAtomicActivitySave::finalizeFailureKeepsDirty()
{
    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;

    const bool saved = completeActivitySave(
        [](QString &) { return true; },
        [](QString &stepError) {
            stepError = QStringLiteral("injected rename failure");
            return false;
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QCOMPARE(error, QStringLiteral("injected rename failure"));
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::qSaveFileWriterCommitsReplacement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    QString error;
    QVERIFY(writeFileAtomically(path, replacement,
                                qSaveFileWriterFactory(), error));
    QVERIFY(error.isEmpty());
    QCOMPARE(readAll(path), replacement);
}

void TestAtomicActivitySave::conversionMovesOriginalToBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("activity.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original imported activity");
    const QByteArray replacement("committed json activity");
    writeFixture(sourcePath, original);
    writeFixture(targetPath, replacement);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    QVERIFY(completeActivitySave(
        [](QString &) { return true; },
        [&](QString &stepError) {
            return finalizeActivityFileReplacement(sourcePath, targetPath,
                                                   true, stepError);
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error));

    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
    QCOMPARE(readAll(targetPath), replacement);
    QVERIFY(!rideFileDirty);
    QVERIFY(!rideItemDirty);
}

void TestAtomicActivitySave::jsonRenameRemovesSupersededSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("old.json"));
    const QString targetPath = dir.filePath(QStringLiteral("new.json"));
    writeFixture(sourcePath, QByteArray("old json"));
    writeFixture(targetPath, QByteArray("new json"));

    QString error;
    QVERIFY(finalizeActivityFileReplacement(sourcePath, targetPath,
                                            false, error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(targetPath), QByteArray("new json"));
}

void TestAtomicActivitySave::finalizationFailurePreservesTargetAndKeepsDirty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingSource = dir.filePath(QStringLiteral("missing.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(targetPath, QByteArray("committed json activity"));

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const bool saved = completeActivitySave(
        [](QString &) { return true; },
        [&](QString &stepError) {
            return finalizeActivityFileReplacement(missingSource, targetPath,
                                                   true, stepError);
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(targetPath), QByteArray("committed json activity"));
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::finalizationFailurePreservesPreviousBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingSource = dir.filePath(QStringLiteral("missing.fit"));
    const QString backupPath = missingSource + QStringLiteral(".bak");
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray previousBackup("previous backup");
    writeFixture(backupPath, previousBackup);
    writeFixture(targetPath, QByteArray("committed json activity"));

    QString error;
    QVERIFY(!finalizeActivityFileReplacement(missingSource, targetPath,
                                             true, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(backupPath), previousBackup);
    QCOMPARE(readAll(targetPath), QByteArray("committed json activity"));
}

void TestAtomicActivitySave::finalizationSyncFailureRestoresSource_data()
{
    QTest::addColumn<bool>("keepSourceBackup");
    QTest::newRow("remove-source") << false;
    QTest::newRow("keep-backup") << true;
}

void TestAtomicActivitySave::finalizationSyncFailureRestoresSource()
{
    QFETCH(bool, keepSourceBackup);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath =
        dir.filePath(QStringLiteral("original.fit"));
    const QString targetPath =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity");
    const QByteArray replacement("replacement activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(sourcePath, original);
    writeFixture(targetPath, replacement);
    if (keepSourceBackup) {
        writeFixture(sourcePath + QStringLiteral(".bak"), previousBackup);
    }

    int syncCalls = 0;
    const auto failDirectorySync =
        [&syncCalls](const QString &, QString &syncError) {
            ++syncCalls;
            syncError = QStringLiteral(
                "injected directory sync failure");
            return false;
        };

    QString error;
    QVERIFY(!finalizeActivityFileReplacement(
        sourcePath, targetPath, keepSourceBackup, error,
        failDirectorySync));
    QVERIFY(!error.isEmpty());
    QVERIFY(syncCalls >= 1);
    QCOMPARE(readAll(sourcePath), original);
    QCOMPARE(readAll(targetPath), replacement);
    if (keepSourceBackup) {
        QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")),
                 previousBackup);
    } else {
        QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    }
}

void TestAtomicActivitySave::atomicWriterRejectsSymlinkTarget()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates shortcuts rather than symlinks on Windows");
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString referentPath =
        dir.filePath(QStringLiteral("referent.json"));
    const QString linkPath =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("referent bytes");
    writeFixture(referentPath, original);
    QVERIFY(QFile::link(referentPath, linkPath));
    QVERIFY(QFileInfo(linkPath).isSymLink());

    QString error;
    QVERIFY(!writeFileAtomically(
        linkPath, QByteArray("replacement"),
        qSaveFileWriterFactory(), error));
    QVERIFY(error.contains(QStringLiteral("symbolic"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(referentPath), original);
    QVERIFY(QFileInfo(linkPath).isSymLink());
}

void TestAtomicActivitySave::atomicWriterRejectsCollisionAtCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray replacement("replacement activity");
    const QByteArray concurrent("concurrent activity");

    NewAtomicFileWriter writer(path);
    QVERIFY(writer.open());
    QCOMPARE(writer.write(replacement),
             static_cast<qint64>(replacement.size()));
    QVERIFY(writer.flush());
    writeFixture(path, concurrent);

    QVERIFY(!writer.commit());
    QVERIFY(writer.errorString().contains(
        QStringLiteral("concurrently"), Qt::CaseInsensitive));
    QCOMPARE(readAll(path), concurrent);
}

void TestAtomicActivitySave::atomicWriterRejectsLockedTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity");
    writeFixture(path, original);

    QLockFile lock(dir.filePath(QStringLiteral(".activity.json.lock")));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));

    int factoryCalls = 0;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            ++factoryCalls;
            return qSaveFileWriterFactory()(target, mode);
        };
    QString error;
    QVERIFY(!writeFileAtomically(
        path, QByteArray("replacement"), factory, error));
    QCOMPARE(factoryCalls, 0);
    QVERIFY(error.contains(QStringLiteral("already being saved"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::newWriterRollsBackPartialPublishFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const AtomicPublishFunction partialPublish =
        [](const QString &staging, const QString &target,
           bool &temporaryMoved, QString &error) {
            if (!publishAtomicNew(
                    staging, target, temporaryMoved, error)) {
                return false;
            }
            error = QStringLiteral("injected post-publication failure");
            return false;
        };

    NewAtomicFileWriter writer(path, partialPublish);
    QVERIFY(writer.open());
    QCOMPARE(writer.write(QByteArray("complete activity")),
             qint64(17));
    QVERIFY(writer.flush());
    QVERIFY(!writer.commit());
    QVERIFY(writer.errorString().contains(
        QStringLiteral("post-publication")));
    QVERIFY(!QFile::exists(path));
    QVERIFY(QDir(dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::
newWriterPreservesReplacementAfterPartialPublishFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QString retainedPath =
        dir.filePath(QStringLiteral("transaction-owned.json"));
    const QByteArray staged("complete activity");
    const QByteArray replacement = staged;
    AnchoredFileSystem::DirectoryAnchor parent;
    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile pin;
    QString injectionError;
    bool replacementPinned = false;
    const AtomicPublishFunction partialPublish =
        [&](const QString &stagingPath, const QString &target,
            bool &temporaryMoved, QString &error) {
            if (!publishAtomicNew(
                    stagingPath, target, temporaryMoved, error)) {
                return false;
            }
            replacementPinned = replaceAndPinFixture(
                target,
                retainedPath,
                replacement,
                parent,
                entry,
                pin,
                injectionError);
            temporaryMoved = true;
            error = QStringLiteral("injected post-publication failure");
            return false;
        };

    NewAtomicFileWriter writer(path, partialPublish);
    QVERIFY(writer.open());
    QCOMPARE(writer.write(staged), static_cast<qint64>(staged.size()));
    QVERIFY(writer.flush());
    QVERIFY(!writer.commit());
    QVERIFY2(replacementPinned, qPrintable(injectionError));
    QVERIFY(writer.errorString().contains(
        QStringLiteral("post-publication")));
    QCOMPARE(readAll(path), replacement);
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            entry, pin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "Rollback removed a replacement activity");
    QCOMPARE(readAll(retainedPath), staged);
    QVERIFY(QDir(dir.path()).entryList(
        {QStringLiteral("*.tmp")},
        QDir::Files | QDir::Hidden).isEmpty());
}

void TestAtomicActivitySave::newWriterRejectsSuccessfulNoPublish()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path =
        dir.filePath(QStringLiteral("activity.json"));
    const AtomicPublishFunction noPublish =
        [](const QString &, const QString &,
           bool &, QString &) {
            return true;
        };

    NewAtomicFileWriter writer(path, noPublish);
    QVERIFY(writer.open());
    const QByteArray contents("complete activity");
    QCOMPARE(writer.write(contents),
             static_cast<qint64>(contents.size()));
    QVERIFY(writer.flush());
    QVERIFY(!writer.commit());
    QVERIFY(writer.errorString().contains(
        QStringLiteral("did not create"),
        Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(path));
    QVERIFY(QDir(dir.path()).entryList(
        QDir::Files | QDir::Hidden
            | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::stagedFileSetPublishesAll()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstStage = dir.filePath(QStringLiteral(".first.stage"));
    const QString secondStage = dir.filePath(QStringLiteral(".second.stage"));
    const QString firstTarget = dir.filePath(QStringLiteral("first.json"));
    const QString secondTarget = dir.filePath(QStringLiteral("second.json"));
    writeFixture(firstStage, QByteArray("first activity"));
    writeFixture(secondStage, QByteArray("second activity"));

    QString error;
    QVERIFY2(publishStagedFileSet(
                 {
                     StagedFilePublication(firstStage, firstTarget),
                     StagedFilePublication(secondStage, secondTarget)
                 },
                 error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(firstStage));
    QVERIFY(!QFile::exists(secondStage));
    QCOMPARE(readAll(firstTarget), QByteArray("first activity"));
    QCOMPARE(readAll(secondTarget), QByteArray("second activity"));
}

void TestAtomicActivitySave::publicationFailureSkipsCacheUpdate()
{
    bool cacheUpdated = false;
    QString error;

    QVERIFY(!publishActivityBeforeCacheUpdate(
        [&](QString &publishError) {
            publishError =
                QStringLiteral("injected activity publication failure");
            return false;
        },
        [&]() { cacheUpdated = true; },
        error));

    QVERIFY(!cacheUpdated);
    QCOMPARE(
        error,
        QStringLiteral("injected activity publication failure"));
}

void TestAtomicActivitySave::
publicationSuccessUpdatesCacheAfterPublish()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage =
        dir.filePath(QStringLiteral(".activity.stage"));
    const QString target =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("durable activity");
    writeFixture(stage, contents);

    bool cacheUpdated = false;
    QString error;
    QVERIFY2(publishActivityBeforeCacheUpdate(
                 [&](QString &publishError) {
                     return publishStagedFileSet(
                         { StagedFilePublication(stage, target) },
                         publishError);
                 },
                 [&]() {
                     cacheUpdated = QFile::exists(target)
                         && !QFile::exists(stage)
                         && readAll(target) == contents;
                 },
                 error),
             qPrintable(error));

    QVERIFY(error.isEmpty());
    QVERIFY(cacheUpdated);
}

void TestAtomicActivitySave::stagedFileSetRollsBackOnMiddleFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 3; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral(".stage-%1").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("target-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &stage, const QString &target,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                temporaryMoved = false;
                publishError =
                    QStringLiteral("injected middle publication failure");
                return false;
            }
            return publishAtomicNew(
                stage, target, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(files, error, publish));
    QCOMPARE(publishCalls, 2);
    QVERIFY(error.contains(QStringLiteral("injected middle")));
    for (const StagedFilePublication &file : files) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::
stagedFileSetRollsBackPartiallyPublishedFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 3; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral(".stage-%1").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("target-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &stage, const QString &target,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                if (!QFile::copy(stage, target)) {
                    publishError =
                        QStringLiteral("cannot inject partial publication");
                    return false;
                }
                temporaryMoved = true;
                publishError =
                    QStringLiteral("injected post-publication failure");
                return false;
            }
            return publishAtomicNew(
                stage, target, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(files, error, publish));
    QCOMPARE(publishCalls, 2);
    QVERIFY(error.contains(QStringLiteral("post-publication")));
    for (const StagedFilePublication &file : files) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::stagedFileSetCleansStagingOnCollision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral(".activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray existing("existing activity");
    writeFixture(stage, QByteArray("replacement"));
    writeFixture(target, existing);

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &source, const QString &destination,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            return publishAtomicNew(
                source, destination, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(stage, target) }, error, publish));
    QCOMPARE(publishCalls, 0);
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), existing);
}


void TestAtomicActivitySave::stagedFileSetRejectsUnsafePathGraphs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString samePath =
        dir.filePath(QStringLiteral("same.json"));
    const QByteArray sameContents("must not be deleted");
    writeFixture(samePath, sameContents);

    QString error;
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(samePath, samePath) }, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(samePath), sameContents);

    const QString firstStage =
        dir.filePath(QStringLiteral("first.stage"));
    const QString secondStage =
        dir.filePath(QStringLiteral("second.stage"));
    const QString finalTarget =
        dir.filePath(QStringLiteral("final.json"));
    const QByteArray firstContents("first staging data");
    const QByteArray secondContents("second staging data");
    writeFixture(firstStage, firstContents);
    writeFixture(secondStage, secondContents);

    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstStage, secondStage),
            StagedFilePublication(secondStage, finalTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(firstStage), firstContents);
    QCOMPARE(readAll(secondStage), secondContents);
    QVERIFY(!QFile::exists(finalTarget));
}

void TestAtomicActivitySave::
stagedFileSetRejectsInvalidAndDuplicateStages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString validStage =
        dir.filePath(QStringLiteral("valid.stage"));
    const QString missingStage =
        dir.filePath(QStringLiteral("missing.stage"));
    const QString firstTarget =
        dir.filePath(QStringLiteral("first.json"));
    const QString secondTarget =
        dir.filePath(QStringLiteral("second.json"));
    writeFixture(validStage, QByteArray("valid staging data"));

    QString error;
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(validStage, firstTarget),
            StagedFilePublication(missingStage, secondTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(validStage));
    QVERIFY(!QFile::exists(firstTarget));
    QVERIFY(!QFile::exists(secondTarget));

    const QString duplicateStage =
        dir.filePath(QStringLiteral("duplicate.stage"));
    const QByteArray duplicateContents("duplicate staging data");
    writeFixture(duplicateStage, duplicateContents);
    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(duplicateStage, firstTarget),
            StagedFilePublication(duplicateStage, secondTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(duplicateStage), duplicateContents);
    QVERIFY(!QFile::exists(firstTarget));
    QVERIFY(!QFile::exists(secondTarget));

    const QString firstDuplicateTargetStage =
        dir.filePath(QStringLiteral("first-duplicate-target.stage"));
    const QString secondDuplicateTargetStage =
        dir.filePath(QStringLiteral("second-duplicate-target.stage"));
    writeFixture(firstDuplicateTargetStage, QByteArray("first"));
    writeFixture(secondDuplicateTargetStage, QByteArray("second"));
    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstDuplicateTargetStage, firstTarget),
            StagedFilePublication(secondDuplicateTargetStage, firstTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(firstDuplicateTargetStage));
    QVERIFY(!QFile::exists(secondDuplicateTargetStage));
    QVERIFY(!QFile::exists(firstTarget));

    const QString backing =
        dir.filePath(QStringLiteral("backing.stage"));
    const QString linkedStage =
        dir.filePath(QStringLiteral("linked.stage"));
    const QByteArray backingContents("backing data");
    writeFixture(backing, backingContents);
    if (!QFile::link(backing, linkedStage)
        || !QFileInfo(linkedStage).isSymLink()) {
        QSKIP("Symbolic links are unavailable on this platform");
    }
    error.clear();
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(linkedStage, firstTarget) }, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(linkedStage));
    QCOMPARE(readAll(backing), backingContents);
    QVERIFY(!QFile::exists(firstTarget));
}

void TestAtomicActivitySave::
stagedFileSetReleasesLocksAfterLockFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstStage =
        dir.filePath(QStringLiteral("first.stage"));
    const QString secondStage =
        dir.filePath(QStringLiteral("second.stage"));
    const QString firstTarget =
        dir.filePath(QStringLiteral("a.json"));
    const QString secondTarget =
        dir.filePath(QStringLiteral("z.json"));
    writeFixture(firstStage, QByteArray("first"));
    writeFixture(secondStage, QByteArray("second"));

    QLockFile heldLock(atomicFileLockPath(secondTarget));
    heldLock.setStaleLockTime(0);
    QVERIFY(heldLock.tryLock(0));

    QString error;
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstStage, firstTarget),
            StagedFilePublication(secondStage, secondTarget)
        },
        error));
    QVERIFY(error.contains(QStringLiteral("being saved"),
                           Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(firstStage));
    QVERIFY(!QFile::exists(secondStage));

    QLockFile firstLock(atomicFileLockPath(firstTarget));
    firstLock.setStaleLockTime(0);
    QVERIFY(firstLock.tryLock(0));
}

void TestAtomicActivitySave::stagedFileSetFinalizesWhileTargetsAreLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral("activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(stage, QByteArray("activity"));

    bool sawPublishedTarget = false;
    bool sawHeldTargetLock = false;
    int finalizeCalls = 0;
    const std::function<bool(QString &)> finalize =
        [&](QString &finalizeError) {
            ++finalizeCalls;
            sawPublishedTarget =
                readAll(target) == QByteArray("activity");

            QLockFile competingLock(atomicFileLockPath(target));
            competingLock.setStaleLockTime(0);
            sawHeldTargetLock = !competingLock.tryLock(0);
            if (!sawPublishedTarget || !sawHeldTargetLock) {
                finalizeError =
                    QStringLiteral("target was not durably locked");
                return false;
            }
            return true;
        };

    QString error;
    QVERIFY2(publishStagedFileSet(
                 { StagedFilePublication(stage, target) }, error,
                 publishAtomicNew, finalize),
             qPrintable(error));
    QCOMPARE(finalizeCalls, 1);
    QVERIFY(sawPublishedTarget);
    QVERIFY(sawHeldTargetLock);
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), QByteArray("activity"));
}

void TestAtomicActivitySave::stagedFileSetReturnsSuccessfulFinalizerWarning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral("activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(stage, QByteArray("activity"));

    const std::function<bool(QString &)> finalize =
        [](QString &finalizeError) {
            finalizeError = QStringLiteral("recovery warning");
            return true;
        };

    QString error;
    QVERIFY(publishStagedFileSet(
        { StagedFilePublication(stage, target) }, error,
        publishAtomicNew, finalize));
    QCOMPARE(error, QStringLiteral("recovery warning"));
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), QByteArray("activity"));
}

void TestAtomicActivitySave::stagedFileSetRollsBackWhenFinalizerFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 2; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral("activity-%1.stage").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("activity-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int finalizeCalls = 0;
    bool sawAllTargets = true;
    const std::function<bool(QString &)> finalize =
        [&](QString &finalizeError) {
            ++finalizeCalls;
            for (const StagedFilePublication &file : std::as_const(files)) {
                sawAllTargets = sawAllTargets
                    && QFile::exists(file.second);
            }
            finalizeError = QStringLiteral("injected finalization failure");
            return false;
        };

    QString error;
    QVERIFY(!publishStagedFileSet(
        files, error, publishAtomicNew, finalize));
    QCOMPARE(finalizeCalls, 1);
    QVERIFY(sawAllTargets);
    QVERIFY(error.contains(QStringLiteral("finalization failure")));
    for (const StagedFilePublication &file : std::as_const(files)) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::atomicFileLockTargetName_data()
{
    QTest::addColumn<QString>("entryName");
    QTest::addColumn<bool>("expectedValid");
    QTest::addColumn<QString>("expectedTarget");
    QTest::newRow("lock-without-target")
        << QStringLiteral(".lock") << false << QString();
    QTest::newRow("guard-without-target")
        << QStringLiteral(".lock.rmlock") << false << QString();
    QTest::newRow("ordinary-lock")
        << QStringLiteral(".target.lock") << true
        << QStringLiteral("target");
    QTest::newRow("single-removal-guard")
        << QStringLiteral(".target.lock.rmlock") << true
        << QStringLiteral("target");
    QTest::newRow("nested-removal-guard")
        << QStringLiteral(".target.lock.rmlock.rmlock") << true
        << QStringLiteral("target");
    QTest::newRow("uppercase-suffix")
        << QStringLiteral(".target.lock.RMLOCK") << false << QString();
    QTest::newRow("embedded-suffix")
        << QStringLiteral(".target.rmlock.lock") << true
        << QStringLiteral("target.rmlock");
    QTest::newRow("trailing-lookalike")
        << QStringLiteral(".target.lock.rmlock.tmp") << false << QString();
    QTest::newRow("missing-dot-prefix")
        << QStringLiteral("target.lock.rmlock") << false << QString();
}

void TestAtomicActivitySave::atomicFileLockTargetName()
{
    QFETCH(QString, entryName);
    QFETCH(bool, expectedValid);
    QFETCH(QString, expectedTarget);
    QString targetName = QStringLiteral("stale output");
    QCOMPARE(
        ::atomicFileLockTargetName(entryName, targetName),
        expectedValid);
    QCOMPARE(targetName, expectedTarget);
}

void TestAtomicActivitySave::lockSetKeepsCaseDistinctPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString upperPath =
        dir.filePath(QStringLiteral("Activity.json"));
    const QString lowerPath =
        dir.filePath(QStringLiteral("activity.json"));
    writeFixture(upperPath, QByteArray("upper"));
    if (QFileInfo(lowerPath).exists()) {
        QSKIP("The test filesystem is case-insensitive");
    }

    AtomicFileLockSet locks;
    QString error;
    QVERIFY2(locks.lock({ upperPath, lowerPath }, error),
             qPrintable(error));

    QLockFile upperLock(atomicFileLockPath(upperPath));
    upperLock.setStaleLockTime(0);
    QLockFile lowerLock(atomicFileLockPath(lowerPath));
    lowerLock.setStaleLockTime(0);
    QVERIFY(!upperLock.tryLock(0));
    QVERIFY(!lowerLock.tryLock(0));
}

void TestAtomicActivitySave::atomicWriterPreservesConcurrentNewTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray concurrent("concurrent activity");
    bool sawCreateNew = false;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            sawCreateNew = mode == AtomicFileMode::CreateNew;
            writeFixture(target, concurrent);
            return std::unique_ptr<AtomicFileWriter>(
                new NewAtomicFileWriter(target));
        };

    QString error;
    QVERIFY(!writeFileAtomically(
        path, QByteArray("replacement"), factory, error, false));
    QVERIFY(sawCreateNew);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), concurrent);
}

void TestAtomicActivitySave::jsonWriterFailurePreservesOriginal_data()
{
    failurePreservesOriginalAndDirty_data();
}

void TestAtomicActivitySave::jsonWriterFailurePreservesOriginal()
{
    QFETCH(int, failurePoint);
    QFETCH(QString, expectedError);
    const FailurePoint failure = static_cast<FailurePoint>(failurePoint);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Notes"), QStringLiteral("atomic test"));
    const AtomicFileWriterFactory factory = [failure](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(
            new FaultInjectingWriter(target, failure));
    };
    JsonFileReader reader(factory);
    QFile target(path);
    QString error;

    QVERIFY(!reader.writeRideFile(nullptr, &ride, target, error));
    QVERIFY(!error.isEmpty());
    QVERIFY2(error.contains(expectedError, Qt::CaseInsensitive),
             qPrintable(error));
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::jsonWriterKeepsUtf8BomAndRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    const QString notes = QString::fromUtf8("Hyv\xC3\xA4 harjoitus");
    ride.setTag(QStringLiteral("Notes"), notes);

    JsonFileReader reader;
    QFile target(path);
    QString error;
    QVERIFY2(reader.writeRideFile(nullptr, &ride, target, error),
             qPrintable(error));

    const QByteArray bytes = readAll(path);
    QVERIFY(bytes.startsWith(QByteArray::fromHex("efbbbf")));

    QStringList parseErrors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()), notes);
}

void TestAtomicActivitySave::jsonReaderRejectsMalformedPayload()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("malformed.json"));
    writeFixture(path, QByteArray::fromHex("efbbbf")
                           + QByteArray("{\"RIDE\":{\"STARTTIME\":"));

    JsonFileReader reader;
    QStringList errors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(reader.openRideFile(input, errors));

    QVERIFY(parsed == nullptr);
    QVERIFY(!errors.isEmpty());
}

void TestAtomicActivitySave::jsonWriterRejectsOpenTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    JsonFileReader reader;
    QFile target(path);
    QVERIFY(target.open(QIODevice::ReadWrite));
    QString error;

    QVERIFY(!reader.writeRideFile(nullptr, &ride, target, error));
    QVERIFY(error.contains(QStringLiteral("open"), Qt::CaseInsensitive));
    QVERIFY(target.seek(0));
    QCOMPARE(target.readAll(), original);
    target.close();
}

void TestAtomicActivitySave::
jsonWriterRejectsExistingTargetWhenReplacementDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("existing activity");
    writeFixture(path, original);

    int factoryCalls = 0;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            ++factoryCalls;
            return qSaveFileWriterFactory()(target, mode);
        };
    JsonFileReader reader(factory);
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QFile target(path);
    QString error;

    QVERIFY(!reader.writeRideFile(
        nullptr, &ride, target, error, false));
    QVERIFY(error.contains(QStringLiteral("exists"),
                           Qt::CaseInsensitive));
    QCOMPARE(factoryCalls, 0);
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::jsonReaderKeepsLegacyLatin1Compatibility()
{
    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Notes"), QStringLiteral("placeholder"));

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    QByteArray latin1Notes("Hyv");
    latin1Notes.append(char(0xe4));
    QVERIFY(bytes.contains("placeholder"));
    bytes.replace("placeholder", latin1Notes);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr, qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()),
             QString::fromLatin1(latin1Notes));
}

void TestAtomicActivitySave::
jsonReaderPreservesUtf8ReplacementCharacter()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    const QString notes =
        QStringLiteral("before ")
        + QString(QChar::ReplacementCharacter)
        + QStringLiteral(" after");
    ride.setTag(QStringLiteral("Notes"), notes);

    JsonFileReader reader;
    const QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));

    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()),
             notes);
}

void TestAtomicActivitySave::jsonReaderPreservesLargeInteger()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.0;
    ride.appendPoint(sample);

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    const QByteArray original("\"SECS\":1");
    const QByteArray large("\"SECS\":3000000000");
    QVERIFY(bytes.contains(original));
    bytes.replace(original, large);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->dataPoints().size(), 1);
    QCOMPARE(parsed->dataPoints().constFirst()->secs,
             3000000000.0);
}

void TestAtomicActivitySave::jsonReaderPreservesPositiveExponent()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.0;
    ride.appendPoint(sample);

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    const QByteArray original("\"SECS\":1");
    const QByteArray exponent("\"SECS\":1e+10");
    QVERIFY(bytes.contains(original));
    bytes.replace(original, exponent);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->dataPoints().size(), 1);
    QCOMPARE(parsed->dataPoints().constFirst()->secs, 1e10);
}

void TestAtomicActivitySave::jsonRoundTripsReferenceAndSampleState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 12.5;
    sample.tcore = 38.125;
    sample.interval = 7;
    ride.appendPoint(sample);
    ride.setDataPresent(RideFile::tcore, true);
    ride.setDataPresent(RideFile::interval, true);

    RideFilePoint reference;
    reference.secs = 42.0;
    reference.watts = 321.0;
    reference.cad = 91.0;
    reference.hr = 155.0;
    ride.appendReference(reference);

    JsonFileReader reader;
    QFile target(path);
    QString error;
    QVERIFY2(reader.writeRideFile(nullptr, &ride, target, error),
             qPrintable(error));

    QStringList parseErrors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->referencePoints().size(), 1);
    const RideFilePoint *parsedReference =
        parsed->referencePoints().constFirst();
    QCOMPARE(parsedReference->secs, reference.secs);
    QCOMPARE(parsedReference->watts, reference.watts);
    QCOMPARE(parsedReference->cad, reference.cad);
    QCOMPARE(parsedReference->hr, reference.hr);
    QCOMPARE(parsed->dataPoints().size(), 1);
    const RideFilePoint *parsedSample =
        parsed->dataPoints().constFirst();
    QCOMPARE(parsedSample->tcore, sample.tcore);
    QCOMPARE(parsedSample->interval, sample.interval);
    QVERIFY(parsed->areDataPresent()->tcore);
    QVERIFY(parsed->areDataPresent()->interval);
}

void TestAtomicActivitySave::saveHelpersRejectInvalidOperations()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ActivitySaveOperations operations;
    QString error;

    QVERIFY(!saveActivityTransaction(
        nullptr, nullptr, path, operations, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("writer"),
                           Qt::CaseInsensitive));

    operations.writerFactory = qSaveFileWriterFactory();
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("complete"),
                           Qt::CaseInsensitive));

    operations.finalize = [](QString &) { return true; };
    operations.markClean = []() {};
    operations.stage = [](RideFile *, QString &) { return false; };
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("processor"),
                           Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(path));

    RideFile original(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile replacement(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideItem current(&original, nullptr);
    RideItem candidate(&replacement, nullptr);
    int saveCalls = 0;
    const ActivityCandidateSave save =
        [&](RideItem *, QString &) {
            ++saveCalls;
            return false;
        };

    QVERIFY(!saveActivityCandidate(
        nullptr, &candidate, &replacement, save, error));
    QVERIFY(!saveActivityCandidate(
        &current, &current, &replacement, save, error));
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &original, save, error));
    QCOMPARE(saveCalls, 0);

    error.clear();
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &replacement, save, error));
    QCOMPARE(saveCalls, 1);
    QVERIFY(!error.isEmpty());
    QCOMPARE(current.ride(false), &original);
    QCOMPARE(candidate.ride(false), &replacement);
}

void TestAtomicActivitySave::successfulTransactionPreservesLiveObjectState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.1234567890123;
    sample.watts = 287.987654321;
    ride.appendPoint(sample);
    RideFilePoint *const liveSample = ride.dataPoints().constFirst();

    RideFilePoint reference;
    reference.secs = 15.25;
    reference.watts = 350.5;
    ride.appendReference(reference);
    RideFilePoint *const liveReference =
        ride.referencePoints().constFirst();

    CIQinfo info(QStringLiteral("test-application"), 4, 2);
    info.fields.append(CIQfield(
        QStringLiteral("record"), QStringLiteral("test-field"),
        7, 8, QStringLiteral("float32"), QStringLiteral("unit"),
        10, 3));
    ride.addCIQ(info);

    bool markedClean = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [](QString &) { return true; };
    operations.markClean = [&]() {
        markedClean = true;
        ride.markSaved();
    };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, path, operations, error),
             qPrintable(error));
    QVERIFY(markedClean);
    QCOMPARE(ride.dataPoints().constFirst(), liveSample);
    QCOMPARE(liveSample->secs, sample.secs);
    QCOMPARE(liveSample->watts, sample.watts);
    QCOMPARE(ride.referencePoints().constFirst(), liveReference);
    QCOMPARE(liveReference->secs, reference.secs);
    QCOMPARE(liveReference->watts, reference.watts);
    QCOMPARE(ride.ciqinfo().size(), 1);
    QCOMPARE(ride.ciqinfo().constFirst().appid, info.appid);
    QCOMPARE(ride.ciqinfo().constFirst().fields.size(), 1);
}


void TestAtomicActivitySave::
transactionRemainsCommittedWhenWriterCommitDestroysRide()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path =
        dir.filePath(QStringLiteral("activity.json"));
    RideFile *ride = new RideFile(
        QDateTime(QDate(2026, 8, 1), QTime(11, 30)),
        1.0);
    QPointer<RideFile> guardedRide(ride);
    bool finalizeCalled = false;
    bool markCleanCalled = false;
    bool rollbackCalled = false;

    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None,
                    [&] {
                        delete ride;
                        ride = nullptr;
                    }));
        };
    operations.persistCompletesDurableTransaction = true;
    operations.finalize = [&](QString &) {
        finalizeCalled = true;
        return true;
    };
    operations.markClean = [&] {
        markCleanCalled = true;
    };
    operations.rollback = [&](QString &) {
        rollbackCalled = true;
        return QFile::remove(path);
    };

    QString error;
    QVERIFY2(saveActivityTransaction(
        nullptr, ride, path, operations, error),
        qPrintable(error));
    QVERIFY(guardedRide.isNull());
    QVERIFY(!finalizeCalled);
    QVERIFY(!markCleanCalled);
    QVERIFY(!rollbackCalled);
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo(path).isFile());
    QVERIFY(!readAll(path).isEmpty());
}

void TestAtomicActivitySave::
transactionRollsBackPathChangeWhenWriterCommitDestroysRide()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath =
        dir.filePath(QStringLiteral("activity.fit"));
    const QString targetPath =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("legacy source");
    writeFixture(sourcePath, original);
    RideFile *ride = new RideFile(
        QDateTime(QDate(2026, 8, 1), QTime(11, 45)),
        1.0);
    QPointer<RideFile> guardedRide(ride);
    bool finalizeCalled = false;
    bool markCleanCalled = false;
    bool rollbackCalled = false;

    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None,
                    [&] {
                        delete ride;
                        ride = nullptr;
                    }));
        };
    operations.allowTargetReplacement = false;
    operations.finalize = [&](QString &) {
        finalizeCalled = true;
        return true;
    };
    operations.markClean = [&] {
        markCleanCalled = true;
    };
    operations.rollback = [&](QString &rollbackError) {
        rollbackCalled = true;
        if (QFile::remove(targetPath)) return true;
        rollbackError = QStringLiteral(
            "cannot remove unfinalized target");
        return false;
    };

    QString error;
    QVERIFY(!saveActivityTransaction(
        nullptr, ride, targetPath,
        operations, error));
    QVERIFY(guardedRide.isNull());
    QVERIFY(!finalizeCalled);
    QVERIFY(!markCleanCalled);
    QVERIFY(rollbackCalled);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(sourcePath), original);
    QVERIFY(!QFile::exists(targetPath));
}

void TestAtomicActivitySave::
transactionRemainsCommittedWhenMarkCleanDestroysRide()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path =
        dir.filePath(QStringLiteral("activity.json"));
    RideFile *ride = new RideFile(
        QDateTime(QDate(2026, 8, 1), QTime(12, 0)),
        1.0);
    QPointer<RideFile> guardedRide(ride);

    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [](QString &) { return true; };
    operations.markClean = [&]() {
        delete ride;
        ride = nullptr;
    };

    QString error;
    QVERIFY2(saveActivityTransaction(
        nullptr, ride, path, operations, error),
        qPrintable(error));
    QVERIFY(guardedRide.isNull());
    QVERIFY(error.isEmpty());
    QVERIFY(QFile::exists(path));
}

void TestAtomicActivitySave::
transactionRemainsCommittedWhenFinalizeDestroysRide()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path =
        dir.filePath(QStringLiteral("activity.json"));
    RideFile *ride = new RideFile(
        QDateTime(QDate(2026, 8, 1), QTime(12, 30)),
        1.0);
    QPointer<RideFile> guardedRide(ride);
    bool markCleanCalled = false;

    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [&](QString &) {
        delete ride;
        ride = nullptr;
        return true;
    };
    operations.markClean = [&]() {
        markCleanCalled = true;
    };

    QString error;
    QVERIFY2(saveActivityTransaction(
        nullptr, ride, path, operations, error),
        qPrintable(error));
    QVERIFY(guardedRide.isNull());
    QVERIFY(!markCleanCalled);
    QVERIFY(error.isEmpty());
    QVERIFY(QFile::exists(path));
}

void TestAtomicActivitySave::transactionPreservesZeroValuedSeriesPresence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint point;
    point.secs = 1.0;
    ride.appendPoint(point);
    ride.setDataPresent(RideFile::hr, true);

    bool markedClean = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [](QString &) { return true; };
    operations.markClean = [&]() {
        markedClean = true;
        ride.markSaved();
    };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, path, operations, error),
             qPrintable(error));
    QVERIFY(markedClean);
    QVERIFY(readAll(path).contains(QByteArrayLiteral("\"HR\":0")));
}

void TestAtomicActivitySave::processorFailureSkipsWriteAndKeepsDirty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));

    int writerCreations = 0;
    int stageRuns = 0;
    int finalizeRuns = 0;
    int markedClean = 0;
    bool stageSawLiveRide = false;

    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            ++writerCreations;
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::None));
        };
    operations.stage = [&](RideFile *activity, QString &stageError) {
        ++stageRuns;
        stageSawLiveRide = activity == &ride;
        activity->setTag(QStringLiteral("State"), QStringLiteral("staged"));
        stageError = QStringLiteral("injected processor failure");
        return false;
    };
    operations.finalize = [&](QString &) {
        ++finalizeRuns;
        return true;
    };
    operations.markClean = [&]() { ++markedClean; };

    QString error;
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, targetPath, operations, error));
    QCOMPARE(error, QStringLiteral("injected processor failure"));
    QCOMPARE(stageRuns, 1);
    QCOMPARE(writerCreations, 0);
    QCOMPARE(finalizeRuns, 0);
    QCOMPARE(markedClean, 0);
    QVERIFY(stageSawLiveRide);
    QVERIFY(!QFile::exists(targetPath));
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("staged"));
}

void TestAtomicActivitySave::failedSaveRestoresHistoryUntilCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("activity.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(sourcePath, original);

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Processor Runs"), QStringLiteral("0"));
    const QString initialHistory = QStringLiteral("before\n");
    ride.setTag(QStringLiteral("Change History"), initialHistory);

    bool dirty = true;
    int savedSignals = 0;
    QObject::connect(&ride, &RideFile::saved, &ride, [&]() {
        dirty = false;
        ++savedSignals;
    });

    int stageRuns = 0;
    int finalizeRuns = 0;
    bool stageSawOnlyLiveRide = true;
    bool injectFinalizeFailure = true;
    ActivitySaveOperations operations;
    operations.stage = [&](RideFile *activity, QString &) {
        stageSawOnlyLiveRide =
            stageSawOnlyLiveRide && activity == &ride;
        ++stageRuns;
        const int runs =
            activity->getTag(QStringLiteral("Processor Runs"),
                             QStringLiteral("0")).toInt() + 1;
        activity->setTag(QStringLiteral("Processor Runs"),
                         QString::number(runs));
        return true;
    };
    operations.finalize = [&](QString &stepError) {
        ++finalizeRuns;
        if (injectFinalizeFailure) {
            stepError = QStringLiteral("injected finalize failure");
            return false;
        }
        return finalizeActivityFileReplacement(
            sourcePath, targetPath, true, stepError);
    };
    operations.markClean = [&]() { ride.markSaved(); };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    const QList<FailurePoint> writerFailures = {
        FailurePoint::Open, FailurePoint::ShortWrite,
        FailurePoint::Flush, FailurePoint::Commit,
        FailurePoint::CorruptCommit, FailurePoint::MissingCommit
    };
    for (const FailurePoint failure : writerFailures) {
        operations.writerFactory =
            [failure](const QString &target, AtomicFileMode) {
                return std::unique_ptr<AtomicFileWriter>(
                    new FaultInjectingWriter(target, failure));
            };

        QVERIFY(!saveActivityTransaction(
            nullptr, &ride, targetPath, operations, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAll(sourcePath), original);
        QVERIFY(!QFile::exists(targetPath));
        QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString()),
                 initialHistory);
    }
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size());
    QVERIFY(dirty);
    QVERIFY(stageSawOnlyLiveRide);
    QCOMPARE(stageRuns, writerFailures.size());
    QCOMPARE(finalizeRuns, 0);
    QCOMPARE(savedSignals, 0);

    operations.writerFactory =
        [](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::None));
        };

    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, targetPath, operations, error));
    QCOMPARE(error, QStringLiteral("injected finalize failure"));
    QCOMPARE(readAll(sourcePath), original);
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString()),
             initialHistory);
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size() + 1);
    QVERIFY(dirty);
    QCOMPARE(stageRuns, writerFailures.size() + 1);
    QCOMPARE(finalizeRuns, 1);
    QCOMPARE(savedSignals, 0);

    injectFinalizeFailure = false;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, targetPath, operations, error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!dirty);
    QCOMPARE(stageRuns, writerFailures.size() + 2);
    QCOMPARE(finalizeRuns, 2);
    QCOMPARE(savedSignals, 1);
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size() + 2);
    QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString())
                 .count(QStringLiteral("Changes on ")),
             1);
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(targetPath);
    std::unique_ptr<RideFile> parsed(reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(
        parsed->getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
        writerFailures.size() + 2);
    QCOMPARE(parsed->getTag(QStringLiteral("Change History"), QString()),
             ride.getTag(QStringLiteral("Change History"), QString()));
}

void TestAtomicActivitySave::
candidateSaveTransfersOnlyAfterSuccess()
{
    RideFile original(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile replacement(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideItem current(&original, nullptr);
    current.path = QStringLiteral("/source");
    current.fileName = QStringLiteral("original.json");
    current.setDirty(false);
    RideItem candidate(&replacement, nullptr);

    bool failureSawIsolatedCandidate = false;
    QString error;
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &replacement,
        [&](RideItem *saveItem, QString &saveError) {
            failureSawIsolatedCandidate =
                saveItem == &candidate
                && saveItem->ride(false) == &replacement
                && current.ride(false) == &original;
            saveError = QStringLiteral("injected candidate save failure");
            return false;
        },
        error));
    QVERIFY(failureSawIsolatedCandidate);
    QCOMPARE(error, QStringLiteral("injected candidate save failure"));
    QCOMPARE(current.ride(false), &original);
    QCOMPARE(current.path, QStringLiteral("/source"));
    QCOMPARE(current.fileName, QStringLiteral("original.json"));
    QVERIFY(!current.isDirty());
    QCOMPARE(candidate.ride(false), &replacement);

    bool successSawIsolatedCandidate = false;
    QVERIFY(saveActivityCandidate(
        &current, &candidate, &replacement,
        [&](RideItem *saveItem, QString &) {
            successSawIsolatedCandidate =
                saveItem == &candidate
                && current.ride(false) == &original;
            saveItem->path = QStringLiteral("/committed");
            saveItem->fileName = QStringLiteral("replacement.json");
            return true;
        },
        error));
    QVERIFY(successSawIsolatedCandidate);
    QVERIFY(error.isEmpty());
    QCOMPARE(current.ride(false), &replacement);
    QCOMPARE(current.path, QStringLiteral("/committed"));
    QCOMPARE(current.fileName, QStringLiteral("replacement.json"));
    QVERIFY(!current.isDirty());
    QVERIFY(candidate.ride(false) == nullptr);
}

void TestAtomicActivitySave::
candidateSaveRejectsCurrentDestroyedBySave()
{
    RideFile *original = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile *replacement = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideItem> current = new RideItem(original, nullptr);
    QPointer<RideItem> candidate = new RideItem(replacement, nullptr);
    current->path = QStringLiteral("/activities");
    current->fileName = QStringLiteral("activity.json");

    QString error;
    QVERIFY(!saveActivityCandidate(
        current.data(), candidate.data(), replacement,
        [&](RideItem *, QString &) {
            delete current.data();
            return true;
        },
        error));
    QVERIFY(current.isNull());
    QVERIFY(candidate);
    QVERIFY(!error.isEmpty());

    delete candidate.data();
    delete replacement;
    delete original;
}

void TestAtomicActivitySave::
candidateSaveRejectsCandidateDestroyedBySave()
{
    RideFile *original = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile *replacement = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideItem> current = new RideItem(original, nullptr);
    QPointer<RideItem> candidate = new RideItem(replacement, nullptr);
    current->path = QStringLiteral("/activities");
    current->fileName = QStringLiteral("activity.json");

    QString error;
    QVERIFY(!saveActivityCandidate(
        current.data(), candidate.data(), replacement,
        [&](RideItem *, QString &) {
            delete candidate.data();
            return true;
        },
        error));
    QVERIFY(candidate.isNull());
    QVERIFY(current);
    QVERIFY(!error.isEmpty());

    delete current.data();
    delete replacement;
    delete original;
}

void TestAtomicActivitySave::
candidateSaveRejectsReplacementDestroyedBySave()
{
    RideFile *original = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideFile> replacement = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideItem> current = new RideItem(original, nullptr);
    QPointer<RideItem> candidate =
        new RideItem(replacement.data(), nullptr);
    current->path = QStringLiteral("/activities");
    current->fileName = QStringLiteral("activity.json");

    QString error;
    QVERIFY(!saveActivityCandidate(
        current.data(), candidate.data(), replacement.data(),
        [&](RideItem *, QString &) {
            delete replacement.data();
            return true;
        },
        error));
    QVERIFY(replacement.isNull());
    QVERIFY(current);
    QVERIFY(candidate);
    QVERIFY(!error.isEmpty());

    delete candidate.data();
    delete current.data();
    delete original;
}

void TestAtomicActivitySave::
candidateSaveRejectsCandidateDestroyedBySetDirty()
{
    RideFile *original = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile *replacement = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideItem> current = new RideItem(original, nullptr);
    QPointer<RideItem> candidate = new RideItem(replacement, nullptr);
    current->path = QStringLiteral("/activities");
    current->fileName = QStringLiteral("activity.json");
    setAtomicActivitySaveSetDirtyAction(
        candidate.data(), [&] { delete candidate.data(); });

    int saveCalls = 0;
    QString error;
    QVERIFY(!saveActivityCandidate(
        current.data(), candidate.data(), replacement,
        [&](RideItem *, QString &) {
            ++saveCalls;
            return true;
        },
        error));
    QVERIFY(candidate.isNull());
    QCOMPARE(saveCalls, 0);
    QVERIFY(!error.isEmpty());

    delete current.data();
    delete replacement;
    delete original;
}

void TestAtomicActivitySave::
candidateSaveRejectsCurrentDestroyedBySetRide()
{
    RideFile *original = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile *replacement = new RideFile(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QPointer<RideItem> current = new RideItem(original, nullptr);
    QPointer<RideItem> candidate = new RideItem(replacement, nullptr);
    current->path = QStringLiteral("/activities");
    current->fileName = QStringLiteral("activity.json");
    setAtomicActivitySaveSetRideAction(
        current.data(), [&] { delete current.data(); });

    QString error;
    QVERIFY(!saveActivityCandidate(
        current.data(), candidate.data(), replacement,
        [](RideItem *, QString &) { return true; },
        error));
    QVERIFY(current.isNull());
    QVERIFY(candidate);
    QVERIFY(!error.isEmpty());

    delete candidate.data();
    delete replacement;
    delete original;
}

void TestAtomicActivitySave::
defaultProcessorFailurePreservesFileAndDirtyState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString path = dir.filePath(fileName);
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    resetAtomicActivitySaveProcessorStub();
    setAtomicActivitySaveProcessorFailure(true);

    QString error;
    QVERIFY(!MainWindow::saveSilent(nullptr, &item, &error));
    QCOMPARE(atomicActivitySaveProcessorCalls(), 1);
    QVERIFY(error.contains(QStringLiteral("processor"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, fileName);
    QVERIFY(!ride.tags().contains(QStringLiteral("Change History")));
}

void TestAtomicActivitySave::mainWindowConvertsSourceAfterCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("import.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QString notesPath = dir.filePath(QStringLiteral("import.notes"));
    const QByteArray original("original imported activity");
    writeFixture(sourcePath, original);
    writeFixture(notesPath, QByteArray("notes"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("converted"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool writerCreated = false;
    AtomicFileMode writerMode = AtomicFileMode::ReplaceExisting;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerCreated = true;
            writerMode = mode;
            return realWriter(path, mode);
        };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(writerCreated);
    QCOMPARE(writerMode, AtomicFileMode::CreateNew);
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
    QVERIFY(QFile::exists(targetPath));
    QVERIFY(!QFile::exists(notesPath));
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(targetPath);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("State"), QString()),
             QStringLiteral("converted"));
}

void TestAtomicActivitySave::mainWindowRenamesJsonAfterCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("old-name.json");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("old json activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    AtomicFileMode writerMode = AtomicFileMode::ReplaceExisting;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerMode = mode;
            return realWriter(path, mode);
        };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QCOMPARE(writerMode, AtomicFileMode::CreateNew);
    QVERIFY(!QFile::exists(sourcePath));
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());
}

void TestAtomicActivitySave::linkedFilenameSaveFailureNeverPublishesPrefix()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QDateTime firstOldTime(QDate(2026, 7, 6), QTime(8, 30));
    const QDateTime secondOldTime(QDate(2026, 7, 7), QTime(9, 45));
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName =
        activityFileName(firstOldTime, QStringLiteral("json"));
    const QString secondOldName =
        activityFileName(secondOldTime, QStringLiteral("json"));
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));

    RideFile firstRide(firstOldTime, 1.0);
    RideFile secondRide(secondOldTime, 1.0);
    firstRide.setTag(QStringLiteral("Linked Filename"), secondOldName);
    secondRide.setTag(QStringLiteral("Linked Filename"), firstOldName);

    JsonFileReader json;
    QString error;
    QFile firstOldFile(dir.filePath(firstOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &firstRide, firstOldFile, error),
             qPrintable(error));
    QFile secondOldFile(dir.filePath(secondOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &secondRide, secondOldFile, error),
             qPrintable(error));

    firstRide.setTag(
        QStringLiteral("Change History"),
        QStringLiteral("existing first history"));
    firstRide.setStartTime(firstNewTime);
    secondRide.setStartTime(secondNewTime);
    firstRide.setTag(QStringLiteral("Linked Filename"), secondNewName);
    secondRide.setTag(QStringLiteral("Linked Filename"), firstNewName);

    RideItem firstItem(&firstRide, nullptr);
    RideItem secondItem(&secondRide, nullptr);
    firstItem.path = dir.path();
    firstItem.fileName = firstOldName;
    secondItem.path = dir.path();
    secondItem.fileName = secondOldName;
    secondItem.planned = true;
    firstItem.setLinkedFileName(secondNewName);
    secondItem.setLinkedFileName(firstNewName);
    firstItem.setDirty(true);
    secondItem.setDirty(true);

    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    const ActivitySaveOperationsProvider operationsProvider =
        [&](RideItem *item) {
        ActivitySaveOperations operations;
        operations.writerFactory =
            item == &secondItem
            ? AtomicFileWriterFactory(
                  [](const QString &path, AtomicFileMode) {
                      return std::unique_ptr<AtomicFileWriter>(
                          new FaultInjectingWriter(
                              path, FailurePoint::Commit));
                  })
            : realWriter;
        return operations;
    };

    QVERIFY(!MainWindow::saveLinkedActivitiesTransaction(
        nullptr,
        dir.path(),
        { &firstItem, &secondItem },
        error,
        operationsProvider));

    const bool completeOldPair =
        QFileInfo::exists(dir.filePath(firstOldName))
        && QFileInfo::exists(dir.filePath(secondOldName))
        && !QFileInfo::exists(dir.filePath(firstNewName))
        && !QFileInfo::exists(dir.filePath(secondNewName));
    const bool completeNewPair =
        !QFileInfo::exists(dir.filePath(firstOldName))
        && !QFileInfo::exists(dir.filePath(secondOldName))
        && QFileInfo::exists(dir.filePath(firstNewName))
        && QFileInfo::exists(dir.filePath(secondNewName));
    QVERIFY2(
        completeOldPair || completeNewPair,
        "A linked save must not leave only a successful publication prefix");
    QVERIFY(firstItem.isDirty());
    QVERIFY(secondItem.isDirty());
    QCOMPARE(
        firstRide.getTag(QStringLiteral("Change History"), QString()),
        QStringLiteral("existing first history"));
    QVERIFY(!secondRide.tags().contains(QStringLiteral("Change History")));
}

void TestAtomicActivitySave::linkedFilenameSavePublishesCompletePair()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QDateTime firstOldTime(QDate(2026, 7, 6), QTime(8, 30));
    const QDateTime secondOldTime(QDate(2026, 7, 7), QTime(9, 45));
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName =
        activityFileName(firstOldTime, QStringLiteral("json"));
    const QString secondOldName =
        activityFileName(secondOldTime, QStringLiteral("json"));
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));

    RideFile firstRide(firstOldTime, 1.0);
    RideFile secondRide(secondOldTime, 1.0);
    firstRide.setTag(QStringLiteral("Linked Filename"), secondOldName);
    secondRide.setTag(QStringLiteral("Linked Filename"), firstOldName);
    JsonFileReader json;
    QString error;
    QFile firstOldFile(dir.filePath(firstOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &firstRide, firstOldFile, error),
             qPrintable(error));
    QFile secondOldFile(dir.filePath(secondOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &secondRide, secondOldFile, error),
             qPrintable(error));

    firstRide.setStartTime(firstNewTime);
    secondRide.setStartTime(secondNewTime);
    RideItem firstItem(&firstRide, nullptr);
    RideItem secondItem(&secondRide, nullptr);
    firstItem.path = dir.path();
    firstItem.fileName = firstOldName;
    secondItem.path = dir.path();
    secondItem.fileName = secondOldName;
    secondItem.planned = true;
    firstItem.setLinkedFileName(secondNewName);
    secondItem.setLinkedFileName(firstNewName);
    firstItem.setDirty(true);
    secondItem.setDirty(true);

    QVERIFY2(MainWindow::saveLinkedActivitiesTransaction(
                 nullptr,
                 dir.path(),
                 { &firstItem, &secondItem },
                 error,
                 ActivitySaveOperationsProvider()),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(firstOldName)));
    QVERIFY(!QFileInfo::exists(dir.filePath(secondOldName)));
    QVERIFY(QFileInfo::exists(dir.filePath(firstNewName)));
    QVERIFY(QFileInfo::exists(dir.filePath(secondNewName)));
    QCOMPARE(firstItem.fileName, firstNewName);
    QCOMPARE(secondItem.fileName, secondNewName);
    QVERIFY(!firstItem.isDirty());
    QVERIFY(!secondItem.isDirty());

    const auto linkedFilename = [&](const QString &path) {
        QStringList parseErrors;
        QFile input(path);
        std::unique_ptr<RideFile> parsed(
            json.openRideFile(input, parseErrors));
        if (!parsed) return QString();
        return parsed->getTag(
            QStringLiteral("Linked Filename"), QString());
    };
    QCOMPARE(
        linkedFilename(dir.filePath(firstNewName)), secondNewName);
    QCOMPARE(
        linkedFilename(dir.filePath(secondNewName)), firstNewName);

    const QDir journalRoot(
        dir.filePath(QStringLiteral(".gc-transactions/linked-save")));
    QVERIFY(!journalRoot.exists()
        || journalRoot.entryList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::
linkedConversionPublishesCompletePairAndBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString importedName = QStringLiteral("import.fit");
    const QByteArray importedBytes("original imported activity");
    const QByteArray previousBackup("previous imported backup");
    const QString importedPath = dir.filePath(importedName);
    writeFixture(importedPath, importedBytes);
    writeFixture(
        importedPath + QStringLiteral(".bak"), previousBackup);

    const QDateTime completedTime(QDate(2026, 7, 10), QTime(6, 30));
    const QDateTime plannedOldTime(QDate(2026, 7, 11), QTime(7, 45));
    const QDateTime plannedNewTime(QDate(2026, 7, 12), QTime(8, 15));
    const QString completedNewName =
        activityFileName(completedTime, QStringLiteral("json"));
    const QString plannedOldName =
        activityFileName(plannedOldTime, QStringLiteral("json"));
    const QString plannedNewName =
        activityFileName(plannedNewTime, QStringLiteral("json"));

    RideFile completedRide(completedTime, 1.0);
    RideFile plannedRide(plannedOldTime, 1.0);
    plannedRide.setTag(QStringLiteral("Linked Filename"), importedName);
    JsonFileReader json;
    QString error;
    QFile plannedOldFile(dir.filePath(plannedOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &plannedRide, plannedOldFile, error),
             qPrintable(error));
    plannedRide.setStartTime(plannedNewTime);

    RideItem completedItem(&completedRide, nullptr);
    RideItem plannedItem(&plannedRide, nullptr);
    completedItem.path = dir.path();
    completedItem.fileName = importedName;
    plannedItem.path = dir.path();
    plannedItem.fileName = plannedOldName;
    plannedItem.planned = true;
    completedItem.setLinkedFileName(plannedNewName);
    plannedItem.setLinkedFileName(completedNewName);
    completedItem.setDirty(true);
    plannedItem.setDirty(true);

    QVERIFY2(MainWindow::saveLinkedActivitiesTransaction(
                 nullptr,
                 dir.path(),
                 { &completedItem, &plannedItem },
                 error,
                 ActivitySaveOperationsProvider()),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(importedPath));
    QCOMPARE(
        readAll(importedPath + QStringLiteral(".bak")), importedBytes);
    QVERIFY(QFileInfo::exists(dir.filePath(completedNewName)));
    QVERIFY(!QFileInfo::exists(dir.filePath(plannedOldName)));
    QVERIFY(QFileInfo::exists(dir.filePath(plannedNewName)));

    const auto linkedFilename = [&](const QString &path) {
        QStringList parseErrors;
        QFile input(path);
        std::unique_ptr<RideFile> parsed(
            json.openRideFile(input, parseErrors));
        if (!parsed) return QString();
        return parsed->getTag(
            QStringLiteral("Linked Filename"), QString());
    };
    QCOMPARE(
        linkedFilename(dir.filePath(completedNewName)), plannedNewName);
    QCOMPARE(
        linkedFilename(dir.filePath(plannedNewName)), completedNewName);
}

void TestAtomicActivitySave::linkedBatchProcessesAllBeforeSerializing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName = QStringLiteral("first-old.json");
    const QString secondOldName = QStringLiteral("second-old.json");
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));
    writeFixture(dir.filePath(firstOldName), QByteArray("first old"));
    writeFixture(dir.filePath(secondOldName), QByteArray("second old"));

    RideFile firstRide(firstNewTime, 1.0);
    RideFile secondRide(secondNewTime, 1.0);
    RideItem firstItem(&firstRide, nullptr);
    RideItem secondItem(&secondRide, nullptr);
    firstItem.path = dir.path();
    firstItem.fileName = firstOldName;
    secondItem.path = dir.path();
    secondItem.fileName = secondOldName;
    secondItem.planned = true;
    firstItem.setLinkedFileName(secondNewName);
    secondItem.setLinkedFileName(firstNewName);
    firstItem.setDirty(true);
    secondItem.setDirty(true);

    QHash<RideItem *, int> stageCalls;
    const ActivitySaveOperationsProvider provider =
        [&](RideItem *item) {
            ActivitySaveOperations operations;
            operations.writerFactory = qSaveFileWriterFactory();
            operations.stage = [&, item](RideFile *, QString &) {
                ++stageCalls[item];
                if (item == &secondItem) {
                    firstRide.setTag(
                        QStringLiteral("Peer Processor"),
                        QStringLiteral("included"));
                }
                return true;
            };
            return operations;
        };
    QString error;
    QVERIFY2(MainWindow::saveLinkedActivitiesTransaction(
                 nullptr,
                 dir.path(),
                 { &firstItem, &secondItem },
                 error,
                 provider),
             qPrintable(error));
    QCOMPARE(stageCalls.value(&firstItem), 1);
    QCOMPARE(stageCalls.value(&secondItem), 1);

    JsonFileReader json;
    QStringList parseErrors;
    QFile input(dir.filePath(firstNewName));
    std::unique_ptr<RideFile> parsed(
        json.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(
        parsed->getTag(QStringLiteral("Peer Processor"), QString()),
        QStringLiteral("included"));
}

void TestAtomicActivitySave::
linkedBatchProcessorFailureLeavesCompleteOldGeneration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName = QStringLiteral("first-old.json");
    const QString secondOldName = QStringLiteral("second-old.json");
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));
    const QByteArray firstOld("first old generation");
    const QByteArray secondOld("second old generation");
    writeFixture(dir.filePath(firstOldName), firstOld);
    writeFixture(dir.filePath(secondOldName), secondOld);

    RideFile firstRide(firstNewTime, 1.0);
    RideFile secondRide(secondNewTime, 1.0);
    firstRide.setTag(
        QStringLiteral("Change History"),
        QStringLiteral("existing first history"));
    RideItem firstItem(&firstRide, nullptr);
    RideItem secondItem(&secondRide, nullptr);
    firstItem.path = dir.path();
    firstItem.fileName = firstOldName;
    secondItem.path = dir.path();
    secondItem.fileName = secondOldName;
    secondItem.planned = true;
    firstItem.setLinkedFileName(secondNewName);
    secondItem.setLinkedFileName(firstNewName);
    firstItem.setDirty(true);
    secondItem.setDirty(true);

    QHash<RideItem *, int> stageCalls;
    const ActivitySaveOperationsProvider provider =
        [&](RideItem *item) {
            ActivitySaveOperations operations;
            operations.writerFactory = qSaveFileWriterFactory();
            operations.stage = [&, item](RideFile *, QString &stageError) {
                ++stageCalls[item];
                if (item == &secondItem) {
                    stageError = QStringLiteral("injected peer processor failure");
                    return false;
                }
                return true;
            };
            return operations;
        };
    QString error;
    QVERIFY(!MainWindow::saveLinkedActivitiesTransaction(
        nullptr,
        dir.path(),
        {&firstItem, &secondItem},
        error,
        provider));
    QVERIFY2(
        error.contains(QStringLiteral("processor"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(stageCalls.value(&firstItem), 1);
    QCOMPARE(stageCalls.value(&secondItem), 1);
    QCOMPARE(readAll(dir.filePath(firstOldName)), firstOld);
    QCOMPARE(readAll(dir.filePath(secondOldName)), secondOld);
    QVERIFY(!QFileInfo::exists(dir.filePath(firstNewName)));
    QVERIFY(!QFileInfo::exists(dir.filePath(secondNewName)));
    QVERIFY(firstItem.isDirty());
    QVERIFY(secondItem.isDirty());
    QCOMPARE(
        firstRide.getTag(QStringLiteral("Change History"), QString()),
        QStringLiteral("existing first history"));
    QVERIFY(!secondRide.tags().contains(QStringLiteral("Change History")));
}

void TestAtomicActivitySave::
linkedBatchStopsWhenPeerDestroyedDuringProcessing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName = QStringLiteral("first-old.json");
    const QString secondOldName = QStringLiteral("second-old.json");
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));
    const QByteArray firstOld("first old generation");
    const QByteArray secondOld("second old generation");
    writeFixture(dir.filePath(firstOldName), firstOld);
    writeFixture(dir.filePath(secondOldName), secondOld);

    RideFile firstRide(firstNewTime, 1.0);
    RideFile secondRide(secondNewTime, 1.0);
    RideItem firstItem(&firstRide, nullptr);
    QPointer<RideItem> secondItem(new RideItem(&secondRide, nullptr));
    firstItem.path = dir.path();
    firstItem.fileName = firstOldName;
    secondItem->path = dir.path();
    secondItem->fileName = secondOldName;
    secondItem->planned = true;
    firstItem.setLinkedFileName(secondNewName);
    secondItem->setLinkedFileName(firstNewName);
    firstItem.setDirty(true);
    secondItem->setDirty(true);
    RideItem *const secondRaw = secondItem.data();

    int stageCalls = 0;
    const ActivitySaveOperationsProvider provider =
        [&](RideItem *item) {
            ActivitySaveOperations operations;
            operations.writerFactory = qSaveFileWriterFactory();
            operations.stage = [&, item](RideFile *, QString &) {
                ++stageCalls;
                if (item == &firstItem) delete secondRaw;
                return true;
            };
            return operations;
        };
    QString error;
    QVERIFY(!MainWindow::saveLinkedActivitiesTransaction(
        nullptr,
        dir.path(),
        {&firstItem, secondRaw},
        error,
        provider));
    QVERIFY(secondItem.isNull());
    QCOMPARE(stageCalls, 1);
    QVERIFY2(
        error.contains(QStringLiteral("changed"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(readAll(dir.filePath(firstOldName)), firstOld);
    QCOMPARE(readAll(dir.filePath(secondOldName)), secondOld);
    QVERIFY(!QFileInfo::exists(dir.filePath(firstNewName)));
    QVERIFY(!QFileInfo::exists(dir.filePath(secondNewName)));
    QVERIFY(firstItem.isDirty());
}

void TestAtomicActivitySave::
linkedSaveRequirementRejectsUnpreparedRenames()
{
    const QDateTime firstOldTime(QDate(2026, 7, 6), QTime(8, 30));
    const QDateTime secondOldTime(QDate(2026, 7, 7), QTime(9, 45));
    RideFile firstRide(QDateTime(QDate(2026, 7, 8), QTime(10, 15)), 1.0);
    RideFile secondRide(QDateTime(QDate(2026, 7, 9), QTime(11, 20)), 1.0);
    RideItem firstItem(&firstRide, nullptr);
    RideItem secondItem(&secondRide, nullptr);
    firstItem.path = QStringLiteral("/tmp");
    firstItem.fileName =
        activityFileName(firstOldTime, QStringLiteral("json"));
    secondItem.path = QStringLiteral("/tmp");
    secondItem.fileName =
        activityFileName(secondOldTime, QStringLiteral("json"));
    secondItem.planned = true;
    firstItem.setLinkedFileName(secondItem.fileName);
    secondItem.setLinkedFileName(firstItem.fileName);
    firstItem.setDirty(true);
    secondItem.setDirty(true);

    QString error;
    QCOMPARE(
        linkedActivitySaveRequirement(
            { &firstItem, &secondItem }, error),
        LinkedActivitySaveRequirement::Invalid);
    QVERIFY2(
        error.contains(QStringLiteral("not updated"), Qt::CaseInsensitive),
        qPrintable(error));
}

void TestAtomicActivitySave::
pendingLinkedRemovalJournalBlocksLinkedSaveTransaction()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstSource = dir.filePath(QStringLiteral("first.json"));
    const QString secondSource = dir.filePath(QStringLiteral("second.json"));
    writeFixture(firstSource, QByteArray("first"));
    writeFixture(secondSource, QByteArray("second"));
    const QString pending = dir.filePath(
        QStringLiteral(".gc-transactions/linked-removal/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    QVERIFY(QDir().mkpath(pending));

    LinkedActivitySave::Specification specification;
    specification.athleteRoot = dir.path();
    specification.entries = {
        {firstSource,
         dir.filePath(QStringLiteral("first-new.json")),
         firstSource + QStringLiteral(".bak"),
         false},
        {secondSource,
         dir.filePath(QStringLiteral("second-new.json")),
         secondSource + QStringLiteral(".bak"),
         false}};
    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(!journal);
    QVERIFY2(
        error.contains(QStringLiteral("recovery"), Qt::CaseInsensitive),
        qPrintable(error));
}

void TestAtomicActivitySave::
concurrentLinkedSaveJournalsAreSerialized()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> first =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(first, qPrintable(error));

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> concurrent =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(!concurrent);
    QVERIFY2(
        error.contains(QStringLiteral("active"), Qt::CaseInsensitive),
        qPrintable(error));

    error.clear();
    QVERIFY2(first->cleanupAfterRollback(error), qPrintable(error));
    first.reset();

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> next =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(next, qPrintable(error));
    QVERIFY2(next->cleanupAfterRollback(error), qPrintable(error));
}

void TestAtomicActivitySave::
abandonedLinkedSaveJournalBlocksNextTransaction()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> abandoned =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(abandoned, qPrintable(error));
    const QString journalPath = abandoned->directoryPath();
    abandoned.reset();
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> blocked =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(!blocked);
    QVERIFY2(
        error.contains(QStringLiteral("recovery"), Qt::CaseInsensitive),
        qPrintable(error));

    error.clear();
    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> next =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(next, qPrintable(error));
    QVERIFY2(next->cleanupAfterRollback(error), qPrintable(error));
}

void TestAtomicActivitySave::
linkedSaveQLockFileRemovalGuardDoesNotPoisonReconcile_data()
{
    QTest::addColumn<bool>("journalEntry");
    QTest::addColumn<QString>("lockName");
    QTest::addColumn<int>("suffixCount");
    QTest::addColumn<bool>("removeManifest");
    const QString namespaceLock =
        QStringLiteral(".01234567-89ab-cdef-8123-456789abcdef.lock");
    QTest::newRow("namespace-single-rmlock")
        << false << namespaceLock << 1 << false;
    QTest::newRow("namespace-nested-rmlock")
        << false << namespaceLock << 2 << false;
    QTest::newRow("journal-manifest-single-rmlock")
        << true << QStringLiteral(".manifest.json.lock") << 1 << false;
    QTest::newRow("journal-manifest-nested-rmlock")
        << true << QStringLiteral(".manifest.json.lock") << 2 << false;
    QTest::newRow("journal-stage-rmlock")
        << true << QStringLiteral(".new-0000.stage.lock") << 1 << false;
    QTest::newRow("pre-manifest-stage-rmlock")
        << true << QStringLiteral(".new-0000.stage.lock") << 1 << true;
}

void TestAtomicActivitySave::
linkedSaveQLockFileRemovalGuardDoesNotPoisonReconcile()
{
    QFETCH(bool, journalEntry);
    QFETCH(QString, lockName);
    QFETCH(int, suffixCount);
    QFETCH(bool, removeManifest);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());

    QString journalPath;
    QString guardPath;
    QString error;
    if (journalEntry) {
        std::shared_ptr<LinkedActivitySave::Journal> journal =
            LinkedActivitySave::Journal::prepare(
                linkedSaveJournalSpecification(dir.path()), error);
        QVERIFY2(journal, qPrintable(error));
        journalPath = journal->directoryPath();
        guardPath = QDir(journalPath).filePath(
            qlockRemovalGuardName(lockName, suffixCount));
        journal.reset();
        if (removeManifest) {
            QVERIFY(QFile::remove(
                QDir(journalPath).filePath(
                    QStringLiteral("manifest.json"))));
        }
    } else {
        const QString namespacePath = QDir(dir.path()).filePath(
            QStringLiteral(".gc-transactions/linked-save"));
        QVERIFY(QDir().mkpath(namespacePath));
        guardPath = QDir(namespacePath).filePath(
            qlockRemovalGuardName(lockName, suffixCount));
    }
    writeFixture(
        guardPath, QByteArray("stale QLockFile removal guard"));

    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        qPrintable(error));
    if (journalEntry) QVERIFY(!QFileInfo::exists(journalPath));
    error.clear();
    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        qPrintable(error));

    const std::shared_ptr<LinkedActivitySave::Journal> next =
        LinkedActivitySave::Journal::prepare(
            linkedSaveJournalSpecification(dir.path()), error);
    QVERIFY2(next, qPrintable(error));
    QVERIFY2(next->cleanupAfterRollback(error), qPrintable(error));
}

void TestAtomicActivitySave::
linkedSaveUnsafeQLockFileRemovalGuardsRemainRejected_data()
{
    QTest::addColumn<QString>("entryName");
    QTest::addColumn<int>("entryKind");
    QTest::newRow("invalid-uuid")
        << QStringLiteral(".not-a-uuid.lock.rmlock")
        << int(LockArtifactEntryKind::RegularFile);
    QTest::newRow("suffix-lookalike")
        << qlockRemovalGuardName() + QStringLiteral(".tmp")
        << int(LockArtifactEntryKind::RegularFile);
    QTest::newRow("symbolic-link")
        << qlockRemovalGuardName()
        << int(LockArtifactEntryKind::SymbolicLink);
    QTest::newRow("directory")
        << qlockRemovalGuardName()
        << int(LockArtifactEntryKind::Directory);
}

void TestAtomicActivitySave::
linkedSaveUnsafeQLockFileRemovalGuardsRemainRejected()
{
    QFETCH(QString, entryName);
    QFETCH(int, entryKind);
    const LockArtifactEntryKind kind =
        LockArtifactEntryKind(entryKind);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString namespacePath = QDir(dir.path()).filePath(
        QStringLiteral(".gc-transactions/linked-save"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QString entryPath = QDir(namespacePath).filePath(entryName);
    if (kind == LockArtifactEntryKind::Directory) {
        QVERIFY(QDir().mkpath(entryPath));
    } else if (kind == LockArtifactEntryKind::SymbolicLink) {
#ifdef Q_OS_WIN
        QSKIP("QFile::link creates shortcuts rather than symlinks on Windows");
#else
        const QString targetPath = QDir(dir.path()).filePath(
            QStringLiteral("qlock-removal-guard-target"));
        writeFixture(targetPath, QByteArray("target"));
        QVERIFY(QFile::link(targetPath, entryPath));
#endif
    } else {
        writeFixture(entryPath, QByteArray("lookalike"));
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        QString error;
        QVERIFY(!LinkedActivitySave::Journal::reconcileAll(
            dir.path(), error));
        QVERIFY2(error.contains(entryName), qPrintable(error));
    }
}

void TestAtomicActivitySave::
linkedSaveJournalLocksCompleteProductionPathSet_data()
{
    QTest::addColumn<int>("entryIndex");
    QTest::addColumn<QString>("role");

    for (int index = 0; index < 2; ++index) {
        for (const QString &role : {
                 QStringLiteral("source"),
                 QStringLiteral("target"),
                 QStringLiteral("backup")}) {
            QTest::newRow(
                qPrintable(QStringLiteral("entry-%1-%2")
                               .arg(index + 1)
                               .arg(role)))
                << index << role;
        }
    }
}

void TestAtomicActivitySave::
linkedSaveJournalLocksCompleteProductionPathSet()
{
    QFETCH(int, entryIndex);
    QFETCH(QString, role);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const LinkedActivitySave::EntrySpecification &entry =
        specification.entries.at(entryIndex);
    const QString path = role == QStringLiteral("source")
        ? entry.sourcePath
        : role == QStringLiteral("target")
            ? entry.targetPath
            : entry.backupPath;

    AtomicFileLockSet competingLock;
    QString error;
    QVERIFY2(competingLock.lock({path}, error), qPrintable(error));

    error.clear();
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(!journal);
    QVERIFY2(
        error.contains(QStringLiteral("already"), Qt::CaseInsensitive),
        qPrintable(error));
}

void TestAtomicActivitySave::
linkedSaveJournalRejectsUnsafePathGraphs_data()
{
    QTest::addColumn<QString>("pathCase");

    QTest::newRow("duplicate-source")
        << QStringLiteral("duplicate-source");
    QTest::newRow("duplicate-target")
        << QStringLiteral("duplicate-target");
    QTest::newRow("duplicate-backup")
        << QStringLiteral("duplicate-backup");
    QTest::newRow("cross-target-source")
        << QStringLiteral("cross-target-source");
    QTest::newRow("cross-backup-source")
        << QStringLiteral("cross-backup-source");
    QTest::newRow("same-entry-backup-source")
        << QStringLiteral("same-entry-backup-source");
    QTest::newRow("cross-backup-target")
        << QStringLiteral("cross-backup-target");
    QTest::newRow("existing-target")
        << QStringLiteral("existing-target");
    QTest::newRow("outside-root")
        << QStringLiteral("outside-root");
    QTest::newRow("transaction-namespace")
        << QStringLiteral("transaction-namespace");
    QTest::newRow("symlink-source")
        << QStringLiteral("symlink-source");
    QTest::newRow("symlink-parent")
        << QStringLiteral("symlink-parent");
}

void TestAtomicActivitySave::
linkedSaveJournalRejectsUnsafePathGraphs()
{
    QFETCH(QString, pathCase);

    QTemporaryDir dir;
    QTemporaryDir outside;
    QVERIFY(dir.isValid());
    QVERIFY(outside.isValid());
    writeLinkedSaveJournalSources(dir.path());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    if (pathCase == QStringLiteral("duplicate-source")) {
        specification.entries[1].sourcePath =
            specification.entries.at(0).sourcePath;
    } else if (pathCase == QStringLiteral("duplicate-target")) {
        specification.entries[1].targetPath =
            specification.entries.at(0).targetPath;
    } else if (pathCase == QStringLiteral("duplicate-backup")) {
        specification.entries[1].backupPath =
            specification.entries.at(0).backupPath;
    } else if (pathCase == QStringLiteral("cross-target-source")) {
        specification.entries[0].targetPath =
            specification.entries.at(1).sourcePath;
    } else if (pathCase == QStringLiteral("cross-backup-source")) {
        specification.entries[1].backupPath =
            specification.entries.at(0).sourcePath;
    } else if (pathCase == QStringLiteral("same-entry-backup-source")) {
        specification.entries[0].backupPath =
            specification.entries.at(0).sourcePath;
    } else if (pathCase == QStringLiteral("cross-backup-target")) {
        specification.entries[1].backupPath =
            specification.entries.at(0).targetPath;
    } else if (pathCase == QStringLiteral("existing-target")) {
        writeFixture(
            specification.entries.at(0).targetPath,
            QByteArray("unrelated target"));
    } else if (pathCase == QStringLiteral("outside-root")) {
        const QString outsideSource = outside.filePath(
            QStringLiteral("outside.json"));
        writeFixture(outsideSource, QByteArray("outside"));
        specification.entries[0].sourcePath = outsideSource;
    } else if (pathCase == QStringLiteral("transaction-namespace")) {
        const QString transactionSource = dir.filePath(
            QStringLiteral(".gc-transactions/attacker/source.json"));
        QVERIFY(QDir().mkpath(QFileInfo(transactionSource).absolutePath()));
        writeFixture(transactionSource, QByteArray("transaction data"));
        specification.entries[0].sourcePath = transactionSource;
    } else if (pathCase == QStringLiteral("symlink-source")) {
        const QString realSource = dir.filePath(
            QStringLiteral("real-source.json"));
        const QString linkedSource = dir.filePath(
            QStringLiteral("linked-source.json"));
        writeFixture(realSource, QByteArray("real source"));
        if (!QFile::link(realSource, linkedSource)) {
            QSKIP("Symbolic links are unavailable");
        }
        specification.entries[0].sourcePath = linkedSource;
    } else if (pathCase == QStringLiteral("symlink-parent")) {
        const QString realParent = dir.filePath(QStringLiteral("real-parent"));
        const QString linkedParent = dir.filePath(
            QStringLiteral("linked-parent"));
        QVERIFY(QDir().mkpath(realParent));
        writeFixture(
            QDir(realParent).filePath(QStringLiteral("source.json")),
            QByteArray("source under linked parent"));
        if (!QFile::link(realParent, linkedParent)) {
            QSKIP("Directory symbolic links are unavailable");
        }
        specification.entries[0].sourcePath =
            QDir(linkedParent).filePath(QStringLiteral("source.json"));
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(journal == nullptr);
    QVERIFY2(!error.isEmpty(), qPrintable(pathCase));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("first-old.json"))),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("second-old.json"))),
        QByteArray("second old generation"));

    const QDir journalRoot(dir.filePath(
        QStringLiteral(".gc-transactions/linked-save")));
    QVERIFY(!journalRoot.exists()
        || journalRoot.entryList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::
linkedSaveRejectsHardLinkedRetirementSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QString aliasPath =
        specification.entries.at(0).sourcePath
        + QStringLiteral(".hardlink");
    if (!createHardLink(
            specification.entries.at(0).sourcePath, aliasPath)) {
        QSKIP("Hard links are unavailable");
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY(!journal);
    QVERIFY2(
        error.contains(QStringLiteral("one link"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(readAll(aliasPath), QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
}

void TestAtomicActivitySave::
linkedSaveRetirementSkipsInPlaceEntry_data()
{
    QTest::addColumn<bool>("allInPlace");

    QTest::newRow("mixed") << false;
    QTest::newRow("all-in-place") << true;
}

void TestAtomicActivitySave::
linkedSaveRetirementSkipsInPlaceEntry()
{
    QFETCH(bool, allInPlace);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    specification.entries[0].targetPath =
        specification.entries.at(0).sourcePath;
    if (allInPlace) {
        specification.entries[1].targetPath =
            specification.entries.at(1).sourcePath;
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged in-place generation");
    const QByteArray secondStaged("staged renamed generation");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), firstStaged);
    if (allInPlace) {
        QCOMPARE(
            readAll(specification.entries.at(1).sourcePath), secondStaged);
    } else {
        QVERIFY(!QFileInfo::exists(
            specification.entries.at(1).sourcePath));
    }
    QCOMPARE(
        readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveCleanupReleasesTransactionResources_data()
{
    QTest::addColumn<bool>("committed");

    QTest::newRow("rollback") << false;
    QTest::newRow("commit") << true;
}

void TestAtomicActivitySave::
linkedSaveCleanupReleasesTransactionResources()
{
    QFETCH(bool, committed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString athleteRoot =
        dir.filePath(QStringLiteral("athlete"));
    const QString movedRoot =
        dir.filePath(QStringLiteral("moved-athlete"));
    QVERIFY(QDir().mkpath(athleteRoot));
    writeLinkedSaveJournalSources(athleteRoot);
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(athleteRoot);

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    if (committed) {
        for (int index = 0; index < journal->entryCount(); ++index) {
            writeFixture(
                journal->stagingPath(index),
                QByteArray("staged generation ")
                    + QByteArray::number(index));
            QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
        }
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
        QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));
    } else {
        QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    }
    QVERIFY(!journal->hasCommitMarker());

    QVERIFY2(
        QDir().rename(athleteRoot, movedRoot),
        "A completed journal retained an athlete-directory resource");
    QVERIFY(QFileInfo::exists(movedRoot));
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalRemovalStaysRetryable_data()
{
    QTest::addColumn<bool>("committed");

    QTest::newRow("rollback") << false;
    QTest::newRow("commit") << true;
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalRemovalStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QFETCH(bool, committed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    if (committed) {
        for (int index = 0; index < journal->entryCount(); ++index) {
            writeFixture(
                journal->stagingPath(index),
                QByteArray("staged generation ")
                    + QByteArray::number(index));
            QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
        }
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    }

    bool removalCompleted = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-directory-removed"),
        [&]() { removalCompleted = true; });
    anchoredFilesystemSyncFailurePath = QFileInfo(
        journalPath).absolutePath();

    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY2(!firstCleanup, "A nondurable journal removal was accepted");
    QVERIFY2(!error.isEmpty(), "Nondurable removal must report an error");
    QVERIFY(!removalCompleted);
    QVERIFY(!QFileInfo::exists(journalPath));

    anchoredFilesystemSyncFailurePath.clear();
    error.clear();
    const bool secondCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY2(secondCleanup, qPrintable(error));
    QVERIFY(removalCompleted);
    error.clear();
    QVERIFY2(
        committed
            ? journal->cleanupAfterCommit(error)
            : journal->cleanupAfterRollback(error),
        qPrintable(error));
#endif
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalRemovalRejectsReplacement_data()
{
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("replaceNamespace");

    QTest::newRow("rollback-journal") << false << false;
    QTest::newRow("commit-journal") << true << false;
    QTest::newRow("rollback-namespace") << false << true;
    QTest::newRow("commit-namespace") << true << true;
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalRemovalRejectsReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QFETCH(bool, committed);
    QFETCH(bool, replaceNamespace);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString namespacePath = QFileInfo(journalPath).absolutePath();
    if (committed) {
        for (int index = 0; index < journal->entryCount(); ++index) {
            writeFixture(
                journal->stagingPath(index),
                QByteArray("staged generation ")
                    + QByteArray::number(index));
            QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
        }
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    }

    anchoredFilesystemSyncFailurePath = namespacePath;
    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    QVERIFY2(!firstCleanup, "A nondurable journal removal was accepted");
    QVERIFY2(!error.isEmpty(), "Nondurable removal must report an error");
    QVERIFY(!QFileInfo::exists(journalPath));
    anchoredFilesystemSyncFailurePath.clear();

    const QString movedNamespace = dir.filePath(
        QStringLiteral("retained-linked-save-namespace"));
    if (replaceNamespace) {
        QVERIFY(QDir().rename(namespacePath, movedNamespace));
    }
    QVERIFY(QDir().mkpath(journalPath));
    const QString sentinelPath = QDir(journalPath).filePath(
        QStringLiteral("replacement-sentinel"));
    const QByteArray sentinelContents("replacement journal contents");
    writeFixture(sentinelPath, sentinelContents);

    error.clear();
    const bool secondCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY2(
        !secondCleanup,
        "A nondurable removal retry accepted a replacement journal");
    QVERIFY2(!error.isEmpty(), "Rejected cleanup must report an error");
    QCOMPARE(readAll(sentinelPath), sentinelContents);
    if (replaceNamespace) {
        QVERIFY(QFileInfo::exists(movedNamespace));
    }
#endif
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalFileRemovalStaysRetryable_data()
{
    QTest::addColumn<bool>("committed");
    QTest::addColumn<QByteArray>("failureTransition");
    QTest::addColumn<QString>("removedName");

    QTest::newRow("rollback-manifest")
        << false
        << QByteArray("linked-save-journal-files-pinned")
        << QStringLiteral("manifest.json");
    QTest::newRow("commit-manifest")
        << true
        << QByteArray("linked-save-journal-files-pinned")
        << QStringLiteral("manifest.json");
    QTest::newRow("rollback-source-copy")
        << false
        << QByteArray("linked-save-manifest-removed")
        << QStringLiteral("source-0000.old");
    QTest::newRow("commit-source-copy")
        << true
        << QByteArray("linked-save-manifest-removed")
        << QStringLiteral("source-0000.old");
    QTest::newRow("rollback-staging")
        << false
        << QByteArray("linked-save-cleanup-file")
        << QStringLiteral("new-0000.stage");
    QTest::newRow("commit-staging")
        << true
        << QByteArray("linked-save-cleanup-file")
        << QStringLiteral("new-0000.stage");
}

void TestAtomicActivitySave::
linkedSaveNondurableJournalFileRemovalStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QFETCH(bool, committed);
    QFETCH(QByteArray, failureTransition);
    QFETCH(QString, removedName);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }
    if (committed) {
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    }

    bool failureArmed = false;
    setLinkedActivitySaveTransitionAction(
        failureTransition,
        [&]() {
            failureArmed = true;
            anchoredFilesystemSyncFailurePath = journalPath;
        });
    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(failureArmed);
    QVERIFY2(!firstCleanup, "A nondurable journal file removal was accepted");
    QVERIFY2(!error.isEmpty(), "Nondurable removal must report an error");
    QVERIFY(!QFileInfo::exists(manifestPath));
    QVERIFY(!QFileInfo::exists(QDir(journalPath).filePath(removedName)));
    QVERIFY(QFileInfo::exists(journalPath));

    anchoredFilesystemSyncFailurePath.clear();
    error.clear();
    const bool secondCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);

    QVERIFY2(secondCleanup, qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
#endif
}

void TestAtomicActivitySave::
linkedSaveTrackedJournalReplacementStopsRetry_data()
{
    QTest::addColumn<bool>("afterScan");

    QTest::newRow("before-scan") << false;
    QTest::newRow("after-scan") << true;
}

void TestAtomicActivitySave::
linkedSaveTrackedJournalReplacementStopsRetry()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QFETCH(bool, afterScan);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-manifest-removed"),
        [&]() { anchoredFilesystemSyncFailurePath = journalPath; });
    error.clear();
    QVERIFY(!journal->cleanupAfterRollback(error));
    clearLinkedActivitySaveTransitionAction();
    QVERIFY2(!error.isEmpty(), "Nondurable removal must report an error");

    const QString sourceCopyPath = QDir(journalPath).filePath(
        QStringLiteral("source-0000.old"));
    const QString stagingPath = QDir(journalPath).filePath(
        QStringLiteral("new-0000.stage"));
    QVERIFY(!QFileInfo::exists(sourceCopyPath));
    QVERIFY(QFileInfo::exists(stagingPath));

    anchoredFilesystemSyncFailurePath.clear();
    const QByteArray replacementContents("first old generation");
    bool replacementWritten = false;
    const auto writeReplacement = [&]() {
        writeFixture(sourceCopyPath, replacementContents);
        replacementWritten = QFileInfo::exists(sourceCopyPath);
    };
    if (afterScan) {
        setLinkedActivitySaveTransitionAction(
            QByteArray("linked-save-journal-files-pinned"),
            writeReplacement);
    } else {
        writeReplacement();
    }

    error.clear();
    QVERIFY2(
        !journal->cleanupAfterRollback(error),
        "Cleanup accepted a replacement for a tracked removed file");
    clearLinkedActivitySaveTransitionAction();
    QVERIFY(replacementWritten);
    QVERIFY2(!error.isEmpty(), "Rejected cleanup must report an error");
    QVERIFY(QFileInfo::exists(stagingPath));

    AnchoredFileSystem::DirectoryAnchor parent;
    QVERIFY2(
        AnchoredFileSystem::DirectoryAnchor::open(
            journalPath, parent, error),
        qPrintable(error));
    const AnchoredFileSystem::EntryRef replacementEntry =
        parent.entry(QStringLiteral("source-0000.old"), error);
    QVERIFY2(replacementEntry.isValid(), qPrintable(error));
    AnchoredFileSystem::PinnedFile replacement;
    QVERIFY2(
        AnchoredFileSystem::pinRegularFile(
            replacementEntry, replacement, error),
        qPrintable(error));
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            replacementEntry, replacement, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY(stillMatches);
#endif
}

void TestAtomicActivitySave::
linkedSaveNondurableRetirementStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    bool retirementValidated = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retirement-validated"),
        [&]() {
            retirementValidated = true;
            anchoredFilesystemSyncFailurePath = dir.path();
        });
    QVERIFY(!journal->publishAndCommit(error));
    clearLinkedActivitySaveTransitionAction();
    QVERIFY(retirementValidated);
    QVERIFY2(!error.isEmpty(), "A nondurable retirement must report an error");
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).sourcePath));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));

    error.clear();
    QVERIFY(!journal->cleanupAfterRollback(error));
    QVERIFY2(!error.isEmpty(), "Nondurable recovery must remain retryable");
    QVERIFY(QFileInfo::exists(journalPath));

    anchoredFilesystemSyncFailurePath.clear();
    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).targetPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).targetPath));
#endif
}

void TestAtomicActivitySave::
linkedSavePartialRetirementKeepsRecoveryJournal()
{
#ifndef Q_OS_UNIX
    QSKIP("Quarantine transition injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    QString retainedQuarantine;
    bool injected = false;
    anchoredFilesystemAction =
        [&](const char *transition,
            const QString &primary,
            const QString &) {
            if (injected
                || QByteArray(transition) != "remove-quarantine-verified") {
                return;
            }
            injected = true;
            retainedQuarantine = primary + QStringLiteral(".retained");
            QFile::rename(primary, retainedQuarantine);
        };

    QVERIFY(!journal->publishAndCommit(error));
    anchoredFilesystemAction = {};
    QVERIFY(injected);
    QVERIFY2(!error.isEmpty(), "A partial retirement must report an error");
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(retainedQuarantine));

    error.clear();
    QVERIFY(!journal->cleanupAfterRollback(error));
    QVERIFY2(!error.isEmpty(), "Partial recovery must remain explicit");
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(retainedQuarantine));
#endif
}

void TestAtomicActivitySave::
linkedSavePublishRetryPreservesIncompleteRetirementIntent()
{
#ifndef Q_OS_UNIX
    QSKIP("Quarantine transition injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString intentPath = QDir(journalPath).filePath(
        QStringLiteral("retirement-0000.pending"));
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    QString retainedQuarantine;
    bool injected = false;
    anchoredFilesystemAction =
        [&](const char *transition,
            const QString &primary,
            const QString &) {
            if (injected
                || QByteArray(transition) != "remove-quarantine-verified") {
                return;
            }
            injected = true;
            retainedQuarantine = primary + QStringLiteral(".retained");
            QFile::rename(primary, retainedQuarantine);
        };

    QVERIFY(!journal->publishAndCommit(error));
    anchoredFilesystemAction = {};
    QVERIFY(injected);
    QVERIFY(QFileInfo::exists(intentPath));
    QVERIFY(QFileInfo::exists(retainedQuarantine));

    AnchoredFileSystem::DirectoryAnchor intentParent;
    AnchoredFileSystem::EntryRef intentEntry;
    AnchoredFileSystem::PinnedFile intentPin;
    QString pinError;
    QVERIFY2(
        AnchoredFileSystem::DirectoryAnchor::open(
            journalPath, intentParent, pinError),
        qPrintable(pinError));
    intentEntry = intentParent.entry(
        QStringLiteral("retirement-0000.pending"), pinError);
    QVERIFY2(
        intentEntry.isValid()
            && AnchoredFileSystem::pinRegularFile(
                intentEntry, intentPin, pinError),
        qPrintable(pinError));

    error.clear();
    QVERIFY2(
        !journal->publishAndCommit(error),
        "A retry accepted an incomplete source retirement");
    QVERIFY2(!error.isEmpty(), "Rejected retry must report an error");
    QVERIFY(QFileInfo::exists(intentPath));
    bool intentStillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            intentEntry, intentPin, intentStillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(
        intentStillMatches,
        "A publish retry removed the incomplete retirement intent");

    journal.reset();
    error.clear();
    QVERIFY2(
        !LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        "Fresh recovery forgot the incomplete source retirement");
    QVERIFY2(!error.isEmpty(), "Blocked recovery must report an error");
    QVERIFY(QFileInfo::exists(intentPath));
    QVERIFY(QFileInfo::exists(retainedQuarantine));
#endif
}

void TestAtomicActivitySave::
linkedSaveIncompleteRetirementBlocksFreshRecovery()
{
#ifndef Q_OS_UNIX
    QSKIP("Quarantine transition injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    QString retainedQuarantine;
    bool injected = false;
    anchoredFilesystemAction =
        [&](const char *transition,
            const QString &primary,
            const QString &) {
            if (injected
                || QByteArray(transition) != "remove-quarantine-verified") {
                return;
            }
            injected = true;
            retainedQuarantine = primary + QStringLiteral(".retained");
            QFile::rename(primary, retainedQuarantine);
        };

    QVERIFY(!journal->publishAndCommit(error));
    anchoredFilesystemAction = {};
    QVERIFY(injected);
    QVERIFY(QFileInfo::exists(retainedQuarantine));
    journal.reset();

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);

    QVERIFY2(
        !reconciled,
        "Fresh recovery forgot an incomplete source retirement");
    QVERIFY2(!error.isEmpty(), "Blocked recovery must report an error");
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(retainedQuarantine));
#endif
}

void TestAtomicActivitySave::
linkedSaveRetirementIntentControlFiles_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("inPlace");
    QTest::addColumn<bool>("recoverable");
    QTest::addColumn<QString>("errorFragment");

    QTest::newRow("valid-pending")
        << QStringLiteral("valid") << false << false
        << QStringLiteral("manual recovery");
    QTest::newRow("invalid-contents")
        << QStringLiteral("invalid-contents") << false << false
        << QStringLiteral("invalid source-retirement intent");
    QTest::newRow("out-of-range")
        << QStringLiteral("out-of-range") << false << false
        << QStringLiteral("index is invalid");
    QTest::newRow("in-place")
        << QStringLiteral("valid") << true << false
        << QStringLiteral("does not retire its source");
    QTest::newRow("atomic-temporary")
        << QStringLiteral("temporary") << false << true << QString();
    QTest::newRow("temporary-out-of-range")
        << QStringLiteral("temporary-out-of-range") << false << false
        << QStringLiteral("index is invalid");
    QTest::newRow("temporary-in-place")
        << QStringLiteral("temporary") << true << false
        << QStringLiteral("does not retire its source");
    QTest::newRow("pending-directory")
        << QStringLiteral("directory") << false << false
        << QStringLiteral("unsafe entry");
    QTest::newRow("pending-symbolic-link")
        << QStringLiteral("symbolic-link") << false << false
        << QStringLiteral("unsafe entry");
    QTest::newRow("pending-oversized")
        << QStringLiteral("oversized") << false << false
        << QStringLiteral("unexpectedly large");
    QTest::newRow("lookalike")
        << QStringLiteral("lookalike") << false << false
        << QStringLiteral("unknown file");
}

void TestAtomicActivitySave::
linkedSaveRetirementIntentControlFiles()
{
    QFETCH(QString, kind);
    QFETCH(bool, inPlace);
    QFETCH(bool, recoverable);
    QFETCH(QString, errorFragment);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    if (inPlace) {
        specification.entries[0].targetPath =
            specification.entries.at(0).sourcePath;
    }

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString id = QFileInfo(journalPath).fileName();
    QString name = QStringLiteral("retirement-0000.pending");
    QByteArray contents = id.toLatin1() + ":0\n";
    if (kind == QStringLiteral("invalid-contents")) {
        contents = QByteArray("forged retirement intent\n");
    } else if (kind == QStringLiteral("out-of-range")) {
        name = QStringLiteral("retirement-9999.pending");
        contents = id.toLatin1() + ":9999\n";
    } else if (kind == QStringLiteral("temporary")) {
        name = QStringLiteral(
            ".retirement-0000.pending.ABC123.tmp");
        contents = QByteArray("incomplete temporary data");
    } else if (kind == QStringLiteral("temporary-out-of-range")) {
        name = QStringLiteral(
            ".retirement-9999.pending.ABC123.tmp");
        contents = QByteArray("incomplete temporary data");
    } else if (kind == QStringLiteral("lookalike")) {
        name = QStringLiteral("retirement-0000.pending.extra");
    }
    const QString controlPath = QDir(journalPath).filePath(name);
    if (kind == QStringLiteral("directory")) {
        QVERIFY(QDir().mkdir(controlPath));
    } else if (kind == QStringLiteral("symbolic-link")) {
        const QString sentinel = dir.filePath(
            QStringLiteral("retirement-intent-sentinel"));
        writeFixture(sentinel, contents);
        if (!QFile::link(sentinel, controlPath)) {
            QSKIP("Symbolic links are unavailable");
        }
    } else {
        if (kind == QStringLiteral("oversized")) {
            contents = QByteArray(129, 'x');
        }
        writeFixture(controlPath, contents);
    }
    journal.reset();

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);

    QCOMPARE(reconciled, recoverable);
    if (recoverable) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(journalPath));
    } else {
        QVERIFY2(
            error.contains(errorFragment, Qt::CaseInsensitive),
            qPrintable(error));
        QVERIFY(QFileInfo::exists(journalPath));
    }
}

void TestAtomicActivitySave::
linkedSaveRetirementIntentMustRemainNamed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString intentPath = QDir(journalPath).filePath(
        QStringLiteral("retirement-0000.pending"));
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    bool hookReached = false;
    bool intentRemoved = false;
    bool removalSynced = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retirement-validated"),
        [&]() {
            hookReached = true;
            intentRemoved = QFile::remove(intentPath);
            QString syncError;
            removalSynced = intentRemoved
                && syncParentDirectory(intentPath, syncError);
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(intentRemoved);
    QVERIFY(removalSynced);
    QVERIFY2(!published, "Source retirement ignored a missing intent");
    QVERIFY2(!error.isEmpty(), "A missing intent must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveIntentRemovalPartialStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory-permission injection is required");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    const QFileDevice::Permissions originalPermissions =
        QFile::permissions(journalPath);
    bool injected = false;
    bool permissionsRestricted = false;
    anchoredFilesystemAction =
        [&](const char *transition,
            const QString &primary,
            const QString &) {
            if (injected
                || QByteArray(transition)
                    != "remove-quarantine-finally-verified"
                || !primary.startsWith(journalPath + QLatin1Char('/'))) {
                return;
            }
            injected = true;
            permissionsRestricted = QFile::setPermissions(
                journalPath,
                QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        };

    error.clear();
    const bool published = journal->publishAndCommit(error);
    anchoredFilesystemAction = {};
    const bool permissionsRestored = QFile::setPermissions(
        journalPath, originalPermissions);

    QVERIFY(injected);
    QVERIFY(permissionsRestricted);
    QVERIFY(permissionsRestored);
    QVERIFY2(!published, "Injected marker removal unexpectedly committed");
    QVERIFY2(!error.isEmpty(), "Partial marker removal must report an error");
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).targetPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).targetPath));
#endif
}

void TestAtomicActivitySave::
linkedSaveIntentRemovalNondurableStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    anchoredFilesystemSyncFailurePath = journalPath;
    error.clear();
    QVERIFY(!journal->publishAndCommit(error));
    QVERIFY2(!error.isEmpty(), "Nondurable marker removal must report an error");
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    QVERIFY(!journal->cleanupAfterRollback(error));
    QVERIFY2(!error.isEmpty(), "Nondurable marker cleanup must remain retryable");
    QVERIFY(QFileInfo::exists(journalPath));

    anchoredFilesystemSyncFailurePath.clear();
    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).targetPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).targetPath));
#endif
}

#ifdef Q_OS_WIN
void TestAtomicActivitySave::
linkedSavePendingWindowsRetirementRetriesAfterHandleClose()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    const QString sourcePath = specification.entries.at(0).sourcePath;
    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(sourcePath.utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());
    forceLegacyWindowsDelete = true;
    QVERIFY(!journal->publishAndCommit(error));
    forceLegacyWindowsDelete = false;
    QVERIFY2(!error.isEmpty(), "Pending retirement must report an error");
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));

    observer.reset();
    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).targetPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).targetPath));
}

void TestAtomicActivitySave::
linkedSaveWindowsJournalRemovalRetriesAfterHandleClose()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(journalPath.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());

    error.clear();
    QVERIFY2(
        !journal->cleanupAfterRollback(error),
        "A sharing-blocked journal directory removal was accepted");
    QVERIFY2(!error.isEmpty(), "Blocked cleanup must report an error");
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QDir(journalPath).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System).isEmpty());

    observer.reset();
    error.clear();
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveLegacyWindowsJournalFileRemovalRetries_data()
{
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("externalObserver");

    QTest::newRow("rollback-internal") << false << false;
    QTest::newRow("commit-internal") << true << false;
    QTest::newRow("rollback-external") << false << true;
    QTest::newRow("commit-external") << true << true;
}

void TestAtomicActivitySave::
linkedSaveLegacyWindowsJournalFileRemovalRetries()
{
    QFETCH(bool, committed);
    QFETCH(bool, externalObserver);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }
    if (committed) {
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    }

    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    WindowsTestHandle observer(externalObserver
        ? ::CreateFileW(
            reinterpret_cast<LPCWSTR>(manifestPath.utf16()),
            GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)
        : INVALID_HANDLE_VALUE);
    if (externalObserver) {
        QVERIFY(observer.isValid());
    }

    forceLegacyWindowsDelete = true;
    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    forceLegacyWindowsDelete = false;

    QVERIFY2(!firstCleanup, "A pending legacy deletion was accepted");
    QVERIFY2(!error.isEmpty(), "Pending deletion must report an error");
    QVERIFY(QFileInfo::exists(journalPath));

    observer.reset();
    error.clear();
    QVERIFY2(
        committed
            ? journal->cleanupAfterCommit(error)
            : journal->cleanupAfterRollback(error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QVERIFY(!journal->hasCommitMarker());
}
#endif

void TestAtomicActivitySave::
linkedSavePreparedSourceIdentityReplacementIsRejected_data()
{
    QTest::addColumn<bool>("conversion");

    QTest::newRow("rename") << false;
    QTest::newRow("conversion") << true;
}

void TestAtomicActivitySave::
linkedSavePreparedSourceIdentityReplacementIsRejected()
{
    QFETCH(bool, conversion);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    if (conversion) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(
            specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString sourcePath = specification.entries.at(0).sourcePath;
    const QString retainedPath = sourcePath + QStringLiteral(".retained");
    QVERIFY(QFile::rename(sourcePath, retainedPath));
    writeFixture(sourcePath, originalSource);

    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    error.clear();
    const bool published = journal->publishAndCommit(error);

    QVERIFY2(!published, "A byte-identical replacement was committed");
    QVERIFY2(!error.isEmpty(), "A rejected replacement must report an error");
    QCOMPARE(readAll(sourcePath), originalSource);
    QCOMPARE(readAll(retainedPath), originalSource);
    QCOMPARE(readAll(specification.entries.at(1).sourcePath), secondSource);
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).targetPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).targetPath));
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("source-0000.old"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("new-0000.stage"))));
    if (conversion) {
        QCOMPARE(
            readAll(specification.entries.at(0).backupPath),
            previousBackup);
        QCOMPARE(
            readAll(QDir(journalPath).filePath(
                QStringLiteral("backup-0000.old"))),
            previousBackup);
    } else {
        QVERIFY(!QFileInfo::exists(
            specification.entries.at(0).backupPath));
    }
}

void TestAtomicActivitySave::
linkedSaveSourceRetirementSubstitutionIsRejected_data()
{
    QTest::addColumn<bool>("conversion");

    QTest::newRow("rename") << false;
    QTest::newRow("conversion") << true;
}

void TestAtomicActivitySave::
linkedSaveSourceRetirementSubstitutionIsRejected()
{
    QFETCH(bool, conversion);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    const QByteArray sentinel("concurrent source sentinel");
    if (conversion) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(
            specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    const QString sourcePath = specification.entries.at(0).sourcePath;
    const QString retainedPath = sourcePath + QStringLiteral(".retained");
    bool hookReached = false;
    bool sourceRenamed = false;
    bool sentinelWritten = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retirement-validated"),
        [&]() {
            hookReached = true;
            sourceRenamed = QFile::rename(sourcePath, retainedPath);
            if (!sourceRenamed) return;
            QFile replacement(sourcePath);
            sentinelWritten = replacement.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && replacement.write(sentinel) == sentinel.size()
                && replacement.flush();
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(sourceRenamed);
    QVERIFY(sentinelWritten);
    QVERIFY2(!published, "A substituted source pathname was committed");
    QVERIFY2(!error.isEmpty(), "A rejected substitution must report an error");
    QCOMPARE(readAll(sourcePath), sentinel);
    QCOMPARE(readAll(retainedPath), originalSource);
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(!QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("source-0000.old"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("new-0000.stage"))));
    QCOMPARE(
        readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(
        readAll(specification.entries.at(1).targetPath), secondStaged);
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath), secondSource);
    if (conversion) {
        QCOMPARE(
            readAll(specification.entries.at(0).backupPath),
            originalSource);
        QCOMPARE(
            readAll(QDir(journalPath).filePath(
                QStringLiteral("backup-0000.old"))),
            previousBackup);
    } else {
        QVERIFY(!QFileInfo::exists(
            specification.entries.at(0).backupPath));
    }
}

void TestAtomicActivitySave::
linkedSaveSourceRepopulationStopsFurtherRetirement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    const QByteArray sentinel("repopulated source sentinel");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool hookReached = false;
    bool sentinelWritten = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retired"),
        [&]() {
            hookReached = true;
            QFile replacement(specification.entries.at(0).sourcePath);
            sentinelWritten = replacement.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && replacement.write(sentinel) == sentinel.size()
                && replacement.flush();
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(sentinelWritten);
    QVERIFY(!published);
    QVERIFY2(!error.isEmpty(), "Repopulation must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), sentinel);
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveFinalSourceRepopulationIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    const QByteArray sentinel("last-moment source sentinel");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool hookReached = false;
    bool sentinelWritten = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-before-final-retirement-check"),
        [&]() {
            hookReached = true;
            QFile replacement(specification.entries.at(0).sourcePath);
            sentinelWritten = replacement.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && replacement.write(sentinel) == sentinel.size()
                && replacement.flush();
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(sentinelWritten);
    QVERIFY2(!published, "A repopulated source name was committed");
    QVERIFY2(!error.isEmpty(), "Rejected repopulation must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), sentinel);
    QVERIFY(!journal->hasCommitMarker());
    QVERIFY(QFileInfo::exists(journalPath));
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
}

void TestAtomicActivitySave::
linkedSaveCommitBoundarySourceRepopulationIsRejected_data()
{
    QTest::addColumn<QByteArray>("transition");
    QTest::addColumn<bool>("commitMarkerExpected");
    QTest::addColumn<bool>("conversion");

    QTest::newRow("before-marker-rename")
        << QByteArray("linked-save-after-final-retirement-check")
        << false << false;
    QTest::newRow("before-marker-conversion")
        << QByteArray("linked-save-after-final-retirement-check")
        << false << true;
    QTest::newRow("after-marker-rename")
        << QByteArray("linked-save-commit-marker") << true << false;
    QTest::newRow("after-marker-conversion")
        << QByteArray("linked-save-commit-marker") << true << true;
}

void TestAtomicActivitySave::
linkedSaveCommitBoundarySourceRepopulationIsRejected()
{
    QFETCH(QByteArray, transition);
    QFETCH(bool, commitMarkerExpected);
    QFETCH(bool, conversion);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    if (conversion) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(
            specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool hookReached = false;
    bool sourceWritten = false;
    setLinkedActivitySaveTransitionAction(
        transition,
        [&]() {
            hookReached = true;
            QFile source(specification.entries.at(0).sourcePath);
            sourceWritten = source.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && source.write(originalSource) == originalSource.size()
                && source.flush();
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(sourceWritten);
    QVERIFY2(!published, "Source repopulation crossed the commit boundary");
    QVERIFY2(!error.isEmpty(), "Rejected repopulation must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), originalSource);
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QCOMPARE(journal->hasCommitMarker(), commitMarkerExpected);
    QVERIFY(QFileInfo::exists(journalPath));

    if (commitMarkerExpected) {
        error.clear();
        QVERIFY2(
            !journal->publishAndCommit(error),
            "A committed retry ignored a repopulated source");
        QVERIFY2(!error.isEmpty(), "Rejected retry must report an error");
        QCOMPARE(
            readAll(specification.entries.at(0).sourcePath),
            originalSource);
        QVERIFY(QFileInfo::exists(journalPath));

        error.clear();
        QVERIFY(!journal->cleanupAfterCommit(error));
        QVERIFY2(!error.isEmpty(), "Committed cleanup must reject the source");
        QCOMPARE(
            readAll(specification.entries.at(0).sourcePath),
            originalSource);
        QVERIFY(QFileInfo::exists(journalPath));
    }
}

void TestAtomicActivitySave::
linkedSaveRollbackPreservesRecreatedRetiredSource_data()
{
    QTest::addColumn<QByteArray>("transition");
    QTest::addColumn<bool>("freshRecovery");

    QTest::newRow("active-rollback")
        << QByteArray("linked-save-after-final-retirement-check") << false;
    QTest::newRow("fresh-recovery")
        << QByteArray("linked-save-retirement-intent-removed") << true;
}

void TestAtomicActivitySave::
linkedSaveRollbackPreservesRecreatedRetiredSource()
{
    QFETCH(QByteArray, transition);
    QFETCH(bool, freshRecovery);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool hookReached = false;
    bool sourceWritten = false;
    setLinkedActivitySaveTransitionAction(
        transition,
        [&]() {
            hookReached = true;
            QFile source(specification.entries.at(0).sourcePath);
            sourceWritten = source.open(QIODevice::WriteOnly)
                && source.write(firstStaged) == firstStaged.size()
                && source.flush();
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(sourceWritten);
    QVERIFY2(!published, "A recreated retired source was committed");
    QVERIFY2(!error.isEmpty(), "Rejected repopulation must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), firstStaged);
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    bool cleaned = false;
    if (freshRecovery) {
        journal.reset();
        cleaned = LinkedActivitySave::Journal::reconcileAll(
            dir.path(), error);
    } else {
        cleaned = journal->cleanupAfterRollback(error);
    }

    QVERIFY2(!cleaned, "Rollback overwrote a recreated retired source");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), firstStaged);
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
}

void TestAtomicActivitySave::
linkedSaveRollbackPublicationRacePreservesNewSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool targetRemoved = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retired"),
        [&]() {
            targetRemoved = QFile::remove(
                specification.entries.at(0).targetPath);
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(targetRemoved);
    QVERIFY2(!published, "Publication ignored a missing staged target");
    QVERIFY2(!error.isEmpty(), "Failed publication must report an error");
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).sourcePath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).sourcePath));

    const QString sourcePath = specification.entries.at(0).sourcePath;
    const QByteArray concurrentSource("concurrent source generation");
    AnchoredFileSystem::DirectoryAnchor sourceParent;
    AnchoredFileSystem::EntryRef sourceEntry;
    AnchoredFileSystem::PinnedFile sourcePin;
    QString injectionError;
    bool sourceWritten = false;
    bool sourcePinned = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-before-source-restore"),
        [&]() {
            QFile source(sourcePath);
            sourceWritten = source.open(QIODevice::WriteOnly)
                && source.write(concurrentSource) == concurrentSource.size()
                && source.flush();
            if (!sourceWritten
                || !AnchoredFileSystem::DirectoryAnchor::open(
                    QFileInfo(sourcePath).absolutePath(),
                    sourceParent,
                    injectionError)) {
                return;
            }
            sourceEntry = sourceParent.entry(
                QFileInfo(sourcePath).fileName(), injectionError);
            sourcePinned = sourceEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    sourceEntry, sourcePin, injectionError);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(sourceWritten);
    QVERIFY2(sourcePinned, qPrintable(injectionError));
    QVERIFY2(!cleaned, "Rollback replaced a post-observation source");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(readAll(sourcePath), concurrentSource);
    bool sourceStillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            sourceEntry, sourcePin, sourceStillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(sourceStillMatches, "Rollback changed the new source identity");
    QVERIFY(QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveRollbackProductionSubstitutionIsRejected_data()
{
    QTest::addColumn<QString>("mutation");

    QTest::newRow("in-place-source") << QStringLiteral("in-place-source");
    QTest::newRow("backup-restore") << QStringLiteral("backup-restore");
    QTest::newRow("backup-remove") << QStringLiteral("backup-remove");
    QTest::newRow("target-remove") << QStringLiteral("target-remove");
}

void TestAtomicActivitySave::
linkedSaveRollbackProductionSubstitutionIsRejected()
{
    QFETCH(QString, mutation);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    const bool backupMutation = mutation.startsWith(
        QStringLiteral("backup-"));
    if (backupMutation) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(specification.entries.at(0).sourcePath, originalSource);
        if (mutation == QStringLiteral("backup-restore")) {
            writeFixture(
                specification.entries.at(0).backupPath, previousBackup);
        }
        writeFixture(specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }
    if (mutation == QStringLiteral("in-place-source")) {
        specification.entries[0].targetPath =
            specification.entries.at(0).sourcePath;
    }

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool targetRemoved = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retired"),
        [&]() {
            targetRemoved = QFile::remove(
                specification.entries.at(1).targetPath);
        });
    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(targetRemoved);
    QVERIFY2(!published, "Publication ignored a missing linked target");
    QVERIFY2(!error.isEmpty(), "Failed publication must report an error");

    QString path;
    QByteArray observedBytes;
    QByteArray transition;
    if (mutation == QStringLiteral("in-place-source")) {
        path = specification.entries.at(0).sourcePath;
        observedBytes = firstStaged;
        transition = QByteArray("linked-save-before-source-restore");
    } else if (mutation == QStringLiteral("backup-restore")) {
        path = specification.entries.at(0).backupPath;
        observedBytes = originalSource;
        transition = QByteArray("linked-save-before-backup-restore");
    } else if (mutation == QStringLiteral("backup-remove")) {
        path = specification.entries.at(0).backupPath;
        observedBytes = originalSource;
        transition = QByteArray("linked-save-before-backup-remove");
    } else {
        path = specification.entries.at(0).targetPath;
        observedBytes = firstStaged;
        transition = QByteArray("linked-save-before-target-remove");
    }
    QCOMPARE(readAll(path), observedBytes);

    AnchoredFileSystem::DirectoryAnchor parent;
    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile pin;
    QString injectionError;
    bool replacementPinned = false;
    setLinkedActivitySaveTransitionAction(
        transition,
        [&]() {
            replacementPinned = replaceAndPinFixture(
                path,
                path + QStringLiteral(".transaction-owned"),
                observedBytes,
                parent,
                entry,
                pin,
                injectionError);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY2(replacementPinned, qPrintable(injectionError));
    QVERIFY2(!cleaned, "Rollback changed a replacement production file");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(readAll(path), observedBytes);
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            entry, pin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "Rollback changed the replacement file identity");
    QVERIFY(QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveRollbackProductionParentSubstitutionIsRejected_data()
{
    QTest::addColumn<bool>("removeTarget");
    QTest::addColumn<bool>("duringRemoval");
    QTest::addColumn<bool>("beforeQuarantine");

    QTest::newRow("restore-source") << false << false << false;
    QTest::newRow("remove-target") << true << false << false;
    QTest::newRow("remove-target-during-syscall")
        << true << true << false;
    QTest::newRow("remove-target-before-quarantine")
        << true << true << true;
}

void TestAtomicActivitySave::
linkedSaveRollbackProductionParentSubstitutionIsRejected()
{
#ifndef Q_OS_UNIX
    QSKIP("Renaming an open production directory is Unix-specific");
#else
    QFETCH(bool, removeTarget);
    QFETCH(bool, duringRemoval);
    QFETCH(bool, beforeQuarantine);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString productionPath =
        dir.filePath(QStringLiteral("activities"));
    const QString displacedPath =
        dir.filePath(QStringLiteral("displaced-activities"));
    QVERIFY(QDir().mkpath(productionPath));

    LinkedActivitySave::Specification specification;
    specification.athleteRoot = dir.path();
    specification.entries = {
        {QDir(productionPath).filePath(QStringLiteral("first-old.json")),
         QDir(productionPath).filePath(QStringLiteral("first-new.json")),
         QDir(productionPath).filePath(QStringLiteral("first-old.json.bak")),
         false},
        {QDir(productionPath).filePath(QStringLiteral("second-old.json")),
         QDir(productionPath).filePath(QStringLiteral("second-new.json")),
         QDir(productionPath).filePath(QStringLiteral("second-old.json.bak")),
         false}};
    const QByteArray firstSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(specification.entries.at(0).sourcePath, firstSource);
    writeFixture(specification.entries.at(1).sourcePath, secondSource);

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool targetRemoved = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retired"),
        [&]() {
            targetRemoved = QFile::remove(
                specification.entries.at(1).targetPath);
        });
    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();
    QVERIFY(targetRemoved);
    QVERIFY2(!published, "Publication ignored a missing linked target");

    if (removeTarget) {
        writeFixture(specification.entries.at(0).sourcePath, firstSource);
        writeFixture(specification.entries.at(1).sourcePath, secondSource);
    }

    bool hookReached = false;
    bool parentMoved = false;
    bool substituteCreated = false;
    const auto moveParent = [&]() {
        hookReached = true;
        parentMoved = QDir().rename(productionPath, displacedPath);
        substituteCreated = parentMoved && QDir().mkpath(productionPath);
        if (!substituteCreated) return;
        writeFixture(
            specification.entries.at(0).sourcePath, firstSource);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    };
    if (duringRemoval) {
        anchoredFilesystemAction =
            [&](const char *transition,
                const QString &primary,
                const QString &) {
                if (hookReached
                    || QByteArray(transition)
                        != (beforeQuarantine
                            ? "remove-before-quarantine"
                            : "remove-quarantine-finally-verified")
                    || !primary.startsWith(
                        productionPath + QLatin1Char('/'))) {
                    return;
                }
                moveParent();
            };
    } else {
        setLinkedActivitySaveTransitionAction(
            QByteArray("linked-save-activity-destination-anchored"),
            moveParent);
    }

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();
    anchoredFilesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(parentMoved);
    QVERIFY(substituteCreated);
    QVERIFY2(!cleaned, "Rollback accepted a substituted production parent");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath), firstSource);
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath), secondSource);
    QVERIFY(!QFileInfo::exists(
        specification.entries.at(0).targetPath));
    if (beforeQuarantine) {
        QCOMPARE(
            readAll(QDir(displacedPath).filePath(
                QStringLiteral("first-new.json"))),
            firstStaged);
        QCOMPARE(
            readAll(QDir(displacedPath).filePath(
                QStringLiteral("first-old.json"))),
            firstSource);
    } else if (duringRemoval) {
        QCOMPARE(
            readAll(QDir(displacedPath).filePath(
                QStringLiteral("first-old.json"))),
            firstSource);
    } else if (removeTarget) {
        QCOMPARE(
            readAll(QDir(displacedPath).filePath(
                QStringLiteral("first-new.json"))),
            firstStaged);
    } else {
        QVERIFY(!QFileInfo::exists(
            QDir(displacedPath).filePath(
                QStringLiteral("first-old.json"))));
    }
    QVERIFY(QFileInfo::exists(journalPath));
#endif
}

void TestAtomicActivitySave::
linkedSaveCommittedRecoverySubstitutionIsRejected_data()
{
    QTest::addColumn<QString>("mutation");

    QTest::newRow("target") << QStringLiteral("target");
    QTest::newRow("backup") << QStringLiteral("backup");
}

void TestAtomicActivitySave::
linkedSaveCommittedRecoverySubstitutionIsRejected()
{
    QFETCH(QString, mutation);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    if (mutation == QStringLiteral("backup")) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());

    const QString path = mutation == QStringLiteral("backup")
        ? specification.entries.at(0).backupPath
        : specification.entries.at(0).targetPath;
    const QByteArray observedBytes = mutation == QStringLiteral("backup")
        ? previousBackup
        : originalSource;
    const QByteArray transition = mutation == QStringLiteral("backup")
        ? QByteArray("linked-save-before-recovery-backup-publish")
        : QByteArray("linked-save-before-recovery-target-publish");
    writeFixture(path, observedBytes);
    journal.reset();

    AnchoredFileSystem::DirectoryAnchor parent;
    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile pin;
    QString injectionError;
    bool replacementPinned = false;
    setLinkedActivitySaveTransitionAction(
        transition,
        [&]() {
            replacementPinned = replaceAndPinFixture(
                path,
                path + QStringLiteral(".transaction-owned"),
                observedBytes,
                parent,
                entry,
                pin,
                injectionError);
        });

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY2(replacementPinned, qPrintable(injectionError));
    QVERIFY2(!reconciled, "Recovery changed a replacement production file");
    QVERIFY2(!error.isEmpty(), "Rejected recovery must report an error");
    QCOMPARE(readAll(path), observedBytes);
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            entry, pin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "Recovery changed the replacement file identity");
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
}

void TestAtomicActivitySave::
linkedSaveRollbackPreservesLateCommitMarker()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString markerPath = QDir(journalPath).filePath(
        QStringLiteral("COMMITTED"));
    const QByteArray markerContents =
        QFileInfo(journalPath).fileName().toLatin1() + '\n';

    AnchoredFileSystem::DirectoryAnchor markerParent;
    AnchoredFileSystem::EntryRef markerEntry;
    AnchoredFileSystem::PinnedFile markerPin;
    QString injectionError;
    bool markerPinned = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-rollback-decision-observed"),
        [&]() {
            writeFixture(markerPath, markerContents);
            if (!AnchoredFileSystem::DirectoryAnchor::open(
                    journalPath, markerParent, injectionError)) {
                return;
            }
            markerEntry = markerParent.entry(
                QStringLiteral("COMMITTED"), injectionError);
            markerPinned = markerEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    markerEntry, markerPin, injectionError);
        });

    error.clear();
    const bool cleaned = journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY2(markerPinned, qPrintable(injectionError));
    QVERIFY2(!cleaned, "Rollback erased a late commit decision");
    QVERIFY2(!error.isEmpty(), "A late commit decision must report an error");
    QCOMPARE(readAll(markerPath), markerContents);
    bool markerStillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            markerEntry, markerPin, markerStillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(markerStillMatches, "Rollback changed the late commit marker");
    QVERIFY(QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveMovedLiveJournalCannotClaimCleanup()
{
#ifndef Q_OS_UNIX
    QSKIP("Renaming an open journal is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString journalId = QFileInfo(journalPath).fileName();
    const QString movedJournalPath =
        dir.filePath(QStringLiteral("moved-linked-save-journal"));
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    bool hookReached = false;
    bool journalMoved = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-source-retired"),
        [&]() {
            hookReached = true;
            journalMoved = QDir().rename(journalPath, movedJournalPath);
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(journalMoved);
    QVERIFY2(!published, "A transaction with a moved journal was committed");
    QVERIFY2(!error.isEmpty(), "A moved journal must report an error");
    QVERIFY(!QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(QDir(movedJournalPath).filePath(
            QStringLiteral("retirement-0000.pending"))),
        journalId.toLatin1() + ":0\n");
    QCOMPARE(
        readAll(QDir(movedJournalPath).filePath(
            QStringLiteral("source-0000.old"))),
        QByteArray("first old generation"));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).sourcePath));

    error.clear();
    QVERIFY2(
        !journal->cleanupAfterRollback(error),
        "A moved live journal was reported as successfully cleaned");
    QVERIFY2(!error.isEmpty(), "Rejected cleanup must report an error");
    QCOMPARE(
        readAll(QDir(movedJournalPath).filePath(
            QStringLiteral("retirement-0000.pending"))),
        journalId.toLatin1() + ":0\n");
    QCOMPARE(
        readAll(QDir(movedJournalPath).filePath(
            QStringLiteral("source-0000.old"))),
        QByteArray("first old generation"));
#endif
}

void TestAtomicActivitySave::
linkedSaveMovedJournalCannotReceiveCommitMarker()
{
#ifndef Q_OS_UNIX
    QSKIP("Renaming an open journal is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString movedJournalPath =
        dir.filePath(QStringLiteral("moved-before-commit-marker"));
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ") + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    bool hookReached = false;
    bool journalMoved = false;
    bool substituteCreated = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-after-final-retirement-check"),
        [&]() {
            hookReached = true;
            journalMoved = QDir().rename(journalPath, movedJournalPath);
            substituteCreated = journalMoved && QDir().mkpath(journalPath);
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(hookReached);
    QVERIFY(journalMoved);
    QVERIFY(substituteCreated);
    QVERIFY2(!published, "A substituted journal received the commit marker");
    QVERIFY2(!error.isEmpty(), "A substituted journal must report an error");
    QVERIFY(!QFileInfo::exists(
        QDir(movedJournalPath).filePath(QStringLiteral("COMMITTED"))));
    QVERIFY(!QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
#endif
}

void TestAtomicActivitySave::
linkedSaveNondurableCommitMarkerStaysRecoverable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString markerPath = QDir(journalPath).filePath(
        QStringLiteral("COMMITTED"));
    const QByteArray markerContents =
        QFileInfo(journalPath).fileName().toLatin1() + '\n';
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    bool markerTemporaryPublished = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-commit-marker-temporary"),
        [&]() {
            markerTemporaryPublished = true;
            anchoredFilesystemSyncFailurePath = journalPath;
        });
    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(markerTemporaryPublished);
    QVERIFY2(!published, "A nondurable commit marker was accepted");
    QVERIFY2(!error.isEmpty(), "Nondurable publication must report an error");
    QCOMPARE(readAll(markerPath), markerContents);
    QVERIFY(QFileInfo::exists(journalPath));

    anchoredFilesystemSyncFailurePath.clear();
    journal.reset();
    error.clear();
    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(0).sourcePath));
    QVERIFY(!QFileInfo::exists(specification.entries.at(1).sourcePath));
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
#endif
}

void TestAtomicActivitySave::
linkedSaveCommitMarkerIdentityLossIsRejected_data()
{
    QTest::addColumn<bool>("replaceMarker");

    QTest::newRow("removed") << false;
    QTest::newRow("replaced") << true;
}

void TestAtomicActivitySave::
linkedSaveCommitMarkerIdentityLossIsRejected()
{
    QFETCH(bool, replaceMarker);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString markerPath = QDir(journalPath).filePath(
        QStringLiteral("COMMITTED"));
    const QString retainedPath = QDir(journalPath).filePath(
        QStringLiteral("retained-COMMITTED"));
    const QByteArray markerContents =
        QFileInfo(journalPath).fileName().toLatin1() + '\n';
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    AnchoredFileSystem::DirectoryAnchor markerParent;
    AnchoredFileSystem::EntryRef originalEntry;
    AnchoredFileSystem::EntryRef replacementEntry;
    AnchoredFileSystem::PinnedFile originalPin;
    AnchoredFileSystem::PinnedFile replacementPin;
    QString injectionError;
    bool markerPinned = false;
    bool markerMutated = false;
    bool replacementPinned = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-commit-marker"),
        [&]() {
            if (!AnchoredFileSystem::DirectoryAnchor::open(
                    journalPath, markerParent, injectionError)) {
                return;
            }
            originalEntry = markerParent.entry(
                QStringLiteral("COMMITTED"), injectionError);
            markerPinned = originalEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    originalEntry, originalPin, injectionError);
            if (!markerPinned) return;
            markerMutated = replaceMarker
                ? QFile::rename(markerPath, retainedPath)
                : QFile::remove(markerPath);
            if (!markerMutated || !replaceMarker) return;
            writeFixture(markerPath, markerContents);
            replacementEntry = markerParent.entry(
                QStringLiteral("COMMITTED"), injectionError);
            replacementPinned = replacementEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    replacementEntry, replacementPin, injectionError);
        });

    error.clear();
    const bool published = journal->publishAndCommit(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY2(markerPinned, qPrintable(injectionError));
    QVERIFY(markerMutated);
    if (replaceMarker) {
        QVERIFY2(replacementPinned, qPrintable(injectionError));
    }
    QVERIFY2(!published, "Publication accepted a lost commit-marker identity");
    QVERIFY2(!error.isEmpty(), "Marker identity loss must report an error");
    QVERIFY(originalPin.isValid());
    if (replaceMarker) {
        bool replacementMatches = false;
        QString matchError;
        const AnchoredFileSystem::EntryRef retainedEntry =
            markerParent.entry(QStringLiteral("retained-COMMITTED"), matchError);
        QVERIFY2(retainedEntry.isValid(), qPrintable(matchError));
        AnchoredFileSystem::PinnedFile retainedPin;
        QVERIFY2(
            AnchoredFileSystem::pinRegularFile(
                retainedEntry, retainedPin, matchError),
            qPrintable(matchError));
        QVERIFY(retainedPin.identity() == originalPin.identity());
        QVERIFY2(
            AnchoredFileSystem::entryMatches(
                replacementEntry,
                replacementPin,
                replacementMatches,
                matchError),
            qPrintable(matchError));
        QVERIFY(replacementMatches);
    } else {
        QVERIFY(!QFileInfo::exists(markerPath));
    }

    error.clear();
    QVERIFY2(
        !journal->cleanupAfterRollback(error),
        "Rollback ignored an ambiguous commit-marker publication");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
}

void TestAtomicActivitySave::
linkedSavePartialCommitMarkerPublicationBlocksRollback()
{
#ifndef Q_OS_UNIX
    QSKIP("Unlinking a pinned commit marker is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString markerPath = QDir(journalPath).filePath(
        QStringLiteral("COMMITTED"));
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));

    AnchoredFileSystem::DirectoryAnchor markerParent;
    AnchoredFileSystem::EntryRef markerEntry;
    AnchoredFileSystem::PinnedFile markerPin;
    QString injectionError;
    bool hookReached = false;
    bool markerPinned = false;
    bool markerRemoved = false;
    anchoredFilesystemAction =
        [&](const char *transition,
            const QString &,
            const QString &secondary) {
            if (hookReached
                || QByteArray(transition) != "move-published"
                || secondary != markerPath) {
                return;
            }
            hookReached = true;
            if (!AnchoredFileSystem::DirectoryAnchor::open(
                    journalPath, markerParent, injectionError)) {
                return;
            }
            markerEntry = markerParent.entry(
                QStringLiteral("COMMITTED"), injectionError);
            markerPinned = markerEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    markerEntry, markerPin, injectionError);
            markerRemoved = markerPinned && QFile::remove(markerPath);
        };

    error.clear();
    const bool published = journal->publishAndCommit(error);
    anchoredFilesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY2(markerPinned, qPrintable(injectionError));
    QVERIFY(markerRemoved);
    QVERIFY2(!published, "A partial commit-marker publication was accepted");
    QVERIFY2(!error.isEmpty(), "Partial publication must report an error");
    QVERIFY(!QFileInfo::exists(markerPath));
    QVERIFY(markerPin.isValid());

    error.clear();
    QVERIFY2(
        !journal->cleanupAfterRollback(error),
        "Rollback ignored a partial commit-marker publication");
    QVERIFY2(!error.isEmpty(), "Rejected rollback must report an error");
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
#endif
}

#ifdef Q_OS_WIN
void TestAtomicActivitySave::
anchoredFilesDenyConcurrentWindowsWrites_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("pin-existing") << QStringLiteral("pin");
    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAtomicActivitySave::
anchoredFilesDenyConcurrentWindowsWrites()
{
    QFETCH(QString, operation);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("source.bin"));
    const QString destinationPath = operation == QStringLiteral("pin")
        ? sourcePath : dir.filePath(QStringLiteral("destination.bin"));
    const QByteArray contents("identity-bound contents");
    writeFixture(sourcePath, contents);

    QString error;
    AnchoredFileSystem::DirectoryAnchor parent;
    QVERIFY2(
        AnchoredFileSystem::DirectoryAnchor::open(
            dir.path(), parent, error),
        qPrintable(error));
    const AnchoredFileSystem::EntryRef sourceEntry =
        parent.entry(QStringLiteral("source.bin"), error);
    QVERIFY2(sourceEntry.isValid(), qPrintable(error));
    AnchoredFileSystem::PinnedFile source;
    QVERIFY2(
        AnchoredFileSystem::pinRegularFile(sourceEntry, source, error),
        qPrintable(error));

    AnchoredFileSystem::PinnedFile destination;
    if (operation == QStringLiteral("pin")) {
        destination = std::move(source);
    } else {
        const AnchoredFileSystem::EntryRef destinationEntry =
            parent.entry(QStringLiteral("destination.bin"), error);
        QVERIFY2(destinationEntry.isValid(), qPrintable(error));
        const bool created = operation == QStringLiteral("copy")
            ? AnchoredFileSystem::copyToNewFile(
                  source, destinationEntry, destination, error)
            : AnchoredFileSystem::writeNewFile(
                  contents, destinationEntry, destination, error);
        QVERIFY2(created, qPrintable(error));
    }

    WindowsTestHandle concurrentWriter(::CreateFileW(
        reinterpret_cast<LPCWSTR>(destinationPath.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    const DWORD nativeError = ::GetLastError();
    QVERIFY2(
        !concurrentWriter.isValid(),
        "A live anchored pin allowed a concurrent Windows writer");
    QCOMPARE(nativeError, DWORD(ERROR_SHARING_VIOLATION));
}

void TestAtomicActivitySave::
anchoredOutputFilesCanBeRepinned_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAtomicActivitySave::
anchoredOutputFilesCanBeRepinned()
{
    QFETCH(QString, operation);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("source.bin"));
    const QByteArray contents("identity-bound contents");
    writeFixture(sourcePath, contents);

    QString error;
    AnchoredFileSystem::DirectoryAnchor parent;
    QVERIFY2(
        AnchoredFileSystem::DirectoryAnchor::open(
            dir.path(), parent, error),
        qPrintable(error));
    const AnchoredFileSystem::EntryRef sourceEntry =
        parent.entry(QStringLiteral("source.bin"), error);
    const AnchoredFileSystem::EntryRef destinationEntry =
        parent.entry(QStringLiteral("destination.bin"), error);
    QVERIFY2(sourceEntry.isValid(), qPrintable(error));
    QVERIFY2(destinationEntry.isValid(), qPrintable(error));

    AnchoredFileSystem::PinnedFile source;
    QVERIFY2(
        AnchoredFileSystem::pinRegularFile(sourceEntry, source, error),
        qPrintable(error));
    AnchoredFileSystem::PinnedFile output;
    const bool created = operation == QStringLiteral("copy")
        ? AnchoredFileSystem::copyToNewFile(
              source, destinationEntry, output, error)
        : AnchoredFileSystem::writeNewFile(
              contents, destinationEntry, output, error);
    QVERIFY2(created, qPrintable(error));

    AnchoredFileSystem::PinnedFile repinned;
    QVERIFY2(
        AnchoredFileSystem::pinRegularFile(
            destinationEntry, repinned, error),
        qPrintable(error));
    QCOMPARE(repinned.identity(), output.identity());
    QCOMPARE(repinned.size(), output.size());
    QCOMPARE(repinned.sha256(), output.sha256());
}
#endif

void TestAtomicActivitySave::
linkedSaveJournalCleanupSubstitutionPreservesReplacement_data()
{
    QTest::addColumn<QByteArray>("transition");
    QTest::addColumn<QString>("substituteName");
    QTest::addColumn<bool>("committed");

    QTest::newRow("before-first-remove")
        << QByteArray("linked-save-journal-files-pinned")
        << QStringLiteral("manifest.json") << false;
    QTest::newRow("after-manifest")
        << QByteArray("linked-save-manifest-removed")
        << QStringLiteral("source-0000.old") << false;
    QTest::newRow("between-journal-files")
        << QByteArray("linked-save-cleanup-file")
        << QStringLiteral("new-0000.stage") << false;
    QTest::newRow("after-manifest-committed")
        << QByteArray("linked-save-manifest-removed")
        << QStringLiteral("COMMITTED") << true;
}

void TestAtomicActivitySave::
linkedSaveJournalCleanupSubstitutionPreservesReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("Renaming an open journal is Unix-specific");
#else
    QFETCH(QByteArray, transition);
    QFETCH(QString, substituteName);
    QFETCH(bool, committed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString movedJournalPath =
        dir.filePath(QStringLiteral("retained-linked-save-journal"));
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    if (committed) {
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
        QVERIFY(journal->hasCommitMarker());
    }

    QByteArray substituteBytes;
    if (substituteName == QStringLiteral("manifest.json")
        || substituteName == QStringLiteral("COMMITTED")) {
        substituteBytes = readAll(
            QDir(journalPath).filePath(substituteName));
    } else if (substituteName == QStringLiteral("source-0000.old")) {
        substituteBytes = QByteArray("first old generation");
    } else {
        substituteBytes = firstStaged;
    }
    QVERIFY(!substituteBytes.isEmpty());

    AnchoredFileSystem::DirectoryAnchor substituteParent;
    AnchoredFileSystem::EntryRef substituteEntry;
    AnchoredFileSystem::PinnedFile substitutePin;
    QString injectionError;
    bool journalMoved = false;
    bool substitutePinned = false;
    const QString substitutePath =
        QDir(journalPath).filePath(substituteName);
    setLinkedActivitySaveTransitionAction(
        transition,
        [&]() {
            journalMoved = QDir().rename(journalPath, movedJournalPath);
            if (!journalMoved || !QDir().mkpath(journalPath)) return;
            QFile substitute(substitutePath);
            if (!substitute.open(QIODevice::WriteOnly)
                || substitute.write(substituteBytes)
                    != substituteBytes.size()
                || !substitute.flush()) {
                injectionError = substitute.errorString();
                return;
            }
            substitute.close();
            if (!AnchoredFileSystem::DirectoryAnchor::open(
                    journalPath, substituteParent, injectionError)) {
                return;
            }
            substituteEntry = substituteParent.entry(
                substituteName, injectionError);
            substitutePinned = substituteEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    substituteEntry, substitutePin, injectionError);
        });

    error.clear();
    const bool cleaned = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(journalMoved);
    QVERIFY2(substitutePinned, qPrintable(injectionError));
    QVERIFY2(!cleaned, "Cleanup accepted a substituted journal directory");
    QVERIFY2(!error.isEmpty(), "Rejected cleanup must report an error");
    QCOMPARE(readAll(substitutePath), substituteBytes);
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            substituteEntry, substitutePin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "Cleanup changed a substitute journal file");
    QVERIFY(QFileInfo::exists(movedJournalPath));

    error.clear();
    const bool retried = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    QVERIFY2(!retried, "A cleanup retry accepted a substituted journal");
    QVERIFY2(!error.isEmpty(), "Rejected retry must report an error");
    stillMatches = false;
    matchError.clear();
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            substituteEntry, substitutePin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "A cleanup retry changed the substitute file");
#endif
}

void TestAtomicActivitySave::
linkedSavePreManifestCleanupSubstitutionPreservesReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("Renaming a scanned journal is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString namespacePath = dir.filePath(
        QStringLiteral(".gc-transactions/linked-save"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QString journalId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    const QString journalPath = QDir(namespacePath).filePath(journalId);
    const QString movedJournalPath =
        dir.filePath(QStringLiteral("retained-pre-manifest-journal"));
    QVERIFY(QDir().mkpath(journalPath));
    QVERIFY(QFile::setPermissions(
        journalPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    const QString substituteName = QStringLiteral("source-0000.old");
    const QString substitutePath =
        QDir(journalPath).filePath(substituteName);
    const QByteArray substituteBytes("pre-manifest transaction data");
    writeFixture(substitutePath, substituteBytes);

    AnchoredFileSystem::DirectoryAnchor substituteParent;
    AnchoredFileSystem::EntryRef substituteEntry;
    AnchoredFileSystem::PinnedFile substitutePin;
    QString injectionError;
    bool journalMoved = false;
    bool substitutePinned = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-pre-manifest-scanned"),
        [&]() {
            journalMoved = QDir().rename(journalPath, movedJournalPath);
            if (!journalMoved || !QDir().mkpath(journalPath)) return;
            if (!QFile::setPermissions(
                    journalPath,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner
                        | QFileDevice::ExeOwner)) {
                injectionError = QStringLiteral(
                    "Cannot restrict the substitute journal");
                return;
            }
            QFile substitute(substitutePath);
            if (!substitute.open(QIODevice::WriteOnly)
                || substitute.write(substituteBytes)
                    != substituteBytes.size()
                || !substitute.flush()) {
                injectionError = substitute.errorString();
                return;
            }
            substitute.close();
            if (!AnchoredFileSystem::DirectoryAnchor::open(
                    journalPath, substituteParent, injectionError)) {
                return;
            }
            substituteEntry = substituteParent.entry(
                substituteName, injectionError);
            substitutePinned = substituteEntry.isValid()
                && AnchoredFileSystem::pinRegularFile(
                    substituteEntry, substitutePin, injectionError);
        });

    QString error;
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(journalMoved);
    QVERIFY2(substitutePinned, qPrintable(injectionError));
    QVERIFY2(!reconciled, "Recovery accepted a substituted journal directory");
    QVERIFY2(!error.isEmpty(), "Rejected recovery must report an error");
    QCOMPARE(readAll(substitutePath), substituteBytes);
    bool stillMatches = false;
    QString matchError;
    QVERIFY2(
        AnchoredFileSystem::entryMatches(
            substituteEntry, substitutePin, stillMatches, matchError),
        qPrintable(matchError));
    QVERIFY2(stillMatches, "Recovery changed a substitute journal file");
    QVERIFY(QFileInfo::exists(movedJournalPath));
#endif
}

void TestAtomicActivitySave::
linkedSavePreManifestRemovalDurabilityStaysRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync injection is Unix-specific");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString namespacePath = dir.filePath(
        QStringLiteral(".gc-transactions/linked-save"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QString journalId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    const QString journalPath = QDir(namespacePath).filePath(journalId);
    QVERIFY(QDir().mkpath(journalPath));
    QVERIFY(QFile::setPermissions(
        journalPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    writeFixture(
        QDir(journalPath).filePath(QStringLiteral("source-0000.old")),
        QByteArray("pre-manifest transaction data"));

    anchoredFilesystemSyncFailurePath = namespacePath;
    QString error;
    QVERIFY2(
        !LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        "A nondurable pre-manifest removal was accepted");
    QVERIFY2(!error.isEmpty(), "Nondurable removal must report an error");
    QVERIFY(!QFileInfo::exists(journalPath));

    error.clear();
    QVERIFY2(
        !LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        "Recovery forgot the pending namespace synchronization");
    QVERIFY2(!error.isEmpty(), "Pending synchronization must report an error");

    anchoredFilesystemSyncFailurePath.clear();
    error.clear();
    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error),
        qPrintable(error));
#endif
}

void TestAtomicActivitySave::
linkedSaveRecoveryRejectsStagedSourceAtOldName()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());

    const QString sourcePath = specification.entries.at(0).sourcePath;
    writeFixture(sourcePath, firstStaged);
    journal.reset();

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);

    QVERIFY2(!reconciled, "Recovery retired an external staged generation");
    QVERIFY2(!error.isEmpty(), "Rejected recovery must report an error");
    QCOMPARE(readAll(sourcePath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
}

void TestAtomicActivitySave::
linkedSaveRecoveryRejectsRecreatedOriginalSource_data()
{
    QTest::addColumn<bool>("conversion");

    QTest::newRow("rename") << false;
    QTest::newRow("conversion") << true;
}

void TestAtomicActivitySave::
linkedSaveRecoveryRejectsRecreatedOriginalSource()
{
    QFETCH(bool, conversion);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    if (conversion) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(
            specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());

    const QString sourcePath = specification.entries.at(0).sourcePath;
    writeFixture(sourcePath, originalSource);
    journal.reset();

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);

    QVERIFY2(
        !reconciled,
        "Committed recovery retired a recreated original source");
    QVERIFY2(!error.isEmpty(), "Rejected recovery must report an error");
    QCOMPARE(readAll(sourcePath), originalSource);
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
}

void TestAtomicActivitySave::
linkedSaveRecoverySourceRetirementSubstitutionIsRejected_data()
{
    QTest::addColumn<bool>("conversion");

    QTest::newRow("rename") << false;
    QTest::newRow("conversion") << true;
}

void TestAtomicActivitySave::
linkedSaveRecoverySourceRetirementSubstitutionIsRejected()
{
    QFETCH(bool, conversion);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    const QByteArray originalSource("first old generation");
    const QByteArray secondSource("second old generation");
    const QByteArray previousBackup("previous imported backup");
    const QByteArray sentinel("concurrent recovery source sentinel");
    if (conversion) {
        specification.entries[0].sourcePath =
            dir.filePath(QStringLiteral("import.fit"));
        specification.entries[0].backupPath =
            dir.filePath(QStringLiteral("import.fit.bak"));
        specification.entries[0].keepSourceBackup = true;
        writeFixture(
            specification.entries.at(0).sourcePath, originalSource);
        writeFixture(
            specification.entries.at(0).backupPath, previousBackup);
        writeFixture(
            specification.entries.at(1).sourcePath, secondSource);
    } else {
        writeLinkedSaveJournalSources(dir.path());
    }

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());

    const QString sourcePath = specification.entries.at(0).sourcePath;
    const QString retainedPath = sourcePath + QStringLiteral(".retained");
    writeFixture(sourcePath, originalSource);
    journal.reset();
    bool hookReached = false;
    bool sourceRenamed = false;
    bool sentinelWritten = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-recovery-source-retirement-validated"),
        [&]() {
            hookReached = true;
            sourceRenamed = QFile::rename(sourcePath, retainedPath);
            if (!sourceRenamed) return;
            QFile replacement(sourcePath);
            sentinelWritten = replacement.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && replacement.write(sentinel) == sentinel.size()
                && replacement.flush();
        });

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(!hookReached);
    QVERIFY(!sourceRenamed);
    QVERIFY(!sentinelWritten);
    QVERIFY2(!reconciled, "Recovery deleted a substituted source pathname");
    QVERIFY2(!error.isEmpty(), "Rejected recovery must report an error");
    QCOMPARE(readAll(sourcePath), originalSource);
    QVERIFY(!QFileInfo::exists(retainedPath));
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
    if (conversion) {
        QCOMPARE(
            readAll(specification.entries.at(0).backupPath),
            originalSource);
        QCOMPARE(
            readAll(QDir(journalPath).filePath(
                QStringLiteral("backup-0000.old"))),
            previousBackup);
    }
}

void TestAtomicActivitySave::
linkedSaveRecoveryRepopulationStopsFurtherRetirement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QByteArray firstStaged("staged generation 0");
    const QByteArray secondStaged("staged generation 1");
    const QByteArray sentinel("repopulated recovery source sentinel");
    writeFixture(journal->stagingPath(0), firstStaged);
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    writeFixture(journal->stagingPath(1), secondStaged);
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());
    writeFixture(
        specification.entries.at(0).sourcePath,
        QByteArray("first old generation"));
    writeFixture(
        specification.entries.at(1).sourcePath,
        QByteArray("second old generation"));
    journal.reset();

    bool hookReached = false;
    bool sentinelWritten = false;
    setLinkedActivitySaveTransitionAction(
        QByteArray("linked-save-recovery-source-retired"),
        [&]() {
            hookReached = true;
            QFile replacement(specification.entries.at(0).sourcePath);
            sentinelWritten = replacement.open(
                    QIODevice::WriteOnly | QIODevice::Truncate)
                && replacement.write(sentinel) == sentinel.size()
                && replacement.flush();
        });

    error.clear();
    const bool reconciled =
        LinkedActivitySave::Journal::reconcileAll(dir.path(), error);
    clearLinkedActivitySaveTransitionAction();

    QVERIFY(!hookReached);
    QVERIFY(!sentinelWritten);
    QVERIFY(!reconciled);
    QVERIFY2(!error.isEmpty(), "Repopulation must report an error");
    QCOMPARE(
        readAll(specification.entries.at(0).sourcePath),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(specification.entries.at(1).sourcePath),
        QByteArray("second old generation"));
    QCOMPARE(readAll(specification.entries.at(0).targetPath), firstStaged);
    QCOMPARE(readAll(specification.entries.at(1).targetPath), secondStaged);
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY(QFileInfo::exists(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED"))));
}

void TestAtomicActivitySave::
linkedSavePublicationPreservesExternalChanges_data()
{
    QTest::addColumn<QString>("role");

    QTest::newRow("source") << QStringLiteral("source");
    QTest::newRow("target") << QStringLiteral("target");
    QTest::newRow("backup") << QStringLiteral("backup");
}

void TestAtomicActivitySave::
linkedSavePublicationPreservesExternalChanges()
{
    QFETCH(QString, role);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    if (role == QStringLiteral("backup")) {
        writeFixture(
            specification.entries.at(0).backupPath,
            QByteArray("previous backup"));
    }

    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    for (int index = 0; index < journal->entryCount(); ++index) {
        writeFixture(
            journal->stagingPath(index),
            QByteArray("staged generation ")
                + QByteArray::number(index));
        QVERIFY2(journal->recordStaged(index, error), qPrintable(error));
    }

    const LinkedActivitySave::EntrySpecification &first =
        specification.entries.at(0);
    const QString changedPath = role == QStringLiteral("source")
        ? first.sourcePath
        : role == QStringLiteral("target")
            ? first.targetPath
            : first.backupPath;
    const QByteArray external("concurrent external contents");
    writeFixture(changedPath, external);

    error.clear();
    QVERIFY(!journal->publishAndCommit(error));
    QVERIFY2(!error.isEmpty(), qPrintable(role));
    QCOMPARE(readAll(changedPath), external);
    QVERIFY(!journal->hasCommitMarker());

    error.clear();
    QVERIFY(!journal->cleanupAfterRollback(error));
    QVERIFY2(!error.isEmpty(), qPrintable(role));
    QCOMPARE(readAll(changedPath), external);
    QVERIFY(QFileInfo::exists(journalPath));
}

void TestAtomicActivitySave::
linkedSaveRecoveryRejectsUnsafeJournalEntries_data()
{
    QTest::addColumn<QString>("entryCase");

    QTest::newRow("unknown-file") << QStringLiteral("unknown-file");
    QTest::newRow("nested-directory") << QStringLiteral("nested-directory");
    QTest::newRow("symbolic-link") << QStringLiteral("symbolic-link");
    QTest::newRow("malformed-manifest")
        << QStringLiteral("malformed-manifest");
    QTest::newRow("manifest-unknown-key")
        << QStringLiteral("manifest-unknown-key");
    QTest::newRow("manifest-path-traversal")
        << QStringLiteral("manifest-path-traversal");
    QTest::newRow("manifest-duplicate-target")
        << QStringLiteral("manifest-duplicate-target");
    QTest::newRow("manifest-id-mismatch")
        << QStringLiteral("manifest-id-mismatch");
    QTest::newRow("invalid-commit-marker")
        << QStringLiteral("invalid-commit-marker");
    QTest::newRow("tampered-source-copy")
        << QStringLiteral("tampered-source-copy");
    QTest::newRow("tampered-staged-file")
        << QStringLiteral("tampered-staged-file");
}

void TestAtomicActivitySave::
linkedSaveRecoveryRejectsUnsafeJournalEntries()
{
    QFETCH(QString, entryCase);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const LinkedActivitySave::Specification specification =
        linkedSaveJournalSpecification(dir.path());
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();

    if (entryCase == QStringLiteral("unknown-file")) {
        writeFixture(
            QDir(journalPath).filePath(QStringLiteral("attacker.data")),
            QByteArray("unknown"));
    } else if (entryCase == QStringLiteral("nested-directory")) {
        QVERIFY(QDir().mkdir(
            QDir(journalPath).filePath(QStringLiteral("nested"))));
    } else if (entryCase == QStringLiteral("symbolic-link")) {
        const QString sentinel = dir.filePath(QStringLiteral("sentinel"));
        const QString linked = QDir(journalPath).filePath(
            QStringLiteral("linked-entry"));
        writeFixture(sentinel, QByteArray("sentinel"));
        if (!QFile::link(sentinel, linked)) {
            QSKIP("Symbolic links are unavailable");
        }
    } else if (entryCase == QStringLiteral("malformed-manifest")) {
        writeFixture(
            QDir(journalPath).filePath(QStringLiteral("manifest.json")),
            QByteArray("{}\n"));
    } else if (entryCase.startsWith(QStringLiteral("manifest-"))) {
        const QString manifestPath = QDir(journalPath).filePath(
            QStringLiteral("manifest.json"));
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            readAll(manifestPath), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        QJsonObject root = document.object();
        if (entryCase == QStringLiteral("manifest-unknown-key")) {
            root.insert(QStringLiteral("unexpected"), true);
        } else if (entryCase
                   == QStringLiteral("manifest-path-traversal")) {
            QJsonArray entries =
                root.value(QStringLiteral("entries")).toArray();
            QJsonObject entry = entries.at(0).toObject();
            QJsonObject source =
                entry.value(QStringLiteral("source")).toObject();
            source.insert(
                QStringLiteral("path"),
                QStringLiteral("../outside.json"));
            entry.insert(QStringLiteral("source"), source);
            entries.replace(0, entry);
            root.insert(QStringLiteral("entries"), entries);
        } else if (entryCase
                   == QStringLiteral("manifest-duplicate-target")) {
            QJsonArray entries =
                root.value(QStringLiteral("entries")).toArray();
            QJsonObject second = entries.at(1).toObject();
            second.insert(
                QStringLiteral("target"),
                entries.at(0).toObject().value(
                    QStringLiteral("target")));
            entries.replace(1, second);
            root.insert(QStringLiteral("entries"), entries);
        } else if (entryCase
                   == QStringLiteral("manifest-id-mismatch")) {
            root.insert(
                QStringLiteral("id"),
                QUuid::createUuid()
                    .toString(QUuid::WithoutBraces)
                    .toLower());
        }
        QByteArray contents =
            QJsonDocument(root).toJson(QJsonDocument::Compact);
        contents.append('\n');
        writeFixture(manifestPath, contents);
    } else if (entryCase == QStringLiteral("invalid-commit-marker")) {
        writeFixture(
            QDir(journalPath).filePath(QStringLiteral("COMMITTED")),
            QByteArray("not committed\n"));
    } else if (entryCase == QStringLiteral("tampered-source-copy")) {
        writeFixture(
            QDir(journalPath).filePath(QStringLiteral("source-0000.old")),
            QByteArray("tampered source"));
    } else if (entryCase == QStringLiteral("tampered-staged-file")) {
        writeFixture(journal->stagingPath(0), QByteArray("staged"));
        QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
        writeFixture(journal->stagingPath(0), QByteArray("tampered stage"));
    }
    journal.reset();

    error.clear();
    QVERIFY(!LinkedActivitySave::Journal::reconcileAll(dir.path(), error));
    QVERIFY2(!error.isEmpty(), qPrintable(entryCase));
    QVERIFY(QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("first-old.json"))),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("second-old.json"))),
        QByteArray("second old generation"));
}

void TestAtomicActivitySave::
linkedSavePreManifestRecoveryRejectsUnknownEntries_data()
{
    QTest::addColumn<QString>("fileName");

    QTest::newRow("embedded-manifest-name")
        << QStringLiteral("attacker-manifest.json-data");
    QTest::newRow("invalid-manifest-temporary")
        << QStringLiteral(".manifest.json.attack.tmp.extra");
    QTest::newRow("retirement-intent")
        << QStringLiteral("retirement-0000.pending");
    QTest::newRow("retirement-intent-temporary")
        << QStringLiteral(".retirement-0000.pending.ABC123.tmp");
}

void TestAtomicActivitySave::
linkedSavePreManifestRecoveryRejectsUnknownEntries()
{
    QFETCH(QString, fileName);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            linkedSaveJournalSpecification(dir.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    const QString unknownPath = QDir(journalPath).filePath(fileName);
    journal.reset();
    QVERIFY(QFile::remove(manifestPath));
    writeFixture(unknownPath, QByteArray("unknown pre-manifest entry"));

    error.clear();
    QVERIFY(!LinkedActivitySave::Journal::reconcileAll(dir.path(), error));
    QVERIFY2(
        error.contains(QStringLiteral("unknown"), Qt::CaseInsensitive),
        qPrintable(error));
    QVERIFY(QFileInfo::exists(journalPath));
    QCOMPARE(
        readAll(unknownPath),
        QByteArray("unknown pre-manifest entry"));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("first-old.json"))),
        QByteArray("first old generation"));
    QCOMPARE(
        readAll(dir.filePath(QStringLiteral("second-old.json"))),
        QByteArray("second old generation"));
}

void TestAtomicActivitySave::
linkedSaveOversizedControlFileFailsBeforeRead_data()
{
    QTest::addColumn<QString>("relativePath");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<bool>("removeManifest");

    QTest::newRow("manifest")
        << QStringLiteral("manifest.json")
        << static_cast<qint64>(4 * 1024 * 1024 + 1) << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("COMMITTED")
        << static_cast<qint64>(129) << false;
    QTest::newRow("manifest-temporary")
        << QStringLiteral(".manifest.json.attack.tmp")
        << static_cast<qint64>(4 * 1024 * 1024 + 1) << false;
    QTest::newRow("commit-marker-temporary")
        << QStringLiteral(".COMMITTED.attack.tmp")
        << static_cast<qint64>(129) << false;
    QTest::newRow("retirement-intent")
        << QStringLiteral("retirement-0000.pending")
        << static_cast<qint64>(129) << false;
    QTest::newRow("manifest-lock")
        << QStringLiteral(".manifest.json.lock")
        << static_cast<qint64>(64 * 1024 + 1) << false;
    QTest::newRow("pre-manifest-commit-marker")
        << QStringLiteral("COMMITTED")
        << static_cast<qint64>(129) << true;
    QTest::newRow("pre-manifest-temporary")
        << QStringLiteral(".manifest.json.attack.tmp")
        << static_cast<qint64>(4 * 1024 * 1024 + 1) << true;
}

void TestAtomicActivitySave::
linkedSaveOversizedControlFileFailsBeforeRead()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix file permissions are required");
#else
    QFETCH(QString, relativePath);
    QFETCH(qint64, size);
    QFETCH(bool, removeManifest);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            linkedSaveJournalSpecification(dir.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const QString controlPath = QDir(journalPath).filePath(relativePath);
    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    journal.reset();
    if (removeManifest) QVERIFY(QFile::remove(manifestPath));

    QFile control(controlPath);
    QVERIFY2(
        control.open(QIODevice::WriteOnly | QIODevice::Truncate),
        qPrintable(control.errorString()));
    QVERIFY(control.resize(size));
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
    QVERIFY(!LinkedActivitySave::Journal::reconcileAll(dir.path(), error));
    QVERIFY2(
        error.contains(QStringLiteral("unexpectedly large")),
        qPrintable(error));
    QVERIFY(QFileInfo::exists(journalPath));

    QFile::setPermissions(
        controlPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

void TestAtomicActivitySave::
linkedSaveTransactionDirectoriesArePrivate()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    const QString transactionsPath = dir.filePath(
        QStringLiteral(".gc-transactions"));
    const QString namespacePath = QDir(transactionsPath).filePath(
        QStringLiteral("linked-save"));
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
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            linkedSaveJournalSpecification(dir.path()), error);
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

void TestAtomicActivitySave::
linkedSaveRecoveryRestrictsExistingDirectories()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeLinkedSaveJournalSources(dir.path());
    QString error;
    std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            linkedSaveJournalSpecification(dir.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    journal.reset();

    const QString transactionsPath = dir.filePath(
        QStringLiteral(".gc-transactions"));
    const QString namespacePath = QDir(transactionsPath).filePath(
        QStringLiteral("linked-save"));
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
    QVERIFY(!LinkedActivitySave::Journal::reconcileAll(dir.path(), error));
    QVERIFY2(!error.isEmpty(), "A corrupt manifest must stop recovery");

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

void TestAtomicActivitySave::
linkedSaveRecoveryWithoutJournalAllowsSymlinkedRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString actualRoot = dir.filePath(
        QStringLiteral("actual-athlete"));
    const QString linkedRoot = dir.filePath(
        QStringLiteral("linked-athlete"));
    QVERIFY(QDir().mkpath(actualRoot));
    if (!QFile::link(actualRoot, linkedRoot)) {
        QSKIP("Directory symbolic links are unavailable");
    }

    QString error;
    QVERIFY2(
        LinkedActivitySave::Journal::reconcileAll(
            linkedRoot, error),
        qPrintable(error));
}

void TestAtomicActivitySave::
linkedFilenameSaveCrashRecoversCompleteGeneration_data()
{
    QTest::addColumn<QString>("crashPhase");
    QTest::addColumn<int>("crashOccurrence");
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("conversion");
    QTest::addColumn<bool>("manualRecovery");

    QTest::newRow("directory-created")
        << QStringLiteral("linked-save-directory-created")
        << 1 << false << false << false;
    QTest::newRow("source-copy-one")
        << QStringLiteral("linked-save-source-copy-published")
        << 1 << false << false << false;
    QTest::newRow("source-copy-two")
        << QStringLiteral("linked-save-source-copy-published")
        << 2 << false << false << false;
    QTest::newRow("initial-manifest")
        << QStringLiteral("linked-save-initial-manifest-published")
        << 1 << false << false << false;
    QTest::newRow("stage-one")
        << QStringLiteral("linked-save-stage-recorded")
        << 1 << false << false << false;
    QTest::newRow("stage-two")
        << QStringLiteral("linked-save-stage-recorded")
        << 2 << false << false << false;
    QTest::newRow("target-one")
        << QStringLiteral("linked-save-target-published")
        << 1 << false << false << false;
    QTest::newRow("target-two")
        << QStringLiteral("linked-save-target-published")
        << 2 << false << false << false;
    QTest::newRow("retirement-intent-published")
        << QStringLiteral("linked-save-retirement-intent-published")
        << 1 << false << false << true;
    QTest::newRow("source-retired-one")
        << QStringLiteral("linked-save-source-retired")
        << 1 << false << false << true;
    QTest::newRow("source-retired-two")
        << QStringLiteral("linked-save-source-retired")
        << 2 << false << false << true;
    QTest::newRow("retirement-intent-removed")
        << QStringLiteral("linked-save-retirement-intent-removed")
        << 1 << false << false << false;
    QTest::newRow("commit-marker-temporary")
        << QStringLiteral("linked-save-commit-marker-temporary")
        << 1 << false << false << false;
    QTest::newRow("commit-marker")
        << QStringLiteral("linked-save-commit-marker")
        << 1 << true << false << false;
    QTest::newRow("manifest-removed")
        << QStringLiteral("linked-save-manifest-removed")
        << 1 << true << false << false;
    for (int occurrence = 1; occurrence <= 4; ++occurrence) {
        QTest::newRow(qPrintable(QStringLiteral("cleanup-%1").arg(occurrence)))
            << QStringLiteral("linked-save-cleanup-file")
            << occurrence << true << false << false;
    }
    QTest::newRow("commit-marker-removed")
        << QStringLiteral("linked-save-commit-marker-removed")
        << 1 << true << false << false;
    QTest::newRow("directory-removed")
        << QStringLiteral("linked-save-directory-removed")
        << 1 << true << false << false;
    QTest::newRow("conversion-backup-copy")
        << QStringLiteral("linked-save-backup-copy-published")
        << 1 << false << true << false;
    QTest::newRow("conversion-backup-published")
        << QStringLiteral("linked-save-backup-published")
        << 1 << false << true << false;
    QTest::newRow("conversion-source-retired")
        << QStringLiteral("linked-save-source-retired")
        << 1 << false << true << true;
    QTest::newRow("conversion-commit-marker")
        << QStringLiteral("linked-save-commit-marker")
        << 1 << true << true << false;
}

void TestAtomicActivitySave::
linkedFilenameSaveCrashRecoversCompleteGeneration()
{
    QFETCH(QString, crashPhase);
    QFETCH(int, crashOccurrence);
    QFETCH(bool, committed);
    QFETCH(bool, conversion);
    QFETCH(bool, manualRecovery);

    static const char RootEnvironment[] =
        "GC_LINKED_ACTIVITY_SAVE_CRASH_ROOT";
    static const char ModeEnvironment[] =
        "GC_LINKED_ACTIVITY_SAVE_CRASH_MODE";
    const QString root = qEnvironmentVariable(RootEnvironment);
    const QString mode = qEnvironmentVariable(ModeEnvironment);

    const QDateTime firstOldTime(QDate(2026, 7, 6), QTime(8, 30));
    const QDateTime secondOldTime(QDate(2026, 7, 7), QTime(9, 45));
    const QDateTime firstNewTime(QDate(2026, 7, 8), QTime(10, 15));
    const QDateTime secondNewTime(QDate(2026, 7, 9), QTime(11, 20));
    const QString firstOldName = conversion
        ? QStringLiteral("import.fit")
        : activityFileName(firstOldTime, QStringLiteral("json"));
    const QString secondOldName =
        activityFileName(secondOldTime, QStringLiteral("json"));
    const QString firstNewName =
        activityFileName(firstNewTime, QStringLiteral("json"));
    const QString secondNewName =
        activityFileName(secondNewTime, QStringLiteral("json"));

    if (!root.isEmpty()) {
        if (mode == QStringLiteral("recover")) {
            QString recoveryError;
            if (!LinkedActivitySave::Journal::reconcileAll(
                    root, recoveryError)) {
                std::_Exit(87);
            }
            return;
        }

        RideFile firstRide(firstNewTime, 1.0);
        RideFile secondRide(secondNewTime, 1.0);
        RideItem firstItem(&firstRide, nullptr);
        RideItem secondItem(&secondRide, nullptr);
        firstItem.path = root;
        firstItem.fileName = firstOldName;
        secondItem.path = root;
        secondItem.fileName = secondOldName;
        secondItem.planned = true;
        firstItem.setLinkedFileName(secondNewName);
        secondItem.setLinkedFileName(firstNewName);
        firstItem.setDirty(true);
        secondItem.setDirty(true);
        QString saveError;
        MainWindow::saveLinkedActivitiesTransaction(
            nullptr,
            root,
            { &firstItem, &secondItem },
            saveError,
            ActivitySaveOperationsProvider());
        QFAIL("The linked-save child did not stop at the requested transition");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    JsonFileReader json;
    QString error;
    RideFile secondOldRide(secondOldTime, 1.0);
    secondOldRide.setTag(QStringLiteral("Linked Filename"), firstOldName);
    const QByteArray importedOriginal("original imported activity");
    const QByteArray previousBackup("previous imported backup");
    if (conversion) {
        writeFixture(
            temporary.filePath(firstOldName), importedOriginal);
        writeFixture(
            temporary.filePath(firstOldName) + QStringLiteral(".bak"),
            previousBackup);
    } else {
        RideFile firstOldRide(firstOldTime, 1.0);
        firstOldRide.setTag(
            QStringLiteral("Linked Filename"), secondOldName);
        QFile firstOldFile(temporary.filePath(firstOldName));
        QVERIFY2(json.writeRideFile(
                     nullptr, &firstOldRide, firstOldFile, error),
                 qPrintable(error));
    }
    QFile secondOldFile(temporary.filePath(secondOldName));
    QVERIFY2(json.writeRideFile(
                 nullptr, &secondOldRide, secondOldFile, error),
             qPrintable(error));

    const auto runChild = [&](const QString &childMode,
                              const QString &requestedPhase) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral(
            "GC_LINKED_ACTIVITY_SAVE_CRASH_PHASE"));
        environment.remove(QStringLiteral(
            "GC_LINKED_ACTIVITY_SAVE_CRASH_OCCURRENCE"));
        environment.insert(
            QString::fromLatin1(RootEnvironment), temporary.path());
        environment.insert(
            QString::fromLatin1(ModeEnvironment), childMode);
        environment.insert(
            QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        if (!requestedPhase.isEmpty()) {
            environment.insert(
                QStringLiteral("GC_LINKED_ACTIVITY_SAVE_CRASH_PHASE"),
                requestedPhase);
            environment.insert(
                QStringLiteral("GC_LINKED_ACTIVITY_SAVE_CRASH_OCCURRENCE"),
                QString::number(crashOccurrence));
        }
        child.setProcessEnvironment(environment);
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral(
                "linkedFilenameSaveCrashRecoversCompleteGeneration:%1")
                 .arg(QString::fromLatin1(QTest::currentDataTag()))});
        if (!child.waitForStarted(5000)) {
            return qMakePair(-1, child.errorString());
        }
        if (!child.waitForFinished(15000)) {
            child.kill();
            child.waitForFinished();
            return qMakePair(-2, QStringLiteral("child timed out"));
        }
        return qMakePair(
            child.exitCode(), QString::fromUtf8(child.readAll()));
    };

    const auto crashed = runChild(QStringLiteral("crash"), crashPhase);
    QCOMPARE(crashed.first, 86);
    const QString firstOldPath = temporary.filePath(firstOldName);
    const QString secondOldPath = temporary.filePath(secondOldName);
    const QString firstNewPath = temporary.filePath(firstNewName);
    const QString secondNewPath = temporary.filePath(secondNewName);
    const QStringList productionPaths = {
        firstOldPath, secondOldPath, firstNewPath, secondNewPath};
    const auto productionState = [&]() {
        QList<QPair<bool, QByteArray>> state;
        for (const QString &path : productionPaths) {
            const bool exists = QFileInfo::exists(path);
            state.append(qMakePair(
                exists, exists ? readAll(path) : QByteArray()));
        }
        return state;
    };
    const QList<QPair<bool, QByteArray>> beforeRecovery =
        productionState();
    const QDir journalRoot(temporary.filePath(
        QStringLiteral(".gc-transactions/linked-save")));

    const auto recovered = runChild(QStringLiteral("recover"), QString());
    if (manualRecovery) {
        QCOMPARE(recovered.first, 87);
        QVERIFY(productionState() == beforeRecovery);
        const QStringList journals = journalRoot.entryList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(journals.size(), 1);
        const QDir journal(journalRoot.filePath(journals.constFirst()));
        const QStringList intents = journal.entryList(
            {QStringLiteral("retirement-*.pending")}, QDir::Files);
        QCOMPARE(intents.size(), 1);
        QVERIFY(!QFileInfo::exists(
            journal.filePath(QStringLiteral("COMMITTED"))));
        const QString intentPath =
            journal.filePath(intents.constFirst());
        const QByteArray intentContents = readAll(intentPath);

        const auto recoveredAgain = runChild(
            QStringLiteral("recover"), QString());
        QCOMPARE(recoveredAgain.first, 87);
        QVERIFY(productionState() == beforeRecovery);
        QCOMPARE(readAll(intentPath), intentContents);
        QVERIFY(QFileInfo::exists(journal.absolutePath()));
        return;
    }
    QCOMPARE(recovered.first, 0);

    if (committed) {
        QVERIFY(!QFileInfo::exists(firstOldPath));
        QVERIFY(!QFileInfo::exists(secondOldPath));
        QVERIFY(QFileInfo::exists(firstNewPath));
        QVERIFY(QFileInfo::exists(secondNewPath));
        if (conversion) {
            QCOMPARE(
                readAll(firstOldPath + QStringLiteral(".bak")),
                importedOriginal);
        }
    } else {
        QVERIFY(QFileInfo::exists(firstOldPath));
        QVERIFY(QFileInfo::exists(secondOldPath));
        QVERIFY(!QFileInfo::exists(firstNewPath));
        QVERIFY(!QFileInfo::exists(secondNewPath));
        if (conversion) {
            QCOMPARE(readAll(firstOldPath), importedOriginal);
            QCOMPARE(
                readAll(firstOldPath + QStringLiteral(".bak")),
                previousBackup);
        }
    }

    const auto linkedFilename = [&](const QString &path) {
        QStringList parseErrors;
        QFile input(path);
        std::unique_ptr<RideFile> parsed(
            json.openRideFile(input, parseErrors));
        if (!parsed) return QString();
        return parsed->getTag(QStringLiteral("Linked Filename"), QString());
    };
    const QString firstPath = committed ? firstNewPath : firstOldPath;
    const QString secondPath = committed ? secondNewPath : secondOldPath;
    if (committed || !conversion) {
        QCOMPARE(
            linkedFilename(firstPath),
            committed ? secondNewName : secondOldName);
    }
    QCOMPARE(
        linkedFilename(secondPath),
        committed ? firstNewName : firstOldName);
    const QByteArray firstGeneration = readAll(firstPath);
    const QByteArray secondGeneration = readAll(secondPath);

    const auto recoveredAgain = runChild(
        QStringLiteral("recover"), QString());
    QCOMPARE(recoveredAgain.first, 0);
    QCOMPARE(readAll(firstPath), firstGeneration);
    QCOMPARE(readAll(secondPath), secondGeneration);

    QVERIFY(!journalRoot.exists()
        || journalRoot.entryList(
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::mainWindowRejectsTargetCollision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("import.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray sourceBytes("source activity");
    const QByteArray targetBytes("unrelated existing activity");
    writeFixture(sourcePath, sourceBytes);
    writeFixture(targetPath, targetBytes);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    int writerCreations = 0;
    int stageRuns = 0;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            ++writerCreations;
            return qSaveFileWriterFactory()(path, mode);
        };
    operations.stage = [&](RideFile *, QString &) {
        ++stageRuns;
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(error.contains(QStringLiteral("already exists"),
                           Qt::CaseInsensitive));
    QCOMPARE(writerCreations, 0);
    QCOMPARE(stageRuns, 0);
    QCOMPARE(readAll(sourcePath), sourceBytes);
    QCOMPARE(readAll(targetPath), targetBytes);
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::mainWindowHoldsSourceAndTargetLocks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("locked.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    writeFixture(sourcePath, QByteArray("source activity"));

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool sourceWasLocked = false;
    bool targetWasLocked = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.stage = [&](RideFile *, QString &) {
        QLockFile sourceLock(atomicFileLockPath(sourcePath));
        sourceLock.setStaleLockTime(0);
        sourceWasLocked = !sourceLock.tryLock(0);
        QLockFile targetLock(atomicFileLockPath(targetPath));
        targetLock.setStaleLockTime(0);
        targetWasLocked = !targetLock.tryLock(0);
        return true;
    };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(sourceWasLocked);
    QVERIFY(targetWasLocked);
}

void TestAtomicActivitySave::
mainWindowFinalizeFailureRemainsRetryable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("retry.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("source activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool removeSourceAfterCommit = true;
    bool sourceWasRemoved = false;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None, [&]() {
                        if (removeSourceAfterCommit) {
                            sourceWasRemoved = QFile::remove(sourcePath);
                        }
                    }));
        };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(sourceWasRemoved);
    QVERIFY(!error.isEmpty());
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(!QFile::exists(targetPath));

    writeFixture(sourcePath, original);
    removeSourceAfterCommit = false;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
}

void TestAtomicActivitySave::mainWindowRejectsSourceChangedDuringSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("changed.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("original source activity");
    const QByteArray concurrent("concurrently changed source");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool sourceWasChanged = false;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None, [&]() {
                        QFile source(sourcePath);
                        sourceWasChanged =
                            source.open(QIODevice::WriteOnly
                                        | QIODevice::Truncate)
                            && source.write(concurrent)
                                == concurrent.size()
                            && source.flush();
                    }));
        };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(sourceWasChanged);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(sourcePath), concurrent);
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QVERIFY(!QFile::exists(targetPath));
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::mainWindowSaveSilentPropagatesFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString path = dir.filePath(fileName);
    const QString notesPath =
        dir.filePath(QFileInfo(fileName).baseName()
                     + QStringLiteral(".notes"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);
    writeFixture(notesPath, QByteArray("notes"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    int stageRuns = 0;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::Commit));
        };
    operations.stage = [&](RideFile *activity, QString &) {
        ++stageRuns;
        activity->setTag(QStringLiteral("State"), QStringLiteral("processed"));
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(error.contains(QStringLiteral("commit"), Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
    QVERIFY(QFile::exists(notesPath));
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, fileName);
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));
    QCOMPARE(stageRuns, 1);
}

void TestAtomicActivitySave::
mainWindowSaveSilentPreservesUppercaseJsonPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("JSON"));
    const QString sourcePath = dir.filePath(fileName);
    writeFixture(sourcePath, QByteArray("old activity"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    int stageRuns = 0;
    bool stageSawLiveRide = false;
    QString writerPath;
    AtomicFileMode writerMode = AtomicFileMode::CreateNew;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerPath = path;
            writerMode = mode;
            return realWriter(path, mode);
        };
    operations.stage = [&](RideFile *activity, QString &) {
        ++stageRuns;
        stageSawLiveRide = activity == &ride;
        activity->setTag(QStringLiteral("State"), QStringLiteral("processed"));
        return true;
    };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(item.fileName, fileName);
    QVERIFY(QFile::exists(sourcePath));
    QCOMPARE(writerPath, sourcePath);
    QCOMPARE(writerMode, AtomicFileMode::ReplaceExisting);
    QVERIFY(!item.isDirty());
    QCOMPARE(stageRuns, 1);
    QVERIFY(stageSawLiveRide);
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(sourcePath);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));
}

void TestAtomicActivitySave::
mainWindowSaveRideSingleDialogPropagatesResult()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RideItem item(nullptr, nullptr);
    item.path = dir.path();
    item.fileName = QStringLiteral("activity.gc");

    int saveCalls = 0;
    bool receivedExpectedActivity = false;
    QString reportedError;
    MainWindow::SaveRideDialogOperations operations;
    operations.saveActivities =
        [&](const QList<RideItem *> &items, QString &) {
            ++saveCalls;
            receivedExpectedActivity =
                items.size() == 1 && items.first() == &item;
            return true;
        };
    operations.reportError =
        [&](const QString &error) { reportedError = error; };

    QVERIFY(MainWindow::saveRideSingleDialog(
        nullptr, &item, &operations));
    QCOMPARE(saveCalls, 1);
    QVERIFY(receivedExpectedActivity);
    QVERIFY(reportedError.isEmpty());

    operations.saveActivities =
        [&](const QList<RideItem *> &, QString &error) {
            ++saveCalls;
            error = QStringLiteral("injected collection save failure");
            return false;
        };
    QVERIFY(!MainWindow::saveRideSingleDialog(
        nullptr, &item, &operations));
    QCOMPARE(saveCalls, 2);
    QCOMPARE(reportedError,
             QStringLiteral("injected collection save failure"));
}

void TestAtomicActivitySave::
preflightSaveRelinksCompleteActivitySetBeforeSaving()
{
    RideItem target(nullptr, nullptr);
    target.fileName = QStringLiteral("target.fit");
    target.setDirty(true);
    RideItem linked(nullptr, nullptr);
    linked.fileName = QStringLiteral("linked.fit");

    QStringList events;
    QList<RideItem*> savedItems;
    bool receivedExpectedTarget = false;
    QString error;
    const bool saved = saveOperationPreflightActivities(
        guardOperationPreflightItems(
            QList<RideItem*>{&target}),
        [&](RideItem *item,
            GuardedOperationPreflightItems &items,
            QString &) {
            events.append(QStringLiteral("relink"));
            receivedExpectedTarget =
                item == &target;
            target.setLinkedFileName(
                QStringLiteral("linked.json"));
            linked.setLinkedFileName(
                QStringLiteral("target.json"));
            items.append(
                GuardedOperationPreflightItem(&linked));
            items.append(
                GuardedOperationPreflightItem(&target));
            return true;
        },
        [&](const QList<RideItem*> &items,
            QString &) {
            events.append(QStringLiteral("save"));
            savedItems = items;
            return true;
        },
        error);

    QVERIFY(saved);
    QVERIFY(error.isEmpty());
    QVERIFY(receivedExpectedTarget);
    QCOMPARE(
        events,
        QStringList({
            QStringLiteral("relink"),
            QStringLiteral("save")}));
    QCOMPARE(
        savedItems,
        QList<RideItem*>({&target, &linked}));
}

void TestAtomicActivitySave::
preflightFindsLinkedPeerByPredictedFilename()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first-old.json");
    first.path = QStringLiteral("/activities");
    first.planned = false;
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second-old.json");
    second.path = QStringLiteral("/planned");
    second.planned = true;
    const QString firstNew = QStringLiteral("first-new.json");
    const QString secondNew = QStringLiteral("second-new.json");
    first.setLinkedFileName(secondNew);
    second.setLinkedFileName(firstNew);
    const GuardedOperationPreflightItems activities =
        guardOperationPreflightItems({&first, &second});
    const OperationPreflightFilenameChange predict =
        [&](RideItem *item, QString *newFilename) {
            if (newFilename) {
                *newFilename = item == &first
                    ? firstNew : secondNew;
            }
            return item == &first || item == &second;
        };

    QString error;
    QCOMPARE(
        findOperationPreflightLinkedActivity(
            &first, activities, predict, error),
        &second);
    QVERIFY(error.isEmpty());
    QCOMPARE(
        findOperationPreflightLinkedActivity(
            &second, activities, predict, error),
        &first);
    QVERIFY(error.isEmpty());

    RideItem ambiguous(nullptr, nullptr);
    ambiguous.fileName = QStringLiteral("third-old.json");
    ambiguous.path = QStringLiteral("/planned");
    ambiguous.planned = true;
    const GuardedOperationPreflightItems ambiguousActivities =
        guardOperationPreflightItems({&first, &second, &ambiguous});
    const OperationPreflightFilenameChange ambiguousPredict =
        [&](RideItem *item, QString *newFilename) {
            if (newFilename) {
                *newFilename = item == &first
                    ? firstNew : secondNew;
            }
            return true;
        };
    error.clear();
    QVERIFY(findOperationPreflightLinkedActivity(
                &first,
                ambiguousActivities,
                ambiguousPredict,
                error)
            == nullptr);
    QVERIFY2(
        error.contains(QStringLiteral("ambiguous"), Qt::CaseInsensitive),
        qPrintable(error));
}

void TestAtomicActivitySave::
preflightItemsRejectDestroyedActivity()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    const GuardedOperationPreflightItems guarded =
        guardOperationPreflightItems(
            QList<RideItem*>{item});
    delete item;

    QList<RideItem*> resolved;
    QString error;
    QVERIFY(!resolveOperationPreflightItems(
        guarded, resolved, error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestAtomicActivitySave::
preflightRelinkRejectsDestroyedActivity()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.fit");
    const GuardedOperationPreflightItems guarded =
        guardOperationPreflightItems(
            QList<RideItem*>{item});
    bool saveCalled = false;
    QString error;

    const bool saved = saveOperationPreflightActivities(
        guarded,
        [&](RideItem *,
            GuardedOperationPreflightItems &,
            QString &) {
            delete item;
            item = nullptr;
            return true;
        },
        [&](const QList<RideItem*> &, QString &) {
            saveCalled = true;
            return true;
        },
        error);

    QVERIFY(!saved);
    QVERIFY(!saveCalled);
    QVERIFY(!error.isEmpty());
}


void TestAtomicActivitySave::
preflightRejectsIdentityMutationDuringLaterRelink()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.fit");
    first.path = QStringLiteral("/activities");
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.fit");
    second.path = QStringLiteral("/activities");
    int relinkCalls = 0;
    bool saveCalled = false;
    QString error;

    const bool saved = saveOperationPreflightActivities(
        guardOperationPreflightItems(
            QList<RideItem*>{&first, &second}),
        [&](RideItem *,
            GuardedOperationPreflightItems &,
            QString &) {
            ++relinkCalls;
            if (relinkCalls == 2) {
                first.fileName =
                    QStringLiteral("replacement.fit");
            }
            return true;
        },
        [&](const QList<RideItem*> &, QString &) {
            saveCalled = true;
            return true;
        },
        error);

    QVERIFY(!saved);
    QCOMPARE(relinkCalls, 2);
    QVERIFY(!saveCalled);
    QVERIFY(error.contains(
        QStringLiteral("changed"),
        Qt::CaseInsensitive));
}

void TestAtomicActivitySave::
preflightRejectsDestroyedOwnerDuringRelink()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    QObject *owner = new QObject;
    QPointer<QObject> guardedOwner(owner);
    bool saveCalled = false;
    QString error;

    const bool saved = saveOperationPreflightActivities(
        guardOperationPreflightItems(
            QList<RideItem*>{&item}),
        [&](RideItem *,
            GuardedOperationPreflightItems &,
            QString &) {
            delete owner;
            owner = nullptr;
            return true;
        },
        [&](const QList<RideItem*> &, QString &) {
            saveCalled = true;
            return true;
        },
        error,
        [&](QString &validationError) {
            if (guardedOwner) return true;
            validationError = QStringLiteral(
                "operation owner was destroyed");
            return false;
        });

    QVERIFY(!saved);
    QVERIFY(!saveCalled);
    QCOMPARE(error, QStringLiteral(
        "operation owner was destroyed"));
}

void TestAtomicActivitySave::
preflightRejectsDestroyedOwnerDuringSave()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    QObject *owner = new QObject;
    QPointer<QObject> guardedOwner(owner);
    int saveCalls = 0;
    QString error;

    const bool saved = saveOperationPreflightActivities(
        guardOperationPreflightItems(
            QList<RideItem*>{&item}),
        [](RideItem *,
           GuardedOperationPreflightItems &,
           QString &) {
            return true;
        },
        [&](const QList<RideItem*> &, QString &) {
            ++saveCalls;
            delete owner;
            owner = nullptr;
            return true;
        },
        error,
        [&](QString &validationError) {
            if (guardedOwner) return true;
            validationError = QStringLiteral(
                "operation owner was destroyed");
            return false;
        });

    QVERIFY(!saved);
    QCOMPARE(saveCalls, 1);
    QCOMPARE(error, QStringLiteral(
        "operation owner was destroyed"));
}

void TestAtomicActivitySave::discardReloadFailureIsRejected()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    item.path = QStringLiteral("/activities");
    const GuardedOperationPreflightItems guardedItems =
        guardOperationPreflightItems({&item});
    int reloadCalls = 0;
    bool expectedItemSeen = false;
    QString error;

    QVERIFY(!reloadOperationPreflightActivities(
        guardedItems,
        [&](RideItem *reloadItem) -> RideFile * {
            expectedItemSeen = reloadItem == &item;
            ++reloadCalls;
            return nullptr;
        },
        error));
    QCOMPARE(reloadCalls, 1);
    QVERIFY(expectedItemSeen);
    QVERIFY(error.contains(
        QStringLiteral("reload"),
        Qt::CaseInsensitive));
}

void TestAtomicActivitySave::
discardReloadSurvivesActivityDestroyedByClose()
{
    QPointer<RideItem> item =
        new RideItem(nullptr, nullptr);
    setAtomicActivitySaveCloseAction(
        item.data(), [&] { delete item.data(); });

    RideFile *const reloaded =
        reloadDiscardedActivity(item.data());

    QVERIFY(item.isNull());
    QVERIFY(reloaded == nullptr);
}

void TestAtomicActivitySave::saveSingleDialogSaveAndAbandon()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    item.path = QStringLiteral("/activities");
    item.setDirty(true);

    TestSaveSingleDialog saveDialog(&item);
    saveDialog.setResult(42);
    saveDialog.saveResult = false;
    saveDialog.saveClicked();
    QCOMPARE(saveDialog.result(), 42);
    QVERIFY(!saveDialog.mayProceed());
    QCOMPARE(saveDialog.saveCalls, 1);
    QCOMPARE(saveDialog.errorReports, 1);
    QVERIFY(item.isDirty());

    saveDialog.saveResult = true;
    saveDialog.saveAction = [&item]() {
        item.fileName = QStringLiteral("converted.json");
    };
    saveDialog.saveClicked();
    QCOMPARE(saveDialog.result(), int(QDialog::Accepted));
    QVERIFY(saveDialog.mayProceed());
    QCOMPARE(saveDialog.saveCalls, 2);

    item.setDirty(true);
    TestSaveSingleDialog abandonDialog(&item);
    abandonDialog.setResult(42);
    abandonDialog.abandonClicked();
    QCOMPARE(abandonDialog.result(), int(QDialog::Rejected));
    QVERIFY(abandonDialog.mayProceed());
    QVERIFY(!item.isDirty());
}

void TestAtomicActivitySave::
saveSingleDialogRejectsDestroyedActivity()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.fit");
    item->path = QStringLiteral("/activities");
    item->setDirty(true);
    TestSaveSingleDialog dialog(item);
    dialog.setResult(42);
    delete item;

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QVERIFY(!dialog.mayProceed());
    QCOMPARE(dialog.saveCalls, 0);
    QCOMPARE(dialog.errorReports, 1);
}

void TestAtomicActivitySave::
saveSingleDialogRejectsIdentityMutation_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("planned");

    QTest::newRow("filename")
        << QStringLiteral("replacement.fit")
        << QStringLiteral("/activities") << false;
    QTest::newRow("path")
        << QStringLiteral("activity.fit")
        << QStringLiteral("/detached") << false;
    QTest::newRow("namespace")
        << QStringLiteral("activity.fit")
        << QStringLiteral("/activities") << true;
}

void TestAtomicActivitySave::
saveSingleDialogRejectsIdentityMutation()
{
    QFETCH(QString, fileName);
    QFETCH(QString, path);
    QFETCH(bool, planned);
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    item.path = QStringLiteral("/activities");
    item.planned = false;
    item.setDirty(true);
    TestSaveSingleDialog dialog(&item);
    dialog.setResult(42);

    item.fileName = fileName;
    item.path = path;
    item.planned = planned;
    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QVERIFY(!dialog.mayProceed());
    QCOMPARE(dialog.saveCalls, 0);
    QCOMPARE(dialog.errorReports, 1);
    QVERIFY(item.isDirty());

    item.fileName = QStringLiteral("activity.fit");
    item.path = QStringLiteral("/activities");
    item.planned = false;
    TestSaveSingleDialog abandonDialog(&item);
    abandonDialog.setResult(42);
    item.fileName = fileName;
    item.path = path;
    item.planned = planned;

    abandonDialog.abandonClicked();

    QCOMPARE(abandonDialog.result(), 42);
    QVERIFY(!abandonDialog.mayProceed());
    QCOMPARE(abandonDialog.saveCalls, 0);
    QCOMPARE(abandonDialog.errorReports, 1);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::
saveSingleDialogCommitsWhenActivityDestroyedDuringSave()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.fit");
    item->path = QStringLiteral("/activities");
    item->setDirty(true);
    TestSaveSingleDialog dialog(item);
    dialog.setResult(42);
    dialog.saveAction = [&] {
        delete item;
        item = nullptr;
    };

    dialog.saveClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.mayProceed());
    QCOMPARE(dialog.saveCalls, 1);
    QCOMPARE(dialog.errorReports, 0);
}

void TestAtomicActivitySave::
saveSingleDialogRejectsActivityDestroyedDuringFailedSave()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.fit");
    item->path = QStringLiteral("/activities");
    item->setDirty(true);
    TestSaveSingleDialog dialog(item);
    dialog.setResult(42);
    dialog.saveResult = false;
    dialog.saveAction = [&] {
        delete item;
        item = nullptr;
    };

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QVERIFY(!dialog.mayProceed());
    QCOMPARE(dialog.saveCalls, 1);
    QCOMPARE(dialog.errorReports, 1);
}

void TestAtomicActivitySave::
saveSingleDialogSurvivesParentDestroyedDuringSave()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    item.path = QStringLiteral("/activities");
    item.setDirty(true);
    QWidget *parent = new QWidget;
    TestSaveSingleDialog *dialog =
        new TestSaveSingleDialog(&item);
    dialog->setParent(parent);
    QPointer<TestSaveSingleDialog> guardedDialog(dialog);
    dialog->saveAction = [parent]() {
        delete parent;
    };

    dialog->saveClicked();

    QVERIFY(guardedDialog.isNull());
}

void TestAtomicActivitySave::
saveOnExitDialogStopsUntilAllSelectedSave()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.path = QStringLiteral("/activities");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.path = QStringLiteral("/activities");
    second.setDirty(true);

    TestSaveOnExitDialog dialog({&first, &second});
    dialog.results.insert(&first, true);
    dialog.results.insert(&second, false);
    dialog.saveAction = [&first]() {
        first.fileName = QStringLiteral("first-converted.json");
    };
    dialog.setResult(42);

    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&first, &second}));
    QVERIFY(!first.isDirty());
    QVERIFY(second.isDirty());

    dialog.calls.clear();
    dialog.results.insert(&second, true);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&second}));
    QVERIFY(!second.isDirty());
}

void TestAtomicActivitySave::
saveOnExitCompletesRenamedLinkedSaveGroup()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first-old.json");
    first.path = QStringLiteral("/activities");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second-old.json");
    second.path = QStringLiteral("/planned");
    second.planned = true;
    second.setDirty(true);
    RideItem failing(nullptr, nullptr);
    failing.fileName = QStringLiteral("failing.json");
    failing.path = QStringLiteral("/activities");
    failing.setDirty(true);

    TestSaveOnExitDialog dialog({&first, &second, &failing});
    dialog.linkedActivities.insert(&first, &second);
    dialog.linkedActivities.insert(&second, &first);
    dialog.results.insert(&first, true);
    dialog.results.insert(&failing, false);
    dialog.saveActions.insert(&first, [&] {
        first.fileName = QStringLiteral("first-new.json");
        second.fileName = QStringLiteral("second-new.json");
        first.setDirty(false);
        second.setDirty(false);
    });
    dialog.setResult(42);

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QCOMPARE(
        dialog.calls,
        QList<RideItem *>({&first, &failing}));
    QVERIFY(!first.isDirty());
    QVERIFY(!second.isDirty());
    QVERIFY(failing.isDirty());
    QCOMPARE(dialog.errorReports, 0);

    dialog.calls.clear();
    dialog.results.insert(&failing, true);
    dialog.saveClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&failing}));
    QCOMPARE(dialog.errorReports, 0);
    QVERIFY(!failing.isDirty());
}

void TestAtomicActivitySave::
saveOnExitDialogDoesNotAcceptRedirtiedCompletedActivity()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.path = QStringLiteral("/activities");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.path = QStringLiteral("/activities");
    second.setDirty(true);

    TestSaveOnExitDialog dialog({&first, &second});
    dialog.results.insert(&first, true);
    dialog.results.insert(&second, true);
    dialog.saveActions.insert(&second, [&first]() {
        first.setDirty(true);
    });
    dialog.setResult(42);

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&first, &second}));
    QVERIFY(first.isDirty());
    QVERIFY(!second.isDirty());

    dialog.calls.clear();
    dialog.saveClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&first}));
    QVERIFY(!first.isDirty());
}

void TestAtomicActivitySave::
saveOnExitDialogDoesNotAcceptNewDirtyActivity()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.path = QStringLiteral("/activities");
    first.setDirty(true);
    RideItem added(nullptr, nullptr);
    added.fileName = QStringLiteral("added.json");
    added.path = QStringLiteral("/activities");
    added.setDirty(false);

    TestSaveOnExitDialog dialog({&first});
    dialog.trackActivity(&added);
    dialog.results.insert(&first, true);
    dialog.results.insert(&added, true);
    dialog.saveActions.insert(&first, [&added]() {
        added.setDirty(true);
    });
    dialog.setResult(42);

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&first}));
    QVERIFY(added.isDirty());
    QTableWidget *const table = dialog.findChild<QTableWidget *>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 2);

    dialog.calls.clear();
    dialog.saveClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&added}));
    QVERIFY(!added.isDirty());
}

void TestAtomicActivitySave::
saveOnExitDialogDefersSkippedStateUntilSuccess()
{
    RideItem skipped(nullptr, nullptr);
    skipped.fileName = QStringLiteral("skipped.json");
    skipped.path = QStringLiteral("/activities");
    skipped.setDirty(true);
    RideItem failing(nullptr, nullptr);
    failing.fileName = QStringLiteral("failing.json");
    failing.path = QStringLiteral("/activities");
    failing.setDirty(true);

    TestSaveOnExitDialog dialog({&skipped, &failing});
    QTableWidget *const table = dialog.findChild<QTableWidget *>();
    QVERIFY(table != nullptr);
    QCheckBox *const skippedCheckBox =
        qobject_cast<QCheckBox *>(table->cellWidget(0, 0));
    QVERIFY(skippedCheckBox != nullptr);
    skippedCheckBox->setChecked(false);
    dialog.results.insert(&failing, false);
    dialog.setResult(42);

    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&failing}));
    QVERIFY(!skipped.skipsave);
    QVERIFY(skipped.isDirty());
    QVERIFY(failing.isDirty());

    dialog.calls.clear();
    dialog.results.insert(&failing, true);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&failing}));
    QVERIFY(skipped.skipsave);
    QVERIFY(skipped.isDirty());
    QVERIFY(!failing.isDirty());
}

void TestAtomicActivitySave::
saveOnExitAbandonMarksRedirtiedCompletedActivity()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.path = QStringLiteral("/activities");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.path = QStringLiteral("/activities");
    second.setDirty(true);

    TestSaveOnExitDialog dialog({&first, &second});
    dialog.results.insert(&first, true);
    dialog.results.insert(&second, false);
    dialog.setResult(42);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QVERIFY(!first.isDirty());
    QVERIFY(second.isDirty());

    first.setDirty(true);
    dialog.abandonClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(first.skipsave);
    QVERIFY(second.skipsave);
}

void TestAtomicActivitySave::
saveOnExitAbandonMarksNewDirtyActivity()
{
    RideItem initial(nullptr, nullptr);
    initial.fileName = QStringLiteral("initial.json");
    initial.path = QStringLiteral("/activities");
    initial.setDirty(true);
    RideItem added(nullptr, nullptr);
    added.fileName = QStringLiteral("added.json");
    added.path = QStringLiteral("/activities");
    added.setDirty(false);

    TestSaveOnExitDialog dialog({&initial});
    dialog.trackActivity(&added);
    dialog.results.insert(&initial, false);
    dialog.saveActions.insert(&initial, [&added] {
        added.setDirty(true);
    });
    dialog.setResult(42);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QVERIFY(initial.isDirty());
    QVERIFY(added.isDirty());

    dialog.abandonClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(initial.skipsave);
    QVERIFY(added.skipsave);
}

void TestAtomicActivitySave::
saveOnExitDialogRejectsIdentityMutation_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("planned");

    QTest::newRow("filename")
        << QStringLiteral("replacement.json")
        << QStringLiteral("/activities") << false;
    QTest::newRow("path")
        << QStringLiteral("activity.json")
        << QStringLiteral("/detached") << false;
    QTest::newRow("namespace")
        << QStringLiteral("activity.json")
        << QStringLiteral("/activities") << true;
}

void TestAtomicActivitySave::
saveOnExitDialogRejectsIdentityMutation()
{
    QFETCH(QString, fileName);
    QFETCH(QString, path);
    QFETCH(bool, planned);
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.json");
    item.path = QStringLiteral("/activities");
    item.planned = false;
    item.setDirty(true);
    TestSaveOnExitDialog dialog({&item});
    dialog.setResult(42);

    item.fileName = fileName;
    item.path = path;
    item.planned = planned;
    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QVERIFY(dialog.calls.isEmpty());
    QCOMPARE(dialog.errorReports, 1);
    QVERIFY(item.isDirty());
    QVERIFY(!item.skipsave);

    item.fileName = QStringLiteral("activity.json");
    item.path = QStringLiteral("/activities");
    item.planned = false;
    TestSaveOnExitDialog abandonDialog({&item});
    abandonDialog.setResult(42);
    item.fileName = fileName;
    item.path = path;
    item.planned = planned;

    abandonDialog.abandonClicked();

    QCOMPARE(abandonDialog.result(), 42);
    QVERIFY(abandonDialog.calls.isEmpty());
    QCOMPARE(abandonDialog.errorReports, 1);
    QVERIFY(item.isDirty());
    QVERIFY(!item.skipsave);
}

void TestAtomicActivitySave::
saveOnExitDialogRejectsDestroyedActivity()
{
    RideItem *removed = new RideItem(nullptr, nullptr);
    removed->fileName = QStringLiteral("removed.json");
    removed->path = QStringLiteral("/activities");
    removed->setDirty(true);
    RideItem survivor(nullptr, nullptr);
    survivor.fileName = QStringLiteral("survivor.json");
    survivor.path = QStringLiteral("/activities");
    survivor.setDirty(true);
    TestSaveOnExitDialog dialog({removed, &survivor});
    dialog.results.insert(&survivor, true);
    dialog.setResult(42);
    delete removed;

    dialog.saveClicked();

    QCOMPARE(dialog.result(), 42);
    QVERIFY(dialog.calls.isEmpty());
    QCOMPARE(dialog.errorReports, 1);
    QVERIFY(survivor.isDirty());
    QVERIFY(!survivor.skipsave);
}

void TestAtomicActivitySave::
saveOnExitDialogCommitsWhenActivityDestroyedDuringSave()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.json");
    item->path = QStringLiteral("/activities");
    item->setDirty(true);
    RideItem *const expectedItem = item;
    TestSaveOnExitDialog dialog({item});
    dialog.results.insert(item, true);
    dialog.setResult(42);
    dialog.saveAction = [&] {
        delete item;
        item = nullptr;
    };

    dialog.saveClicked();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({expectedItem}));
    QCOMPARE(dialog.errorReports, 0);
}

void TestAtomicActivitySave::
saveOnExitDialogSurvivesParentDestroyedDuringSave()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.json");
    item.path = QStringLiteral("/activities");
    item.setDirty(true);
    QWidget *parent = new QWidget;
    TestSaveOnExitDialog *dialog =
        new TestSaveOnExitDialog({&item});
    dialog->setParent(parent);
    QPointer<TestSaveOnExitDialog> guardedDialog(dialog);
    dialog->saveAction = [parent]() {
        delete parent;
    };

    dialog->saveClicked();

    QVERIFY(guardedDialog.isNull());
}

void TestAtomicActivitySave::rideCacheSaveActivityPropagatesFailure()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.json");
    item.setDirty(true);

    int saveCalls = 0;
    int notifications = 0;
    RideCache::SaveActivityFunction save =
        [&](Context *, RideItem *, QString *error) {
            ++saveCalls;
            *error = QStringLiteral("injected cache save failure");
            return false;
        };
    const RideCache::ActivitySavedFunction notify =
        [&](RideItem *savedItem) {
            QCOMPARE(savedItem, &item);
            ++notifications;
        };

    QString error;
    QVERIFY(!RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QCOMPARE(error, QStringLiteral("injected cache save failure"));
    QCOMPARE(saveCalls, 1);
    QCOMPARE(notifications, 0);
    QVERIFY(item.isDirty());

    save = [&](Context *, RideItem *savedItem, QString *) {
        ++saveCalls;
        savedItem->setDirty(false);
        return true;
    };
    QVERIFY(RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QVERIFY(error.isEmpty());
    QCOMPARE(saveCalls, 2);
    QCOMPARE(notifications, 1);
    QVERIFY(!item.isDirty());

    QVERIFY(RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QCOMPARE(saveCalls, 2);
    QCOMPARE(notifications, 1);

    QVERIFY(!RideCache::saveActivity(
        nullptr, nullptr, error, save, notify));
    QVERIFY(!error.isEmpty());
}

void TestAtomicActivitySave::
rideCacheSaveActivityRejectsDestructionDuringSave()
{
    RideItem *item = new RideItem(nullptr, nullptr);
    item->fileName = QStringLiteral("activity.json");
    item->setDirty(true);
    int notifications = 0;
    bool receivedExpectedItem = false;
    QString error;

    const bool saved = RideCache::saveActivity(
        nullptr, item, error,
        [&](Context *, RideItem *savedItem, QString *) {
            receivedExpectedItem =
                savedItem == item;
            delete item;
            item = nullptr;
            return true;
        },
        [&](RideItem *) {
            ++notifications;
        });

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QVERIFY(receivedExpectedItem);
    QCOMPARE(notifications, 0);
}

void TestAtomicActivitySave::rideCacheSaveActivitiesAggregatesFailures()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.setDirty(true);
    RideItem third(nullptr, nullptr);
    third.fileName = QStringLiteral("third.json");
    third.setDirty(true);
    RideItem clean(nullptr, nullptr);
    clean.fileName = QStringLiteral("clean.json");
    clean.setDirty(false);

    bool failBatch = true;
    int saveCalls = 0;
    QList<RideItem *> notifications;
    const RideCache::SaveActivityFunction save =
        [&](Context *, RideItem *item, QString *error) {
            ++saveCalls;
            if (failBatch && (item == &first || item == &third)) {
                *error = item == &first
                    ? QStringLiteral("injected first failure")
                    : QStringLiteral("injected third failure");
                return false;
            }
            item->setDirty(false);
            return true;
        };
    const RideCache::ActivitySavedFunction notify =
        [&](RideItem *item) { notifications.append(item); };

    QString error = QStringLiteral("stale error");
    QVERIFY(!RideCache::saveActivities(
        nullptr, {&first, &second, &third, &clean}, error, save, notify));
    QCOMPARE(saveCalls, 3);
    QCOMPARE(notifications, QList<RideItem *>({&second}));
    QVERIFY(error.contains(first.fileName));
    QVERIFY(error.contains(third.fileName));
    QVERIFY(error.contains(QStringLiteral("injected first failure")));
    QVERIFY(error.contains(QStringLiteral("injected third failure")));
    QVERIFY(first.isDirty());
    QVERIFY(!second.isDirty());
    QVERIFY(third.isDirty());

    failBatch = false;
    QVERIFY(RideCache::saveActivities(
        nullptr, {&first, &second, &third, &clean}, error, save, notify));
    QVERIFY(error.isEmpty());
    QCOMPARE(saveCalls, 5);
    QCOMPARE(notifications,
             QList<RideItem *>({&second, &first, &third}));
    QVERIFY(!first.isDirty());
    QVERIFY(!third.isDirty());

    QVERIFY(!RideCache::saveActivities(
        nullptr, {nullptr}, error, save, notify));
    QVERIFY(error.contains(QStringLiteral("unknown activity")));
    QCOMPARE(saveCalls, 5);
}

void TestAtomicActivitySave::
rideCacheSaveActivitiesSurviveReentrantNextDeletion_data()
{
    QTest::addColumn<bool>("deleteFromSave");

    QTest::newRow("save-callback") << true;
    QTest::newRow("saved-notification") << false;
}

void TestAtomicActivitySave::
rideCacheSaveActivitiesSurviveReentrantNextDeletion()
{
    QFETCH(bool, deleteFromSave);

    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.setDirty(true);
    RideItem *second = new RideItem(nullptr, nullptr);
    second->fileName = QStringLiteral("second.json");
    second->setDirty(true);
    QPointer<RideItem> secondGuard(second);
    int saveCalls = 0;
    QList<RideItem*> notifications;
    QString error;

    const bool saved = RideCache::saveActivities(
        nullptr, {&first, second}, error,
        [&](Context *, RideItem *item, QString *) {
            ++saveCalls;
            item->setDirty(false);
            if (deleteFromSave && item == &first) {
                delete second;
                second = nullptr;
            }
            return true;
        },
        [&](RideItem *item) {
            notifications.append(item);
            if (!deleteFromSave && item == &first) {
                delete second;
                second = nullptr;
            }
        });

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QCOMPARE(saveCalls, 1);
    QCOMPARE(notifications, QList<RideItem*>({&first}));
    QVERIFY(secondGuard.isNull());
}

void TestAtomicActivitySave::
rideCacheSaveActivitiesStopWhenOwnerDestroyed_data()
{
    QTest::addColumn<bool>("destroyFromSave");

    QTest::newRow("save-callback") << true;
    QTest::newRow("saved-notification") << false;
}

void TestAtomicActivitySave::
rideCacheSaveActivitiesStopWhenOwnerDestroyed()
{
    QFETCH(bool, destroyFromSave);

    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.setDirty(true);
    QObject *owner = new QObject;
    QPointer<QObject> ownerGuard(owner);
    int saveCalls = 0;
    int notifications = 0;
    QString error;

    const bool saved = RideCache::saveActivities(
        nullptr, {&first, &second}, error,
        [&](Context *, RideItem *item, QString *) {
            ++saveCalls;
            item->setDirty(false);
            if (destroyFromSave) {
                delete owner;
                owner = nullptr;
            }
            return true;
        },
        [&](RideItem *) {
            ++notifications;
            if (!destroyFromSave) {
                delete owner;
                owner = nullptr;
            }
        },
        owner);

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QVERIFY(ownerGuard.isNull());
    QCOMPARE(saveCalls, 1);
    QCOMPARE(notifications, destroyFromSave ? 0 : 1);
    QVERIFY(!first.isDirty());
    QVERIFY(second.isDirty());
}

void TestAtomicActivitySave::
mainWindowRejectsIdentityChangedByProcessor_data()
{
    QTest::addColumn<QString>("mutation");

    QTest::newRow("filename")
        << QStringLiteral("filename");
    QTest::newRow("path")
        << QStringLiteral("path");
    QTest::newRow("namespace")
        << QStringLiteral("planned");
}

void TestAtomicActivitySave::
mainWindowRejectsIdentityChangedByProcessor()
{
    QFETCH(QString, mutation);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(
        QDate(2026, 8, 1), QTime(10, 11, 12));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(fileName);
    const QByteArray original("original activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.stage = [&](RideFile *, QString &) {
        if (mutation == QStringLiteral("filename")) {
            item.fileName = QStringLiteral("replacement.json");
        } else if (mutation == QStringLiteral("path")) {
            item.path = dir.filePath(
                QStringLiteral("replacement"));
        } else {
            item.planned = true;
        }
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(error.contains(
        QStringLiteral("changed"),
        Qt::CaseInsensitive));
    QCOMPARE(readAll(sourcePath), original);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::
mainWindowSurvivesItemDestroyedByProcessor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(
        QDate(2026, 8, 1), QTime(10, 12, 13));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(fileName);
    const QByteArray original("original activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem *item = new RideItem(&ride, nullptr);
    item->path = dir.path();
    item->fileName = fileName;
    item->setDirty(true);
    QPointer<RideItem> guardedItem(item);

    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.stage = [&](RideFile *, QString &) {
        delete item;
        item = nullptr;
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, item, &error, &operations));
    QVERIFY(guardedItem.isNull());
    QVERIFY(error.contains(
        QStringLiteral("changed"),
        Qt::CaseInsensitive));
    QCOMPARE(readAll(sourcePath), original);
}

QTEST_MAIN(TestAtomicActivitySave)

#include "testAtomicActivitySave.moc"
