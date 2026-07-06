#include <QtTest>

#include "SplitActivitySave.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace {

QString sourceFileName()
{
    return QStringLiteral("2026_07_06_08_00_00.json");
}

QString firstOutputName()
{
    return QStringLiteral("2026_07_06_08_30_00.json");
}

QString secondOutputName()
{
    return QStringLiteral("2026_07_06_09_00_00.json");
}

QString thirdOutputName()
{
    return QStringLiteral("2026_07_06_09_30_00.json");
}

QByteArray sourceContents()
{
    return QByteArray("source-version-one");
}

bool filePathPresent(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool writeBytes(const QString &path, const QByteArray &contents, QString &error)
{
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString();
        return false;
    }
    if (file.write(contents) != static_cast<qint64>(contents.size())) {
        error = file.errorString().isEmpty()
            ? QStringLiteral("short write")
            : file.errorString();
        return false;
    }
    if (!file.flush()) {
        error = file.errorString();
        return false;
    }
    return true;
}

void writeFixture(const QString &path, const QByteArray &contents)
{
    QString error;
    QVERIFY2(writeBytes(path, contents, error), qPrintable(error));
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

struct Fixture
{
    Fixture()
        : activities(temporary.filePath(QStringLiteral("activities"))),
          sourcePath(activities.filePath(sourceFileName())),
          backupPath(temporary.filePath(
              QStringLiteral("backup/original-activity.json.bak")))
    {
    }

    bool initialize()
    {
        return temporary.isValid()
            && QDir().mkpath(activities.absolutePath())
            && QDir().mkpath(QFileInfo(backupPath).absolutePath());
    }

    QTemporaryDir temporary;
    QDir activities;
    QString sourcePath;
    QString backupPath;
};

SplitActivityOutput outputWriting(
    const QString &fileName,
    const QByteArray &contents,
    QStringList *stagingPaths = nullptr,
    int *stageCalls = nullptr)
{
    return SplitActivityOutput{
        fileName,
        [contents, stagingPaths, stageCalls](
            const QString &stagingPath, QString &error) {
            if (stageCalls) {
                ++*stageCalls;
            }
            if (stagingPaths) {
                stagingPaths->append(stagingPath);
            }
            return writeBytes(stagingPath, contents, error);
        }
    };
}

AtomicPublishFunction publishingThroughDefault(int &publishCalls)
{
    return [&publishCalls](
               const QString &stagingPath,
               const QString &targetPath,
               bool &targetPublished,
               QString &error) {
        ++publishCalls;
        return publishAtomicNew(
            stagingPath, targetPath, targetPublished, error);
    };
}

void verifyNoSplitStageArtifacts(
    const QDir &activities, const QStringList &observedStagingPaths)
{
    for (const QString &path : observedStagingPaths) {
        QVERIFY2(
            !filePathPresent(path),
            qPrintable(QStringLiteral("staging file remains: %1").arg(path)));
    }

    const QFileInfoList entries = activities.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        QVERIFY2(
            !entry.fileName().contains(
                QStringLiteral(".split-stage"), Qt::CaseInsensitive),
            qPrintable(QStringLiteral("split staging artifact remains: %1")
                           .arg(entry.absoluteFilePath())));
    }
}

void verifyTargetsAbsent(const QDir &activities, const QStringList &fileNames)
{
    for (const QString &fileName : fileNames) {
        const QString path = activities.filePath(fileName);
        QVERIFY2(
            !filePathPresent(path),
            qPrintable(QStringLiteral("target remains: %1").arg(path)));
    }
}

void verifyNoArtifactsContaining(
    const QDir &directory, const QString &fragment)
{
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        QVERIFY2(
            !entry.fileName().contains(fragment, Qt::CaseInsensitive),
            qPrintable(QStringLiteral("temporary artifact remains: %1")
                           .arg(entry.absoluteFilePath())));
    }
}

} // namespace

class TestSplitActivitySave : public QObject
{
    Q_OBJECT

private slots:
    void emptyOutputsAreRejected();
    void unsafeOutputNamesAreRejected_data();
    void unsafeOutputNamesAreRejected();
    void duplicateOutputNamesAreRejected_data();
    void duplicateOutputNamesAreRejected();
    void targetsMatchingSourceOrBackupAreRejected_data();
    void targetsMatchingSourceOrBackupAreRejected();
    void existingTargetIsPreserved();
    void targetSymlinkIsPreserved();
    void stagingFailureCleansAllStagesWithoutPublishing();
    void middlePublishFailureRollsBackEveryTarget();
    void archiveFailureRunsAfterPublishAndRollsBackTargets();
    void keepOriginalSuccessPublishesAllWithoutArchiving();
    void removeOriginalSuccessSafelyReplacesBackup();
    void sourceChangeDuringStagingRejectsFinalization();
    void targetAppearingAfterPreflightIsPreserved();
    void publishedFileNamesAreSetOnlyOnSuccess();
    void atomicMoveDoesNotCopyAcrossFilesystems();
    void archiveSyncFailureRestoresSourceAndPreviousBackup_data();
    void archiveSyncFailureRestoresSourceAndPreviousBackup();
    void failedArchiveRollbackKeepsCommittedSplitFiles();
    void archiveMoveFailureRestoresPreviousBackup();
    void archiveSyncsDistinctCaseSensitiveDirectories();
    void sourceAliasIsCanonicalizedBeforeArchive();
};

void TestSplitActivitySave::emptyOutputsAreRejected()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int publishCalls = 0;
    int archiveCalls = 0;
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        QList<SplitActivityOutput>(),
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!filePathPresent(fixture.backupPath));
    verifyNoSplitStageArtifacts(fixture.activities, QStringList());
}

void TestSplitActivitySave::unsafeOutputNamesAreRejected_data()
{
    QTest::addColumn<QString>("fileName");

    QTest::newRow("empty") << QString();
    QTest::newRow("parent-traversal")
        << QStringLiteral("../x.json");
    QTest::newRow("backslash-parent-traversal")
        << QStringLiteral("..\\x.json");
    QTest::newRow("subdirectory")
        << QStringLiteral("sub/x.json");
    QTest::newRow("absolute")
        << QStringLiteral("<absolute>");
    QTest::newRow("wrong-suffix")
        << QStringLiteral("2026_07_06_08_30_00.fit");
    QTest::newRow("uppercase-json-suffix")
        << QStringLiteral("2026_07_06_08_30_00.JSON");
    QTest::newRow("not-a-timestamp")
        << QStringLiteral("activity.json");
}

void TestSplitActivitySave::unsafeOutputNamesAreRejected()
{
    QFETCH(QString, fileName);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    if (fileName == QStringLiteral("<absolute>")) {
        fileName = fixture.temporary.filePath(firstOutputName());
    }

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            fileName,
            QByteArray("unsafe output"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!filePathPresent(fixture.backupPath));
    if (!fileName.isEmpty()) {
        const QString candidate = QFileInfo(fileName).isAbsolute()
            ? fileName
            : fixture.activities.filePath(fileName);
        QVERIFY(!filePathPresent(candidate));
    }
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::duplicateOutputNamesAreRejected_data()
{
    QTest::addColumn<QString>("secondName");

    QTest::newRow("identical") << firstOutputName();
    QTest::newRow("case-variant")
        << QStringLiteral("2026_07_06_08_30_00.JSON");
}

void TestSplitActivitySave::duplicateOutputNamesAreRejected()
{
    QFETCH(QString, secondName);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("first duplicate"),
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondName,
            QByteArray("second duplicate"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    verifyTargetsAbsent(
        fixture.activities,
        QStringList{ firstOutputName(), secondName });
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::targetsMatchingSourceOrBackupAreRejected_data()
{
    QTest::addColumn<QString>("matchingPath");

    QTest::newRow("source") << QStringLiteral("source");
    QTest::newRow("backup") << QStringLiteral("backup");
}

void TestSplitActivitySave::targetsMatchingSourceOrBackupAreRejected()
{
    QFETCH(QString, matchingPath);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    QString outputName;
    if (matchingPath == QStringLiteral("source")) {
        outputName = sourceFileName();
    } else {
        outputName = firstOutputName();
        fixture.backupPath = fixture.activities.filePath(outputName);
    }

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            outputName,
            QByteArray("path collision"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    if (matchingPath == QStringLiteral("backup")) {
        QVERIFY(!filePathPresent(fixture.backupPath));
    }
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::existingTargetIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QString targetPath =
        fixture.activities.filePath(firstOutputName());
    const QByteArray existingContents("existing target");
    writeFixture(targetPath, existingContents);

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("replacement target"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        true,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(targetPath), existingContents);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::targetSymlinkIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QString backingPath =
        fixture.temporary.filePath(QStringLiteral("symlink-backing.json"));
    const QByteArray backingContents("symlink backing");
    writeFixture(backingPath, backingContents);
    const QString targetPath =
        fixture.activities.filePath(firstOutputName());
    if (!QFile::link(backingPath, targetPath)
        || !QFileInfo(targetPath).isSymLink()) {
        QSKIP("Symbolic links are unavailable on this platform");
    }

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("replacement target"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        true,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 0);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QVERIFY(QFileInfo(targetPath).isSymLink());
    QCOMPARE(readBytes(targetPath), backingContents);
    QCOMPARE(readBytes(backingPath), backingContents);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::stagingFailureCleansAllStagesWithoutPublishing()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    int publishCalls = 0;
    int archiveCalls = 0;
    QStringList stagingPaths;

    SplitActivityOutput failingOutput;
    failingOutput.fileName = secondOutputName();
    failingOutput.stage =
        [&stageCalls, &stagingPaths](
            const QString &stagingPath, QString &error) {
            ++stageCalls;
            stagingPaths.append(stagingPath);
            if (!writeBytes(
                    stagingPath, QByteArray("partial staged output"), error)) {
                return false;
            }
            error = QStringLiteral("injected staging failure");
            return false;
        };

    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("first output"),
            &stagingPaths,
            &stageCalls),
        failingOutput,
        outputWriting(
            thirdOutputName(),
            QByteArray("third output"),
            &stagingPaths,
            &stageCalls)
    };
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY2(
        error.contains(QStringLiteral("injected staging")),
        qPrintable(error));
    QCOMPARE(stageCalls, 2);
    QCOMPARE(publishCalls, 0);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    verifyTargetsAbsent(
        fixture.activities,
        QStringList{
            firstOutputName(), secondOutputName(), thirdOutputName()
        });
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::middlePublishFailureRollsBackEveryTarget()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("first output"),
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondOutputName(),
            QByteArray("second output"),
            &stagingPaths,
            &stageCalls),
        outputWriting(
            thirdOutputName(),
            QByteArray("third output"),
            &stagingPaths,
            &stageCalls)
    };

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&publishCalls](
            const QString &stagingPath,
            const QString &targetPath,
            bool &targetPublished,
            QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                targetPublished = false;
                publishError =
                    QStringLiteral("injected middle publish failure");
                return false;
            }
            return publishAtomicNew(
                stagingPath,
                targetPath,
                targetPublished,
                publishError);
        };
    int archiveCalls = 0;
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY2(
        error.contains(QStringLiteral("injected middle")),
        qPrintable(error));
    QCOMPARE(stageCalls, 3);
    QCOMPARE(publishCalls, 2);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!filePathPresent(fixture.backupPath));
    verifyTargetsAbsent(
        fixture.activities,
        QStringList{
            firstOutputName(), secondOutputName(), thirdOutputName()
        });
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::
archiveFailureRunsAfterPublishAndRollsBackTargets()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    const QByteArray firstContents("first output");
    const QByteArray secondContents("second output");
    int stageCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            firstContents,
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondOutputName(),
            secondContents,
            &stagingPaths,
            &stageCalls)
    };

    int publishCalls = 0;
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    int archiveCalls = 0;
    bool archivePathsMatch = false;
    bool allTargetsExistedAtArchive = false;
    bool allStagesWereGoneAtArchive = false;
    const QString firstTarget =
        fixture.activities.filePath(firstOutputName());
    const QString secondTarget =
        fixture.activities.filePath(secondOutputName());
    const SplitActivityArchiveFunction archive =
        [&](const QString &sourcePath,
            const QString &backupPath,
            QString &archiveError) {
            ++archiveCalls;
            archivePathsMatch =
                sourcePath == fixture.sourcePath
                && backupPath == fixture.backupPath;
            allTargetsExistedAtArchive =
                readBytes(firstTarget) == firstContents
                && readBytes(secondTarget) == secondContents;
            allStagesWereGoneAtArchive = true;
            for (const QString &stagingPath : stagingPaths) {
                allStagesWereGoneAtArchive =
                    allStagesWereGoneAtArchive
                    && !filePathPresent(stagingPath);
            }
            archiveError = QStringLiteral("injected archive failure");
            return false;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY2(
        error.contains(QStringLiteral("injected archive")),
        qPrintable(error));
    QCOMPARE(stageCalls, 2);
    QCOMPARE(publishCalls, 2);
    QCOMPARE(archiveCalls, 1);
    QVERIFY(archivePathsMatch);
    QVERIFY(allTargetsExistedAtArchive);
    QVERIFY(allStagesWereGoneAtArchive);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), previousBackup);
    verifyTargetsAbsent(
        fixture.activities,
        QStringList{ firstOutputName(), secondOutputName() });
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::
keepOriginalSuccessPublishesAllWithoutArchiving()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    const QByteArray firstContents("first output");
    const QByteArray secondContents("second output");
    int stageCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            firstContents,
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondOutputName(),
            secondContents,
            &stagingPaths,
            &stageCalls)
    };
    int publishCalls = 0;
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    int archiveCalls = 0;
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](
            const QString &, const QString &, QString &archiveError) {
            ++archiveCalls;
            archiveError = QStringLiteral("archive must not run");
            return false;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            fixture.sourcePath,
            fixture.backupPath,
            outputs,
            true,
            publishedFileNames,
            error,
            publish,
            archive),
        qPrintable(error));

    QVERIFY(error.isEmpty());
    QCOMPARE(stageCalls, 2);
    QCOMPARE(publishCalls, 2);
    QCOMPARE(archiveCalls, 0);
    const QStringList expectedNames = {
        firstOutputName(), secondOutputName()
    };
    QCOMPARE(publishedFileNames, expectedNames);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), previousBackup);
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        firstContents);
    QCOMPARE(
        readBytes(fixture.activities.filePath(secondOutputName())),
        secondContents);
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::removeOriginalSuccessSafelyReplacesBackup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    const QByteArray firstContents("first output");
    const QByteArray secondContents("second output");
    int stageCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            firstContents,
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondOutputName(),
            secondContents,
            &stagingPaths,
            &stageCalls)
    };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            fixture.sourcePath,
            fixture.backupPath,
            outputs,
            false,
            publishedFileNames,
            error),
        qPrintable(error));

    QVERIFY(error.isEmpty());
    QCOMPARE(stageCalls, 2);
    const QStringList expectedNames = {
        firstOutputName(), secondOutputName()
    };
    QCOMPARE(publishedFileNames, expectedNames);
    QVERIFY(!filePathPresent(fixture.sourcePath));
    QCOMPARE(readBytes(fixture.backupPath), sourceContents());
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        firstContents);
    QCOMPARE(
        readBytes(fixture.activities.filePath(secondOutputName())),
        secondContents);
    QVERIFY(readBytes(fixture.backupPath) != previousBackup);
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
    verifyNoArtifactsContaining(
        QDir(QFileInfo(fixture.backupPath).absolutePath()),
        QStringLiteral(".rollback-"));
}

void TestSplitActivitySave::sourceChangeDuringStagingRejectsFinalization()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    const QByteArray originalContents("source-version-one");
    const QByteArray changedContents("source-version-two");
    QCOMPARE(originalContents.size(), changedContents.size());
    writeFixture(fixture.sourcePath, originalContents);
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    int stageCalls = 0;
    QStringList stagingPaths;
    SplitActivityOutput mutatingOutput;
    mutatingOutput.fileName = firstOutputName();
    mutatingOutput.stage =
        [&](const QString &stagingPath, QString &error) {
            ++stageCalls;
            stagingPaths.append(stagingPath);
            if (!writeBytes(
                    stagingPath, QByteArray("first output"), error)) {
                return false;
            }
            QString mutationError;
            if (!writeBytes(
                    fixture.sourcePath, changedContents, mutationError)) {
                error = QStringLiteral("cannot mutate source: %1")
                            .arg(mutationError);
                return false;
            }
            return true;
        };
    const QList<SplitActivityOutput> outputs = {
        mutatingOutput,
        outputWriting(
            secondOutputName(),
            QByteArray("second output"),
            &stagingPaths,
            &stageCalls)
    };

    int publishCalls = 0;
    const AtomicPublishFunction publish = publishingThroughDefault(publishCalls);
    int archiveCalls = 0;
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QVERIFY2(
        error.contains(QStringLiteral("changed"), Qt::CaseInsensitive)
            || error.contains(
                QStringLiteral("modified"), Qt::CaseInsensitive),
        qPrintable(error));
    QCOMPARE(stageCalls, 2);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QCOMPARE(readBytes(fixture.sourcePath), changedContents);
    QCOMPARE(readBytes(fixture.backupPath), previousBackup);
    verifyTargetsAbsent(
        fixture.activities,
        QStringList{ firstOutputName(), secondOutputName() });
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
    QCOMPARE(publishCalls, 2);
}

void TestSplitActivitySave::targetAppearingAfterPreflightIsPreserved()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    int stageCalls = 0;
    QStringList stagingPaths;
    const QList<SplitActivityOutput> outputs = {
        outputWriting(
            firstOutputName(),
            QByteArray("first output"),
            &stagingPaths,
            &stageCalls),
        outputWriting(
            secondOutputName(),
            QByteArray("second output"),
            &stagingPaths,
            &stageCalls)
    };

    const QByteArray concurrentContents("concurrent target");
    bool concurrentTargetCreated = false;
    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &stagingPath,
            const QString &targetPath,
            bool &targetPublished,
            QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                QString creationError;
                concurrentTargetCreated = writeBytes(
                    targetPath, concurrentContents, creationError);
                if (!concurrentTargetCreated) {
                    targetPublished = false;
                    publishError =
                        QStringLiteral("cannot create concurrent target: %1")
                            .arg(creationError);
                    return false;
                }
            }
            return publishAtomicNew(
                stagingPath,
                targetPath,
                targetPublished,
                publishError);
        };
    int archiveCalls = 0;
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        error,
        publish,
        archive));

    QVERIFY(!error.isEmpty());
    QCOMPARE(stageCalls, 2);
    QCOMPARE(publishCalls, 2);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(concurrentTargetCreated);
    QVERIFY(publishedFileNames.isEmpty());
    QVERIFY(!filePathPresent(
        fixture.activities.filePath(firstOutputName())));
    QCOMPARE(
        readBytes(fixture.activities.filePath(secondOutputName())),
        concurrentContents);
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QVERIFY(!filePathPresent(fixture.backupPath));
    verifyNoSplitStageArtifacts(fixture.activities, stagingPaths);
}

void TestSplitActivitySave::publishedFileNamesAreSetOnlyOnSuccess()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    QStringList firstStagingPaths;
    int firstStageCalls = 0;
    const QList<SplitActivityOutput> firstAttempt = {
        outputWriting(
            firstOutputName(),
            QByteArray("published output"),
            &firstStagingPaths,
            &firstStageCalls)
    };
    int failedPublishCalls = 0;
    const AtomicPublishFunction failedPublish =
        [&failedPublishCalls](
            const QString &,
            const QString &,
            bool &targetPublished,
            QString &publishError) {
            ++failedPublishCalls;
            targetPublished = false;
            publishError = QStringLiteral("injected publish failure");
            return false;
        };
    int archiveCalls = 0;
    const SplitActivityArchiveFunction archive =
        [&archiveCalls](const QString &, const QString &, QString &) {
            ++archiveCalls;
            return true;
        };
    QStringList publishedFileNames = { QStringLiteral("stale.json") };
    QString error;

    QVERIFY(!saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        firstAttempt,
        true,
        publishedFileNames,
        error,
        failedPublish,
        archive));

    QCOMPARE(firstStageCalls, 1);
    QCOMPARE(failedPublishCalls, 1);
    QCOMPARE(archiveCalls, 0);
    QVERIFY(publishedFileNames.isEmpty());
    QVERIFY(!filePathPresent(
        fixture.activities.filePath(firstOutputName())));
    verifyNoSplitStageArtifacts(fixture.activities, firstStagingPaths);

    QStringList retryStagingPaths;
    int retryStageCalls = 0;
    const QList<SplitActivityOutput> retry = {
        outputWriting(
            firstOutputName(),
            QByteArray("published output"),
            &retryStagingPaths,
            &retryStageCalls)
    };
    int retryPublishCalls = 0;
    const AtomicPublishFunction retryPublish =
        publishingThroughDefault(retryPublishCalls);
    publishedFileNames = { QStringLiteral("stale-again.json") };
    error = QStringLiteral("stale error");

    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            fixture.sourcePath,
            fixture.backupPath,
            retry,
            true,
            publishedFileNames,
            error,
            retryPublish,
            archive),
        qPrintable(error));

    QVERIFY(error.isEmpty());
    QCOMPARE(retryStageCalls, 1);
    QCOMPARE(retryPublishCalls, 1);
    QCOMPARE(archiveCalls, 0);
    const QStringList expectedNames = { firstOutputName() };
    QCOMPARE(publishedFileNames, expectedNames);
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        QByteArray("published output"));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    verifyNoSplitStageArtifacts(fixture.activities, retryStagingPaths);
}

void TestSplitActivitySave::atomicMoveDoesNotCopyAcrossFilesystems()
{
#ifdef Q_OS_UNIX
    QTemporaryDir sourceDirectory;
    QTemporaryDir targetDirectory(
        QStringLiteral("/dev/shm/gc-split-move-XXXXXX"));
    if (!sourceDirectory.isValid() || !targetDirectory.isValid()) {
        QSKIP("Two temporary filesystems are unavailable");
    }

    struct stat sourceStat;
    struct stat targetStat;
    const QByteArray sourceDirectoryName =
        QFile::encodeName(sourceDirectory.path());
    const QByteArray targetDirectoryName =
        QFile::encodeName(targetDirectory.path());
    QVERIFY(::stat(sourceDirectoryName.constData(), &sourceStat) == 0);
    QVERIFY(::stat(targetDirectoryName.constData(), &targetStat) == 0);
    if (sourceStat.st_dev == targetStat.st_dev) {
        QSKIP("Temporary directories are on the same filesystem");
    }

    const QString sourcePath =
        sourceDirectory.filePath(QStringLiteral("source.json"));
    const QString targetPath =
        targetDirectory.filePath(QStringLiteral("target.json"));
    const QByteArray contents("must not be copied and removed");
    writeFixture(sourcePath, contents);

    QString error;
    QVERIFY(!moveAtomicFile(sourcePath, targetPath, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(sourcePath), contents);
    QVERIFY(!filePathPresent(targetPath));
#else
    QSKIP("Cross-filesystem atomic move test requires Unix");
#endif
}

void TestSplitActivitySave::
archiveSyncFailureRestoresSourceAndPreviousBackup_data()
{
    QTest::addColumn<int>("failedSyncCall");

    QTest::newRow("preserve-backup-directory") << 1;
    QTest::newRow("source-directory-after-archive") << 2;
    QTest::newRow("backup-directory-after-archive") << 3;
}

void TestSplitActivitySave::
archiveSyncFailureRestoresSourceAndPreviousBackup()
{
    QFETCH(int, failedSyncCall);

    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    int syncCalls = 0;
    const AtomicDirectorySyncFunction syncDirectory =
        [&](const QString &, QString &syncError) {
            ++syncCalls;
            if (syncCalls == failedSyncCall) {
                syncError = QStringLiteral("injected directory sync failure");
                return false;
            }
            return true;
        };
    QString error;

    QVERIFY(!archiveSplitActivitySource(
        fixture.sourcePath,
        fixture.backupPath,
        error,
        syncDirectory));

    QVERIFY2(
        error.contains(QStringLiteral("injected directory sync failure")),
        qPrintable(error));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), previousBackup);
    verifyNoArtifactsContaining(
        QDir(QFileInfo(fixture.backupPath).absolutePath()),
        QStringLiteral(".rollback-"));
}

void TestSplitActivitySave::failedArchiveRollbackKeepsCommittedSplitFiles()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    const QByteArray outputContents("committed split output");
    const QList<SplitActivityOutput> outputs = {
        outputWriting(firstOutputName(), outputContents)
    };

    int syncCalls = 0;
    const AtomicDirectorySyncFunction syncDirectory =
        [&](const QString &, QString &syncError) {
            ++syncCalls;
            if (syncCalls == 2) {
                syncError = QStringLiteral("injected post-archive sync failure");
                return false;
            }
            return true;
        };
    int moveCalls = 0;
    const AtomicMoveFunction move =
        [&](const QString &from, const QString &to, QString &moveError) {
            ++moveCalls;
            if (moveCalls == 3) {
                moveError = QStringLiteral("injected source rollback failure");
                return false;
            }
            return moveAtomicFile(from, to, moveError);
        };
    const SplitActivityArchiveFunction archive =
        [&](const QString &source,
            const QString &backup,
            QString &archiveError) {
            return archiveSplitActivitySource(
                source,
                backup,
                archiveError,
                syncDirectory,
                true,
                move);
        };

    QStringList publishedFileNames;
    QString warning;
    QVERIFY(saveSplitActivityFiles(
        fixture.activities,
        fixture.sourcePath,
        fixture.backupPath,
        outputs,
        false,
        publishedFileNames,
        warning,
        publishAtomicNew,
        archive));

    QVERIFY2(
        warning.contains(QStringLiteral("injected source rollback failure")),
        qPrintable(warning));
    QCOMPARE(publishedFileNames, QStringList{ firstOutputName() });
    QVERIFY(!filePathPresent(fixture.sourcePath));
    QCOMPARE(readBytes(fixture.backupPath), sourceContents());
    QCOMPARE(
        readBytes(fixture.activities.filePath(firstOutputName())),
        outputContents);

    const QDir backupDirectory(
        QFileInfo(fixture.backupPath).absolutePath());
    const QFileInfoList rollbackFiles = backupDirectory.entryInfoList(
        QStringList{ QStringLiteral("*.rollback-*") },
        QDir::Files | QDir::Hidden | QDir::System);
    QCOMPARE(rollbackFiles.size(), 1);
    QCOMPARE(readBytes(rollbackFiles.first().absoluteFilePath()),
             previousBackup);
}

void TestSplitActivitySave::archiveMoveFailureRestoresPreviousBackup()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());
    const QByteArray previousBackup("previous backup");
    writeFixture(fixture.backupPath, previousBackup);

    int moveCalls = 0;
    const AtomicMoveFunction move =
        [&](const QString &from, const QString &to, QString &moveError) {
            ++moveCalls;
            if (moveCalls == 2) {
                moveError = QStringLiteral("injected cross-filesystem move");
                return false;
            }
            return moveAtomicFile(from, to, moveError);
        };
    QString error;

    QVERIFY(!archiveSplitActivitySource(
        fixture.sourcePath,
        fixture.backupPath,
        error,
        [](const QString &, QString &) { return true; },
        false,
        move));

    QVERIFY2(
        error.contains(QStringLiteral("injected cross-filesystem move")),
        qPrintable(error));
    QCOMPARE(readBytes(fixture.sourcePath), sourceContents());
    QCOMPARE(readBytes(fixture.backupPath), previousBackup);
    verifyNoArtifactsContaining(
        QDir(QFileInfo(fixture.backupPath).absolutePath()),
        QStringLiteral(".rollback-"));
}

void TestSplitActivitySave::archiveSyncsDistinctCaseSensitiveDirectories()
{
#ifdef Q_OS_UNIX
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir upper(temporary.filePath(QStringLiteral("A")));
    const QDir lower(temporary.filePath(QStringLiteral("a")));
    QVERIFY(QDir().mkpath(upper.absolutePath()));
    QVERIFY(QDir().mkpath(lower.absolutePath()));
    if (upper.canonicalPath() == lower.canonicalPath()) {
        QSKIP("Filesystem is case-insensitive");
    }

    const QString sourcePath =
        upper.filePath(QStringLiteral("source.json"));
    const QString backupPath =
        lower.filePath(QStringLiteral("source.json.bak"));
    writeFixture(sourcePath, sourceContents());

    QSet<QString> syncedDirectories;
    const AtomicDirectorySyncFunction syncDirectory =
        [&](const QString &path, QString &) {
            syncedDirectories.insert(QFileInfo(path).absolutePath());
            return true;
        };
    QString error;
    QVERIFY2(
        archiveSplitActivitySource(
            sourcePath, backupPath, error, syncDirectory),
        qPrintable(error));

    QCOMPARE(syncedDirectories.size(), 2);
    QVERIFY(syncedDirectories.contains(upper.absolutePath()));
    QVERIFY(syncedDirectories.contains(lower.absolutePath()));
#else
    QSKIP("Case-sensitive directory test requires Unix");
#endif
}

void TestSplitActivitySave::sourceAliasIsCanonicalizedBeforeArchive()
{
    Fixture fixture;
    QVERIFY(fixture.initialize());
    writeFixture(fixture.sourcePath, sourceContents());

    const QString aliasPath =
        fixture.temporary.filePath(QStringLiteral("activities-alias"));
    if (!QFile::link(fixture.activities.absolutePath(), aliasPath)
        || !QFileInfo(aliasPath).isSymLink()) {
        QSKIP("Directory symbolic links are unavailable on this platform");
    }
    const QString aliasedSource =
        QDir(aliasPath).filePath(sourceFileName());
    QString archivedSource;
    const SplitActivityArchiveFunction archive =
        [&](const QString &source,
            const QString &,
            QString &) {
            archivedSource = source;
            return true;
        };

    QStringList publishedFileNames;
    QString error;
    QVERIFY2(
        saveSplitActivityFiles(
            fixture.activities,
            aliasedSource,
            fixture.backupPath,
            { outputWriting(firstOutputName(), QByteArray("output")) },
            false,
            publishedFileNames,
            error,
            publishAtomicNew,
            archive),
        qPrintable(error));

    QCOMPARE(
        archivedSource,
        fixture.activities.filePath(sourceFileName()));
}

QTEST_MAIN(TestSplitActivitySave)
#include "testSplitActivitySave.moc"
