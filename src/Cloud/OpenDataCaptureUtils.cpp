/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataCaptureUtils.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstdint>
#include <string>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

struct ValidatedActivitySource
{
    QString root;
    QString source;
};

bool sameActivityMetadata(
    const OpenDataCaptureUtils::ActivityFileIdentity &left,
    const OpenDataCaptureUtils::ActivityFileIdentity &right)
{
    return left.valid
        && right.valid
        && left.volume == right.volume
        && left.file == right.file
        && left.size == right.size
        && left.modified == right.modified
        && left.modifiedFraction == right.modifiedFraction;
}

bool hashActivitySource(
    QFile &source,
    QByteArray &sha256,
    QString &error)
{
    if (!source.seek(0)) {
        error = QStringLiteral(
            "Cannot read the OpenData activity");
        return false;
    }
    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    constexpr qint64 ChunkSize = 1024 * 1024;
    QByteArray buffer(
        static_cast<qsizetype>(ChunkSize),
        Qt::Uninitialized);
    for (;;) {
        const qint64 count =
            source.read(buffer.data(), buffer.size());
        if (count < 0) {
            error = QStringLiteral(
                "Cannot read the OpenData activity");
            return false;
        }
        if (count == 0) {
            if (!source.atEnd()) {
                error = QStringLiteral(
                    "Cannot read the OpenData activity");
                return false;
            }
            break;
        }
        hash.addData(QByteArrayView(
            buffer.constData(),
            static_cast<qsizetype>(count)));
    }
    sha256 = hash.result();
    if (!source.seek(0)) {
        error = QStringLiteral(
            "Cannot reset the OpenData activity");
        return false;
    }
    return true;
}

bool pathIsInside(
    const QString &root,
    const QString &path,
    Qt::CaseSensitivity caseSensitivity)
{
    return path.startsWith(
        root + QLatin1Char('/'), caseSensitivity);
}

bool validateActivitySource(
    const QString &allowedRoot,
    const QString &directory,
    const QString &fileName,
    ValidatedActivitySource &validated)
{
    const QDir sourceDirectory(directory);
    const QFileInfo nameInfo(fileName);
    if (!QDir(allowedRoot).isAbsolute()
        || !sourceDirectory.isAbsolute()
        || fileName.isEmpty()
        || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\'))
        || nameInfo.isAbsolute()
        || nameInfo.fileName() != fileName) {
        return false;
    }

    const QString root = QDir::fromNativeSeparators(
        QDir(allowedRoot).canonicalPath());
    const QString sourceDirectoryPath =
        QDir::fromNativeSeparators(
            sourceDirectory.canonicalPath());
    const QFileInfo sourceInfo(
        sourceDirectory.filePath(fileName));
    const QString source = QDir::fromNativeSeparators(
        sourceInfo.canonicalFilePath());
    if (root.isEmpty()
        || sourceDirectoryPath.isEmpty()
        || source.isEmpty()
        || !sourceInfo.exists()
        || !sourceInfo.isFile()
        || sourceInfo.isSymLink()
        || !sourceInfo.isReadable()) {
        return false;
    }

    constexpr Qt::CaseSensitivity PathCase =
        Qt::CaseSensitive;
    const bool directoryAllowed =
        sourceDirectoryPath.compare(root, PathCase) == 0
        || pathIsInside(root, sourceDirectoryPath, PathCase);
    if (!directoryAllowed
        || !pathIsInside(root, source, PathCase)) {
        return false;
    }
    validated = {root, source};
    return true;
}

bool validateCapturedActivitySource(
    const QString &allowedRoot,
    const QString &capturedPath,
    ValidatedActivitySource &validated)
{
    const QString root = QDir::fromNativeSeparators(
        QDir(allowedRoot).canonicalPath());
    const QString source = QDir::fromNativeSeparators(
        QDir::cleanPath(capturedPath));
    constexpr Qt::CaseSensitivity PathCase =
        Qt::CaseSensitive;
    if (root.isEmpty()
        || !QDir(allowedRoot).isAbsolute()
        || !QFileInfo(source).isAbsolute()
        || !pathIsInside(root, source, PathCase)) {
        return false;
    }
    validated = {root, source};
    return true;
}

#ifdef Q_OS_UNIX
class FileDescriptor final
{
public:
    explicit FileDescriptor(int descriptor = -1)
        : descriptor_(descriptor)
    {
    }

    ~FileDescriptor()
    {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    int get() const
    {
        return descriptor_;
    }

    void reset(int descriptor)
    {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = descriptor;
    }

    int release()
    {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

private:
    int descriptor_ = -1;
};

int readOnlyFlags(bool directory)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if (directory) flags |= O_DIRECTORY;
#else
    Q_UNUSED(directory)
#endif
    return flags;
}

std::unique_ptr<QFile> openValidatedActivitySource(
    const ValidatedActivitySource &validated,
    const OpenDataCaptureUtils::ActivityFileIdentity *expectedIdentity,
    OpenDataCaptureUtils::ActivityFileIdentity *actualIdentity,
    QString &error)
{
    FileDescriptor directoryDescriptor(
        ::open(
            QFile::encodeName(validated.root).constData(),
            readOnlyFlags(true)));
    if (directoryDescriptor.get() < 0) {
        error = QStringLiteral(
            "Cannot open the OpenData activity directory");
        return {};
    }

    const QString relative =
        QDir(validated.root).relativeFilePath(validated.source);
    const QStringList components =
        QDir::fromNativeSeparators(relative).split(
            QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.isEmpty()) {
        error = QStringLiteral(
            "Invalid OpenData activity path");
        return {};
    }
    for (const QString &component : components) {
        if (component == QStringLiteral(".")
            || component == QStringLiteral("..")) {
            error = QStringLiteral(
                "Invalid OpenData activity path");
            return {};
        }
    }

    for (qsizetype index = 0;
         index + 1 < components.size();
         ++index) {
        const QByteArray component =
            QFile::encodeName(components.at(index));
        const int nextDescriptor =
            ::openat(
                directoryDescriptor.get(),
                component.constData(),
                readOnlyFlags(true));
        if (nextDescriptor < 0) {
            error = QStringLiteral(
                "Cannot open the OpenData activity directory");
            return {};
        }
        directoryDescriptor.reset(nextDescriptor);
    }

    const QByteArray fileName =
        QFile::encodeName(components.constLast());
    FileDescriptor fileDescriptor(
        ::openat(
            directoryDescriptor.get(),
            fileName.constData(),
            readOnlyFlags(false)));
    struct stat status {};
    if (fileDescriptor.get() < 0
        || ::fstat(fileDescriptor.get(), &status) != 0
        || !S_ISREG(status.st_mode)) {
        error = QStringLiteral(
            "Cannot securely open the OpenData activity");
        return {};
    }
    OpenDataCaptureUtils::ActivityFileIdentity identity;
    identity.volume = quint64(status.st_dev);
    identity.file = quint64(status.st_ino);
    identity.size = qint64(status.st_size);
#if defined(Q_OS_DARWIN)
    identity.modified = quint64(status.st_mtimespec.tv_sec);
    identity.modifiedFraction =
        quint64(status.st_mtimespec.tv_nsec);
#elif defined(st_mtime)
    identity.modified = quint64(status.st_mtim.tv_sec);
    identity.modifiedFraction = quint64(status.st_mtim.tv_nsec);
#else
    identity.modified = quint64(status.st_mtime);
#endif
    identity.valid = true;
    if (expectedIdentity
        && !sameActivityMetadata(
            identity, *expectedIdentity)) {
        error = QStringLiteral(
            "The OpenData activity changed during capture");
        return {};
    }
    if (actualIdentity) *actualIdentity = identity;

    auto source = std::make_unique<QFile>(validated.source);
    const int descriptor = fileDescriptor.release();
    if (!source->open(
            descriptor,
            QIODevice::ReadOnly,
            QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        error = QStringLiteral(
            "Cannot read the OpenData activity");
        return {};
    }
    return source;
}
#elif defined(Q_OS_WIN)
QString windowsHandlePath(HANDLE handle)
{
    const DWORD required =
        GetFinalPathNameByHandleW(
            handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (required == 0) return {};
    std::wstring buffer(required, L'\0');
    const DWORD length =
        GetFinalPathNameByHandleW(
            handle,
            buffer.data(),
            required,
            FILE_NAME_NORMALIZED);
    if (length == 0 || length >= required) return {};

    QString path =
        QString::fromWCharArray(buffer.data(), int(length));
    if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
        path = QStringLiteral("//") + path.mid(8);
    } else if (path.startsWith(QStringLiteral("\\\\?\\"))) {
        path = path.mid(4);
    }
    return QDir::fromNativeSeparators(QDir::cleanPath(path));
}

std::unique_ptr<QFile> openValidatedActivitySource(
    const ValidatedActivitySource &validated,
    const OpenDataCaptureUtils::ActivityFileIdentity *expectedIdentity,
    OpenDataCaptureUtils::ActivityFileIdentity *actualIdentity,
    QString &error)
{
    const HANDLE handle =
        CreateFileW(
            reinterpret_cast<LPCWSTR>(
                validated.source.utf16()),
            GENERIC_READ,
            FILE_SHARE_READ
                | FILE_SHARE_WRITE
                | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = QStringLiteral(
            "Cannot securely open the OpenData activity");
        return {};
    }

    BY_HANDLE_FILE_INFORMATION information {};
    const QString openedPath = windowsHandlePath(handle);
    if (!GetFileInformationByHandle(handle, &information)
        || information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY
               | FILE_ATTRIBUTE_REPARSE_POINT)
        || openedPath.isEmpty()
        || openedPath.compare(
               validated.source,
               Qt::CaseSensitive) != 0
        || !pathIsInside(
            validated.root,
            openedPath,
            Qt::CaseSensitive)) {
        CloseHandle(handle);
        error = QStringLiteral(
            "Cannot securely open the OpenData activity");
        return {};
    }
    OpenDataCaptureUtils::ActivityFileIdentity identity;
    identity.volume =
        quint64(information.dwVolumeSerialNumber);
    identity.file =
        (quint64(information.nFileIndexHigh) << 32)
        | quint64(information.nFileIndexLow);
    identity.size =
        qint64(
            (quint64(information.nFileSizeHigh) << 32)
            | quint64(information.nFileSizeLow));
    identity.modified =
        (quint64(information.ftLastWriteTime.dwHighDateTime) << 32)
        | quint64(information.ftLastWriteTime.dwLowDateTime);
    identity.valid = true;
    if (expectedIdentity
        && !sameActivityMetadata(
            identity, *expectedIdentity)) {
        CloseHandle(handle);
        error = QStringLiteral(
            "The OpenData activity changed during capture");
        return {};
    }
    if (actualIdentity) *actualIdentity = identity;

    const int descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle),
        _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        error = QStringLiteral(
            "Cannot read the OpenData activity");
        return {};
    }
    auto source = std::make_unique<QFile>(validated.source);
    if (!source->open(
            descriptor,
            QIODevice::ReadOnly,
            QFileDevice::AutoCloseHandle)) {
        _close(descriptor);
        error = QStringLiteral(
            "Cannot read the OpenData activity");
        return {};
    }
    return source;
}
#else
std::unique_ptr<QFile> openValidatedActivitySource(
    const ValidatedActivitySource &,
    const OpenDataCaptureUtils::ActivityFileIdentity *,
    OpenDataCaptureUtils::ActivityFileIdentity *,
    QString &error)
{
    error = QStringLiteral(
        "Secure OpenData activity access is unavailable");
    return {};
}
#endif

bool writeAll(
    QIODevice *destination,
    const QByteArray &data,
    QString &error)
{
    if (!destination || !destination->isWritable()) {
        error = QStringLiteral(
            "Cannot write OpenData activity samples");
        return false;
    }

    qint64 offset = 0;
    while (offset < data.size()) {
        const qint64 written = destination->write(
            data.constData() + offset, data.size() - offset);
        if (written <= 0) {
            error = QStringLiteral(
                "Cannot write OpenData activity samples");
            return false;
        }
        offset += written;
    }
    return true;
}

QByteArray number(double value)
{
    return QByteArray::number(value, 'g', 15);
}

} // namespace

namespace OpenDataCaptureUtils {

bool captureManifestThenSummary(
    const SnapshotCaptureOperations &operations,
    QString &error)
{
    error.clear();
    if (!operations.settleRefresh
        || !operations.captureManifest
        || !operations.writeSummary) {
        error = QStringLiteral(
            "Invalid OpenData snapshot capture operations");
        return false;
    }
    if (!operations.settleRefresh(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot settle OpenData activity refresh");
        }
        return false;
    }
    if (!operations.captureManifest(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot capture the OpenData activity manifest");
        }
        return false;
    }
    if (!operations.writeSummary(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot prepare the OpenData summary");
        }
        return false;
    }
    return true;
}

bool ActivityFileIdentity::operator==(
    const ActivityFileIdentity &other) const
{
    return sameActivityMetadata(*this, other)
        && sha256.size()
            == QCryptographicHash::hashLength(
                QCryptographicHash::Sha256)
        && sha256 == other.sha256;
}

QString jsonStringLiteral(const QString &value)
{
    const QByteArray array =
        QJsonDocument(QJsonArray{value}).toJson(
            QJsonDocument::Compact);
    if (array.size() < 2) return QStringLiteral("\"\"");
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

QString activitySourcePath(
    const QString &allowedRoot,
    const QString &directory,
    const QString &fileName)
{
    ValidatedActivitySource validated;
    return validateActivitySource(
               allowedRoot,
               directory,
               fileName,
               validated)
        ? validated.source
        : QString();
}

std::unique_ptr<QFile> openActivitySource(
    const QString &allowedRoot,
    const QString &validatedPath,
    QString &error)
{
    return openActivitySource(
        allowedRoot,
        validatedPath,
        nullptr,
        nullptr,
        error);
}

std::unique_ptr<QFile> openActivitySource(
    const QString &allowedRoot,
    const QString &validatedPath,
    const ActivityFileIdentity *expectedIdentity,
    ActivityFileIdentity *actualIdentity,
    QString &error)
{
    error.clear();
    ValidatedActivitySource validated;
    if (!validateCapturedActivitySource(
            allowedRoot,
            validatedPath,
            validated)) {
        error = QStringLiteral(
            "Invalid OpenData activity path");
        return {};
    }
    ActivityFileIdentity identity;
    std::unique_ptr<QFile> source =
        openValidatedActivitySource(
        validated,
        expectedIdentity,
        &identity,
        error);
    if (!source)
        return {};
    if (!hashActivitySource(
            *source, identity.sha256, error)) {
        return {};
    }
    if (expectedIdentity
        && !(identity == *expectedIdentity)) {
        error = QStringLiteral(
            "The OpenData activity changed during capture");
        return {};
    }
    if (actualIdentity)
        *actualIdentity = identity;
    return source;
}

QString activitySnapshotTemplate(const QString &fileName)
{
    if (fileName.isEmpty()
        || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\'))
        || QFileInfo(fileName).isAbsolute()
        || QFileInfo(fileName).fileName() != fileName) {
        return {};
    }

    QFileInfo info(fileName);
    QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("gz")
        || suffix == QStringLiteral("zip")) {
        info = QFileInfo(info.completeBaseName());
        suffix = info.suffix().toLower();
    }
    if (suffix.isEmpty()) return {};
    return QStringLiteral("source-XXXXXX.%1").arg(suffix);
}

bool includeActivityInSnapshot(
    bool hasMetrics,
    bool skipSave)
{
    return hasMetrics && !skipSave;
}

QString activityArchiveName(const QString &fileName)
{
    if (fileName.isEmpty()
        || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\'))
        || QFileInfo(fileName).isAbsolute()
        || QFileInfo(fileName).fileName() != fileName) {
        return {};
    }

    QFileInfo info(fileName);
    const QString suffix = info.suffix().toLower();
    QString base = info.completeBaseName();
    if (suffix == QStringLiteral("gz")
        || suffix == QStringLiteral("zip")) {
        info = QFileInfo(base);
        base = info.completeBaseName();
    }
    if (base.isEmpty()) return {};
    return base + QStringLiteral(".csv");
}

bool copyActivitySnapshot(
    QIODevice *source,
    QIODevice *destination,
    QString &error)
{
    return copyActivitySnapshot(
        source,
        destination,
        QByteArray(),
        error);
}

bool copyActivitySnapshot(
    QIODevice *source,
    QIODevice *destination,
    const QByteArray &expectedSha256,
    QString &error)
{
    error.clear();
    if (!source || !source->isReadable()
        || !destination || !destination->isWritable()) {
        error = QStringLiteral(
            "Cannot snapshot the OpenData activity");
        return false;
    }

    constexpr qint64 ChunkSize = 1024 * 1024;
    QByteArray buffer(static_cast<qsizetype>(ChunkSize),
                      Qt::Uninitialized);
    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    for (;;) {
        const qint64 count =
            source->read(buffer.data(), buffer.size());
        if (count < 0) {
            error = QStringLiteral(
                "Cannot read the OpenData activity");
            return false;
        }
        if (count == 0) {
            if (source->atEnd()) {
                if (!expectedSha256.isEmpty()
                    && hash.result() != expectedSha256) {
                    error = QStringLiteral(
                        "The OpenData activity changed during capture");
                    return false;
                }
                return true;
            }
            error = QStringLiteral(
                "Cannot read the OpenData activity");
            return false;
        }
        hash.addData(QByteArrayView(
            buffer.constData(),
            static_cast<qsizetype>(count)));

        qint64 offset = 0;
        while (offset < count) {
            const qint64 written = destination->write(
                buffer.constData() + offset,
                count - offset);
            if (written <= 0) {
                error = QStringLiteral(
                    "Cannot snapshot the OpenData activity");
                return false;
            }
            offset += written;
        }
    }
}

bool writeCsvHeader(QIODevice *destination, QString &error)
{
    error.clear();
    return writeAll(
        destination,
        QByteArrayLiteral("secs,km,power,hr,cad,alt\n"),
        error);
}

bool writeCsvSample(
    QIODevice *destination,
    const ActivitySeries &series,
    const ActivitySample &sample,
    QString &error)
{
    error.clear();
    QByteArray line;
    line.reserve(128);
    line += number(sample.secs);
    line += ',';
    line += number(sample.km);
    line += ',';
    if (series.power) line += number(sample.power);
    line += ',';
    if (series.heartRate) line += number(sample.heartRate);
    line += ',';
    if (series.cadence) line += number(sample.cadence);
    line += ',';
    if (series.altitude) line += number(sample.altitude);
    line += '\n';
    return writeAll(destination, line, error);
}

} // namespace OpenDataCaptureUtils
