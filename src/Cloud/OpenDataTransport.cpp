/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataTransport.h"

#include "NetworkReplyWait.h"
#include "OpenDataEndpointPolicy.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslError>

#include <algorithm>
#include <limits>
#include <memory>

namespace {

constexpr qint64 MaxDiscoveryBytes = 64 * 1024;
constexpr qint64 MaxSmallResponseBytes = 1024;

class BoundedReplyBody final
{
public:
    BoundedReplyBody(QNetworkReply *reply, qint64 limit)
        : reply_(reply)
        , limit_(limit)
    {
        if (reply_) {
            const qint64 readBufferLimit =
                limit_ < std::numeric_limits<qint64>::max()
                    ? limit_ + 1
                    : limit_;
            reply_->setReadBufferSize(readBufferLimit);
        }
        connection_ = QObject::connect(
            reply_, &QIODevice::readyRead,
            reply_,
            [this]() {
                drain();
            });
    }

    ~BoundedReplyBody()
    {
        QObject::disconnect(connection_);
    }

    void drain()
    {
        if (!reply_ || exceeded_) return;
        while (reply_->bytesAvailable() > 0) {
            const QByteArray chunk =
                reply_->read(std::min<qint64>(
                    reply_->bytesAvailable(), 16 * 1024));
            if (chunk.isEmpty()) break;
            const qint64 remaining =
                std::max<qint64>(0, limit_ - body_.size());
            if (remaining > 0)
                body_.append(chunk.left(remaining));
            bytesSeen_ += chunk.size();
            if (bytesSeen_ <= limit_) continue;
            exceeded_ = true;
            reply_->abort();
            break;
        }
    }

    const QByteArray &body() const
    {
        return body_;
    }

    bool exceeded() const
    {
        return exceeded_;
    }

private:
    QNetworkReply *reply_ = nullptr;
    qint64 limit_ = 0;
    qint64 bytesSeen_ = 0;
    QByteArray body_;
    QMetaObject::Connection connection_;
    bool exceeded_ = false;
};

bool cancellationRequested(
    const OpenDataExport::CancellationCheck &cancelled)
{
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

void reportProgress(
    const OpenDataExport::ProgressCallback &progress,
    int step,
    int lastStep,
    const QString &message)
{
    if (progress) progress(step, lastStep, message);
}

bool replySucceeded(const QNetworkReply *reply)
{
    return reply
        && OpenDataEndpointPolicy::isSuccessfulResponse(
            reply->error(),
            reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt(),
            reply->attribute(
                QNetworkRequest::RedirectionTargetAttribute).toUrl());
}

bool validPolicy(const OpenDataTransport::Policy &policy)
{
    return policy.discoveryUrl.isValid()
        && !policy.discoveryUrl.isRelative()
        && policy.parseServerRoots
        && policy.metricsUrl
        && policy.makeRequest
        && policy.requestTimeoutMs > 0
        && policy.serverSearchTimeoutMs > 0
        && policy.uploadTimeoutMs > 0;
}

bool validAthleteId(const QString &athleteId)
{
    static const QRegularExpression Allowed(
        QStringLiteral("^[A-Za-z0-9_{}-]{1,128}$"));
    return Allowed.match(athleteId).hasMatch();
}

void observeReply(
    const OpenDataTransport::Policy &policy,
    const QNetworkReply *reply)
{
    if (policy.replyObserver)
        policy.replyObserver(reply);
}

} // namespace

namespace OpenDataTransport {

Policy productionPolicy()
{
    Policy policy;
    policy.discoveryUrl = OpenDataEndpointPolicy::discoveryUrl();
    policy.parseServerRoots =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery;
    policy.metricsUrl = OpenDataEndpointPolicy::metricsUrl;
    policy.makeRequest = OpenDataEndpointPolicy::makeRequest;
    return policy;
}

OpenDataExport::UploadResult upload(
    const OpenDataExport::Request &request,
    const OpenDataExport::CancellationCheck &cancelled,
    const OpenDataExport::ProgressCallback &progress,
    const QByteArray &secret,
    const Policy &policy)
{
    constexpr int LastStep = 5;

    if (cancellationRequested(cancelled))
        return OpenDataExport::UploadResult::cancelled();
    if (!validPolicy(policy))
        return OpenDataExport::UploadResult::failed(
            QObject::tr("Invalid OpenData transport policy"));
    if (!validAthleteId(request.athleteId))
        return OpenDataExport::UploadResult::failed(
            QObject::tr("Invalid OpenData athlete identifier"));

    QString archiveError;
    QFile archiveFile;
    const OpenDataExport::ArchiveValidationResult archiveValidation =
        OpenDataExport::openValidatedArchive(
            request,
            archiveFile,
            cancelled,
            archiveError);
    if (archiveValidation
        == OpenDataExport::ArchiveValidationResult::Cancelled) {
        return OpenDataExport::UploadResult::cancelled();
    }
    if (archiveValidation
        != OpenDataExport::ArchiveValidationResult::Valid) {
        return OpenDataExport::UploadResult::failed(archiveError);
    }

    reportProgress(
        progress, 1, LastStep, QObject::tr("Fetching server list."));
    QNetworkAccessManager manager;
    QString sslError;
    QObject::connect(
        &manager, &QNetworkAccessManager::sslErrors,
        &manager,
        [&sslError](
            QNetworkReply *,
            const QList<QSslError> &errors) {
            QStringList descriptions;
            for (const QSslError &error : errors)
                descriptions.append(error.errorString());
            sslError = QObject::tr("SSL error(s): %1")
                           .arg(descriptions.join(
                               QStringLiteral(", ")));
        });

    std::unique_ptr<QNetworkReply> discoveryReply(
        manager.get(policy.makeRequest(policy.discoveryUrl)));
    BoundedReplyBody discoveryBody(
        discoveryReply.get(), MaxDiscoveryBytes);
    observeReply(policy, discoveryReply.get());
    const NetworkReplyWaitResult discoveryWait = waitForNetworkReply(
        discoveryReply.get(), policy.requestTimeoutMs, cancelled);
    discoveryBody.drain();
    if (discoveryWait == NetworkReplyWaitResult::Interrupted)
        return OpenDataExport::UploadResult::cancelled();
    if (discoveryBody.exceeded()) {
        return OpenDataExport::UploadResult::failed(
            QObject::tr(
                "Invalid server list, please try again later"));
    }
    if (discoveryWait != NetworkReplyWaitResult::Finished
        || !replySucceeded(discoveryReply.get())) {
        return OpenDataExport::UploadResult::failed(
            sslError.isEmpty()
                ? QObject::tr("Network problem reading server list")
                : sslError);
    }

    QString discoveryError;
    const QList<QUrl> servers = policy.parseServerRoots(
        discoveryBody.body(), &discoveryError);
    if (!discoveryError.isEmpty()) {
        return OpenDataExport::UploadResult::failed(
            QObject::tr("Invalid server list, please try again later"));
    }

    reportProgress(
        progress, 2, LastStep, QObject::tr("Finding an available server."));
    QDeadlineTimer serverSearch(policy.serverSearchTimeoutMs);
    QUrl server;
    for (const QUrl &candidate : servers) {
        if (cancellationRequested(cancelled))
            return OpenDataExport::UploadResult::cancelled();
        const qint64 remaining = serverSearch.remainingTime();
        if (remaining <= 0) break;

        const QUrl metrics = policy.metricsUrl(candidate);
        if (!metrics.isValid() || metrics.isRelative()) continue;
        std::unique_ptr<QNetworkReply> pingReply(
            manager.get(policy.makeRequest(metrics)));
        BoundedReplyBody pingBody(
            pingReply.get(), MaxSmallResponseBytes);
        observeReply(policy, pingReply.get());
        const NetworkReplyWaitResult pingWait = waitForNetworkReply(
            pingReply.get(),
            int(std::min<qint64>(
                policy.requestTimeoutMs, remaining)),
            cancelled);
        pingBody.drain();
        if (pingWait == NetworkReplyWaitResult::Interrupted)
            return OpenDataExport::UploadResult::cancelled();
        if (pingWait == NetworkReplyWaitResult::Finished
            && !pingBody.exceeded()
            && replySucceeded(pingReply.get())) {
            server = candidate;
            break;
        }
    }
    if (server.isEmpty()) {
        return OpenDataExport::UploadResult::failed(
            sslError.isEmpty()
                ? QObject::tr(
                      "No servers available, please try later.")
                : sslError);
    }

    if (cancellationRequested(cancelled))
        return OpenDataExport::UploadResult::cancelled();
    reportProgress(
        progress, 3, LastStep, QObject::tr("Preparing data to send."));
    if (!archiveFile.seek(0)) {
        return OpenDataExport::UploadResult::failed(
            QObject::tr("Cannot read the sealed OpenData archive"));
    }

    const QUrl serverPost = policy.metricsUrl(server);
    if (!serverPost.isValid() || serverPost.isRelative()) {
        return OpenDataExport::UploadResult::failed(
            QObject::tr("Invalid OpenData server"));
    }
    const QNetworkRequest postRequest =
        policy.makeRequest(serverPost);

    QHttpMultiPart form(QHttpMultiPart::FormDataType);
    const QString boundary =
        QString::number(QRandomGenerator::global()->generate())
        + QString::number(QRandomGenerator::global()->generate())
        + QString::number(QRandomGenerator::global()->generate());
    form.setBoundary(boundary.toLatin1());

    QHttpPart secretPart;
    secretPart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"secret\""));
    secretPart.setBody(secret);
    form.append(secretPart);

    QHttpPart userIdPart;
    userIdPart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"id\""));
    userIdPart.setBody(request.athleteId.toLatin1());
    form.append(userIdPart);

    QHttpPart filePart;
    filePart.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/zip"));
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral(
            "form-data; name=\"data\"; filename=\"%1.zip\"; "
            "type=\"application/zip\"")
            .arg(request.athleteId));
    filePart.setBodyDevice(&archiveFile);
    form.append(filePart);

    reportProgress(
        progress, 4, LastStep, QObject::tr("Sending data to server."));
    std::unique_ptr<QNetworkReply> uploadReply(
        manager.post(postRequest, &form));
    BoundedReplyBody uploadBody(
        uploadReply.get(), MaxSmallResponseBytes);
    observeReply(policy, uploadReply.get());
    const NetworkReplyWaitResult uploadWait = waitForNetworkReply(
        uploadReply.get(), policy.uploadTimeoutMs, cancelled);
    uploadBody.drain();
    if (uploadWait == NetworkReplyWaitResult::Interrupted)
        return OpenDataExport::UploadResult::cancelled();
    if (uploadWait != NetworkReplyWaitResult::Finished
        || uploadReply->error() == QNetworkReply::TimeoutError) {
        return OpenDataExport::UploadResult::failed(
            QObject::tr("OpenData upload timed out"));
    }
    if (!replySucceeded(uploadReply.get())) {
        const QString response =
            QString::fromUtf8(uploadBody.body());
        return OpenDataExport::UploadResult::failed(
            response.isEmpty()
                ? QObject::tr("OpenData server rejected the upload")
                : QObject::tr("Server replied: %1").arg(response));
    }

    reportProgress(
        progress, 5, LastStep, QObject::tr("Finishing."));
    reportProgress(progress, 0, LastStep, QObject::tr("Done"));
    return OpenDataExport::UploadResult::succeeded();
}

} // namespace OpenDataTransport
