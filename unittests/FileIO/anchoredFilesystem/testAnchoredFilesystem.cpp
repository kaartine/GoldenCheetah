#include <QtTest>

#include "AnchoredFileSystem.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace AnchoredFileSystem;

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
    void directoryAnchorSurvivesPathReplacement();
    void moveUsesPinnedParentAfterPathReplacement();
    void moveRejectsFinalEntryReplacement();
    void moveDoesNotReplaceDestination();
    void moveDoesNotReplaceDestinationAcrossDirectories();
    void removeUsesPinnedParentAfterPathReplacement();
    void removeRejectsFinalEntryReplacement();
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
