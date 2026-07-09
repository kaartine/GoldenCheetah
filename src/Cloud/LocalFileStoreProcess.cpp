/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "LocalFileStoreProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

constexpr char HelperSwitch[] = "--gc-local-file-store-helper-v1";
constexpr quint32 ProtocolMagic = 0x47434c46;
constexpr quint16 ProtocolVersion = 1;
constexpr qint64 MaxTransferBytes = 128LL * 1024 * 1024;
constexpr qint64 MaxMetadataBytes = 16LL * 1024 * 1024;
constexpr qint64 MaxRequestBytes = 128 * 1024;
constexpr qint64 MaxResponseBytes =
        MaxTransferBytes + MaxMetadataBytes + 128 * 1024;
constexpr int MaxEntries = 50000;
constexpr int PollIntervalMs = 10;
constexpr int MaximumTimeoutMs = 120000;
constexpr int TerminationTimeoutMs = 1000;
constexpr int ReaperRetryIntervalMs = 100;
constexpr int MaxPathBytes = 32768;

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
std::atomic<qint64> lastHelperProcessId{0};
std::atomic<int> helperProcessStartCount{0};
std::atomic<int> reaperProcessCount{0};
std::atomic<int> reaperRegistrationAttempts{0};
std::atomic_bool reaperRegistrationRetryOnceConsumed{false};
std::mutex reaperAdoptTestMutex;
std::condition_variable reaperAdoptTestCondition;
bool reaperAdoptTestPaused = false;
bool reaperAdoptTestEntered = false;
bool reaperAdoptTestReleased = false;
std::mutex reaperDispatchTestMutex;
std::condition_variable reaperDispatchTestCondition;
bool reaperDispatchTestPaused = false;
bool reaperDispatchTestEntered = false;
bool reaperDispatchTestReleased = false;
bool reaperDispatchTeardownProbed = false;
bool reaperDispatchHoldsStartupMutex = false;
std::atomic_bool reaperDispatchLostTarget{false};

class ReaperDispatchStartupLockProbe final
{
public:
    ReaperDispatchStartupLockProbe()
    {
        const std::lock_guard<std::mutex> lock(
            reaperDispatchTestMutex);
        reaperDispatchHoldsStartupMutex = true;
    }

    ~ReaperDispatchStartupLockProbe()
    {
        const std::lock_guard<std::mutex> lock(
            reaperDispatchTestMutex);
        reaperDispatchHoldsStartupMutex = false;
    }
};

void pauseReaperDispatchForTest()
{
    std::unique_lock<std::mutex> lock(reaperDispatchTestMutex);
    if (!reaperDispatchTestPaused) return;
    reaperDispatchTestEntered = true;
    reaperDispatchTestCondition.notify_all();
    reaperDispatchTestCondition.wait(lock, []() {
        return reaperDispatchTestReleased;
    });
}
#endif

struct Request
{
    LocalFileStoreProcess::Operation operation =
            LocalFileStoreProcess::Operation::Open;
    QString root;
    QString path;
};

LocalFileStoreProcessResult failedResult(const QString &error)
{
    LocalFileStoreProcessResult result;
    result.error = error;
    return result;
}

class FrameWriter
{
public:
    void appendU8(quint8 value)
    {
        frame.append(char(value));
    }

    void appendU16(quint16 value)
    {
        appendU8(quint8(value >> 8));
        appendU8(quint8(value));
    }

    void appendU32(quint32 value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            appendU8(quint8(value >> shift));
    }

    void appendU64(quint64 value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
            appendU8(quint8(value >> shift));
    }

    bool appendBytes(const QByteArray &value, qint64 maximum)
    {
        if (value.size() < 0
            || value.size() > maximum
            || quint64(value.size())
                > std::numeric_limits<quint32>::max()) {
            return false;
        }
        appendU32(quint32(value.size()));
        frame.append(value);
        return true;
    }

    bool appendString(const QString &value, qint64 maximum)
    {
        return appendBytes(value.toUtf8(), maximum);
    }

    QByteArray take()
    {
        return std::move(frame);
    }

private:
    QByteArray frame;
};

class FrameReader
{
public:
    explicit FrameReader(const QByteArray &frame)
        : frame(frame)
    {
    }

    bool takeU8(quint8 &value)
    {
        if (remaining() < 1) return false;
        value = quint8(uchar(frame.at(offset++)));
        return true;
    }

    bool takeU16(quint16 &value)
    {
        quint8 first = 0;
        quint8 second = 0;
        if (!takeU8(first) || !takeU8(second)) return false;
        value = (quint16(first) << 8) | quint16(second);
        return true;
    }

    bool takeU32(quint32 &value)
    {
        value = 0;
        for (int index = 0; index < 4; ++index) {
            quint8 part = 0;
            if (!takeU8(part)) return false;
            value = (value << 8) | quint32(part);
        }
        return true;
    }

    bool takeU64(quint64 &value)
    {
        value = 0;
        for (int index = 0; index < 8; ++index) {
            quint8 part = 0;
            if (!takeU8(part)) return false;
            value = (value << 8) | quint64(part);
        }
        return true;
    }

    bool takeBytes(QByteArray &value, qint64 maximum)
    {
        quint32 length = 0;
        if (!takeU32(length)
            || qint64(length) > maximum
            || qint64(length) > remaining()) {
            return false;
        }
        value = frame.mid(offset, qsizetype(length));
        offset += qsizetype(length);
        return true;
    }

    bool takeString(QString &value, qint64 maximum)
    {
        QByteArray encoded;
        if (!takeBytes(encoded, maximum)) return false;
        value = QString::fromUtf8(encoded);
        return value.toUtf8() == encoded;
    }

    bool atEnd() const
    {
        return offset == frame.size();
    }

private:
    qint64 remaining() const
    {
        return qint64(frame.size() - offset);
    }

    const QByteArray &frame;
    qsizetype offset = 0;
};

bool validOperation(quint8 value)
{
    return value <= quint8(LocalFileStoreProcess::Operation::Read);
}

bool serializeRequest(const Request &request, QByteArray &frame)
{
    if (!QDir::isAbsolutePath(request.root)
        || request.root.contains(QChar::Null)) {
        return false;
    }

    FrameWriter writer;
    writer.appendU32(ProtocolMagic);
    writer.appendU16(ProtocolVersion);
    writer.appendU8(quint8(request.operation));
    if (!writer.appendString(request.root, MaxPathBytes)
        || !writer.appendString(request.path, MaxPathBytes)) {
        return false;
    }

    frame = writer.take();
    return frame.size() <= MaxRequestBytes;
}

bool parseRequest(const QByteArray &frame, Request &request)
{
    if (frame.size() > MaxRequestBytes) return false;

    FrameReader reader(frame);
    quint32 magic = 0;
    quint16 version = 0;
    quint8 operation = 0;
    if (!reader.takeU32(magic)
        || !reader.takeU16(version)
        || !reader.takeU8(operation)
        || magic != ProtocolMagic
        || version != ProtocolVersion
        || !validOperation(operation)
        || !reader.takeString(request.root, MaxPathBytes)
        || !reader.takeString(request.path, MaxPathBytes)
        || !reader.atEnd()
        || !QDir::isAbsolutePath(request.root)
        || request.root.contains(QChar::Null)) {
        return false;
    }

    request.operation =
            static_cast<LocalFileStoreProcess::Operation>(operation);
    return true;
}

bool validComponent(const QString &component);

bool serializeResponse(
        LocalFileStoreProcess::Operation operation,
        const LocalFileStoreProcessResult &result,
        QByteArray &frame)
{
    if (result.entries.size() > MaxEntries
        || result.data.size() > MaxTransferBytes) {
        return false;
    }

    FrameWriter writer;
    writer.appendU32(ProtocolMagic);
    writer.appendU16(ProtocolVersion);
    writer.appendU8(quint8(operation));
    writer.appendU8(quint8(result.status));
    if (!writer.appendString(result.error, 1024)) return false;
    writer.appendU32(quint32(result.entries.size()));

    qint64 metadataBytes = 0;
    for (const LocalFileStoreEntryValue &entry : result.entries) {
        const QByteArray encodedName = entry.name.toUtf8();
        metadataBytes += encodedName.size() + 32;
        if (!validComponent(entry.name)
            || metadataBytes > MaxMetadataBytes
            || !writer.appendBytes(encodedName, 4096)) {
            return false;
        }
        writer.appendU8(entry.isDir ? 1 : 0);
        writer.appendU64(quint64(entry.size));
        writer.appendU64(
            quint64(entry.modified.isValid()
                ? entry.modified.toMSecsSinceEpoch()
                : -1));
    }

    if (!writer.appendBytes(result.data, MaxTransferBytes)) return false;
    frame = writer.take();
    return frame.size() <= MaxResponseBytes;
}

bool parseResponse(
        const QByteArray &frame,
        LocalFileStoreProcess::Operation expectedOperation,
        LocalFileStoreProcessResult &result)
{
    if (frame.size() > MaxResponseBytes) return false;

    FrameReader reader(frame);
    quint32 magic = 0;
    quint16 version = 0;
    quint8 operation = 0;
    quint8 status = 0;
    quint32 entryCount = 0;
    if (!reader.takeU32(magic)
        || !reader.takeU16(version)
        || !reader.takeU8(operation)
        || !reader.takeU8(status)
        || magic != ProtocolMagic
        || version != ProtocolVersion
        || operation != quint8(expectedOperation)
        || status > quint8(LocalFileStoreProcessResult::Status::Failed)
        || !reader.takeString(result.error, 1024)
        || !reader.takeU32(entryCount)
        || entryCount > MaxEntries) {
        return false;
    }

    qint64 metadataBytes = 0;
    for (quint32 index = 0; index < entryCount; ++index) {
        QByteArray encodedName;
        quint8 isDir = 0;
        quint64 size = 0;
        quint64 modified = 0;
        if (!reader.takeBytes(encodedName, 4096)
            || !reader.takeU8(isDir)
            || !reader.takeU64(size)
            || !reader.takeU64(modified)
            || isDir > 1
            || size > quint64(std::numeric_limits<qint64>::max())) {
            return false;
        }

        metadataBytes += encodedName.size() + 32;
        const QString name = QString::fromUtf8(encodedName);
        if (metadataBytes > MaxMetadataBytes
            || name.toUtf8() != encodedName
            || !validComponent(name)) {
            return false;
        }

        LocalFileStoreEntryValue entry;
        entry.name = name;
        entry.isDir = isDir != 0;
        entry.size = qint64(size);
        const qint64 modifiedMs = qint64(modified);
        if (modifiedMs >= 0) {
            entry.modified =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    QDateTime::fromMSecsSinceEpoch(
                        modifiedMs, QTimeZone::UTC);
#else
                    QDateTime::fromMSecsSinceEpoch(modifiedMs, Qt::UTC);
#endif
        }
        result.entries.append(std::move(entry));
    }

    if (!reader.takeBytes(result.data, MaxTransferBytes)
        || !reader.atEnd()) {
        return false;
    }

    result.status =
            static_cast<LocalFileStoreProcessResult::Status>(status);
    return true;
}

bool validComponent(const QString &component)
{
    const QByteArray encoded = component.toUtf8();
    return !component.isEmpty()
        && component != QStringLiteral(".")
        && component != QStringLiteral("..")
        && !component.contains(QLatin1Char('/'))
        && !component.contains(QLatin1Char('\\'))
        && !component.contains(QLatin1Char(':'))
        && !component.contains(QChar::Null)
        && !encoded.isEmpty()
        && encoded.size() <= 255;
}

#ifdef Q_OS_UNIX

bool parseLogicalPath(const QString &path, QStringList &components)
{
    components.clear();
    if (path == QStringLiteral("/")) return true;
    if (!path.startsWith(QLatin1Char('/'))
        || path.endsWith(QLatin1Char('/'))
        || path.contains(QStringLiteral("//"))
        || path.toUtf8().size() > MaxPathBytes) {
        return false;
    }

    components = path.mid(1).split(
        QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &component : std::as_const(components)) {
        if (!validComponent(component)) return false;
    }
    return true;
}

bool canonicalRoot(const QString &configuredRoot, QString &root)
{
    if (!QDir::isAbsolutePath(configuredRoot)
        || configuredRoot.contains(QChar::Null)) {
        return false;
    }
    const QFileInfo info(configuredRoot);
    root = info.canonicalFilePath();
    return info.isDir() && !root.isEmpty()
        && QDir::isAbsolutePath(root)
        && !root.contains(QChar::Null);
}

bool allowSpecialFileForTest()
{
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    return qEnvironmentVariableIsSet(
        "GC_TEST_LOCAL_FILE_STORE_ALLOW_SPECIAL_FILE");
#else
    return false;
#endif
}

class UniqueFd
{
public:
    explicit UniqueFd(int descriptor = -1)
        : descriptor(descriptor)
    {
    }

    ~UniqueFd()
    {
        if (descriptor >= 0) ::close(descriptor);
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept
        : descriptor(std::exchange(other.descriptor, -1))
    {
    }

    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            if (descriptor >= 0) ::close(descriptor);
            descriptor = std::exchange(other.descriptor, -1);
        }
        return *this;
    }

    int get() const { return descriptor; }

    int release()
    {
        return std::exchange(descriptor, -1);
    }

private:
    int descriptor = -1;
};

UniqueFd openRoot(const QString &configuredRoot)
{
    QString canonical;
    if (!canonicalRoot(configuredRoot, canonical))
        return UniqueFd();

    const QString cleaned = QDir::cleanPath(canonical);
    const QStringList components = cleaned.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    UniqueFd current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current.get() < 0) return UniqueFd();

    for (const QString &component : components) {
        const QByteArray encoded = QFile::encodeName(component);
        if (component == QStringLiteral(".")
            || component == QStringLiteral("..")
            || encoded.isEmpty()
            || encoded.size() > 255) {
            return UniqueFd();
        }
        UniqueFd next(::openat(
            current.get(), encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) return UniqueFd();
        current = std::move(next);
    }
    return current;
}

UniqueFd openDirectory(
        const QString &configuredRoot,
        const QString &logicalPath)
{
    QStringList components;
    if (!parseLogicalPath(logicalPath, components)) return UniqueFd();

    UniqueFd current = openRoot(configuredRoot);
    if (current.get() < 0) return current;

    for (const QString &component : std::as_const(components)) {
        const QByteArray encoded = QFile::encodeName(component);
        UniqueFd next(::openat(
            current.get(), encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) return UniqueFd();
        current = std::move(next);
    }
    return current;
}

QDateTime modifiedTime(const struct stat &status)
{
#if defined(Q_OS_DARWIN)
    const qint64 milliseconds =
            qint64(status.st_mtimespec.tv_sec) * 1000
            + status.st_mtimespec.tv_nsec / 1000000;
#else
    const qint64 milliseconds =
            qint64(status.st_mtim.tv_sec) * 1000
            + status.st_mtim.tv_nsec / 1000000;
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::UTC);
#else
    return QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC);
#endif
}

LocalFileStoreProcessResult executeListUnix(const Request &request)
{
    UniqueFd directory = openDirectory(request.root, request.path);
    if (directory.get() < 0) {
        return failedResult(QStringLiteral("invalid-directory"));
    }

    DIR *stream = ::fdopendir(directory.release());
    if (!stream) {
        return failedResult(QStringLiteral("list-open-failed"));
    }

    LocalFileStoreProcessResult result;
    qint64 metadataBytes = 0;
    while (true) {
        errno = 0;
        dirent *item = ::readdir(stream);
        if (!item) {
            const int readError = errno;
            ::closedir(stream);
            if (readError != 0) {
                return failedResult(QStringLiteral("list-failed"));
            }
            break;
        }

        const QByteArray encodedName(item->d_name);
        if (encodedName == "." || encodedName == "..") continue;

        struct stat status {};
        if (::fstatat(
                ::dirfd(stream), item->d_name, &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }

        const bool isDirectory = S_ISDIR(status.st_mode);
        const bool isRegular = S_ISREG(status.st_mode);
        if (!isDirectory && !isRegular
            && !allowSpecialFileForTest()) {
            continue;
        }

        const QString name = QFile::decodeName(encodedName);
        metadataBytes += encodedName.size() + 32;
        if (!validComponent(name)
            || metadataBytes > MaxMetadataBytes
            || result.entries.size() >= MaxEntries) {
            ::closedir(stream);
            return failedResult(QStringLiteral("invalid-entry-list"));
        }

        LocalFileStoreEntryValue entry;
        entry.name = name;
        entry.isDir = isDirectory;
        entry.size = std::max<qint64>(0, qint64(status.st_size));
        entry.modified = modifiedTime(status);
        result.entries.append(std::move(entry));
    }

    result.status = LocalFileStoreProcessResult::Status::Succeeded;
    return result;
}

LocalFileStoreProcessResult executeReadUnix(
        const QString &configuredRoot,
        const QString &name)
{
    if (!validComponent(name)) {
        return failedResult(QStringLiteral("invalid-file"));
    }

    UniqueFd root = openRoot(configuredRoot);
    if (root.get() < 0) {
        return failedResult(QStringLiteral("invalid-root"));
    }

    const QByteArray encodedName = QFile::encodeName(name);
    UniqueFd file(::openat(
        root.get(), encodedName.constData(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
        return failedResult(QStringLiteral("read-open-failed"));
    }

    struct stat status {};
    if (::fstat(file.get(), &status) != 0
        || status.st_size > MaxTransferBytes) {
        return failedResult(QStringLiteral("invalid-file"));
    }
    if (!S_ISREG(status.st_mode)) {
        if (!allowSpecialFileForTest()) {
            return failedResult(QStringLiteral("invalid-file"));
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        UniqueFd blockingFile(::openat(
            root.get(), encodedName.constData(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat blockingStatus {};
        if (blockingFile.get() < 0
            || ::fstat(blockingFile.get(), &blockingStatus) != 0
            || S_ISREG(blockingStatus.st_mode)) {
            return failedResult(QStringLiteral("invalid-file"));
        }
        file = std::move(blockingFile);
#endif
    }

    LocalFileStoreProcessResult result;
    QByteArray chunk(64 * 1024, '\0');
    while (true) {
        const ssize_t bytesRead =
                ::read(file.get(), chunk.data(), size_t(chunk.size()));
        if (bytesRead < 0) {
            if (errno == EINTR) continue;
            return failedResult(QStringLiteral("read-failed"));
        }
        if (bytesRead == 0) break;
        if (result.data.size() + bytesRead > MaxTransferBytes) {
            return failedResult(QStringLiteral("file-too-large"));
        }
        result.data.append(chunk.constData(), qsizetype(bytesRead));
    }

    result.status = LocalFileStoreProcessResult::Status::Succeeded;
    return result;
}

LocalFileStoreProcessResult executeWriteUnix(
        const QString &configuredRoot,
        const QString &name,
        const QByteArray &data)
{
    if (!validComponent(name)
        || data.size() > MaxTransferBytes) {
        return failedResult(QStringLiteral("invalid-file"));
    }

    UniqueFd root = openRoot(configuredRoot);
    if (root.get() < 0) {
        return failedResult(QStringLiteral("invalid-root"));
    }

    const QByteArray encodedName = QFile::encodeName(name);
    struct stat existing {};
    if (::fstatat(
            root.get(), encodedName.constData(), &existing,
            AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode)) {
            return failedResult(QStringLiteral("invalid-file"));
        }
    } else if (errno != ENOENT) {
        return failedResult(QStringLiteral("invalid-file"));
    }

    QByteArray temporaryName;
    UniqueFd temporary;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporaryName = QByteArray(".gc-write-")
                + QByteArray::number(QCoreApplication::applicationPid())
                + '-'
                + QByteArray::number(
                    QRandomGenerator::global()->generate64(), 16);
        temporary = UniqueFd(::openat(
            root.get(), temporaryName.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600));
        if (temporary.get() >= 0) break;
        if (errno != EEXIST) {
            return failedResult(QStringLiteral("write-open-failed"));
        }
    }
    if (temporary.get() < 0) {
        return failedResult(QStringLiteral("write-open-failed"));
    }

    bool writeSucceeded = true;
    qsizetype offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(
            temporary.get(), data.constData() + offset,
            size_t(data.size() - offset));
        if (written < 0) {
            if (errno == EINTR) continue;
            writeSucceeded = false;
            break;
        }
        if (written == 0) {
            writeSucceeded = false;
            break;
        }
        offset += qsizetype(written);
    }
    if (::fchmod(temporary.get(), 0600) != 0
        || ::fsync(temporary.get()) != 0) {
        writeSucceeded = false;
    }

    const int temporaryDescriptor = temporary.release();
    if (::close(temporaryDescriptor) != 0)
        writeSucceeded = false;
    if (!writeSucceeded
        || ::renameat(
            root.get(), temporaryName.constData(),
            root.get(), encodedName.constData()) != 0) {
        ::unlinkat(root.get(), temporaryName.constData(), 0);
        return failedResult(QStringLiteral("write-failed"));
    }

    if (::fsync(root.get()) != 0
        && errno != EINVAL
#ifdef ENOTSUP
        && errno != ENOTSUP
#endif
        && errno != EROFS) {
        return failedResult(QStringLiteral("directory-sync-failed"));
    }

    LocalFileStoreProcessResult result;
    result.status = LocalFileStoreProcessResult::Status::Succeeded;
    return result;
}

#endif

LocalFileStoreProcessResult executeOpen(const Request &request)
{
#ifdef Q_OS_UNIX
    UniqueFd root = openRoot(request.root);
    if (root.get() < 0) {
        return failedResult(QStringLiteral("invalid-root"));
    }
#else
    Q_UNUSED(request)
    return failedResult(QStringLiteral("unsupported-platform"));
#endif

    LocalFileStoreProcessResult result;
    result.status = LocalFileStoreProcessResult::Status::Succeeded;
    return result;
}

LocalFileStoreProcessResult executeList(const Request &request)
{
#ifdef Q_OS_UNIX
    return executeListUnix(request);
#else
    Q_UNUSED(request)
    return failedResult(QStringLiteral("unsupported-platform"));
#endif
}

LocalFileStoreProcessResult executeRead(const Request &request)
{
#ifdef Q_OS_UNIX
    return executeReadUnix(request.root, request.path);
#else
    Q_UNUSED(request)
    return failedResult(QStringLiteral("unsupported-platform"));
#endif
}

LocalFileStoreProcessResult executeRequest(const Request &request)
{
    switch (request.operation) {
    case LocalFileStoreProcess::Operation::Open:
        return executeOpen(request);
    case LocalFileStoreProcess::Operation::List:
        return executeList(request);
    case LocalFileStoreProcess::Operation::Read:
        return executeRead(request);
    }
    return failedResult(QStringLiteral("invalid-operation"));
}

bool readBoundedFrame(
        QFile &file, QByteArray &frame, qint64 maximum)
{
    QByteArray chunk(4096, '\0');
    while (true) {
        const qint64 count = file.read(chunk.data(), chunk.size());
        if (count < 0) return false;
        if (count == 0) return true;
        if (frame.size() + count > maximum) return false;
        frame.append(chunk.constData(), qsizetype(count));
    }
}

bool writeAll(QFile &file, const QByteArray &frame)
{
    qsizetype offset = 0;
    while (offset < frame.size()) {
        const qint64 count = file.write(
            frame.constData() + offset, frame.size() - offset);
        if (count <= 0) return false;
        offset += qsizetype(count);
    }
    return file.flush();
}

class HelperDeadline
{
public:
    explicit HelperDeadline(int timeoutMs)
        : watcher(
            [this, timeoutMs]() {
                QElapsedTimer timer;
                timer.start();
                while (!finished.load(std::memory_order_acquire)) {
                    if (timer.elapsed() >= timeoutMs)
                        std::_Exit(124);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(PollIntervalMs));
                }
            })
    {
    }

    ~HelperDeadline()
    {
        finished.store(true, std::memory_order_release);
        watcher.join();
    }

private:
    std::atomic_bool finished{false};
    std::thread watcher;
};

class HelperReaper final : public QThread
{
public:
    enum class Lifecycle {
        NotStarted,
        Accepting,
        Stopping,
        Stopped
    };

    static HelperReaper &instance()
    {
        static HelperReaper *const reaper = new HelperReaper;
        return *reaper;
    }

    void adopt(std::unique_ptr<QProcess> process)
    {
        if (!process) return;
        Q_ASSERT(operationCount() > 0);

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        {
            std::unique_lock<std::mutex> testLock(
                reaperAdoptTestMutex);
            if (reaperAdoptTestPaused) {
                reaperAdoptTestEntered = true;
                reaperAdoptTestCondition.notify_all();
                reaperAdoptTestCondition.wait(
                    testLock, []() {
                        return reaperAdoptTestReleased;
                    });
            }
        }
#endif

        QObject *target = workerTarget();
        if (!target || !isRunning()
            || process->parent()
            || process->thread() != QThread::currentThread()) {
            qFatal("Invalid Local Store helper reaper handoff");
        }

        process->moveToThread(this);
        if (process->thread() != this) {
            qFatal("Could not transfer Local Store helper to reaper");
        }

        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            pendingProcessCount.fetch_add(
                1, std::memory_order_acq_rel);
        }
        {
            const std::lock_guard<std::mutex> lock(pendingMutex);
            pendingProcesses.push_back(std::move(process));
        }
        stateCondition.notify_all();
        if (!invokeWorkerQueued(
                []() {
                    HelperReaper::instance().adoptPending();
                })) {
            qWarning("Could not notify the Local Store helper reaper");
        }
    }

    bool initialize()
    {
        const Lifecycle current =
            lifecycle.load(std::memory_order_acquire);
        if (current != Lifecycle::NotStarted)
            return isAccepting();
        if (!ensureStarted()) return false;

        // Initialization and shutdown are both restricted to the
        // application thread.
        operationState.store(0, std::memory_order_release);
        lifecycle.store(
            Lifecycle::Accepting, std::memory_order_release);
        return true;
    }

    bool isAccepting()
    {
        return lifecycle.load(std::memory_order_acquire)
                == Lifecycle::Accepting
            && !(operationState.load(std::memory_order_acquire)
                 & AdmissionClosedBit);
    }

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    bool isStoppedForTest() const
    {
        return lifecycle.load(std::memory_order_acquire)
            == Lifecycle::Stopped;
    }
#endif

    bool beginOperation()
    {
        quint64 state =
            operationState.load(std::memory_order_acquire);
        while (!(state & AdmissionClosedBit)) {
            if ((state & OperationCountMask)
                == OperationCountMask) {
                return false;
            }
            if (operationState.compare_exchange_weak(
                    state, state + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void endOperation()
    {
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            const quint64 previous = operationState.fetch_sub(
                1, std::memory_order_acq_rel);
            Q_ASSERT((previous & OperationCountMask) > 0);
        }
        stateCondition.notify_all();
    }

    bool shutdown(int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(std::max(0, timeoutMs));

        if (lifecycle.load(std::memory_order_acquire)
            == Lifecycle::Stopped) {
            return true;
        }
        lifecycle.store(
            Lifecycle::Stopping, std::memory_order_release);
        operationState.fetch_or(
            AdmissionClosedBit, std::memory_order_acq_rel);

        if (!isRunning()) {
            if (operationCount() != 0) return false;
            const bool clean = ownershipClean();
            if (clean) {
                lifecycle.store(
                    Lifecycle::Stopped,
                    std::memory_order_release);
            }
            return clean;
        }

        std::unique_lock<std::mutex> stateLock(stateMutex);
        if (!stateCondition.wait_until(
                stateLock, deadline, [this]() {
                    return operationCount() == 0;
                })) {
            return false;
        }
        stateLock.unlock();

        int remaining = remainingMilliseconds(deadline);
        if (remaining <= 0 || !drain(remaining)) return false;

        if (!ownershipClean()) return false;
        quit();
        remaining = remainingMilliseconds(deadline);
        if (remaining <= 0
            || !wait(static_cast<unsigned long>(remaining))) {
            return false;
        }

        if (!ownershipClean()) return false;
        lifecycle.store(
            Lifecycle::Stopped, std::memory_order_release);
        return true;
    }

    bool drain(int timeoutMs)
    {
        if (timeoutMs <= 0) return false;
        if (!isRunning()) {
            if (operationCount() != 0) return false;
            return ownershipClean();
        }

        const quint64 request = drainRequests.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        if (!invokeWorkerQueued(
                []() {
                    HelperReaper::instance().processDrainRequests();
                })) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(stateMutex);
        return stateCondition.wait_until(
            lock, deadline, [this, request]() {
                return completedDrainRequests.load(
                           std::memory_order_acquire) >= request
                    && activeProcessCount.load(
                           std::memory_order_acquire) == 0
                    && pendingProcessCount.load(
                           std::memory_order_acquire) == 0;
            });
    }

private:
    HelperReaper() = default;
    ~HelperReaper() override = default;

    static int remainingMilliseconds(
            const std::chrono::steady_clock::time_point &deadline)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
        return int(std::max<qint64>(1, remaining));
    }

    quint64 operationCount() const
    {
        return operationState.load(std::memory_order_acquire)
            & OperationCountMask;
    }

    template<typename Function>
    bool invokeWorkerQueued(Function &&function)
    {
        const std::lock_guard<std::mutex> lock(startupMutex);
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        ReaperDispatchStartupLockProbe startupLockProbe;
#endif
        QObject *target = workerObject;
        if (!target) return false;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        pauseReaperDispatchForTest();
        if (workerObject != target) {
            reaperDispatchLostTarget.store(
                true, std::memory_order_release);
            return false;
        }
#endif
        return QMetaObject::invokeMethod(
            target, std::forward<Function>(function),
            Qt::QueuedConnection);
    }

    bool ownershipClean()
    {
        if (pendingProcessCount.load(std::memory_order_acquire)
                != 0
            || activeProcessCount.load(std::memory_order_acquire)
                != 0) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(pendingMutex);
        return pendingProcesses.empty();
    }

    void requeuePending(std::unique_ptr<QProcess> process)
    {
        Q_ASSERT(process);
        Q_ASSERT(process->thread() == this);
        const std::lock_guard<std::mutex> lock(pendingMutex);
        pendingProcesses.push_back(std::move(process));
    }

    void completePending(std::unique_ptr<QProcess> process)
    {
        Q_ASSERT(process);
        Q_ASSERT(QThread::currentThread() == this);
        Q_ASSERT(process->thread() == this);
        process.reset();
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            const int previous = pendingProcessCount.fetch_sub(
                1, std::memory_order_acq_rel);
            Q_ASSERT(previous > 0);
        }
        stateCondition.notify_all();
    }

    void adoptPending()
    {
        std::vector<std::unique_ptr<QProcess>> pending;
        {
            const std::lock_guard<std::mutex> lock(pendingMutex);
            pending.swap(pendingProcesses);
        }
        for (std::unique_ptr<QProcess> &process : pending)
            registerProcess(std::move(process));
    }

    void registerProcess(std::unique_ptr<QProcess> process)
    {
        if (!process) return;
        QObject *target = workerTarget();
        if (!target
            || QThread::currentThread() != this
            || process->thread() != this) {
            qFatal("Invalid Local Store helper reaper ownership");
        }

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        reaperRegistrationAttempts.fetch_add(
            1, std::memory_order_relaxed);
        bool retryExpected = false;
        if (qEnvironmentVariableIsSet(
                "GC_TEST_LOCAL_FILE_STORE_REAPER_"
                "REGISTRATION_RETRY_ONCE")
            && reaperRegistrationRetryOnceConsumed
                   .compare_exchange_strong(
                retryExpected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (process->state() != QProcess::NotRunning)
                process->kill();
            requeuePending(std::move(process));
            return;
        }
#endif
        if (process->state() == QProcess::NotRunning) {
            completePending(std::move(process));
            return;
        }

        QProcess *rawProcess = process.get();
        rawProcess->setParent(target);
        processes.insert(rawProcess);
        QObject::connect(
            rawProcess, &QObject::destroyed,
            target, [this, rawProcess]() {
                processes.erase(rawProcess);
                {
                    const std::lock_guard<std::mutex> lock(
                        stateMutex);
                    const int previous =
                            activeProcessCount.fetch_sub(
                        1, std::memory_order_acq_rel);
                    Q_ASSERT(previous > 0);
                }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                reaperProcessCount.fetch_sub(
                    1, std::memory_order_relaxed);
#endif
                stateCondition.notify_all();
            });
        QObject::connect(
            rawProcess,
            qOverload<int, QProcess::ExitStatus>(
                &QProcess::finished),
            rawProcess, &QObject::deleteLater);

        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            activeProcessCount.fetch_add(
                1, std::memory_order_acq_rel);
            const int previous = pendingProcessCount.fetch_sub(
                1, std::memory_order_acq_rel);
            Q_ASSERT(previous > 0);
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        reaperProcessCount.fetch_add(
            1, std::memory_order_relaxed);
#endif
        process.release();
        stateCondition.notify_all();
        if (rawProcess->state() == QProcess::NotRunning) {
            rawProcess->deleteLater();
            return;
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        if (qEnvironmentVariableIsSet(
                "GC_TEST_LOCAL_FILE_STORE_REAPER_HOLD")) {
            return;
        }
#endif
        beginTermination(rawProcess);
    }

    void terminateAll()
    {
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        bool validDelay = false;
        const int delayMs = qEnvironmentVariableIntValue(
            "GC_TEST_LOCAL_FILE_STORE_REAPER_DRAIN_DELAY_MS",
            &validDelay);
        if (validDelay && delayMs > 0
            && delayMs <= MaximumTimeoutMs) {
            QThread::msleep(
                static_cast<unsigned long>(delayMs));
        }
#endif
        adoptPending();
        const std::set<QProcess *> snapshot = processes;
        for (QProcess *process : snapshot)
            beginTermination(process);
    }

    void serviceRetries()
    {
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        {
            const std::lock_guard<std::mutex> lock(
                reaperDispatchTestMutex);
            if (reaperDispatchTestPaused) return;
        }
#endif
        adoptPending();
        if (drainRequests.load(std::memory_order_acquire)
            > completedDrainRequests.load(
                std::memory_order_acquire)) {
            processDrainRequests();
        }
    }

    void processDrainRequests()
    {
        const quint64 request = drainRequests.load(
            std::memory_order_acquire);
        terminateAll();
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        const bool forceDirty = qEnvironmentVariableIsSet(
            "GC_TEST_LOCAL_FILE_STORE_REAPER_FINAL_DIRTY");
#else
        constexpr bool forceDirty = false;
#endif
        if (!forceDirty
            && pendingProcessCount.load(
                   std::memory_order_acquire) == 0
            && activeProcessCount.load(
                   std::memory_order_acquire) == 0) {
            const std::lock_guard<std::mutex> lock(stateMutex);
            completedDrainRequests.store(
                request, std::memory_order_release);
        }
        stateCondition.notify_all();
    }

    bool ensureStarted()
    {
        std::call_once(started, [this]() { start(); });
        std::unique_lock<std::mutex> lock(startupMutex);
        return startupCondition.wait_for(
            lock, std::chrono::milliseconds(TerminationTimeoutMs),
            [this]() { return workerObject != nullptr; });
    }

    QObject *workerTarget()
    {
        const std::lock_guard<std::mutex> lock(startupMutex);
        return workerObject;
    }

    void beginTermination(QProcess *process)
    {
        if (!process || process->state() == QProcess::NotRunning) {
            if (process) process->deleteLater();
            return;
        }
        if (!process->property("_gcReaperTimer").toBool()) {
            process->setProperty("_gcReaperTimer", true);
            QTimer *retry = new QTimer(process);
            retry->setInterval(TerminationTimeoutMs);
            QObject::connect(
                retry, &QTimer::timeout,
                process, [process]() {
                    process->kill();
                });
            retry->start();
        }
        process->kill();
    }

    void run() override
    {
        QObject target;
        QTimer retryTimer;
        retryTimer.setInterval(ReaperRetryIntervalMs);
        QObject::connect(
            &retryTimer, &QTimer::timeout,
            &target, [this]() {
                serviceRetries();
            });
        {
            const std::lock_guard<std::mutex> lock(startupMutex);
            workerObject = &target;
        }
        startupCondition.notify_all();
        retryTimer.start();
        exec();
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        bool probeDispatchLock = false;
        bool dispatchHoldsStartupMutex = false;
        {
            const std::lock_guard<std::mutex> lock(
                reaperDispatchTestMutex);
            probeDispatchLock = reaperDispatchTestPaused;
            dispatchHoldsStartupMutex =
                reaperDispatchHoldsStartupMutex;
        }
        if (probeDispatchLock) {
            if (!dispatchHoldsStartupMutex) {
                reaperDispatchLostTarget.store(
                    true, std::memory_order_release);
            }
            {
                const std::lock_guard<std::mutex> lock(
                    reaperDispatchTestMutex);
                reaperDispatchTeardownProbed = true;
            }
            reaperDispatchTestCondition.notify_all();
        }
#endif
        {
            const std::lock_guard<std::mutex> lock(startupMutex);
            workerObject = nullptr;
        }
    }

    std::once_flag started;
    std::mutex startupMutex;
    std::condition_variable startupCondition;
    QObject *workerObject = nullptr;
    std::mutex pendingMutex;
    static constexpr quint64 AdmissionClosedBit =
            quint64(1) << 63;
    static constexpr quint64 OperationCountMask =
            ~AdmissionClosedBit;
    std::atomic<Lifecycle> lifecycle{Lifecycle::NotStarted};
    std::atomic<quint64> operationState{AdmissionClosedBit};
    std::vector<std::unique_ptr<QProcess>> pendingProcesses;
    std::atomic<int> pendingProcessCount{0};
    std::atomic<int> activeProcessCount{0};
    std::set<QProcess *> processes;
    std::atomic<quint64> drainRequests{0};
    std::atomic<quint64> completedDrainRequests{0};
    std::mutex stateMutex;
    std::condition_variable stateCondition;
};

class HelperOperationLease final
{
public:
    explicit HelperOperationLease(HelperReaper &reaper)
        : reaper(reaper), active(reaper.beginOperation())
    {
    }

    ~HelperOperationLease()
    {
        if (active) reaper.endOperation();
    }

    HelperOperationLease(const HelperOperationLease &) = delete;
    HelperOperationLease &operator=(
        const HelperOperationLease &) = delete;

    explicit operator bool() const { return active; }

private:
    HelperReaper &reaper;
    bool active;
};

void reapHelper(std::unique_ptr<QProcess> helper)
{
    HelperReaper::instance().adopt(std::move(helper));
}



bool stopHelper(QProcess &process)
{
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    if (qEnvironmentVariableIsSet(
            "GC_TEST_LOCAL_FILE_STORE_FORCE_REAPER")) {
        return false;
    }
#endif
    if (process.state() != QProcess::NotRunning) process.kill();
    if (process.state() == QProcess::NotRunning) return true;
    return process.waitForFinished(TerminationTimeoutMs)
        || process.state() == QProcess::NotRunning;
}

LocalFileStoreProcessResult stoppedResult(
        LocalFileStoreProcessResult::Status status,
        const QString &error)
{
    LocalFileStoreProcessResult result;
    result.status = status;
    result.error = error;
    return result;
}

} // namespace

bool LocalFileStoreProcess::initializeReaper()
{
#ifndef Q_OS_UNIX
    return true;
#else
    HelperReaper &reaper = HelperReaper::instance();
    if (reaper.isAccepting()) return true;

    QCoreApplication *application = QCoreApplication::instance();
    if (application
        && QThread::currentThread() != application->thread()) {
        return false;
    }

    return reaper.initialize();
#endif
}

bool LocalFileStoreProcess::shutdownReaper()
{
#ifndef Q_OS_UNIX
    return true;
#else
    QCoreApplication *application = QCoreApplication::instance();
    if (application
        && QThread::currentThread() != application->thread()) {
        return false;
    }

    return HelperReaper::instance().shutdown(
        TerminationTimeoutMs);
#endif
}

bool LocalFileStoreProcess::isHelperInvocation(
        int argc, char *argv[])
{
    return argc >= 2
        && std::strcmp(argv[1], HelperSwitch) == 0;
}

int LocalFileStoreProcess::runHelper(
        const QStringList &arguments)
{
    if (arguments.size() != 3
        || arguments.at(1) != QString::fromLatin1(HelperSwitch)) {
        return 2;
    }

    bool validTimeout = false;
    const int timeoutMs = arguments.at(2).toInt(&validTimeout);
    if (!validTimeout || timeoutMs < 100
        || timeoutMs > MaximumTimeoutMs) {
        return 2;
    }

    int helperDeadlineMs = timeoutMs;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    bool validTestDeadline = false;
    const int testDeadline = qEnvironmentVariableIntValue(
        "GC_TEST_LOCAL_FILE_STORE_HELPER_DEADLINE_MS",
        &validTestDeadline);
    if (validTestDeadline
        && testDeadline >= timeoutMs
        && testDeadline <= MaximumTimeoutMs) {
        helperDeadlineMs = testDeadline;
    }
#endif
    HelperDeadline deadline(helperDeadlineMs);

    QFile input;
    if (!input.open(
            stdin, QIODevice::ReadOnly,
            QFileDevice::DontCloseHandle)) {
        return 3;
    }
    QByteArray requestFrame;
    if (!readBoundedFrame(input, requestFrame, MaxRequestBytes)) {
        return 3;
    }

    Request request;
    if (!parseRequest(requestFrame, request)) return 3;
    const LocalFileStoreProcessResult result = executeRequest(request);

    QByteArray responseFrame;
    if (!serializeResponse(request.operation, result, responseFrame)) {
        return 4;
    }

    QFile output;
    if (!output.open(
            stdout, QIODevice::WriteOnly,
            QFileDevice::DontCloseHandle)
        || !writeAll(output, responseFrame)) {
        return 4;
    }
    return 0;
}

LocalFileStoreProcessResult LocalFileStoreProcess::run(
        Operation operation,
        const QString &root,
        const QString &path,
        int timeoutMs)
{
    timeoutMs = std::clamp(timeoutMs, 100, MaximumTimeoutMs);

    Request request{operation, root, path};
    QByteArray requestFrame;
    if (!serializeRequest(request, requestFrame)) {
        return failedResult(QStringLiteral("invalid-request"));
    }

    if (!initializeReaper()) {
        return failedResult(QStringLiteral("helper-reaper-unavailable"));
    }

    HelperOperationLease operationLease(
        HelperReaper::instance());
    if (!operationLease) {
        return failedResult(QStringLiteral("helper-reaper-unavailable"));
    }

    const QString executable = QCoreApplication::applicationFilePath();
    if (executable.isEmpty()) {
        return failedResult(QStringLiteral("helper-executable-missing"));
    }

    auto helper = std::make_unique<QProcess>();
    helper->setProgram(executable);
    helper->setArguments({
        QString::fromLatin1(HelperSwitch),
        QString::number(timeoutMs)});
    helper->setProcessChannelMode(QProcess::SeparateChannels);

#if defined(GC_TEST_CLOUD_AUTODOWNLOAD_PROBE) \
    && defined(Q_OS_UNIX) \
    && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool validStartDelay = false;
    const int startDelayMs = qEnvironmentVariableIntValue(
        "GC_TEST_LOCAL_FILE_STORE_START_DELAY_MS",
        &validStartDelay);
    if (validStartDelay
        && startDelayMs > 0
        && startDelayMs <= MaximumTimeoutMs) {
        helper->setChildProcessModifier([startDelayMs]() {
            struct timespec remaining {
                startDelayMs / 1000,
                long(startDelayMs % 1000) * 1000000L
            };
            while (::nanosleep(&remaining, &remaining) != 0
                   && errno == EINTR) {
            }
        });
    }
#endif

    QElapsedTimer timer;
    timer.start();
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    helperProcessStartCount.fetch_add(1, std::memory_order_relaxed);
#endif
    helper->start(QIODevice::ReadWrite);

    while (helper->state() == QProcess::Starting) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            if (!stopHelper(*helper)) {
                reapHelper(std::move(helper));
                return failedResult(
                    QStringLiteral("helper-termination-failed"));
            }
            return stoppedResult(
                LocalFileStoreProcessResult::Status::Cancelled,
                QStringLiteral("cancelled"));
        }
        if (timer.elapsed() >= timeoutMs) {
            if (!stopHelper(*helper)) {
                reapHelper(std::move(helper));
                return failedResult(
                    QStringLiteral("helper-termination-failed"));
            }
            return stoppedResult(
                LocalFileStoreProcessResult::Status::TimedOut,
                QStringLiteral("timed-out"));
        }
        helper->waitForStarted(PollIntervalMs);
    }
    if (helper->state() != QProcess::Running) {
        if (!stopHelper(*helper)) reapHelper(std::move(helper));
        return failedResult(QStringLiteral("helper-start-failed"));
    }

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    lastHelperProcessId.store(
        helper->processId(), std::memory_order_release);
#endif

    if (helper->write(requestFrame) != requestFrame.size()) {
        if (!stopHelper(*helper)) reapHelper(std::move(helper));
        return failedResult(QStringLiteral("helper-request-failed"));
    }
    helper->closeWriteChannel();

    QByteArray responseFrame;
    while (helper->state() != QProcess::NotRunning) {
        const QByteArray output = helper->readAllStandardOutput();
        helper->readAllStandardError();
        if (responseFrame.size() + output.size() > MaxResponseBytes) {
            if (!stopHelper(*helper)) reapHelper(std::move(helper));
            return failedResult(QStringLiteral("helper-response-too-large"));
        }
        responseFrame.append(output);

        if (QThread::currentThread()->isInterruptionRequested()) {
            if (!stopHelper(*helper)) {
                reapHelper(std::move(helper));
                return failedResult(
                    QStringLiteral("helper-termination-failed"));
            }
            return stoppedResult(
                LocalFileStoreProcessResult::Status::Cancelled,
                QStringLiteral("cancelled"));
        }
        if (timer.elapsed() >= timeoutMs) {
            if (!stopHelper(*helper)) {
                reapHelper(std::move(helper));
                return failedResult(
                    QStringLiteral("helper-termination-failed"));
            }
            return stoppedResult(
                LocalFileStoreProcessResult::Status::TimedOut,
                QStringLiteral("timed-out"));
        }

        helper->waitForReadyRead(PollIntervalMs);
    }

    const QByteArray output = helper->readAllStandardOutput();
    helper->readAllStandardError();
    if (responseFrame.size() + output.size() > MaxResponseBytes) {
        return failedResult(QStringLiteral("helper-response-too-large"));
    }
    responseFrame.append(output);

    if (helper->exitStatus() != QProcess::NormalExit
        || helper->exitCode() != 0) {
        if (helper->exitStatus() == QProcess::NormalExit
            && helper->exitCode() == 124) {
            return stoppedResult(
                LocalFileStoreProcessResult::Status::TimedOut,
                QStringLiteral("timed-out"));
        }
        return failedResult(QStringLiteral("helper-failed"));
    }

    LocalFileStoreProcessResult result;
    if (!parseResponse(responseFrame, operation, result)) {
        return failedResult(QStringLiteral("invalid-response"));
    }
    return result;
}

LocalFileStoreProcessResult
LocalFileStoreProcess::readFileInProcess(
        const QString &root, const QString &name)
{
#ifdef Q_OS_UNIX
    return executeReadUnix(root, name);
#else
    Q_UNUSED(root)
    Q_UNUSED(name)
    return failedResult(QStringLiteral("unsupported-platform"));
#endif
}

LocalFileStoreProcessResult
LocalFileStoreProcess::writeFileInProcess(
        const QString &root,
        const QString &name,
        const QByteArray &data)
{
#ifdef Q_OS_UNIX
    return executeWriteUnix(root, name, data);
#else
    Q_UNUSED(root)
    Q_UNUSED(name)
    Q_UNUSED(data)
    return failedResult(QStringLiteral("unsupported-platform"));
#endif
}

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
qint64 LocalFileStoreProcess::lastHelperProcessIdForTest()
{
    return lastHelperProcessId.load(std::memory_order_acquire);
}

int LocalFileStoreProcess::helperProcessStartCountForTest()
{
    return helperProcessStartCount.load(std::memory_order_relaxed);
}

bool LocalFileStoreProcess::helperProcessIsRunningForTest(
        qint64 processId)
{
    if (processId <= 0) return false;
#ifdef Q_OS_UNIX
    if (::kill(pid_t(processId), 0) == 0) return true;
    return errno == EPERM;
#elif defined(Q_OS_WIN)
    HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, DWORD(processId));
    if (!process) return false;
    DWORD exitCode = 0;
    const bool running =
            GetExitCodeProcess(process, &exitCode)
            && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return false;
#endif
}

int LocalFileStoreProcess::reaperRegistrationAttemptCountForTest()
{
    return reaperRegistrationAttempts.load(std::memory_order_relaxed);
}

int LocalFileStoreProcess::reaperProcessCountForTest()
{
    return reaperProcessCount.load(std::memory_order_relaxed);
}

bool LocalFileStoreProcess::drainReaperForTest(int timeoutMs)
{
    return HelperReaper::instance().drain(timeoutMs);
}

bool LocalFileStoreProcess::reaperIsStoppedForTest()
{
    return HelperReaper::instance().isStoppedForTest();
}

QThread *LocalFileStoreProcess::reaperOwnerThreadForTest()
{
    return HelperReaper::instance().thread();
}

void LocalFileStoreProcess::prepareReaperAdoptPauseForTest()
{
    const std::lock_guard<std::mutex> lock(reaperAdoptTestMutex);
    reaperAdoptTestPaused = true;
    reaperAdoptTestEntered = false;
    reaperAdoptTestReleased = false;
}

bool LocalFileStoreProcess::waitForReaperAdoptPauseForTest(
        int timeoutMs)
{
    std::unique_lock<std::mutex> lock(reaperAdoptTestMutex);
    return reaperAdoptTestCondition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), []() {
            return reaperAdoptTestEntered;
        });
}

void LocalFileStoreProcess::releaseReaperAdoptForTest()
{
    {
        const std::lock_guard<std::mutex> lock(
            reaperAdoptTestMutex);
        reaperAdoptTestReleased = true;
        reaperAdoptTestPaused = false;
    }
    reaperAdoptTestCondition.notify_all();
}

void LocalFileStoreProcess::prepareReaperDispatchPauseForTest()
{
    const std::lock_guard<std::mutex> lock(
        reaperDispatchTestMutex);
    reaperDispatchTestPaused = true;
    reaperDispatchTestEntered = false;
    reaperDispatchTestReleased = false;
    reaperDispatchTeardownProbed = false;
    reaperDispatchHoldsStartupMutex = false;
    reaperDispatchLostTarget.store(
        false, std::memory_order_release);
}

bool LocalFileStoreProcess::waitForReaperDispatchPauseForTest(
        int timeoutMs)
{
    std::unique_lock<std::mutex> lock(reaperDispatchTestMutex);
    return reaperDispatchTestCondition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), []() {
            return reaperDispatchTestEntered;
        });
}

void LocalFileStoreProcess::stopReaperEventLoopForTest()
{
    HelperReaper::instance().quit();
}

bool LocalFileStoreProcess::waitForReaperTeardownProbeForTest(
        int timeoutMs)
{
    std::unique_lock<std::mutex> lock(reaperDispatchTestMutex);
    return reaperDispatchTestCondition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), []() {
            return reaperDispatchTeardownProbed;
        });
}

void LocalFileStoreProcess::releaseReaperDispatchForTest()
{
    {
        const std::lock_guard<std::mutex> lock(
            reaperDispatchTestMutex);
        reaperDispatchTestReleased = true;
        reaperDispatchTestPaused = false;
    }
    reaperDispatchTestCondition.notify_all();
}

bool LocalFileStoreProcess::waitForReaperThreadForTest(
        int timeoutMs)
{
    return HelperReaper::instance().wait(
        static_cast<unsigned long>(std::max(0, timeoutMs)));
}

bool LocalFileStoreProcess::reaperDispatchLostTargetForTest()
{
    return reaperDispatchLostTarget.load(
        std::memory_order_acquire);
}

bool LocalFileStoreProcess::parseResponseForTest(
        const QByteArray &frame,
        Operation expectedOperation,
        LocalFileStoreProcessResult &result)
{
    return parseResponse(frame, expectedOperation, result);
}
#endif
