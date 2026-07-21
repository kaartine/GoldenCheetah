#include <QtTest>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FileIO/AthleteBackupArchive.h"
#include "zipreader.h"
#include "zipwriter.h"

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;

    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
}

class FailingReadDevice final : public QIODevice
{
public:
    FailingReadDevice(QByteArray data, qint64 failAfter)
        : data_(std::move(data)), failAfter_(failAfter)
    {
        open(QIODevice::ReadOnly);
    }

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (offset_ >= failAfter_) return -1;
        const qint64 available = std::min(
            failAfter_ - offset_,
            static_cast<qint64>(data_.size()) - offset_);
        const qint64 count = std::min(maxSize, available);
        if (count <= 0) return 0;
        std::memcpy(data,
                    data_.constData() + static_cast<qsizetype>(offset_),
                    static_cast<size_t>(count));
        offset_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override { return -1; }

private:
    QByteArray data_;
    qint64 failAfter_;
    qint64 offset_ = 0;
};

class InterleavingReadDevice final : public QIODevice
{
public:
    InterleavingReadDevice(QByteArray data, QString archivePath)
        : data_(std::move(data)), archivePath_(std::move(archivePath))
    {
        open(QIODevice::ReadOnly);
    }

    bool isSequential() const override { return true; }
    bool observedOutputBeforeEnd() const { return observedOutputBeforeEnd_; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (offset_ > 0
            && offset_ < data_.size()
            && QFileInfo(archivePath_).size() > 0) {
            observedOutputBeforeEnd_ = true;
        }
        if (offset_ >= data_.size()) return 0;

        const qint64 count = std::min<qint64>(
            std::min<qint64>(maxSize, 4096),
            data_.size() - offset_);
        std::memcpy(data,
                    data_.constData() + static_cast<qsizetype>(offset_),
                    static_cast<size_t>(count));
        offset_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override { return -1; }

private:
    QByteArray data_;
    QString archivePath_;
    qint64 offset_ = 0;
    bool observedOutputBeforeEnd_ = false;
};

class FailingWriteBuffer final : public QBuffer
{
public:
    explicit FailingWriteBuffer(qint64 bytesBeforeFailure)
        : bytesBeforeFailure_(bytesBeforeFailure)
    {
    }

    void failWrites() { bytesBeforeFailure_ = 0; }

protected:
    qint64 writeData(const char *data, qint64 length) override
    {
        if (bytesBeforeFailure_ <= 0) return -1;
        const qint64 accepted = std::min(length, bytesBeforeFailure_);
        const qint64 written = QBuffer::writeData(data, accepted);
        if (written > 0) bytesBeforeFailure_ -= written;
        return written;
    }

private:
    qint64 bytesBeforeFailure_;
};

QStringList archiveFilePaths(const QString &archivePath)
{
    ZipReader reader(archivePath);
    if (reader.status() != ZipReader::NoError) return {};

    QStringList paths;
    const QList<ZipReader::FileInfo> entries = reader.fileInfoList();
    for (const ZipReader::FileInfo &entry : entries) {
        if (entry.isFile) paths.append(entry.filePath);
    }
    paths.sort();
    return paths;
}

} // namespace

class TestAthleteBackupArchive : public QObject
{
    Q_OBJECT

private slots:
    void manifestIncludesAllPersistentDataRecursively();
    void manifestRejectsSymlinks();
    void manifestReportsReadFailure();
    void zipStreamsSourceData();
    void zipReportsSourceReadFailure();
    void zipReportsOutputWriteFailure();
    void zipReportsDirectoryWriteFailure();
    void zipRejectsNullOutputDevice();
    void publicationIsVerifiedAndExact();
    void verificationAllowsTrustedHighRatioEntry();
    void changedSourceIsNotPublished();
    void corruptPayloadIsRejected();
    void corruptArchiveIsNotPublished();
    void failedBuildDoesNotDamageExistingTarget();
    void cancellationDoesNotPublish();
};

void TestAthleteBackupArchive::manifestIncludesAllPersistentDataRecursively()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));

    const QMap<QString, QByteArray> expected = {
        {QStringLiteral("activities/top.json"), QByteArray("activity")},
        {QStringLiteral("imports/device/nested.fit"), QByteArray("import")},
        {QStringLiteral("media/video/clip.bin"), QByteArray(128, '\x17')},
        {QStringLiteral("planned/season/week/session.json"), QByteArray("plan")},
        {QStringLiteral("quarantine/problem.fit"), QByteArray("quarantine")},
        {QStringLiteral("snippets/favorite.xml"), QByteArray("snippet")},
        {QStringLiteral("global/trainDB"), QByteArray("sqlite")},
        {QStringLiteral("global/trainDB-journal"), QByteArray("journal")},
        {QStringLiteral("global/trainDB-shm"), QByteArray("shared memory")},
        {QStringLiteral("global/trainDB-wal"), QByteArray("write ahead log")}
    };

    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const QString path = it.key().startsWith(QStringLiteral("global/"))
            ? globalHome.filePath(it.key().mid(7))
            : athleteHome.filePath(it.key());
        QVERIFY2(writeFile(path, it.value()), qPrintable(path));
    }
    QVERIFY(writeFile(
        athleteHome.filePath(QStringLiteral("cache/derived.bin")),
        QByteArray("cache")));
    QVERIFY(writeFile(
        athleteHome.filePath(QStringLiteral("temp/partial.tmp")),
        QByteArray("temporary")));
    QVERIFY(writeFile(
        athleteHome.filePath(QStringLiteral("logs/application.log")),
        QByteArray("log")));

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY2(buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error), qPrintable(error));

    QStringList actualPaths;
    qint64 expectedSize = 0;
    for (const AthleteBackupArchiveEntry &entry : manifest) {
        actualPaths.append(entry.archivePath);
        QCOMPARE(entry.size, expected.value(entry.archivePath).size());
        QVERIFY(entry.crc32 != 0);
        expectedSize += expected.value(entry.archivePath).size();
    }
    QStringList expectedPaths = expected.keys();
    expectedPaths.sort();
    QCOMPARE(actualPaths, expectedPaths);
    QCOMPARE(totalSize, expectedSize);
}

void TestAthleteBackupArchive::manifestRejectsSymlinks()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider/planned")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QString outside = globalHome.filePath(QStringLiteral("outside.fit"));
    QVERIFY(writeFile(outside, QByteArray("outside")));
    QVERIFY(QFile::link(
        outside,
        athleteHome.filePath(QStringLiteral("planned/linked.fit"))));

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY(!buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error));
    QVERIFY2(error.contains(QStringLiteral("symbolic"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(manifest.isEmpty());
}

void TestAthleteBackupArchive::manifestReportsReadFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider/planned")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QString source =
        athleteHome.filePath(QStringLiteral("planned/session.json"));
    QVERIFY(writeFile(source, QByteArray("planned workout")));

    const AthleteBackupSourceFactory sourceFactory =
        [source](const QString &path) -> std::unique_ptr<QIODevice> {
            if (path == source) {
                return std::make_unique<FailingReadDevice>(
                    QByteArray("planned workout"), 4);
            }
            return std::make_unique<QFile>(path);
        };

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY(!buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error, sourceFactory));
    QVERIFY2(error.contains(QStringLiteral("read"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(manifest.isEmpty());
}

void TestAthleteBackupArchive::zipStreamsSourceData()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString archivePath =
        QDir(temporary.path()).filePath(QStringLiteral("stream.zip"));
    QByteArray contents(2 * 1024 * 1024, '\0');
    for (qsizetype index = 0; index < contents.size(); ++index) {
        contents[index] = static_cast<char>((index * 131 + 17) & 0xff);
    }

    InterleavingReadDevice source(contents, archivePath);
    ZipWriter writer(archivePath);
    writer.setCompressionPolicy(ZipWriter::NeverCompress);
    writer.addFile(QStringLiteral("media/large.bin"), &source);
    writer.close();

    QCOMPARE(writer.status(), ZipWriter::NoError);
    QVERIFY(source.observedOutputBeforeEnd());

    ZipReader reader(archivePath);
    QCOMPARE(reader.status(), ZipReader::NoError);
    QCOMPARE(reader.fileData(QStringLiteral("media/large.bin")), contents);
}

void TestAthleteBackupArchive::zipReportsSourceReadFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString archivePath =
        QDir(temporary.path()).filePath(QStringLiteral("read-error.zip"));
    FailingReadDevice source(QByteArray("abcdefgh"), 4);
    ZipWriter writer(archivePath);
    writer.addFile(QStringLiteral("activity.json"), &source);
    writer.close();

    QCOMPARE(writer.status(), ZipWriter::FileReadError);
}

void TestAthleteBackupArchive::zipReportsOutputWriteFailure()
{
    FailingWriteBuffer output(48);
    QVERIFY(output.open(QIODevice::WriteOnly));

    ZipWriter writer(&output);
    writer.setCompressionPolicy(ZipWriter::NeverCompress);
    writer.addFile(QStringLiteral("activity.json"), QByteArray(1024, '\x23'));
    writer.close();

    QCOMPARE(writer.status(), ZipWriter::FileWriteError);
}

void TestAthleteBackupArchive::zipReportsDirectoryWriteFailure()
{
    const QString fileName = QStringLiteral("activity.json");
    const QByteArray payload("ride");
    FailingWriteBuffer output(1024);
    QVERIFY(output.open(QIODevice::WriteOnly));

    ZipWriter writer(&output);
    writer.setCompressionPolicy(ZipWriter::NeverCompress);
    writer.addFile(fileName, payload);
    QCOMPARE(writer.status(), ZipWriter::NoError);

    output.failWrites();
    writer.close();
    QCOMPARE(writer.status(), ZipWriter::FileWriteError);
}

void TestAthleteBackupArchive::zipRejectsNullOutputDevice()
{
    ZipWriter writer(static_cast<QIODevice *>(nullptr));

    QCOMPARE(writer.status(), ZipWriter::FileError);
    QVERIFY(!writer.isWritable());
    writer.close();
}

void TestAthleteBackupArchive::publicationIsVerifiedAndExact()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QMap<QString, QByteArray> expected = {
        {QStringLiteral("activities/ride.json"), QByteArray("ride")},
        {QStringLiteral("planned/week/session.json"), QByteArray("session")},
        {QStringLiteral("global/trainDB"), QByteArray("database")}
    };
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const QString path = it.key().startsWith(QStringLiteral("global/"))
            ? globalHome.filePath(it.key().mid(7))
            : athleteHome.filePath(it.key());
        QVERIFY(writeFile(path, it.value()));
    }

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY2(buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error), qPrintable(error));

    const QString target = globalHome.filePath(QStringLiteral("verified.zip"));
    QVERIFY2(publishVerifiedAthleteBackup(target, manifest, error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(target));

    QStringList expectedPaths = expected.keys();
    expectedPaths.sort();
    QCOMPARE(archiveFilePaths(target), expectedPaths);

    ZipReader reader(target);
    QCOMPARE(reader.status(), ZipReader::NoError);
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        QCOMPARE(reader.fileData(it.key()), it.value());
    }

    QString verifyError;
    QVERIFY2(verifyAthleteBackupArchive(target, manifest, verifyError),
             qPrintable(verifyError));
}

void TestAthleteBackupArchive::verificationAllowsTrustedHighRatioEntry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider/activities")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QString source = athleteHome.filePath(
        QStringLiteral("activities/high-ratio.bin"));
    QVERIFY(writeFile(source, QByteArray(2 * 1024 * 1024, '\0')));

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY2(buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error),
        qPrintable(error));
    QCOMPARE(manifest.size(), 1);

    const QString target =
        globalHome.filePath(QStringLiteral("high-ratio.zip"));
    QVERIFY2(publishVerifiedAthleteBackup(target, manifest, error),
             qPrintable(error));
    QVERIFY2(verifyAthleteBackupArchive(target, manifest, error),
             qPrintable(error));
}

void TestAthleteBackupArchive::changedSourceIsNotPublished()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider/activities")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QString source =
        athleteHome.filePath(QStringLiteral("activities/ride.json"));
    QVERIFY(writeFile(source, QByteArray("before")));

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY(buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error));
    QVERIFY(writeFile(source, QByteArray("after!")));

    const QString target = globalHome.filePath(QStringLiteral("changed.zip"));
    QVERIFY(!publishVerifiedAthleteBackup(target, manifest, error));
    QVERIFY2(error.contains(QStringLiteral("match"), Qt::CaseInsensitive)
                 || error.contains(QStringLiteral("changed"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(target));
}

void TestAthleteBackupArchive::corruptPayloadIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir globalHome(temporary.path());
    QVERIFY(globalHome.mkpath(QStringLiteral("Rider/activities")));
    const QDir athleteHome(globalHome.filePath(QStringLiteral("Rider")));
    const QString sourcePath =
        athleteHome.filePath(QStringLiteral("activities/ride.json"));
    QVERIFY(writeFile(sourcePath, QByteArray("payload")));

    AthleteBackupManifest manifest;
    qint64 totalSize = 0;
    QString error;
    QVERIFY(buildAthleteBackupManifest(
        athleteHome, globalHome, manifest, totalSize, error));
    QCOMPARE(manifest.size(), 1);

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QString archivePath =
        globalHome.filePath(QStringLiteral("payload.zip"));
    ZipWriter writer(archivePath);
    writer.setCompressionPolicy(ZipWriter::NeverCompress);
    writer.addFile(manifest.first().archivePath, &source);
    writer.close();
    QCOMPARE(writer.status(), ZipWriter::NoError);

    QFile archive(archivePath);
    QVERIFY(archive.open(QIODevice::ReadWrite));
    const qint64 payloadPosition =
        30 + manifest.first().archivePath.toLocal8Bit().size();
    QVERIFY(archive.seek(payloadPosition));
    char byte = 0;
    QCOMPARE(archive.read(&byte, 1), 1);
    byte ^= 0x5a;
    QVERIFY(archive.seek(payloadPosition));
    QCOMPARE(archive.write(&byte, 1), 1);
    QVERIFY(archive.flush());
    archive.close();

    QVERIFY(!verifyAthleteBackupArchive(
        archivePath, manifest, error));
    QVERIFY2(error.contains(QStringLiteral("match"), Qt::CaseInsensitive),
             qPrintable(error));
}

void TestAthleteBackupArchive::corruptArchiveIsNotPublished()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir root(temporary.path());
    const QString source = root.filePath(QStringLiteral("ride.json"));
    QVERIFY(writeFile(source, QByteArray("ride")));

    AthleteBackupManifest manifest = {
        {source, QStringLiteral("activities/ride.json"), 4, 0x12345678U}
    };
    const AthleteBackupArchiveBuildFunction corruptBuilder =
        [](const QString &path,
           const AthleteBackupManifest &,
           const AthleteBackupSourceFactory &,
           const AthleteBackupProgressFunction &,
           QString &) {
            return writeFile(path, QByteArray("not a zip archive"));
        };

    QString error;
    const QString target = root.filePath(QStringLiteral("corrupt.zip"));
    QVERIFY(!publishVerifiedAthleteBackup(
        target,
        manifest,
        error,
        athleteBackupFileSourceFactory(),
        {},
        corruptBuilder));
    QVERIFY2(error.contains(QStringLiteral("verify"), Qt::CaseInsensitive)
                 || error.contains(QStringLiteral("archive"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(target));
}

void TestAthleteBackupArchive::failedBuildDoesNotDamageExistingTarget()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir root(temporary.path());
    const QString source = root.filePath(QStringLiteral("ride.json"));
    const QString target = root.filePath(QStringLiteral("backup.zip"));
    QVERIFY(writeFile(source, QByteArray("ride")));
    QVERIFY(writeFile(target, QByteArray("existing backup")));

    AthleteBackupManifest manifest = {
        {source, QStringLiteral("activities/ride.json"), 4, 0U}
    };
    bool builderCalled = false;
    const AthleteBackupArchiveBuildFunction failingBuilder =
        [&builderCalled](const QString &path,
                         const AthleteBackupManifest &,
                         const AthleteBackupSourceFactory &,
                         const AthleteBackupProgressFunction &,
                         QString &error) {
            builderCalled = true;
            writeFile(path, QByteArray("partial"));
            error = QStringLiteral("forced write failure");
            return false;
        };

    QString error;
    QVERIFY(!publishVerifiedAthleteBackup(
        target,
        manifest,
        error,
        athleteBackupFileSourceFactory(),
        {},
        failingBuilder));
    QVERIFY(!builderCalled);
    QCOMPARE(readFile(target), QByteArray("existing backup"));
}

void TestAthleteBackupArchive::cancellationDoesNotPublish()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QDir root(temporary.path());
    const QString first = root.filePath(QStringLiteral("first.json"));
    const QString second = root.filePath(QStringLiteral("second.json"));
    QVERIFY(writeFile(first, QByteArray("first")));
    QVERIFY(writeFile(second, QByteArray("second")));

    AthleteBackupManifest manifest = {
        {first, QStringLiteral("activities/first.json"), 5, 0U},
        {second, QStringLiteral("activities/second.json"), 6, 0U}
    };
    const AthleteBackupProgressFunction cancelAfterFirst =
        [](int completed, int) { return completed < 1; };

    QString error;
    const QString target = root.filePath(QStringLiteral("canceled.zip"));
    QVERIFY(!publishVerifiedAthleteBackup(
        target,
        manifest,
        error,
        athleteBackupFileSourceFactory(),
        cancelAfterFirst));
    QVERIFY2(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(target));
}

QTEST_MAIN(TestAthleteBackupArchive)
#include "testAthleteBackupArchive.moc"
