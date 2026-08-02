#include <QtTest>

#include "AnchoredFileSystem.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <functional>

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

} // namespace

void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary)
{
    if (filesystemAction) filesystemAction(
        transition, primary, secondary);
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
    void readsPinnedContentsAfterPathReplacement();
    void directoryAnchorSurvivesPathReplacement();
    void directoryAnchorDetectsPathReplacement();
    void childAnchorsRemainInOneRootGeneration();
    void inspectsEntriesThroughPinnedDirectory();
    void copiesPinnedContentsThroughAnchoredParents();
    void copyDoesNotReplaceDestination();
    void moveDoesNotRestoreUnverifiedDestination();
    void moveRejectsNewHardLink();
    void moveUsesPinnedParentAfterPathReplacement();
    void moveRejectsFinalEntryReplacement();
    void moveDoesNotReplaceDestination();
    void moveDoesNotReplaceDestinationAcrossDirectories();
    void removeUsesPinnedParentAfterPathReplacement();
    void removeRejectsFinalEntryReplacement();
    void removeRetainsReplacementAtQuarantine();
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
    const PinnedFile original = pin(source);

    const bool replaced = QFile::rename(source.displayPath(), retained);
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
        QCOMPARE(readFixture(retained), originalContents);
    }
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
    pinned = {};
    QVERIFY(!QFileInfo::exists(source.displayPath()));
    QCOMPARE(readFixture(target.displayPath()), substitute);
    QCOMPARE(readFixture(retained), original);
#endif
}

void TestAnchoredFilesystem::moveRejectsNewHardLink()
{
#ifndef Q_OS_UNIX
    QSKIP("The Unix hard-link race is platform-specific");
#else
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
    QVERIFY(QFileInfo::exists(target.displayPath()));
    QVERIFY(QFileInfo::exists(extra));
#endif
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

void TestAnchoredFilesystem::removeUsesPinnedParentAfterPathReplacement()
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

    verifyApplied(remove(pinned));
    QCOMPARE(readFixture(substitutePath), contents);
    QCOMPARE(pin(entry(openDirectory(sourcePath),
                       QStringLiteral("activity"))).identity(),
             substituteIdentity);
    QVERIFY(!QFileInfo::exists(
        QDir(retainedPath).filePath(QStringLiteral("activity"))));
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
