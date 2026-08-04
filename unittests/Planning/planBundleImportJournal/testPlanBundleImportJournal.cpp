#include <QtTest>

#include "PlanBundleImportJournal.h"
#include "PlanReplacementJournal.h"
#include "TrainDB.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

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
            && QDir().mkpath(workoutRoot)
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
            {{QStringLiteral("threshold.erg"),
              QByteArray("workout payload"), {}}},
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

PlanBundleImport::DatabaseCompletion removeDecision(
    TrainDB &database)
{
    return [&database](
        const TrainDB::PlanImportJournal &journal,
        QString &error) {
        if (!database.startLUW()) {
            error = QStringLiteral("cannot start completion");
            return false;
        }
        if (!database.removePlanImportJournal(
                journal.id, error)) {
            database.rollbackLUW();
            return false;
        }
        if (!database.endLUW()) {
            database.rollbackLUW();
            error = QStringLiteral("cannot commit completion");
            return false;
        }
        return true;
    };
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
};

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

    const PlanBundleImport::DatabaseCompletion fail = [](
        const TrainDB::PlanImportJournal &, QString &error) {
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

    const PlanBundleImport::DatabaseCompletion incomplete = [](
        const TrainDB::PlanImportJournal &, QString &) {
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

QTEST_GUILESS_MAIN(TestPlanBundleImportJournal)

#include "testPlanBundleImportJournal.moc"
