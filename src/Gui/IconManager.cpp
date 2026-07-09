/*
 * Copyright (c) 2025 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "IconManager.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSet>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSvgRenderer>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#include "../qzip/zipwriter.h"
#include "../qzip/zipreader.h"

namespace {

const QSet<QString> &bundleMetadataFiles()
{
    static const QSet<QString> files {
        "LICENSE",
        "LICENSE.md",
        "LICENSE.txt",
        "README",
        "README.md",
        "README.txt",
        "COPYING",
        "mapping.json",
    };
    return files;
}


bool isWindowsDeviceName(const QString &name)
{
    static const QSet<QString> deviceNames {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };
    return deviceNames.contains(
        name.section(QLatin1Char('.'), 0, 0).toUpper());
}


bool isExpectedBundleFile(const ZipReader::FileInfo &info)
{
    if (!info.isFile || info.isDir || info.isSymLink)
        return false;

    const QString path = info.filePath;
    if (path.isEmpty()
        || path.contains(QChar::Null)
        || path != path.normalized(QString::NormalizationForm_C)
        || path.endsWith(QLatin1Char('.'))
        || path.endsWith(QLatin1Char(' '))
        || path.contains(QLatin1Char(':'))
        || path.contains('/')
        || path.contains('\\')
        || QDir::isAbsolutePath(path)
        || QFileInfo(path).fileName() != path
        || isWindowsDeviceName(path)) {
        return false;
    }

    return bundleMetadataFiles().contains(path)
        || (path.size() > 4 && path.endsWith(".svg", Qt::CaseSensitive));
}


bool parseMapping(
    const QByteArray &data,
    const QSet<QString> &iconFiles,
    QHash<QString, QHash<QString, QString>> *parsedIcons)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;

    parsedIcons->clear();
    parsedIcons->insert("Sport", {});
    parsedIcons->insert("SubSport", {});

    const QJsonObject root = document.object();
    for (auto group = root.constBegin(); group != root.constEnd(); ++group) {
        if (!parsedIcons->contains(group.key()) || !group.value().isObject())
            return false;

        const QJsonObject assignments = group.value().toObject();
        for (auto assignment = assignments.constBegin();
             assignment != assignments.constEnd();
             ++assignment) {
            if (!assignment.value().isString())
                return false;

            const QString filename = assignment.value().toString();
            if (!iconFiles.contains(filename))
                return false;
            (*parsedIcons)[group.key()].insert(assignment.key(), filename);
        }
    }
    return true;
}


struct BundleInstallFile {
    QString name;
    QByteArray contents;
    bool existed = false;
    QByteArray originalContents;
    QFile::Permissions originalPermissions;
};


#if defined(GC_ICON_BUNDLE_SECURITY_TEST)
IconManager::BundleCommitHook bundleCommitHook;
IconManager::BundleValidationHook bundleValidationHook;
#endif


bool readFile(const QString &path, QByteArray *contents)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    *contents = file.readAll();
    return file.error() == QFile::NoError;
}


bool fileSystemPathIsLink(const QFileInfo &fileInfo)
{
#if defined(Q_OS_WIN)
    const QString nativePath =
        QDir::toNativeSeparators(fileInfo.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return fileInfo.isSymLink();
}


bool pathContainsFileSystemLink(QString absolutePath)
{
    absolutePath = QDir::cleanPath(absolutePath);
    while (true) {
        const QFileInfo pathInfo(absolutePath);
        if (fileSystemPathIsLink(pathInfo))
            return true;

        const QString parentPath = pathInfo.dir().absolutePath();
        if (parentPath == absolutePath)
            return false;
        absolutePath = parentPath;
    }
}


bool destinationRootIsSafe(const QDir &destination)
{
    const QString rootPath = destination.absolutePath();
    const QFileInfo rootInfo(rootPath);
    return !pathContainsFileSystemLink(rootPath)
        && rootInfo.exists()
        && rootInfo.isDir();
}


bool destinationTargetIsSafe(const QDir &destination, const QString &name)
{
    if (!destinationRootIsSafe(destination))
        return false;

    const QString targetPath = destination.absoluteFilePath(name);
    if (QDir::cleanPath(targetPath)
        != QDir(destination.absolutePath()).filePath(name)) {
        return false;
    }

    const QFileInfoList entries = destination.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot);
    const QString alias =
        name.normalized(QString::NormalizationForm_C).toCaseFolded();
    for (const QFileInfo &entry : entries) {
        const QString existingName = entry.fileName();
        const QString existingAlias =
            existingName.normalized(QString::NormalizationForm_C)
                .toCaseFolded();
        if (existingName != name && existingAlias == alias)
            return false;
    }

    const QFileInfo targetInfo(targetPath);
    return !fileSystemPathIsLink(targetInfo)
        && (!targetInfo.exists() || targetInfo.isFile());
}


bool targetMatches(const QDir &destination,
                   const BundleInstallFile &file,
                   bool installed)
{
    if (!destinationTargetIsSafe(destination, file.name))
        return false;

    const QFileInfo targetInfo(destination.absoluteFilePath(file.name));
    const bool expectedToExist = installed || file.existed;
    if (targetInfo.exists() != expectedToExist)
        return false;
    if (!expectedToExist)
        return true;

    QByteArray contents;
    if (!readFile(targetInfo.absoluteFilePath(), &contents))
        return false;
    if (contents != (installed ? file.contents : file.originalContents))
        return false;

    return installed || targetInfo.permissions() == file.originalPermissions;
}


bool replaceFile(const QString &path,
                 const QByteArray &contents,
                 QFile::Permissions permissions,
                 const std::function<bool()> &validate,
                 const std::function<bool()> &beforeCommit = {})
{
    if (!validate())
        return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size()
        || !file.setPermissions(permissions)) {
        file.cancelWriting();
        return false;
    }

    if (beforeCommit && !beforeCommit()) {
        file.cancelWriting();
        return false;
    }

    const bool targetIsValid = validate();
#if defined(GC_ICON_BUNDLE_SECURITY_TEST)
    if (bundleValidationHook)
        bundleValidationHook(targetIsValid);
#endif
    if (!targetIsValid) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}


bool removeInstalledFile(const QDir &destination,
                         const BundleInstallFile &file)
{
    if (!targetMatches(destination, file, true))
        return false;

    QFile target(destination.absoluteFilePath(file.name));
    if (!targetMatches(destination, file, true) || !target.remove()) {
        return false;
    }
    return destinationTargetIsSafe(destination, file.name)
        && !QFileInfo::exists(destination.absoluteFilePath(file.name));
}


bool rollbackBundleFiles(const QDir &destination,
                         const QList<BundleInstallFile> &files,
                         int committed)
{
    bool rolledBack = true;
    for (int index = committed - 1; index >= 0; --index) {
        const BundleInstallFile &file = files.at(index);
        if (file.existed) {
            const bool restored = replaceFile(
                destination.absoluteFilePath(file.name),
                file.originalContents,
                file.originalPermissions,
                [&]() { return targetMatches(destination, file, true); })
                && targetMatches(destination, file, false);
            rolledBack = restored && rolledBack;
        } else {
            rolledBack = removeInstalledFile(destination, file) && rolledBack;
        }
    }
    return rolledBack;
}


bool installBundleFiles(const QDir &destination,
                        const QDir &staging,
                        QStringList names)
{
    const QString destinationPath = destination.absolutePath();
    const QFileInfo destinationInfo(destinationPath);
    if (pathContainsFileSystemLink(destinationPath)
        || (destinationInfo.exists() && !destinationInfo.isDir())) {
        return false;
    }
    const bool destinationExisted = destinationInfo.exists();
    if (!destinationExisted && !QDir().mkpath(destinationPath)) {
        return false;
    }
    if (!destinationRootIsSafe(destination)) {
        if (!destinationExisted)
            QDir().rmdir(destinationPath);
        return false;
    }

    // Qt has no portable atomic directory exchange. Use atomic file writes,
    // publish the mapping last, and roll back process-local failures.
    names.removeAll("mapping.json");
    std::sort(names.begin(), names.end());
    names.append("mapping.json");

    QList<BundleInstallFile> files;
    for (const QString &name : names) {
        BundleInstallFile file;
        file.name = name;
        const QFileInfo stagedInfo(staging.absoluteFilePath(name));
        if (fileSystemPathIsLink(stagedInfo)
            || !stagedInfo.isFile()
            || !readFile(stagedInfo.absoluteFilePath(), &file.contents)) {
            if (!destinationExisted)
                QDir().rmdir(destinationPath);
            return false;
        }

        const QFileInfo targetInfo(destination.absoluteFilePath(name));
        if (!destinationTargetIsSafe(destination, name)) {
            if (!destinationExisted)
                QDir().rmdir(destinationPath);
            return false;
        }
        file.existed = targetInfo.exists();
        if (file.existed) {
            file.originalPermissions = targetInfo.permissions();
            if (!readFile(targetInfo.absoluteFilePath(), &file.originalContents)) {
                if (!destinationExisted)
                    QDir().rmdir(destinationPath);
                return false;
            }
        }
        files.append(file);
    }

    const QFile::Permissions defaultPermissions =
        QFile::ReadOwner | QFile::WriteOwner
        | QFile::ReadGroup | QFile::ReadOther;
    int committed = 0;
    const auto failAndRollback = [&]() {
        const bool rolledBack = rollbackBundleFiles(
            destination, files, committed);
        if (!rolledBack)
            qWarning() << "Could not fully roll back icon bundle import";
        if (!destinationExisted
            && destinationRootIsSafe(destination)
            && destination.entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System
                    | QDir::NoDotAndDotDot).isEmpty()) {
            QDir().rmdir(destinationPath);
        }
        return false;
    };

    for (const BundleInstallFile &file : files) {
        const QFile::Permissions permissions =
            file.existed ? file.originalPermissions : defaultPermissions;
        const auto validateOriginal = [&]() {
            return targetMatches(destination, file, false);
        };
        const auto beforeCommit = [&]() {
#if defined(GC_ICON_BUNDLE_SECURITY_TEST)
            if (bundleCommitHook
                && !bundleCommitHook(file.name, committed)) {
                return false;
            }
#endif
            return true;
        };

        if (!replaceFile(destination.absoluteFilePath(file.name),
                         file.contents,
                         permissions,
                         validateOriginal,
                         beforeCommit)) {
            if (targetMatches(destination, file, true))
                ++committed;
            return failAndRollback();
        }
        ++committed;
        if (!targetMatches(destination, file, true))
            return failAndRollback();
    }
    return true;
}

} // namespace


IconManager&
IconManager::instance
()
{
    static IconManager instance;
    return instance;
}


#if defined(GC_ICON_BUNDLE_SECURITY_TEST)
void
IconManager::setBundleCommitHookForTest
(BundleCommitHook hook)
{
    bundleCommitHook = std::move(hook);
}


void
IconManager::setBundleValidationHookForTest
(BundleValidationHook hook)
{
    bundleValidationHook = std::move(hook);
}
#endif


IconManager::IconManager
()
{
    baseDir.mkpath(".");
    loadMapping();
}


QString
IconManager::getFilepath
(const QString &sport, const QString &subSport) const
{
    QString ret;
    if (   ! sport.isEmpty()
        && icons.contains("Sport")
        && ! icons["Sport"].value(sport, "").isEmpty()) {
        ret = baseDir.absoluteFilePath(icons["Sport"].value(sport, ""));
    }
    if (   ! subSport.isEmpty()
        && icons.contains("SubSport")
        && ! icons["SubSport"].value(subSport, "").isEmpty()) {
        ret = baseDir.absoluteFilePath(icons["SubSport"].value(subSport, ""));
    }
    QFileInfo fileInfo(ret);
    if (! fileInfo.isFile() || ! fileInfo.isReadable()) {
        ret = defaultIcon;
    }
    return ret;
}


QString
IconManager::getFilepath
(RideItem const * const rideItem) const
{
    if (rideItem != nullptr) {
        return getFilepath(rideItem->sport, rideItem->getText("SubSport", ""));
    }
    return defaultIcon;
}


QString
IconManager::getDefault
() const
{
    return defaultIcon;
}


QStringList
IconManager::listIconFiles
() const
{
    QStringList nameFilter { "*.svg" };
    return baseDir.entryList(nameFilter, QDir::Files | QDir::Readable);
}


QString
IconManager::toFilepath
(const QString &filename)
{
    return baseDir.absoluteFilePath(filename);
}


QString
IconManager::assignedIcon
(const QString &field, const QString &value) const
{
    if (icons.contains(field)) {
        return icons[field].value(value, "");
    }
    return "";
}


void
IconManager::assignIcon
(const QString &field, const QString &value, const QString &filename)
{
    // Normalize Sport values
    QString normValue = field == "Sport" ? RideFile::sportTag(value) : value;
    if (! filename.isEmpty()) {
        icons[field][normValue] = filename;
    } else if (icons.contains(field)) {
        icons[field].remove(normValue);
    }
    saveConfig();
}


bool
IconManager::addIconFile
(const QFile &sourceFile)
{
    QFileInfo fileInfo(sourceFile);
    if (fileInfo.suffix() != "svg") {
        return false;
    }
    QSvgRenderer renderer(sourceFile.fileName());
    if (! renderer.isValid()) {
        return false;
    }
    return QFile::copy(fileInfo.absoluteFilePath(), baseDir.absoluteFilePath(fileInfo.fileName()));
}


bool
IconManager::deleteIconFile
(const QString &filename)
{
    QString filepath = toFilepath(filename);
    QFileInfo fileInfo(filepath);
    if (fileInfo.suffix() != "svg") {
        return false;
    }
    if (QFile::remove(filepath)) {
        bool save = false;
        for (QString &field : icons.keys()) {
            for (QString &value : icons[field].keys()) {
                if (icons[field].value(value, "") == filename) {
                    save = true;
                    icons[field].remove(value);
                }
            }
        }
        if (save) {
            saveConfig();
        }
        return true;
    } else {
        return false;
    }
}


bool
IconManager::exportBundle
(const QString &filename)
{
    QFile zipFile(filename);
    if (! zipFile.open(QIODevice::WriteOnly)) {
        return false;
    }
    zipFile.close();
    ZipWriter writer(zipFile.fileName());
    QStringList files = listIconFiles();
    files << "LICENSE"
          << "LICENSE.md"
          << "LICENSE.txt"
          << "README"
          << "README.md"
          << "README.txt"
          << "COPYING"
          << "mapping.json";
    for (QString file : files) {
        QFile sourceFile(toFilepath(file));
        if (sourceFile.open(QIODevice::ReadOnly)) {
            writer.addFile(file, sourceFile.readAll());
            sourceFile.close();
        }
    }
    writer.close();
    return true;
}


bool
IconManager::importBundle
(const QString &filename)
{
    auto qfile = std::make_unique<QFile>(filename);
    return importBundle(std::move(qfile));
}


bool
IconManager::importBundle
(const QUrl &url)
{
    if (!url.isValid()
        || url.scheme().compare("https", Qt::CaseInsensitive) != 0
        || url.host().isEmpty()) {
        return false;
    }

    const QByteArray zipData = downloadUrl(url);
    if (zipData.isEmpty())
        return false;

    auto buffer = std::make_unique<QBuffer>();
    buffer->setData(zipData);
    buffer->open(QIODevice::ReadOnly);

    return importBundle(std::move(buffer));
}


bool
IconManager::importBundle
(std::unique_ptr<QIODevice> device)
{
    if (!device)
        return false;

    ZipReader reader(std::move(device));
    const QList<ZipReader::FileInfo> members = reader.fileInfoList();
    if (reader.status() != ZipReader::NoError || members.isEmpty())
        return false;

    QSet<QString> memberNames;
    QSet<QString> memberAliases;
    QSet<QString> iconFiles;
    bool hasMapping = false;
    for (const ZipReader::FileInfo &info : members) {
        if (!isExpectedBundleFile(info))
            return false;

        const QString alias = info.filePath.toCaseFolded();
        if (memberNames.contains(info.filePath)
            || memberAliases.contains(alias)) {
            return false;
        }
        memberNames.insert(info.filePath);
        memberAliases.insert(alias);
        if (info.filePath == configFile)
            hasMapping = true;
        else if (info.filePath.endsWith(".svg", Qt::CaseSensitive))
            iconFiles.insert(info.filePath);
    }

    if (!hasMapping)
        return false;

    QTemporaryDir staging;
    QStringList extracted;
    if (!staging.isValid()
        || !reader.extractAll(staging.path(), &extracted)) {
        return false;
    }
    reader.close();

    QSet<QString> extractedNames;
    for (const QString &name : extracted)
        extractedNames.insert(name);
    if (extracted.size() != memberNames.size()
        || extractedNames != memberNames) {
        return false;
    }

    QByteArray mapping;
    QHash<QString, QHash<QString, QString>> parsedIcons;
    const QDir stagingDir(staging.path());
    if (!readFile(stagingDir.absoluteFilePath(configFile), &mapping)
        || !parseMapping(mapping, iconFiles, &parsedIcons)) {
        return false;
    }
    for (const QString &iconFile : iconFiles) {
        QByteArray svg;
        if (!readFile(stagingDir.absoluteFilePath(iconFile), &svg))
            return false;
        const QSvgRenderer renderer(svg);
        if (!renderer.isValid())
            return false;
    }

    if (!installBundleFiles(baseDir, QDir(staging.path()), extracted))
        return false;

    icons["Sport"] = parsedIcons.value("Sport");
    icons["SubSport"] = parsedIcons.value("SubSport");
    return true;
}


bool
IconManager::saveConfig
() const
{
    QJsonObject rootObj;
    for (const QString &field : icons.keys()) {
        writeGroup(rootObj, field, icons[field]);
    }

    QJsonDocument jsonDoc(rootObj);

    QFile file(baseDir.absoluteFilePath(configFile));
    if (! file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Could not open file for writing:" << file.errorString();
        return false;
    }
    file.write(jsonDoc.toJson(QJsonDocument::Indented));
    file.close();

    return true;
}


bool
IconManager::loadMapping
()
{
    QFile file(baseDir.absoluteFilePath(configFile));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QByteArray jsonData = file.readAll();
        file.close();

        QJsonParseError parseError;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData, &parseError);

        if (   ! jsonDoc.isNull()
            && jsonDoc.isObject()) {
            QJsonObject root = jsonDoc.object();
            icons["Sport"] = readGroup(root, "Sport");
            icons["SubSport"] = readGroup(root, "SubSport");
        }
    } else {
        qWarning().noquote().nospace()
            << "Cannot read icon mappings ("
            << file.fileName()
            << "): "
            << file.errorString();
        return false;
    }
    return true;
}


void
IconManager::writeGroup
(QJsonObject &rootObj, const QString &group, const QHash<QString, QString> &data) const
{
    QJsonObject groupObj;
    if (data.size() > 0) {
        for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
            groupObj.insert(it.key(), it.value());
        }
        rootObj.insert(group, groupObj);
    }
}


QHash<QString, QString>
IconManager::readGroup
(const QJsonObject &root, const QString &group)
{
    QHash<QString, QString> result;
    if (root.contains(group)) {
        QJsonObject groupObj = root.value(group).toObject();
        for (auto it = groupObj.constBegin(); it != groupObj.constEnd(); ++it) {
            QString filename = it.value().toString();
            if (filename.endsWith(".svg") && QFile::exists(toFilepath(filename))) {
                result.insert(it.key(), filename);
            }
        }
    }
    return result;
}


QByteArray
IconManager::downloadUrl
(const QUrl &url, int timeoutMs)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QSslConfiguration sslConfiguration = request.sslConfiguration();
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(sslConfiguration);

    QNetworkReply *reply = manager.get(request);

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        reply->abort();
        loop.quit();
    });
    timeoutTimer.start(timeoutMs);
    loop.exec();
    timeoutTimer.stop();

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool authenticatedHttps =
        reply->url().scheme().compare("https", Qt::CaseInsensitive) == 0
        && statusCode >= 200
        && statusCode < 300;
    if (reply->error() != QNetworkReply::NoError || !authenticatedHttps) {
        delete reply;
        return {};
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}
