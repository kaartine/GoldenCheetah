/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "CloudAutoDownloadTestSupport.h"

#include "Cloud/CloudService.h"
#include "Cloud/LocalFileStore.h"
#include "Cloud/LocalFileStoreProcess.h"
#include "Cloud/MeasuresDownload.h"
#include "Cloud/NetworkReplyWait.h"
#include "Cloud/NolioTokenRefresh.h"
#include "Cloud/OAuthPKCE.h"
#include "Cloud/SportsPlusHealth.h"
#include "Cloud/TredictMeasuresDownload.h"
#include "Cloud/TrainingsTageBuch.h"
#include "Cloud/WithingsDownload.h"
#include "Core/Athlete.h"
#include "Core/GcUpgrade.h"
#include "Core/Settings.h"
#include "Train/Library.h"
#include "Train/TrainDB.h"
#include "../../../contrib/qzip/zipwriter.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QHostAddress>
#include <QSignalSpy>
#include <QSslError>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <zlib.h>

void resetAthleteMigrationTestSettings();
void setAthleteMigrationThrowOnIdWrite(bool enabled);
void setAthleteMigrationThrowOnRideCacheConstruction(bool enabled);
void setAthleteMigrationEmitRideCacheLoadComplete(bool enabled);
void setAthleteMigrationThrowOnChartLoad(bool enabled);
void setAthleteMigrationMeasuresWriteFails(bool enabled);
void setAthleteMigrationIncludeHrvMeasuresGroup(bool enabled);
Context *createAthleteMigrationTestContext();
int athleteMigrationRideFileOpenCalls();
QList<QByteArray> athleteMigrationRideFilePayloads();
void configureAthleteMigrationTrainDbUpgrade(
    TrainDB *database,
    const TrainDB::LegacyMigrationPlan &plan,
    const LibraryImportResult &result);
int athleteMigrationLibraryImportCalls();
QStringList athleteMigrationImportedFiles();
int athleteMigrationLibraryInitialiseCalls();
bool athleteMigrationLibraryInitialisedBeforeImport();
int athleteMigrationLegacyFinalizeCalls();
int athleteMigrationLegacyDropCalls();
bool athleteMigrationFinalizedExpectedImport();
int athleteMigrationCalendarDownloadConstructionCalls();
int athleteMigrationRideCacheCancelCalls();
int athleteMigrationRideCacheRefreshCalls();

namespace {

constexpr char ReaperAffinityChildSwitch[] =
        "--gc-test-local-store-reaper-affinity-child";
constexpr char ReaperShutdownRaceChildSwitch[] =
        "--gc-test-local-store-reaper-shutdown-race-child";
constexpr char ReaperDrainDeadlineChildSwitch[] =
        "--gc-test-local-store-reaper-drain-deadline-child";
constexpr char ReaperRetainedRetryChildSwitch[] =
        "--gc-test-local-store-reaper-retained-retry-child";
constexpr char ReaperFailedShutdownStateChildSwitch[] =
        "--gc-test-local-store-reaper-failed-shutdown-state-child";
constexpr char ReaperDispatchRaceChildSwitch[] =
        "--gc-test-local-store-reaper-dispatch-race-child";
constexpr char DeferredLoadExceptionChildSwitch[] =
        "--gc-test-deferred-athlete-load-exception-child";
constexpr char ManualMeasuresDeletionChildSwitch[] =
        "--gc-test-manual-measures-deletion-child";

class EmptyQtThread final : public QThread
{
protected:
    void run() override {}
};

class SslWarningThread final : public QThread
{
public:
    void release() { completionGate.release(); }

protected:
    void run() override
    {
        CloudService::sslErrors(
            nullptr, nullptr,
            {QSslError(QSslError::SelfSignedCertificate)});
        completionGate.acquire();
    }

private:
    QSemaphore completionGate;
};

class SslWarningFloodThread final : public QThread
{
public:
    SslWarningFloodThread(int duplicateCount, int distinctCount)
        : duplicateCount(duplicateCount), distinctCount(distinctCount)
    {
    }

protected:
    void run() override
    {
        const QSslError duplicate(QSslError::SelfSignedCertificate);
        for (int warning = 0; warning < duplicateCount; ++warning) {
            CloudService::sslErrors(nullptr, nullptr, {duplicate});
        }
        for (int warning = 0; warning < distinctCount; ++warning) {
            QList<QSslError> errors;
            const int errorCount = warning + 2;
            errors.reserve(errorCount);
            for (int error = 0; error < errorCount; ++error) {
                errors.append(duplicate);
            }
            CloudService::sslErrors(nullptr, nullptr, errors);
        }
    }

private:
    int duplicateCount;
    int distinctCount;
};

class OAuthRefreshThread final : public QThread
{
public:
    struct Result
    {
        bool succeeded = false;
        QString error;
        QString accessToken;
        QString refreshToken;
        int expiresIn = 0;
    };

    explicit OAuthRefreshThread(QString endpoint, int timeoutMs = -1)
        : endpoint(std::move(endpoint)),
          timeoutMs(timeoutMs)
    {
    }

    Result result() const
    {
        const std::lock_guard<std::mutex> lock(resultMutex);
        return resultValue;
    }

protected:
    void run() override
    {
        QString accessToken;
        QString refreshToken;
        int expiresIn = 0;
        QString error;
        const bool succeeded = timeoutMs >= 0
            ? OAuthPKCE::refreshAccessTokenWithTimeout(
                  endpoint, QStringLiteral("client"),
                  QStringLiteral("refresh"), accessToken, refreshToken,
                  expiresIn, error, timeoutMs)
            : OAuthPKCE::refreshAccessToken(
                  endpoint, QStringLiteral("client"),
                  QStringLiteral("refresh"), accessToken, refreshToken,
                  expiresIn, error);

        const std::lock_guard<std::mutex> lock(resultMutex);
        resultValue = {
            succeeded,
            std::move(error),
            std::move(accessToken),
            std::move(refreshToken),
            expiresIn};
    }

private:
    QString endpoint;
    int timeoutMs;
    mutable std::mutex resultMutex;
    Result resultValue;
};

class NolioRefreshThread final : public QThread
{
public:
    NolioRefreshThread(
            QString inputToken,
            std::atomic<int> &operationCalls,
            QSemaphore &operationStarted,
            QSemaphore &releaseOperation)
        : inputToken(std::move(inputToken)),
          operationCalls(operationCalls),
          operationStarted(operationStarted),
          releaseOperation(releaseOperation)
    {
    }

    NolioTokenRefreshResult result() const
    {
        const std::lock_guard<std::mutex> lock(resultMutex);
        return resultValue;
    }

protected:
    void run() override
    {
        NolioTokenRefreshResult result = NolioTokenRefreshCoordinator::refresh(
            inputToken,
            [this]() {
                const int call =
                        operationCalls.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                operationStarted.release();
                releaseOperation.acquire();
                NolioTokenRefreshResult result;
                result.success = true;
                result.accessToken =
                        QStringLiteral("access-%1").arg(call);
                result.refreshToken =
                        QStringLiteral("refresh-%1").arg(call);
                result.refreshedAt = QStringLiteral("now");
                return result;
            },
            [this]() {
                return isInterruptionRequested();
            });

        const std::lock_guard<std::mutex> lock(resultMutex);
        resultValue = std::move(result);
    }

private:
    QString inputToken;
    std::atomic<int> &operationCalls;
    QSemaphore &operationStarted;
    QSemaphore &releaseOperation;
    mutable std::mutex resultMutex;
    NolioTokenRefreshResult resultValue;
};

class AbortProbeReply final : public QNetworkReply
{
public:
    explicit AbortProbeReply(QObject *parent)
        : QNetworkReply(parent)
    {
        setOpenMode(QIODevice::ReadOnly);
    }

    void abort() override
    {
        ++abortCount;
        setFinished(true);
    }

    int abortCalls() const { return abortCount; }
    void finish() { setFinished(true); }

protected:
    qint64 readData(char *, qint64) override { return -1; }

private:
    int abortCount = 0;
};

class SequencedHttpServer
{
public:
    struct Response {
        QByteArray body;
        int delayMs = 0;
    };

    explicit SequencedHttpServer(QList<Response> responses)
        : responses(std::move(responses))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(
            &server, &QTcpServer::newConnection, &server,
            [this]() {
                while (server.hasPendingConnections()) {
                    QTcpSocket *socket =
                            server.nextPendingConnection();
                    QObject::connect(
                        socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                    QObject::connect(
                        socket, &QTcpSocket::readyRead,
                        socket, [this, socket]() {
                            QByteArray request =
                                    socket->property(
                                        "gcRequest").toByteArray();
                            request.append(socket->readAll());
                            socket->setProperty("gcRequest", request);
                            const int headerEnd =
                                request.indexOf("\r\n\r\n");
                            if (socket->property(
                                    "gcResponded").toBool()
                                || headerEnd < 0) {
                                return;
                            }

                            int contentLength = 0;
                            for (const QByteArray &line :
                                 request.left(headerEnd).split('\n')) {
                                const QByteArray header =
                                    line.trimmed();
                                const QByteArray prefix =
                                    QByteArrayLiteral(
                                        "Content-Length:");
                                if (header.left(prefix.size()).compare(
                                        prefix,
                                        Qt::CaseInsensitive) == 0) {
                                    contentLength = header.mid(
                                        prefix.size()).trimmed().toInt();
                                }
                            }
                            if (request.size()
                                < headerEnd + 4 + contentLength) {
                                return;
                            }
                            socket->setProperty("gcResponded", true);

                            const QList<QByteArray> requestLine =
                                    request.left(
                                        request.indexOf("\r\n"))
                                    .split(' ');
                            if (requestLine.size() >= 2) {
                                paths.append(QString::fromUtf8(
                                    requestLine.at(1)));
                            }
                            requests.append(request);

                            const int index = requestCountValue++;
                            const Response response =
                                    index < this->responses.size()
                                    ? this->responses.at(index)
                                    : Response{
                                        QByteArrayLiteral("{}"), 0};
                            QPointer<QTcpSocket> guardedSocket(socket);
                            QTimer::singleShot(
                                response.delayMs, socket,
                                [guardedSocket, response]() {
                                    if (!guardedSocket) return;
                                    const QByteArray message =
                                            QByteArrayLiteral(
                                                "HTTP/1.1 200 OK\r\n"
                                                "Content-Type: "
                                                "application/json\r\n"
                                                "Content-Length: ")
                                            + QByteArray::number(
                                                response.body.size())
                                            + QByteArrayLiteral(
                                                "\r\nConnection: "
                                                "close\r\n\r\n")
                                            + response.body;
                                    guardedSocket->write(message);
                                    guardedSocket->flush();
                                    guardedSocket->
                                        disconnectFromHost();
                                });
                        });
                }
            });
    }

    bool isListening() const { return server.isListening(); }
    int requestCount() const { return requestCountValue; }
    QStringList requestPaths() const { return paths; }
    QList<QByteArray> requestBytes() const { return requests; }
    QUrl endpoint(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2")
            .arg(server.serverPort()).arg(path));
    }

private:
    QTcpServer server;
    QList<Response> responses;
    QStringList paths;
    QList<QByteArray> requests;
    int requestCountValue = 0;
};

class LocalFileStoreRunThread final : public QThread
{
public:
    LocalFileStoreRunThread(QString root, int timeoutMs)
        : root(std::move(root)), timeoutMs(timeoutMs)
    {
    }

    LocalFileStoreProcessResult result() const
    {
        return resultValue;
    }

protected:
    void run() override
    {
        resultValue = LocalFileStoreProcess::run(
            LocalFileStoreProcess::Operation::Open,
            root, QString(), timeoutMs);
    }

private:
    QString root;
    int timeoutMs;
    LocalFileStoreProcessResult resultValue;
};

class ReaperOwnerProbeThread final : public QThread
{
public:
    QThread *ownerThread() const
    {
        return owner.load(std::memory_order_acquire);
    }

protected:
    void run() override
    {
        owner.store(
            LocalFileStoreProcess::reaperOwnerThreadForTest(),
            std::memory_order_release);
    }

private:
    std::atomic<QThread *> owner{nullptr};
};

int runReaperAffinityChild(QApplication &application)
{
    ReaperOwnerProbeThread probe;
    probe.start();
    if (!probe.wait(5000)) {
        probe.requestInterruption();
        probe.wait();
        return 2;
    }
    return probe.ownerThread() == application.thread() ? 0 : 1;
}

int runReaperShutdownRaceChild()
{
#ifndef Q_OS_UNIX
    return 0;
#else
    QTemporaryDir store;
    if (!store.isValid()) return 10;

    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS",
        QByteArrayLiteral("5000"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD",
        QByteArrayLiteral("1"));
    LocalFileStoreProcess::prepareReaperAdoptPauseForTest();

    LocalFileStoreRunThread worker(store.path(), 100);
    worker.start();
    if (!LocalFileStoreProcess::waitForReaperAdoptPauseForTest(
            3000)) {
        worker.requestInterruption();
        LocalFileStoreProcess::releaseReaperAdoptForTest();
        worker.wait(5000);
        return 11;
    }

    std::atomic_bool adoptReleased{false};
    std::thread releaser([&]() {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1500));
        LocalFileStoreProcess::releaseReaperAdoptForTest();
        adoptReleased.store(true, std::memory_order_release);
    });

    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    const bool shutdownSucceeded =
            LocalFileStoreProcess::shutdownReaper();
    const qint64 shutdownElapsedMs = shutdownTimer.elapsed();
    const bool returnedBeforeRelease =
            !adoptReleased.load(std::memory_order_acquire);

    const int startsBeforeRejectedRun =
            LocalFileStoreProcess::helperProcessStartCountForTest();
    LocalFileStoreRunThread rejectedWorker(store.path(), 100);
    rejectedWorker.start();
    bool rejectedWorkerJoined = rejectedWorker.wait(1000);
    const int startsAfterRejectedRun =
            LocalFileStoreProcess::helperProcessStartCountForTest();

    releaser.join();
    const bool workerJoined = worker.wait(5000);
    bool preAdmittedProcessOwned = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (LocalFileStoreProcess::reaperProcessCountForTest()
            == 1) {
            preAdmittedProcessOwned = true;
            break;
        }
        QTest::qWait(10);
    }
    if (!rejectedWorkerJoined) {
        rejectedWorker.requestInterruption();
        rejectedWorkerJoined = rejectedWorker.wait(5000);
    }
    const QString rejectedError = rejectedWorkerJoined
        ? rejectedWorker.result().error
        : QString();

    qunsetenv("GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD");

    const bool cleanupSucceeded =
            LocalFileStoreProcess::shutdownReaper();

    if (!workerJoined) return 12;
    if (shutdownSucceeded) return 13;
    if (!returnedBeforeRelease) return 14;
    if (shutdownElapsedMs >= 1300) return 15;
    if (!cleanupSucceeded) return 16;
    if (!rejectedWorkerJoined) return 18;
    if (!preAdmittedProcessOwned) return 21;
    if (rejectedError != QStringLiteral("helper-reaper-unavailable")) {
        return 19;
    }
    if (startsAfterRejectedRun != startsBeforeRejectedRun) return 20;
    if (LocalFileStoreProcess::reaperProcessCountForTest() != 0)
        return 17;
    return 0;
#endif
}

int runReaperDrainDeadlineChild()
{
#ifndef Q_OS_UNIX
    return 0;
#else
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_DRAIN_DELAY_MS",
        QByteArrayLiteral("750"));
    QElapsedTimer timer;
    timer.start();
    const bool drained =
            LocalFileStoreProcess::drainReaperForTest(100);
    const qint64 elapsedMs = timer.elapsed();
    qunsetenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_DRAIN_DELAY_MS");

    QTest::qWait(800);
    const bool cleanupDrained =
            LocalFileStoreProcess::drainReaperForTest(1000);
    if (drained) return 20;
    if (elapsedMs >= 500) return 21;
    if (!cleanupDrained) return 22;
    return 0;
#endif
}

int runReaperRetainedRetryChild()
{
#ifndef Q_OS_UNIX
    return 0;
#else
    QTemporaryDir store;
    if (!store.isValid()) return 30;

    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS",
        QByteArrayLiteral("1000"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_REGISTRATION_RETRY_ONCE",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD",
        QByteArrayLiteral("1"));
    const LocalFileStoreProcessResult transferred =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                store.path(), QString(), 100);
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER");

    bool registrationRetried = false;
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (LocalFileStoreProcess::
                reaperRegistrationAttemptCountForTest() >= 2) {
            registrationRetried = true;
            break;
        }
        QThread::msleep(10);
    }
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD");
    const bool shutdownSucceeded =
            LocalFileStoreProcess::shutdownReaper();
    qunsetenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_REGISTRATION_RETRY_ONCE");
    if (transferred.status
        != LocalFileStoreProcessResult::Status::Failed) {
        return 31;
    }
    if (transferred.error
        != QStringLiteral("helper-termination-failed")) {
        return 32;
    }
    if (!registrationRetried) return 33;
    if (!shutdownSucceeded) return 34;
    return 0;
#endif
}

int runReaperFailedShutdownStateChild()
{
#ifndef Q_OS_UNIX
    return 0;
#else
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_FINAL_DIRTY",
        QByteArrayLiteral("1"));
    const bool firstShutdown =
            LocalFileStoreProcess::shutdownReaper();
    const bool markedStopped =
            LocalFileStoreProcess::reaperIsStoppedForTest();
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_REAPER_FINAL_DIRTY");
    const bool retryShutdown =
            LocalFileStoreProcess::shutdownReaper();

    if (firstShutdown) return 40;
    if (markedStopped) return 41;
    if (!retryShutdown) return 42;
    return 0;
#endif
}

int runReaperDispatchRaceChild()
{
#ifndef Q_OS_UNIX
    return 0;
#else
    LocalFileStoreProcess::prepareReaperDispatchPauseForTest();
    bool drainResult = true;
    std::thread drainer([&drainResult]() {
        drainResult =
            LocalFileStoreProcess::drainReaperForTest(500);
    });
    if (!LocalFileStoreProcess::waitForReaperDispatchPauseForTest(
            1000)) {
        LocalFileStoreProcess::releaseReaperDispatchForTest();
        drainer.join();
        return 50;
    }

    LocalFileStoreProcess::stopReaperEventLoopForTest();
    if (!LocalFileStoreProcess::waitForReaperTeardownProbeForTest(
            1000)) {
        LocalFileStoreProcess::releaseReaperDispatchForTest();
        drainer.join();
        return 51;
    }
    LocalFileStoreProcess::releaseReaperDispatchForTest();
    drainer.join();

    const bool threadStopped =
            LocalFileStoreProcess::waitForReaperThreadForTest(1000);
    const bool lostTarget =
            LocalFileStoreProcess::reaperDispatchLostTargetForTest();
    const bool shutdownSucceeded =
            LocalFileStoreProcess::shutdownReaper();
    if (!threadStopped) return 52;
    if (lostTarget) return 53;
    if (drainResult) return 54;
    if (!shutdownSucceeded) return 55;
    return 0;
#endif
}

std::atomic<int> unclassifiedCloudOpenCalls{0};

class UnclassifiedCloudService final : public CloudService
{
public:
    explicit UnclassifiedCloudService(Context *context)
        : CloudService(context)
    {
    }

    CloudService *clone(Context *context) override
    {
        return new UnclassifiedCloudService(context);
    }

    QString id() const override
    {
        return QStringLiteral("Unclassified Cloud");
    }

    QString uiName() const override
    {
        return QStringLiteral("Unclassified Cloud");
    }

    QImage logo() const override { return {}; }

    bool open(QStringList &) override
    {
        unclassifiedCloudOpenCalls.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
};

void registerUnclassifiedCloudService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId = QStringLiteral("Unclassified Cloud");
    if (!factory.service(serviceId)) {
        factory.addService(new UnclassifiedCloudService(nullptr));
    }
}

std::atomic<int> uploadOnlyCloudOpenCalls{0};

class UploadOnlyCloudService final : public CloudService
{
public:
    explicit UploadOnlyCloudService(Context *context)
        : CloudService(context)
    {
    }

    CloudService *clone(Context *context) override
    {
        return new UploadOnlyCloudService(context);
    }

    AutoDownloadExecution autoDownloadExecution() const override
    {
        return AutoDownloadExecution::Cooperative;
    }

    QString id() const override
    {
        return QStringLiteral("Upload Only Cloud");
    }

    QString uiName() const override
    {
        return QStringLiteral("Upload Only Cloud");
    }

    QImage logo() const override { return {}; }

    int capabilities() const override { return Upload | Query; }

    bool open(QStringList &) override
    {
        uploadOnlyCloudOpenCalls.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
};

void registerUploadOnlyCloudService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId = QStringLiteral("Upload Only Cloud");
    if (!factory.service(serviceId)) {
        factory.addService(new UploadOnlyCloudService(nullptr));
    }
}

class MeasureConfigurationService final : public CloudService
{
public:
    MeasureConfigurationService()
        : CloudService(nullptr) {}

    CloudService *clone(Context *) override
    {
        return new MeasureConfigurationService();
    }

    QString id() const override
    {
        return QStringLiteral("Measure Configuration");
    }

    QString uiName() const override
    {
        return QStringLiteral("Measure Configuration");
    }

    QImage logo() const override { return {}; }

    int type() const override { return Measures; }
    int capabilities() const override { return Download; }
};

void registerWithingsMeasureProbeService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    if (factory.service(QStringLiteral("Withings"))) return;

    class WithingsMeasureProbeService final : public CloudService
    {
    public:
        explicit WithingsMeasureProbeService(Context *context)
            : CloudService(context) {}

        CloudService *clone(Context *context) override
        {
            return new WithingsMeasureProbeService(context);
        }
        QString id() const override
            { return QStringLiteral("Withings"); }
        QString uiName() const override
            { return QStringLiteral("Withings"); }
        QImage logo() const override { return {}; }
        int type() const override { return Measures; }
        int capabilities() const override { return Download; }
        StartupMeasuresExecution
            startupMeasuresExecution() const override
        {
            return StartupMeasuresExecution::Withings;
        }
    };

    factory.addService(new WithingsMeasureProbeService(nullptr));
}

class AbortProbeService final : public CloudService
{
public:
    AbortProbeService() : CloudService(nullptr) {}

    CloudService *clone(Context *) override
    {
        return new AbortProbeService();
    }

    QString id() const override
    {
        return QStringLiteral("AbortProbe");
    }

    QImage logo() const override { return {}; }
};

QByteArray gzipPayload(const QByteArray &source)
{
    z_stream stream = {};
    if (deflateInit2(
            &stream, Z_BEST_COMPRESSION, Z_DEFLATED,
            15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    QByteArray output(
        int(compressBound(uLong(source.size())) + 32), '\0');
    stream.avail_in = uInt(source.size());
    stream.next_in = reinterpret_cast<Bytef *>(
        const_cast<char *>(source.constData()));
    stream.avail_out = uInt(output.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());

    const int result = deflate(&stream, Z_FINISH);
    const int outputSize = int(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) return {};

    output.resize(outputSize);
    return output;
}

QByteArray zipPayload(const QByteArray &source)
{
    QTemporaryFile archive;
    if (!archive.open()) return {};

    {
        ZipWriter writer(&archive);
        writer.addFile(QStringLiteral("activity.fit"), source);
        writer.close();
    }
    QFile completedArchive(archive.fileName());
    if (!completedArchive.open(QIODevice::ReadOnly)) return {};
    return completedArchive.readAll();
}

class SeededAthleteStorage
{
public:
    SeededAthleteStorage()
    {
        storage.fill(std::byte{0xa5});
    }

    ~SeededAthleteStorage()
    {
        if (athlete) athlete->~Athlete();
    }

    Athlete *construct(Context *context, const QDir &home)
    {
        Q_ASSERT(!athlete);
        athlete = ::new (storage.data()) Athlete(context, home);
        return athlete;
    }

    void destroy()
    {
        Q_ASSERT(athlete);
        athlete->~Athlete();
        athlete = nullptr;
        destructionCompleted = true;
    }

    bool constructed() const { return athlete != nullptr; }
    bool destroyed() const { return destructionCompleted; }

private:
    alignas(Athlete) std::array<std::byte, sizeof(Athlete)> storage;
    Athlete *athlete = nullptr;
    bool destructionCompleted = false;
};

template<typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QTest::qWait(5);
    }
    return predicate();
}

enum class ModalResponseType {
    Dialog,
    MessageBox
};

struct ModalResponse
{
    ModalResponseType type;
    int value;

    static ModalResponse dialog(int result)
    {
        return { ModalResponseType::Dialog, result };
    }

    static ModalResponse message(QMessageBox::StandardButton button)
    {
        return { ModalResponseType::MessageBox, int(button) };
    }
};

struct ModalSequenceState
{
    QList<ModalResponse> responses;
    int *handled = nullptr;
};

void pollModalSequence(const std::shared_ptr<ModalSequenceState> &state)
{
    QWidget *modal = QApplication::activeModalWidget();
    if (!modal) {
        QTimer::singleShot(0, [state]() { pollModalSequence(state); });
        return;
    }

    const ModalResponse response = state->responses.takeFirst();
    bool answered = false;

    if (response.type == ModalResponseType::Dialog) {
        if (auto *dialog = qobject_cast<QDialog *>(modal)) {
            dialog->done(response.value);
            answered = true;
        }
    } else if (auto *messageBox = qobject_cast<QMessageBox *>(modal)) {
        QAbstractButton *button = messageBox->button(
            QMessageBox::StandardButton(response.value));
        if (button) {
            button->click();
            answered = true;
        }
    }

    if (answered) ++*state->handled;
    if (!state->responses.isEmpty()) {
        QTimer::singleShot(0, [state]() { pollModalSequence(state); });
    }
}

void answerModalSequence(
    std::initializer_list<ModalResponse> responses, int &handled)
{
    handled = 0;
    auto state = std::make_shared<ModalSequenceState>();
    state->responses = QList<ModalResponse>(
        responses.begin(), responses.end());
    state->handled = &handled;
    QTimer::singleShot(0, [state]() { pollModalSequence(state); });
}

void answerNextMessageBox(
    QMessageBox::StandardButton answer, bool &handled)
{
    handled = false;
    QTimer::singleShot(0, [answer, &handled]() {
        auto *messageBox = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (messageBox) {
            QAbstractButton *button = messageBox->button(answer);
            if (button) {
                handled = true;
                button->click();
            }
        }
    });
}

struct DeleteOnMeasuresDialogState
{
    bool active = true;
    bool *dialogShown = nullptr;
    QPointer<Athlete> athlete;
};

void pollDeleteOnMeasuresDialog(
        const std::shared_ptr<DeleteOnMeasuresDialogState> &state)
{
    if (!state->active) return;

    auto *messageBox = qobject_cast<QMessageBox *>(
        QApplication::activeModalWidget());
    if (!messageBox) {
        QTimer::singleShot(
            0, [state]() { pollDeleteOnMeasuresDialog(state); });
        return;
    }

    *state->dialogShown = true;
    state->active = false;
    if (state->athlete) delete state->athlete.data();
    messageBox->accept();
}

std::shared_ptr<DeleteOnMeasuresDialogState>
deleteAthleteOnMeasuresDialog(
        Athlete *athlete, bool &dialogShown)
{
    dialogShown = false;
    auto state = std::make_shared<DeleteOnMeasuresDialogState>();
    state->dialogShown = &dialogShown;
    state->athlete = athlete;
    QTimer::singleShot(
        0, [state]() { pollDeleteOnMeasuresDialog(state); });
    return state;
}


struct GuiMessageResponseState
{
    bool active = true;
    bool *handled = nullptr;
    bool *shownOnGuiThread = nullptr;
};

void pollGuiMessageResponse(
        const std::shared_ptr<GuiMessageResponseState> &state)
{
    if (!state->active) return;

    auto *messageBox = qobject_cast<QMessageBox *>(
        QApplication::activeModalWidget());
    if (!messageBox) {
        QTimer::singleShot(
            0, [state]() { pollGuiMessageResponse(state); });
        return;
    }

    *state->shownOnGuiThread =
            messageBox->thread() == QApplication::instance()->thread();
    if (QAbstractButton *button = messageBox->button(QMessageBox::Ok)) {
        *state->handled = true;
        state->active = false;
        button->click();
    }
}

std::shared_ptr<GuiMessageResponseState>
answerNextMessageBoxOnGuiThread(
        bool &handled, bool &shownOnGuiThread)
{
    handled = false;
    shownOnGuiThread = false;
    auto state = std::make_shared<GuiMessageResponseState>();
    state->handled = &handled;
    state->shownOnGuiThread = &shownOnGuiThread;
    QTimer::singleShot(
        0, [state]() { pollGuiMessageResponse(state); });
    return state;
}

bool createStructuredAthlete(QDir &athleteDir, const QString &name)
{
    if (!athleteDir.mkdir(name) || !athleteDir.cd(name)
        || !athleteDir.mkdir(QStringLiteral("activities"))
        || !athleteDir.mkdir(QStringLiteral("config"))) {
        return false;
    }

    QFile configMarker(
        athleteDir.filePath(QStringLiteral("config/athlete.xml")));
    if (!configMarker.open(QIODevice::WriteOnly)) return false;
    return configMarker.write("test") == 4;
}

void configureAthlete(
    const QDir &athleteDir, int version, bool folderUpgradeComplete)
{
    const QString athlete = athleteDir.dirName();
    appsettings->setCValue(
        athlete, GC_UPGRADE_FOLDER_SUCCESS, folderUpgradeComplete);
    appsettings->setCValue(athlete, GC_VERSION_USED, version);
}


int runDeferredLoadExceptionChild()
{
    resetAthleteMigrationTestSettings();
    QTemporaryDir root;
    if (!root.isValid()) return 30;
    QDir athleteDir(root.path());
    if (!createStructuredAthlete(
            athleteDir,
            QStringLiteral("DeferredLoadException"))) {
        return 31;
    }
    configureAthlete(athleteDir, VERSION_LATEST, true);

    int publications = 0;
    int rollbacks = 0;
    int completions = 0;
    setAthleteMigrationThrowOnChartLoad(true);
    setAthleteMigrationEmitRideCacheLoadComplete(true);
    Context *context = Athlete::createInNewContext(
        nullptr, athleteDir,
        [&publications, &completions](Context *candidate) {
            ++publications;
            QObject::connect(
                candidate, &Context::loadCompleted,
                candidate,
                [&completions](const QString &, Context *) {
                    ++completions;
                });
        },
        [&rollbacks](Context *) { ++rollbacks; });
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;

    const bool released = waitUntil(
        [&guardedContext]() { return guardedContext.isNull(); },
        2000);
    setAthleteMigrationEmitRideCacheLoadComplete(false);
    setAthleteMigrationThrowOnChartLoad(false);
    if (guardedContext) {
        delete guardedContext->athlete;
        delete guardedContext.data();
    }

    if (publications != 1) return 32;
    if (rollbacks != 1) return 33;
    if (completions != 0) return 34;
    if (!guardedAthlete.isNull()) return 35;
    if (!released) return 36;
    return 0;
}

int runManualMeasuresDeletionChild()
{
    resetAthleteMigrationTestSettings();
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    if (!root.isValid()) return 40;
    QDir athleteDir(root.path());
    if (!createStructuredAthlete(
            athleteDir,
            QStringLiteral("ManualMeasuresDeletion"))) {
        return 41;
    }
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    Athlete *athlete = new Athlete(context.get(), athleteDir);
    MeasuresGroup *group =
            athlete->measures->getGroup(Measures::Body);
    if (!group) {
        delete athlete;
        return 42;
    }
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh"));

    bool probeCalled = false;
    bool dialogShown = false;
    bool invoked = false;
    {
        MeasuresDownload dialog(context.get(), group);
        const auto dialogState =
                deleteAthleteOnMeasuresDialog(nullptr, dialogShown);
        MeasuresDownload::setManualDownloadProbeForTest(
            [&probeCalled](
                    Context *probeContext, const QString &,
                    QString &error, const QDateTime &,
                    const QDateTime &, QList<Measure> &) {
                probeCalled = true;
                error = QStringLiteral("cancelled");
                delete probeContext->athlete;
                return false;
            });
        invoked = QMetaObject::invokeMethod(
            &dialog, "download", Qt::DirectConnection);
        MeasuresDownload::setManualDownloadProbeForTest({});
        dialogState->active = false;
    }

    const bool athleteDeleted = context->athlete == nullptr;
    if (context->athlete) delete context->athlete;

    if (!invoked) return 43;
    if (!probeCalled) return 44;
    if (!athleteDeleted) return 45;
    if (dialogShown) return 46;
    return 0;
}
} // namespace

class TestAthleteMigrationSafety : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void folderUpgradeRejectionSkipsAction();
    void folderAcceptanceThenCompatibilityRejectionSkipsAction();
    void allRequiredPromptsAcceptedRunActionOnce();
    void cancelledCompatibilityStopsBeforeAthleteConstruction();
    void acceptedCompatibilityAllowsAthleteConstruction();
    void currentVersionNeedsNoCompatibilityPrompt();
    void newAthleteNeedsNoCompatibilityPrompt();
    void lateUpgradeWithoutTrainDbSucceeds();
    void lateUpgradeFinalizesVerifiedTrainDbMigration();
    void publishedContextRollsBackLateConstructionFailure();
    void constructorFailureRollsBackPublishedContextAndOwners();
    void invalidContextStopsCloudDownloadPromptly();
    void unclassifiedCloudProviderIsSkipped();
    void uploadOnlyCloudProviderIsSkipped();
    void measureProviderRetainsStartupSynchronization();
    void measureProviderAuthorizationUsesRefreshToken();
    void measureProviderDispatchRespectsExecutionContract();
    void constructorDefersLoadCompleteUntilFullyConstructed();
    void factoryCompletesObservedRideCacheBeforeReturning();
    void deferredLoadDeletionRollsBackPublishedContext();
    void nestedMeasuresDownloadDeletionStopsLoadComplete();
    void measuresWriteFailureDoesNotEnterNestedDialog();
    void manualMeasuresDownloadHandlesAthleteDeletion();
    void localFileStoreReaperUsesApplicationThread();
    void deferredLoadExceptionRollsBackPublishedContext();
    void localFileStoreReaperShutdownClosesAdmission();
    void localFileStoreReaperDrainHonorsDeadline();
    void localFileStoreReaperRetriesQuarantinedProcesses();
    void localFileStoreFailedShutdownRemainsRetryable();
    void localFileStoreWorkerDispatchSurvivesThreadExit();
    void localFileStoreRegistrationMatchesPlatform();
    void localFileStoreHelperRoundTrip();
    void localFileStoreHelperRejectsMalformedFrames();
    void localFileStoreRejectsMalformedResponses();
    void localFileStoreManualBrowserUsesAbsolutePaths();
    void localFileStoreManualReadRejectsSpecialFilePromptly();
    void localFileStoreHelperTimeoutIsBounded();
    void localFileStoreFailedTerminationIsReaped();
    void localFileStoreDelayedStartIsBounded();
    void localFileStoreConfinesPathsToConfiguredRoot();
    void athleteTeardownJoinsBlockedCloudDownload();
    void localFileStoreBlockedReadHasBoundedTeardown();
    void synchronousCloudCompletionIsPromptAndThreadIsolated();
    void inlineCloudCompletionDoesNotReenterProvider();
    void queuedCloudResultsSurviveWorkerCleanup();
    void naturalCloudCompletionStopsWithoutGuiDispatch();
    void stalledGuiCoalescesAutoDownloadProgress();
    void stalledGuiBackpressuresDownloadedPayloads();
    void cancellationPurgesBackpressuredGeneration();
    void queuedCloudSettingsPreserveNewerValues();
    void queuedCloudSettingsRejectRemovedExpectedValue();
    void sequentialQueuedCloudSettingsPreserveLatestValues();
    void sequentialQueuedCloudSettingsRejectConflictChain();
    void stalledGuiCoalescesCloudSettingsTransactions();
    void guiCloudSettingsSaveWritesAllScopes();
    void guiCloudSettingsSaveClearsCustomUrl();
    void workerCloudSettingsClearUrlUsesDefault();
    void defaultCloudUrlDoesNotInvalidatePayload();
    void emptyCloudUrlUsesDefault();
    void changedCloudUrlInvalidatesPayload();
    void queuedCloudPayloadIsDiscardedWhenStartupSyncIsDisabled();
    void progressCallbackCanDestroyDownloader();
    void timedOutCloudReadReleasesBuffer();
    void rejectedCloudReadReleasesBufferPromptly();
    void duplicateCloudCompletionIsAppliedOnceAcrossRepeatedRuns();
    void providerCancellationDoesNotDestroyActiveCall_data();
    void providerCancellationDoesNotDestroyActiveCall();
    void cancellationFromStartSignalStopsSafely();
    void staleQueuedResultsAreDiscardedAfterRestart();
    void legacyReadCompleteDoesNotTakeCallerOwnership();
    void noEnabledCloudServiceEmitsNoLifecycleSignals();
    void autoDownloadEndFollowsImportsAndProgress();
    void plainQtThreadLifecycleIsJoined();
    void baseCloudAbortStopsRunningReplies();
    void cloudCompressionModesAreExtracted();
    void decommissionedCloudServicesAreNotRegistered();
    void factoryRejectsDecommissionedCloudServices();
    void decommissionedCloudServicesFailClosed();
    void nolioTokenRefreshIsSingleFlight();
    void nolioTokenRefreshCacheExpires();
    void nolioTokenRefreshFollowerCanCancel();
    void withingsSuccessfulResponseUpdatesTokensAndMeasures();
    void withingsDelayedResponseHandlesAthleteDeletion();
    void withingsTokenRequestTimesOut();
    void withingsMeasuresRequestTimesOut();
    void tredictSuccessfulResponsesUpdateTokensAndEncodeHrvRange();
    void tredictMeasuresRequestTimesOut();
    void tredictTokenRefreshHandlesAthleteDeletion();
    void networkReplyWaitTimesOut();
    void networkReplyWaitPrefersPreexistingInterruption();
    void oauthRefreshHandlesImmediateReply();
    void oauthRefreshTimesOut();
    void oauthRefreshHonorsThreadInterruption();
    void sslWarningsAreMarshaledToGuiThread();
    void stalledGuiBoundsAndCoalescesSslWarnings();
};

void TestAthleteMigrationSafety::init()
{
    resetCloudServiceHandoffQueuesForTest();
    disableCloudSslWarningCapture();
    resetAthleteMigrationTestSettings();
}

void TestAthleteMigrationSafety::folderUpgradeRejectionSkipsAction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("FolderReject")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        { ModalResponse::dialog(QDialog::Rejected) }, handled);

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 1);
    QCOMPARE(actionCount, 0);
}

void TestAthleteMigrationSafety::
folderAcceptanceThenCompatibilityRejectionSkipsAction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CompatibilityReject")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        {
            ModalResponse::dialog(QDialog::Accepted),
            ModalResponse::message(QMessageBox::Cancel)
        },
        handled);

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 2);
    QCOMPARE(actionCount, 0);
}

void TestAthleteMigrationSafety::allRequiredPromptsAcceptedRunActionOnce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("AllAccepted")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        {
            ModalResponse::dialog(QDialog::Accepted),
            ModalResponse::message(QMessageBox::Ok)
        },
        handled);

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 2);
    QCOMPARE(actionCount, 1);
}

void TestAthleteMigrationSafety::
cancelledCompatibilityStopsBeforeAthleteConstruction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LegacyAthleteCancel")));
    configureAthlete(athleteDir, VERSION35_BUILD, true);

    bool promptHandled = false;
    answerNextMessageBox(QMessageBox::Cancel, promptHandled);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() { athleteStorage.construct(context.get(), athleteDir); }));

    QVERIFY(promptHandled);
    QVERIFY(!athleteStorage.constructed());
    QVERIFY(!athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::
acceptedCompatibilityAllowsAthleteConstruction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LegacyAthleteAccept")));
    configureAthlete(athleteDir, VERSION35_BUILD, true);

    bool promptHandled = false;
    answerNextMessageBox(QMessageBox::Ok, promptHandled);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    Athlete *athleteInstance = nullptr;
    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() {
            athleteInstance =
                athleteStorage.construct(context.get(), athleteDir);
        }));
    QVERIFY(promptHandled);
    QCOMPARE(context->athlete, athleteInstance);

    athleteStorage.destroy();
    QVERIFY(athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::currentVersionNeedsNoCompatibilityPrompt()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CurrentAthlete")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    bool unexpectedPrompt = false;
    answerNextMessageBox(QMessageBox::Cancel, unexpectedPrompt);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() { athleteStorage.construct(context.get(), athleteDir); }));
    QCoreApplication::processEvents();
    QVERIFY(!unexpectedPrompt);
    QVERIFY(athleteStorage.constructed());

    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::newAthleteNeedsNoCompatibilityPrompt()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("NewAthlete")));
    configureAthlete(athleteDir, 0, true);

    bool unexpectedPrompt = false;
    answerNextMessageBox(QMessageBox::Cancel, unexpectedPrompt);
    std::unique_ptr<Context> context;
    Context *publishedContext = nullptr;
    int actionCount = 0;
    int publishCount = 0;
    int rollbackCount = 0;

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() {
            ++actionCount;
            context.reset(Athlete::createInNewContext(
                nullptr, athleteDir,
                [&](Context *candidate) {
                    ++publishCount;
                    publishedContext = candidate;
                },
                [&](Context *) { ++rollbackCount; }));
        }));
    QCoreApplication::processEvents();

    QVERIFY(!unexpectedPrompt);
    QCOMPARE(actionCount, 1);
    QCOMPARE(publishCount, 1);
    QCOMPARE(rollbackCount, 0);
    QCOMPARE(publishedContext, context.get());
    QVERIFY(context->athlete);

    delete context->athlete;
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::lateUpgradeWithoutTrainDbSucceeds()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateUpgradeNoTrainDb")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
        athleteStorage.construct(context.get(), athleteDir);
    QCOMPARE(context->athlete, athlete);

    GcUpgrade upgrade;
    QCOMPARE(upgrade.upgradeLate(context.get()), 0);

    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::lateUpgradeFinalizesVerifiedTrainDbMigration()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    gcroot = root.path();
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateUpgradeTrainDb")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    athleteStorage.construct(context.get(), athleteDir);

    TrainDB database(QDir(root.path()));
    TrainDB::LegacyMigrationPlan plan;
    plan.valid = true;
    plan.workouts = {QStringLiteral("/legacy.erg")};
    plan.videos = {QStringLiteral("/legacy.mp4")};
    plan.videoSyncs = {QStringLiteral("/legacy.rlv")};

    LibraryImportResult result;
    result.completed = true;
    result.requestedFiles = plan.files();
    result.importedWorkouts.insert(
        plan.workouts.first(), QStringLiteral("/library/legacy.erg"));
    result.importedVideos.insert(plan.videos.first(), plan.videos.first());
    result.importedVideoSyncs.insert(
        plan.videoSyncs.first(), QStringLiteral("/library/legacy.rlv"));
    configureAthleteMigrationTrainDbUpgrade(&database, plan, result);

    GcUpgrade upgrade;
    QCOMPARE(upgrade.upgradeLate(context.get()), 0);
    QCOMPARE(athleteMigrationLibraryInitialiseCalls(), 1);
    QVERIFY(athleteMigrationLibraryInitialisedBeforeImport());
    QCOMPARE(athleteMigrationLibraryImportCalls(), 1);
    QCOMPARE(athleteMigrationImportedFiles(), plan.files());
    QCOMPARE(athleteMigrationLegacyFinalizeCalls(), 1);
    QCOMPARE(athleteMigrationLegacyDropCalls(), 0);
    QVERIFY(athleteMigrationFinalizedExpectedImport());

    trainDB = nullptr;
    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::
publishedContextRollsBackLateConstructionFailure()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateConstructionFailure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    setAthleteMigrationThrowOnRideCacheConstruction(true);
    Context *publishedContext = nullptr;
    Context *unexpectedContext = nullptr;
    int publishCount = 0;
    int rollbackCount = 0;
    bool rollbackContextMatched = false;
    bool rollbackSawClearedAthlete = false;
    bool exceptionCaught = false;

    try {
        unexpectedContext = Athlete::createInNewContext(
            nullptr, athleteDir,
            [&](Context *candidate) {
                ++publishCount;
                publishedContext = candidate;
            },
            [&](Context *candidate) {
                ++rollbackCount;
                rollbackContextMatched = candidate == publishedContext;
                rollbackSawClearedAthlete = !candidate->athlete;
                publishedContext = nullptr;
            });
    } catch (const std::runtime_error &) {
        exceptionCaught = true;
    }
    setAthleteMigrationThrowOnRideCacheConstruction(false);

    if (unexpectedContext) {
        delete unexpectedContext->athlete;
        delete unexpectedContext;
    }

    QVERIFY(exceptionCaught);
    QCOMPARE(publishCount, 1);
    QCOMPARE(rollbackCount, 1);
    QVERIFY(rollbackContextMatched);
    QVERIFY(rollbackSawClearedAthlete);
    QVERIFY(!publishedContext);
}

void TestAthleteMigrationSafety::
constructorFailureRollsBackPublishedContextAndOwners()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("ThrowingAthlete")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    setAthleteMigrationThrowOnIdWrite(true);

    bool exceptionCaught = false;
    try {
        athleteStorage.construct(context.get(), athleteDir);
    } catch (const std::runtime_error &) {
        exceptionCaught = true;
    }
    setAthleteMigrationThrowOnIdWrite(false);

    QVERIFY(exceptionCaught);
    QVERIFY(!athleteStorage.constructed());
    QVERIFY(!athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::invalidContextStopsCloudDownloadPromptly()
{
    Context *context = createAthleteMigrationTestContext();
    CloudServiceAutoDownload worker(context);
    delete context;

    worker.checkDownload();
    const bool stoppedPromptly = worker.wait(500);
    bool cleanupStopped = stoppedPromptly;
    if (!cleanupStopped) cleanupStopped = worker.wait(5000);

    QVERIFY2(cleanupStopped,
             "cloud worker did not finish during test cleanup");
    QVERIFY2(stoppedPromptly,
             "cloud worker continued after its context became invalid");
}

void TestAthleteMigrationSafety::unclassifiedCloudProviderIsSkipped()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("UnclassifiedCloudProvider")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    registerUnclassifiedCloudService();
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Unclassified Cloud"));
    QVERIFY(service);
    QVERIFY(!service->supportsStartupAutoDownload());
    unclassifiedCloudOpenCalls.store(0, std::memory_order_relaxed);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));

    athlete->cloudAutoDownload->checkDownload();
    QVERIFY2(
        athlete->cloudAutoDownload->wait(2000),
        "unclassified provider worker did not stop");
    athleteStorage.destroy();

    QCOMPARE(unclassifiedCloudOpenCalls.load(std::memory_order_relaxed), 0);
}

void TestAthleteMigrationSafety::uploadOnlyCloudProviderIsSkipped()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("UploadOnlyCloudProvider")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    registerUploadOnlyCloudService();
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Upload Only Cloud"));
    QVERIFY(service);
    QVERIFY(!service->supportsStartupAutoDownload());
    uploadOnlyCloudOpenCalls.store(0, std::memory_order_relaxed);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));

    athlete->cloudAutoDownload->checkDownload();
    QVERIFY2(
        athlete->cloudAutoDownload->wait(2000),
        "upload-only provider worker did not stop");
    QCOMPARE(uploadOnlyCloudOpenCalls.load(
                 std::memory_order_relaxed), 0);

    std::unique_ptr<CloudService> configured(
        CloudServiceFactory::instance().newService(
            QStringLiteral("Upload Only Cloud"), context.get()));
    QVERIFY(configured);
    QCOMPARE(
        configured->getSetting(
            configured->syncOnStartupSettingName()).toString(),
        QStringLiteral("true"));
    configured->setSetting(
        configured->syncOnStartupSettingName(),
        QStringLiteral("false"));
    CloudServiceFactory::instance().saveSettings(
        configured.get(), context.get());
    QCOMPARE(appsettings->cvalue(
        athlete->cyclist, service->syncOnStartupSettingName()).toString(),
        QStringLiteral("false"));
    athleteStorage.destroy();
}

void TestAthleteMigrationSafety::
measureProviderRetainsStartupSynchronization()
{
    MeasureConfigurationService incompleteService;
    QVERIFY(!incompleteService.supportsStartupAutoDownload());

    registerWithingsMeasureProbeService();
    const CloudService *withings =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(withings);
    QVERIFY(withings->supportsStartupAutoDownload());
}

void TestAthleteMigrationSafety::
measureProviderAuthorizationUsesRefreshToken()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("MeasureRefreshAuthorization")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), athleteDir));
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));

    int calls = 0;
    MeasuresDownload::setAutoDownloadProbeForTest(
        [&calls](Context *, const QString &, QString &,
                 const QDateTime &, const QDateTime &,
                 QList<Measure> &) {
            ++calls;
            return false;
        });

    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_TOKEN, QString());
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh-only"));
    MeasuresDownload::autoDownload(context.get());
    QVERIFY2(calls > 0,
             "refresh-only authorization was skipped");

    const int refreshAuthorizedCalls = calls;
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN, QString());
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_TOKEN,
        QStringLiteral("stale-access"));
    MeasuresDownload::autoDownload(context.get());
    QCOMPARE(calls, refreshAuthorizedCalls);

    MeasuresDownload::setAutoDownloadProbeForTest({});
}

void TestAthleteMigrationSafety::
measureProviderDispatchRespectsExecutionContract()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("MeasureProviderDispatch")));
    setAthleteMigrationIncludeHrvMeasuresGroup(true);
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    std::unique_ptr<Athlete> athlete(
        new Athlete(context.get(), athleteDir));
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_TOKEN,
        QStringLiteral("access"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh"));

    QStringList groups;
    MeasuresDownload::setAutoDownloadProbeForTest(
        [&groups](Context *, const QString &group, QString &,
                  const QDateTime &, const QDateTime &,
                  QList<Measure> &) {
            groups.append(group);
            return false;
        });

    MeasuresDownload::autoDownload(context.get());
    MeasuresDownload::setAutoDownloadProbeForTest({});

    QCOMPARE(groups, QStringList{QStringLiteral("Body")});
}

void TestAthleteMigrationSafety::
constructorDefersLoadCompleteUntilFullyConstructed()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("ConstructorLoadComplete")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    const QString cyclist = athleteDir.dirName();
    appsettings->setCValue(
        cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    appsettings->setCValue(
        cyclist, GC_NOKIA_TOKEN,
        QStringLiteral("access"));
    appsettings->setCValue(
        cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh"));

    bool constructorReturned = false;
    bool calledDuringConstruction = false;
    MeasuresDownload::setAutoDownloadProbeForTest(
        [&constructorReturned, &calledDuringConstruction](
                Context *probeContext, const QString &, QString &,
                const QDateTime &, const QDateTime &,
                QList<Measure> &) {
            calledDuringConstruction = !constructorReturned;
            delete probeContext->athlete;
            return true;
        });
    setAthleteMigrationEmitRideCacheLoadComplete(true);
    Athlete::setWaitForRideCacheLoadForTest(true);

    Athlete *athlete = new Athlete(context.get(), athleteDir);
    constructorReturned = true;
    QPointer<Athlete> guardedAthlete(athlete);
    const bool deletedAfterConstruction = waitUntil(
        [&guardedAthlete]() { return guardedAthlete.isNull(); },
        2000);

    Athlete::setWaitForRideCacheLoadForTest(false);
    setAthleteMigrationEmitRideCacheLoadComplete(false);
    MeasuresDownload::setAutoDownloadProbeForTest({});

    QVERIFY2(deletedAfterConstruction,
             "queued load completion did not run");
    QVERIFY(!calledDuringConstruction);
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::
factoryCompletesObservedRideCacheBeforeReturning()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("FactoryInitialLoad")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    setAthleteMigrationEmitRideCacheLoadComplete(true);
    Athlete::setWaitForRideCacheLoadForTest(true);
    Context *context = Athlete::createInNewContext(
        nullptr, athleteDir, {}, {});
    Athlete::setWaitForRideCacheLoadForTest(false);
    setAthleteMigrationEmitRideCacheLoadComplete(false);

    const int calendarConstructions =
            athleteMigrationCalendarDownloadConstructionCalls();
    if (context) {
        delete context->athlete;
        delete context;
    }

    QVERIFY(context);
    QCOMPARE(calendarConstructions, 1);
}

void TestAthleteMigrationSafety::
deferredLoadDeletionRollsBackPublishedContext()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("DeferredLoadRollback")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    appsettings->setCValue(
        athleteDir.dirName(), service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    appsettings->setCValue(
        athleteDir.dirName(), GC_NOKIA_TOKEN,
        QStringLiteral("access"));
    appsettings->setCValue(
        athleteDir.dirName(), GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh"));

    MeasuresDownload::setAutoDownloadProbeForTest(
        [](Context *probeContext, const QString &, QString &,
           const QDateTime &, const QDateTime &,
           QList<Measure> &) {
            delete probeContext->athlete;
            return true;
        });
    setAthleteMigrationEmitRideCacheLoadComplete(true);

    Context *published = nullptr;
    bool rolledBack = false;
    int completions = 0;
    Context *context = Athlete::createInNewContext(
        nullptr, athleteDir,
        [&published, &completions](Context *candidate) {
            published = candidate;
            QObject::connect(
                candidate, &Context::loadCompleted,
                candidate,
                [&completions](const QString &, Context *) {
                    ++completions;
                });
        },
        [&rolledBack, &published](Context *candidate) {
            rolledBack = candidate == published;
        });
    QVERIFY(context);
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete(context->athlete);

    const bool contextReleased = waitUntil(
        [&guardedContext]() { return guardedContext.isNull(); },
        2000);
    setAthleteMigrationEmitRideCacheLoadComplete(false);
    MeasuresDownload::setAutoDownloadProbeForTest({});
    if (guardedContext) delete guardedContext.data();

    QVERIFY(guardedAthlete.isNull());
    QVERIFY(rolledBack);
    QCOMPARE(completions, 0);
    QVERIFY(contextReleased);
}

void TestAthleteMigrationSafety::
deferredLoadExceptionRollsBackPublishedContext()
{
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(DeferredLoadExceptionChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void TestAthleteMigrationSafety::
nestedMeasuresDownloadDeletionStopsLoadComplete()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("NestedMeasuresDeletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    Athlete *athlete = new Athlete(context.get(), athleteDir);
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_TOKEN,
        QStringLiteral("configured-token"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("configured-refresh-token"));

    MeasuresDownload::setAutoDownloadProbeForTest(
        [athlete](Context *, const QString &, QString &,
                  const QDateTime &, const QDateTime &,
                  QList<Measure> &) {
            delete athlete;
            return true;
        });

    athlete->loadComplete();
    MeasuresDownload::setAutoDownloadProbeForTest({});

    QVERIFY(!context->athlete);
    QCOMPARE(
        athleteMigrationCalendarDownloadConstructionCalls(), 0);
}

void TestAthleteMigrationSafety::
measuresWriteFailureDoesNotEnterNestedDialog()
{
    registerWithingsMeasureProbeService();

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("MeasuresWriteFailure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    Athlete *athlete = new Athlete(context.get(), athleteDir);
    QPointer<Athlete> guardedAthlete(athlete);
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Withings"));
    QVERIFY(service);
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_TOKEN,
        QStringLiteral("access"));
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh"));

    MeasuresGroup *body =
            athlete->measures->getGroup(Measures::Body);
    QVERIFY(body);
    Measure previous;
    previous.when =
            QDateTime::currentDateTimeUtc().addDays(-1);
    previous.values[Measure::WeightKg] = 68.0;
    QList<Measure> previousMeasures{previous};
    body->setMeasures(previousMeasures);

    MeasuresDownload::setAutoDownloadProbeForTest(
        [](Context *, const QString &, QString &,
           const QDateTime &, const QDateTime &,
           QList<Measure> &measures) {
            Measure measure;
            measure.when =
                    QDateTime::currentDateTimeUtc().addSecs(-60);
            measure.values[Measure::WeightKg] = 70.0;
            measures.append(measure);
            return true;
        });
    setAthleteMigrationMeasuresWriteFails(true);
    bool dialogShown = false;
    const auto deleteState =
            deleteAthleteOnMeasuresDialog(athlete, dialogShown);

    MeasuresDownload::autoDownload(context.get());

    deleteState->active = false;
    setAthleteMigrationMeasuresWriteFails(false);
    MeasuresDownload::setAutoDownloadProbeForTest({});

    const QList<Measure> persistedMeasures = body->measures();
    const int cancelCalls =
            athleteMigrationRideCacheCancelCalls();
    const int refreshCalls =
            athleteMigrationRideCacheRefreshCalls();

    QVERIFY(!dialogShown);
    QVERIFY(guardedAthlete);
    QCOMPARE(context->athlete, guardedAthlete.data());
    QCOMPARE(persistedMeasures.size(), 1);
    QCOMPARE(persistedMeasures.first().when, previous.when);
    QCOMPARE(
        persistedMeasures.first().values[Measure::WeightKg],
        68.0);
    QCOMPARE(cancelCalls, 1);
    QCOMPARE(refreshCalls, 1);
    delete guardedAthlete.data();
}

void TestAthleteMigrationSafety::
manualMeasuresDownloadHandlesAthleteDeletion()
{
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ManualMeasuresDeletionChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void TestAthleteMigrationSafety::
localFileStoreReaperUsesApplicationThread()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperAffinityChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreReaperShutdownClosesAdmission()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperShutdownRaceChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreReaperDrainHonorsDeadline()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperDrainDeadlineChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreReaperRetriesQuarantinedProcesses()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperRetainedRetryChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreFailedShutdownRemainsRetryable()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperFailedShutdownStateChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreWorkerDispatchSurvivesThreadExit()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store process isolation requires Unix");
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QString::fromLatin1(ReaperDispatchRaceChildSwitch)});
    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.start();

    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
#endif
}

void TestAthleteMigrationSafety::
localFileStoreRegistrationMatchesPlatform()
{
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Local Store"));
#ifdef Q_OS_UNIX
    QVERIFY(LocalFileStore::isSupportedPlatform());
    QVERIFY(service);
    QCOMPARE(
        service->capabilities(),
        int(CloudService::Upload | CloudService::Download
            | CloudService::Query));
#else
    QVERIFY(!LocalFileStore::isSupportedPlatform());
    QVERIFY(!service);

    QTemporaryDir store;
    QVERIFY(store.isValid());

    QFile source(store.filePath(QStringLiteral("source.fit")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("source-data"), qint64(11));
    source.close();

    LocalFileStore unsupported(nullptr);
    unsupported.configuration.insert(
        QString::fromLatin1(GC_NETWORKFILESTORE_FOLDER),
        store.path());

    QCOMPARE(unsupported.capabilities(), 0);
    QCOMPARE(
        unsupported.autoDownloadExecution(),
        CloudService::AutoDownloadExecution::Unsupported);
    QVERIFY(!unsupported.cloneForAutoDownload(nullptr));

    QStringList errors;
    QVERIFY(!unsupported.open(errors));
    QVERIFY(!errors.isEmpty());
    QCOMPARE(unsupported.home(), QString());

    errors.clear();
    QVERIFY(unsupported.readdir(store.path(), errors).isEmpty());
    QVERIFY(!errors.isEmpty());

    QByteArray readData("sentinel");
    QVERIFY(!unsupported.readFile(
        &readData, QStringLiteral("source.fit"), QString()));
    QCOMPARE(readData, QByteArray("sentinel"));

    QByteArray writeData("blocked");
    QVERIFY(!unsupported.writeFile(
        writeData, QStringLiteral("service-write.fit"), nullptr));
    QVERIFY(!QFileInfo::exists(
        store.filePath(QStringLiteral("service-write.fit"))));

    const QString folderPath =
        store.filePath(QStringLiteral("blocked-folder"));
    QVERIFY(!unsupported.createFolder(folderPath));
    QVERIFY(!QFileInfo::exists(folderPath));
    QVERIFY(unsupported.close());

    const LocalFileStoreProcessResult processRead =
            LocalFileStoreProcess::readFileInProcess(
                store.path(), QStringLiteral("source.fit"));
    QVERIFY(!processRead.succeeded());
    QCOMPARE(
        processRead.error,
        QStringLiteral("unsupported-platform"));

    const LocalFileStoreProcessResult processWrite =
            LocalFileStoreProcess::writeFileInProcess(
                store.path(), QStringLiteral("process-write.fit"),
                writeData);
    QVERIFY(!processWrite.succeeded());
    QCOMPARE(
        processWrite.error,
        QStringLiteral("unsupported-platform"));
    QVERIFY(!QFileInfo::exists(
        store.filePath(QStringLiteral("process-write.fit"))));
#endif
}

void TestAthleteMigrationSafety::localFileStoreHelperRoundTrip()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store requires handle-relative Unix file APIs");
#endif
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString sourcePath =
            QDir(store.path()).filePath(QStringLiteral("source.fit"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("source-data"), qint64(11));
    source.close();

    LocalFileStore service(nullptr);
    service.configuration.insert(
        QString::fromLatin1(GC_NETWORKFILESTORE_FOLDER), store.path());
    QCOMPARE(
        service.capabilities(),
        int(CloudService::Upload | CloudService::Download
            | CloudService::Query));
#ifdef Q_OS_UNIX
    QCOMPARE(
        service.autoDownloadExecution(),
        CloudService::AutoDownloadExecution::ProcessIsolated);
#else
    QCOMPARE(
        service.autoDownloadExecution(),
        CloudService::AutoDownloadExecution::Unsupported);
#endif

    QStringList errors;
    QVERIFY(service.open(errors));
    QVERIFY(errors.isEmpty());
    QCOMPARE(service.home(), store.path());

    const QList<CloudServiceEntry *> initialEntries =
            service.readdir(store.path(), errors);
    QVERIFY(errors.isEmpty());
    bool sourceListed = false;
    for (const CloudServiceEntry *entry : initialEntries) {
        if (entry->name == QStringLiteral("source.fit")) {
            sourceListed = true;
            QVERIFY(!entry->isDir);
            QCOMPARE(entry->size, 11UL);
        }
    }
    QVERIFY(sourceListed);

    QByteArray readData;
    QVERIFY(service.readFile(
        &readData, QStringLiteral("source.fit"), QString()));
    QCOMPARE(readData, QByteArray("source-data"));

    QByteArray writeData("written-data");
    QVERIFY(service.writeFile(
        writeData, QStringLiteral("written.fit"), nullptr));
    QFile written(
        QDir(store.path()).filePath(QStringLiteral("written.fit")));
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), writeData);
    written.close();

#ifdef Q_OS_UNIX
    struct stat writtenStatus {};
    const QByteArray encodedWrittenPath =
            QFile::encodeName(written.fileName());
    QVERIFY(::stat(encodedWrittenPath.constData(), &writtenStatus) == 0);
    QCOMPARE(writtenStatus.st_mode & 0777, mode_t(0600));

    const QString existingPath =
            store.filePath(QStringLiteral("existing.fit"));
    QFile existing(existingPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("old"), qint64(3));
    existing.close();
    const QByteArray encodedExistingPath =
            QFile::encodeName(existingPath);
    QVERIFY(::chmod(encodedExistingPath.constData(), 0644) == 0);
    QByteArray replacement("replacement");
    QVERIFY(service.writeFile(
        replacement, QStringLiteral("existing.fit"), nullptr));
    struct stat existingStatus {};
    QVERIFY(::stat(
        encodedExistingPath.constData(), &existingStatus) == 0);
    QCOMPARE(existingStatus.st_mode & 0777, mode_t(0600));
#endif

    const QString nestedPath =
            QDir(store.path()).filePath(QStringLiteral("nested"));
    QVERIFY(service.createFolder(nestedPath));
    QVERIFY(QFileInfo(
        nestedPath).isDir());

    errors.clear();
    const QList<CloudServiceEntry *> finalEntries =
            service.readdir(store.path(), errors);
    QVERIFY(errors.isEmpty());
    bool nestedListed = false;
    bool writtenListed = false;
    for (const CloudServiceEntry *entry : finalEntries) {
        if (entry->name == QStringLiteral("nested")) {
            nestedListed = entry->isDir;
        }
        if (entry->name == QStringLiteral("written.fit")) {
            writtenListed = !entry->isDir;
        }
    }
    QVERIFY(nestedListed);
    QVERIFY(writtenListed);
    QVERIFY(service.close());


#ifdef Q_OS_UNIX
    const LocalFileStoreProcessResult helperOpen =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                store.path());
    QVERIFY(helperOpen.succeeded());

    const LocalFileStoreProcessResult helperList =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::List,
                store.path(), QStringLiteral("/"));
    QVERIFY(helperList.succeeded());
    QVERIFY(std::any_of(
        helperList.entries.cbegin(), helperList.entries.cend(),
        [](const LocalFileStoreEntryValue &entry) {
            return entry.name == QStringLiteral("source.fit")
                && !entry.isDir && entry.size == 11;
        }));

    const LocalFileStoreProcessResult helperRead =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                store.path(), QStringLiteral("source.fit"));
    QVERIFY(helperRead.succeeded());
    QCOMPARE(helperRead.data, QByteArray("source-data"));
#endif

}

void TestAthleteMigrationSafety::
localFileStoreHelperRejectsMalformedFrames()
{
    auto appendU16 = [](QByteArray &frame, quint16 value) {
        frame.append(char(value >> 8));
        frame.append(char(value));
    };
    auto appendU32 = [](QByteArray &frame, quint32 value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            frame.append(char(value >> shift));
    };
    auto requestHeader = [&appendU16, &appendU32]() {
        QByteArray frame;
        appendU32(frame, 0x47434c46);
        appendU16(frame, 1);
        frame.append(char(LocalFileStoreProcess::Operation::Open));
        return frame;
    };

    QByteArray oversizedLength = requestHeader();
    appendU32(oversizedLength, std::numeric_limits<quint32>::max());

    QByteArray invalidUtf8 = requestHeader();
    appendU32(invalidUtf8, 1);
    invalidUtf8.append(char(0xff));
    appendU32(invalidUtf8, 0);

    QByteArray trailingData = requestHeader();
    appendU32(trailingData, 1);
    trailingData.append('/');
    appendU32(trailingData, 0);
    trailingData.append('x');

    QByteArray embeddedNullRoot = requestHeader();
    QByteArray nullRoot("/tmp", 4);
    nullRoot.append('\0');
    nullRoot.append("ignored");
    appendU32(embeddedNullRoot, quint32(nullRoot.size()));
    embeddedNullRoot.append(nullRoot);
    appendU32(embeddedNullRoot, 0);

    const QList<QByteArray> malformedFrames{
        QByteArray::fromHex("47434c"),
        oversizedLength,
        invalidUtf8,
        trailingData,
        embeddedNullRoot,
        QByteArray(128 * 1024 + 1, '\0')};

    for (const QByteArray &frame : malformedFrames) {
        QProcess helper;
        helper.setProgram(QCoreApplication::applicationFilePath());
        helper.setArguments({
            QStringLiteral("--gc-local-file-store-helper-v1"),
            QStringLiteral("1000")});
        helper.setProcessChannelMode(QProcess::SeparateChannels);
        helper.start(QIODevice::ReadWrite);

        QVERIFY2(helper.waitForStarted(2000),
                 qPrintable(helper.errorString()));
        QCOMPARE(helper.write(frame), qint64(frame.size()));
        helper.closeWriteChannel();
        QVERIFY2(helper.waitForFinished(2000),
                 qPrintable(helper.errorString()));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 3);
        QVERIFY(helper.readAllStandardOutput().isEmpty());
    }
}

void TestAthleteMigrationSafety::
localFileStoreRejectsMalformedResponses()
{
    auto appendU32 = [](QByteArray &frame, quint32 value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            frame.append(char(value >> shift));
    };
    auto appendU64 = [](QByteArray &frame, quint64 value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.append(char(value >> shift));
    };
    auto responseForName = [&appendU32, &appendU64](
            const QByteArray &name) {
        QByteArray frame;
        appendU32(frame, 0x47434c46);
        frame.append(char(0));
        frame.append(char(1));
        frame.append(char(LocalFileStoreProcess::Operation::List));
        frame.append(char(
            LocalFileStoreProcessResult::Status::Succeeded));
        appendU32(frame, 0);
        appendU32(frame, 1);
        appendU32(frame, quint32(name.size()));
        frame.append(name);
        frame.append(char(0));
        appendU64(frame, 0);
        appendU64(frame, std::numeric_limits<quint64>::max());
        appendU32(frame, 0);
        return frame;
    };

    LocalFileStoreProcessResult validResult;
    QVERIFY(LocalFileStoreProcess::parseResponseForTest(
        responseForName(QByteArray("ride.fit")),
        LocalFileStoreProcess::Operation::List,
        validResult));
    QCOMPARE(validResult.entries.size(), 1);
    QCOMPARE(validResult.entries.first().name,
             QStringLiteral("ride.fit"));

    const QList<QByteArray> invalidNames{
        QByteArray("."),
        QByteArray(".."),
        QByteArray("nested/ride.fit"),
        QByteArray("nested\\ride.fit"),
        QByteArray("bad\0name", 8)};
    for (const QByteArray &name : invalidNames) {
        LocalFileStoreProcessResult result;
        QVERIFY2(
            !LocalFileStoreProcess::parseResponseForTest(
                responseForName(name),
                LocalFileStoreProcess::Operation::List,
                result),
            name.constData());
    }
}

void TestAthleteMigrationSafety::
localFileStoreManualBrowserUsesAbsolutePaths()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store requires handle-relative Unix file APIs");
#endif
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString rideName = QStringLiteral("manual.fit");
    QFile ride(store.filePath(rideName));
    QVERIFY(ride.open(QIODevice::WriteOnly));
    QCOMPARE(ride.write("manual-data"), qint64(11));
    ride.close();

    LocalFileStore service(nullptr);
    service.configuration.insert(
        QString::fromLatin1(GC_NETWORKFILESTORE_FOLDER), store.path());

    QCOMPARE(service.home(), store.path());
    QStringList errors;
    const QList<CloudServiceEntry *> entries =
            service.readdir(store.path(), errors);
    QVERIFY(errors.isEmpty());
    QVERIFY(std::any_of(
        entries.cbegin(), entries.cend(),
        [&rideName](const CloudServiceEntry *entry) {
            return entry && entry->name == rideName && !entry->isDir;
        }));
}

void TestAthleteMigrationSafety::
localFileStoreManualReadRejectsSpecialFilePromptly()
{
#ifndef Q_OS_UNIX
    QSKIP("special-file race test requires Unix");
#else
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString fifoPath =
            store.filePath(QStringLiteral("manual.fifo"));
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QVERIFY(::mkfifo(encodedFifoPath.constData(), 0600) == 0);

    std::thread delayedWriter([encodedFifoPath]() {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(250));
        const int descriptor = ::open(
            encodedFifoPath.constData(),
            O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor >= 0) ::close(descriptor);
    });

    LocalFileStore service(nullptr);
    service.configuration.insert(
        QString::fromLatin1(GC_NETWORKFILESTORE_FOLDER),
        store.path());
    QByteArray data("sentinel");
    QElapsedTimer timer;
    timer.start();
    const bool read = service.readFile(
        &data, QStringLiteral("manual.fifo"), QString());
    const qint64 elapsedMs = timer.elapsed();
    delayedWriter.join();

    QVERIFY(!read);
    QCOMPARE(data, QByteArray("sentinel"));
    QVERIFY2(elapsedMs < 150,
             "manual read blocked while opening a special file");
#endif
}

void TestAthleteMigrationSafety::
localFileStoreHelperTimeoutIsBounded()
{
#ifndef Q_OS_UNIX
    QSKIP("FIFO-backed helper timeout test requires Unix");
#else
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString rideName = QStringLiteral("blocked.fit");
    const QString fifoPath = store.filePath(rideName);
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QVERIFY2(
        ::mkfifo(encodedFifoPath.constData(), 0600) == 0,
        "failed to create helper timeout FIFO");

    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE",
        QByteArrayLiteral("1"));
    std::atomic_bool stopWriter{false};
    std::atomic_bool writerConnected{false};
    std::thread writer(
        [&]() {
            int descriptor = -1;
            while (!stopWriter.load(std::memory_order_acquire)
                   && descriptor < 0) {
                descriptor = ::open(
                    encodedFifoPath.constData(),
                    O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (descriptor < 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            if (descriptor < 0) return;

            writerConnected.store(true, std::memory_order_release);
            while (!stopWriter.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }
            ::close(descriptor);
        });

    QElapsedTimer timer;
    timer.start();
    const LocalFileStoreProcessResult result =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                store.path(), rideName, 1000);
    const qint64 elapsedMs = timer.elapsed();
    const qint64 helperProcessId =
            LocalFileStoreProcess::lastHelperProcessIdForTest();
    const bool helperRunningAfterReturn =
            LocalFileStoreProcess::helperProcessIsRunningForTest(
                helperProcessId);
    stopWriter.store(true, std::memory_order_release);
    writer.join();
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE");

    QVERIFY2(writerConnected.load(std::memory_order_acquire),
             "helper did not enter the blocking FIFO read");
    QVERIFY(result.status
            == LocalFileStoreProcessResult::Status::TimedOut);
    QVERIFY(helperProcessId > 0);
    QVERIFY2(!helperRunningAfterReturn,
        "timed-out helper process was still running");
    QVERIFY2(elapsedMs < 2000,
             "helper timeout exceeded its bounded deadline");
#endif
}

void TestAthleteMigrationSafety::
localFileStoreFailedTerminationIsReaped()
{
#ifndef Q_OS_UNIX
    QSKIP("FIFO-backed helper reaper test requires Unix");
#else
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString rideName = QStringLiteral("reaper.fit");
    const QString fifoPath = store.filePath(rideName);
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QVERIFY2(
        ::mkfifo(encodedFifoPath.constData(), 0600) == 0,
        "failed to create helper reaper FIFO");

    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_HELPER_DEADLINE_MS",
        QByteArrayLiteral("5000"));

    std::atomic_bool stopWriter{false};
    std::atomic_bool writerConnected{false};
    std::thread writer(
        [&]() {
            int descriptor = -1;
            while (!stopWriter.load(std::memory_order_acquire)
                   && descriptor < 0) {
                descriptor = ::open(
                    encodedFifoPath.constData(),
                    O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (descriptor < 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            if (descriptor < 0) return;

            writerConnected.store(true, std::memory_order_release);
            while (!stopWriter.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }
            ::close(descriptor);
        });

    const LocalFileStoreProcessResult result =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                store.path(), rideName, 1000);
    const qint64 helperProcessId =
            LocalFileStoreProcess::lastHelperProcessIdForTest();
    const bool adoptedWhileRunning = waitUntil(
        [helperProcessId]() {
            return LocalFileStoreProcess::
                        helperProcessIsRunningForTest(helperProcessId)
                && LocalFileStoreProcess::
                        reaperProcessCountForTest() == 1;
        },
        1000);
    const bool drained =
            LocalFileStoreProcess::drainReaperForTest(2000);
    const bool helperStopped = waitUntil(
        [helperProcessId]() {
            return !LocalFileStoreProcess::
                        helperProcessIsRunningForTest(
                            helperProcessId)
                && LocalFileStoreProcess::
                        reaperProcessCountForTest() == 0;
        },
        1000);

    stopWriter.store(true, std::memory_order_release);
    writer.join();
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_HELPER_DEADLINE_MS");

    QVERIFY2(writerConnected.load(std::memory_order_acquire),
             "helper did not enter the reaper FIFO read");
    QCOMPARE(
        result.status,
        LocalFileStoreProcessResult::Status::Failed);
    QCOMPARE(result.error, QStringLiteral("helper-termination-failed"));
    QVERIFY(helperProcessId > 0);
    QVERIFY2(adoptedWhileRunning,
             "reaper did not adopt a live helper process");
    QVERIFY2(drained,
             "reaper shutdown drain exceeded its deadline");
    QVERIFY2(helperStopped,
             "reaper did not observe helper termination");
#endif
}

void TestAthleteMigrationSafety::
localFileStoreDelayedStartIsBounded()
{
#if !defined(Q_OS_UNIX) || QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QSKIP("delayed helper start test requires Qt 6 on Unix");
#else
    QTemporaryDir store;
    QVERIFY(store.isValid());
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS",
        QByteArrayLiteral("1000"));

    QElapsedTimer timeoutTimer;
    timeoutTimer.start();
    const LocalFileStoreProcessResult timedOut =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                store.path(), QString(), 100);
    const qint64 timeoutElapsedMs = timeoutTimer.elapsed();

    LocalFileStoreRunThread cancellation(
        store.path(), 5000);
    cancellation.start();
    QTest::qWait(50);
    cancellation.requestInterruption();
    const bool cancelledPromptly = cancellation.wait(2000);
    const bool cancellationJoined =
            cancelledPromptly || cancellation.wait(5000);
    const LocalFileStoreProcessResult cancelled =
            cancellation.result();

    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER",
        QByteArrayLiteral("1"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD",
        QByteArrayLiteral("1"));
    const LocalFileStoreProcessResult transferred =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                store.path(), QString(), 100);
    const bool adopted = waitUntil(
        []() {
            return LocalFileStoreProcess::
                reaperProcessCountForTest() == 1;
        },
        1000);
    const bool drained =
            LocalFileStoreProcess::drainReaperForTest(2000);

    qunsetenv("GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER");
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD");

    QVERIFY(timedOut.status
            == LocalFileStoreProcessResult::Status::TimedOut);
    QVERIFY2(timeoutElapsedMs < 1500,
             "helper startup timeout exceeded its deadline");
    QVERIFY(cancellationJoined);
    QVERIFY2(cancelledPromptly,
             "helper startup ignored thread interruption");
    QVERIFY(cancelled.status
            == LocalFileStoreProcessResult::Status::Cancelled);
    QCOMPARE(
        transferred.status,
        LocalFileStoreProcessResult::Status::Failed);
    QCOMPARE(
        transferred.error,
        QStringLiteral("helper-termination-failed"));
    QVERIFY2(adopted,
             "starting helper was not transferred to the reaper");
    QVERIFY2(drained,
             "starting helper was not drained by the reaper");
    QCOMPARE(LocalFileStoreProcess::reaperProcessCountForTest(), 0);
#endif
}


void TestAthleteMigrationSafety::
localFileStoreConfinesPathsToConfiguredRoot()
{
#ifndef Q_OS_UNIX
    QSKIP("Local Store requires handle-relative Unix file APIs");
#endif
    QTemporaryDir base;
    QVERIFY(base.isValid());
    QDir baseDir(base.path());
    QVERIFY(baseDir.mkdir(QStringLiteral("store")));
    const QString storePath = baseDir.filePath(QStringLiteral("store"));
    const QString outsidePath =
            baseDir.filePath(QStringLiteral("outside.fit"));

    QFile outside(outsidePath);
    QVERIFY(outside.open(QIODevice::WriteOnly));
    QCOMPARE(outside.write("outside"), qint64(7));
    outside.close();

    LocalFileStore service(nullptr);
    service.configuration.insert(
        QString::fromLatin1(GC_NETWORKFILESTORE_FOLDER), storePath);

    QByteArray readData("sentinel");
    QVERIFY(!service.readFile(
        &readData, QStringLiteral("../outside.fit"), QString()));
    QCOMPARE(readData, QByteArray("sentinel"));

    QByteArray writeData("replacement");
    QVERIFY(!service.writeFile(
        writeData, QStringLiteral("../outside.fit"), nullptr));
    QVERIFY(outside.open(QIODevice::ReadOnly));
    QCOMPARE(outside.readAll(), QByteArray("outside"));
    outside.close();

    const QString insidePath =
            QDir(storePath).filePath(QStringLiteral("inside.fit"));
    QFile inside(insidePath);
    QVERIFY(inside.open(QIODevice::WriteOnly));
    QCOMPARE(inside.write("inside"), qint64(6));
    inside.close();

    readData.clear();
    QVERIFY(service.readFile(
        &readData, QStringLiteral("inside.fit"), QString()));
    QCOMPARE(readData, QByteArray("inside"));

#ifdef Q_OS_UNIX
    const LocalFileStoreProcessResult helperInside =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                storePath, QStringLiteral("inside.fit"));
    QVERIFY(helperInside.succeeded());
    QCOMPARE(helperInside.data, QByteArray("inside"));
#endif

    QString nullRoot = storePath;
    nullRoot.append(QChar::Null);
    nullRoot.append(QStringLiteral("ignored"));
    const LocalFileStoreProcessResult embeddedNullRoot =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                nullRoot);
    QVERIFY(!embeddedNullRoot.succeeded());

#ifdef Q_OS_UNIX
    const QString linkedRoot =
            baseDir.filePath(QStringLiteral("linked-store"));
    QVERIFY(QFile::link(storePath, linkedRoot));
    const LocalFileStoreProcessResult linkedRootOpen =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Open,
                linkedRoot);
    QVERIFY(linkedRootOpen.succeeded());
    const LocalFileStoreProcessResult linkedRootRead =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                linkedRoot, QStringLiteral("inside.fit"));
    QVERIFY(linkedRootRead.succeeded());
    QCOMPARE(linkedRootRead.data, QByteArray("inside"));
#endif

    const LocalFileStoreProcessResult helperEscape =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::Read,
                storePath, QStringLiteral("../outside.fit"));
    QVERIFY(!helperEscape.succeeded());

    const LocalFileStoreProcessResult listEscape =
            LocalFileStoreProcess::run(
                LocalFileStoreProcess::Operation::List,
                storePath, base.path());
    QVERIFY(!listEscape.succeeded());

#ifdef Q_OS_UNIX
    const QString linkPath =
            QDir(storePath).filePath(QStringLiteral("linked.fit"));
    QVERIFY(QFile::link(outsidePath, linkPath));
    readData = "sentinel";
    QVERIFY(!service.readFile(
        &readData, QStringLiteral("linked.fit"), QString()));
    QCOMPARE(readData, QByteArray("sentinel"));
#endif
}

void TestAthleteMigrationSafety::athleteTeardownJoinsBlockedCloudDownload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDownloadLifecycle")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureBlockingCloudAutoDownload(athlete->cyclist);
    QPointer<CloudServiceAutoDownload> worker(athlete->cloudAutoDownload);
    worker->checkDownload();

    const bool readStarted = waitForBlockingCloudRead(5000);
    if (!readStarted) {
        athleteStorage.destroy();
        if (worker) {
            worker->wait(35000);
            delete worker.data();
        }
        cleanupBlockingCloudRead();
    }
    QVERIFY2(readStarted, "production cloud worker did not reach readFile");

    QElapsedTimer teardownTimer;
    teardownTimer.start();
    athleteStorage.destroy();
    const qint64 teardownElapsedMs = teardownTimer.elapsed();
    const bool joinedDuringTeardown = worker.isNull();

    bool releaseSucceeded = true;
    bool cleanupJoined = true;
    if (worker) {
        releaseSucceeded = releaseBlockingCloudRead(worker.data());
        cleanupJoined = worker->wait(7000);
        if (!cleanupJoined) cleanupJoined = worker->wait(35000);
        if (cleanupJoined) delete worker.data();
    }
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    cleanupBlockingCloudRead();

    QVERIFY2(releaseSucceeded, "failed to release blocked test provider");
    QVERIFY2(cleanupJoined, "cloud worker did not finish during test cleanup");
    QVERIFY2(joinedDuringTeardown,
             "Athlete teardown returned while cloud worker was still running");
    QVERIFY2(teardownElapsedMs < 2000,
             "Athlete teardown did not cancel and join within two seconds");
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
localFileStoreBlockedReadHasBoundedTeardown()
{
#ifndef Q_OS_UNIX
    QSKIP("FIFO-backed blocking read test requires Unix");
#else
    static const char childEnvironment[] =
            "GC_LOCAL_FILE_STORE_BLOCKED_READ_CHILD";
    static const QByteArray readStartedMarker("LOCAL_READ_STARTED\n");
    static const QByteArray teardownReturnedMarker(
            "LOCAL_TEARDOWN_RETURNED\n");

    if (!qEnvironmentVariableIsSet(childEnvironment)) {
        QProcess child;
        QProcessEnvironment environment =
                QProcessEnvironment::systemEnvironment();
        environment.insert(
            QString::fromLatin1(childEnvironment),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral(
                "localFileStoreBlockedReadHasBoundedTeardown")});

        const bool started = child.waitForStarted(2000);
        QByteArray output;
        bool readStarted = false;
        QElapsedTimer startupTimer;
        startupTimer.start();
        while (started && child.state() != QProcess::NotRunning
               && startupTimer.elapsed() < 7000) {
            child.waitForReadyRead(100);
            output.append(child.readAll());
            if (output.contains(readStartedMarker)) {
                readStarted = true;
                break;
            }
        }
        output.append(child.readAll());
        readStarted =
                readStarted || output.contains(readStartedMarker);

        const bool finished =
                readStarted && child.waitForFinished(2000);
        output.append(child.readAll());
        if (!finished && child.state() != QProcess::NotRunning) {
            child.kill();
            child.waitForFinished(2000);
            output.append(child.readAll());
        }

        QVERIFY2(started, output.constData());
        QVERIFY2(readStarted, output.constData());
        QVERIFY2(finished, output.constData());
        QVERIFY2(
            output.contains(teardownReturnedMarker),
            output.constData());
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit
                && child.exitCode() == 0,
            output.constData());
        return;
    }

    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString rideName =
            QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
            + QStringLiteral(".fit");
    const QString fifoPath = source.filePath(rideName);
    const QByteArray encodedFifoPath = QFile::encodeName(fifoPath);
    QVERIFY2(
        ::mkfifo(encodedFifoPath.constData(), 0600) == 0,
        "failed to create activity FIFO");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("BlockedLocalFileStore")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const CloudService *service =
            CloudServiceFactory::instance().service(
                QStringLiteral("Local Store"));
    QVERIFY2(service, "Local Store provider was not registered");
    appsettings->setCValue(
        athlete->cyclist, GC_NETWORKFILESTORE_FOLDER, source.path());
    appsettings->setCValue(
        athlete->cyclist, service->syncOnStartupSettingName(),
        QStringLiteral("true"));
    qputenv(
        "GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE",
        QByteArrayLiteral("1"));
    athlete->cloudAutoDownload->checkDownload();

    int writer = -1;
    QElapsedTimer readerTimer;
    readerTimer.start();
    while (writer < 0 && readerTimer.elapsed() < 5000) {
        writer = ::open(
            encodedFifoPath.constData(),
            O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer < 0) QTest::qWait(10);
    }
    const qint64 helperProcessId =
            LocalFileStoreProcess::lastHelperProcessIdForTest();
    QVERIFY2(writer >= 0, "Local Store did not start reading the FIFO");

    std::fputs(readStartedMarker.constData(), stdout);
    std::fflush(stdout);
    athleteStorage.destroy();
    qunsetenv("GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE");
    QVERIFY(helperProcessId > 0);
    QVERIFY2(
        !LocalFileStoreProcess::helperProcessIsRunningForTest(
            helperProcessId),
        "Local Store helper survived athlete teardown");
    std::fputs(teardownReturnedMarker.constData(), stdout);
    std::fflush(stdout);
    ::close(writer);
#endif
}

void TestAthleteMigrationSafety::
synchronousCloudCompletionIsPromptAndThreadIsolated()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudInlineCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Inline, 1, 50);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool resultParsed = waitUntil(
            []() { return athleteMigrationRideFileOpenCalls() == 1; },
            1000);
    const bool finishedPromptly = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool athleteSettingSaved = waitUntil(
            [athlete]() {
                return appsettings->cvalue(
                    athlete->cyclist,
                    QStringLiteral("<athlete-private>controlled/thread"))
                           .toString() == QStringLiteral("worker-value");
            },
            1000);
    const bool globalSettingSaved = waitUntil(
            []() {
                return appsettings->value(
                    nullptr,
                    QStringLiteral("<global-general>controlled/thread"))
                           .toString() ==
                        QStringLiteral("global-worker-value");
            },
            1000);
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int providerOperations =
            controlledCloudProviderOperations();
    const int crossThreadSettingsWrites =
            athleteMigrationSettingsCrossThreadWrites();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "production cloud worker did not reach readFile");
    QVERIFY2(completionEmitted,
             "inline cloud completion was not emitted");
    QVERIFY2(resultParsed,
             "inline cloud result was not delivered for parsing");
    QVERIFY2(finishedPromptly,
             "inline cloud completion was lost before the wait began");
    QVERIFY2(athleteSettingSaved,
             "worker athlete setting was not persisted on the GUI thread");
    QVERIFY2(globalSettingSaved,
             "worker global setting was not persisted on the GUI thread");
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QVERIFY(providerOperations >= 7);
    QCOMPARE(crossThreadSettingsWrites, 0);
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void
TestAthleteMigrationSafety::inlineCloudCompletionDoesNotReenterProvider()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudInlineNestedEvents")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::InlineWithNestedEvents,
        2,
        500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool readsStarted = waitForControlledCloudReads(2, 5000);
    const bool completionsEmitted =
            waitForControlledCloudCompletions(2, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 2000);
    const int reentrantReadCalls =
            controlledCloudReentrantReadCalls();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(readsStarted);
    QVERIFY(completionsEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(reentrantReadCalls, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::queuedCloudResultsSurviveWorkerCleanup()
{
    constexpr int EntryCount = 8;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudQueuedCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool allReadsStarted =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool allCompletionsEmitted =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(6000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool allResultsParsed = waitUntil(
            []() {
                return athleteMigrationRideFileOpenCalls() == EntryCount;
            },
            1000);
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(allReadsStarted,
             "not all queued cloud reads were started");
    QVERIFY2(allCompletionsEmitted,
             "not all queued cloud completions were emitted");
    QVERIFY2(providerDestroyed,
             "cloud provider was not cleaned up by the worker");
    QVERIFY2(finishedDelivered,
             "queued cloud download did not report completion");
    QVERIFY2(allResultsParsed,
             "queued cloud results were lost during worker cleanup");
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QCOMPARE(parsedPayloads.size(), EntryCount);
    for (const QByteArray &payload : parsedPayloads) {
        QCOMPARE(payload, QByteArray("not-a-valid-fit"));
    }
    QCOMPARE(buffersAllocated, EntryCount);
    QCOMPARE(buffersReleased, EntryCount);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
naturalCloudCompletionStopsWithoutGuiDispatch()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNaturalThreadExit")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    worker->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyedBeforeGuiDispatch =
            waitForControlledCloudProviderDestroyed(5000);
    const bool stoppedNaturally = worker->wait(1000);
    worker->cancelAndWait();
    const bool providerDestroyedDuringCleanup =
            providerDestroyedBeforeGuiDispatch
            || waitForControlledCloudProviderDestroyed(1000);
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(readStarted);
    QVERIFY(completionEmitted);
    QVERIFY2(providerDestroyedDuringCleanup,
             "cloud provider was not destroyed during cleanup");
    QVERIFY2(providerDestroyedBeforeGuiDispatch,
             "worker cleanup depended on the GUI completion callback");
    QVERIFY2(stoppedNaturally,
             "QThread::wait blocked before GUI completion dispatch");
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
stalledGuiCoalescesAutoDownloadProgress()
{
    constexpr int EntryCount = 4096;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudProgressBackpressure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::Reject,
        EntryCount,
        500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    worker->checkDownload();

    const bool allReadsStarted =
        waitForControlledCloudReads(EntryCount, 15000);
    const bool providerDestroyed =
        waitForControlledCloudProviderDestroyed(5000);
    const bool workerStoppedWithoutGuiDispatch = worker->wait(5000);
    const CloudAutoDownloadQueueStats stalledStats =
        worker->queuedEventStatsForTest();
    const bool delivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 5000);
    const CloudAutoDownloadQueueStats drainedStats =
        worker->queuedEventStatsForTest();

    bool monotonic = true;
    int previous = -1;
    for (const QList<QVariant> &values : progress) {
        const int current = values.at(2).toInt();
        monotonic = monotonic && current >= previous;
        previous = current;
    }
    const QList<QVariant> finalProgress =
        progress.isEmpty() ? QList<QVariant>() : progress.constLast();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(allReadsStarted);
    QVERIFY(providerDestroyed);
    QVERIFY(workerStoppedWithoutGuiDispatch);
    QVERIFY(delivered);
    QVERIFY(monotonic);
    QVERIFY(progress.count() > 0);
    QVERIFY(progress.count() <= stalledStats.maximumProgressEvents);
    QCOMPARE(finalProgress.at(2).toInt(), EntryCount);
    QCOMPARE(finalProgress.at(3).toInt(), EntryCount);
    QCOMPARE(finalProgress.at(1).toDouble(), 100.0);
    QVERIFY(stalledStats.currentProgressEvents
            <= stalledStats.maximumProgressEvents);
    QVERIFY(stalledStats.peakProgressEvents
            <= stalledStats.maximumProgressEvents);
    QVERIFY(stalledStats.coalescedProgress > 0);
    QCOMPARE(stalledStats.dispatchPosts, quint64(1));
    QCOMPARE(drainedStats.currentEvents, 0);
}

void TestAthleteMigrationSafety::
stalledGuiBackpressuresDownloadedPayloads()
{
    constexpr int EntryCount = 6;
    constexpr int MaximumDownloads = 2;
    constexpr int PayloadBytes = 32 * 1024;
    constexpr qint64 MaximumBytes = 1024 * 1024;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudPayloadBackpressure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::Queued,
        EntryCount,
        500,
        PayloadBytes,
        0,
        MaximumDownloads,
        MaximumBytes);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    worker->checkDownload();

    const bool thirdReadStarted =
        waitForControlledCloudReads(MaximumDownloads + 1, 5000);
    const bool admittedCompletions =
        waitForControlledCloudCompletions(MaximumDownloads, 5000);
    QTest::qSleep(100);
    const bool extraCompletionBeforeDrain =
        waitForControlledCloudCompletions(1, 100);
    const CloudAutoDownloadQueueStats stalledStats =
        worker->queuedEventStatsForTest();
    const bool allResultsDelivered = waitUntil(
        [&finished]() {
            return finished.count() == 1
                && athleteMigrationRideFileOpenCalls() == EntryCount;
        },
        5000);
    const bool providerDestroyed =
        waitForControlledCloudProviderDestroyed(5000);
    const CloudAutoDownloadQueueStats drainedStats =
        worker->queuedEventStatsForTest();
    const int buffersOutstanding =
        cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(thirdReadStarted);
    QVERIFY(admittedCompletions);
    QVERIFY(!extraCompletionBeforeDrain);
    QCOMPARE(stalledStats.currentDownloadEvents, MaximumDownloads);
    QVERIFY(stalledStats.peakDownloadEvents
            <= stalledStats.maximumDownloadEvents);
    QVERIFY(stalledStats.peakDownloadBytes
            <= stalledStats.maximumDownloadBytes);
    QVERIFY(stalledStats.producerWaits > 0);
    QVERIFY(allResultsDelivered);
    QVERIFY(providerDestroyed);
    QCOMPARE(drainedStats.currentDownloadEvents, 0);
    QCOMPARE(drainedStats.currentDownloadBytes, qint64(0));
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
cancellationPurgesBackpressuredGeneration()
{
    constexpr int EntryCount = 10;
    constexpr int MaximumDownloads = 2;
    constexpr int PayloadBytes = 32 * 1024;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudBackpressureCancellation")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::Queued,
        EntryCount,
        500,
        PayloadBytes,
        0,
        MaximumDownloads,
        1024 * 1024);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    worker->checkDownload();

    const bool thirdReadStarted =
        waitForControlledCloudReads(MaximumDownloads + 1, 5000);
    const bool admittedCompletions =
        waitForControlledCloudCompletions(MaximumDownloads, 5000);
    QTest::qSleep(100);
    const CloudAutoDownloadQueueStats stalledStats =
        worker->queuedEventStatsForTest();

    QElapsedTimer timer;
    timer.start();
    worker->cancelAndWait();
    const qint64 cancelElapsedMs = timer.elapsed();
    const bool providerDestroyed =
        waitForControlledCloudProviderDestroyed(2000);
    const CloudAutoDownloadQueueStats cancelledStats =
        worker->queuedEventStatsForTest();
    QCoreApplication::sendPostedEvents(worker, QEvent::MetaCall);
    QCoreApplication::processEvents();
    const bool workerStopped = !worker->isRunning();
    const int buffersOutstanding =
        cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(thirdReadStarted);
    QVERIFY(admittedCompletions);
    QVERIFY(stalledStats.producerWaits > 0);
    QVERIFY(cancelElapsedMs < 2000);
    QVERIFY(workerStopped);
    QVERIFY(providerDestroyed);
    QCOMPARE(cancelledStats.currentEvents, 0);
    QCOMPARE(cancelledStats.currentDownloadEvents, 0);
    QCOMPARE(cancelledStats.currentDownloadBytes, qint64(0));
    QCOMPARE(progress.count(), 0);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(athleteMigrationRideFileOpenCalls(), 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
queuedCloudSettingsPreserveNewerValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsConflict")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    appsettings->setValue(
        QStringLiteral("<global-general>controlled/thread"),
        QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("newer-athlete-value"));

    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread")).toString();
    const QString globalValue = appsettings->value(
        nullptr,
        QStringLiteral("<global-general>controlled/thread")).toString();
    const int crossThreadWrites =
            athleteMigrationSettingsCrossThreadWrites();
    const int parsedResults =
            athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(athleteValue, QStringLiteral("newer-athlete-value"));
    QCOMPARE(globalValue, QStringLiteral("initial-global-value"));
    QCOMPARE(crossThreadWrites, 0);
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
queuedCloudSettingsRejectRemovedExpectedValue()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsRemoval")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    appsettings->setValue(
        globalKey, QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->remove(globalKey);

    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString globalValue = appsettings->value(
        nullptr, globalKey, QStringLiteral("missing")).toString();
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(globalValue, QStringLiteral("missing"));
    QCOMPARE(athleteValue, QStringLiteral("missing"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
sequentialQueuedCloudSettingsPreserveLatestValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsSequence")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    appsettings->setCValue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("initial-athlete-value"));
    appsettings->setValue(
        QStringLiteral("<global-general>controlled/thread"),
        QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::QueuedSettingsTwice, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread")).toString();
    const QString globalValue = appsettings->value(
        nullptr,
        QStringLiteral("<global-general>controlled/thread")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(athleteValue, QStringLiteral("worker-value-2"));
    QCOMPARE(globalValue, QStringLiteral("global-worker-value-2"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::
sequentialQueuedCloudSettingsRejectConflictChain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsConflictChain")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString athleteKey =
            QStringLiteral("<athlete-private>controlled/thread");
    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    appsettings->setCValue(
        athlete->cyclist, athleteKey,
        QStringLiteral("initial-athlete-value"));
    appsettings->setValue(
        globalKey, QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::QueuedSettingsTwice, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist, athleteKey,
        QStringLiteral("external-athlete-value"));
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist, athleteKey).toString();
    const QString globalValue = appsettings->value(
        nullptr, globalKey).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(
        athleteValue, QStringLiteral("external-athlete-value"));
    QCOMPARE(globalValue, QStringLiteral("initial-global-value"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
stalledGuiCoalescesCloudSettingsTransactions()
{
    constexpr int UpdateCount = 4096;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudSettingsBackpressure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::QueuedSettingsFlood,
        0,
        500,
        0,
        UpdateCount);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    worker->checkDownload();

    const bool providerDestroyed =
        waitForControlledCloudProviderDestroyed(15000);
    const bool workerStoppedWithoutGuiDispatch = worker->wait(5000);
    const CloudGuiHandoffQueueStats stalledStats =
        cloudSettingsQueueStatsForTest();
    const bool finishedDelivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 5000);
    const CloudGuiHandoffQueueStats drainedStats =
        cloudSettingsQueueStatsForTest();
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread")).toString();
    const QString globalValue = appsettings->value(
        nullptr,
        QStringLiteral("<global-general>controlled/thread")).toString();
    const int crossThreadWrites =
        athleteMigrationSettingsCrossThreadWrites();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(providerDestroyed);
    QVERIFY(workerStoppedWithoutGuiDispatch);
    QCOMPARE(stalledStats.currentItems, 1);
    QVERIFY(stalledStats.peakItems <= stalledStats.maximumItems);
    QVERIFY(stalledStats.peakBytes <= stalledStats.maximumBytes);
    QCOMPARE(stalledStats.dispatchPosts, quint64(1));
    QCOMPARE(stalledStats.coalesced, quint64(UpdateCount - 1));
    QCOMPARE(stalledStats.rejected, quint64(0));
    QVERIFY(finishedDelivered);
    QCOMPARE(
        athleteValue,
        QStringLiteral("worker-value-%1").arg(UpdateCount - 1));
    QCOMPARE(
        globalValue,
        QStringLiteral("global-worker-value-%1").arg(UpdateCount - 1));
    QCOMPARE(crossThreadWrites, 0);
    QCOMPARE(drainedStats.currentItems, 0);
    QCOMPARE(drainedStats.currentBytes, qint64(0));
    QCOMPARE(drainedStats.dispatchCalls, quint64(1));
}

void TestAthleteMigrationSafety::guiCloudSettingsSaveWritesAllScopes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudGuiSettings")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    std::unique_ptr<CloudService> service(
        CloudServiceFactory::instance().newService(
            QStringLiteral("ControlledLifecycleCloud"), context.get()));
    QVERIFY(service);

    const QString athleteKey =
            QStringLiteral("<athlete-private>controlled/thread");
    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    service->setSetting(athleteKey, QStringLiteral("gui-athlete-value"));
    service->setSetting(globalKey, QStringLiteral("gui-global-value"));
    service->setSetting(
        service->syncOnStartupSettingName(), QStringLiteral("false"));
    service->setSetting(
        service->syncOnImportSettingName(), QStringLiteral("true"));
    CloudServiceFactory::instance().saveSettings(
        service.get(), context.get());

    QCOMPARE(
        appsettings->cvalue(athlete->cyclist, athleteKey).toString(),
        QStringLiteral("gui-athlete-value"));
    QCOMPARE(
        appsettings->value(nullptr, globalKey).toString(),
        QStringLiteral("gui-global-value"));
    QCOMPARE(
        appsettings->cvalue(
            athlete->cyclist,
            service->syncOnStartupSettingName()).toString(),
        QStringLiteral("false"));
    QCOMPARE(
        appsettings->cvalue(
            athlete->cyclist,
            service->syncOnImportSettingName()).toString(),
        QStringLiteral("true"));
    QCOMPARE(athleteMigrationSettingsCrossThreadWrites(), 0);

    service.reset();
    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();
}

void TestAthleteMigrationSafety::guiCloudSettingsSaveClearsCustomUrl()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudGuiClearUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://custom.invalid"));
    std::unique_ptr<CloudService> service(
        CloudServiceFactory::instance().newService(
            QStringLiteral("ControlledLifecycleCloud"), context.get()));
    QVERIFY(service);
    QCOMPARE(
        service->getSetting(urlKey, QString()).toString(),
        QStringLiteral("https://custom.invalid"));

    service->setSetting(urlKey, QString());
    CloudServiceFactory::instance().saveSettings(
        service.get(), context.get());
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey,
        QStringLiteral("missing")).toString();

    service.reset();
    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QCOMPARE(storedUrl, QString());
}

void TestAthleteMigrationSafety::workerCloudSettingsClearUrlUsesDefault()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudWorkerClearUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::QueuedClearsUrlTwice,
        1,
        500);
    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    const QString threadKey =
            QStringLiteral("<athlete-private>controlled/thread");
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://custom.invalid"));
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey,
        QStringLiteral("missing")).toString();
    const QString storedThread = appsettings->cvalue(
        athlete->cyclist, threadKey,
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QString());
    QCOMPARE(storedThread, QStringLiteral("worker-value-after-url-clear"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::
defaultCloudUrlDoesNotInvalidatePayload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDefaultUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/url"),
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QStringLiteral("missing"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::emptyCloudUrlUsesDefault()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudEmptyUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    appsettings->setCValue(athlete->cyclist, urlKey, QString());
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey, QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QString());
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::changedCloudUrlInvalidatesPayload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudChangedUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://changed.invalid"));
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QStringLiteral("https://changed.invalid"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
queuedCloudPayloadIsDiscardedWhenStartupSyncIsDisabled()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSyncDisabled")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    disableControlledCloudAutoDownload(athlete->cyclist);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString syncValue = appsettings->cvalue(
        athlete->cyclist,
        CloudServiceFactory::instance()
            .service(QStringLiteral("ControlledLifecycleCloud"))
            ->syncOnStartupSettingName()).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(syncValue, QStringLiteral("false"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
progressCallbackCanDestroyDownloader()
{
    constexpr int EntryCount = 3;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudProgressDestruction")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued,
            EntryCount, 500);
    QPointer<CloudServiceAutoDownload> worker(
        athlete->cloudAutoDownload);
    bool destroyedFromProgress = false;
    connect(
        context.get(), &Context::autoDownloadProgress,
        context.get(), [&]() {
            if (destroyedFromProgress) return;
            destroyedFromProgress = true;
            athleteStorage.destroy();
        });

    worker->checkDownload();
    const bool allReadsStarted =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool allCompletionsEmitted =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool callbackRan = waitUntil(
            [&]() { return destroyedFromProgress; }, 2000);

    if (athleteStorage.constructed()) athleteStorage.destroy();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    cleanupControlledCloudAutoDownload();

    QVERIFY(allReadsStarted);
    QVERIFY(allCompletionsEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(callbackRan);
    QVERIFY(worker.isNull());
    QCOMPARE(parsedResults, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::timedOutCloudReadReleasesBuffer()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudTimedOutRead")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Never, 8, 25);
    enableSuccessfulFollowUpCloudAutoDownload(athlete->cyclist);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool additionalReadStarted =
            waitForControlledCloudReads(1, 50);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool followUpParsed = waitUntil(
            []() { return athleteMigrationRideFileOpenCalls() == 1; },
            1000);
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "timed-out cloud read did not start");
    QVERIFY2(!additionalReadStarted,
             "provider continued after its first request timed out");
    QVERIFY2(providerDestroyed,
             "timed-out cloud provider was not cleaned up");
    QVERIFY2(finishedDelivered,
             "timed-out cloud download did not report completion");
    QVERIFY2(followUpParsed,
             "a healthy provider did not continue after another timed out");
    QCOMPARE(parsedResults, 1);
    QCOMPARE(parsedPayloads.size(), 1);
    QCOMPARE(parsedPayloads.first(), QByteArray("follow-up-payload"));
    QCOMPARE(buffersAllocated, 2);
    QCOMPARE(buffersReleased, 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::rejectedCloudReadReleasesBufferPromptly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudRejectedRead")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Reject, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    QElapsedTimer timer;
    timer.start();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedPromptly = waitUntil(
            [&finished]() { return finished.count() == 1; }, 250);
    const qint64 elapsedMs = timer.elapsed();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "rejected cloud read did not start");
    QVERIFY2(providerDestroyed,
             "provider for rejected cloud read was not cleaned up");
    QVERIFY2(finishedPromptly,
             "rejected cloud read waited for the request timeout");
    QVERIFY2(elapsedMs < 250,
             "rejected cloud read did not finish promptly");
    QCOMPARE(parsedResults, 0);
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
duplicateCloudCompletionIsAppliedOnceAcrossRepeatedRuns()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDuplicateCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Duplicate, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();

    athlete->cloudAutoDownload->checkDownload();
    const bool firstRead = waitForControlledCloudReads(1, 5000);
    const bool firstCompletions =
            waitForControlledCloudCompletions(2, 5000);
    const bool firstProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool firstFinished = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);

    athlete->cloudAutoDownload->checkDownload();
    const bool secondRead = waitForControlledCloudReads(1, 5000);
    const bool secondCompletions =
            waitForControlledCloudCompletions(2, 5000);
    const bool secondProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool secondFinished = waitUntil(
            [&finished]() { return finished.count() == 2; }, 1000);

    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY(firstRead);
    QVERIFY(firstCompletions);
    QVERIFY(firstProviderDestroyed);
    QVERIFY(firstFinished);
    QVERIFY(secondRead);
    QVERIFY(secondCompletions);
    QVERIFY(secondProviderDestroyed);
    QVERIFY(secondFinished);
    QCOMPARE(parsedResults, 2);
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QCOMPARE(parsedPayloads.size(), 2);
    for (const QByteArray &payload : parsedPayloads) {
        QCOMPARE(payload, QByteArray("not-a-valid-fit"));
    }
    QCOMPARE(buffersAllocated, 2);
    QCOMPARE(buffersReleased, 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
providerCancellationDoesNotDestroyActiveCall_data()
{
    QTest::addColumn<int>("completionMode");
    QTest::newRow("open")
            << int(TestCloudCompletionMode::BlockInOpen);
    QTest::newRow("directory")
            << int(TestCloudCompletionMode::BlockInDirectory);
    QTest::newRow("read")
            << int(TestCloudCompletionMode::BlockInRead);
    QTest::newRow("base-network-reply")
            << int(TestCloudCompletionMode::BlockInBaseAbort);
    QTest::newRow("completion-callback")
            << int(TestCloudCompletionMode::BlockInCompletion);
}

void TestAthleteMigrationSafety::
providerCancellationDoesNotDestroyActiveCall()
{
    QFETCH(int, completionMode);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNestedCancellation")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode(completionMode),
            1,
            30000);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    worker->checkDownload();

    const bool callBlocked = waitForControlledCloudBlockedCall(5000);
    QElapsedTimer timer;
    timer.start();
    worker->cancelAndWait();
    const qint64 cancelElapsedMs = timer.elapsed();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(1000);
    const bool workerStopped = !worker->isRunning();
    const int destroyedDuringCall =
            controlledCloudDestroyedDuringCall();
    const int abortCalls = controlledCloudAbortCalls();
    const int baseReplyAbortCalls =
            controlledCloudBaseReplyAbortCalls();
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY2(callBlocked, "provider did not enter its nested event loop");
    QVERIFY2(workerStopped, "cancelled cloud worker remained running");
    QVERIFY2(cancelElapsedMs < 2000,
             "nested provider call did not cancel promptly");
    QVERIFY2(providerDestroyed,
             "cancelled provider was not destroyed after its call returned");
    QCOMPARE(destroyedDuringCall, 0);
    QVERIFY(abortCalls >= 1);
    if (TestCloudCompletionMode(completionMode)
            == TestCloudCompletionMode::BlockInBaseAbort) {
        QVERIFY(baseReplyAbortCalls >= 1);
    }
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::cancellationFromStartSignalStopsSafely()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudCancelFromStart")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Inline, 1, 500);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    const bool spyValid = started.isValid();
    connect(
        context.get(), &Context::autoDownloadStart,
        worker, [worker]() { worker->cancelAndWait(); },
        Qt::DirectConnection);

    worker->checkDownload();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(1000);
    const bool workerStopped = !worker->isRunning();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QCOMPARE(started.count(), 1);
    QVERIFY(workerStopped);
    QVERIFY(providerDestroyed);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
staleQueuedResultsAreDiscardedAfterRestart()
{
    constexpr int EntryCount = 2;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudStaleGeneration")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    const bool spyValid = finished.isValid() && progress.isValid();

    worker->checkDownload();
    const bool firstReads =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool firstCompletions =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool firstProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    worker->cancelAndWait();
    const int parsedBeforeRestart = athleteMigrationRideFileOpenCalls();

    worker->checkDownload();
    const bool secondReads =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool secondCompletions =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool secondProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool secondRunDelivered = waitUntil(
            [&finished, &progress]() {
                return finished.count() == 1
                    && progress.count() == EntryCount + 1
                    && athleteMigrationRideFileOpenCalls() == EntryCount;
            },
            2000);

    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY(firstReads);
    QVERIFY(firstCompletions);
    QVERIFY(firstProviderDestroyed);
    QCOMPARE(parsedBeforeRestart, 0);
    QVERIFY(secondReads);
    QVERIFY(secondCompletions);
    QVERIFY(secondProviderDestroyed);
    QVERIFY(secondRunDelivered);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(parsedResults, EntryCount);
    QCOMPARE(progress.count(), EntryCount + 1);
    for (int i = 0; i <= EntryCount; ++i) {
        const QList<QVariant> values = progress.at(i);
        QCOMPARE(values.size(), 4);
        QCOMPARE(values.at(2).toInt(), i);
        QCOMPARE(values.at(3).toInt(), EntryCount);
    }
    QCOMPARE(buffersAllocated, EntryCount * 2);
    QCOMPARE(buffersReleased, EntryCount * 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
legacyReadCompleteDoesNotTakeCallerOwnership()
{
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    CloudServiceAutoDownload receiver(context.get());
    QByteArray callerOwned("caller-owned");

    receiver.readComplete(
        &callerOwned, QStringLiteral("legacy.fit"), QString());

    QCOMPARE(callerOwned, QByteArray("caller-owned"));
}

void TestAthleteMigrationSafety::
noEnabledCloudServiceEmitsNoLifecycleSignals()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNoEnabledService")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);

    athlete->cloudAutoDownload->checkDownload();
    QCoreApplication::processEvents();

    const bool spiesValid = started.isValid() && finished.isValid();
    const bool workerRunning = athlete->cloudAutoDownload->isRunning();
    athleteStorage.destroy();

    QVERIFY(spiesValid);
    QVERIFY(!workerRunning);
    QCOMPARE(started.count(), 0);
    QCOMPARE(finished.count(), 0);
}

void TestAthleteMigrationSafety::
autoDownloadEndFollowsImportsAndProgress()
{
    constexpr int EntryCount = 3;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudEventOrdering")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    const bool spiesValid =
            started.isValid() && finished.isValid() && progress.isValid();
    QStringList eventOrder;
    connect(
        context.get(), &Context::autoDownloadStart,
        context.get(), [&eventOrder]() {
            eventOrder.append(QStringLiteral("start"));
        });
    connect(
        context.get(), &Context::autoDownloadProgress,
        context.get(),
        [&eventOrder](const QString &, double, int current, int) {
            eventOrder.append(
                QStringLiteral("progress:%1").arg(current));
        });
    bool endFollowedImports = false;
    connect(
        context.get(), &Context::autoDownloadEnd,
        context.get(), [&endFollowedImports, &eventOrder]() {
            eventOrder.append(QStringLiteral("end"));
            endFollowedImports =
                    athleteMigrationRideFileOpenCalls() == EntryCount;
        });

    athlete->cloudAutoDownload->checkDownload();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool eventsDelivered = waitUntil(
            [&finished, &progress]() {
                return finished.count() == 1
                    && progress.count() == EntryCount + 1;
            },
            2000);

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spiesValid);
    QVERIFY(providerDestroyed);
    QVERIFY(eventsDelivered);
    QCOMPARE(started.count(), 1);
    QVERIFY(endFollowedImports);
    QCOMPARE(
        eventOrder,
        QStringList({
            QStringLiteral("start"),
            QStringLiteral("progress:0"),
            QStringLiteral("progress:1"),
            QStringLiteral("progress:2"),
            QStringLiteral("progress:3"),
            QStringLiteral("end")}));
    QCOMPARE(progress.count(), EntryCount + 1);
    for (int i = 0; i <= EntryCount; ++i) {
        const QList<QVariant> values = progress.at(i);
        QCOMPARE(values.size(), 4);
        QCOMPARE(values.at(0).toString(),
                 QStringLiteral("Controlled lifecycle cloud"));
        QCOMPARE(
            values.at(1).toDouble(),
            (100.0 * double(i)) / double(EntryCount));
        QCOMPARE(values.at(2).toInt(), i);
        QCOMPARE(values.at(3).toInt(), EntryCount);
    }
}

void TestAthleteMigrationSafety::plainQtThreadLifecycleIsJoined()
{
    EmptyQtThread thread;
    thread.start();
    QVERIFY(thread.wait(5000));
}

void TestAthleteMigrationSafety::baseCloudAbortStopsRunningReplies()
{
    AbortProbeService service;
    auto *reply = new AbortProbeReply(&service);

    QVERIFY(reply->isRunning());
    service.abortRequests();
    QCOMPARE(reply->abortCalls(), 1);
    QVERIFY(!reply->isRunning());

    service.abortRequests();
    QCOMPARE(reply->abortCalls(), 1);
}

void TestAthleteMigrationSafety::cloudCompressionModesAreExtracted()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudCompression")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    athleteStorage.construct(context.get(), athleteDir);

    const QByteArray payload =
            QByteArray("payload-for-cloud-compression-").repeated(128);
    const QByteArray zipped = zipPayload(payload);
    const QByteArray gzipped = gzipPayload(payload);
    QVERIFY(!zipped.isEmpty());
    QVERIFY(!gzipped.isEmpty());

    const QString name = QStringLiteral("2026_07_08_12_00_00.fit");
    QStringList errors;
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::none, payload, name, errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::zip, zipped,
        name + QStringLiteral(".zip"), errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::gzip, gzipped,
        name + QStringLiteral(".gz"), errors));
    QVERIFY(errors.isEmpty());

    QStringList mismatchErrors;
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::zip, zipped, name, mismatchErrors));
    QVERIFY(!mismatchErrors.isEmpty());

    const QList<QByteArray> extracted =
            athleteMigrationRideFilePayloads();
    athleteStorage.destroy();

    QCOMPARE(athleteMigrationRideFileOpenCalls(), 3);
    QCOMPARE(extracted.size(), 3);
    QCOMPARE(extracted.at(0), payload);
    QCOMPARE(extracted.at(1), payload);
    QCOMPARE(extracted.at(2), payload);
}

void
TestAthleteMigrationSafety::decommissionedCloudServicesAreNotRegistered()
{
    const CloudServiceFactory &factory =
            CloudServiceFactory::instance();

    QVERIFY(factory.service(
        QStringLiteral("SportPlusHealth")) == nullptr);
    QVERIFY(factory.service(
        QStringLiteral("TrainingsTageBuch")) == nullptr);
    QVERIFY(!factory.serviceNames().contains(
        QStringLiteral("SportPlusHealth")));
    QVERIFY(!factory.serviceNames().contains(
        QStringLiteral("TrainingsTageBuch")));
}

void
TestAthleteMigrationSafety::factoryRejectsDecommissionedCloudServices()
{
    CloudServiceFactory &factory =
            CloudServiceFactory::instance();

    auto sportsPlusHealth =
            std::make_unique<SportsPlusHealth>(nullptr);
    const bool sportsPlusHealthAdded =
            factory.addService(sportsPlusHealth.get());
    if (sportsPlusHealthAdded) {
        sportsPlusHealth.release();
    }
    QVERIFY(!sportsPlusHealthAdded);

    auto trainingsTageBuch =
            std::make_unique<TrainingsTageBuch>(nullptr);
    const bool trainingsTageBuchAdded =
            factory.addService(trainingsTageBuch.get());
    if (trainingsTageBuchAdded) {
        trainingsTageBuch.release();
    }
    QVERIFY(!trainingsTageBuchAdded);
}

void
TestAthleteMigrationSafety::decommissionedCloudServicesFailClosed()
{
    SportsPlusHealth sportsPlusHealth(nullptr);
    TrainingsTageBuch trainingsTageBuch(nullptr);
    const std::array<CloudService *, 2> services = {
        &sportsPlusHealth, &trainingsTageBuch};

    for (CloudService *service : services) {
        QCOMPARE(service->capabilities(), 0);
        QVERIFY(service->settings.isEmpty());

        QStringList errors;
        QVERIFY(!service->open(errors));
        QVERIFY(!errors.isEmpty());

        QByteArray payload(
            "private-health-and-activity-payload");
        const QByteArray originalPayload = payload;
        QSignalSpy writes(service, &CloudService::writeComplete);
        QVERIFY(!service->writeFile(
            payload, QStringLiteral("private-ride"), nullptr));
        QCOMPARE(payload, originalPayload);
        QCOMPARE(writes.count(), 0);
        QVERIFY(service->close());
    }
}

void TestAthleteMigrationSafety::nolioTokenRefreshIsSingleFlight()
{
    std::atomic<int> operationCalls{0};
    QSemaphore operationStarted;
    QSemaphore releaseOperation;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-refresh-token");
    NolioRefreshThread first(
        inputToken, operationCalls, operationStarted, releaseOperation);
    NolioRefreshThread second(
        inputToken, operationCalls, operationStarted, releaseOperation);

    first.start();
    const bool firstStarted = operationStarted.tryAcquire(1, 2000);
    second.start();
    QTest::qWait(50);
    releaseOperation.release(2);
    const bool firstJoined = first.wait(2000);
    const bool secondJoined = second.wait(2000);
    const NolioTokenRefreshResult firstResult = first.result();
    const NolioTokenRefreshResult secondResult = second.result();
    const NolioTokenRefreshResult cachedResult =
            NolioTokenRefreshCoordinator::refresh(
        inputToken,
        [&operationCalls]() {
            operationCalls.fetch_add(1, std::memory_order_relaxed);
            return NolioTokenRefreshResult();
        });

    QVERIFY(firstStarted);
    QVERIFY(firstJoined);
    QVERIFY(secondJoined);
    QCOMPARE(operationCalls.load(std::memory_order_relaxed), 1);
    QVERIFY(firstResult.success);
    QVERIFY(secondResult.success);
    QCOMPARE(firstResult.accessToken, secondResult.accessToken);
    QCOMPARE(firstResult.refreshToken, secondResult.refreshToken);
    QCOMPARE(firstResult.refreshedAt, secondResult.refreshedAt);
    QVERIFY(cachedResult.success);
    QCOMPARE(cachedResult.accessToken, firstResult.accessToken);
    QCOMPARE(cachedResult.refreshToken, firstResult.refreshToken);
}

void TestAthleteMigrationSafety::nolioTokenRefreshCacheExpires()
{
    int operationCalls = 0;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-expiring-cache-token");
    const auto operation = [&operationCalls, &inputToken]() {
        ++operationCalls;
        NolioTokenRefreshResult result;
        result.success = true;
        result.accessToken = QStringLiteral("access-%1")
                .arg(operationCalls);
        result.refreshToken = inputToken;
        result.refreshedAt = QStringLiteral("refresh-%1")
                .arg(operationCalls);
        return result;
    };

    const NolioTokenRefreshResult first =
            NolioTokenRefreshCoordinator::refresh(
        inputToken, operation, {},
        std::chrono::milliseconds(1));
    QTest::qWait(10);
    const NolioTokenRefreshResult second =
            NolioTokenRefreshCoordinator::refresh(
        inputToken, operation, {},
        std::chrono::milliseconds(1));

    QVERIFY(first.success);
    QVERIFY(second.success);
    QCOMPARE(operationCalls, 2);
    QCOMPARE(first.accessToken, QStringLiteral("access-1"));
    QCOMPARE(second.accessToken, QStringLiteral("access-2"));
}

void
TestAthleteMigrationSafety::nolioTokenRefreshFollowerCanCancel()
{
    std::atomic<int> operationCalls{0};
    QSemaphore operationStarted;
    QSemaphore releaseOperation;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-cancel-token");
    NolioRefreshThread leader(
        inputToken, operationCalls, operationStarted, releaseOperation);
    NolioRefreshThread follower(
        inputToken, operationCalls, operationStarted, releaseOperation);

    leader.start();
    const bool leaderStarted = operationStarted.tryAcquire(1, 2000);
    follower.start();
    QTest::qWait(50);
    follower.requestInterruption();
    const bool followerStoppedPromptly = follower.wait(1000);

    releaseOperation.release(2);
    const bool leaderJoined = leader.wait(2000);
    const bool followerJoined =
            followerStoppedPromptly || follower.wait(2000);
    const NolioTokenRefreshResult leaderResult = leader.result();
    const NolioTokenRefreshResult followerResult = follower.result();

    QVERIFY(leaderStarted);
    QVERIFY2(followerStoppedPromptly,
             "Nolio refresh follower ignored cancellation");
    QVERIFY(leaderJoined);
    QVERIFY(followerJoined);
    QCOMPARE(operationCalls.load(std::memory_order_relaxed), 1);
    QVERIFY(leaderResult.success);
    QVERIFY(!followerResult.success);
    QVERIFY(followerResult.error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::
withingsSuccessfulResponseUpdatesTokensAndMeasures()
{
    const QByteArray tokenBody(
        "{\"status\":0,\"body\":{"
        "\"access_token\":\"access-2\","
        "\"refresh_token\":\"refresh-2\","
        "\"userid\":42}}");
    const QByteArray measuresBody(
        "{\"status\":0,\"body\":{\"measuregrps\":[{"
        "\"grpid\":1,\"attrib\":0,\"category\":1,"
        "\"date\":1704153600,\"comment\":\"synthetic\","
        "\"measures\":["
        "{\"value\":72500,\"type\":1,\"unit\":-3},"
        "{\"value\":180,\"type\":6,\"unit\":-1}"
        "]}]}}");
    SequencedHttpServer server({
        {tokenBody, 0},
        {measuresBody, 0}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("WithingsSuccess")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
            athleteStorage.construct(context.get(), athleteDir);
    const QString cyclist = athlete->cyclist;
    appsettings->setCValue(
        cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));

    WithingsDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/measures")),
         1000});
    QString error;
    QList<Measure> measures;
    const QDateTime from(
        QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime to(
        QDate(2024, 1, 3), QTime(0, 0), QTimeZone::UTC);
    const bool succeeded = downloader.getBodyMeasures(
        error, from, to, measures);
    const QString accessToken = appsettings->cvalue(
        cyclist, GC_NOKIA_TOKEN).toString();
    const QString refreshToken = appsettings->cvalue(
        cyclist, GC_NOKIA_REFRESH_TOKEN).toString();
    const QString userId = appsettings->cvalue(
        cyclist, GC_WIUSER).toString();
    const QStringList requestPaths = server.requestPaths();
    const QList<QByteArray> requestBytes =
        server.requestBytes();
    athleteStorage.destroy();

    QVERIFY2(succeeded, qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(server.requestCount(), 2);
    QCOMPARE(requestPaths.size(), 2);
    QCOMPARE(requestBytes.size(), 2);
    QCOMPARE(accessToken, QStringLiteral("access-2"));
    QCOMPARE(refreshToken, QStringLiteral("refresh-2"));
    QCOMPARE(userId, QStringLiteral("42"));
    QCOMPARE(measures.size(), 1);
    QCOMPARE(measures.first().source, Measure::Withings);
    QCOMPARE(measures.first().comment, QStringLiteral("synthetic"));
    QVERIFY(qAbs(
        measures.first().values[Measure::WeightKg] - 72.5) < 0.0001);
    QVERIFY(qAbs(
        measures.first().values[Measure::FatPercent] - 18.0) < 0.0001);

    QCOMPARE(requestPaths.first(), QStringLiteral("/token"));
    const QUrl measuresRequest(requestPaths.at(1));
    QCOMPARE(measuresRequest.path(), QStringLiteral("/measures"));
    QVERIFY(measuresRequest.query().isEmpty());
    QVERIFY(requestBytes.at(1).startsWith(
        QByteArrayLiteral("POST /measures HTTP/1.1\r\n")));
    QVERIFY(requestBytes.at(1).toLower().contains(
        QByteArrayLiteral(
            "\r\nauthorization: bearer access-2\r\n")));
    const int bodyOffset =
        requestBytes.at(1).indexOf("\r\n\r\n") + 4;
    QVERIFY(bodyOffset >= 4);
    const QUrlQuery measuresParameters(QString::fromUtf8(
        requestBytes.at(1).mid(bodyOffset)));
    QCOMPARE(
        measuresParameters.queryItemValue(QStringLiteral("action")),
        QStringLiteral("getmeas"));
    QCOMPARE(
        measuresParameters.queryItemValue(QStringLiteral("startdate")),
        QString::number(from.toSecsSinceEpoch()));
    QCOMPARE(
        measuresParameters.queryItemValue(QStringLiteral("enddate")),
        QString::number(to.toSecsSinceEpoch()));
}

void TestAthleteMigrationSafety::
withingsDelayedResponseHandlesAthleteDeletion()
{
    const QByteArray tokenBody(
        "{\"status\":0,\"body\":{"
        "\"access_token\":\"access-2\","
        "\"refresh_token\":\"refresh-2\","
        "\"userid\":42}}");
    SequencedHttpServer server({{tokenBody, 500}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("WithingsDeletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    Athlete *athlete = new Athlete(context.get(), athleteDir);
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));
    QPointer<Athlete> guardedAthlete(athlete);

    WithingsDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/measures")),
         2000});
    QTimer::singleShot(
        30, context.get(), [guardedAthlete]() {
            delete guardedAthlete.data();
        });
    QString error;
    QList<Measure> measures;
    const QDateTime to = QDateTime::currentDateTimeUtc();
    QElapsedTimer timer;
    timer.start();
    const bool succeeded = downloader.getBodyMeasures(
        error, to.addDays(-1), to, measures);
    const qint64 elapsedMs = timer.elapsed();
    if (guardedAthlete) delete guardedAthlete.data();

    QVERIFY(!succeeded);
    QVERIFY(guardedAthlete.isNull());
    QVERIFY(!context->athlete);
    QVERIFY(error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY2(elapsedMs < 1000,
             "Withings request ignored athlete deletion");
}

void TestAthleteMigrationSafety::
tredictSuccessfulResponsesUpdateTokensAndEncodeHrvRange()
{
    const QByteArray firstTokenBody(
        "{\"access_token\":\"access-2\","
        "\"refresh_token\":\"refresh-2\","
        "\"expires_in\":3600}");
    const QByteArray bodyMeasures(
        "{\"bodyvalues\":["
        "{\"timestamp\":\"2024-01-02T12:00:00Z\","
        "\"weightInKilograms\":80.0,"
        "\"bodyFatInPercent\":20.0,"
        "\"muscleMassInPercent\":40.0},"
        "{\"timestamp\":\"2024-01-05T12:00:00Z\","
        "\"weightInKilograms\":99.0}]}");
    const QByteArray secondTokenBody(
        "{\"access_token\":\"access-3\","
        "\"refresh_token\":\"refresh-3\","
        "\"expires_in\":3600}");
    const QByteArray hrvMeasures(
        "{\"hrv\":{\"20240102\":[45.5,42.0]}}");
    SequencedHttpServer server({
        {firstTokenBody, 0},
        {bodyMeasures, 0},
        {secondTokenBody, 0},
        {hrvMeasures, 0}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("TredictSuccess")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
            athleteStorage.construct(context.get(), athleteDir);
    const QString cyclist = athlete->cyclist;
    appsettings->setCValue(
        cyclist, GC_TREDICT_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));
    appsettings->setCValue(
        cyclist, GC_TREDICT_TOKEN,
        QStringLiteral("access-1"));

    TredictMeasuresDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/body")),
         server.endpoint(QStringLiteral("/hrv")),
         1000});
    const QDateTime from(
        QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime to(
        QDate(2024, 1, 3), QTime(0, 0), QTimeZone::UTC);
    QString bodyError;
    QList<Measure> body;
    const bool bodySucceeded = downloader.getBodyMeasures(
        bodyError, from, to, body);
    QString hrvError;
    QList<Measure> hrv;
    const bool hrvSucceeded = downloader.getHrvMeasures(
        hrvError, from, to, hrv);
    const QString accessToken = appsettings->cvalue(
        cyclist, GC_TREDICT_TOKEN).toString();
    const QString refreshToken = appsettings->cvalue(
        cyclist, GC_TREDICT_REFRESH_TOKEN).toString();
    const QStringList requestPaths = server.requestPaths();
    athleteStorage.destroy();

    QVERIFY2(bodySucceeded, qPrintable(bodyError));
    QVERIFY(bodyError.isEmpty());
    QCOMPARE(body.size(), 1);
    QCOMPARE(body.first().source, Measure::Tredict);
    QCOMPARE(body.first().originalSource, QStringLiteral("Tredict"));
    QVERIFY(qAbs(
        body.first().values[Measure::WeightKg] - 80.0) < 0.0001);
    QVERIFY(qAbs(
        body.first().values[Measure::FatKg] - 16.0) < 0.0001);
    QVERIFY(qAbs(
        body.first().values[Measure::MuscleKg] - 32.0) < 0.0001);
    QVERIFY(qAbs(
        body.first().values[Measure::LeanKg] - 64.0) < 0.0001);

    QVERIFY2(hrvSucceeded, qPrintable(hrvError));
    QVERIFY(hrvError.isEmpty());
    QCOMPARE(hrv.size(), 1);
    QCOMPARE(hrv.first().source, Measure::Tredict);
    QVERIFY(qAbs(hrv.first().values[0] - 45.5) < 0.0001);

    QCOMPARE(server.requestCount(), 4);
    QCOMPARE(requestPaths.size(), 4);
    QCOMPARE(requestPaths.at(0), QStringLiteral("/token"));
    QCOMPARE(requestPaths.at(1), QStringLiteral("/body"));
    QCOMPARE(requestPaths.at(2), QStringLiteral("/token"));
    const QUrl hrvRequest(requestPaths.at(3));
    QCOMPARE(hrvRequest.path(), QStringLiteral("/hrv"));
    const QUrlQuery hrvQuery(hrvRequest);
    QCOMPARE(
        hrvQuery.queryItemValue(QStringLiteral("startDate")),
        to.toUTC().toString(Qt::ISODate));
    QCOMPARE(
        hrvQuery.queryItemValue(QStringLiteral("endDate")),
        from.toUTC().toString(Qt::ISODate));
    QCOMPARE(accessToken, QStringLiteral("access-3"));
    QCOMPARE(refreshToken, QStringLiteral("refresh-3"));
}

void TestAthleteMigrationSafety::withingsTokenRequestTimesOut()
{
    const QByteArray tokenBody(
        "{\"status\":0,\"body\":{"
        "\"access_token\":\"access\","
        "\"refresh_token\":\"refresh-2\","
        "\"userid\":1}}");
    const QByteArray measuresBody(
        "{\"status\":0,\"body\":{\"measuregrps\":[]}}");
    SequencedHttpServer server({
        {tokenBody, 500},
        {measuresBody, 0}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("WithingsTokenTimeout")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
            athleteStorage.construct(context.get(), athleteDir);
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));

    WithingsDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/measures")),
         30});
    QString error;
    QList<Measure> measures;
    const QDateTime to = QDateTime::currentDateTimeUtc();
    QElapsedTimer timer;
    timer.start();
    const bool succeeded = downloader.getBodyMeasures(
        error, to.addDays(-1), to, measures);
    const qint64 elapsedMs = timer.elapsed();
    athleteStorage.destroy();

    QVERIFY(server.requestCount() >= 1);
    QVERIFY(!succeeded);
    QVERIFY(error.contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
    QVERIFY(measures.isEmpty());
    QVERIFY2(elapsedMs < 300,
             "Withings token request ignored its deadline");
}

void TestAthleteMigrationSafety::withingsMeasuresRequestTimesOut()
{
    const QByteArray tokenBody(
        "{\"status\":0,\"body\":{"
        "\"access_token\":\"access\","
        "\"refresh_token\":\"refresh-2\","
        "\"userid\":1}}");
    const QByteArray measuresBody(
        "{\"status\":0,\"body\":{\"measuregrps\":[]}}");
    SequencedHttpServer server({
        {tokenBody, 0},
        {measuresBody, 500}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("WithingsMeasuresTimeout")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
            athleteStorage.construct(context.get(), athleteDir);
    appsettings->setCValue(
        athlete->cyclist, GC_NOKIA_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));

    WithingsDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/measures")),
         30});
    QString error;
    QList<Measure> measures;
    const QDateTime to = QDateTime::currentDateTimeUtc();
    QElapsedTimer timer;
    timer.start();
    const bool succeeded = downloader.getBodyMeasures(
        error, to.addDays(-1), to, measures);
    const qint64 elapsedMs = timer.elapsed();
    athleteStorage.destroy();

    QVERIFY(server.requestCount() >= 2);
    QVERIFY(!succeeded);
    QVERIFY(error.contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
    QVERIFY(measures.isEmpty());
    QVERIFY2(elapsedMs < 300,
             "Withings measures request ignored its deadline");
}

void TestAthleteMigrationSafety::tredictMeasuresRequestTimesOut()
{
    const QByteArray tokenBody(
        "{\"access_token\":\"access-2\","
        "\"refresh_token\":\"refresh-2\","
        "\"expires_in\":3600}");
    const QByteArray measuresBody(
        "{\"bodyvalues\":[]}");
    SequencedHttpServer server({
        {tokenBody, 0},
        {measuresBody, 500}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("TredictMeasuresTimeout")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
            athleteStorage.construct(context.get(), athleteDir);
    appsettings->setCValue(
        athlete->cyclist, GC_TREDICT_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));
    appsettings->setCValue(
        athlete->cyclist, GC_TREDICT_TOKEN,
        QStringLiteral("access-1"));

    TredictMeasuresDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/body")),
         server.endpoint(QStringLiteral("/hrv")),
         30});
    QString error;
    QList<Measure> measures;
    const QDateTime to = QDateTime::currentDateTimeUtc();
    QElapsedTimer timer;
    timer.start();
    const bool succeeded = downloader.getBodyMeasures(
        error, to.addDays(-1), to, measures);
    const qint64 elapsedMs = timer.elapsed();
    athleteStorage.destroy();

    QVERIFY(server.requestCount() >= 2);
    QVERIFY(!succeeded);
    QVERIFY(error.contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
    QVERIFY(measures.isEmpty());
    QVERIFY2(elapsedMs < 300,
             "Tredict measures request ignored its deadline");
}

void TestAthleteMigrationSafety::
tredictTokenRefreshHandlesAthleteDeletion()
{
    const QByteArray tokenBody(
        R"json({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})json");
    SequencedHttpServer server({
        {tokenBody, 2000}});
    QVERIFY(server.isListening());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("TredictTokenDeletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);
    std::unique_ptr<Context> context(
        createAthleteMigrationTestContext());
    Athlete *athlete = new Athlete(context.get(), athleteDir);
    appsettings->setCValue(
        athlete->cyclist, GC_TREDICT_REFRESH_TOKEN,
        QStringLiteral("refresh-1"));
    appsettings->setCValue(
        athlete->cyclist, GC_TREDICT_TOKEN,
        QStringLiteral("access-1"));
    QPointer<Athlete> guardedAthlete(athlete);

    TredictMeasuresDownload downloader(
        context.get(),
        {server.endpoint(QStringLiteral("/token")),
         server.endpoint(QStringLiteral("/body")),
         server.endpoint(QStringLiteral("/hrv")),
         5000});
    QTimer::singleShot(
        30, context.get(), [guardedAthlete]() {
            delete guardedAthlete.data();
        });

    QString error;
    QList<Measure> measures;
    const QDateTime to = QDateTime::currentDateTimeUtc();
    QElapsedTimer timer;
    timer.start();
    const bool succeeded = downloader.getBodyMeasures(
        error, to.addDays(-1), to, measures);
    const qint64 elapsedMs = timer.elapsed();
    if (guardedAthlete) delete guardedAthlete.data();

    QVERIFY(server.requestCount() >= 1);
    QVERIFY(!succeeded);
    QVERIFY(guardedAthlete.isNull());
    QVERIFY(!context->athlete);
    QVERIFY(error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY2(elapsedMs < 1000,
             "Tredict token refresh ignored athlete deletion");
}

void TestAthleteMigrationSafety::networkReplyWaitTimesOut()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    QNetworkAccessManager manager;
    QNetworkReply *reply = manager.post(
        QNetworkRequest(QUrl(endpoint)), QByteArray());

    QElapsedTimer timer;
    timer.start();
    const NetworkReplyWaitResult result =
            waitForNetworkReply(reply, 30);
    const qint64 elapsedMs = timer.elapsed();
    const bool connected = server.hasPendingConnections();
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;

    if (connection) {
        connection->abort();
        connection->deleteLater();
    }
    reply->deleteLater();

    QVERIFY2(connected,
             "network wait did not connect to the local test server");
    QVERIFY(result == NetworkReplyWaitResult::TimedOut);
    QVERIFY2(elapsedMs >= 15,
             "network wait timed out substantially too early");
    QVERIFY2(elapsedMs < 1000,
             "network wait did not respect its timeout");
}

void
TestAthleteMigrationSafety::
networkReplyWaitPrefersPreexistingInterruption()
{
    AbortProbeReply reply(nullptr);
    reply.finish();

    const NetworkReplyWaitResult result =
            waitForNetworkReply(
                &reply, 1000, []() { return true; });

    QVERIFY(result == NetworkReplyWaitResult::Interrupted);
    QCOMPARE(reply.abortCalls(), 1);
}

void TestAthleteMigrationSafety::oauthRefreshHandlesImmediateReply()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QByteArray request;
    bool responded = false;
    connect(
        &server, &QTcpServer::newConnection, &server,
        [&server, &request, &responded]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *connection = server.nextPendingConnection();
                connect(
                    connection, &QTcpSocket::readyRead, connection,
                    [connection, &request, &responded]() {
                        request.append(connection->readAll());
                        if (responded
                            || !request.contains("\r\n\r\n")) {
                            return;
                        }

                        const QByteArray body(
                            "{\"access_token\":\"new-access\","
                            "\"refresh_token\":\"new-refresh\","
                            "\"expires_in\":3600}");
                        const QByteArray response =
                            QByteArray("HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: ")
                            + QByteArray::number(body.size())
                            + QByteArray("\r\nConnection: close\r\n\r\n")
                            + body;
                        connection->write(response);
                        connection->flush();
                        responded = true;
                    });
            }
        });

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint);
    worker.start();

    const bool completed = waitUntil(
        [&worker]() { return worker.isFinished(); }, 3000);
    if (!completed) worker.requestInterruption();
    const bool joined = worker.wait(completed ? 1000 : 5000);
    const OAuthRefreshThread::Result result = worker.result();

    QVERIFY2(joined, "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(completed, "OAuth refresh did not finish promptly");
    QVERIFY(responded);
    QVERIFY(result.succeeded);
    QCOMPARE(result.error, QString());
    QCOMPARE(result.accessToken, QStringLiteral("new-access"));
    QCOMPARE(result.refreshToken, QStringLiteral("new-refresh"));
    QCOMPARE(result.expiresIn, 3600);
}

void TestAthleteMigrationSafety::oauthRefreshTimesOut()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint, 30);

    QElapsedTimer timer;
    timer.start();
    worker.start();

    const bool connected = waitUntil(
        [&server]() { return server.hasPendingConnections(); }, 1000);
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;
    const bool completed = waitUntil(
        [&worker]() { return worker.isFinished(); }, 1000);
    const qint64 elapsedMs = timer.elapsed();

    if (!completed) worker.requestInterruption();
    const bool joined = worker.wait(completed ? 1000 : 5000);
    const OAuthRefreshThread::Result result = worker.result();

    if (connection) {
        connection->abort();
        connection->deleteLater();
    }

    QVERIFY2(joined, "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(connected,
             "OAuth refresh did not connect to the local test server");
    QVERIFY2(completed, "OAuth refresh did not respect its timeout");
    QVERIFY(elapsedMs < 1000);
    QVERIFY(!result.succeeded);
    QVERIFY(result.error.contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::oauthRefreshHonorsThreadInterruption()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint);
    worker.start();

    const bool connected = waitUntil(
        [&server]() { return server.hasPendingConnections(); }, 2000);
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;

    QElapsedTimer timer;
    timer.start();
    worker.requestInterruption();
    const bool stoppedPromptly = worker.wait(1000);
    const qint64 stopElapsedMs = timer.elapsed();

    if (!stoppedPromptly) {
        if (connection) connection->abort();
        server.close();
    }
    const bool cleanupStopped =
            stoppedPromptly || worker.wait(5000);
    const OAuthRefreshThread::Result result = worker.result();

    QVERIFY2(cleanupStopped,
             "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(connected,
             "OAuth refresh did not connect to the local test server");
    QVERIFY2(stoppedPromptly,
             "OAuth refresh ignored QThread interruption");
    QVERIFY(stopElapsedMs < 1000);
    QVERIFY(!result.succeeded);
    QVERIFY(result.error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::sslWarningsAreMarshaledToGuiThread()
{
    bool warningHandled = false;
    bool shownOnGuiThread = false;
    const std::shared_ptr<GuiMessageResponseState> response =
            answerNextMessageBoxOnGuiThread(
                warningHandled, shownOnGuiThread);

    SslWarningThread worker;
    worker.start();
    const bool completed = waitUntil(
        [&warningHandled]() { return warningHandled; },
        2000);
    response->active = false;
    worker.release();
    const bool joined = worker.wait(5000);

    QVERIFY(completed);
    QVERIFY(joined);
    QVERIFY(warningHandled);
    QVERIFY(shownOnGuiThread);
}

void TestAthleteMigrationSafety::
stalledGuiBoundsAndCoalescesSslWarnings()
{
    constexpr int DuplicateCount = 4096;
    constexpr int DistinctCount = 64;
    constexpr quint64 SubmittedCount = DuplicateCount + DistinctCount;

    enableCloudSslWarningCapture();
    SslWarningFloodThread worker(DuplicateCount, DistinctCount);
    worker.start();
    const bool producerFinishedWithoutGuiDispatch = worker.wait(15000);
    const CloudGuiHandoffQueueStats stalledStats =
        cloudSslWarningQueueStatsForTest();
    const bool warningDelivered = waitUntil(
        []() { return cloudSslWarningCaptureCalls() == 1; },
        5000);
    const quint64 deliveredOccurrences =
        cloudSslWarningCapturedOccurrences();
    const quint64 deliveredOmitted =
        cloudSslWarningCapturedOmitted();
    const bool usedGuiThread =
        cloudSslWarningCaptureUsedGuiThread();
    const CloudGuiHandoffQueueStats drainedStats =
        cloudSslWarningQueueStatsForTest();
    disableCloudSslWarningCapture();

    QVERIFY(producerFinishedWithoutGuiDispatch);
    QVERIFY(stalledStats.currentItems <= stalledStats.maximumItems);
    QVERIFY(stalledStats.peakItems <= stalledStats.maximumItems);
    QVERIFY(stalledStats.currentBytes <= stalledStats.maximumBytes);
    QVERIFY(stalledStats.peakBytes <= stalledStats.maximumBytes);
    QCOMPARE(stalledStats.dispatchPosts, quint64(1));
    QVERIFY(stalledStats.coalesced >= quint64(DuplicateCount - 1));
    QVERIFY(stalledStats.rejected > 0);
    QCOMPARE(
        stalledStats.queuedOccurrences
            + stalledStats.omittedOccurrences,
        SubmittedCount);
    QVERIFY(warningDelivered);
    QVERIFY(usedGuiThread);
    QCOMPARE(deliveredOccurrences + deliveredOmitted, SubmittedCount);
    QCOMPARE(drainedStats.currentItems, 0);
    QCOMPARE(drainedStats.currentBytes, qint64(0));
    QCOMPARE(drainedStats.dispatchCalls, quint64(1));
}

int main(int argc, char *argv[])
{
    if (LocalFileStoreProcess::isHelperInvocation(argc, argv)) {
        QCoreApplication helperApplication(argc, argv);
        return LocalFileStoreProcess::runHelper(
            helperApplication.arguments());
    }

    QApplication application(argc, argv);
    if (!LocalFileStoreProcess::initializeReaper()) return 3;

    int result = 0;
    if (application.arguments().contains(
            QString::fromLatin1(ReaperAffinityChildSwitch))) {
        result = runReaperAffinityChild(application);
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ReaperShutdownRaceChildSwitch))) {
        result = runReaperShutdownRaceChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ReaperDrainDeadlineChildSwitch))) {
        result = runReaperDrainDeadlineChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ReaperRetainedRetryChildSwitch))) {
        result = runReaperRetainedRetryChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ReaperFailedShutdownStateChildSwitch))) {
        result = runReaperFailedShutdownStateChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ReaperDispatchRaceChildSwitch))) {
        result = runReaperDispatchRaceChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                DeferredLoadExceptionChildSwitch))) {
        result = runDeferredLoadExceptionChild();
    } else if (application.arguments().contains(
            QString::fromLatin1(
                ManualMeasuresDeletionChildSwitch))) {
        result = runManualMeasuresDeletionChild();
    } else {
        TestAthleteMigrationSafety test;
        result = QTest::qExec(&test, argc, argv);
    }

    if (!LocalFileStoreProcess::shutdownReaper()
        && result == 0) {
        result = 4;
    }
    return result;
}

#include "testAthleteMigrationSafety.moc"
