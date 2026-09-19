#include "Core/LocalApiEndpointInput.h"
#include "Core/LocalApiFileStore.h"

#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>
#include <limits>

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
    void readsRelativeToRetainedDirectory();
    void rejectsReplacedRetainedDirectoryPath();
    void detectsMutationBeforePublishingBytes();
    void rejectsRelativeOrEmptyRoot_data();
    void rejectsRelativeOrEmptyRoot();
    void enforcesMaximumSize();
    void requiresMaximumSize();
    void writesPrivateSnapshot();
    void rejectsUnsafeSnapshotName();
    void removesSnapshotDirectoryAtEndOfLifetime();
    void separateSnapshotsPermitMatchingNames();
    void endpointPolicyAcceptsLimitAndRejectsLargerFile();
    void endpointRejectsReplacedRetainedAthlete();
    void meanMaxFailurePublishesNoInput();
    void meanMaxMatchingNamesUseSeparateSnapshotsAndCleanUp();
    void endpointContracts_data();
    void endpointContracts();
    void unavailableMeanMaxPreservesCsvContract();
    void rideDatabaseDecodePreservesTextStreamBehavior();
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

void TestLocalApiFileStore::readsRelativeToRetainedDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString path = QDir(root.path()).filePath(
        QStringLiteral("alice/activities/ride.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("retained")));

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));
    LocalApiFileGeneration generation;
    QVERIFY2(store.captureRegularFile(
                 athleteDirectory, {QStringLiteral("activities")},
                 QStringLiteral("ride.fit"), generation, error, 1024),
             qPrintable(error));
    QByteArray contents;
    QVERIFY2(generation.readAll(contents, error), qPrintable(error));
    QCOMPARE(contents, QByteArrayLiteral("retained"));
}

void TestLocalApiFileStore::rejectsReplacedRetainedDirectoryPath()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString athletePath = QDir(root.path()).filePath(
        QStringLiteral("alice"));
    const QString retainedPath = QDir(root.path()).filePath(
        QStringLiteral("retained-alice"));
    QVERIFY(writeFile(
        QDir(athletePath).filePath(QStringLiteral("activities/ride.fit")),
        QByteArrayLiteral("retained")));

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));
    QVERIFY(QDir().rename(athletePath, retainedPath));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    QVERIFY(writeFile(
        QDir(athletePath).filePath(QStringLiteral("activities/ride.fit")),
        QByteArrayLiteral("replacement")));

    LocalApiFileGeneration generation;
    QVERIFY(!store.captureRegularFile(
        athleteDirectory, {QStringLiteral("activities")},
        QStringLiteral("ride.fit"), generation, error, 1024));
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

void TestLocalApiFileStore::writesPrivateSnapshot()
{
    LocalApiFileSnapshotDirectory snapshot;
    QVERIFY(snapshot.isValid());
    QString path;
    QString error;
    QVERIFY2(snapshot.writeFile(
                 QStringLiteral("ride.fit"),
                 QByteArrayLiteral("verified"), path, error),
             qPrintable(error));
    QVERIFY(QFileInfo(path).isFile());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("verified"));
    const QFileDevice::Permissions permissions = file.permissions();
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadOther));
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions directoryPermissions =
        QFileInfo(QFileInfo(path).absolutePath()).permissions();
    QVERIFY(directoryPermissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(directoryPermissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(directoryPermissions.testFlag(QFileDevice::ExeOwner));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::WriteGroup));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::ExeGroup));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::ReadOther));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::WriteOther));
    QVERIFY(!directoryPermissions.testFlag(QFileDevice::ExeOther));
#endif
}

void TestLocalApiFileStore::rejectsUnsafeSnapshotName()
{
    LocalApiFileSnapshotDirectory snapshot;
    QString path;
    QString error;
    QVERIFY(!snapshot.writeFile(
        QStringLiteral("../ride.fit"),
        QByteArrayLiteral("untrusted"), path, error));
    QVERIFY(path.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestLocalApiFileStore::removesSnapshotDirectoryAtEndOfLifetime()
{
    QString directoryPath;
    {
        LocalApiFileSnapshotDirectory snapshot;
        QString path;
        QString error;
        QVERIFY2(snapshot.writeFile(
                     QStringLiteral("ride.fit"),
                     QByteArrayLiteral("verified"), path, error),
                 qPrintable(error));
        directoryPath = QFileInfo(path).absolutePath();
        QVERIFY(QFileInfo::exists(directoryPath));
    }
    QVERIFY(!QFileInfo::exists(directoryPath));
}

void TestLocalApiFileStore::separateSnapshotsPermitMatchingNames()
{
    LocalApiFileSnapshotDirectory sourceSnapshot;
    LocalApiFileSnapshotDirectory cacheSnapshot;
    QString sourcePath;
    QString cachePath;
    QString error;
    QVERIFY2(sourceSnapshot.writeFile(
                 QStringLiteral("ride.cpx"),
                 QByteArrayLiteral("source"), sourcePath, error),
             qPrintable(error));
    QVERIFY2(cacheSnapshot.writeFile(
                 QStringLiteral("ride.cpx"),
                 QByteArrayLiteral("cache"), cachePath, error),
             qPrintable(error));
    QVERIFY(QFileInfo(sourcePath).absolutePath()
            != QFileInfo(cachePath).absolutePath());
}

void TestLocalApiFileStore::endpointPolicyAcceptsLimitAndRejectsLargerFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/config")));
    const qint64 limit = LocalApiEndpointInput::maximumSize(
        LocalApiEndpointInput::FileKind::Zone);
    QVERIFY(limit > 0 && limit <= std::numeric_limits<int>::max());

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));

    const QString acceptedPath = QDir(root.path()).filePath(
        QStringLiteral("alice/config/accepted.zones"));
    QVERIFY(writeFile(acceptedPath, QByteArray(int(limit), 'a')));
    LocalApiEndpointInput::PreparedInput accepted =
        LocalApiEndpointInput::prepareBytes(
            store, athleteDirectory, QStringLiteral("config"),
            QStringLiteral("accepted.zones"),
            LocalApiEndpointInput::FileKind::Zone, error);
    QVERIFY2(accepted.status() == LocalApiEndpointInput::Status::Ready,
             qPrintable(error));
    QCOMPARE(accepted.bytes().size(), int(limit));

    const QString rejectedPath = QDir(root.path()).filePath(
        QStringLiteral("alice/config/rejected.zones"));
    QVERIFY(writeFile(rejectedPath, QByteArray(int(limit + 1), 'b')));
    LocalApiEndpointInput::PreparedInput rejected =
        LocalApiEndpointInput::prepareBytes(
            store, athleteDirectory, QStringLiteral("config"),
            QStringLiteral("rejected.zones"),
            LocalApiEndpointInput::FileKind::Zone, error);
    QVERIFY(rejected.status()
            == LocalApiEndpointInput::Status::Unavailable);
    QVERIFY(rejected.bytes().isEmpty());
}

void TestLocalApiFileStore::endpointRejectsReplacedRetainedAthlete()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    const QString athletePath = QDir(root.path()).filePath(
        QStringLiteral("alice"));
    const QString retainedPath = QDir(root.path()).filePath(
        QStringLiteral("retained-alice"));
    QVERIFY(writeFile(
        QDir(athletePath).filePath(QStringLiteral("activities/ride.fit")),
        QByteArrayLiteral("retained")));

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));
    QVERIFY(QDir().rename(athletePath, retainedPath));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    QVERIFY(writeFile(
        QDir(athletePath).filePath(QStringLiteral("activities/ride.fit")),
        QByteArrayLiteral("replacement")));

    LocalApiEndpointInput::PreparedInput input =
        LocalApiEndpointInput::prepareSnapshot(
            store, athleteDirectory, QStringLiteral("activities"),
            QStringLiteral("ride.fit"),
            LocalApiEndpointInput::FileKind::Activity, error);
    QVERIFY(input.status()
            == LocalApiEndpointInput::Status::Unavailable);
    QVERIFY(input.firstPath().isEmpty());
}

void TestLocalApiFileStore::meanMaxFailurePublishesNoInput()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/cache")));
    QVERIFY(writeFile(
        QDir(root.path()).filePath(QStringLiteral("alice/activities/ride.fit")),
        QByteArrayLiteral("activity")));

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));
    LocalApiEndpointInput::PreparedInput input =
        LocalApiEndpointInput::prepareMeanMax(
            store, athleteDirectory, QStringLiteral("ride.fit"), error);
    QVERIFY(input.status()
            == LocalApiEndpointInput::Status::Unavailable);
    QVERIFY(input.bytes().isEmpty());
    QVERIFY(input.firstPath().isEmpty());
    QVERIFY(input.secondPath().isEmpty());
}

void TestLocalApiFileStore::meanMaxMatchingNamesUseSeparateSnapshotsAndCleanUp()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/activities")));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("alice/cache")));
    QVERIFY(writeFile(
        QDir(root.path()).filePath(QStringLiteral("alice/activities/ride.cpx")),
        QByteArrayLiteral("activity")));
    QVERIFY(writeFile(
        QDir(root.path()).filePath(QStringLiteral("alice/cache/ride.cpx")),
        QByteArrayLiteral("cache")));

    LocalApiFileStore store(root.path());
    AnchoredFileSystem::DirectoryAnchor athleteDirectory;
    QString error;
    QVERIFY2(store.openDirectory(
                 {QStringLiteral("alice")}, athleteDirectory, error),
             qPrintable(error));
    QString firstDirectory;
    QString secondDirectory;
    {
        LocalApiEndpointInput::PreparedInput input =
            LocalApiEndpointInput::prepareMeanMax(
                store, athleteDirectory, QStringLiteral("ride.cpx"), error);
        QVERIFY2(input.status() == LocalApiEndpointInput::Status::Ready,
                 qPrintable(error));
        firstDirectory = QFileInfo(input.firstPath()).absolutePath();
        secondDirectory = QFileInfo(input.secondPath()).absolutePath();
        QVERIFY(firstDirectory != secondDirectory);
        QVERIFY(QFileInfo::exists(input.firstPath()));
        QVERIFY(QFileInfo::exists(input.secondPath()));
    }
    QVERIFY(!QFileInfo::exists(firstDirectory));
    QVERIFY(!QFileInfo::exists(secondDirectory));
}

void TestLocalApiFileStore::endpointContracts_data()
{
    QTest::addColumn<int>("endpoint");
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("httpStatus");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("processInput");
    QTest::newRow("activity-ready")
        << int(LocalApiEndpointInput::Endpoint::Activity)
        << int(LocalApiEndpointInput::Status::Ready)
        << 200 << QByteArray() << true;
    QTest::newRow("activity-unavailable")
        << int(LocalApiEndpointInput::Endpoint::Activity)
        << int(LocalApiEndpointInput::Status::Unavailable)
        << 404 << QByteArrayLiteral("file not found or unsafe") << false;
    QTest::newRow("zone-ready")
        << int(LocalApiEndpointInput::Endpoint::Zone)
        << int(LocalApiEndpointInput::Status::Ready)
        << 200 << QByteArray() << true;
    QTest::newRow("zone-unavailable")
        << int(LocalApiEndpointInput::Endpoint::Zone)
        << int(LocalApiEndpointInput::Status::Unavailable)
        << 500 << QByteArray() << false;
    QTest::newRow("snapshot-error")
        << int(LocalApiEndpointInput::Endpoint::Activity)
        << int(LocalApiEndpointInput::Status::InternalError)
        << 500
        << QByteArrayLiteral("unable to create a private input snapshot")
        << false;
    QTest::newRow("mean-max-snapshot-error")
        << int(LocalApiEndpointInput::Endpoint::MeanMax)
        << int(LocalApiEndpointInput::Status::InternalError)
        << 500
        << QByteArrayLiteral("unable to create private mean-max snapshots\n")
        << false;
}

void TestLocalApiFileStore::endpointContracts()
{
    QFETCH(int, endpoint);
    QFETCH(int, status);
    QFETCH(int, httpStatus);
    QFETCH(QByteArray, body);
    QFETCH(bool, processInput);
    const LocalApiEndpointInput::Contract result =
        LocalApiEndpointInput::contract(
            LocalApiEndpointInput::Endpoint(endpoint),
            LocalApiEndpointInput::Status(status));
    QCOMPARE(result.statusCode, httpStatus);
    QCOMPARE(result.bodyPrefix, body);
    QCOMPARE(result.processInput, processInput);
}

void TestLocalApiFileStore::unavailableMeanMaxPreservesCsvContract()
{
    const LocalApiEndpointInput::Contract result =
        LocalApiEndpointInput::contract(
            LocalApiEndpointInput::Endpoint::MeanMax,
            LocalApiEndpointInput::Status::Unavailable,
            QByteArrayLiteral("watts"));
    QCOMPARE(result.statusCode, 200);
    QCOMPARE(result.bodyPrefix, QByteArrayLiteral("secs, watts\n"));
    QVERIFY(!result.processInput);
}

void TestLocalApiFileStore::rideDatabaseDecodePreservesTextStreamBehavior()
{
    const QByteArray contents = QByteArray::fromHex("efbbbf")
        + QString::fromUtf8("Mäki – 東京\n").toUtf8();
    QByteArray expectedBytes = contents;
    QBuffer buffer(&expectedBytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QTextStream stream(&buffer);
    const QString expected = stream.readAll();
    QCOMPARE(LocalApiEndpointInput::decodeRideDatabase(contents), expected);
    QVERIFY(!expected.startsWith(QChar::ByteOrderMark));
    QVERIFY(expected.contains(QString::fromUtf8("Mäki – 東京")));
}

QTEST_MAIN(TestLocalApiFileStore)
#include "testLocalApiFileStore.moc"
