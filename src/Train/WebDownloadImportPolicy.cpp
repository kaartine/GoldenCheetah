/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WebDownloadImportPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace {

constexpr qint64 MaximumImportSize =
    Q_INT64_C(128) * 1024 * 1024;
constexpr qsizetype MaximumUrlLength = 8 * 1024;
constexpr qsizetype MaximumSuffixLength = 16;

bool hasUserInfo(const QUrl &url)
{
    return url.authority(QUrl::FullyEncoded)
        .contains(QLatin1Char('@'));
}

bool hasAllowedNetworkAuthority(const QUrl &url)
{
    return !url.host().isEmpty()
        && !hasUserInfo(url);
}

bool isLoopbackHost(const QString &host)
{
    return host.compare(
               QStringLiteral("localhost"),
               Qt::CaseInsensitive) == 0
        || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
}

bool hasAllowedPageUrl(const QUrl &url)
{
    if (!url.isValid()
        || url.isRelative()
        || url.toEncoded().size() > MaximumUrlLength) {
        return false;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https")) {
        return hasAllowedNetworkAuthority(url);
    }
    if (scheme == QStringLiteral("http")) {
        return hasAllowedNetworkAuthority(url)
            && isLoopbackHost(url.host());
    }
    return scheme == QStringLiteral("file")
        && url.isLocalFile();
}

bool hasAllowedDownloadUrl(const QUrl &url)
{
    if (!url.isValid()
        || url.isRelative()
        || url.toEncoded().size() > MaximumUrlLength) {
        return false;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https")) {
        return hasAllowedNetworkAuthority(url);
    }
    if (scheme == QStringLiteral("http")) {
        return hasAllowedNetworkAuthority(url)
            && isLoopbackHost(url.host());
    }
    if (scheme == QStringLiteral("file")) {
        return url.isLocalFile();
    }
    return scheme == QStringLiteral("blob")
        || scheme == QStringLiteral("data");
}

bool hasAllowedUrlRelationship(const QUrl &pageUrl,
                               const QUrl &downloadUrl)
{
    return downloadUrl.scheme().compare(
               QStringLiteral("file"),
               Qt::CaseInsensitive) != 0
        || pageUrl.scheme().compare(
               QStringLiteral("file"),
               Qt::CaseInsensitive) == 0;
}

bool isAsciiSuffix(const QString &suffix)
{
    if (suffix.isEmpty()
        || suffix.size() > MaximumSuffixLength) {
        return false;
    }

    for (const QChar character : suffix) {
        const ushort value = character.unicode();
        if (!((value >= 'a' && value <= 'z')
              || (value >= '0' && value <= '9'))) {
            return false;
        }
    }
    return true;
}

QString absoluteCleanPath(const QString &path)
{
    return QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
}

} // namespace

namespace WebDownloadImportPolicy {

Decision Gate::handleRequest(
    const Request &request,
    const QString &stagingDirectory,
    UserDecision userDecision)
{
    Decision decision;

    if (request.sourcePage == 0
        || request.expectedPage == 0) {
        cancel(request.id);
        return decision;
    }
    if (request.sourcePage != request.expectedPage) {
        decision.action = RequestAction::Ignore;
        return decision;
    }

    if (!isValidRequest(request)) {
        cancel(request.id);
        return decision;
    }

    if (userDecision == UserDecision::NotAsked) {
        if (!awaiting_.isEmpty()
            || !pending_.isEmpty()) {
            return decision;
        }

        awaiting_.insert(request.id, request);
        decision.action = RequestAction::Confirm;
        return decision;
    }

    const auto awaiting = awaiting_.constFind(request.id);
    if (awaiting == awaiting_.constEnd()
        || !isSameRequest(awaiting.value(), request)) {
        awaiting_.remove(request.id);
        return decision;
    }
    awaiting_.remove(request.id);

    if (userDecision != UserDecision::Approved
        || pending_.contains(request.id)) {
        return decision;
    }

    const QString fileName =
        safeFileName(request.suggestedFileName);
    Pending pending;
    pending.sourcePage = request.sourcePage;
    if (!prepareDestination(
            stagingDirectory, fileName, &pending)) {
        return decision;
    }

    pending_.insert(request.id, pending);
    decision.action = RequestAction::Accept;
    decision.fileName = fileName;
    decision.filePath = pending.filePath;
    return decision;
}

QString Gate::takeCompletedImport(
    const Completion &completion)
{
    if (!completion.finished) {
        return QString();
    }

    const auto pendingIterator =
        pending_.constFind(completion.id);
    if (pendingIterator == pending_.constEnd()) {
        return QString();
    }
    const Pending pending = pendingIterator.value();
    pending_.remove(completion.id);

    if (!completion.succeeded
        || completion.sourcePage != pending.sourcePage
        || absoluteCleanPath(completion.filePath)
            != pending.filePath) {
        return QString();
    }

    const QFileInfo file(pending.filePath);
    if (!file.exists()
        || file.isSymLink()
        || !file.isFile()
        || file.size() <= 0
        || file.size() > MaximumImportSize) {
        return QString();
    }

    const QString canonicalFile =
        file.canonicalFilePath();
    if (canonicalFile.isEmpty()
        || QFileInfo(canonicalFile).absolutePath()
            != pending.stagingDirectory) {
        return QString();
    }

    return pending.filePath;
}

void Gate::cancel(quint32 id)
{
    awaiting_.remove(id);
    pending_.remove(id);
}

bool Gate::hasPending(quint32 id) const
{
    return pending_.contains(id);
}

int Gate::awaitingCount() const
{
    return awaiting_.size();
}

int Gate::pendingCount() const
{
    return pending_.size();
}

QString Gate::safeFileName(
    const QString &suggestedFileName)
{
    QString normalized =
        suggestedFileName.left(4 * 1024);
    normalized.replace(
        QLatin1Char('\\'), QLatin1Char('/'));
    const QString leaf =
        normalized.section(QLatin1Char('/'), -1);
    if (leaf.isEmpty()
        || leaf.startsWith(QLatin1Char('.'))) {
        return QStringLiteral("download");
    }

    const QStringList parts =
        leaf.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (parts.size() < 2) {
        return QStringLiteral("download");
    }

    const QString last = parts.constLast().toLower();
    QStringList suffixParts;
    if ((last == QStringLiteral("gz")
         || last == QStringLiteral("zip"))
        && parts.size() >= 3) {
        suffixParts
            << parts.at(parts.size() - 2).toLower()
            << last;
    } else {
        suffixParts << last;
    }

    for (const QString &part : suffixParts) {
        if (!isAsciiSuffix(part)) {
            return QStringLiteral("download");
        }
    }

    return QStringLiteral("download.")
        + suffixParts.join(QLatin1Char('.'));
}

qint64 Gate::maximumImportBytes()
{
    return MaximumImportSize;
}

bool Gate::isValidRequest(const Request &request)
{
    return request.id != 0
        && request.pageVisible
        && !request.savePageDownload
        && request.totalBytes >= -1
        && request.totalBytes <= MaximumImportSize
        && hasAllowedPageUrl(request.pageUrl)
        && hasAllowedDownloadUrl(request.downloadUrl)
        && hasAllowedUrlRelationship(
            request.pageUrl, request.downloadUrl);
}

bool Gate::isSameRequest(
    const Request &left,
    const Request &right)
{
    return left.id == right.id
        && left.sourcePage == right.sourcePage
        && left.expectedPage == right.expectedPage
        && left.pageVisible == right.pageVisible
        && left.savePageDownload == right.savePageDownload
        && left.pageUrl == right.pageUrl
        && left.downloadUrl == right.downloadUrl
        && left.suggestedFileName
            == right.suggestedFileName;
}

bool Gate::prepareDestination(
    const QString &stagingDirectory,
    const QString &fileName,
    Pending *pending)
{
    if (!pending
        || stagingDirectory.isEmpty()
        || fileName.isEmpty()
        || QFileInfo(fileName).fileName() != fileName) {
        return false;
    }

    const QFileInfo directory(stagingDirectory);
    if (!directory.exists()
        || directory.isSymLink()
        || !directory.isDir()
        || !directory.isWritable()) {
        return false;
    }

    const QString canonicalDirectory =
        directory.canonicalFilePath();
    if (canonicalDirectory.isEmpty()) {
        return false;
    }

    const QString filePath = absoluteCleanPath(
        QDir(stagingDirectory).absoluteFilePath(fileName));
    if (QFileInfo(filePath).absolutePath()
            != absoluteCleanPath(stagingDirectory)
        || QFileInfo::exists(filePath)) {
        return false;
    }

    pending->stagingDirectory = canonicalDirectory;
    pending->filePath = filePath;
    return true;
}

} // namespace WebDownloadImportPolicy
