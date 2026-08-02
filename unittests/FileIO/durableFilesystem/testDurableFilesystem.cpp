#include <QtTest>

#include "AtomicFileWriter.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

const char ChildOperation[] = "GC_DURABLE_FILESYSTEM_CHILD_OPERATION";
const char ChildPath[] = "GC_DURABLE_FILESYSTEM_CHILD_PATH";
const char CrashPhase[] = "GC_DURABLE_FILESYSTEM_CRASH_PHASE";

void writeFixture(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("durability fixture"), qint64(18));
    QVERIFY(file.flush());
}

int runChild(
    const QByteArray &operation,
    const QString &path,
    const QByteArray &crashPhase = {})
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(ChildOperation),
                       QString::fromLatin1(operation));
    environment.insert(QString::fromLatin1(ChildPath), path);
    if (!crashPhase.isEmpty()) {
        environment.insert(QString::fromLatin1(CrashPhase),
                           QString::fromLatin1(crashPhase));
    }
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationFilePath(), {});
    if (!process.waitForStarted() || !process.waitForFinished(30000)) {
        return -1;
    }
    return process.exitCode();
}

} // namespace

void durableFilesystemTransitionReached(const char *transition)
{
    if (qgetenv(CrashPhase) == QByteArray(transition))
        std::_Exit(86);
}

class TestDurableFilesystem : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void mutationSurvivesImmediateProcessExit_data();
    void mutationSurvivesImmediateProcessExit();
    void mutationCrashBoundaryIsRecoverable_data();
    void mutationCrashBoundaryIsRecoverable();
    void reportedBarrierFailureLeavesRecoverableState_data();
    void reportedBarrierFailureLeavesRecoverableState();
    void directoryCreationRejectsCollision();
};

void TestDurableFilesystem::initTestCase()
{
    const QByteArray operation = qgetenv(ChildOperation);
    if (operation.isEmpty()) return;

    const QString path = QString::fromUtf8(qgetenv(ChildPath));
    QString error;
    bool success = false;
    if (operation == QByteArrayLiteral("create-directory")) {
        success = createDirectoryDurably(path, error);
    } else if (operation == QByteArrayLiteral("remove-file")) {
        success = removeFileDurably(path, error);
    } else if (operation == QByteArrayLiteral("remove-directory")) {
        success = removeDirectoryDurably(path, error);
    }
    std::_Exit(success ? 86 : 87);
}

void TestDurableFilesystem::mutationSurvivesImmediateProcessExit_data()
{
    QTest::addColumn<QByteArray>("operation");
    QTest::newRow("create-directory") << QByteArray("create-directory");
    QTest::newRow("remove-file") << QByteArray("remove-file");
    QTest::newRow("remove-directory") << QByteArray("remove-directory");
}

void TestDurableFilesystem::mutationSurvivesImmediateProcessExit()
{
    QFETCH(QByteArray, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("entry"));
    if (operation == QByteArrayLiteral("remove-file")) {
        writeFixture(path);
    } else if (operation == QByteArrayLiteral("remove-directory")) {
        QVERIFY(QDir().mkdir(path));
    }

    QCOMPARE(runChild(operation, path), 86);
    QCOMPARE(QFileInfo::exists(path),
             operation == QByteArrayLiteral("create-directory"));
    if (operation == QByteArrayLiteral("create-directory")) {
        QVERIFY(QFileInfo(path).isDir());
    }
}

void TestDurableFilesystem::reportedBarrierFailureLeavesRecoverableState_data()
{
    mutationSurvivesImmediateProcessExit_data();
}

void TestDurableFilesystem::mutationCrashBoundaryIsRecoverable_data()
{
    QTest::addColumn<QByteArray>("operation");
    QTest::addColumn<QByteArray>("phase");
    QTest::addColumn<bool>("entryExists");

#ifdef Q_OS_WIN
    QTest::newRow("directory-staging-created")
        << QByteArray("create-directory")
        << QByteArray("directory-staging-created") << false;
#endif
    QTest::newRow("directory-published")
        << QByteArray("create-directory")
        << QByteArray("directory-published") << true;
    QTest::newRow("directory-parent-synchronized")
        << QByteArray("create-directory")
        << QByteArray("parent-synchronized") << true;
    QTest::newRow("file-removal-requested")
        << QByteArray("remove-file")
        << QByteArray("file-removal-requested") << false;
    QTest::newRow("file-removed")
        << QByteArray("remove-file")
        << QByteArray("file-removed") << false;
    QTest::newRow("file-parent-synchronized")
        << QByteArray("remove-file")
        << QByteArray("parent-synchronized") << false;
    QTest::newRow("directory-removal-requested")
        << QByteArray("remove-directory")
        << QByteArray("directory-removal-requested") << false;
    QTest::newRow("directory-removed")
        << QByteArray("remove-directory")
        << QByteArray("directory-removed") << false;
    QTest::newRow("removed-directory-parent-synchronized")
        << QByteArray("remove-directory")
        << QByteArray("parent-synchronized") << false;
}

void TestDurableFilesystem::mutationCrashBoundaryIsRecoverable()
{
    QFETCH(QByteArray, operation);
    QFETCH(QByteArray, phase);
    QFETCH(bool, entryExists);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("entry"));
    if (operation == QByteArrayLiteral("remove-file")) {
        writeFixture(path);
    } else if (operation == QByteArrayLiteral("remove-directory")) {
        QVERIFY(QDir().mkdir(path));
    }

    QCOMPARE(runChild(operation, path, phase), 86);
    QCOMPARE(QFileInfo::exists(path), entryExists);
#ifdef Q_OS_WIN
    if (phase == QByteArrayLiteral("directory-staging-created")) {
        const QFileInfoList staging = QDir(root.path()).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(staging.size(), 1);
        QVERIFY(!QUuid(staging.constFirst().fileName()).isNull());
    }
#endif
}

void TestDurableFilesystem::reportedBarrierFailureLeavesRecoverableState()
{
    QFETCH(QByteArray, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("entry"));
    if (operation == QByteArrayLiteral("remove-file")) {
        writeFixture(path);
    } else if (operation == QByteArrayLiteral("remove-directory")) {
        QVERIFY(QDir().mkdir(path));
    }

    const AtomicDirectorySyncFunction rejectBarrier =
        [](const QString &, QString &error) {
            error = QStringLiteral("injected durability barrier failure");
            return false;
        };
    QString error;
    bool success = false;
    if (operation == QByteArrayLiteral("create-directory")) {
        success = createDirectoryDurably(path, error, rejectBarrier);
    } else if (operation == QByteArrayLiteral("remove-file")) {
        success = removeFileDurably(path, error, rejectBarrier);
    } else {
        success = removeDirectoryDurably(path, error, rejectBarrier);
    }

    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("injected")));
    QCOMPARE(QFileInfo::exists(path),
             operation == QByteArrayLiteral("create-directory"));
}

void TestDurableFilesystem::directoryCreationRejectsCollision()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("entry"));
    QVERIFY(QDir().mkdir(path));

    QString error;
    QVERIFY(!createDirectoryDurably(path, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo(path).isDir());
    QCOMPARE(QDir(root.path()).entryList(
                 QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList({QStringLiteral("entry")}));
}

QTEST_GUILESS_MAIN(TestDurableFilesystem)

#include "testDurableFilesystem.moc"
