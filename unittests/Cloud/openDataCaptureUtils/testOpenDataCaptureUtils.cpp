#include "Cloud/OpenDataCaptureUtils.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <cstring>

namespace {

class ChunkedReadDevice : public QBuffer
{
public:
    explicit ChunkedReadDevice(const QByteArray &data)
    {
        setData(data);
        open(QIODevice::ReadOnly);
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        return QBuffer::readData(data, qMin<qint64>(maximumSize, 3));
    }
};

class ShortWriteDevice : public QIODevice
{
public:
    ShortWriteDevice()
    {
        open(QIODevice::WriteOnly);
    }

    const QByteArray &data() const
    {
        return data_;
    }

protected:
    qint64 readData(char *, qint64) override
    {
        return -1;
    }

    qint64 writeData(const char *data, qint64 maximumSize) override
    {
        const qint64 count = qMin<qint64>(maximumSize, 2);
        data_.append(data, static_cast<qsizetype>(count));
        return count;
    }

private:
    QByteArray data_;
};

class FailingReadDevice : public QIODevice
{
public:
    FailingReadDevice()
    {
        open(QIODevice::ReadOnly);
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        if (firstRead_) {
            firstRead_ = false;
            const QByteArray prefix("abc");
            const qint64 count =
                qMin<qint64>(maximumSize, prefix.size());
            std::memcpy(
                data, prefix.constData(),
                static_cast<size_t>(count));
            return count;
        }
        return -1;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    bool firstRead_ = true;
};

class ZeroWriteDevice : public QIODevice
{
public:
    ZeroWriteDevice()
    {
        open(QIODevice::WriteOnly);
    }

protected:
    qint64 readData(char *, qint64) override
    {
        return -1;
    }

    qint64 writeData(const char *, qint64) override
    {
        return 0;
    }
};

} // namespace

class TestOpenDataCaptureUtils : public QObject
{
    Q_OBJECT

private slots:
    void jsonLiteralEscapesAllControlCharacters();
    void sourcePathStaysInsideActivityDirectory();
    void sourceOpenRejectsPostValidationSymlinkSwap();
    void sourceIdentityRejectsReplacement();
    void sourceIdentityRejectsSameMetadataMutation();
    void snapshotTemplatePreservesReaderSuffix();
    void snapshotSelectionMatchesSummaryAndSources();
    void snapshotSettlesRefreshBeforeManifestAndSummary();
    void snapshotStopsWhenRefreshSettlementFails();
    void snapshotStopsWhenManifestFails();
    void snapshotCopyHandlesShortReadsAndWrites();
    void snapshotCopyRejectsUnexpectedContent();
    void snapshotCopyRejectsReadAndWriteFailures();
    void archiveNameHandlesCompressedActivities();
    void csvWriterPreservesAvailableAndMissingSeries();
};

void
TestOpenDataCaptureUtils::jsonLiteralEscapesAllControlCharacters()
{
    QString value =
        QStringLiteral("quote\" slash\\ line\n tab\t unicode \u20ac");
    value.insert(0, QChar(0x0001));
    value.insert(0, QChar::Null);

    const QByteArray document =
        QByteArrayLiteral("{\"value\":")
        + OpenDataCaptureUtils::jsonStringLiteral(value).toUtf8()
        + QByteArrayLiteral("}");
    QJsonParseError error;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(document, &error);

    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(parsed.isObject());
    QCOMPARE(
        parsed.object().value(QStringLiteral("value")).toString(),
        value);
}

void
TestOpenDataCaptureUtils::sourcePathStaysInsideActivityDirectory()
{
    QTemporaryDir directory;
    QTemporaryDir outside;
    QVERIFY(directory.isValid());
    QVERIFY(outside.isValid());
    const QString fileName =
        QStringLiteral("2026_07_28_12_00_00.json");
    const QString canonicalDirectory =
        QDir(directory.path()).canonicalPath();
    QVERIFY(!canonicalDirectory.isEmpty());
    const QString expected = QDir(canonicalDirectory).filePath(fileName);
    QFile activity(expected);
    QVERIFY(activity.open(QIODevice::WriteOnly));
    QVERIFY(activity.write("{}") == 2);
    activity.close();

    QCOMPARE(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            fileName),
        expected);
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            QStringLiteral("relative"),
            QStringLiteral("activity.json")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            QStringLiteral("../private.json")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            QStringLiteral("folder\\private.json")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            expected).isEmpty());
    const QString sibling =
        QDir(outside.path()).filePath(fileName);
    QFile siblingFile(sibling);
    QVERIFY(siblingFile.open(QIODevice::WriteOnly));
    QVERIFY(siblingFile.write("{}") == 2);
    siblingFile.close();
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            outside.path(),
            fileName).isEmpty());

    const QString linkName =
        QDir(directory.path()).filePath(
            QStringLiteral("linked.json"));
    if (!QFile::link(sibling, linkName))
        QSKIP("Symbolic links are unavailable on this platform");
    QVERIFY(
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            QStringLiteral("linked.json")).isEmpty());
}

void
TestOpenDataCaptureUtils::sourceOpenRejectsPostValidationSymlinkSwap()
{
    QTemporaryDir directory;
    QTemporaryDir outside;
    QVERIFY(directory.isValid());
    QVERIFY(outside.isValid());
    const QString fileName = QStringLiteral("activity.json");
    const QString sourcePath =
        QDir(directory.path()).filePath(fileName);
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("inside"), qint64(6));
    source.close();
    const QString validatedPath =
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            fileName);
    QVERIFY(!validatedPath.isEmpty());
    QString error;
    std::unique_ptr<QFile> opened =
        OpenDataCaptureUtils::openActivitySource(
            directory.path(),
            validatedPath,
            error);
    QVERIFY2(opened, qPrintable(error));
    QCOMPARE(opened->readAll(), QByteArrayLiteral("inside"));
    opened.reset();

    const QString outsidePath =
        QDir(outside.path()).filePath(
            QStringLiteral("private.json"));
    QFile privateFile(outsidePath);
    QVERIFY(privateFile.open(QIODevice::WriteOnly));
    QCOMPARE(privateFile.write("private"), qint64(7));
    privateFile.close();
    QVERIFY(QFile::remove(sourcePath));
    if (!QFile::link(outsidePath, sourcePath))
        QSKIP("Symbolic links are unavailable on this platform");

    opened =
        OpenDataCaptureUtils::openActivitySource(
            directory.path(),
            validatedPath,
            error);

    QVERIFY(!opened);
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataCaptureUtils::sourceIdentityRejectsReplacement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = QStringLiteral("activity.json");
    const QString sourcePath =
        QDir(directory.path()).filePath(fileName);
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("first"), qint64(5));
    source.close();

    const QString validatedPath =
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            fileName);
    QVERIFY(!validatedPath.isEmpty());
    QString error;
    OpenDataCaptureUtils::ActivityFileIdentity identity;
    std::unique_ptr<QFile> opened =
        OpenDataCaptureUtils::openActivitySource(
            directory.path(),
            validatedPath,
            nullptr,
            &identity,
            error);
    QVERIFY2(opened, qPrintable(error));
    QVERIFY(identity.valid);
    opened.reset();

    QVERIFY(QFile::remove(sourcePath));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("replacement"), qint64(11));
    source.close();

    opened =
        OpenDataCaptureUtils::openActivitySource(
            directory.path(),
            validatedPath,
            &identity,
            nullptr,
            error);
    QVERIFY(!opened);
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataCaptureUtils::sourceIdentityRejectsSameMetadataMutation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = QStringLiteral("activity.json");
    const QString sourcePath =
        QDir(directory.path()).filePath(fileName);
    const QDateTime fixedTime =
        QDateTime::fromSecsSinceEpoch(
            1700000000, QTimeZone::UTC);
    QFile source(sourcePath);
    QVERIFY(source.open(
        QIODevice::ReadWrite | QIODevice::Truncate));
    QCOMPARE(source.write("AAAA"), qint64(4));
    QVERIFY(source.setFileTime(
        fixedTime, QFileDevice::FileModificationTime));
    source.close();

    const QString validatedPath =
        OpenDataCaptureUtils::activitySourcePath(
            directory.path(),
            directory.path(),
            fileName);
    OpenDataCaptureUtils::ActivityFileIdentity identity;
    QString error;
    std::unique_ptr<QFile> opened =
        OpenDataCaptureUtils::openActivitySource(
            directory.path(),
            validatedPath,
            nullptr,
            &identity,
            error);
    QVERIFY2(opened, qPrintable(error));
    opened.reset();

    QVERIFY(source.open(
        QIODevice::ReadWrite | QIODevice::Truncate));
    QCOMPARE(source.write("BBBB"), qint64(4));
    QVERIFY(source.setFileTime(
        fixedTime, QFileDevice::FileModificationTime));
    source.close();

    opened = OpenDataCaptureUtils::openActivitySource(
        directory.path(),
        validatedPath,
        &identity,
        nullptr,
        error);
    QVERIFY(!opened);
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataCaptureUtils::snapshotTemplatePreservesReaderSuffix()
{
    QCOMPARE(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("activity.json")),
        QStringLiteral("source-XXXXXX.json"));
    QCOMPARE(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("activity.gc")),
        QStringLiteral("source-XXXXXX.gc"));
    QCOMPARE(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("activity.fit.zip")),
        QStringLiteral("source-XXXXXX.fit"));
    QCOMPARE(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("activity.json.gz")),
        QStringLiteral("source-XXXXXX.json"));
    QVERIFY(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("activity")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activitySnapshotTemplate(
            QStringLiteral("../activity.json")).isEmpty());
}

void
TestOpenDataCaptureUtils::snapshotSelectionMatchesSummaryAndSources()
{
    QVERIFY(
        OpenDataCaptureUtils::includeActivityInSnapshot(
            true, false));
    QVERIFY(
        !OpenDataCaptureUtils::includeActivityInSnapshot(
            false, false));
    QVERIFY(
        !OpenDataCaptureUtils::includeActivityInSnapshot(
            true, true));
    QVERIFY(
        !OpenDataCaptureUtils::includeActivityInSnapshot(
            false, true));
}

void
TestOpenDataCaptureUtils::
snapshotSettlesRefreshBeforeManifestAndSummary()
{
    QStringList calls;
    QString error;
    OpenDataCaptureUtils::SnapshotCaptureOperations operations;
    operations.settleRefresh = [&calls](QString &) {
        calls.append(QStringLiteral("settle"));
        return true;
    };
    operations.captureManifest = [&calls](QString &) {
        calls.append(QStringLiteral("manifest"));
        return true;
    };
    operations.writeSummary = [&calls](QString &) {
        calls.append(QStringLiteral("summary"));
        return true;
    };

    QVERIFY2(
        OpenDataCaptureUtils::captureManifestThenSummary(
            operations, error),
        qPrintable(error));
    QCOMPARE(
        calls,
        QStringList({
            QStringLiteral("settle"),
            QStringLiteral("manifest"),
            QStringLiteral("summary")
        }));
}

void
TestOpenDataCaptureUtils::snapshotStopsWhenRefreshSettlementFails()
{
    int manifestCalls = 0;
    int summaryCalls = 0;
    QString error;
    OpenDataCaptureUtils::SnapshotCaptureOperations operations;
    operations.settleRefresh = [](QString &operationError) {
        operationError = QStringLiteral("refresh failed");
        return false;
    };
    operations.captureManifest = [&manifestCalls](QString &) {
        ++manifestCalls;
        return true;
    };
    operations.writeSummary = [&summaryCalls](QString &) {
        ++summaryCalls;
        return true;
    };

    QVERIFY(
        !OpenDataCaptureUtils::captureManifestThenSummary(
            operations, error));
    QCOMPARE(manifestCalls, 0);
    QCOMPARE(summaryCalls, 0);
    QCOMPARE(error, QStringLiteral("refresh failed"));
}

void
TestOpenDataCaptureUtils::snapshotStopsWhenManifestFails()
{
    int summaryCalls = 0;
    QString error;
    OpenDataCaptureUtils::SnapshotCaptureOperations operations;
    operations.settleRefresh = [](QString &) {
        return true;
    };
    operations.captureManifest = [](QString &operationError) {
        operationError = QStringLiteral("manifest failed");
        return false;
    };
    operations.writeSummary = [&summaryCalls](QString &) {
        ++summaryCalls;
        return true;
    };

    QVERIFY(
        !OpenDataCaptureUtils::captureManifestThenSummary(
            operations, error));
    QCOMPARE(summaryCalls, 0);
    QCOMPARE(error, QStringLiteral("manifest failed"));
}

void
TestOpenDataCaptureUtils::snapshotCopyHandlesShortReadsAndWrites()
{
    const QByteArray expected("0123456789");
    ChunkedReadDevice source(expected);
    ShortWriteDevice destination;
    QString error;

    QVERIFY2(
        OpenDataCaptureUtils::copyActivitySnapshot(
            &source, &destination, error),
        qPrintable(error));
    QCOMPARE(destination.data(), expected);

    QByteArray empty;
    QBuffer emptySource(&empty);
    QBuffer emptyDestination;
    QVERIFY(emptySource.open(QIODevice::ReadOnly));
    QVERIFY(emptyDestination.open(QIODevice::WriteOnly));
    QVERIFY2(
        OpenDataCaptureUtils::copyActivitySnapshot(
            &emptySource, &emptyDestination, error),
        qPrintable(error));
    QVERIFY(emptyDestination.data().isEmpty());
}

void
TestOpenDataCaptureUtils::snapshotCopyRejectsUnexpectedContent()
{
    QByteArray changed("BBBB");
    QBuffer source(&changed);
    QBuffer destination;
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(destination.open(QIODevice::WriteOnly));
    const QByteArray expected =
        QCryptographicHash::hash(
            QByteArrayLiteral("AAAA"),
            QCryptographicHash::Sha256);
    QString error;

    QVERIFY(
        !OpenDataCaptureUtils::copyActivitySnapshot(
            &source,
            &destination,
            expected,
            error));
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataCaptureUtils::snapshotCopyRejectsReadAndWriteFailures()
{
    FailingReadDevice failingSource;
    QByteArray output;
    QBuffer destination(&output);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QString error;
    QVERIFY(
        !OpenDataCaptureUtils::copyActivitySnapshot(
            &failingSource, &destination, error));
    QVERIFY(!error.isEmpty());

    QByteArray input("data");
    QBuffer source(&input);
    QVERIFY(source.open(QIODevice::ReadOnly));
    ZeroWriteDevice failingDestination;
    QVERIFY(
        !OpenDataCaptureUtils::copyActivitySnapshot(
            &source, &failingDestination, error));
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataCaptureUtils::archiveNameHandlesCompressedActivities()
{
    QCOMPARE(
        OpenDataCaptureUtils::activityArchiveName(
            QStringLiteral("2026_07_28_12_00_00.json")),
        QStringLiteral("2026_07_28_12_00_00.csv"));
    QCOMPARE(
        OpenDataCaptureUtils::activityArchiveName(
            QStringLiteral("2026_07_28_12_00_00.json.gz")),
        QStringLiteral("2026_07_28_12_00_00.csv"));
    QCOMPARE(
        OpenDataCaptureUtils::activityArchiveName(
            QStringLiteral("2026_07_28_12_00_00.fit.zip")),
        QStringLiteral("2026_07_28_12_00_00.csv"));
    QVERIFY(
        OpenDataCaptureUtils::activityArchiveName(
            QStringLiteral("../activity.json")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activityArchiveName(
            QStringLiteral("folder\\activity.json")).isEmpty());
    QVERIFY(
        OpenDataCaptureUtils::activityArchiveName(
        QStringLiteral(".json")).isEmpty());
}

void
TestOpenDataCaptureUtils::csvWriterPreservesAvailableAndMissingSeries()
{
    QByteArray data;
    QBuffer destination(&data);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QString error;
    OpenDataCaptureUtils::ActivitySeries series;
    series.power = true;
    series.cadence = true;
    OpenDataCaptureUtils::ActivitySample sample;
    sample.secs = 1.5;
    sample.km = 0.25;
    sample.power = 200;
    sample.heartRate = 130;
    sample.cadence = 88;
    sample.altitude = 42;

    QVERIFY2(
        OpenDataCaptureUtils::writeCsvHeader(
            &destination, error),
        qPrintable(error));
    QVERIFY2(
        OpenDataCaptureUtils::writeCsvSample(
            &destination, series, sample, error),
        qPrintable(error));

    QCOMPARE(
        data,
        QByteArrayLiteral(
            "secs,km,power,hr,cad,alt\n"
            "1.5,0.25,200,,88,\n"));
    QVERIFY(!OpenDataCaptureUtils::writeCsvHeader(nullptr, error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestOpenDataCaptureUtils)
#include "testOpenDataCaptureUtils.moc"
