#include <QtTest>

#include "LinkedActivitySaveJournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool forceLegacyWindowsDelete = false;

#ifdef Q_OS_WIN
class WindowsTestHandle
{
public:
    explicit WindowsTestHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsTestHandle() { reset(); }

    WindowsTestHandle(const WindowsTestHandle &) = delete;
    WindowsTestHandle &operator=(const WindowsTestHandle &) = delete;

    bool isValid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const { return handle_; }

    void reset()
    {
        if (isValid()) ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

void writeFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    QVERIFY2(
        file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    QVERIFY(file.flush());
}

LinkedActivitySave::Specification specificationFor(const QString &root)
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

void writeSources(const QString &root)
{
    writeFixture(
        QDir(root).filePath(QStringLiteral("first-old.json")),
        QByteArray("first old generation"));
    writeFixture(
        QDir(root).filePath(QStringLiteral("second-old.json")),
        QByteArray("second old generation"));
}

bool stageEntries(
    const std::shared_ptr<LinkedActivitySave::Journal> &journal,
    QString &error)
{
    for (int index = 0; index < journal->entryCount(); ++index) {
        const QByteArray contents =
            QByteArray("staged generation ") + QByteArray::number(index);
        QFile file(journal->stagingPath(index));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(contents) != contents.size()
            || !file.flush()) {
            error = file.errorString();
            return false;
        }
        file.close();
        if (!journal->recordStaged(index, error)) return false;
    }
    return true;
}

} // namespace

void anchoredFilesystemTransitionReached(
    const char *, const QString &, const QString &)
{
}

bool anchoredFilesystemSyncFailureRequested(const QString &)
{
    return false;
}

bool anchoredFilesystemFileUnlinkFailureRequested(const QString &)
{
    return false;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return forceLegacyWindowsDelete;
}

class TestLinkedActivitySaveCleanup : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void releasesTransactionResources_data();
    void releasesTransactionResources();
#ifdef Q_OS_WIN
    void legacyWindowsJournalFileRemovalRetries_data();
    void legacyWindowsJournalFileRemovalRetries();
    void legacyWindowsJournalDirectoryRemovalRetries_data();
    void legacyWindowsJournalDirectoryRemovalRetries();
#endif
};

void TestLinkedActivitySaveCleanup::cleanup()
{
    forceLegacyWindowsDelete = false;
}

void TestLinkedActivitySaveCleanup::releasesTransactionResources_data()
{
    QTest::addColumn<bool>("committed");

    QTest::newRow("rollback") << false;
    QTest::newRow("commit") << true;
}

void TestLinkedActivitySaveCleanup::releasesTransactionResources()
{
    QFETCH(bool, committed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString athleteRoot = dir.filePath(QStringLiteral("athlete"));
    const QString movedRoot = dir.filePath(QStringLiteral("moved-athlete"));
    QVERIFY(QDir().mkpath(athleteRoot));
    writeSources(athleteRoot);

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            specificationFor(athleteRoot), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();

    if (committed) {
        QVERIFY2(stageEntries(journal, error), qPrintable(error));
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
        QVERIFY(journal->hasCommitMarker());
        QVERIFY2(journal->cleanupAfterCommit(error), qPrintable(error));
    } else {
        QVERIFY2(journal->cleanupAfterRollback(error), qPrintable(error));
    }
    QVERIFY(!journal->hasCommitMarker());

    QVERIFY(QDir().mkpath(journalPath));
    writeFixture(
        QDir(journalPath).filePath(QStringLiteral("COMMITTED")),
        QByteArray("replacement marker\n"));
    QVERIFY2(
        !journal->hasCommitMarker(),
        "A completed journal accepted a replacement commit marker");

    QVERIFY2(
        QDir().rename(athleteRoot, movedRoot),
        "A completed journal retained an athlete-directory resource");
    QVERIFY(QFileInfo::exists(movedRoot));
}

#ifdef Q_OS_WIN
void TestLinkedActivitySaveCleanup::
legacyWindowsJournalFileRemovalRetries_data()
{
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("externalObserver");

    QTest::newRow("rollback-internal") << false << false;
    QTest::newRow("commit-internal") << true << false;
    QTest::newRow("rollback-external") << false << true;
    QTest::newRow("commit-external") << true << true;
}

void TestLinkedActivitySaveCleanup::
legacyWindowsJournalFileRemovalRetries()
{
    QFETCH(bool, committed);
    QFETCH(bool, externalObserver);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeSources(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            specificationFor(dir.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    QVERIFY2(stageEntries(journal, error), qPrintable(error));
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
    if (externalObserver) QVERIFY(observer.isValid());

    forceLegacyWindowsDelete = true;
    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    forceLegacyWindowsDelete = false;

    if (!externalObserver) {
        QVERIFY2(firstCleanup, qPrintable(error));
        QVERIFY(!QFileInfo::exists(journalPath));
        QVERIFY(!journal->hasCommitMarker());
        return;
    }

    QVERIFY2(!firstCleanup, "A pending legacy deletion was accepted");
    QVERIFY2(!error.isEmpty(), "Pending deletion must report an error");
    FILE_STANDARD_INFO pendingInfo {};
    QVERIFY(::GetFileInformationByHandleEx(
        observer.get(), FileStandardInfo,
        &pendingInfo, sizeof(pendingInfo)));
    QVERIFY(pendingInfo.DeletePending);
    WindowsTestHandle blockedReopen(::CreateFileW(
        reinterpret_cast<LPCWSTR>(manifestPath.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    const DWORD reopenError = ::GetLastError();
    QVERIFY(!blockedReopen.isValid());
    QCOMPARE(reopenError, DWORD(ERROR_ACCESS_DENIED));
    QVERIFY(QFileInfo::exists(journalPath));

    error.clear();
    QVERIFY2(
        !(committed
              ? journal->cleanupAfterCommit(error)
              : journal->cleanupAfterRollback(error)),
        "A still-pending legacy deletion was accepted on retry");
    QVERIFY2(!error.isEmpty(), "Pending retry must report an error");
    pendingInfo = {};
    QVERIFY(::GetFileInformationByHandleEx(
        observer.get(), FileStandardInfo,
        &pendingInfo, sizeof(pendingInfo)));
    QVERIFY(pendingInfo.DeletePending);
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

void TestLinkedActivitySaveCleanup::
legacyWindowsJournalDirectoryRemovalRetries_data()
{
    QTest::addColumn<bool>("committed");

    QTest::newRow("rollback") << false;
    QTest::newRow("commit") << true;
}

void TestLinkedActivitySaveCleanup::
legacyWindowsJournalDirectoryRemovalRetries()
{
    QFETCH(bool, committed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeSources(dir.path());

    QString error;
    const std::shared_ptr<LinkedActivitySave::Journal> journal =
        LinkedActivitySave::Journal::prepare(
            specificationFor(dir.path()), error);
    QVERIFY2(journal, qPrintable(error));
    const QString journalPath = journal->directoryPath();
    QVERIFY2(stageEntries(journal, error), qPrintable(error));
    if (committed) {
        QVERIFY2(journal->publishAndCommit(error), qPrintable(error));
    }

    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(journalPath.utf16()),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());

    forceLegacyWindowsDelete = true;
    error.clear();
    const bool firstCleanup = committed
        ? journal->cleanupAfterCommit(error)
        : journal->cleanupAfterRollback(error);
    forceLegacyWindowsDelete = false;

    QVERIFY2(!firstCleanup, "A pending legacy directory deletion was accepted");
    QVERIFY2(!error.isEmpty(), "Pending deletion must report an error");
    FILE_STANDARD_INFO pendingInfo {};
    QVERIFY(::GetFileInformationByHandleEx(
        observer.get(), FileStandardInfo,
        &pendingInfo, sizeof(pendingInfo)));
    QVERIFY(pendingInfo.DeletePending);
    WindowsTestHandle blockedReopen(::CreateFileW(
        reinterpret_cast<LPCWSTR>(journalPath.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    const DWORD reopenError = ::GetLastError();
    QVERIFY(!blockedReopen.isValid());
    QCOMPARE(reopenError, DWORD(ERROR_ACCESS_DENIED));
    QVERIFY2(
        !QDir().mkdir(journalPath),
        "The observed journal name was not left delete-pending");

    error.clear();
    QVERIFY2(
        !(committed
              ? journal->cleanupAfterCommit(error)
              : journal->cleanupAfterRollback(error)),
        "A still-pending legacy directory deletion was accepted on retry");
    QVERIFY2(!error.isEmpty(), "Pending retry must report an error");
    pendingInfo = {};
    QVERIFY(::GetFileInformationByHandleEx(
        observer.get(), FileStandardInfo,
        &pendingInfo, sizeof(pendingInfo)));
    QVERIFY(pendingInfo.DeletePending);

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

QTEST_APPLESS_MAIN(TestLinkedActivitySaveCleanup)

#include "testLinkedActivitySaveCleanup.moc"
