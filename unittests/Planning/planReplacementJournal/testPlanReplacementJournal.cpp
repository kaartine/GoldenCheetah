#include <QtTest>

#include "PlanReplacementJournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <cstdlib>
#include <memory>

namespace {

constexpr int CrashExitCode = 96;
const char CrashRootEnvironment[] = "GC_PLAN_REPLACEMENT_CRASH_ROOT";
const char CrashActionEnvironment[] = "GC_PLAN_REPLACEMENT_CRASH_ACTION";
const char CrashPhaseEnvironment[] = "GC_PLAN_REPLACEMENT_CRASH_PHASE";
const char CrashOccurrenceEnvironment[] =
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

bool createTestSymbolicLink(
    const QString &targetPath,
    const QString &linkPath)
{
#ifdef Q_OS_WIN
    const QString nativeTarget = QDir::toNativeSeparators(targetPath);
    const QString nativeLink = QDir::toNativeSeparators(linkPath);
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (::CreateSymbolicLinkW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
            reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        return true;
    }
    if (::GetLastError() != ERROR_INVALID_PARAMETER) return false;
#endif
    return ::CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
        0);
#else
    return QFile::link(targetPath, linkPath);
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
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

struct PlanPaths
{
    QString scope;
    QString oldOne;
    QString oldTwo;
    QString newOne;
    QString newTwo;
};

struct RootScopePaths
{
    QString plannedRoot;
    QString backupRoot;
    QString oldPlan;
    QString newPlan;
    QString backup;
};

PlanPaths planPaths(const QString &root)
{
    PlanPaths paths;
    paths.scope = QDir(root).filePath(QStringLiteral("planned"));
    paths.oldOne = QDir(paths.scope).filePath(
        QStringLiteral("old-one.json"));
    paths.oldTwo = QDir(paths.scope).filePath(
        QStringLiteral("old-two.json"));
    paths.newOne = QDir(paths.scope).filePath(
        QStringLiteral("new-one.json"));
    paths.newTwo = QDir(paths.scope).filePath(
        QStringLiteral("new-two.json"));
    return paths;
}

RootScopePaths rootScopePaths(const QString &root)
{
    RootScopePaths paths;
    paths.plannedRoot = QDir(root).filePath(
        QStringLiteral("planned"));
    paths.backupRoot = QDir(root).filePath(
        QStringLiteral("bak/planned"));
    paths.oldPlan = QDir(paths.plannedRoot).filePath(
        QStringLiteral("old.json"));
    paths.newPlan = QDir(paths.plannedRoot).filePath(
        QStringLiteral("new.json"));
    paths.backup = QDir(paths.backupRoot).filePath(
        QStringLiteral("old.json.bak"));
    return paths;
}

bool createOldGeneration(const QString &root)
{
    const PlanPaths paths = planPaths(root);
    return QDir().mkpath(paths.scope)
        && writeFile(paths.oldOne, QByteArray("old generation one"))
        && writeFile(paths.oldTwo, QByteArray("old generation two"));
}

bool createCrashGeneration(
    const QString &root, const QString &scenario)
{
    if (scenario != QStringLiteral("root-scope"))
        return createOldGeneration(root);

    const RootScopePaths paths = rootScopePaths(root);
    return QDir().mkpath(paths.plannedRoot)
        && QDir().mkpath(paths.backupRoot)
        && writeFile(paths.oldPlan, QByteArray("old plan"))
        && writeFile(paths.backup, QByteArray("previous backup"));
}

PlanReplacement::Specification replacementSpecification(
    const QString &root)
{
    const PlanPaths paths = planPaths(root);
    PlanReplacement::Specification specification;
    specification.athleteRoot = root;
    specification.scopeRoot = paths.scope;
    specification.inputPaths = {paths.oldOne, paths.oldTwo};
    specification.removalPaths = {paths.oldOne, paths.oldTwo};
    specification.targetPaths = {paths.newOne, paths.newTwo};
    return specification;
}

PlanReplacement::Specification crashSpecification(
    const QString &root, const QString &scenario)
{
    if (scenario == QStringLiteral("normal"))
        return replacementSpecification(root);

    if (scenario == QStringLiteral("overlap")) {
        const PlanPaths paths = planPaths(root);
        PlanReplacement::Specification specification;
        specification.athleteRoot = root;
        specification.scopeRoot = paths.scope;
        specification.inputPaths = {paths.oldOne, paths.oldTwo};
        specification.removalPaths = {paths.oldOne, paths.oldTwo};
        specification.targetPaths = {paths.oldOne, paths.newTwo};
        return specification;
    }

    const RootScopePaths paths = rootScopePaths(root);
    PlanReplacement::Specification specification;
    specification.athleteRoot = root;
    specification.scopeRoot = root;
    specification.inputPaths = {paths.oldPlan};
    specification.removalPaths = {paths.oldPlan, paths.backup};
    specification.targetPaths = {paths.newPlan, paths.backup};
    return specification;
}

bool stageNewGeneration(
    const std::shared_ptr<PlanReplacement::Journal> &journal,
    QString &error)
{
    return journal && journal->targetCount() == 2
        && writeFile(
            journal->stagingPath(0), QByteArray("new generation one"))
        && journal->recordStaged(0, error)
        && writeFile(
            journal->stagingPath(1), QByteArray("new generation two"))
        && journal->recordStaged(1, error);
}

bool stageCrashGeneration(
    const std::shared_ptr<PlanReplacement::Journal> &journal,
    const QString &scenario,
    QString &error)
{
    if (scenario == QStringLiteral("normal"))
        return stageNewGeneration(journal, error);

    const QByteArray first = scenario == QStringLiteral("overlap")
        ? QByteArray("replaced in place")
        : QByteArray("new plan");
    const QByteArray second = scenario == QStringLiteral("overlap")
        ? QByteArray("new second path")
        : QByteArray("old plan");
    return journal && journal->targetCount() == 2
        && writeFile(journal->stagingPath(0), first)
        && journal->recordStaged(0, error)
        && writeFile(journal->stagingPath(1), second)
        && journal->recordStaged(1, error);
}

bool verifyCrashGeneration(
    const QString &root, const QString &scenario,
    bool newGeneration)
{
    if (scenario == QStringLiteral("root-scope")) {
        const RootScopePaths paths = rootScopePaths(root);
        if (newGeneration) {
            return !QFileInfo::exists(paths.oldPlan)
                && readFile(paths.newPlan) == QByteArray("new plan")
                && readFile(paths.backup) == QByteArray("old plan");
        }
        return readFile(paths.oldPlan) == QByteArray("old plan")
            && !QFileInfo::exists(paths.newPlan)
            && readFile(paths.backup)
                == QByteArray("previous backup");
    }

    const PlanPaths paths = planPaths(root);
    if (scenario == QStringLiteral("overlap")) {
        if (newGeneration) {
            return readFile(paths.oldOne)
                    == QByteArray("replaced in place")
                && !QFileInfo::exists(paths.oldTwo)
                && readFile(paths.newTwo)
                    == QByteArray("new second path");
        }
        return readFile(paths.oldOne)
                == QByteArray("old generation one")
            && readFile(paths.oldTwo)
                == QByteArray("old generation two")
            && !QFileInfo::exists(paths.newTwo);
    }

    if (newGeneration) {
        return !QFileInfo::exists(paths.oldOne)
            && !QFileInfo::exists(paths.oldTwo)
            && readFile(paths.newOne)
                == QByteArray("new generation one")
            && readFile(paths.newTwo)
                == QByteArray("new generation two");
    }
    return readFile(paths.oldOne) == QByteArray("old generation one")
        && readFile(paths.oldTwo) == QByteArray("old generation two")
        && !QFileInfo::exists(paths.newOne)
        && !QFileInfo::exists(paths.newTwo);
}

void addCrashRecoveryRows(
    const QString &scenario, int oldRemovalCount)
{
    const auto row = [&](const QString &name,
                         const char *phase,
                         int occurrence,
                         const char *action,
                         bool newGeneration) {
        const QByteArray rowName = QStringLiteral("%1-%2")
            .arg(scenario, name).toLatin1();
        QTest::newRow(rowName.constData())
            << scenario << QString::fromLatin1(phase)
            << occurrence << QString::fromLatin1(action)
            << newGeneration;
    };
    row(QStringLiteral("directory-created"),
        "plan-replacement-directory-created", 1,
        "publish", false);
    row(QStringLiteral("first-old-copy"),
        "plan-replacement-old-copy-published", 1,
        "publish", false);
    row(QStringLiteral("second-old-copy"),
        "plan-replacement-old-copy-published", 2,
        "publish", false);
    row(QStringLiteral("initial-manifest"),
        "plan-replacement-initial-manifest-published", 1,
        "publish", false);
    row(QStringLiteral("first-stage-recorded"),
        "plan-replacement-stage-recorded", 1,
        "publish", false);
    row(QStringLiteral("second-stage-recorded"),
        "plan-replacement-stage-recorded", 2,
        "publish", false);
    row(QStringLiteral("first-target-published"),
        "plan-replacement-target-published", 1,
        "publish", false);
    row(QStringLiteral("second-target-published"),
        "plan-replacement-target-published", 2,
        "publish", false);
    for (int occurrence = 1;
         occurrence <= oldRemovalCount; ++occurrence) {
        row(QStringLiteral("old-removed-%1").arg(occurrence),
            "plan-replacement-old-removed", occurrence,
            "publish", false);
    }
    row(QStringLiteral("commit-marker"),
        "plan-replacement-commit-marker", 1,
        "publish", true);
    row(QStringLiteral("commit-manifest-removed"),
        "plan-replacement-manifest-removed", 1,
        "commit-cleanup", true);
    for (int occurrence = 1; occurrence <= 4; ++occurrence) {
        row(QStringLiteral("commit-cleanup-file-%1").arg(occurrence),
            "plan-replacement-cleanup-file", occurrence,
            "commit-cleanup", true);
    }
    row(QStringLiteral("commit-marker-removed"),
        "plan-replacement-commit-marker-removed", 1,
        "commit-cleanup", true);
    row(QStringLiteral("commit-directory-removed"),
        "plan-replacement-directory-removed", 1,
        "commit-cleanup", true);
    row(QStringLiteral("rollback-manifest-removed"),
        "plan-replacement-manifest-removed", 1,
        "rollback-cleanup", false);
    for (int occurrence = 1; occurrence <= 4; ++occurrence) {
        row(QStringLiteral("rollback-cleanup-file-%1").arg(occurrence),
            "plan-replacement-cleanup-file", occurrence,
            "rollback-cleanup", false);
    }
    row(QStringLiteral("rollback-directory-removed"),
        "plan-replacement-directory-removed", 1,
        "rollback-cleanup", false);
}

bool journalNamespaceIsEmpty(const QString &root)
{
    const QDir journalRoot(QDir(root).filePath(
        QStringLiteral(".gc-transactions/plan-replacement")));
    return !journalRoot.exists()
        || journalRoot.entryInfoList(
               QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
               .isEmpty();
}

QPair<int, QByteArray> runChild(
    const QString &testName,
    const QString &root,
    const QString &action,
    const QString &phase,
    int occurrence)
{
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QString::fromLatin1(CrashPhaseEnvironment));
    environment.remove(QString::fromLatin1(CrashOccurrenceEnvironment));
    environment.insert(QString::fromLatin1(CrashRootEnvironment), root);
    environment.insert(QString::fromLatin1(CrashActionEnvironment), action);
    environment.insert(QString::fromLatin1(CrashPhaseEnvironment), phase);
    environment.insert(
        QString::fromLatin1(CrashOccurrenceEnvironment),
        QString::number(occurrence));
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {testName});
    if (!child.waitForStarted(5000)) {
        return qMakePair(-1, child.errorString().toUtf8());
    }
    if (!child.waitForFinished(15000)) {
        child.kill();
        child.waitForFinished();
        return qMakePair(-2, QByteArray("child timed out"));
    }
    return qMakePair(child.exitCode(), child.readAll());
}

enum class SpecificationFailure
{
    EmptyTargets,
    RelativeTarget,
    OutsideRoot,
    OutsideScope,
    DuplicateTarget,
    DuplicateRemoval,
    ExistingNewTarget,
    MissingRemoval,
    MissingInput,
    SymlinkScope
};

enum class ManifestTamper
{
    FractionalVersion,
    UnknownRootKey,
    WrongIdentifier,
    TraversalPath,
    DuplicatePath,
    DuplicateStageIndex,
    NonContiguousStageIndex,
    OutOfRangeStageIndex,
    EscapedScope,
    InvalidDigest,
    NoOpEntry
};

enum class DirectoryTamper
{
    UnknownFile,
    NestedDirectory,
    Symlink,
    OversizedCommitMarker,
    OversizedLockFile
};

enum class DataTamper
{
    OldCopy,
    StagedCopy,
    OldGeneration,
    CommittedGeneration
};

} // namespace

void planReplacementTransitionReached(const char *transition)
{
    const QByteArray requested = qgetenv(CrashPhaseEnvironment);
    if (requested.isEmpty() || requested != transition) return;
    static QHash<QByteArray, int> occurrences;
    const int occurrence = ++occurrences[requested];
    bool valid = false;
    const int requestedOccurrence = qEnvironmentVariableIntValue(
        CrashOccurrenceEnvironment, &valid);
    if (occurrence == (valid ? requestedOccurrence : 1)) {
        std::_Exit(CrashExitCode);
    }
}

class TestPlanReplacementJournal : public QObject
{
    Q_OBJECT

private slots:
    void crashRecoveryIsGenerationAtomic_data();
    void crashRecoveryIsGenerationAtomic();
    void successfulReplacementSupportsOverlappingPaths();
    void successfulReplacementSupportsAthleteRootScope();
    void incompleteStagingPreservesOldGeneration();
    void unrecordedStageIsDiscardedDuringRecovery();
    void committedCoordinatorCanResumePreparedJournal();
    void staleQLockFileRemovalGuardDoesNotPoisonReconcile_data();
    void staleQLockFileRemovalGuardDoesNotPoisonReconcile();
    void staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile_data();
    void staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile();
    void unsafeQLockFileRemovalGuardEntriesRemainRejected_data();
    void unsafeQLockFileRemovalGuardEntriesRemainRejected();
    void rejectsUnsafeSpecifications_data();
    void rejectsUnsafeSpecifications();
    void allowsSymlinkRootWithoutTransactionNamespace();
    void concurrentReplacementIsRejected();
    void pendingSiblingTransactionIsRejected_data();
    void pendingSiblingTransactionIsRejected();
    void manifestTamperingIsFailClosed_data();
    void manifestTamperingIsFailClosed();
    void journalDirectoryTamperingIsFailClosed_data();
    void journalDirectoryTamperingIsFailClosed();
    void transactionDataTamperingIsFailClosed_data();
    void transactionDataTamperingIsFailClosed();
    void invalidCommitMarkerIsNotReportedCommitted();
    void journalDirectoriesArePrivate();
};

void TestPlanReplacementJournal::crashRecoveryIsGenerationAtomic_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("occurrence");
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("newGeneration");

    addCrashRecoveryRows(QStringLiteral("normal"), 2);
    addCrashRecoveryRows(QStringLiteral("overlap"), 1);
    addCrashRecoveryRows(QStringLiteral("root-scope"), 1);
}

void TestPlanReplacementJournal::crashRecoveryIsGenerationAtomic()
{
    QFETCH(QString, scenario);
    QFETCH(QString, phase);
    QFETCH(int, occurrence);
    QFETCH(QString, action);
    QFETCH(bool, newGeneration);

    const QString childRoot = qEnvironmentVariable(CrashRootEnvironment);
    if (!childRoot.isEmpty()) {
        QString error;
        std::shared_ptr<PlanReplacement::Journal> journal =
            PlanReplacement::Journal::prepare(
                crashSpecification(childRoot, scenario), error);
        if (!stageCrashGeneration(journal, scenario, error))
            std::_Exit(98);
        if (action == QStringLiteral("rollback-cleanup")) {
            if (!journal->cleanupAfterRollback(error)) std::_Exit(99);
        } else {
            if (!journal->publishAndCommit(error)) std::_Exit(100);
            if (action == QStringLiteral("commit-cleanup")
                && !journal->cleanupAfterCommit(error)) {
                std::_Exit(101);
            }
        }
        std::_Exit(102);
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createCrashGeneration(temporary.path(), scenario));
    const QString testName = QStringLiteral(
        "crashRecoveryIsGenerationAtomic:%1")
                                 .arg(QString::fromLatin1(
                                     QTest::currentDataTag()));
    const auto crashed = runChild(
        testName, temporary.path(), action, phase, occurrence);
    QCOMPARE(crashed.first, CrashExitCode);

    QString error;
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
    QVERIFY(verifyCrashGeneration(
        temporary.path(), scenario, newGeneration));
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
    QVERIFY(journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::
successfulReplacementSupportsOverlappingPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    const PlanPaths paths = planPaths(temporary.path());

    PlanReplacement::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.scopeRoot = paths.scope;
    specification.inputPaths = {paths.oldOne, paths.oldTwo};
    specification.removalPaths = {paths.oldOne, paths.oldTwo};
    specification.targetPaths = {paths.oldOne, paths.newTwo};

    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY(writeFile(journal->stagingPath(0), QByteArray("replaced in place")));
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    QVERIFY(writeFile(journal->stagingPath(1), QByteArray("new second path")));
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));

    QCOMPARE(readFile(paths.oldOne), QByteArray("replaced in place"));
    QVERIFY(!QFileInfo::exists(paths.oldTwo));
    QCOMPARE(readFile(paths.newTwo), QByteArray("new second path"));
    QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));
    QVERIFY(journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::
successfulReplacementSupportsAthleteRootScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString plannedRoot = QDir(temporary.path()).filePath(
        QStringLiteral("planned"));
    const QString backupRoot = QDir(temporary.path()).filePath(
        QStringLiteral("bak/planned"));
    QVERIFY(QDir().mkpath(plannedRoot));
    QVERIFY(QDir().mkpath(backupRoot));
    const QString oldPlan = QDir(plannedRoot).filePath(
        QStringLiteral("old.json"));
    const QString newPlan = QDir(plannedRoot).filePath(
        QStringLiteral("new.json"));
    const QString backup = QDir(backupRoot).filePath(
        QStringLiteral("old.json.bak"));
    QVERIFY(writeFile(oldPlan, QByteArray("old plan")));
    QVERIFY(writeFile(backup, QByteArray("previous backup")));

    PlanReplacement::Specification specification;
    specification.athleteRoot = temporary.path();
    specification.scopeRoot = temporary.path();
    specification.inputPaths = {oldPlan};
    specification.removalPaths = {oldPlan, backup};
    specification.targetPaths = {newPlan, backup};
    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(journal, qPrintable(error));
    QCOMPARE(journal->targetCount(), 2);
    QVERIFY(writeFile(
        journal->stagingPath(0), QByteArray("new plan")));
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    QVERIFY(writeFile(
        journal->stagingPath(1), QByteArray("old plan")));
    QVERIFY2(journal->recordStaged(1, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));

    QVERIFY(!QFileInfo::exists(oldPlan));
    QCOMPARE(readFile(newPlan), QByteArray("new plan"));
    QCOMPARE(readFile(backup), QByteArray("old plan"));
}

void TestPlanReplacementJournal::incompleteStagingPreservesOldGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY(writeFile(journal->stagingPath(0), QByteArray("partial new plan")));
    QVERIFY2(journal->recordStaged(0, error), qPrintable(error));
    QVERIFY(!journal->publishAndCommit(error));

    const PlanPaths paths = planPaths(temporary.path());
    QCOMPARE(readFile(paths.oldOne), QByteArray("old generation one"));
    QCOMPARE(readFile(paths.oldTwo), QByteArray("old generation two"));
    QVERIFY(!QFileInfo::exists(paths.newOne));
    QVERIFY(!QFileInfo::exists(paths.newTwo));
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
}

void TestPlanReplacementJournal::unrecordedStageIsDiscardedDuringRecovery()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY(writeFile(
        journal->stagingPath(0), QByteArray("unrecorded stage")));
    journal.reset();

    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
    const PlanPaths paths = planPaths(temporary.path());
    QCOMPARE(readFile(paths.oldOne), QByteArray("old generation one"));
    QCOMPARE(readFile(paths.oldTwo), QByteArray("old generation two"));
    QVERIFY(!QFileInfo::exists(paths.newOne));
    QVERIFY(!QFileInfo::exists(paths.newTwo));
    QVERIFY(journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::
committedCoordinatorCanResumePreparedJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY2(stageNewGeneration(journal, error), qPrintable(error));
    const QString transactionId =
        QFileInfo(journal->directoryPath()).fileName();
    QVERIFY(!transactionId.isEmpty());
    journal.reset();

    journal = PlanReplacement::Journal::openPrepared(
        temporary.path(), transactionId, error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    QVERIFY(journal->hasCommitMarker());
    QVERIFY(verifyCrashGeneration(
        temporary.path(), QStringLiteral("normal"), true));
    QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));
    QVERIFY(journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::
staleQLockFileRemovalGuardDoesNotPoisonReconcile_data()
{
    QTest::addColumn<int>("suffixCount");
    QTest::newRow("single-rmlock") << 1;
    QTest::newRow("nested-rmlock") << 2;
}

void TestPlanReplacementJournal::
staleQLockFileRemovalGuardDoesNotPoisonReconcile()
{
    QFETCH(int, suffixCount);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    const QString namespacePath = QDir(temporary.path()).filePath(
        QStringLiteral(".gc-transactions/plan-replacement"));
    QVERIFY(QDir().mkpath(namespacePath));
    QVERIFY(writeFile(
        QDir(namespacePath).filePath(
            qlockRemovalGuardName(suffixCount)),
        QByteArray("stale QLockFile removal guard")));

    QString error;
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
    error.clear();
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));

    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
}

void TestPlanReplacementJournal::
staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile_data()
{
    QTest::addColumn<QString>("lockName");
    QTest::addColumn<int>("suffixCount");
    QTest::addColumn<bool>("removeManifest");
    QTest::newRow("manifest-single-rmlock")
        << QStringLiteral(".manifest.json.lock") << 1 << false;
    QTest::newRow("manifest-nested-rmlock")
        << QStringLiteral(".manifest.json.lock") << 2 << false;
    QTest::newRow("commit-marker-rmlock")
        << QStringLiteral(".COMMITTED.lock") << 1 << false;
    QTest::newRow("stage-rmlock")
        << QStringLiteral(".new-0000.stage.lock") << 1 << false;
    QTest::newRow("pre-manifest-stage-rmlock")
        << QStringLiteral(".new-0000.stage.lock") << 1 << true;
}

void TestPlanReplacementJournal::
staleJournalQLockFileRemovalGuardDoesNotPoisonReconcile()
{
    QFETCH(QString, lockName);
    QFETCH(int, suffixCount);
    QFETCH(bool, removeManifest);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));

    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    QVERIFY(writeFile(
        QDir(journalPath).filePath(qlockRemovalGuardName(
            lockName, suffixCount)),
        QByteArray("stale journal QLockFile removal guard")));
    journal.reset();
    if (removeManifest) {
        QVERIFY(QFile::remove(
            QDir(journalPath).filePath(QStringLiteral("manifest.json"))));
    }

    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(journalPath));
    error.clear();
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(temporary.path(), error),
        qPrintable(error));
}

void TestPlanReplacementJournal::
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

void TestPlanReplacementJournal::
unsafeQLockFileRemovalGuardEntriesRemainRejected()
{
    QFETCH(QString, entryName);
    QFETCH(int, entryKind);
    const JournalNamespaceEntryKind kind =
        JournalNamespaceEntryKind(entryKind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString namespacePath = QDir(temporary.path()).filePath(
        QStringLiteral(".gc-transactions/plan-replacement"));
    QVERIFY(QDir().mkpath(namespacePath));
    const QString entryPath = QDir(namespacePath).filePath(entryName);

    if (kind == JournalNamespaceEntryKind::Directory) {
        QVERIFY(QDir().mkpath(entryPath));
    } else if (kind == JournalNamespaceEntryKind::SymbolicLink) {
        const QString targetPath = QDir(temporary.path()).filePath(
            QStringLiteral("qlock-removal-guard-target"));
        QVERIFY(writeFile(targetPath, QByteArray("target")));
        if (!createTestSymbolicLink(targetPath, entryPath))
            QSKIP("File symbolic links are unavailable");
    } else {
        QVERIFY(writeFile(entryPath, QByteArray("lookalike")));
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        QString error;
        QVERIFY(!PlanReplacement::Journal::reconcileAll(
            temporary.path(), error));
        QVERIFY2(error.contains(entryName), qPrintable(error));
    }
}

void TestPlanReplacementJournal::rejectsUnsafeSpecifications_data()
{
    QTest::addColumn<int>("failure");
    QTest::newRow("empty-targets")
        << int(SpecificationFailure::EmptyTargets);
    QTest::newRow("relative-target")
        << int(SpecificationFailure::RelativeTarget);
    QTest::newRow("outside-root")
        << int(SpecificationFailure::OutsideRoot);
    QTest::newRow("outside-scope")
        << int(SpecificationFailure::OutsideScope);
    QTest::newRow("duplicate-target")
        << int(SpecificationFailure::DuplicateTarget);
    QTest::newRow("duplicate-removal")
        << int(SpecificationFailure::DuplicateRemoval);
    QTest::newRow("existing-new-target")
        << int(SpecificationFailure::ExistingNewTarget);
    QTest::newRow("missing-removal")
        << int(SpecificationFailure::MissingRemoval);
    QTest::newRow("missing-input")
        << int(SpecificationFailure::MissingInput);
    QTest::newRow("symlink-scope")
        << int(SpecificationFailure::SymlinkScope);
}

void TestPlanReplacementJournal::rejectsUnsafeSpecifications()
{
    QFETCH(int, failure);
    const SpecificationFailure scenario = SpecificationFailure(failure);
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    const PlanPaths paths = planPaths(temporary.path());
    PlanReplacement::Specification specification =
        replacementSpecification(temporary.path());

    switch (scenario) {
    case SpecificationFailure::EmptyTargets:
        specification.targetPaths.clear();
        break;
    case SpecificationFailure::RelativeTarget:
        specification.targetPaths[0] = QStringLiteral("relative.json");
        break;
    case SpecificationFailure::OutsideRoot:
        specification.targetPaths[0] = outside.filePath(
            QStringLiteral("outside.json"));
        break;
    case SpecificationFailure::OutsideScope: {
        const QString other = temporary.filePath(QStringLiteral("other"));
        QVERIFY(QDir().mkpath(other));
        specification.targetPaths[0] = QDir(other).filePath(
            QStringLiteral("outside-scope.json"));
        break;
    }
    case SpecificationFailure::DuplicateTarget:
        specification.targetPaths[1] = specification.targetPaths[0];
        break;
    case SpecificationFailure::DuplicateRemoval:
        specification.removalPaths[1] = specification.removalPaths[0];
        break;
    case SpecificationFailure::ExistingNewTarget:
        QVERIFY(writeFile(paths.newOne, QByteArray("unrelated existing")));
        break;
    case SpecificationFailure::MissingRemoval:
        QVERIFY(QFile::remove(paths.oldOne));
        break;
    case SpecificationFailure::MissingInput:
        specification.inputPaths.append(
            QDir(paths.scope).filePath(QStringLiteral("missing.json")));
        break;
    case SpecificationFailure::SymlinkScope: {
#ifdef Q_OS_WIN
        QSKIP("Symbolic-link creation is not generally available on Windows");
#else
        const QString linkedScope = temporary.filePath(
            QStringLiteral("linked-planned"));
        QVERIFY(QFile::link(paths.scope, linkedScope));
        specification.scopeRoot = linkedScope;
        specification.targetPaths[0] = QDir(linkedScope).filePath(
            QStringLiteral("new-one.json"));
        specification.targetPaths[1] = QDir(linkedScope).filePath(
            QStringLiteral("new-two.json"));
        break;
#endif
    }
    }

    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY(!journal);
    QVERIFY(!error.isEmpty());
    if (QFileInfo::exists(paths.oldOne)) {
        QCOMPARE(readFile(paths.oldOne), QByteArray("old generation one"));
    }
    if (QFileInfo::exists(paths.oldTwo)) {
        QCOMPARE(readFile(paths.oldTwo), QByteArray("old generation two"));
    }
}

void TestPlanReplacementJournal::
allowsSymlinkRootWithoutTransactionNamespace()
{
#ifdef Q_OS_WIN
    QSKIP("Symbolic-link creation is not generally available on Windows");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString realRoot = temporary.filePath(QStringLiteral("real"));
    const QString linkedRoot = temporary.filePath(QStringLiteral("linked"));
    QVERIFY(QDir().mkpath(realRoot));
    QVERIFY(QFile::link(realRoot, linkedRoot));
    QString error;
    QVERIFY2(
        PlanReplacement::Journal::reconcileAll(linkedRoot, error),
        qPrintable(error));
#endif
}

void TestPlanReplacementJournal::concurrentReplacementIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    const PlanReplacement::Specification specification =
        replacementSpecification(temporary.path());
    std::shared_ptr<PlanReplacement::Journal> first =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(first, qPrintable(error));
    const std::shared_ptr<PlanReplacement::Journal> second =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY(!second);
    QVERIFY(!error.isEmpty());
    QVERIFY2(first->cleanupAfterRollback(error), qPrintable(error));
    first.reset();
    const std::shared_ptr<PlanReplacement::Journal> afterCleanup =
        PlanReplacement::Journal::prepare(specification, error);
    QVERIFY2(afterCleanup, qPrintable(error));
    QVERIFY2(afterCleanup->cleanupAfterRollback(error), qPrintable(error));
}

void TestPlanReplacementJournal::pendingSiblingTransactionIsRejected_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("linked-save") << QStringLiteral("linked-save");
    QTest::newRow("linked-removal") << QStringLiteral("linked-removal");
}

void TestPlanReplacementJournal::pendingSiblingTransactionIsRejected()
{
    QFETCH(QString, name);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    const QString sibling = QDir(temporary.path()).filePath(
        QStringLiteral(".gc-transactions/%1").arg(name));
    QVERIFY(QDir().mkpath(QDir(sibling).filePath(QStringLiteral("pending"))));

    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY(!journal);
    QVERIFY(!error.isEmpty());
}

void TestPlanReplacementJournal::manifestTamperingIsFailClosed_data()
{
    QTest::addColumn<int>("tamper");
    QTest::newRow("fractional-version") << int(ManifestTamper::FractionalVersion);
    QTest::newRow("unknown-root-key") << int(ManifestTamper::UnknownRootKey);
    QTest::newRow("wrong-identifier") << int(ManifestTamper::WrongIdentifier);
    QTest::newRow("traversal-path") << int(ManifestTamper::TraversalPath);
    QTest::newRow("duplicate-path") << int(ManifestTamper::DuplicatePath);
    QTest::newRow("duplicate-stage-index")
        << int(ManifestTamper::DuplicateStageIndex);
    QTest::newRow("noncontiguous-stage-index")
        << int(ManifestTamper::NonContiguousStageIndex);
    QTest::newRow("out-of-range-stage-index")
        << int(ManifestTamper::OutOfRangeStageIndex);
    QTest::newRow("escaped-scope") << int(ManifestTamper::EscapedScope);
    QTest::newRow("invalid-digest") << int(ManifestTamper::InvalidDigest);
    QTest::newRow("no-op-entry") << int(ManifestTamper::NoOpEntry);
}

void TestPlanReplacementJournal::manifestTamperingIsFailClosed()
{
    QFETCH(int, tamper);
    const ManifestTamper scenario = ManifestTamper(tamper);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(stageNewGeneration(journal, error), qPrintable(error));
    const QString journalPath = journal->directoryPath();
    journal.reset();

    const QString manifestPath = QDir(journalPath).filePath(
        QStringLiteral("manifest.json"));
    QJsonDocument document = QJsonDocument::fromJson(readFile(manifestPath));
    QVERIFY(document.isObject());
    QJsonObject root = document.object();
    QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    QVERIFY(entries.size() >= 2);

    switch (scenario) {
    case ManifestTamper::FractionalVersion:
        root.insert(QStringLiteral("version"), 1.5);
        break;
    case ManifestTamper::UnknownRootKey:
        root.insert(QStringLiteral("unexpected"), true);
        break;
    case ManifestTamper::WrongIdentifier:
        root.insert(
            QStringLiteral("id"),
            QStringLiteral("00000000-0000-4000-8000-000000000001"));
        break;
    case ManifestTamper::TraversalPath: {
        QJsonObject entry = entries.at(0).toObject();
        entry.insert(QStringLiteral("path"), QStringLiteral("../escape.json"));
        entries[0] = entry;
        break;
    }
    case ManifestTamper::DuplicatePath: {
        QJsonObject second = entries.at(1).toObject();
        second.insert(
            QStringLiteral("path"),
            entries.at(0).toObject().value(QStringLiteral("path")));
        entries[1] = second;
        break;
    }
    case ManifestTamper::DuplicateStageIndex: {
        QJsonObject second = entries.at(1).toObject();
        second.insert(QStringLiteral("stage_index"), 0);
        entries[1] = second;
        break;
    }
    case ManifestTamper::NonContiguousStageIndex: {
        QJsonObject second = entries.at(1).toObject();
        second.insert(QStringLiteral("stage_index"), 2);
        entries[1] = second;
        break;
    }
    case ManifestTamper::OutOfRangeStageIndex: {
        QJsonObject second = entries.at(1).toObject();
        second.insert(QStringLiteral("stage_index"), 1e100);
        entries[1] = second;
        break;
    }
    case ManifestTamper::EscapedScope:
        root.insert(QStringLiteral("scope"), QStringLiteral("other"));
        break;
    case ManifestTamper::InvalidDigest: {
        QJsonObject entry = entries.at(0).toObject();
        entry.insert(QStringLiteral("new_sha256"), QString(64, QLatin1Char('0')));
        entries[0] = entry;
        break;
    }
    case ManifestTamper::NoOpEntry: {
        QJsonObject entry = entries.at(0).toObject();
        entry.insert(QStringLiteral("old_exists"), false);
        entry.insert(QStringLiteral("old_size"), QStringLiteral("0"));
        entry.insert(QStringLiteral("old_sha256"), QString());
        entry.insert(QStringLiteral("has_new"), false);
        entry.insert(QStringLiteral("stage_index"), -1);
        entry.insert(QStringLiteral("staged"), false);
        entry.insert(QStringLiteral("new_size"), QStringLiteral("0"));
        entry.insert(QStringLiteral("new_sha256"), QString());
        entries[0] = entry;
        break;
    }
    }
    root.insert(QStringLiteral("entries"), entries);
    QVERIFY(writeFile(
        manifestPath,
        QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n'));

    QVERIFY(!PlanReplacement::Journal::reconcileAll(temporary.path(), error));
    QVERIFY(!error.isEmpty());
    const PlanPaths paths = planPaths(temporary.path());
    QCOMPARE(readFile(paths.oldOne), QByteArray("old generation one"));
    QCOMPARE(readFile(paths.oldTwo), QByteArray("old generation two"));
    QVERIFY(!QFileInfo::exists(paths.newOne));
    QVERIFY(!QFileInfo::exists(paths.newTwo));
    QVERIFY(!journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::
invalidCommitMarkerIsNotReportedCommitted()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(stageNewGeneration(journal, error), qPrintable(error));
    QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    const QString marker = QDir(journal->directoryPath()).filePath(
        QStringLiteral("COMMITTED"));
    QVERIFY(writeFile(marker, QByteArray("corrupt marker")));

    QVERIFY(!journal->hasCommitMarker());
}

void TestPlanReplacementJournal::journalDirectoryTamperingIsFailClosed_data()
{
    QTest::addColumn<int>("tamper");
    QTest::newRow("unknown-file") << int(DirectoryTamper::UnknownFile);
    QTest::newRow("nested-directory") << int(DirectoryTamper::NestedDirectory);
    QTest::newRow("symlink") << int(DirectoryTamper::Symlink);
    QTest::newRow("oversized-commit-marker")
        << int(DirectoryTamper::OversizedCommitMarker);
    QTest::newRow("oversized-lock-file")
        << int(DirectoryTamper::OversizedLockFile);
}

void TestPlanReplacementJournal::journalDirectoryTamperingIsFailClosed()
{
    QFETCH(int, tamper);
    const DirectoryTamper scenario = DirectoryTamper(tamper);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(stageNewGeneration(journal, error), qPrintable(error));
    const QString journalPath = journal->directoryPath();
    journal.reset();

    switch (scenario) {
    case DirectoryTamper::UnknownFile:
        QVERIFY(writeFile(
            QDir(journalPath).filePath(QStringLiteral("unknown")),
            QByteArray("unexpected")));
        break;
    case DirectoryTamper::NestedDirectory:
        QVERIFY(QDir().mkdir(
            QDir(journalPath).filePath(QStringLiteral("nested"))));
        break;
    case DirectoryTamper::Symlink:
#ifdef Q_OS_WIN
        QSKIP("Symbolic-link creation is not generally available on Windows");
#else
        QVERIFY(QFile::link(
            QDir(journalPath).filePath(QStringLiteral("manifest.json")),
            QDir(journalPath).filePath(QStringLiteral("unsafe-link"))));
        break;
#endif
    case DirectoryTamper::OversizedCommitMarker:
        QVERIFY(writeFile(
            QDir(journalPath).filePath(QStringLiteral("COMMITTED")),
            QByteArray(129, 'x')));
        break;
    case DirectoryTamper::OversizedLockFile:
        QVERIFY(writeFile(
            QDir(journalPath).filePath(QStringLiteral(".manifest.json.lock")),
            QByteArray(64 * 1024 + 1, 'x')));
        break;
    }

    QVERIFY(!PlanReplacement::Journal::reconcileAll(temporary.path(), error));
    QVERIFY(!error.isEmpty());
    const PlanPaths paths = planPaths(temporary.path());
    QCOMPARE(readFile(paths.oldOne), QByteArray("old generation one"));
    QCOMPARE(readFile(paths.oldTwo), QByteArray("old generation two"));
    QVERIFY(!QFileInfo::exists(paths.newOne));
    QVERIFY(!QFileInfo::exists(paths.newTwo));
}

void TestPlanReplacementJournal::transactionDataTamperingIsFailClosed_data()
{
    QTest::addColumn<int>("tamper");
    QTest::newRow("old-copy") << int(DataTamper::OldCopy);
    QTest::newRow("staged-copy") << int(DataTamper::StagedCopy);
    QTest::newRow("old-generation") << int(DataTamper::OldGeneration);
    QTest::newRow("committed-generation")
        << int(DataTamper::CommittedGeneration);
}

void TestPlanReplacementJournal::transactionDataTamperingIsFailClosed()
{
    QFETCH(int, tamper);
    const DataTamper scenario = DataTamper(tamper);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(stageNewGeneration(journal, error), qPrintable(error));
    const QString journalPath = journal->directoryPath();
    const PlanPaths paths = planPaths(temporary.path());

    if (scenario == DataTamper::CommittedGeneration) {
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
        journal.reset();
        QVERIFY(writeFile(paths.newOne, QByteArray("external committed edit")));
    } else {
        journal.reset();
        if (scenario == DataTamper::OldCopy) {
            const QFileInfoList copies = QDir(journalPath).entryInfoList(
                {QStringLiteral("old-*.copy")}, QDir::Files, QDir::Name);
            QVERIFY(!copies.isEmpty());
            QVERIFY(writeFile(
                copies.constFirst().absoluteFilePath(),
                QByteArray("tampered preserved data")));
        } else if (scenario == DataTamper::StagedCopy) {
            QVERIFY(writeFile(
                QDir(journalPath).filePath(QStringLiteral("new-0000.stage")),
                QByteArray("tampered staged data")));
        } else {
            QVERIFY(writeFile(paths.oldOne, QByteArray("external old edit")));
        }
    }

    QVERIFY(!PlanReplacement::Journal::reconcileAll(temporary.path(), error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!journalNamespaceIsEmpty(temporary.path()));
}

void TestPlanReplacementJournal::journalDirectoriesArePrivate()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(createOldGeneration(temporary.path()));
    QString error;
    const std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            replacementSpecification(temporary.path()), error);
    QVERIFY2(journal, qPrintable(error));
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions nonOwner =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther
        | QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QString transactions = QDir(temporary.path()).filePath(
        QStringLiteral(".gc-transactions"));
    const QString journalRoot = QDir(transactions).filePath(
        QStringLiteral("plan-replacement"));
    for (const QString &directory :
         {transactions, journalRoot, journal->directoryPath()}) {
        const QFileDevice::Permissions permissions =
            QFileInfo(directory).permissions();
        QCOMPARE(
            permissions & nonOwner,
            QFileDevice::Permissions());
        QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(permissions.testFlag(QFileDevice::ExeOwner));
    }
#endif
    QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
}

QTEST_GUILESS_MAIN(TestPlanReplacementJournal)

#include "testPlanReplacementJournal.moc"
