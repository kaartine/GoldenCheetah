#include <QtTest>

#include "AnchoredFileSystem.h"
#include "AtomicFileWriter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <functional>
#include <memory>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace AnchoredFileSystem;

namespace {

using FilesystemAction = std::function<void(
    const char *, const QString &, const QString &)>;

FilesystemAction filesystemAction;
bool failDirectorySync = false;
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

    ~WindowsTestHandle()
    {
        if (isValid()) ::CloseHandle(handle_);
    }

    bool isValid() const
    {
        return handle_ != nullptr
            && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

} // namespace

void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary)
{
    if (filesystemAction) filesystemAction(
        transition, primary, secondary);
}

bool anchoredFilesystemSyncFailureRequested(const QString &)
{
    return failDirectorySync;
}

bool anchoredFilesystemUseLegacyWindowsDelete()
{
    return forceLegacyWindowsDelete;
}

namespace {

void writeFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    QVERIFY(file.flush());
}

QByteArray readFixture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool createHardLink(const QString &source, const QString &target)
{
#ifdef Q_OS_UNIX
    return ::link(QFile::encodeName(source).constData(),
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

bool createSymbolicLink(const QString &source, const QString &target)
{
#ifdef Q_OS_UNIX
    return ::symlink(QFile::encodeName(source).constData(),
                     QFile::encodeName(target).constData()) == 0;
#elif defined(Q_OS_WIN)
    return ::CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(target.utf16()),
        reinterpret_cast<LPCWSTR>(source.utf16()),
        0);
#else
    Q_UNUSED(source)
    Q_UNUSED(target)
    return false;
#endif
}

DirectoryAnchor openDirectory(const QString &path)
{
    DirectoryAnchor directory;
    QString error;
    if (!DirectoryAnchor::open(path, directory, error)) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return directory;
}

EntryRef entry(const DirectoryAnchor &directory, const QString &name)
{
    QString error;
    EntryRef result = directory.entry(name, error);
    if (!result.isValid()) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return result;
}

PinnedFile pin(const EntryRef &reference)
{
    PinnedFile file;
    QString error;
    if (!pinRegularFile(reference, file, error)) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return {};
    }
    return file;
}

void verifyApplied(const MutationResult &result)
{
    QVERIFY2(result.applied(), qPrintable(result.error));
    QVERIFY(result.effect == MutationEffect::AppliedDurable
            || result.effect == MutationEffect::AppliedNotDurable);
}

void verifyPinnedAt(
    PinnedFile &file,
    const EntryRef &entry,
    const NativeIdentity &expected)
{
    bool matches = false;
    QString error;
    QVERIFY(entryMatches(entry, file, matches, error));
    QVERIFY2(matches, qPrintable(error));
    QCOMPARE(file.identity(), expected);
    file = {};
    QCOMPARE(pin(entry).identity(), expected);
}

} // namespace

class TestAnchoredFilesystem : public QObject
{
    Q_OBJECT

private slots:
    void rejectsUnsafeComponents_data();
    void rejectsUnsafeComponents();
    void rejectsUnsafeFileTypes();
    void pinsIdentityAndContentThroughOneHandle();
    void permitsConcurrentPinsOfOneIdentity();
    void permitsOrdinaryQtReadsWhilePinned();
    void permitsOrdinaryQtReadsOfPinnedCopy();
#ifdef Q_OS_WIN
    void newAtomicWriterHandsOffWindowsStagingPin();
    void outputFilesDenyConcurrentWindowsWrites_data();
    void outputFilesDenyConcurrentWindowsWrites();
    void outputFilesCanBeRepinned_data();
    void outputFilesCanBeRepinned();
    void outputPinFinalizationFailureRetainsFile_data();
    void outputPinFinalizationFailureRetainsFile();
    void outputDigestMismatchRetainsFile_data();
    void outputDigestMismatchRetainsFile();
#endif
    void permitsAtomicSiblingReplacementWhileDirectoryAnchored();
    void permitsAtomicReplacementWhilePinned();
    void readsPinnedContentsAfterPathReplacement();
    void directoryAnchorSurvivesPathReplacement();
    void directoryAnchorDetectsPathReplacement();
    void childAnchorsRemainInOneRootGeneration();
    void optionalChildOpenDistinguishesMissingAndUnsafeEntries();
    void inspectsEntriesThroughPinnedDirectory();
    void copiesPinnedContentsThroughAnchoredParents();
    void copyDoesNotReplaceDestination();
    void copyReportsNonDurableCleanup();
    void moveDoesNotRestoreUnverifiedDestination();
    void moveRejectsNewHardLink();
    void moveUsesPinnedParentAfterPathReplacement();
    void moveRejectsFinalEntryReplacement();
    void moveDoesNotReplaceDestination();
    void moveDoesNotReplaceDestinationAcrossDirectories();
    void removeRejectsPinnedParentPathReplacement();
    void removeRejectsFinalEntryReplacement();
    void removesAnchoredEmptyDirectory();
    void emptyDirectoryRemovalRejectsReplacedParent();
    void emptyDirectoryRemovalRetainsPreQuarantineReplacement();
    void emptyDirectoryRemovalRetainsFinalNameReplacement();
    void emptyDirectoryRemovalRetainsNonEmptyQuarantine();
    void emptyDirectoryRemovalRejectsRepopulatedOriginalName();
    void emptyDirectoryRemovalReportsParentSyncFailure();
    void emptyDirectoryRemovalReportsPartialSyncFailure();
    void emptyDirectoryRemovalCanRetryAfterNonEmptyFailure();
    void emptyDirectoryRemovalRetriesAfterDeleteSharingConflict();
    void emptyDirectoryRemovalRejectsWindowsRepopulation();
    void emptyDirectoryRemovalRejectsWindowsAliases();
    void emptyDirectoryRemovalUnlinksWindowsNameWithSharedObserver();
    void emptyDirectoryRemovalLegacyWindowsDeleteReportsPendingName();
    void removeRetainsReplacementAtQuarantine();
    void removePartialMoveReportsGeneratedQuarantine();
    void removeDetectsReplacementAfterFinalQuarantineCheck();
    void removeUnlinksWindowsNameWithSharedObserver();
    void removeLegacyWindowsDeleteReportsPendingName();
    void syncsPinnedDirectory();
};

void TestAnchoredFilesystem::rejectsUnsafeComponents_data()
{
    QTest::addColumn<QString>("component");
    QTest::newRow("empty") << QString();
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("dot-dot") << QStringLiteral("..");
    QTest::newRow("slash") << QStringLiteral("one/two");
    QTest::newRow("backslash") << QStringLiteral("one\\two");
    QTest::newRow("absolute") << QDir::rootPath();
}

void TestAnchoredFilesystem::rejectsUnsafeComponents()
{
    QFETCH(QString, component);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());

    QString error;
    const EntryRef reference = directory.entry(component, error);
    QVERIFY(!reference.isValid());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::rejectsUnsafeFileTypes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());

    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("directory")));
    QString error;
    PinnedFile pinned;
    QVERIFY(!pinRegularFile(
        entry(directory, QStringLiteral("directory")), pinned, error));
    QVERIFY(!error.isEmpty());

    const QString ordinary = root.filePath(QStringLiteral("ordinary"));
    const QString hardLink = root.filePath(QStringLiteral("hard-link"));
    writeFixture(ordinary, QByteArray("fixture"));
    if (createHardLink(ordinary, hardLink)) {
        error.clear();
        QVERIFY(!pinRegularFile(
            entry(directory, QStringLiteral("ordinary")), pinned, error));
        QVERIFY(!error.isEmpty());
    }

    const QString symbolic = root.filePath(QStringLiteral("symbolic"));
    if (createSymbolicLink(ordinary, symbolic)) {
        error.clear();
        QVERIFY(!pinRegularFile(
            entry(directory, QStringLiteral("symbolic")), pinned, error));
        QVERIFY(!error.isEmpty());
    }
}

void TestAnchoredFilesystem::pinsIdentityAndContentThroughOneHandle()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray contents("same-content replacement");
    writeFixture(source.displayPath(), contents);

    const PinnedFile original = pin(source);
    QVERIFY(original.identity().isValid());
    QCOMPARE(original.identity().linkCount(), quint64(1));
    QCOMPARE(original.size(), qint64(contents.size()));
    QCOMPARE(original.sha256(), QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256));

    const bool replaced = QFile::rename(
        source.displayPath(), root.filePath(QStringLiteral("retained")));
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        bool matches = false;
        QString error;
        QVERIFY(entryMatches(source, original, matches, error));
        QVERIFY2(matches, qPrintable(error));
        return;
    }
    writeFixture(source.displayPath(), contents);
    const PinnedFile substitute = pin(source);
    QVERIFY(original.identity() != substitute.identity());

    bool matches = true;
    QString error;
    QVERIFY(entryMatches(source, original, matches, error));
    QVERIFY(!matches);
    QVERIFY(error.isEmpty());
}

void TestAnchoredFilesystem::permitsConcurrentPinsOfOneIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    writeFixture(source.displayPath(), QByteArray("fixture"));

    const PinnedFile first = pin(source);
    PinnedFile second;
    QString error;
    QVERIFY2(
        pinRegularFile(source, second, error),
        qPrintable(error));
    QCOMPARE(second.identity(), first.identity());
}

void TestAnchoredFilesystem::permitsOrdinaryQtReadsWhilePinned()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray contents("fixture");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinned = pin(source);
    QFile ordinaryReader(source.displayPath());
    QVERIFY2(
        ordinaryReader.open(QIODevice::ReadOnly),
        qPrintable(ordinaryReader.errorString()));
    QCOMPARE(ordinaryReader.readAll(), contents);
}

void TestAnchoredFilesystem::permitsOrdinaryQtReadsOfPinnedCopy()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("fixture");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile pinnedCopy;
    QString error;
    QVERIFY2(
        copyToNewFile(
            pinnedSource, destination, pinnedCopy, error),
        qPrintable(error));

    QFile ordinaryReader(destination.displayPath());
    QVERIFY2(
        ordinaryReader.open(QIODevice::ReadOnly),
        qPrintable(ordinaryReader.errorString()));
    QCOMPARE(ordinaryReader.readAll(), contents);
}

#ifdef Q_OS_WIN
void TestAnchoredFilesystem::newAtomicWriterHandsOffWindowsStagingPin()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("complete activity");

    NewAtomicFileWriter writer(target);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(contents), qint64(contents.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));
    QCOMPARE(readFixture(target), contents);
}

void TestAnchoredFilesystem::
outputFilesDenyConcurrentWindowsWrites_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputFilesDenyConcurrentWindowsWrites()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    QVERIFY2(created, qPrintable(error));

    WindowsTestHandle concurrentWriter(::CreateFileW(
        reinterpret_cast<LPCWSTR>(destination.displayPath().utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    const DWORD nativeError = ::GetLastError();
    QVERIFY2(
        !concurrentWriter.isValid(),
        "A live anchored output pin allowed a concurrent Windows writer");
    QCOMPARE(nativeError, DWORD(ERROR_SHARING_VIOLATION));
}

void TestAnchoredFilesystem::outputFilesCanBeRepinned_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputFilesCanBeRepinned()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    QVERIFY2(created, qPrintable(error));

    PinnedFile repinned;
    QVERIFY2(
        pinRegularFile(destination, repinned, error),
        qPrintable(error));
    QCOMPARE(repinned.identity(), output.identity());
    QCOMPARE(repinned.size(), output.size());
    QCOMPARE(repinned.sha256(), output.sha256());
}

void TestAnchoredFilesystem::
outputPinFinalizationFailureRetainsFile_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputPinFinalizationFailureRetainsFile()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    bool hookReached = false;
    std::unique_ptr<WindowsTestHandle> concurrentWriter;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(transition, "output-pin-writer-released") != 0
            || primary != destination.displayPath()) {
            return;
        }
        hookReached = true;
        concurrentWriter = std::make_unique<WindowsTestHandle>(
            ::CreateFileW(
                reinterpret_cast<LPCWSTR>(primary.utf16()),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
    };

    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(concurrentWriter && concurrentWriter->isValid());
    QVERIFY2(!created, "A sharing-blocked final output pin was accepted");
    QVERIFY2(!error.isEmpty(), "Rejected output must report an error");
    concurrentWriter.reset();
    QVERIFY2(
        QFileInfo::exists(destination.displayPath()),
        "Failed pin finalization deleted a concurrently writable output");
    QCOMPARE(readFixture(destination.displayPath()), contents);
}

void TestAnchoredFilesystem::outputDigestMismatchRetainsFile_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("copy-new") << QStringLiteral("copy");
    QTest::newRow("write-new") << QStringLiteral("write");
}

void TestAnchoredFilesystem::outputDigestMismatchRetainsFile()
{
    QFETCH(QString, operation);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef destination = entry(
        directory, QStringLiteral("destination"));
    const QByteArray contents("identity-bound contents");
    const QByteArray replacement(contents.size(), 'x');
    writeFixture(source.displayPath(), contents);

    const PinnedFile pinnedSource = pin(source);
    bool hookReached = false;
    bool writerOpened = false;
    bool replacementWritten = false;
    filesystemAction = [&](const char *transition,
                           const QString &primary,
                           const QString &) {
        if (qstrcmp(transition, "output-pin-writer-released") != 0
            || primary != destination.displayPath()) {
            return;
        }
        hookReached = true;
        WindowsTestHandle writer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(primary.utf16()),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        writerOpened = writer.isValid();
        if (!writerOpened) return;
        LARGE_INTEGER start {};
        DWORD written = 0;
        replacementWritten = ::SetFilePointerEx(
                                 writer.get(), start,
                                 nullptr, FILE_BEGIN)
            && ::WriteFile(
                writer.get(), replacement.constData(),
                DWORD(replacement.size()), &written, nullptr)
            && written == DWORD(replacement.size())
            && ::FlushFileBuffers(writer.get());
    };

    PinnedFile output;
    QString error;
    const bool created = operation == QStringLiteral("copy")
        ? copyToNewFile(pinnedSource, destination, output, error)
        : writeNewFile(contents, destination, output, error);
    filesystemAction = {};

    QVERIFY(hookReached);
    QVERIFY(writerOpened);
    QVERIFY(replacementWritten);
    QVERIFY2(!created, "A digest-mismatched final output pin was accepted");
    QVERIFY2(!error.isEmpty(), "Rejected output must report an error");
    QVERIFY2(
        QFileInfo::exists(destination.displayPath()),
        "Digest failure deleted a concurrently modified output");
    QCOMPARE(readFixture(destination.displayPath()), replacement);
}
#endif

void TestAnchoredFilesystem::
permitsAtomicSiblingReplacementWhileDirectoryAnchored()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath = root.filePath(
        QStringLiteral(".gc-transactions/linked-removal/journal"));
    const QString plannedPath = root.filePath(
        QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(journalPath));
    QVERIFY(QDir().mkpath(plannedPath));
    const DirectoryAnchor journal = openDirectory(journalPath);
    const QString targetPath = QDir(plannedPath).filePath(
        QStringLiteral("activity.json"));
    writeFixture(targetPath, QByteArray("original"));

    const QByteArray replacement("replacement");
    ReplaceAtomicFileWriter writer(targetPath);
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(replacement), qint64(replacement.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));

    QCOMPARE(readFixture(targetPath), replacement);
    QString error;
    QVERIFY2(journal.pathMatches(error), qPrintable(error));
}

void TestAnchoredFilesystem::permitsAtomicReplacementWhilePinned()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef target = entry(
        directory, QStringLiteral("activity.json"));
    const QByteArray original("original");
    const QByteArray replacement("replacement");
    writeFixture(target.displayPath(), original);
    const PinnedFile pinned = pin(target);

    ReplaceAtomicFileWriter writer(target.displayPath());
    QVERIFY2(writer.open(), qPrintable(writer.errorString()));
    QCOMPARE(writer.write(replacement), qint64(replacement.size()));
    QVERIFY2(writer.flush(), qPrintable(writer.errorString()));
    QVERIFY2(writer.commit(), qPrintable(writer.errorString()));

    QCOMPARE(readFixture(target.displayPath()), replacement);
    QVERIFY(pinned.isValid());
    QCOMPARE(pinned.size(), qint64(original.size()));
}

void TestAnchoredFilesystem::readsPinnedContentsAfterPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray originalContents("original pinned contents");
    const QByteArray substituteContents("substitute contents");
    writeFixture(source.displayPath(), originalContents);
    bool replaced = false;
    {
        const PinnedFile original = pin(source);

        replaced = QFile::rename(source.displayPath(), retained);
#ifndef Q_OS_WIN
        QVERIFY(replaced);
#endif
        if (replaced) writeFixture(source.displayPath(), substituteContents);

        QByteArray contents;
        QString error;
        QVERIFY(readAll(original, 1024, contents, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents, originalContents);
        if (replaced) {
            QCOMPARE(readFixture(source.displayPath()), substituteContents);
        }
    }
    if (replaced) QCOMPARE(readFixture(retained), originalContents);
}

void TestAnchoredFilesystem::directoryAnchorSurvivesPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));

    const DirectoryAnchor original = openDirectory(live);
    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        QCOMPARE(openDirectory(live).identity(), original.identity());
        return;
    }
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor substitute = openDirectory(live);

    QVERIFY(original.identity().isValid());
    QVERIFY(original.identity() != substitute.identity());
    QString error;
    QVERIFY(original.sync(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TestAnchoredFilesystem::directoryAnchorDetectsPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor original = openDirectory(live);

    QString error;
    QVERIFY(original.pathMatches(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        QVERIFY(original.pathMatches(error));
        return;
    }
    QVERIFY(QDir().mkdir(live));
    QVERIFY(!original.pathMatches(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TestAnchoredFilesystem::childAnchorsRemainInOneRootGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString live = temporary.filePath(QStringLiteral("live"));
    const QString retained = temporary.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    QVERIFY(QDir(live).mkdir(QStringLiteral("activities")));
    QVERIFY(QDir(live).mkdir(QStringLiteral("backup")));
    const DirectoryAnchor root = openDirectory(live);

    DirectoryAnchor activities;
    QString error;
    QVERIFY(root.openChild(
        QStringLiteral("activities"), activities, error));

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (replaced) {
        QVERIFY(QDir().mkdir(live));
        QVERIFY(QDir(live).mkdir(QStringLiteral("backup")));
    }

    DirectoryAnchor backup;
    QVERIFY(root.openChild(QStringLiteral("backup"), backup, error));
    const QString expectedPath = replaced
        ? QDir(retained).filePath(QStringLiteral("backup"))
        : QDir(live).filePath(QStringLiteral("backup"));
    QCOMPARE(backup.identity(), openDirectory(expectedPath).identity());
    QVERIFY(activities.identity().isValid());
    if (replaced) {
        QVERIFY(backup.identity()
                != openDirectory(
                    QDir(live).filePath(QStringLiteral("backup")))
                       .identity());
    }
}

void TestAnchoredFilesystem::
optionalChildOpenDistinguishesMissingAndUnsafeEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const DirectoryAnchor root = openDirectory(temporary.path());

    DirectoryAnchor child;
    bool exists = true;
    QString error;
    QVERIFY(root.openChildIfExists(
        QStringLiteral("missing"), child, exists, error));
    QVERIFY(!exists);
    QVERIFY(!child.isValid());
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY(QDir(temporary.path()).mkdir(QStringLiteral("present")));
    QVERIFY(root.openChildIfExists(
        QStringLiteral("present"), child, exists, error));
    QVERIFY(exists);
    QVERIFY(child.isValid());

    writeFixture(
        temporary.filePath(QStringLiteral("regular-file")),
        QByteArray("not a directory"));
    QVERIFY(!root.openChildIfExists(
        QStringLiteral("regular-file"), child, exists, error));
    QVERIFY(!exists);
    QVERIFY(!child.isValid());
    QVERIFY(!error.isEmpty());
}

void TestAnchoredFilesystem::inspectsEntriesThroughPinnedDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString live = root.filePath(QStringLiteral("live"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    QVERIFY(QDir().mkdir(live));
    const DirectoryAnchor directory = openDirectory(live);
    const EntryRef present = entry(directory, QStringLiteral("present"));
    const EntryRef missing = entry(directory, QStringLiteral("missing"));
    writeFixture(present.displayPath(), QByteArray("present"));

    bool exists = false;
    QString error;
    QVERIFY(entryExists(present, exists, error));
    QVERIFY(exists);
    QVERIFY(entryExists(missing, exists, error));
    QVERIFY(!exists);

    const bool replaced = QDir().rename(live, retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (replaced) {
        QVERIFY(QDir().mkdir(live));
        QVERIFY(!QFileInfo::exists(present.displayPath()));
        QVERIFY(entryExists(present, exists, error));
        QVERIFY(exists);
    }
}

void TestAnchoredFilesystem::copiesPinnedContentsThroughAnchoredParents()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    const QString retainedSource =
        root.filePath(QStringLiteral("retained-source"));
    const QString retainedTarget =
        root.filePath(QStringLiteral("retained-target"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("source"));
    const EntryRef target = entry(targetDirectory, QStringLiteral("target"));
    const QByteArray originalContents("anchored copy contents");
    const QByteArray substituteContents("substitute source contents");
    writeFixture(source.displayPath(), originalContents);
    PinnedFile pinned = pin(source);

    const bool sourceReplaced = QDir().rename(sourcePath, retainedSource);
    const bool targetReplaced = QDir().rename(targetPath, retainedTarget);
#ifndef Q_OS_WIN
    QVERIFY(sourceReplaced);
    QVERIFY(targetReplaced);
#endif
    if (sourceReplaced) {
        QVERIFY(QDir().mkdir(sourcePath));
        writeFixture(source.displayPath(), substituteContents);
    }
    if (targetReplaced) QVERIFY(QDir().mkdir(targetPath));

    PinnedFile copied;
    QString error;
    QVERIFY(copyToNewFile(pinned, target, copied, error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(copied.size(), qint64(originalContents.size()));
    QCOMPARE(copied.sha256(), QCryptographicHash::hash(
        originalContents, QCryptographicHash::Sha256));
    QByteArray copiedContents;
    QVERIFY(readAll(copied, 1024, copiedContents, error));
    QCOMPARE(copiedContents, originalContents);
    copied = {};
    pinned = {};

    const QString actualTarget = targetReplaced
        ? QDir(retainedTarget).filePath(QStringLiteral("target"))
        : target.displayPath();
    QCOMPARE(readFixture(actualTarget), originalContents);
    if (sourceReplaced) {
        QCOMPARE(readFixture(source.displayPath()), substituteContents);
        QCOMPARE(readFixture(
            QDir(retainedSource).filePath(QStringLiteral("source"))),
            originalContents);
    }
    if (targetReplaced) {
        QVERIFY(!QFileInfo::exists(target.displayPath()));
    }
}

void TestAnchoredFilesystem::copyDoesNotReplaceDestination()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QByteArray sourceContents("source");
    const QByteArray targetContents("target");
    writeFixture(source.displayPath(), sourceContents);
    writeFixture(target.displayPath(), targetContents);
    PinnedFile pinned = pin(source);

    PinnedFile copied;
    QString error;
    QVERIFY(!copyToNewFile(pinned, target, copied, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!copied.isValid());
    pinned = {};
    QCOMPARE(readFixture(source.displayPath()), sourceContents);
    QCOMPARE(readFixture(target.displayPath()), targetContents);
}

void TestAnchoredFilesystem::copyReportsNonDurableCleanup()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory fsync cleanup reporting is Unix-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    const PinnedFile pinned = pin(source);

    failDirectorySync = true;
    PinnedFile copied;
    QString error;
    const bool succeeded = copyToNewFile(
        pinned, target, copied, error);
    failDirectorySync = false;

    QVERIFY(!succeeded);
    QVERIFY(!copied.isValid());
    QVERIFY2(
        error.contains(QStringLiteral(
            "incomplete anchored copy cleanup was not durable")),
        qPrintable(error));
    QVERIFY(!QFileInfo::exists(target.displayPath()));
#endif
}

void TestAnchoredFilesystem::moveDoesNotRestoreUnverifiedDestination()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix rename race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        actionReached = true;
        QVERIFY(QFile::rename(destination, retained));
        writeFixture(destination, substitute);
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.verifiedRecoveryPath.isEmpty());
    pinned = {};
    QVERIFY(!QFileInfo::exists(source.displayPath()));
    QCOMPARE(readFixture(target.displayPath()), substitute);
    QCOMPARE(readFixture(retained), original);
#endif
}

void TestAnchoredFilesystem::moveRejectsNewHardLink()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString extra = root.filePath(QStringLiteral("extra-link"));
    writeFixture(source.displayPath(), QByteArray("contents"));
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        actionReached = true;
        QVERIFY(createHardLink(destination, extra));
    };

    const MutationResult result = moveNoReplace(pinned, target);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QCOMPARE(result.verifiedRecoveryPath, target.displayPath());
    QVERIFY(QFileInfo::exists(target.displayPath()));
    QVERIFY(QFileInfo::exists(extra));
}

void TestAnchoredFilesystem::moveUsesPinnedParentAfterPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString retainedPath = root.filePath(QStringLiteral("retained-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("activity"));
    const EntryRef target = entry(targetDirectory, QStringLiteral("moved"));
    const QByteArray contents("same-content identity sentinel");
    writeFixture(source.displayPath(), contents);
    PinnedFile pinned = pin(source);
    const NativeIdentity originalIdentity = pinned.identity();

    const bool replaced = QDir().rename(sourcePath, retainedPath);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(moveNoReplace(pinned, target));
        verifyPinnedAt(pinned, target, originalIdentity);
        return;
    }
    QVERIFY(QDir().mkdir(sourcePath));
    const QString substitutePath =
        QDir(sourcePath).filePath(QStringLiteral("activity"));
    writeFixture(substitutePath, contents);
    const NativeIdentity substituteIdentity =
        pin(entry(openDirectory(sourcePath), QStringLiteral("activity")))
            .identity();

    verifyApplied(moveNoReplace(pinned, target));
    QCOMPARE(readFixture(substitutePath), contents);
    verifyPinnedAt(pinned, target, originalIdentity);
    QVERIFY(originalIdentity != substituteIdentity);
    QVERIFY(!QFileInfo::exists(
        QDir(retainedPath).filePath(QStringLiteral("activity"))));
    QCOMPARE(QDir(sourcePath).entryList(
                 QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList({QStringLiteral("activity")}));
}

void TestAnchoredFilesystem::moveRejectsFinalEntryReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray contents("same bytes");
    writeFixture(source.displayPath(), contents);
    PinnedFile original = pin(source);
    const NativeIdentity originalIdentity = original.identity();

    const bool replaced = QFile::rename(source.displayPath(), retained);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(moveNoReplace(original, target));
        verifyPinnedAt(original, target, originalIdentity);
        return;
    }
    writeFixture(source.displayPath(), contents);
    const NativeIdentity substituteIdentity = pin(source).identity();

    const MutationResult result = moveNoReplace(original, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(pin(source).identity(), substituteIdentity);
    QCOMPARE(pin(entry(directory, QStringLiteral("retained"))).identity(),
             originalIdentity);
    QVERIFY(!QFileInfo::exists(target.displayPath()));
}

void TestAnchoredFilesystem::moveDoesNotReplaceDestination()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef target = entry(directory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    writeFixture(target.displayPath(), QByteArray("target"));
    PinnedFile pinned = pin(source);
    const NativeIdentity sourceIdentity = pinned.identity();
    const NativeIdentity targetIdentity = pin(target).identity();

    const MutationResult result = moveNoReplace(pinned, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    verifyPinnedAt(pinned, source, sourceIdentity);
    QCOMPARE(pin(target).identity(), targetIdentity);
}

void TestAnchoredFilesystem::moveDoesNotReplaceDestinationAcrossDirectories()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString targetPath = root.filePath(QStringLiteral("target-dir"));
    QVERIFY(QDir().mkdir(sourcePath));
    QVERIFY(QDir().mkdir(targetPath));
    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const DirectoryAnchor targetDirectory = openDirectory(targetPath);
    const EntryRef source = entry(
        sourceDirectory, QStringLiteral("source"));
    const EntryRef target = entry(
        targetDirectory, QStringLiteral("target"));
    writeFixture(source.displayPath(), QByteArray("source"));
    writeFixture(target.displayPath(), QByteArray("target"));
    PinnedFile pinned = pin(source);
    const NativeIdentity sourceIdentity = pinned.identity();
    const NativeIdentity targetIdentity = pin(target).identity();

    const MutationResult result = moveNoReplace(pinned, target);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    verifyPinnedAt(pinned, source, sourceIdentity);
    QCOMPARE(pin(target).identity(), targetIdentity);
}

void TestAnchoredFilesystem::removeRejectsPinnedParentPathReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString sourcePath = root.filePath(QStringLiteral("source-dir"));
    const QString retainedPath = root.filePath(QStringLiteral("retained-dir"));
    QVERIFY(QDir().mkdir(sourcePath));

    const DirectoryAnchor sourceDirectory = openDirectory(sourcePath);
    const EntryRef source = entry(sourceDirectory, QStringLiteral("activity"));
    const QByteArray contents("same-content identity sentinel");
    writeFixture(source.displayPath(), contents);
    PinnedFile pinned = pin(source);

    const bool replaced = QDir().rename(sourcePath, retainedPath);
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(remove(pinned));
        QVERIFY(!QFileInfo::exists(
            QDir(sourcePath).filePath(QStringLiteral("activity"))));
        return;
    }
    QVERIFY(QDir().mkdir(sourcePath));
    const QString substitutePath =
        QDir(sourcePath).filePath(QStringLiteral("activity"));
    writeFixture(substitutePath, contents);
    const NativeIdentity substituteIdentity =
        pin(entry(openDirectory(sourcePath), QStringLiteral("activity")))
            .identity();

    const NativeIdentity originalIdentity = pinned.identity();
    const MutationResult result = remove(pinned);
    QCOMPARE(result.effect, MutationEffect::NoEffect);
    QVERIFY(!result.applied());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(pinned.identity(), originalIdentity);
    QCOMPARE(readFixture(substitutePath), contents);
    QCOMPARE(pin(entry(openDirectory(sourcePath),
                       QStringLiteral("activity"))).identity(),
             substituteIdentity);
    const QString retainedActivity =
        QDir(retainedPath).filePath(QStringLiteral("activity"));
    QCOMPARE(readFixture(retainedActivity), contents);
    QCOMPARE(
        pin(entry(openDirectory(retainedPath), QStringLiteral("activity")))
            .identity(),
        originalIdentity);
}

void TestAnchoredFilesystem::removeRejectsFinalEntryReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const EntryRef retained = entry(directory, QStringLiteral("retained"));
    const QByteArray contents("same bytes");
    writeFixture(source.displayPath(), contents);
    PinnedFile original = pin(source);
    const NativeIdentity originalIdentity = original.identity();

    const bool replaced = QFile::rename(
        source.displayPath(), retained.displayPath());
#ifndef Q_OS_WIN
    QVERIFY(replaced);
#endif
    if (!replaced) {
        verifyApplied(remove(original));
        QVERIFY(!QFileInfo::exists(source.displayPath()));
        return;
    }
    writeFixture(source.displayPath(), contents);
    const NativeIdentity substituteIdentity = pin(source).identity();

    const MutationResult result = remove(original);
    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.applied());
    QCOMPARE(pin(source).identity(), substituteIdentity);
    QCOMPARE(pin(retained).identity(), originalIdentity);
}

void TestAnchoredFilesystem::removesAnchoredEmptyDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("journal")));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("journal"))));
    QCOMPARE(
        QDir(root.path()).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList());
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsReplacedParent()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString namespacePath =
        root.filePath(QStringLiteral("namespace"));
    const QString displacedPath =
        root.filePath(QStringLiteral("namespace.displaced"));
    QVERIFY(QDir().mkpath(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));

    const DirectoryAnchor rootAnchor = openDirectory(root.path());
    DirectoryAnchor namespaceAnchor;
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        rootAnchor.openChild(
            QStringLiteral("namespace"), namespaceAnchor, error),
        qPrintable(error));
    QVERIFY2(
        namespaceAnchor.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const bool replaced = QDir().rename(
        namespacePath, displacedPath);
#ifdef Q_OS_WIN
    if (!replaced) {
        QSKIP("Open Windows directory handles prevent this parent race");
    }
#else
    QVERIFY(replaced);
#endif
    QVERIFY(QDir().mkpath(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::Conflict);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("journal"))));
    QVERIFY(QFileInfo::exists(
        QDir(displacedPath).filePath(QStringLiteral("journal"))));
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsPreQuarantineReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString retainedPath =
        root.filePath(QStringLiteral("journal.retained"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    NativeIdentity substituteIdentity;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-before-quarantine") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().rename(target, retainedPath));
        QVERIFY(QDir().mkdir(target));
        DirectoryAnchor substitute;
        QString actionError;
        QVERIFY2(
            parent.openChild(
                QStringLiteral("journal"), substitute, actionError),
            qPrintable(actionError));
        substituteIdentity = substitute.identity();
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFileInfo(retainedPath).isDir());
    QVERIFY(substituteIdentity.isValid());
    const QFileInfoList directories = QDir(root.path()).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    QCOMPARE(directories.size(), 2);
    for (const QFileInfo &entryInfo : directories) {
        if (entryInfo.absoluteFilePath() == retainedPath) continue;
        DirectoryAnchor retainedSubstitute;
        QVERIFY2(
            parent.openChild(
                entryInfo.fileName(), retainedSubstitute, error),
            qPrintable(error));
        QCOMPARE(retainedSubstitute.identity(), substituteIdentity);
    }
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsFinalNameReplacement()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString retainedPath =
        root.filePath(QStringLiteral("journal.retained"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    QString replacementPath;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        replacementPath = target;
        QVERIFY(QDir().rename(target, retainedPath));
        QVERIFY(QDir().mkdir(target));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFileInfo(retainedPath).isDir());
    QVERIFY2(
        QFileInfo(replacementPath).isDir(),
        "Directory removal deleted a substituted final component");
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetainsNonEmptyQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    QString quarantinePath;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        quarantinePath = target;
        writeFixture(
            QDir(target).filePath(QStringLiteral("entry")),
            QByteArray("entry"));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(!quarantinePath.isEmpty());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(journal.isValid());
    QCOMPARE(result.verifiedRecoveryPath, quarantinePath);
    QVERIFY(QFileInfo(QDir(quarantinePath).filePath(
        QStringLiteral("entry"))).isFile());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsRepopulatedOriginalName()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix directory quarantine is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().mkdir(journalPath));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(QFileInfo(journalPath).isDir());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalReportsPartialSyncFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory durability is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-finally-verified") != 0) {
            return;
        }
        writeFixture(
            QDir(target).filePath(QStringLiteral("entry")),
            QByteArray("entry"));
    };
    failDirectorySync = true;

    const MutationResult result = removeEmptyDirectory(journal);
    failDirectorySync = false;
    filesystemAction = {};

    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.error.contains(QStringLiteral("synchronization")));
    QVERIFY(journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalReportsParentSyncFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory durability is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    failDirectorySync = true;
    const MutationResult result = removeEmptyDirectory(journal);
    failDirectorySync = false;

    QCOMPARE(result.effect, MutationEffect::AppliedNotDurable);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalCanRetryAfterNonEmptyFailure()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    const QString childPath =
        QDir(journalPath).filePath(QStringLiteral("entry"));
    QVERIFY(QDir().mkdir(journalPath));
    writeFixture(childPath, QByteArray("entry"));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    const MutationResult first = removeEmptyDirectory(journal);

    QCOMPARE(first.effect, MutationEffect::NoEffect);
    QVERIFY(!first.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(QFile::remove(childPath));

    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY2(retry.error.isEmpty(), qPrintable(retry.error));
    QVERIFY(!journal.isValid());
    QVERIFY(!QFileInfo::exists(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRetriesAfterDeleteSharingConflict()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    {
        WindowsTestHandle observer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(journalPath.utf16()),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        QVERIFY(observer.isValid());

        const MutationResult first = removeEmptyDirectory(journal);

        QCOMPARE(first.effect, MutationEffect::NoEffect);
        QVERIFY(!first.error.isEmpty());
        QVERIFY(journal.isValid());
        error.clear();
        QVERIFY2(journal.pathMatches(error), qPrintable(error));
    }

    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY2(retry.error.isEmpty(), qPrintable(retry.error));
    QVERIFY(!journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsWindowsRepopulation()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory removal is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &target,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-directory-disposition-completed") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QDir().mkdir(target));
    };

    const MutationResult result = removeEmptyDirectory(journal);
    filesystemAction = {};

    QVERIFY(actionReached);
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!journal.isValid());
    QVERIFY(QFileInfo(journalPath).isDir());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalRejectsWindowsAliases()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));
    DirectoryAnchor alias = journal;

    const MutationResult aliased = removeEmptyDirectory(journal);

    QCOMPARE(aliased.effect, MutationEffect::NoEffect);
    QVERIFY(!aliased.error.isEmpty());
    QVERIFY(journal.isValid());
    QVERIFY(alias.isValid());
    error.clear();
    QVERIFY2(journal.pathMatches(error), qPrintable(error));
    error.clear();
    QVERIFY2(alias.pathMatches(error), qPrintable(error));

    alias = {};
    const MutationResult retry = removeEmptyDirectory(journal);

    QCOMPARE(retry.effect, MutationEffect::AppliedDurable);
    QVERIFY(!journal.isValid());
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalUnlinksWindowsNameWithSharedObserver()
{
#ifndef Q_OS_WIN
    QSKIP("Windows shared-delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));
    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(journalPath.utf16()),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());

    const MutationResult result = removeEmptyDirectory(journal);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!journal.isValid());
    QVERIFY(QDir().mkdir(journalPath));
#endif
}

void TestAnchoredFilesystem::
emptyDirectoryRemovalLegacyWindowsDeleteReportsPendingName()
{
#ifndef Q_OS_WIN
    QSKIP("Windows legacy delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString journalPath =
        root.filePath(QStringLiteral("journal"));
    QVERIFY(QDir().mkdir(journalPath));
    const DirectoryAnchor parent = openDirectory(root.path());
    DirectoryAnchor journal;
    QString error;
    QVERIFY2(
        parent.openChild(
            QStringLiteral("journal"), journal, error),
        qPrintable(error));

    {
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
        const MutationResult result = removeEmptyDirectory(journal);
        forceLegacyWindowsDelete = false;

        QCOMPARE(result.effect, MutationEffect::Partial);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!journal.isValid());
        QVERIFY(!QDir().mkdir(journalPath));
    }

    QVERIFY(QDir().mkdir(journalPath));
#endif
}

void TestAnchoredFilesystem::removeRetainsReplacementAtQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &quarantine,
                           const QString &) {
        if (qstrcmp(transition, "remove-quarantine-verified") != 0) return;
        actionReached = true;
        QVERIFY(QFile::rename(quarantine, retained));
        writeFixture(quarantine, substitute);
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QCOMPARE(readFixture(retained), original);
    const QStringList entries = QDir(root.path()).entryList(
        QStringList({QStringLiteral(".gc-remove-*")}),
        QDir::Files | QDir::Hidden);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(readFixture(root.filePath(entries.constFirst())), substitute);
#endif
}

void TestAnchoredFilesystem::removePartialMoveReportsGeneratedQuarantine()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix quarantine move is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    QString quarantine;
    QString extraLink;
    filesystemAction = [&](const char *transition,
                           const QString &,
                           const QString &destination) {
        if (qstrcmp(transition, "move-published") != 0) return;
        quarantine = destination;
        extraLink = destination + QStringLiteral(".extra-link");
        QVERIFY(createHardLink(destination, extraLink));
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(!quarantine.isEmpty());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QCOMPARE(result.verifiedRecoveryPath, quarantine);
    QCOMPARE(readFixture(quarantine), original);
    QCOMPARE(readFixture(extraLink), original);
    QVERIFY(!QFileInfo::exists(source.displayPath()));
#endif
}

void TestAnchoredFilesystem::
removeDetectsReplacementAfterFinalQuarantineCheck()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix unlink race is platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QString retained = root.filePath(QStringLiteral("retained"));
    const QByteArray original("original");
    const QByteArray substitute("substitute");
    writeFixture(source.displayPath(), original);
    PinnedFile pinned = pin(source);
    const NativeIdentity originalIdentity = pinned.identity();
    bool actionReached = false;
    filesystemAction = [&](const char *transition,
                           const QString &quarantine,
                           const QString &) {
        if (qstrcmp(
                transition,
                "remove-quarantine-finally-verified") != 0) {
            return;
        }
        actionReached = true;
        QVERIFY(QFile::rename(quarantine, retained));
        writeFixture(quarantine, substitute);
    };

    const MutationResult result = remove(pinned);
    filesystemAction = {};

    QVERIFY(actionReached);
    QVERIFY(!result.applied());
    QCOMPARE(result.effect, MutationEffect::Partial);
    QVERIFY(result.verifiedRecoveryPath.isEmpty());
    QVERIFY(pinned.isValid());
    QCOMPARE(pinned.identity(), originalIdentity);
    QCOMPARE(readFixture(retained), original);
    const QStringList entries = QDir(root.path()).entryList(
        QStringList({QStringLiteral(".gc-remove-*")}),
        QDir::Files | QDir::Hidden);
    QVERIFY(entries.isEmpty());
#endif
}

void TestAnchoredFilesystem::
removeUnlinksWindowsNameWithSharedObserver()
{
#ifndef Q_OS_WIN
    QSKIP("Windows shared-delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original contents");
    const QByteArray replacement("replacement contents");
    writeFixture(source.displayPath(), original);

    WindowsTestHandle observer(::CreateFileW(
        reinterpret_cast<LPCWSTR>(source.displayPath().utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    QVERIFY(observer.isValid());
    PinnedFile pinned = pin(source);

    const MutationResult result = remove(pinned);

    QCOMPARE(result.effect, MutationEffect::AppliedDurable);
    QVERIFY(!pinned.isValid());
    writeFixture(source.displayPath(), replacement);

    LARGE_INTEGER start {};
    QVERIFY(::SetFilePointerEx(
        observer.get(), start, nullptr, FILE_BEGIN));
    QByteArray observed(original.size(), '\0');
    DWORD bytesRead = 0;
    QVERIFY(::ReadFile(
        observer.get(), observed.data(), DWORD(observed.size()),
        &bytesRead, nullptr));
    QCOMPARE(bytesRead, DWORD(original.size()));
    QCOMPARE(observed, original);
    QCOMPARE(readFixture(source.displayPath()), replacement);
#endif
}

void TestAnchoredFilesystem::
removeLegacyWindowsDeleteReportsPendingName()
{
#ifndef Q_OS_WIN
    QSKIP("Windows legacy delete semantics are platform-specific");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    const EntryRef source = entry(directory, QStringLiteral("source"));
    const QByteArray original("original contents");
    writeFixture(source.displayPath(), original);

    {
        WindowsTestHandle observer(::CreateFileW(
            reinterpret_cast<LPCWSTR>(source.displayPath().utf16()),
            GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        QVERIFY(observer.isValid());
        PinnedFile pinned = pin(source);

        forceLegacyWindowsDelete = true;
        const MutationResult result = remove(pinned);
        forceLegacyWindowsDelete = false;

        QCOMPARE(result.effect, MutationEffect::Partial);
        QVERIFY(!pinned.isValid());
        QFile replacement(source.displayPath());
        QVERIFY(!replacement.open(
            QIODevice::WriteOnly | QIODevice::NewOnly));

        LARGE_INTEGER start {};
        QVERIFY(::SetFilePointerEx(
            observer.get(), start, nullptr, FILE_BEGIN));
        QByteArray observed(original.size(), '\0');
        DWORD bytesRead = 0;
        QVERIFY(::ReadFile(
            observer.get(), observed.data(), DWORD(observed.size()),
            &bytesRead, nullptr));
        QCOMPARE(bytesRead, DWORD(original.size()));
        QCOMPARE(observed, original);
    }

    writeFixture(source.displayPath(), QByteArray("replacement"));
#endif
}

void TestAnchoredFilesystem::syncsPinnedDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const DirectoryAnchor directory = openDirectory(root.path());
    QString error;
    QVERIFY(directory.sync(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

QTEST_GUILESS_MAIN(TestAnchoredFilesystem)

#include "testAnchoredFilesystem.moc"
