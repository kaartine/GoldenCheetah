#include "Cloud/OpenDataExport.h"
#include "Cloud/OpenDataTransport.h"
#include "Cloud/OpenDataUploadWorker.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <memory>
#include <utility>

namespace {

using namespace OpenDataExport;

class ScriptedHttpServer final : public QObject
{
public:
    explicit ScriptedHttpServer(
        bool holdUpload = false,
        QByteArray discoveryBody =
            QByteArrayLiteral("discovery"))
        : holdUpload_(holdUpload)
        , discoveryBody_(std::move(discoveryBody))
    {
        connect(
            &server_, &QTcpServer::newConnection,
            this, [this]() {
                while (server_.hasPendingConnections()) {
                    QTcpSocket *socket =
                        server_.nextPendingConnection();
                    if (!socket) continue;
                    socket->setParent(this);
                    connect(
                        socket, &QTcpSocket::readyRead,
                        this, [this, socket]() {
                            consume(socket);
                        });
                    connect(
                        socket, &QObject::destroyed,
                        this, [this, socket]() {
                            pending_.remove(socket);
                        });
                }
            });
    }

    bool listen()
    {
        return server_.listen(QHostAddress::LocalHost);
    }

    QUrl url(const QString &path = QStringLiteral("/")) const
    {
        QUrl result;
        result.setScheme(QStringLiteral("http"));
        result.setHost(QStringLiteral("127.0.0.1"));
        result.setPort(server_.serverPort());
        result.setPath(path);
        return result;
    }

    int requestCount() const
    {
        return requests_.size();
    }

    QByteArray request(int index) const
    {
        return requests_.value(index);
    }

private:
    void consume(QTcpSocket *socket)
    {
        QByteArray &buffer = pending_[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd =
            buffer.indexOf(QByteArrayLiteral("\r\n\r\n"));
        if (headerEnd < 0) return;

        qint64 contentLength = 0;
        const QList<QByteArray> lines =
            buffer.left(headerEnd).split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (!line.toLower().startsWith("content-length:"))
                continue;
            bool ok = false;
            const qint64 parsed =
                line.mid(line.indexOf(':') + 1).trimmed().toLongLong(&ok);
            if (ok && parsed >= 0) contentLength = parsed;
        }
        const qint64 requestSize =
            qint64(headerEnd) + 4 + contentLength;
        if (buffer.size() < requestSize) return;

        requests_.append(buffer.left(requestSize));
        pending_.remove(socket);
        const int index = requests_.size() - 1;
        if (holdUpload_ && index == 2) return;

        const QByteArray body =
            index == 0
                ? discoveryBody_
                : QByteArrayLiteral("OK");
        const QByteArray status =
            index == 2
                ? QByteArrayLiteral("201 Created")
                : QByteArrayLiteral("200 OK");
        socket->write(
            "HTTP/1.1 " + status + "\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QHash<QTcpSocket *, QByteArray> pending_;
    QList<QByteArray> requests_;
    bool holdUpload_ = false;
    QByteArray discoveryBody_;
};

Request sealedRequest(const QString &archivePath, QString &error)
{
    Request request;
    request.athleteId = QStringLiteral("athlete-id");
    request.cyclist = QStringLiteral("settings-key");
    request.rideCount = 1;
    request.formatVersion = 1;

    ArchiveWriter writer(archivePath);
    QBuffer summary;
    summary.setData(QByteArrayLiteral("{\"RIDES\":1}"));
    if (!summary.open(QIODevice::ReadOnly)
        || !writer.addFile(
            QStringLiteral("athlete-id.json"), &summary, error)
        || !writer.finish(error)
        || !describeArchive(
            archivePath,
            request.archiveSize,
            request.archiveSha256,
            error)) {
        return {};
    }
    request.archivePath = archivePath;
    return request;
}

OpenDataTransport::Policy localPolicy(
    const ScriptedHttpServer &server,
    int uploadTimeoutMs = 2000)
{
    OpenDataTransport::Policy policy;
    const QUrl root = server.url();
    policy.discoveryUrl = server.url(QStringLiteral("/discovery"));
    policy.parseServerRoots =
        [root](const QByteArray &, QString *error) {
            if (error) error->clear();
            return QList<QUrl>{root};
        };
    policy.metricsUrl = [](const QUrl &serverRoot) {
        QUrl result(serverRoot);
        result.setPath(QStringLiteral("/metrics"));
        return result;
    };
    policy.makeRequest = [](const QUrl &url) {
        QNetworkRequest request(url);
        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::ManualRedirectPolicy);
        return request;
    };
    policy.requestTimeoutMs = 2000;
    policy.serverSearchTimeoutMs = 2000;
    policy.uploadTimeoutMs = uploadTimeoutMs;
    return policy;
}

OpenDataExport::UploadTask uploadTask(
    const OpenDataTransport::Policy &policy)
{
    return [policy](
               const Request &request,
               const CancellationCheck &cancelled,
               const ProgressCallback &progress) {
        return OpenDataTransport::upload(
            request,
            cancelled,
            progress,
            QByteArrayLiteral("unit-secret"),
            policy);
    };
}

} // namespace

class TestOpenDataTransport : public QObject
{
    Q_OBJECT

private slots:
    void streamsSealedArchiveAsMultipart();
    void replyBuffersAreBoundedInQt();
    void changedArchiveStopsBeforeNetwork();
    void oversizedDiscoveryStopsBeforeServerSelection();
    void uploadDeadlineIsReported();
    void cancellationAbortsActiveUpload();
};

void TestOpenDataTransport::streamsSealedArchiveAsMultipart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    ScriptedHttpServer server;
    QVERIFY(server.listen());

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(request),
        uploadTask(localPolicy(server)));
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(succeeded.count(), 1, 5000);
    QVERIFY(worker.wait(2000));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(server.requestCount(), 3);
    const QByteArray post = server.request(2);
    QVERIFY(post.startsWith("POST /metrics HTTP/1.1\r\n"));
    QVERIFY(post.contains("name=\"secret\""));
    QVERIFY(post.contains("unit-secret"));
    QVERIFY(post.contains("name=\"id\""));
    QVERIFY(post.contains("athlete-id"));
    QVERIFY(post.contains("filename=\"athlete-id.zip\""));

    QFile archive(request.archivePath);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    const QByteArray archiveBytes = archive.readAll();
    QVERIFY(!archiveBytes.isEmpty());
    QVERIFY(post.contains(archiveBytes));
}

void TestOpenDataTransport::replyBuffersAreBoundedInQt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    ScriptedHttpServer server;
    QVERIFY(server.listen());
    OpenDataTransport::Policy policy = localPolicy(server);
    QList<qint64> readBufferLimits;
    policy.replyObserver =
        [&readBufferLimits](const QNetworkReply *reply) {
            readBufferLimits.append(reply->readBufferSize());
        };

    const OpenDataExport::UploadResult result =
        OpenDataTransport::upload(
            request,
            {},
            {},
            QByteArrayLiteral("unit-secret"),
            policy);

    QCOMPARE(
        result.status,
        OpenDataExport::UploadResult::Status::Succeeded);
    QCOMPARE(readBufferLimits.size(), 3);
    QCOMPARE(readBufferLimits.at(0), qint64(64 * 1024 + 1));
    QCOMPARE(readBufferLimits.at(1), qint64(1024 + 1));
    QCOMPARE(readBufferLimits.at(2), qint64(1024 + 1));
}

void TestOpenDataTransport::changedArchiveStopsBeforeNetwork()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    QFile archive(request.archivePath);
    QVERIFY(archive.open(QIODevice::Append));
    QCOMPARE(archive.write("changed"), qint64(7));
    archive.close();
    ScriptedHttpServer server;
    QVERIFY(server.listen());

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(request),
        uploadTask(localPolicy(server)));
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QVERIFY(worker.wait(2000));
    QCOMPARE(server.requestCount(), 0);
}

void
TestOpenDataTransport::oversizedDiscoveryStopsBeforeServerSelection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    ScriptedHttpServer server(
        false, QByteArray(65 * 1024, 'x'));
    QVERIFY(server.listen());

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(request),
        uploadTask(localPolicy(server)));
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QVERIFY(worker.wait(2000));
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(server.requestCount(), 1);
}

void TestOpenDataTransport::uploadDeadlineIsReported()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    ScriptedHttpServer server(true);
    QVERIFY(server.listen());

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(request),
        uploadTask(localPolicy(server, 100)));
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    worker.start();

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QVERIFY(worker.wait(2000));
    QCOMPARE(server.requestCount(), 3);
    QVERIFY(
        failed.at(0).at(0).toString().contains(
            QStringLiteral("timed out"), Qt::CaseInsensitive));
}

void TestOpenDataTransport::cancellationAbortsActiveUpload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const Request request = sealedRequest(
        directory.filePath(QStringLiteral("opendata.zip")), error);
    QVERIFY2(!request.archivePath.isEmpty(), qPrintable(error));
    ScriptedHttpServer server(true);
    QVERIFY(server.listen());

    OpenDataUploadWorker worker(
        std::make_shared<const Request>(request),
        uploadTask(localPolicy(server, 30000)));
    QSignalSpy succeeded(&worker, &OpenDataUploadWorker::succeeded);
    QSignalSpy failed(&worker, &OpenDataUploadWorker::failed);
    QSignalSpy finished(&worker, &OpenDataUploadWorker::finished);
    worker.start();
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 3, 5000);

    QElapsedTimer elapsed;
    elapsed.start();
    worker.requestCancellation();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QVERIFY(worker.wait(2000));

    QVERIFY2(
        elapsed.elapsed() < 1000,
        "Cancelling the production upload did not abort promptly");
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 0);
}

QTEST_MAIN(TestOpenDataTransport)
#include "testOpenDataTransport.moc"
