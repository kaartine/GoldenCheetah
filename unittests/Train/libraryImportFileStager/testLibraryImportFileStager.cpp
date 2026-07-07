#include <QtTest>

#include "LibraryImportFileStager.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(data) == data.size()
        && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

class TestLibraryImportFileStager : public QObject
{
    Q_OBJECT

private slots:
    void copiesMissingTarget();
    void reusesIdenticalExistingTarget();
    void rejectsDifferentExistingTarget();
    void rejectsDifferentSourceForPreparedTarget();
    void rollbackRemovesOnlyCreatedTargets();
    void missingSourceReportsIoError();
#ifdef Q_OS_UNIX
    void rejectsSymlinkTarget();
#endif
};

void TestLibraryImportFileStager::copiesMissingTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.erg"));
    const QString target = directory.filePath(QStringLiteral("library.erg"));
    const QByteArray contents("workout-data");
    QVERIFY(writeFile(source, contents));

    LibraryImportFileStager stager;
    const LibraryImportStageResult result = stager.stage(source, target);

    QCOMPARE(result.status, LibraryImportStageStatus::copied);
    QVERIFY(result.succeeded());
    QVERIFY(result.created());
    QVERIFY(result.error.isEmpty());
    QCOMPARE(readFile(target), contents);
}

void TestLibraryImportFileStager::reusesIdenticalExistingTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.rlv"));
    const QString target = directory.filePath(QStringLiteral("library.rlv"));
    const QByteArray contents("video-sync-data");
    QVERIFY(writeFile(source, contents));
    QVERIFY(writeFile(target, contents));

    LibraryImportFileStager stager;
    const LibraryImportStageResult result = stager.stage(source, target);

    QCOMPARE(result.status, LibraryImportStageStatus::ready);
    QVERIFY(result.succeeded());
    QVERIFY(!result.created());
    QVERIFY(result.error.isEmpty());
    QVERIFY(stager.rollback().isEmpty());
    QCOMPARE(readFile(target), contents);
}

void TestLibraryImportFileStager::rejectsDifferentExistingTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.erg"));
    const QString target = directory.filePath(QStringLiteral("library.erg"));
    QVERIFY(writeFile(source, QByteArray("new-workout")));
    QVERIFY(writeFile(target, QByteArray("existing-workout")));

    LibraryImportFileStager stager;
    const LibraryImportStageResult result = stager.stage(source, target);

    QCOMPARE(result.status, LibraryImportStageStatus::targetConflict);
    QVERIFY(!result.succeeded());
    QVERIFY(!result.created());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readFile(target), QByteArray("existing-workout"));
}

void TestLibraryImportFileStager::rejectsDifferentSourceForPreparedTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.erg"));
    const QString second = directory.filePath(QStringLiteral("second.erg"));
    const QString target = directory.filePath(QStringLiteral("library.erg"));
    QVERIFY(writeFile(first, QByteArray("same-contents")));
    QVERIFY(writeFile(second, QByteArray("same-contents")));

    LibraryImportFileStager stager;
    QCOMPARE(stager.stage(first, target).status,
             LibraryImportStageStatus::copied);
    const LibraryImportStageResult result = stager.stage(second, target);

    QCOMPARE(result.status, LibraryImportStageStatus::targetConflict);
    QVERIFY(!result.succeeded());
    QCOMPARE(readFile(target), QByteArray("same-contents"));
}

void TestLibraryImportFileStager::rollbackRemovesOnlyCreatedTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString reusedSource =
        directory.filePath(QStringLiteral("reused-source.rlv"));
    const QString reusedTarget =
        directory.filePath(QStringLiteral("reused-target.rlv"));
    const QString copiedSource =
        directory.filePath(QStringLiteral("copied-source.erg"));
    const QString copiedTarget =
        directory.filePath(QStringLiteral("copied-target.erg"));
    QVERIFY(writeFile(reusedSource, QByteArray("existing")));
    QVERIFY(writeFile(reusedTarget, QByteArray("existing")));
    QVERIFY(writeFile(copiedSource, QByteArray("created")));

    LibraryImportFileStager stager;
    QCOMPARE(stager.stage(reusedSource, reusedTarget).status,
             LibraryImportStageStatus::ready);
    QCOMPARE(stager.stage(copiedSource, copiedTarget).status,
             LibraryImportStageStatus::copied);

    QVERIFY(stager.rollback().isEmpty());
    QVERIFY(QFile::exists(reusedTarget));
    QCOMPARE(readFile(reusedTarget), QByteArray("existing"));
    QVERIFY(!QFile::exists(copiedTarget));
}

void TestLibraryImportFileStager::missingSourceReportsIoError()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    LibraryImportFileStager stager;
    const LibraryImportStageResult result = stager.stage(
        directory.filePath(QStringLiteral("missing.erg")),
        directory.filePath(QStringLiteral("library.erg")));

    QCOMPARE(result.status, LibraryImportStageStatus::ioError);
    QVERIFY(!result.succeeded());
    QVERIFY(!result.created());
    QVERIFY(!result.error.isEmpty());
}

#ifdef Q_OS_UNIX
void TestLibraryImportFileStager::rejectsSymlinkTarget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.erg"));
    const QString existing =
        directory.filePath(QStringLiteral("existing.erg"));
    const QString target = directory.filePath(QStringLiteral("library.erg"));
    QVERIFY(writeFile(source, QByteArray("same")));
    QVERIFY(writeFile(existing, QByteArray("same")));
    QVERIFY(QFile::link(existing, target));

    LibraryImportFileStager stager;
    const LibraryImportStageResult result = stager.stage(source, target);

    QCOMPARE(result.status, LibraryImportStageStatus::targetConflict);
    QVERIFY(!result.succeeded());
    QVERIFY(QFileInfo(target).isSymLink());
    QCOMPARE(readFile(existing), QByteArray("same"));
}
#endif

QTEST_MAIN(TestLibraryImportFileStager)
#include "testLibraryImportFileStager.moc"
