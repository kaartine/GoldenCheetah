#include <QtTest>

#include "PlannedActivityFileStager.h"
#include "RideFile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <memory>

namespace {

bool failReads = false;
bool failWrites = false;

class TestRideFileReader final : public RideFileReader
{
public:
    RideFile *openRideFile(
        QFile &file, QStringList &errors,
        QList<RideFile*> *) const override
    {
        if (failReads || !file.open(QIODevice::ReadOnly)) {
            errors.append(QStringLiteral("injected read failure"));
            return nullptr;
        }
        const QJsonDocument document =
            QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            errors.append(QStringLiteral("invalid test activity"));
            return nullptr;
        }
        const QJsonObject object = document.object();
        const QDateTime start = QDateTime::fromString(
            object.value(QStringLiteral("start")).toString(),
            Qt::ISODateWithMs);
        if (!start.isValid()) {
            errors.append(QStringLiteral("invalid test start time"));
            return nullptr;
        }

        std::unique_ptr<RideFile> ride(new RideFile);
        ride->setStartTime(start);
        const QJsonObject tags =
            object.value(QStringLiteral("tags")).toObject();
        for (auto tag = tags.constBegin();
             tag != tags.constEnd(); ++tag) {
            ride->setTag(tag.key(), tag.value().toString());
        }
        return ride.release();
    }

    bool hasWrite() const override
    {
        return true;
    }

    bool writeRideFile(
        Context *, const RideFile *ride,
        QFile &file) const override
    {
        if (failWrites || !file.open(
                QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        QJsonObject tags;
        for (auto tag = ride->tags().cbegin();
             tag != ride->tags().cend(); ++tag) {
            tags.insert(tag.key(), tag.value());
        }
        QJsonObject object;
        object.insert(
            QStringLiteral("start"),
            ride->startTime().toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("tags"), tags);
        const QByteArray bytes =
            QJsonDocument(object).toJson(QJsonDocument::Compact);
        return file.write(bytes) == bytes.size() && file.flush();
    }
};

QString writeActivity(
    const QDir &directory,
    const QString &fileName,
    const QDateTime &start,
    const QMap<QString, QString> &tags)
{
    QJsonObject encodedTags;
    for (auto tag = tags.cbegin(); tag != tags.cend(); ++tag) {
        encodedTags.insert(tag.key(), tag.value());
    }
    QJsonObject object;
    object.insert(
        QStringLiteral("start"),
        start.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("tags"), encodedTags);
    const QString path = directory.filePath(fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush())
        return {};
    return path;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

std::unique_ptr<RideFile> readActivity(
    TestRideFileReader &reader, const QString &path)
{
    QFile file(path);
    QStringList errors;
    return std::unique_ptr<RideFile>(
        reader.openRideFile(file, errors, nullptr));
}

PlannedActivityFile::FileAccess fileAccess(
    TestRideFileReader &reader)
{
    return {
        [&reader](Context *, QFile &source,
                  QStringList &errors) {
            return reader.openRideFile(
                source, errors, nullptr);
        },
        [&reader](Context *context, const RideFile *ride,
                  QFile &target, const QString &) {
            return reader.writeRideFile(
                context, ride, target);
        }
    };
}

} // namespace

class TestPlannedActivityFileStager : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void targetPreservesSourceTime();
    void targetUsesExplicitTime();
    void invalidTargetInputIsRejected_data();
    void invalidTargetInputIsRejected();
    void stagesMutatedCopyWithoutChangingSource();
    void transformFailureLeavesNoStagedFile();
    void readFailureLeavesNoStagedFile();
    void writeFailureLeavesNoStagedFile();
    void invalidFileAccessIsRejected();
    void invalidPathIsRejected_data();
    void invalidPathIsRejected();
    void successfulWriterMustCreateAFile();

private:
    TestRideFileReader reader;
};

void TestPlannedActivityFileStager::init()
{
    failReads = false;
    failWrites = false;
}

void TestPlannedActivityFileStager::targetPreservesSourceTime()
{
    PlannedActivityFile::CopyTarget target;
    QString error;

    QVERIFY2(PlannedActivityFile::resolveCopyTarget(
        QStringLiteral("2026_01_02_06_07_08.gctp"),
        QDateTime(QDate(2026, 1, 2), QTime(6, 7, 8)),
        QDate(2026, 2, 3), QTime(), target, error),
        qPrintable(error));
    QCOMPARE(
        target.fileName,
        QStringLiteral("2026_02_03_06_07_08.gctp"));
    QCOMPARE(
        target.dateTime,
        QDateTime(QDate(2026, 2, 3), QTime(6, 7, 8)));
}

void TestPlannedActivityFileStager::targetUsesExplicitTime()
{
    PlannedActivityFile::CopyTarget target;
    QString error;

    QVERIFY2(PlannedActivityFile::resolveCopyTarget(
        QStringLiteral("2026_01_02_06_07_08.gctp"),
        QDateTime(QDate(2026, 1, 2), QTime(6, 7, 8)),
        QDate(2026, 2, 3), QTime(9, 10, 11), target, error),
        qPrintable(error));
    QCOMPARE(
        target.fileName,
        QStringLiteral("2026_02_03_09_10_11.gctp"));
    QCOMPARE(
        target.dateTime,
        QDateTime(QDate(2026, 2, 3), QTime(9, 10, 11)));
}

void TestPlannedActivityFileStager::invalidTargetInputIsRejected_data()
{
    QTest::addColumn<QString>("sourceFileName");
    QTest::addColumn<QDateTime>("sourceDateTime");
    QTest::addColumn<QDate>("targetDate");

    QTest::newRow("path-instead-of-basename")
        << QStringLiteral("sub/activity.gctp")
        << QDateTime(QDate(2026, 1, 2), QTime(6, 7, 8))
        << QDate(2026, 2, 3);
    QTest::newRow("missing-extension")
        << QStringLiteral("activity")
        << QDateTime(QDate(2026, 1, 2), QTime(6, 7, 8))
        << QDate(2026, 2, 3);
    QTest::newRow("invalid-source-time")
        << QStringLiteral("activity.gctp")
        << QDateTime()
        << QDate(2026, 2, 3);
    QTest::newRow("invalid-target-date")
        << QStringLiteral("activity.gctp")
        << QDateTime(QDate(2026, 1, 2), QTime(6, 7, 8))
        << QDate();
}

void TestPlannedActivityFileStager::invalidTargetInputIsRejected()
{
    QFETCH(QString, sourceFileName);
    QFETCH(QDateTime, sourceDateTime);
    QFETCH(QDate, targetDate);
    PlannedActivityFile::CopyTarget target;
    QString error;

    QVERIFY(!PlannedActivityFile::resolveCopyTarget(
        sourceFileName, sourceDateTime, targetDate,
        QTime(), target, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(target.fileName.isEmpty());
    QVERIFY(!target.dateTime.isValid());
}

void TestPlannedActivityFileStager::
stagesMutatedCopyWithoutChangingSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime sourceStart(
        QDate(2026, 1, 2), QTime(6, 7, 8));
    const QDateTime targetStart(
        QDate(2026, 2, 3), QTime(9, 10, 11));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, sourceStart,
        {{QStringLiteral("Linked Filename"),
          QStringLiteral("linked.json")},
         {QStringLiteral("Keep"), QStringLiteral("source")}});
    QVERIFY(!sourcePath.isEmpty());
    const QByteArray original = readBytes(sourcePath);
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    int transformCalls = 0;
    QString error;

    QVERIFY2(PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, targetStart,
        stagingPath, fileAccess(reader),
        [&transformCalls](RideFile *ride, const QDateTime &when,
                          QString &detail) {
            ++transformCalls;
            if (ride->startTime() != when) {
                detail = QStringLiteral(
                    "transform received the wrong target time");
                return false;
            }
            ride->setTag(QStringLiteral("Transformed"),
                         QStringLiteral("yes"));
            return true;
        },
        error), qPrintable(error));

    QCOMPARE(transformCalls, 1);
    QCOMPARE(readBytes(sourcePath), original);
    std::unique_ptr<RideFile> staged =
        readActivity(reader, stagingPath);
    QVERIFY(staged);
    QCOMPARE(staged->startTime(), targetStart);
    QCOMPARE(staged->getTag("Year", ""), QStringLiteral("2026"));
    QCOMPARE(staged->getTag("Month", ""),
             targetStart.toString(QStringLiteral("MMMM")));
    QCOMPARE(staged->getTag("Weekday", ""),
             targetStart.toString(QStringLiteral("ddd")));
    QCOMPARE(staged->getTag("Original Date", ""),
             QStringLiteral("2026/02/03"));
    QCOMPARE(staged->getTag("Linked Filename", ""), QString());
    QCOMPARE(staged->getTag("Keep", ""), QStringLiteral("source"));
    QCOMPARE(staged->getTag("Transformed", ""),
             QStringLiteral("yes"));
}

void TestPlannedActivityFileStager::
transformFailureLeavesNoStagedFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    QString error;

    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, fileAccess(reader),
        [](RideFile *, const QDateTime &, QString &detail) {
            detail = QStringLiteral("injected transform failure");
            return false;
        },
        error));
    QCOMPARE(error, QStringLiteral("injected transform failure"));
    QVERIFY(!QFileInfo::exists(stagingPath));
}

void TestPlannedActivityFileStager::readFailureLeavesNoStagedFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    failReads = true;
    QString error;

    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, fileAccess(reader), {}, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(stagingPath));
}

void TestPlannedActivityFileStager::writeFailureLeavesNoStagedFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    failWrites = true;
    QString error;

    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, fileAccess(reader), {}, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(stagingPath));
}


void TestPlannedActivityFileStager::invalidFileAccessIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    QString error;

    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, {}, {}, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(stagingPath));
}


void TestPlannedActivityFileStager::invalidPathIsRejected_data()
{
    QTest::addColumn<QString>("condition");

    QTest::newRow("missing-source")
        << QStringLiteral("missing-source");
    QTest::newRow("relative-source")
        << QStringLiteral("relative-source");
    QTest::newRow("mismatched-basename")
        << QStringLiteral("mismatched-basename");
    QTest::newRow("symlink-source")
        << QStringLiteral("symlink-source");
    QTest::newRow("relative-staging")
        << QStringLiteral("relative-staging");
    QTest::newRow("existing-staging")
        << QStringLiteral("existing-staging");
    QTest::newRow("symlink-staging")
        << QStringLiteral("symlink-staging");
    QTest::newRow("missing-staging-parent")
        << QStringLiteral("missing-staging-parent");
}


void TestPlannedActivityFileStager::invalidPathIsRejected()
{
    QFETCH(QString, condition);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    QVERIFY(!sourcePath.isEmpty());
    QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));

    if (condition == QStringLiteral("missing-source")) {
        sourcePath = QDir(temporary.path()).filePath(
            QStringLiteral("missing.gctp"));
        sourceName = QStringLiteral("missing.gctp");
    } else if (condition == QStringLiteral("relative-source")) {
        sourcePath = sourceName;
    } else if (condition == QStringLiteral("mismatched-basename")) {
        sourceName = QStringLiteral("different.gctp");
    } else if (condition == QStringLiteral("symlink-source")) {
        const QString link = QDir(temporary.path()).filePath(
            QStringLiteral("linked.gctp"));
        if (!QFile::link(sourcePath, link)
            || !QFileInfo(link).isSymLink()) {
            QSKIP("Symbolic links are unavailable on this platform");
        }
        sourcePath = link;
        sourceName = QStringLiteral("linked.gctp");
    } else if (condition == QStringLiteral("relative-staging")) {
        stagingPath = QStringLiteral("staged");
    } else if (condition == QStringLiteral("existing-staging")) {
        QFile existing(stagingPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QVERIFY(existing.write("existing") == 8);
        existing.close();
    } else if (condition == QStringLiteral("symlink-staging")) {
        if (!QFile::link(sourcePath, stagingPath)
            || !QFileInfo(stagingPath).isSymLink()) {
            QSKIP("Symbolic links are unavailable on this platform");
        }
    } else if (condition
               == QStringLiteral("missing-staging-parent")) {
        stagingPath = QDir(temporary.path()).filePath(
            QStringLiteral("missing/staged"));
    }

    QString error;
    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, fileAccess(reader), {}, error));
    QVERIFY(!error.isEmpty());
}


void TestPlannedActivityFileStager::successfulWriterMustCreateAFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime start(QDate(2026, 1, 2), QTime(6, 7, 8));
    const QString sourceName =
        QStringLiteral("2026_01_02_06_07_08.gctp");
    const QString sourcePath = writeActivity(
        QDir(temporary.path()), sourceName, start, {});
    const QString stagingPath =
        QDir(temporary.path()).filePath(QStringLiteral("staged"));
    PlannedActivityFile::FileAccess access = fileAccess(reader);
    access.write = [](Context *, const RideFile *,
                      QFile &, const QString &) {
        return true;
    };
    QString error;

    QVERIFY(!PlannedActivityFile::stageCopyWithAccess(
        nullptr, sourcePath, sourceName, start,
        stagingPath, access, {}, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(stagingPath));
}


QTEST_GUILESS_MAIN(TestPlannedActivityFileStager)
#include "testPlannedActivityFileStager.moc"
