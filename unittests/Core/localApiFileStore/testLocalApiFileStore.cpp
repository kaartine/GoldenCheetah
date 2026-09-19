#include "Core/LocalApiFileStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size()
        && file.flush();
}

bool createHardLink(const QString &existing, const QString &link)
{
#ifdef Q_OS_UNIX
    return ::link(
        QFile::encodeName(existing).constData(),
        QFile::encodeName(link).constData()) == 0;
#elif defined(Q_OS_WIN)
    return ::CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(link.utf16()),
        reinterpret_cast<LPCWSTR>(existing.utf16()),
        nullptr) != FALSE;
#else
    Q_UNUSED(existing)
    Q_UNUSED(link)
    return false;
#endif
}

} // namespace

class TestLocalApiFileStore : public QObject
{
    Q_OBJECT

private slots:
    void opensRetainedRegularFile();
    void rejectsDirectoryAlias();
    void rejectsFileAlias();
    void rejectsHardLinkedFile();
    void rejectsReplacementBeforeRead();
    void detectsMutationBeforePublishingBytes();
    void rejectsRelativeOrEmptyRoot_data();
    void rejectsRelativeOrEmptyRoot();
    void enforcesMaximumSize();
    void requiresMaximumSize();
};

void TestLocalApiFileStore::opensRetainedRegularFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/ride.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("ride-data")));

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY2(store.captureRegularFile(
                 {QStringLiteral("alice"), QStringLiteral("activities")},
                 QStringLiteral("ride.fit"), generation, error, 1024),
             qPrintable(error));
    QCOMPARE(generation.size(), qint64(9));

    QByteArray contents;
    QVERIFY2(generation.readAll(contents, error), qPrintable(error));
    QCOMPARE(contents, QByteArrayLiteral("ride-data"));
}

void TestLocalApiFileStore::rejectsDirectoryAlias()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice")));
    if (!QFile::link(
            outside.path(), QDir(root.path()).filePath(
                                QStringLiteral("alice/activities")))) {
        QSKIP("Directory aliases are unavailable in this test environment");
    }

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {QStringLiteral("alice"), QStringLiteral("activities")},
        QStringLiteral("ride.fit"), generation, error, 1024));
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::rejectsFileAlias()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString target = QDir(outside.path()).filePath(
        QStringLiteral("secret.fit"));
    QVERIFY(writeFile(target, QByteArrayLiteral("secret")));
    if (!QFile::link(
            target, QDir(root.path()).filePath(
                        QStringLiteral("alice/activities/ride.fit")))) {
        QSKIP("File aliases are unavailable in this test environment");
    }

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {QStringLiteral("alice"), QStringLiteral("activities")},
        QStringLiteral("ride.fit"), generation, error, 1024));
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::rejectsHardLinkedFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/ride.fit"));
    const QString alias = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/alias.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("ride-data")));
    if (!createHardLink(path, alias)) {
        QSKIP("Hard links are unavailable on this filesystem");
    }

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {QStringLiteral("alice"), QStringLiteral("activities")},
        QStringLiteral("ride.fit"), generation, error, 1024));
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::rejectsReplacementBeforeRead()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/ride.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("original")));

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY2(store.captureRegularFile(
                 {QStringLiteral("alice"), QStringLiteral("activities")},
                 QStringLiteral("ride.fit"), generation, error, 1024),
             qPrintable(error));
    QVERIFY(QFile::remove(path));
    QVERIFY(writeFile(path, QByteArrayLiteral("replacement")));

    QByteArray contents("must be cleared");
    QVERIFY(!generation.readAll(contents, error));
    QVERIFY(contents.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::detectsMutationBeforePublishingBytes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/ride.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("original")));

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY2(store.captureRegularFile(
                 {QStringLiteral("alice"), QStringLiteral("activities")},
                 QStringLiteral("ride.fit"), generation, error, 1024),
             qPrintable(error));

#ifdef Q_OS_WIN
    // The retained Windows handle intentionally denies concurrent writes.
    QVERIFY(!writeFile(path, QByteArrayLiteral("modified")));
    QByteArray contents;
    QVERIFY2(generation.readAll(contents, error), qPrintable(error));
    QCOMPARE(contents, QByteArrayLiteral("original"));
#else
    QVERIFY(writeFile(path, QByteArrayLiteral("modified")));
    QByteArray contents("must be cleared");
    QVERIFY(!generation.readAll(contents, error));
    QVERIFY(contents.isEmpty());
    QVERIFY(!error.isEmpty());
#endif
}

void TestLocalApiFileStore::rejectsRelativeOrEmptyRoot_data()
{
    QTest::addColumn<QString>("root");
    QTest::newRow("empty") << QString();
    QTest::newRow("relative") << QStringLiteral("athletes");
}

void TestLocalApiFileStore::rejectsRelativeOrEmptyRoot()
{
    QFETCH(QString, root);
    LocalApiFileStore store(root);
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {}, QStringLiteral("ride.fit"), generation, error, 1024));
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::enforcesMaximumSize()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/config")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/config/power.zones"));
    QVERIFY(writeFile(path, QByteArrayLiteral("12345")));

    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {QStringLiteral("alice"), QStringLiteral("config")},
        QStringLiteral("power.zones"), generation, error, 4));
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::requiresMaximumSize()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    LocalApiFileStore store(root.path());
    LocalApiFileGeneration generation;
    QString error;
    QVERIFY(!store.captureRegularFile(
        {}, QStringLiteral("ride.fit"), generation, error, -1));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestLocalApiFileStore)
#include "testLocalApiFileStore.moc"
