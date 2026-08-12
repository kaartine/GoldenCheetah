#include <QtTest>

#include "PlanBundleImportJournal.h"
#include "PlanReplacementJournal.h"
#include "TrainDB.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <mutex>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
std::atomic_int publicationChunkDelayMs{0};
std::atomic_int publicationChunkVisits{0};
std::atomic_int decisionPersistenceDelayMs{0};
std::atomic_int decisionPersistenceVisits{0};
std::mutex decisionPersistenceActionMutex;
std::function<void()> decisionPersistenceAction;
std::mutex databasePreOpenActionMutex;
std::function<void()> databasePreOpenAction;
std::mutex databaseOpenBoundaryActionMutex;
std::function<void(bool)> databaseOpenBoundaryAction;
std::mutex publicationMutationActionMutex;
std::function<void()> publicationMutationAction;
std::mutex anchoredSyncActionMutex;
std::function<bool(const QString &)> anchoredSyncAction;
}

void anchoredFilesystemTransitionReached(
    const char *, const QString &, const QString &)
{
}

bool anchoredFilesystemSyncFailureRequested(const QString &path)
{
    std::function<bool(const QString &)> action;
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        action = anchoredSyncAction;
    }
    return action && action(path);
}

bool anchoredFilesystemFileUnlinkFailureRequested(const QString &)
{
    return false;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return false;
}

void planBundleImportPublicationChunkTestHook()
{
    publicationChunkVisits.fetch_add(1, std::memory_order_release);
    const int delay = publicationChunkDelayMs.load(
        std::memory_order_acquire);
    if (delay > 0)
        QThread::msleep(static_cast<unsigned long>(delay));
}

void planBundleImportPublicationMutationTestHook()
{
    std::function<void()> action;
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        action = publicationMutationAction;
    }
    if (action) action();
}

bool trainDbPlanImportCommitReportedFailure()
{
    return false;
}

void trainDbPlanImportPersistenceTestHook()
{
    decisionPersistenceVisits.fetch_add(1, std::memory_order_release);
    const int delay = decisionPersistenceDelayMs.load(
        std::memory_order_acquire);
    std::function<void()> action;
    {
        const std::lock_guard<std::mutex> lock(
            decisionPersistenceActionMutex);
        action = decisionPersistenceAction;
    }
    if (action) action();
    if (delay > 0)
        QThread::msleep(static_cast<unsigned long>(delay));
}

void trainDbPlanImportPreOpenTestHook()
{
    std::function<void()> action;
    {
        const std::lock_guard<std::mutex> lock(
            databasePreOpenActionMutex);
        action = databasePreOpenAction;
    }
    if (action) action();
}

void trainDbPlanImportOpenBoundaryTestHook(bool opened)
{
    std::function<void(bool)> action;
    {
        const std::lock_guard<std::mutex> lock(
            databaseOpenBoundaryActionMutex);
        action = databaseOpenBoundaryAction;
    }
    if (action) action(opened);
}

namespace {

using namespace std::chrono_literals;

#ifdef Q_OS_WIN
class WindowsTestHandle
{
public:
    explicit WindowsTestHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsTestHandle()
    {
        if (isValid()) ::CloseHandle(handle_);
    }

    WindowsTestHandle(const WindowsTestHandle &) = delete;
    WindowsTestHandle &operator=(const WindowsTestHandle &) = delete;

    bool isValid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool createCurrentUserOwnedDirectory(const QString &path)
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    WindowsTestHandle token(rawToken);
    DWORD required = 0;
    ::GetTokenInformation(
        token.get(), TokenUser, nullptr, 0, &required);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return false;
    }
    QByteArray userStorage(int(required), '\0');
    if (!::GetTokenInformation(
            token.get(), TokenUser, userStorage.data(),
            required, &required)) {
        return false;
    }
    auto *user = reinterpret_cast<TOKEN_USER *>(
        userStorage.data());
    if (!::IsValidSid(user->User.Sid)) return false;

    const DWORD aclSize = DWORD(
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE)
        - sizeof(DWORD) + ::GetLengthSid(user->User.Sid));
    QByteArray aclStorage(int(aclSize), '\0');
    auto *acl = reinterpret_cast<PACL>(aclStorage.data());
    SECURITY_DESCRIPTOR descriptor {};
    if (!::InitializeAcl(acl, aclSize, ACL_REVISION)
        || !::AddAccessAllowedAceEx(
            acl, ACL_REVISION,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
            FILE_ALL_ACCESS, user->User.Sid)
        || !::InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !::SetSecurityDescriptorOwner(
            &descriptor, user->User.Sid, FALSE)
        || !::SetSecurityDescriptorDacl(
            &descriptor, TRUE, acl, FALSE)
        || !::SetSecurityDescriptorControl(
            &descriptor, SE_DACL_PROTECTED,
            SE_DACL_PROTECTED)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    const QString native = QDir::toNativeSeparators(path);
    return ::CreateDirectoryW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        &attributes);
}
#endif

bool createOwnedFixtureHierarchy(const QStringList &paths)
{
    if (paths.isEmpty()) return false;
#ifdef Q_OS_WIN
    for (const QString &path : paths) {
        if (!createCurrentUserOwnedDirectory(path)) return false;
    }
    return true;
#else
    return QDir().mkpath(paths.constLast());
#endif
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
        ? file.readAll() : QByteArray();
}

QString qlockRemovalGuardName(int suffixCount)
{
    return QStringLiteral(".01234567-89ab-cdef-8123-456789abcdef.lock")
        + QStringLiteral(".rmlock").repeated(suffixCount);
}

struct Fixture
{
    QTemporaryDir temporary;
    QString databaseRoot;
    QString athleteRoot;
    QString plannedRoot;
    QString workoutRoot;
    QString oldPlan;
    QString newPlan;
    QString workoutTarget;

    bool initialize()
    {
        if (!temporary.isValid()) return false;
        databaseRoot = QDir(temporary.path()).filePath(
            QStringLiteral("database"));
        athleteRoot = QDir(temporary.path()).filePath(
            QStringLiteral("athlete"));
        plannedRoot = QDir(athleteRoot).filePath(
            QStringLiteral("planned"));
        workoutRoot = QDir(temporary.path()).filePath(
            QStringLiteral("workouts"));
        oldPlan = QDir(plannedRoot).filePath(
            QStringLiteral("old.json"));
        newPlan = QDir(plannedRoot).filePath(
            QStringLiteral("new.json"));
        workoutTarget = QDir(workoutRoot).filePath(
            QStringLiteral("threshold.erg"));
        return QDir().mkpath(databaseRoot)
            && createOwnedFixtureHierarchy(
                {athleteRoot, plannedRoot})
            && createOwnedFixtureHierarchy({workoutRoot})
            && writeFile(oldPlan, QByteArray("old plan"));
    }
};

std::shared_ptr<PlanReplacement::Journal> preparePlan(
    const Fixture &fixture, QString &error)
{
    PlanReplacement::Specification specification;
    specification.athleteRoot = fixture.athleteRoot;
    specification.scopeRoot = fixture.plannedRoot;
    specification.inputPaths = {fixture.oldPlan};
    specification.removalPaths = {fixture.oldPlan};
    specification.targetPaths = {fixture.newPlan};
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(specification, error);
    if (!journal
        || !writeFile(journal->stagingPath(0), QByteArray("new plan"))
        || !journal->recordStaged(0, error)) {
        return {};
    }
    return journal;
}

TrainDB::PlanImportWorkout createWorkout(const QByteArray &contents);

std::shared_ptr<PlanBundleImport::Journal> commitDecision(
    Fixture &fixture,
    TrainDB &database,
    std::shared_ptr<PlanReplacement::Journal> &plan,
    QString &error)
{
    plan = preparePlan(fixture, error);
    if (!plan) return {};
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        PlanBundleImport::Journal::create(
            &database, fixture.athleteRoot,
            fixture.workoutRoot,
            {createWorkout(QByteArray("workout payload"))},
            error);
    if (!coordinator) return {};
    bool committed = false;
    if (!coordinator->commitDecision(
            plan->directoryPath(), committed, error)
        || !committed) {
        return {};
    }
    return coordinator;
}

std::shared_ptr<PlanBundleImport::Journal> commitStandaloneDecision(
    Fixture &fixture,
    TrainDB &database,
    QString &error)
{
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        PlanBundleImport::Journal::createStandalone(
            &database, fixture.athleteRoot,
            fixture.workoutRoot,
            {createWorkout(
                QByteArray("standalone workout payload"))},
            error);
    if (!coordinator) return {};
    bool committed = false;
    if (!coordinator->commitStandaloneDecision(committed, error)
        || !committed) {
        return {};
    }
    return coordinator;
}

PlanBundleImport::BoundDatabaseCompletion removeDecision(
    TrainDB &database)
{
    return [&database](
        const TrainDB::PlanImportJournal &journal,
        const PlanBundleImport::PublishedValidation &validatePublished,
        QString &error) {
        TrainDB::ScopedLUW transaction(database);
        if (!transaction.isActive()) {
            error = QStringLiteral("cannot start completion");
            return false;
        }
        if (!validatePublished(error)
            || !database.removePlanImportJournal(
                journal.id, error)) {
            return false;
        }
        if (!transaction.commit()) {
            error = QStringLiteral("cannot commit completion");
            return false;
        }
        return true;
    };
}

PlanBundleImport::DatabaseCompletion removeDecisionUnbound(
    TrainDB &database)
{
    return [&database](
        const TrainDB::PlanImportJournal &journal,
        QString &error) {
        TrainDB::ScopedLUW transaction(database);
        return transaction.isActive()
            && database.removePlanImportJournal(journal.id, error)
            && transaction.commit();
    };
}

TrainDB::PlanImportWorkout replacementWorkoutNamed(
    const QString &targetFileName,
    const QByteArray &previous,
    const QByteArray &replacement)
{
    TrainDB::PlanImportWorkout workout;
    workout.targetFileName = targetFileName;
    workout.contents = replacement;
    workout.replaceExisting = true;
    workout.previousSize = previous.size();
    workout.previousDigest = QCryptographicHash::hash(
        previous, QCryptographicHash::Sha256);
    return workout;
}

TrainDB::PlanImportWorkout replacementWorkout(
    const QByteArray &previous,
    const QByteArray &replacement)
{
    return replacementWorkoutNamed(
        QStringLiteral("threshold.erg"), previous, replacement);
}

TrainDB::PlanImportWorkout createWorkoutNamed(
    const QString &targetFileName,
    const QByteArray &contents)
{
    TrainDB::PlanImportWorkout workout;
    workout.targetFileName = targetFileName;
    workout.contents = contents;
    return workout;
}

TrainDB::PlanImportWorkout createWorkout(const QByteArray &contents)
{
    return createWorkoutNamed(
        QStringLiteral("threshold.erg"), contents);
}

bool decisionExists(
    TrainDB &database,
    const QString &athleteRoot)
{
    TrainDB::PlanImportJournal journal;
    bool found = false;
    QString error;
    return database.loadPlanImportJournal(
               athleteRoot, journal, found, error)
        && found;
}

QStringList importArtifacts(const QString &workoutRoot)
{
    const QDir directory(workoutRoot);
    QStringList artifacts;
    const QFileInfoList entries = directory.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (name.startsWith(QStringLiteral(".gc-import-"))
            || name.startsWith(QStringLiteral(".gc-replace-"))
            || (name.startsWith(QStringLiteral(".gc-plan-import-"))
                && !name.startsWith(
                    QStringLiteral(".gc-plan-import-decision-")))) {
            artifacts.append(entry.absoluteFilePath());
        }
    }
    return artifacts;
}

} // namespace

class TestPlanBundleImportJournal : public QObject
{
    Q_OBJECT

private slots:
    void committedDecisionCompletesAfterRestart();
    void coordinatedStartupRecoverySurvivesQLockFileRemovalGuard_data();
    void coordinatedStartupRecoverySurvivesQLockFileRemovalGuard();
    void databaseFailureRetainsDecisionForRetry();
    void conflictingWorkoutTargetFailsClosed();
    void completionMustRemoveDecisionAtomically();
    void completionValidationMustRunInsideLuw();
    void retainedPublishedValidationExpiresSafely();
    void completionDatabaseDeletionFailsClosed();
    void standaloneDecisionCompletesAfterRestart();
    void standaloneDatabaseFailureRetainsDecisionForRetry();
    void standaloneForeignReplacementIsPreserved();
    void standaloneSymlinkReplacementIsPreserved();
    void standaloneWorkoutRootReplacementFailsClosed();
    void standaloneCreatePreservesExistingTarget();
    void standaloneOverwriteCompletesAfterRestart();
    void standaloneOverwriteRejectsChangedPredecessor();
    void standaloneOverwriteRecoversAfterFilePublication();
    void partialOverwriteNeverDeletesPublishedTarget();
    void successfulOverwriteRemovesPredecessorArtifact();
    void restartRemovesPersistedPredecessorArtifact();
    void restartPreservesSubstitutedPredecessorArtifact();
    void unboundRecoveryCompletionIsRejected();
    void standaloneMultiTargetPreflightIsMutationFree_data();
    void standaloneMultiTargetPreflightIsMutationFree();
    void preparedStandaloneCommitRejectsRootReplacement();
    void preparedStandaloneCommitRejectsDatabaseReplacement();
    void preparedCommitPreOpenSwapDoesNotOpenReplacement();
    void preparedCommitRejectsDatabaseOpenAbaSwap();
    void preparedDigestMismatchIsRejectedBeforeDecision();
    void publicationRootSwapDoesNotWriteReplacement();
    void finalizationRejectsRootWithCopiedMarker();
    void restartRecoveryRejectsRootWithCopiedMarker();
    void oversizedCreateConflictIsRejectedWithoutReadingContents();
    void publishedTargetReplacementRetainsDecision();
    void workerDecisionPersistenceKeepsOwnerEventLoopResponsive();
    void cancellablePublicationKeepsGuiResponsive();
};

void TestPlanBundleImportJournal::
preparedDigestMismatchIsRejectedBeforeDecision()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray("prepared workout"));
    workout.digest = QByteArray(
        QCryptographicHash::hashLength(QCryptographicHash::Sha256), 'x');
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));

    bool committed = false;
    QVERIFY(!coordinator->commitStandaloneDecision(
        database.databaseFilePath(), {}, committed, error));
    QVERIFY(!committed);
    QVERIFY(error.contains(
        QStringLiteral("payload"), Qt::CaseInsensitive));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
preparedStandaloneCommitRejectsDatabaseReplacement()
{
#ifdef Q_OS_WIN
    QSKIP("An open SQLite database cannot be renamed on Windows");
#else
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray("prepared workout"));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        database.databaseFilePath(), error);
    QVERIFY2(generation, qPrintable(error));

    const QString original = database.databaseFilePath();
    const QString displacedRoot = fixture.databaseRoot
        + QStringLiteral("-displaced");
    bool replaced = false;
    {
        const std::lock_guard<std::mutex> lock(
            decisionPersistenceActionMutex);
        decisionPersistenceAction = [&] {
            if (replaced) return;
            replaced = QDir().rename(
                    fixture.databaseRoot, displacedRoot)
                && QDir().mkpath(fixture.databaseRoot)
                && writeFile(original, QByteArray("replacement database"));
        };
    }
    bool committed = false;
    const bool result = coordinator->commitStandaloneDecision(
        original, {}, generation, committed, error);
    {
        const std::lock_guard<std::mutex> lock(
            decisionPersistenceActionMutex);
        decisionPersistenceAction = {};
    }
    QVERIFY(replaced);
    QVERIFY(!result);
    QVERIFY(!committed);
    QVERIFY(error.contains(
        QStringLiteral("database"), Qt::CaseInsensitive));

    QVERIFY(QDir(fixture.databaseRoot).removeRecursively());
    QVERIFY(QDir().rename(displacedRoot, fixture.databaseRoot));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
#endif
}

void TestPlanBundleImportJournal::
preparedCommitPreOpenSwapDoesNotOpenReplacement()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray("prepared workout"));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        database.databaseFilePath(), error);
    QVERIFY2(generation, qPrintable(error));

    const QString original = database.databaseFilePath();
    const QString displacedDatabase = original
        + QStringLiteral(".pre-open-displaced");
    const QByteArray replacement("replacement database sentinel");
    bool swapped = false;
    bool renameDenied = false;
    {
        const std::lock_guard<std::mutex> lock(databasePreOpenActionMutex);
        databasePreOpenAction = [&] {
            if (swapped || renameDenied) return;
            if (!QFile::rename(original, displacedDatabase)) {
                renameDenied = true;
                return;
            }
            swapped = writeFile(original, replacement);
        };
    }
    bool committed = false;
    const bool result = coordinator->commitStandaloneDecision(
        original, {}, generation, committed, error);
    {
        const std::lock_guard<std::mutex> lock(databasePreOpenActionMutex);
        databasePreOpenAction = {};
    }
#ifdef Q_OS_WIN
    if (renameDenied) {
        QVERIFY(result);
        QVERIFY(committed);
        QSKIP("The open SQLite file denies replacement on Windows");
    }
#endif
    QVERIFY(swapped);
    QVERIFY(!result);
    QVERIFY(!committed);
    QCOMPARE(readFile(original), replacement);
    QVERIFY(!QFileInfo::exists(original + QStringLiteral("-journal")));
    QVERIFY(!QFileInfo::exists(original + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(original + QStringLiteral("-shm")));

    QVERIFY(QFile::remove(original));
    QVERIFY(QFile::rename(displacedDatabase, original));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
preparedCommitRejectsDatabaseOpenAbaSwap()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray("prepared workout"));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        database.databaseFilePath(), error);
    QVERIFY2(generation, qPrintable(error));

    const QString replacementRoot = QDir(fixture.temporary.path()).filePath(
        QStringLiteral("replacement-database"));
    QVERIFY(QDir().mkpath(replacementRoot));
    {
        TrainDB replacement{QDir(replacementRoot)};
        QCOMPARE(replacement.schemaStatus(), TrainDB::SchemaStatus::current);
    }

    const QString original = database.databaseFilePath();
    const QString replacement = QDir(replacementRoot).filePath(
        QStringLiteral("trainDB"));
    const QString displacedOriginal = original
        + QStringLiteral(".aba-original");
    const QString displacedReplacement = original
        + QStringLiteral(".aba-replacement");
    bool swapped = false;
    bool restored = false;
    bool renameDenied = false;
    bool restoreDenied = false;
    bool replacementDisplaced = false;
    {
        const std::lock_guard<std::mutex> lock(
            databaseOpenBoundaryActionMutex);
        databaseOpenBoundaryAction = [&](bool opened) {
            if (!opened) {
                if (!QFile::rename(original, displacedOriginal)) {
                    renameDenied = true;
                    return;
                }
                swapped = QFile::rename(replacement, original);
                return;
            }
            if (swapped && !restored) {
                replacementDisplaced =
                    QFile::rename(original, displacedReplacement);
                if (!replacementDisplaced) {
                    restoreDenied = true;
                    return;
                }
                restored = QFile::rename(displacedOriginal, original);
                restoreDenied = !restored;
            }
        };
    }

    bool committed = false;
    const bool result = coordinator->commitStandaloneDecision(
        original, {}, generation, committed, error);
    {
        const std::lock_guard<std::mutex> lock(
            databaseOpenBoundaryActionMutex);
        databaseOpenBoundaryAction = {};
    }
#ifdef Q_OS_WIN
    if (renameDenied) {
        QVERIFY(result);
        QVERIFY(committed);
        QSKIP("The pinned SQLite generation denies replacement on Windows");
    }
    if (restoreDenied) {
        QVERIFY(!result);
        QVERIFY(!committed);
        QVERIFY(error.contains(
            QStringLiteral("database"), Qt::CaseInsensitive));
        if (!replacementDisplaced)
            QVERIFY(QFile::rename(original, displacedReplacement));
        QVERIFY(QFile::rename(displacedOriginal, original));
        QSKIP("The open replacement denies ABA restoration on Windows");
    }
#endif
    QVERIFY(swapped);
    QVERIFY(restored);
    QVERIFY(!result);
    QVERIFY(!committed);
    QVERIFY(error.contains(
        QStringLiteral("database"), Qt::CaseInsensitive));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
publicationRootSwapDoesNotWriteReplacement()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));

    const QString displaced = fixture.workoutRoot
        + QStringLiteral("-displaced");
    const QString replacementSentinel = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("sentinel"));
    const QString markerName = QStringLiteral(".gc-workout-root-id");
    const QByteArray copiedMarker = readFile(
        QDir(fixture.workoutRoot).filePath(markerName));
    QVERIFY(!copiedMarker.isEmpty());
    bool replaced = false;
    bool renameDenied = false;
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        publicationMutationAction = [&] {
            if (replaced || renameDenied) return;
            if (!QDir().rename(fixture.workoutRoot, displaced)) {
                renameDenied = true;
                return;
            }
            replaced = createOwnedFixtureHierarchy({fixture.workoutRoot})
                && writeFile(
                    QDir(fixture.workoutRoot).filePath(markerName),
                    copiedMarker)
                && writeFile(replacementSentinel, QByteArray("replacement"));
        };
    }
    const bool published = coordinator->publishPreparedFiles({}, error);
    {
        const std::lock_guard<std::mutex> lock(
            publicationMutationActionMutex);
        publicationMutationAction = {};
    }
#ifdef Q_OS_WIN
    if (renameDenied) {
        QVERIFY(published);
        QSKIP("The live workout-root anchor denies renames on Windows");
    }
#endif
    QVERIFY(replaced);
    QVERIFY(!published);
    QCOMPARE(readFile(replacementSentinel), QByteArray("replacement"));
    QVERIFY(!QFileInfo::exists(QDir(fixture.workoutRoot).filePath(
        QStringLiteral("threshold.erg"))));
    QVERIFY(!QFileInfo::exists(QDir(displaced).filePath(
        QStringLiteral("threshold.erg"))));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QDir(fixture.workoutRoot).removeRecursively());
    QVERIFY(QDir().rename(displaced, fixture.workoutRoot));
}

void TestPlanBundleImportJournal::
finalizationRejectsRootWithCopiedMarker()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));

    const QString markerName = QStringLiteral(".gc-workout-root-id");
    const QByteArray copiedMarker = readFile(
        QDir(fixture.workoutRoot).filePath(markerName));
    const QString displaced = fixture.workoutRoot
        + QStringLiteral("-finalization-displaced");
    if (!QDir().rename(fixture.workoutRoot, displaced)) {
#ifdef Q_OS_WIN
        QSKIP("The live workout-root anchor denies renames on Windows");
#else
        QFAIL("The workout-root swap failed unexpectedly");
#endif
    }
    QVERIFY(createOwnedFixtureHierarchy({fixture.workoutRoot}));
    QVERIFY(writeFile(
        QDir(fixture.workoutRoot).filePath(markerName), copiedMarker));
    const QString sentinel = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("sentinel"));
    QVERIFY(writeFile(sentinel, QByteArray("replacement")));

    bool completionEntered = false;
    const PlanBundleImport::BoundDatabaseCompletion completion =
        [&database, &completionEntered](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validate,
            QString &completionError) {
            completionEntered = true;
            TrainDB::ScopedLUW transaction(database);
            if (!transaction.isActive()
                || !validate(completionError)
                || !database.removePlanImportJournal(
                    journal.id, completionError)) {
                return false;
            }
            return transaction.commit();
        };
    QVERIFY(!coordinator->completePublishedDatabaseBound(
        completion, error));
    QVERIFY(!completionEntered);
    QCOMPARE(readFile(sentinel), QByteArray("replacement"));
    QVERIFY(!QFileInfo::exists(QDir(fixture.workoutRoot).filePath(
        QStringLiteral("threshold.erg"))));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QDir(fixture.workoutRoot).removeRecursively());
    QVERIFY(QDir().rename(displaced, fixture.workoutRoot));
}

void TestPlanBundleImportJournal::
restartRecoveryRejectsRootWithCopiedMarker()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));

    const QString markerName = QStringLiteral(".gc-workout-root-id");
    const QByteArray copiedMarker = readFile(
        QDir(fixture.workoutRoot).filePath(markerName));
    QVERIFY(!copiedMarker.isEmpty());
    coordinator.reset();

    const QString displaced = fixture.workoutRoot
        + QStringLiteral("-restart-displaced");
    if (!QDir().rename(fixture.workoutRoot, displaced)) {
        QSKIP("The platform did not permit the workout-root swap");
    }
    QVERIFY(createOwnedFixtureHierarchy({fixture.workoutRoot}));
    QVERIFY(writeFile(
        QDir(fixture.workoutRoot).filePath(markerName), copiedMarker));
    const QString sentinel = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("sentinel"));
    QVERIFY(writeFile(sentinel, QByteArray("replacement")));

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(sentinel), QByteArray("replacement"));
    QVERIFY(!QFileInfo::exists(fixture.workoutTarget));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QDir(fixture.workoutRoot).removeRecursively());
    QVERIFY(QDir().rename(displaced, fixture.workoutRoot));
    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecision(database), error), qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
}

void TestPlanBundleImportJournal::
oversizedCreateConflictIsRejectedWithoutReadingContents()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));

    QFile conflict(fixture.workoutTarget);
    QVERIFY(conflict.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QVERIFY(conflict.resize(64LL * 1024 * 1024));
    conflict.close();
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!coordinator->publishPreparedFiles({}, error));
    QVERIFY(elapsed.elapsed() < 1000);
    QVERIFY(error.contains(
        QStringLiteral("large"), Qt::CaseInsensitive)
        || error.contains(
            QStringLiteral("exists"), Qt::CaseInsensitive));
    QCOMPARE(QFileInfo(fixture.workoutTarget).size(), 64LL * 1024 * 1024);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
preparedStandaloneCommitRejectsRootReplacement()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray("prepared workout"));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));

    const QString displaced = fixture.workoutRoot
        + QStringLiteral("-displaced");
    bool rootReplaced = false;
    bool rootRenameDenied = false;
    {
        const std::lock_guard<std::mutex> lock(
            decisionPersistenceActionMutex);
        decisionPersistenceAction = [&] {
            if (rootReplaced || rootRenameDenied) return;
            if (!QDir().rename(fixture.workoutRoot, displaced)) {
                rootRenameDenied = true;
                return;
            }
            rootReplaced =
                createOwnedFixtureHierarchy({fixture.workoutRoot});
        };
    }
    bool committed = false;
    const bool commitResult = coordinator->commitStandaloneDecision(
        database.databaseFilePath(), {}, committed, error);
    {
        const std::lock_guard<std::mutex> lock(
            decisionPersistenceActionMutex);
        decisionPersistenceAction = {};
    }
#ifdef Q_OS_WIN
    if (rootRenameDenied) {
        QVERIFY(commitResult);
        QVERIFY(committed);
        QSKIP("The pinned workout root denies replacement on Windows");
    }
#endif
    QVERIFY(rootReplaced);
    QVERIFY(!commitResult);
    QVERIFY(!committed);
    QVERIFY(!decisionExists(database, fixture.athleteRoot));

    QVERIFY(QDir().rmdir(fixture.workoutRoot));
    QVERIFY(QDir().rename(displaced, fixture.workoutRoot));
}

void TestPlanBundleImportJournal::
publishedTargetReplacementRetainsDecision()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));
    QCOMPARE(
        readFile(fixture.workoutTarget),
        QByteArray("standalone workout payload"));
    QVERIFY(!database.hasWorkout(fixture.workoutTarget));

    QVERIFY(QFile::remove(fixture.workoutTarget));
    QVERIFY(writeFile(
        fixture.workoutTarget, QByteArray("foreign replacement")));
    bool completionEntered = false;
    const PlanBundleImport::BoundDatabaseCompletion completion =
        [&database, &completionEntered](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validate,
            QString &completionError) {
            completionEntered = true;
            TrainDB::ScopedLUW transaction(database);
            if (!transaction.isActive()
                || !validate(completionError)
                || !database.removePlanImportJournal(
                    journal.id, completionError)) {
                return false;
            }
            return transaction.commit();
        };

    QVERIFY(!coordinator->completePublishedDatabaseBound(
        completion, error));
    QVERIFY(completionEntered);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
    QVERIFY(!database.hasWorkout(fixture.workoutTarget));
    QCOMPARE(
        readFile(fixture.workoutTarget),
        QByteArray("foreign replacement"));
}

void TestPlanBundleImportJournal::
workerDecisionPersistenceKeepsOwnerEventLoopResponsive()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray(32 * 1024 * 1024, 'x'));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalonePrepared(
        fixture.athleteRoot, fixture.workoutRoot, {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));

    decisionPersistenceVisits.store(0, std::memory_order_release);
    decisionPersistenceDelayMs.store(250, std::memory_order_release);
    bool committed = false;
    QString workerError;
    auto decision = std::async(std::launch::async, [&] {
        return coordinator->commitStandaloneDecision(
            database.databaseFilePath(), {}, committed, workerError);
    });

    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        decisionPersistenceVisits.load(std::memory_order_acquire) > 0,
        500);
    QTRY_VERIFY_WITH_TIMEOUT(
        decision.wait_for(0ms) == std::future_status::ready,
        5000);
    QVERIFY2(decision.get(), qPrintable(workerError));
    QVERIFY(committed);
    QVERIFY(heartbeats > 0);
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    decisionPersistenceDelayMs.store(0, std::memory_order_release);
    coordinator.reset();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecision(database), error), qPrintable(error));
}

void TestPlanBundleImportJournal::
committedDecisionCompletesAfterRestart()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> plan;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitDecision(fixture, database, plan, error);
    QVERIFY2(coordinator, qPrintable(error));
    plan.reset();
    coordinator.reset();

    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(fixture.oldPlan));
    QCOMPARE(readFile(fixture.newPlan), QByteArray("new plan"));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("workout payload"));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
    QVERIFY2(PlanReplacement::Journal::reconcileAll(
                 fixture.athleteRoot, error),
             qPrintable(error));
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
}

void TestPlanBundleImportJournal::
coordinatedStartupRecoverySurvivesQLockFileRemovalGuard_data()
{
    QTest::addColumn<int>("suffixCount");
    QTest::newRow("single-rmlock") << 1;
    QTest::newRow("nested-rmlock") << 2;
}

void TestPlanBundleImportJournal::
coordinatedStartupRecoverySurvivesQLockFileRemovalGuard()
{
    QFETCH(int, suffixCount);
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> plan;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitDecision(fixture, database, plan, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY(plan);
    const QString namespacePath = QFileInfo(
        plan->directoryPath()).absolutePath();
    QVERIFY(writeFile(
        QDir(namespacePath).filePath(
            qlockRemovalGuardName(suffixCount)),
        QByteArray("stale QLockFile removal guard")));
    plan.reset();
    coordinator.reset();

    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
    QCOMPARE(readFile(fixture.newPlan), QByteArray("new plan"));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("workout payload"));

    error.clear();
    QVERIFY2(PlanReplacement::Journal::reconcileAll(
                 fixture.athleteRoot, error),
             qPrintable(error));
    error.clear();
    QVERIFY2(PlanReplacement::Journal::reconcileAll(
                 fixture.athleteRoot, error),
             qPrintable(error));
}

void TestPlanBundleImportJournal::
databaseFailureRetainsDecisionForRetry()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> plan;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitDecision(fixture, database, plan, error);
    QVERIFY2(coordinator, qPrintable(error));
    plan.reset();
    coordinator.reset();

    const PlanBundleImport::BoundDatabaseCompletion fail = [](
        const TrainDB::PlanImportJournal &,
        const PlanBundleImport::PublishedValidation &,
        QString &error) {
        error = QStringLiteral("injected database failure");
        return false;
    };
    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot, fail, error));
    QVERIFY(decisionExists(database, fixture.athleteRoot));
    QCOMPARE(readFile(fixture.newPlan), QByteArray("new plan"));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("workout payload"));

    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
conflictingWorkoutTargetFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> plan;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitDecision(fixture, database, plan, error);
    QVERIFY2(coordinator, qPrintable(error));
    plan.reset();
    coordinator.reset();
    QVERIFY(writeFile(
        fixture.workoutTarget, QByteArray("conflicting workout")));

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot,
        removeDecision(database), error));
    QVERIFY(decisionExists(database, fixture.athleteRoot));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("conflicting workout"));

    QVERIFY(QFile::remove(fixture.workoutTarget));
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
}

void TestPlanBundleImportJournal::
completionMustRemoveDecisionAtomically()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanReplacement::Journal> plan;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitDecision(fixture, database, plan, error);
    QVERIFY2(coordinator, qPrintable(error));
    plan.reset();
    coordinator.reset();

    const PlanBundleImport::BoundDatabaseCompletion incomplete = [](
        const TrainDB::PlanImportJournal &,
        const PlanBundleImport::PublishedValidation &,
        QString &) {
        return true;
    };
    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot, incomplete, error));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
}

void TestPlanBundleImportJournal::
completionValidationMustRunInsideLuw()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));

    bool validatorAcceptedOutsideLuw = false;
    const PlanBundleImport::BoundDatabaseCompletion completion =
        [&database, &validatorAcceptedOutsideLuw](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validatePublished,
            QString &completionError) {
            validatorAcceptedOutsideLuw =
                validatePublished(completionError);
            if (!validatorAcceptedOutsideLuw) return false;
            TrainDB::ScopedLUW transaction(database);
            return transaction.isActive()
                && database.removePlanImportJournal(
                    journal.id, completionError)
                && transaction.commit();
        };
    QVERIFY(!coordinator->completePublishedDatabaseBound(
        completion, error));
    QVERIFY(!validatorAcceptedOutsideLuw);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
retainedPublishedValidationExpiresSafely()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));

    PlanBundleImport::PublishedValidation retained;
    const PlanBundleImport::BoundDatabaseCompletion completion =
        [&database, &retained](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validatePublished,
            QString &completionError) {
            TrainDB::ScopedLUW transaction(database);
            if (!transaction.isActive()) return false;
            retained = validatePublished;
            return validatePublished(completionError)
                && database.removePlanImportJournal(
                    journal.id, completionError)
                && transaction.commit();
        };
    QVERIFY2(coordinator->completePublishedDatabaseBound(
        completion, error), qPrintable(error));
    coordinator.reset();

    error.clear();
    QVERIFY(!retained(error));
    QVERIFY(error.contains(
        QStringLiteral("synchronous"), Qt::CaseInsensitive));
}

void TestPlanBundleImportJournal::
completionDatabaseDeletionFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB *database = new TrainDB(QDir(fixture.databaseRoot));
    QString error;
    auto coordinator = commitStandaloneDecision(
        fixture, *database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));

    const PlanBundleImport::BoundDatabaseCompletion completion =
        [&database](
            const TrainDB::PlanImportJournal &journal,
            const PlanBundleImport::PublishedValidation &validatePublished,
            QString &completionError) {
            TrainDB::ScopedLUW transaction(*database);
            if (!transaction.isActive()
                || !validatePublished(completionError)
                || !database->removePlanImportJournal(
                    journal.id, completionError)
                || !transaction.commit()) {
                return false;
            }
            delete database;
            database = nullptr;
            return true;
        };
    QVERIFY(!coordinator->completePublishedDatabaseBound(
        completion, error));
    QVERIFY(!database);
    QVERIFY(error.contains(
        QStringLiteral("database"), Qt::CaseInsensitive));
}

void TestPlanBundleImportJournal::
standaloneDecisionCompletesAfterRestart()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    QVERIFY(!QFileInfo::exists(fixture.workoutTarget));

    // Dropping the in-memory coordinator simulates a crash after the
    // durable database decision and before file publication.
    coordinator.reset();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));

    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
}

void TestPlanBundleImportJournal::
standaloneDatabaseFailureRetainsDecisionForRetry()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    coordinator.reset();

    const PlanBundleImport::BoundDatabaseCompletion fail = [](
        const TrainDB::PlanImportJournal &,
        const PlanBundleImport::PublishedValidation &,
        QString &failure) {
        failure = QStringLiteral("injected standalone database failure");
        return false;
    };
    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot, fail, error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
standaloneForeignReplacementIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    coordinator.reset();
    QVERIFY(writeFile(
        fixture.workoutTarget, QByteArray("foreign replacement")));

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("foreign replacement"));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QFile::remove(fixture.workoutTarget));
    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
}

void TestPlanBundleImportJournal::
standaloneSymlinkReplacementIsPreserved()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates shortcuts rather than symlinks on Windows");
#else
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    coordinator.reset();
    const QString foreign = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("foreign.erg"));
    QVERIFY(writeFile(foreign, QByteArray("foreign payload")));
    if (!QFile::link(foreign, fixture.workoutTarget)) {
        QSKIP("Symbolic links are unavailable in this test environment");
    }

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(foreign), QByteArray("foreign payload"));
    QVERIFY(QFileInfo(fixture.workoutTarget).isSymLink());
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QFile::remove(fixture.workoutTarget));
    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(foreign), QByteArray("foreign payload"));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
#endif
}

void TestPlanBundleImportJournal::
standaloneWorkoutRootReplacementFailsClosed()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    std::shared_ptr<PlanBundleImport::Journal> coordinator =
        commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    coordinator.reset();

    const QString ownedRoot = fixture.workoutRoot
        + QStringLiteral("-owned");
    if (!QDir().rename(fixture.workoutRoot, ownedRoot)) {
        QSKIP("The platform did not permit the workout-root swap");
    }
    QVERIFY(createOwnedFixtureHierarchy({fixture.workoutRoot}));
    const QString foreign = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("foreign.txt"));
    QVERIFY(writeFile(foreign, QByteArray("foreign root")));

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot,
        fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(foreign), QByteArray("foreign root"));
    QVERIFY(!QFileInfo::exists(fixture.workoutTarget));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    QVERIFY(QDir(fixture.workoutRoot).removeRecursively());
    QVERIFY(QDir().rename(ownedRoot, fixture.workoutRoot));
    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget),
             QByteArray("standalone workout payload"));
}

void TestPlanBundleImportJournal::
standaloneCreatePreservesExistingTarget()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;

    const auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {createWorkout(QByteArray("replacement"))},
        error);

    QVERIFY(!coordinator);
    QCOMPARE(readFile(fixture.workoutTarget), previous);
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
standaloneOverwriteCompletesAfterRestart()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);
    coordinator.reset();

    int completions = 0;
    bool replacementMetadataObserved = false;
    const auto completion = [
        &database, &completions, &replacementMetadataObserved](
        const TrainDB::PlanImportJournal &journal,
        const PlanBundleImport::PublishedValidation &validatePublished,
        QString &failure) {
        ++completions;
        replacementMetadataObserved = journal.workouts.size() == 1
            && journal.workouts.first().replaceExisting;
        return removeDecision(database)(
            journal, validatePublished, failure);
    };
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot, fixture.workoutRoot,
                 completion, error),
             qPrintable(error));
    QCOMPARE(completions, 1);
    QVERIFY(replacementMetadataObserved);
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
standaloneOverwriteRejectsChangedPredecessor()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    const QByteArray foreign("foreign mutation");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);
    coordinator.reset();
    QVERIFY(writeFile(fixture.workoutTarget, foreign));

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(fixture.workoutTarget), foreign);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
standaloneOverwriteRecoversAfterFilePublication()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);

    const PlanBundleImport::BoundDatabaseCompletion fail = [](
        const TrainDB::PlanImportJournal &,
        const PlanBundleImport::PublishedValidation &,
        QString &failure) {
        failure = QStringLiteral("injected completion failure");
        return false;
    };
    QVERIFY(!coordinator->completePublishedPlan(fail, error));
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
    coordinator.reset();

    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot, fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
partialOverwriteNeverDeletesPublishedTarget()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);

    bool predecessorRemoved = false;
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = [&](const QString &path) {
            if (predecessorRemoved
                || QDir::cleanPath(path)
                    != QDir::cleanPath(fixture.workoutRoot)
                || readFile(fixture.workoutTarget) != replacement) {
                return false;
            }
            const QDir root(fixture.workoutRoot);
            const QFileInfoList entries = root.entryInfoList(
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
            for (const QFileInfo &entry : entries) {
                if (entry.absoluteFilePath() != fixture.workoutTarget
                    && readFile(entry.absoluteFilePath()) == previous) {
                    predecessorRemoved = QFile::remove(
                        entry.absoluteFilePath());
                    break;
                }
            }
            return false;
        };
    }
    const bool published = coordinator->publishPreparedFiles({}, error);
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = {};
    }

    QVERIFY(predecessorRemoved);
    QVERIFY(!published);
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
successfulOverwriteRemovesPredecessorArtifact()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);
    QVERIFY2(coordinator->publishPreparedFiles({}, error), qPrintable(error));

    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(importArtifacts(fixture.workoutRoot).isEmpty());
}

void TestPlanBundleImportJournal::
restartRemovesPersistedPredecessorArtifact()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);

    int publicationSyncFailures = 0;
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = [&](const QString &path) {
            if (QDir::cleanPath(path)
                    == QDir::cleanPath(fixture.workoutRoot)
                && readFile(fixture.workoutTarget) == replacement
                && publicationSyncFailures < 2) {
                ++publicationSyncFailures;
                return true;
            }
            return false;
        };
    }
    QVERIFY(!coordinator->publishPreparedFiles({}, error));
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = {};
    }
    QCOMPARE(publicationSyncFailures, 2);
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(!importArtifacts(fixture.workoutRoot).isEmpty());
    coordinator.reset();

    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot, fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QVERIFY(importArtifacts(fixture.workoutRoot).isEmpty());
    QVERIFY(!decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
restartPreservesSubstitutedPredecessorArtifact()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray previous("existing workout");
    const QByteArray replacement("replacement workout");
    QVERIFY(writeFile(fixture.workoutTarget, previous));
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        {replacementWorkout(previous, replacement)}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);

    int publicationSyncFailures = 0;
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = [&](const QString &path) {
            if (QDir::cleanPath(path)
                    == QDir::cleanPath(fixture.workoutRoot)
                && readFile(fixture.workoutTarget) == replacement
                && publicationSyncFailures < 2) {
                ++publicationSyncFailures;
                return true;
            }
            return false;
        };
    }
    QVERIFY(!coordinator->publishPreparedFiles({}, error));
    {
        const std::lock_guard<std::mutex> lock(anchoredSyncActionMutex);
        anchoredSyncAction = {};
    }
    const QStringList artifacts = importArtifacts(fixture.workoutRoot);
    QCOMPARE(artifacts.size(), 1);
    const QString substitute = QDir(fixture.workoutRoot).filePath(
        QStringLiteral("same-content-substitute"));
    QVERIFY(writeFile(substitute, previous));
    QVERIFY(QFile::remove(artifacts.first()));
    QVERIFY(QFile::rename(substitute, artifacts.first()));
    coordinator.reset();

    error.clear();
    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecision(database), error));
    QCOMPARE(readFile(fixture.workoutTarget), replacement);
    QCOMPARE(readFile(artifacts.first()), previous);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
unboundRecoveryCompletionIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = commitStandaloneDecision(fixture, database, error);
    QVERIFY2(coordinator, qPrintable(error));
    coordinator.reset();

    QVERIFY(!PlanBundleImport::Journal::reconcileAll(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        removeDecisionUnbound(database), error));
    QVERIFY(decisionExists(database, fixture.athleteRoot));
    QVERIFY(!QFileInfo::exists(fixture.workoutTarget));
}

void TestPlanBundleImportJournal::
standaloneMultiTargetPreflightIsMutationFree_data()
{
    QTest::addColumn<bool>("replaceFirstTarget");
    QTest::newRow("create-before-conflict") << false;
    QTest::newRow("overwrite-before-conflict") << true;
}

void TestPlanBundleImportJournal::
standaloneMultiTargetPreflightIsMutationFree()
{
    QFETCH(bool, replaceFirstTarget);
    Fixture fixture;
    QVERIFY(fixture.initialize());

    const QString firstName = QStringLiteral("alpha.erg");
    const QString secondName = QStringLiteral("omega.erg");
    const QString firstPath = QDir(fixture.workoutRoot).filePath(firstName);
    const QString secondPath = QDir(fixture.workoutRoot).filePath(secondName);
    const QByteArray firstPrevious("first predecessor");
    const QByteArray secondPrevious("second predecessor");
    const QByteArray foreignSecond("foreign second replacement");
    if (replaceFirstTarget)
        QVERIFY(writeFile(firstPath, firstPrevious));
    QVERIFY(writeFile(secondPath, secondPrevious));

    QList<TrainDB::PlanImportWorkout> workouts;
    workouts.append(replaceFirstTarget
        ? replacementWorkoutNamed(
            firstName, firstPrevious, QByteArray("first publication"))
        : createWorkoutNamed(
            firstName, QByteArray("first publication")));
    workouts.append(replacementWorkoutNamed(
        secondName, secondPrevious, QByteArray("second publication")));

    TrainDB database{QDir(fixture.databaseRoot)};
    QString error;
    auto coordinator = PlanBundleImport::Journal::createStandalone(
        &database, fixture.athleteRoot, fixture.workoutRoot,
        workouts, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(committed, error),
             qPrintable(error));
    QVERIFY(committed);
    QVERIFY(writeFile(secondPath, foreignSecond));

    QVERIFY(!coordinator->completePublishedPlan(
        removeDecision(database), error));
    if (replaceFirstTarget) {
        QCOMPARE(readFile(firstPath), firstPrevious);
    } else {
        QVERIFY(!QFileInfo::exists(firstPath));
    }
    QCOMPARE(readFile(secondPath), foreignSecond);
    QVERIFY(decisionExists(database, fixture.athleteRoot));
}

void TestPlanBundleImportJournal::
cancellablePublicationKeepsGuiResponsive()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    TrainDB database{QDir(fixture.databaseRoot)};
    TrainDB::PlanImportWorkout workout = createWorkout(
        QByteArray(4 * 1024 * 1024, 'x'));
    workout.digest = QCryptographicHash::hash(
        workout.contents, QCryptographicHash::Sha256);
    QString error;
    auto coordinator =
        PlanBundleImport::Journal::createStandalonePrepared(
            fixture.athleteRoot, fixture.workoutRoot,
            {workout}, error);
    QVERIFY2(coordinator, qPrintable(error));
    bool committed = false;
    QVERIFY2(coordinator->commitStandaloneDecision(
                 &database, committed, error),
             qPrintable(error));
    QVERIFY(committed);

    publicationChunkVisits.store(0, std::memory_order_release);
    publicationChunkDelayMs.store(20, std::memory_order_release);
    std::atomic_bool cancelled{false};
    QString publicationError;
    auto publication = std::async(std::launch::async, [&] {
        return coordinator->publishPreparedFiles(
            [&] { return cancelled.load(std::memory_order_acquire); },
            publicationError);
    });

    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        publicationChunkVisits.load(std::memory_order_acquire) > 0,
        500);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeats > 0, 100);
    QElapsedTimer elapsed;
    elapsed.start();
    cancelled.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(
        publication.wait_for(0ms) == std::future_status::ready,
        300);
    QVERIFY(elapsed.elapsed() < 250);
    QVERIFY(!publication.get());
    QVERIFY(publicationError.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY(!QFileInfo::exists(fixture.workoutTarget));
    QVERIFY(decisionExists(database, fixture.athleteRoot));

    publicationChunkDelayMs.store(0, std::memory_order_release);
    coordinator.reset();
    error.clear();
    QVERIFY2(PlanBundleImport::Journal::reconcileAll(
                 &database, fixture.athleteRoot,
                 fixture.workoutRoot,
                 removeDecision(database), error),
             qPrintable(error));
}

QTEST_GUILESS_MAIN(TestPlanBundleImportJournal)

#include "testPlanBundleImportJournal.moc"
