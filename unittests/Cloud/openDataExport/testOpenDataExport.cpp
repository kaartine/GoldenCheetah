#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif

#include "Cloud/OpenDataExport.h"
#include "Cloud/OpenDataTemporaryArchive.h"
#include "Cloud/OpenDataUploadWorker.h"
#include "Cloud/NetworkReplyWait.h"

#include "zipreader.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>

class Context;

static_assert(!std::is_constructible_v<OpenDataUploadWorker, Context *>);

namespace {

using namespace OpenDataExport;

std::unique_ptr<ZipReader> openArchive(const QByteArray &contents)
{
    auto source = std::make_unique<QBuffer>();
    source->setData(contents);
    if (!source->open(QIODevice::ReadOnly)) return {};
    return std::make_unique<ZipReader>(std::move(source));
}

Request exampleRequest()
{
    Request request;
    request.athleteId = QStringLiteral("athlete-id");
    request.cyclist = QStringLiteral("settings-key");
    request.rideCount = 2;
    request.formatVersion = 1;
    request.entries = {
        {QStringLiteral("athlete-id.json"),
         QByteArrayLiteral("{\"RIDES\":2}")},
        {QStringLiteral("2026_07_28_12_00_00.csv"),
         QByteArrayLiteral("secs,km,power,hr,cad,alt\n0,0,180,120,85,10\n")}
    };
    return request;
}

} // namespace

class TestOpenDataExport : public QObject
{
    Q_OBJECT

private slots:
    void immutableRequestBuildsArchiveAfterSourcesAreDestroyed();
    void cancelledArchivePublishesNoPartialPayload();
    void invalidEntryPublishesNoPartialPayload();
    void diskArchiveStreamsEntriesAndSealsPayload();
    void diskArchiveRejectsUnsafeAndDuplicateNames();
    void sealedArchiveRejectsLaterModification();
    void validatedArchiveDeviceIsRewoundForUpload();
    void archiveHashCanBeCancelled();
    void archiveDescriptionYieldsBetweenChunks();
    void temporaryWorkspaceOwnsEveryArtifact();
    void recentUnlockedWorkspaceIsScavenged();
    void activeWorkspaceLockPreventsScavenging();
    void workspaceScavengerNeverTraversesDirectories();
    void windowsScavengerRejectsNonPrivateWorkspace();
    void failedWorkspaceRemovalCanBeRetried();
    void preexistingWorkspaceSurvivesFailedCreate();
    void renamedUnixWorkspaceIsRemovedByIdentity();
    void failedWindowsPostCreateValidationRollsBackWorkspace();
    void activeWindowsWorkspaceCannotBeRenamed();
    void windowsWorkspaceAndFilesHavePrivateSecurity();
    void windowsSecurityVerifierRejectsPublicDacl();
    void destroyingActiveWorkerCancelsAndJoins();
    void managedWorkerDeletesItself();
    void completionIsDeliveredOnReceiversThread();
    void directResultReceiverMayDeleteWorker();
    void cancelAndJoinWaitsForCooperativeTask();
    void completedSuccessSurvivesLateCancellation();
    void cancellationBeforeStartIsPreserved();
    void selfThreadCancellationDoesNotSelfJoin();
    void selfThreadDeletionDoesNotTerminate();
    void concurrentWaitAndOwnerDeliveryJoinOnce();
    void nativeWorkerRunsQtNetworkEventLoop();
    void cancellationAbortsNativeWorkerNetworkWait();
};

void
TestOpenDataExport::immutableRequestBuildsArchiveAfterSourcesAreDestroyed()
{
    std::shared_ptr<const Request> request;
    QByteArray expectedJson;
    QByteArray expectedCsv;

    {
        Request captured = exampleRequest();
        expectedJson = captured.entries.at(0).contents;
        expectedCsv = captured.entries.at(1).contents;
        request = std::make_shared<const Request>(captured);

        captured.athleteId.fill(QLatin1Char('x'));
        captured.cyclist.clear();
        captured.entries[0].contents.fill('x');
        captured.entries.clear();
    }

    QByteArray archive;
    QThread *executionThread = nullptr;
    QThread *ownerThread = QThread::currentThread();
    const auto task =
        [&](const Request &captured,
            const CancellationCheck &cancelled,
            const ProgressCallback &) {
            executionThread = QThread::currentThread();
            const ArchiveResult result = buildArchive(captured, cancelled);
            archive = result.contents;
            return result.ok()
                ? UploadResult::succeeded()
                : UploadResult::failed(result.error);
        };

    OpenDataUploadWorker worker(request, task);
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    worker.start();
    QVERIFY2(worker.wait(5000), "OpenData archive worker did not finish");
    QCoreApplication::processEvents();

    QVERIFY(executionThread != ownerThread);
    QCOMPARE(succeeded.count(), 1);
    const QList<QVariant> completion = succeeded.takeFirst();
    QCOMPARE(completion.at(0).toString(), QStringLiteral("settings-key"));
    QCOMPARE(completion.at(1).toInt(), 2);
    QCOMPARE(completion.at(2).toInt(), 1);

    const std::unique_ptr<ZipReader> reader = openArchive(archive);
    QVERIFY(reader);
    QCOMPARE(reader->status(), ZipReader::NoError);
    QCOMPARE(reader->count(), 2);
    QCOMPARE(reader->fileData(QStringLiteral("athlete-id.json")),
             expectedJson);
    QCOMPARE(reader->fileData(
                 QStringLiteral("2026_07_28_12_00_00.csv")),
             expectedCsv);
}

void
TestOpenDataExport::cancelledArchivePublishesNoPartialPayload()
{
    const Request request = exampleRequest();
    std::atomic<int> cancellationChecks{0};

    const ArchiveResult result = buildArchive(
        request,
        [&]() {
            return cancellationChecks.fetch_add(
                       1, std::memory_order_relaxed) >= 2;
        });

    QVERIFY(result.cancelled);
    QVERIFY(result.contents.isEmpty());
    QVERIFY(result.error.isEmpty());
}

void
TestOpenDataExport::invalidEntryPublishesNoPartialPayload()
{
    Request request = exampleRequest();
    request.entries[1].name = QStringLiteral("../private.csv");

    const ArchiveResult result = buildArchive(request);

    QVERIFY(!result.ok());
    QVERIFY(!result.cancelled);
    QVERIFY(result.contents.isEmpty());
    QVERIFY(!result.error.isEmpty());
}

void
TestOpenDataExport::diskArchiveStreamsEntriesAndSealsPayload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString archivePath =
        directory.filePath(QStringLiteral("opendata.zip"));

    ArchiveWriter writer(archivePath);
    QString error;
    QBuffer summary;
    summary.setData(QByteArrayLiteral("{\"RIDES\":1}"));
    QVERIFY(summary.open(QIODevice::ReadOnly));
    QVERIFY2(
        writer.addFile(QStringLiteral("athlete.json"), &summary, error),
        qPrintable(error));
    QBuffer samples;
    samples.setData(QByteArrayLiteral(
        "secs,km,power,hr,cad,alt\n0,0,180,120,85,10\n"));
    QVERIFY(samples.open(QIODevice::ReadOnly));
    QVERIFY2(
        writer.addFile(QStringLiteral("activity.csv"), &samples, error),
        qPrintable(error));
    QVERIFY2(writer.finish(error), qPrintable(error));

    Request request;
    request.archivePath = archivePath;
    QVERIFY2(
        describeArchive(
            request.archivePath,
            request.archiveSize,
            request.archiveSha256,
            error),
        qPrintable(error));
    QVERIFY(request.archiveSize > 0);
    QCOMPARE(request.archiveSha256.size(), 32);
    QVERIFY2(validateArchive(request, error), qPrintable(error));

    ZipReader reader(archivePath);
    QCOMPARE(reader.status(), ZipReader::NoError);
    QCOMPARE(reader.count(), 2);
    QCOMPARE(
        reader.fileData(QStringLiteral("athlete.json")),
        QByteArrayLiteral("{\"RIDES\":1}"));
    QCOMPARE(
        reader.fileData(QStringLiteral("activity.csv")),
        samples.data());
}

void
TestOpenDataExport::diskArchiveRejectsUnsafeAndDuplicateNames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ArchiveWriter writer(
        directory.filePath(QStringLiteral("opendata.zip")));
    QString error;
    QBuffer source;
    source.setData(QByteArrayLiteral("payload"));
    QVERIFY(source.open(QIODevice::ReadOnly));

    QVERIFY(!writer.addFile(
        QStringLiteral("../private.json"), &source, error));
    QVERIFY(!error.isEmpty());
    error.clear();
    source.seek(0);
    QVERIFY2(
        writer.addFile(QStringLiteral("athlete.json"), &source, error),
        qPrintable(error));
    source.seek(0);
    QVERIFY(!writer.addFile(
        QStringLiteral("athlete.json"), &source, error));
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataExport::sealedArchiveRejectsLaterModification()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString archivePath =
        directory.filePath(QStringLiteral("opendata.zip"));
    ArchiveWriter writer(archivePath);
    QString error;
    QBuffer source;
    source.setData(QByteArrayLiteral("payload"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY2(
        writer.addFile(QStringLiteral("athlete.json"), &source, error),
        qPrintable(error));
    QVERIFY2(writer.finish(error), qPrintable(error));

    Request request;
    request.archivePath = archivePath;
    QVERIFY(describeArchive(
        request.archivePath,
        request.archiveSize,
        request.archiveSha256,
        error));
    QFile archive(archivePath);
    QVERIFY(archive.open(QIODevice::Append));
    QCOMPARE(archive.write("tampered"), qint64(8));
    archive.close();

    QVERIFY(!validateArchive(request, error));
    QVERIFY(!error.isEmpty());
}

void
TestOpenDataExport::validatedArchiveDeviceIsRewoundForUpload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString archivePath =
        directory.filePath(QStringLiteral("opendata.zip"));
    ArchiveWriter writer(archivePath);
    QString error;
    QBuffer source;
    source.setData(QByteArrayLiteral("payload"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(writer.addFile(
        QStringLiteral("athlete.json"), &source, error));
    QVERIFY(writer.finish(error));

    Request request;
    request.archivePath = archivePath;
    QVERIFY(describeArchive(
        request.archivePath,
        request.archiveSize,
        request.archiveSha256,
        error));
    QFile archive;

    QVERIFY2(
        openValidatedArchive(request, archive, error),
        qPrintable(error));
    QVERIFY(archive.isOpen());
    QCOMPARE(archive.fileName(), request.archivePath);
    QCOMPARE(archive.pos(), qint64(0));
    QCOMPARE(archive.readAll().size(), request.archiveSize);
}

void TestOpenDataExport::archiveHashCanBeCancelled()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("large.zip"));
    QFile payload(path);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QVERIFY(payload.resize(4 * 1024 * 1024));
    payload.close();
    Request request;
    request.archivePath = path;
    request.archiveSize = 4 * 1024 * 1024;
    request.archiveSha256 = QByteArray(32, '\0');
    QFile archive;
    int cancellationChecks = 0;
    QString error;

    const ArchiveValidationResult result =
        openValidatedArchive(
            request,
            archive,
            [&cancellationChecks]() {
                return ++cancellationChecks >= 2;
            },
            error);

    QCOMPARE(result, ArchiveValidationResult::Cancelled);
    QVERIFY(!archive.isOpen());
    QVERIFY(error.isEmpty());
}

void TestOpenDataExport::archiveDescriptionYieldsBetweenChunks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("large.zip"));
    QFile payload(path);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QVERIFY(payload.resize(4 * 1024 * 1024));
    payload.close();
    ArchiveDescriptionBuilder builder(path);
    QString error;

    QCOMPARE(
        builder.processNext(error),
        ArchiveDescriptionResult::InProgress);
    QVERIFY(error.isEmpty());
    QCOMPARE(builder.bytesProcessed(), qint64(1024 * 1024));

    ArchiveDescriptionResult result =
        ArchiveDescriptionResult::InProgress;
    while (result == ArchiveDescriptionResult::InProgress)
        result = builder.processNext(error);

    QCOMPARE(result, ArchiveDescriptionResult::Complete);
    QVERIFY(error.isEmpty());
    QCOMPARE(builder.size(), qint64(4 * 1024 * 1024));
    QCOMPARE(builder.sha256().size(), 32);
}

void TestOpenDataExport::temporaryWorkspaceOwnsEveryArtifact()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    const QString workspace = lease->directoryPath();
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions workspacePermissions =
        QFileInfo(workspace).permissions();
    QVERIFY(workspacePermissions & QFileDevice::ReadOwner);
    QVERIFY(workspacePermissions & QFileDevice::WriteOwner);
    QVERIFY(workspacePermissions & QFileDevice::ExeOwner);
    QVERIFY(!(workspacePermissions
        & (QFileDevice::ReadGroup
           | QFileDevice::WriteGroup
           | QFileDevice::ExeGroup
           | QFileDevice::ReadOther
           | QFileDevice::WriteOther
           | QFileDevice::ExeOther)));
#endif
    QStringList artifacts = {lease->path()};
    const QStringList templates = {
        QStringLiteral("summary-XXXXXX.json"),
        QStringLiteral("samples-XXXXXX.csv"),
        QStringLiteral("source-XXXXXX.fit")
    };
    for (const QString &fileTemplate : templates) {
        std::unique_ptr<
            OpenDataTemporaryArchive::TemporaryFile>
            artifact =
            lease->createFile(fileTemplate, error);
        QVERIFY2(artifact, qPrintable(error));
        QVERIFY(artifact->path().startsWith(
            workspace + QLatin1Char('/')));
        QVERIFY(artifact->device()->write(
            "private workout data") > 0);
        artifacts.append(artifact->path());
    }
    QVERIFY(!lease->createFile(
        QStringLiteral("../outside-XXXXXX.json"),
        error));
    QVERIFY(!error.isEmpty());

    QVERIFY2(lease->remove(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(workspace));
    for (const QString &path : artifacts)
        QVERIFY(!QFileInfo::exists(path));
}

void TestOpenDataExport::recentUnlockedWorkspaceIsScavenged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace = directory.filePath(
        QStringLiteral("gc-opendata-123-crashed"));
    QString creationError;
    QVERIFY2(
        OpenDataTemporaryArchive::
            createPrivateWorkspaceForTest(
                directory.path(),
                QStringLiteral(
                    "gc-opendata-123-crashed"),
                creationError),
        qPrintable(creationError));
    const QStringList artifacts = {
        QDir(workspace).filePath(
            QStringLiteral("payload.zip")),
        QDir(workspace).filePath(
            QStringLiteral("summary.json")),
        QDir(workspace).filePath(
            QStringLiteral("samples.csv")),
        QDir(workspace).filePath(
            QStringLiteral("source.fit"))
    };
    for (const QString &path : artifacts) {
        QFile artifact(path);
        QVERIFY(artifact.open(QIODevice::WriteOnly));
        QVERIFY(artifact.write("private workout data") > 0);
    }
    QStringList errors;

    const int removed =
        OpenDataTemporaryArchive::removeAbandoned(
            directory.path(),
            errors);

    QCOMPARE(removed, 1);
    QVERIFY(errors.isEmpty());
    QVERIFY(!QFileInfo::exists(workspace));
    for (const QString &path : artifacts)
        QVERIFY(!QFileInfo::exists(path));
}

void TestOpenDataExport::activeWorkspaceLockPreventsScavenging()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    QFile file(lease->path());
    QVERIFY(file.open(
        QIODevice::WriteOnly | QIODevice::Append));
    QVERIFY(file.write("private workout data") > 0);
    file.close();
    QStringList errors;

    QCOMPARE(
        OpenDataTemporaryArchive::removeAbandoned(
            directory.path(),
            errors),
        0);
    QVERIFY(errors.isEmpty());
    QVERIFY(QFileInfo::exists(lease->directoryPath()));
    QVERIFY(QFileInfo::exists(lease->path()));
}

void
TestOpenDataExport::workspaceScavengerNeverTraversesDirectories()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace = directory.filePath(
        QStringLiteral("gc-opendata-123-nested"));
    QString creationError;
    QVERIFY2(
        OpenDataTemporaryArchive::
            createPrivateWorkspaceForTest(
                directory.path(),
                QStringLiteral(
                    "gc-opendata-123-nested"),
                creationError),
        qPrintable(creationError));
    const QString nested =
        QDir(workspace).filePath(QStringLiteral("junction-target"));
    QVERIFY(QDir().mkpath(nested));
    const QString sentinel =
        QDir(nested).filePath(QStringLiteral("keep.txt"));
    QFile file(sentinel);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("must remain") > 0);
    file.close();
    QStringList errors;

    QCOMPARE(
        OpenDataTemporaryArchive::removeAbandoned(
            directory.path(), errors),
        0);

    QVERIFY(!errors.isEmpty());
    QVERIFY(QFileInfo::exists(workspace));
    QVERIFY(QFileInfo::exists(sentinel));
}

void
TestOpenDataExport::windowsScavengerRejectsNonPrivateWorkspace()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation is Windows-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace =
        directory.filePath(
            QStringLiteral(
                "gc-opendata-public"));
    QVERIFY(QDir().mkdir(workspace));
    QFile payload(
        QDir(workspace).filePath(
            QStringLiteral("payload.zip")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.close();
    QStringList errors;

    QCOMPARE(
        OpenDataTemporaryArchive::removeAbandoned(
            directory.path(), errors),
        0);
    QVERIFY(!errors.isEmpty());
    QVERIFY(QFileInfo::exists(workspace));
#endif
}

void TestOpenDataExport::failedWorkspaceRemovalCanBeRetried()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory permission failure is Unix-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    QFile file(lease->path());
    QVERIFY(file.open(
        QIODevice::WriteOnly | QIODevice::Append));
    QVERIFY(file.write("private workout data") > 0);
    file.close();
    const QFileDevice::Permissions writablePermissions =
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner;
    QVERIFY(QFile::setPermissions(
        lease->directoryPath(),
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QVERIFY(!lease->remove(error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(lease->path()));
    QVERIFY(QFile::setPermissions(
        lease->directoryPath(), writablePermissions));
    QVERIFY2(lease->remove(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(lease->directoryPath()));
#endif
}

void TestOpenDataExport::preexistingWorkspaceSurvivesFailedCreate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspaceName =
        QStringLiteral("gc-opendata-existing");
    const QString workspace =
        directory.filePath(workspaceName);
    QVERIFY(QDir().mkdir(workspace));
    QString error;

    QVERIFY(!OpenDataTemporaryArchive::
        createPrivateWorkspaceForTest(
            directory.path(), workspaceName, error));

    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(workspace));
}

void TestOpenDataExport::renamedUnixWorkspaceIsRemovedByIdentity()
{
#ifndef Q_OS_UNIX
    QSKIP("Descriptor-based workspace cleanup is Unix-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    const std::unique_ptr<
        OpenDataTemporaryArchive::TemporaryFile> file =
        lease->createFile(
            QStringLiteral("samples-XXXXXX.csv"),
            error);
    QVERIFY2(file, qPrintable(error));
    QCOMPARE(
        file->device()->write("private workout data"),
        qint64(20));
    file->device()->close();

    const QString original = lease->directoryPath();
    const QString renamed =
        directory.filePath(QStringLiteral("renamed-workspace"));
    QVERIFY(QDir().rename(original, renamed));
    QVERIFY(!QFileInfo::exists(original));
    QVERIFY(QFileInfo::exists(renamed));

    QVERIFY2(lease->remove(error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(renamed));
#endif
}

void
TestOpenDataExport::
failedWindowsPostCreateValidationRollsBackWorkspace()
{
#ifndef Q_OS_WIN
    QSKIP("Windows post-create rollback is Windows-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspaceName =
        QStringLiteral(
            "gc-opendata-validation-failure");
    const QString workspace =
        directory.filePath(workspaceName);
    QString error;

    QVERIFY(!OpenDataTemporaryArchive::
        rejectPrivateWorkspaceAfterCreateForTest(
            directory.path(),
            workspaceName,
            error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(workspace));
#endif
}

void TestOpenDataExport::activeWindowsWorkspaceCannotBeRenamed()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory handle sharing is Windows-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    const QString workspace = lease->directoryPath();
    const QString replacement =
        workspace + QStringLiteral("-replacement");

    QVERIFY(!QDir().rename(workspace, replacement));
    QVERIFY(QFileInfo::exists(workspace));
    QVERIFY(!QFileInfo::exists(replacement));
#endif
}

void
TestOpenDataExport::windowsWorkspaceAndFilesHavePrivateSecurity()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation is Windows-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    QVERIFY(OpenDataTemporaryArchive::
        hasPrivateSecurityForTest(
            lease->directoryPath(), true));

    const std::unique_ptr<
        OpenDataTemporaryArchive::TemporaryFile> file =
        lease->createFile(
            QStringLiteral("samples-XXXXXX.csv"),
            error);
    QVERIFY2(file, qPrintable(error));
    file->device()->close();
    QVERIFY(OpenDataTemporaryArchive::
        hasPrivateSecurityForTest(
            file->path(), false));
#endif
}

void TestOpenDataExport::windowsSecurityVerifierRejectsPublicDacl()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation is Windows-specific");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto lease =
        OpenDataTemporaryArchive::Lease::create(
            directory.path(), error);
    QVERIFY2(lease, qPrintable(error));
    const std::unique_ptr<
        OpenDataTemporaryArchive::TemporaryFile> file =
        lease->createFile(
            QStringLiteral("samples-XXXXXX.csv"),
            error);
    QVERIFY2(file, qPrintable(error));
    file->device()->close();
    QString nativePath =
        QDir::toNativeSeparators(file->path());

    QCOMPARE(
        SetNamedSecurityInfoW(
            reinterpret_cast<LPWSTR>(
                nativePath.data()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION
                | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        DWORD(ERROR_SUCCESS));
    QVERIFY(!OpenDataTemporaryArchive::
        hasPrivateSecurityForTest(
            file->path(), false));
#endif
}

void
TestOpenDataExport::destroyingActiveWorkerCancelsAndJoins()
{
    QSemaphore started;
    std::atomic<bool> taskExited{false};
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    const auto task =
        [&](const Request &,
            const CancellationCheck &cancelled,
            const ProgressCallback &) {
            started.release();
            std::unique_lock<std::mutex> lock(waitMutex);
            while (!cancelled())
                waitCondition.wait_for(lock, std::chrono::milliseconds(10));
            taskExited.store(true, std::memory_order_release);
            return UploadResult::cancelled();
        };

    auto *worker = new OpenDataUploadWorker(
        std::make_shared<const Request>(exampleRequest()), task);
    worker->start();
    QVERIFY2(started.tryAcquire(1, 2000), "Worker task did not start");

    QElapsedTimer elapsed;
    elapsed.start();
    delete worker;

    QVERIFY(taskExited.load(std::memory_order_acquire));
    QVERIFY2(elapsed.elapsed() < 2000,
             "Worker destruction did not join promptly");
}

void
TestOpenDataExport::managedWorkerDeletesItself()
{
    const auto task =
        [](const Request &,
           const CancellationCheck &,
           const ProgressCallback &) {
            return UploadResult::succeeded();
        };
    QPointer<OpenDataUploadWorker> worker =
        new OpenDataUploadWorker(
            std::make_shared<const Request>(exampleRequest()), task);

    worker->startManaged();

    QTRY_VERIFY_WITH_TIMEOUT(worker.isNull(), 2000);
}

void
TestOpenDataExport::completionIsDeliveredOnReceiversThread()
{
    const auto task =
        [](const Request &,
           const CancellationCheck &,
           const ProgressCallback &) {
            return UploadResult::succeeded();
        };
    auto *worker = new OpenDataUploadWorker(
        std::make_shared<const Request>(exampleRequest()), task);
    QObject receiver;
    QThread *deliveryThread = nullptr;
    connect(
        worker, &OpenDataUploadWorker::succeeded,
        &receiver,
        [&](const QString &, int, int) {
            deliveryThread = QThread::currentThread();
        });

    worker->startManaged();

    QTRY_VERIFY_WITH_TIMEOUT(deliveryThread != nullptr, 2000);
    QCOMPARE(deliveryThread, QThread::currentThread());
}

void TestOpenDataExport::directResultReceiverMayDeleteWorker()
{
    const auto task =
        [](const Request &,
           const CancellationCheck &,
           const ProgressCallback &) {
            return UploadResult::succeeded();
        };
    QPointer<OpenDataUploadWorker> worker =
        new OpenDataUploadWorker(
            std::make_shared<const Request>(exampleRequest()), task);
    QObject receiver;
    bool resultDelivered = false;
    bool finishedAfterDeletion = false;
    connect(
        worker, &OpenDataUploadWorker::succeeded,
        &receiver,
        [&]() {
            resultDelivered = true;
            delete worker.data();
        },
        Qt::DirectConnection);
    connect(
        worker, &OpenDataUploadWorker::finished,
        &receiver,
        [&]() {
            finishedAfterDeletion = true;
        },
        Qt::DirectConnection);

    worker->start();

    QTRY_VERIFY_WITH_TIMEOUT(resultDelivered, 2000);
    QVERIFY(worker.isNull());
    QVERIFY(!finishedAfterDeletion);
}

void
TestOpenDataExport::cancelAndJoinWaitsForCooperativeTask()
{
    QSemaphore started;
    std::atomic<bool> taskExited{false};
    const auto task =
        [&](const Request &,
            const CancellationCheck &cancelled,
            const ProgressCallback &) {
            started.release();
            while (!cancelled())
                std::this_thread::yield();
            taskExited.store(true, std::memory_order_release);
            return UploadResult::cancelled();
        };
    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    worker.start();
    QVERIFY(started.tryAcquire(1, 2000));

    QElapsedTimer elapsed;
    elapsed.start();
    worker.cancelAndJoin();

    QVERIFY(taskExited.load(std::memory_order_acquire));
    QVERIFY(!worker.isRunning());
    QVERIFY2(elapsed.elapsed() < 2000,
             "Explicit shutdown join did not stop promptly");
}

void
TestOpenDataExport::completedSuccessSurvivesLateCancellation()
{
    const auto task =
        [](const Request &,
           const CancellationCheck &,
           const ProgressCallback &) {
            return UploadResult::succeeded();
        };
    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    QSignalSpy succeeded(
        &worker, &OpenDataUploadWorker::succeeded);
    QSignalSpy finished(
        &worker, &OpenDataUploadWorker::finished);

    worker.start();
    QElapsedTimer elapsed;
    elapsed.start();
    while (worker.isRunning() && elapsed.elapsed() < 2000)
        QThread::msleep(1);
    QVERIFY2(!worker.isRunning(), "Worker task did not complete");

    worker.cancelAndJoin();

    QCOMPARE(succeeded.size(), 1);
    QCOMPARE(finished.size(), 1);
}

void
TestOpenDataExport::cancellationBeforeStartIsPreserved()
{
    std::atomic<bool> observedCancellation{false};
    const auto task =
        [&](const Request &,
            const CancellationCheck &cancelled,
            const ProgressCallback &) {
            observedCancellation.store(
                cancelled(), std::memory_order_release);
            return UploadResult::cancelled();
        };
    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);

    worker.requestCancellation();
    worker.start();

    QVERIFY(worker.wait(2000));
    QVERIFY(observedCancellation.load(
        std::memory_order_acquire));
}

void
TestOpenDataExport::selfThreadCancellationDoesNotSelfJoin()
{
    OpenDataUploadWorker *workerPointer = nullptr;
    std::atomic<bool> returnedFromCancellation{false};
    const auto task =
        [&](const Request &,
            const CancellationCheck &,
            const ProgressCallback &) {
            workerPointer->cancelAndJoin();
            returnedFromCancellation.store(
                true, std::memory_order_release);
            return UploadResult::cancelled();
        };
    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    workerPointer = &worker;

    worker.start();

    QVERIFY(worker.wait(2000));
    QVERIFY(returnedFromCancellation.load(
        std::memory_order_acquire));
}

void TestOpenDataExport::selfThreadDeletionDoesNotTerminate()
{
    static const QString ChildEnvironment =
        QStringLiteral("GC_OPENDATA_SELF_DELETE_CHILD");
    if (qEnvironmentVariableIsSet(
            ChildEnvironment.toLatin1().constData())) {
        std::atomic<bool> returnedFromTask{false};
        OpenDataUploadWorker *worker = nullptr;
        const auto task =
            [&](const Request &,
                const CancellationCheck &,
                const ProgressCallback &) {
                OpenDataUploadWorker *owned = worker;
                worker = nullptr;
                delete owned;
                returnedFromTask.store(
                    true, std::memory_order_release);
                return UploadResult::cancelled();
            };
        worker = new OpenDataUploadWorker(
            std::make_shared<const Request>(
                exampleRequest()),
            task);

        worker->start();

        QTRY_VERIFY_WITH_TIMEOUT(
            returnedFromTask.load(
                std::memory_order_acquire),
            2000);
        return;
    }

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(ChildEnvironment, QStringLiteral("1"));
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral(
            "selfThreadDeletionDoesNotTerminate")});

    QVERIFY2(
        child.waitForStarted(2000),
        qPrintable(child.errorString()));
    QVERIFY2(
        child.waitForFinished(5000),
        qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void
TestOpenDataExport::concurrentWaitAndOwnerDeliveryJoinOnce()
{
    QSemaphore started;
    QSemaphore releaseTask;
    const auto task =
        [&](const Request &,
            const CancellationCheck &,
            const ProgressCallback &) {
            started.release();
            releaseTask.acquire();
            return UploadResult::succeeded();
        };
    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    std::atomic<bool> waiterDone{false};
    std::atomic<bool> waiterSucceeded{false};

    worker.start();
    QVERIFY(started.tryAcquire(1, 2000));
    std::thread waiter([&]() {
        waiterSucceeded.store(
            worker.wait(2000), std::memory_order_release);
        waiterDone.store(true, std::memory_order_release);
    });
    releaseTask.release();

    QTRY_VERIFY_WITH_TIMEOUT(
        waiterDone.load(std::memory_order_acquire), 3000);
    waiter.join();
    QVERIFY(waiterSucceeded.load(std::memory_order_acquire));
    QTRY_COMPARE_WITH_TIMEOUT(succeeded.count(), 1, 2000);
}

void
TestOpenDataExport::nativeWorkerRunsQtNetworkEventLoop()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(
        &server, &QTcpServer::newConnection,
        &server,
        [&server]() {
            QTcpSocket *socket = server.nextPendingConnection();
            if (!socket) return;
            socket->write(
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n"
                "\r\n"
                "OK");
            socket->disconnectFromHost();
        });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));
    const auto task =
        [url](const Request &,
              const CancellationCheck &cancelled,
              const ProgressCallback &) {
            QNetworkAccessManager manager;
            std::unique_ptr<QNetworkReply> reply(
                manager.get(QNetworkRequest(url)));
            const NetworkReplyWaitResult waitResult =
                waitForNetworkReply(reply.get(), 2000, cancelled);
            if (waitResult == NetworkReplyWaitResult::Interrupted)
                return UploadResult::cancelled();
            if (waitResult != NetworkReplyWaitResult::Finished
                || reply->error() != QNetworkReply::NoError
                || reply->readAll() != QByteArrayLiteral("OK")) {
                return UploadResult::failed(
                    QStringLiteral("Local request failed"));
            }
            return UploadResult::succeeded();
        };

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(succeeded.count(), 1, 3000);
    QVERIFY(worker.wait(2000));
}

void
TestOpenDataExport::cancellationAbortsNativeWorkerNetworkWait()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QSignalSpy connectionAccepted(&server, &QTcpServer::newConnection);
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));
    const auto task =
        [url](const Request &,
              const CancellationCheck &cancelled,
              const ProgressCallback &) {
            QNetworkAccessManager manager;
            std::unique_ptr<QNetworkReply> reply(
                manager.get(QNetworkRequest(url)));
            const NetworkReplyWaitResult waitResult =
                waitForNetworkReply(reply.get(), 30000, cancelled);
            return waitResult == NetworkReplyWaitResult::Interrupted
                ? UploadResult::cancelled()
                : UploadResult::failed(
                      QStringLiteral("Request was not cancelled"));
        };

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(exampleRequest()), task);
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(connectionAccepted.count(), 1, 3000);
    QElapsedTimer elapsed;
    elapsed.start();
    worker.requestCancellation();
    QVERIFY(worker.wait(2000));

    QVERIFY2(elapsed.elapsed() < 1000,
             "Network cancellation did not stop promptly");
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 0);

    while (server.hasPendingConnections())
        delete server.nextPendingConnection();
}

QTEST_MAIN(TestOpenDataExport)
#include "testOpenDataExport.moc"
