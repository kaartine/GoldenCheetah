/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AnchoredFileSystem.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <stdio.h>
#endif
#if defined(Q_OS_LINUX)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

#ifdef Q_OS_WIN
#include <array>
#include <cstddef>
#include <qt_windows.h>
#endif

#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary);
bool anchoredFilesystemSyncFailureRequested(const QString &path);
#endif

namespace AnchoredFileSystem {
namespace {

void reportAnchoredFilesystemTransition(
    const char *transition,
    const QString &primary,
    const QString &secondary = QString())
{
#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
    anchoredFilesystemTransitionReached(
        transition, primary, secondary);
#else
    Q_UNUSED(transition)
    Q_UNUSED(primary)
    Q_UNUSED(secondary)
#endif
}

QString nativeError(const QString &operation, int errorNumber)
{
    return QStringLiteral("%1: %2")
        .arg(operation,
             QString::fromLocal8Bit(std::strerror(errorNumber)));
}

bool validPortableComponent(const QString &component)
{
    if (component.isEmpty()
        || component == QStringLiteral(".")
        || component == QStringLiteral("..")
        || QDir::isAbsolutePath(component)
        || QFileInfo(component).fileName() != component
        || component.endsWith(QLatin1Char(' '))
        || component.endsWith(QLatin1Char('.'))
        || component.toUtf8().size() > 240) {
        return false;
    }

    static const QString forbidden =
        QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : component) {
        const ushort value = character.unicode();
        if (value <= 0x1f || value == 0x7f
            || forbidden.contains(character)) {
            return false;
        }
    }

    const int dot = component.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? component : component.left(dot))
        .toUpper();
    static const QStringList reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    return !reserved.contains(stem);
}

#ifdef Q_OS_UNIX

class FileDescriptor
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

    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }

    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other) {
            if (descriptor_ >= 0) ::close(descriptor_);
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    bool isValid() const { return descriptor_ >= 0; }
    int get() const { return descriptor_; }

private:
    int descriptor_ = -1;
};

struct UnixStamp
{
    quint64 device = 0;
    quint64 inode = 0;
    quint64 links = 0;
    qint64 size = -1;
    qint64 modifiedSeconds = 0;
    qint64 modifiedNanoseconds = 0;
    qint64 changedSeconds = 0;
    qint64 changedNanoseconds = 0;

    bool operator==(const UnixStamp &other) const
    {
        return device == other.device
            && inode == other.inode
            && links == other.links
            && size == other.size
            && modifiedSeconds == other.modifiedSeconds
            && modifiedNanoseconds == other.modifiedNanoseconds
            && changedSeconds == other.changedSeconds
            && changedNanoseconds == other.changedNanoseconds;
    }
};

bool sameUnixObjectAndData(
    const UnixStamp &left, const UnixStamp &right)
{
    return left.device == right.device
        && left.inode == right.inode
        && left.size == right.size
        && left.modifiedSeconds == right.modifiedSeconds
        && left.modifiedNanoseconds == right.modifiedNanoseconds;
}

bool sameUnixIdentityAndData(
    const UnixStamp &left, const UnixStamp &right)
{
    return left.links == right.links
        && sameUnixObjectAndData(left, right);
}

bool captureUnixStamp(
    int descriptor, UnixStamp &stamp, bool directory, QString &error)
{
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        error = nativeError(
            QStringLiteral("Cannot inspect an anchored filesystem entry"),
            errno);
        return false;
    }
    if ((directory && !S_ISDIR(status.st_mode))
        || (!directory && !S_ISREG(status.st_mode))
        || status.st_size < 0) {
        error = QStringLiteral(
            "The anchored filesystem entry has an unsafe type");
        return false;
    }
    stamp.device = quint64(status.st_dev);
    stamp.inode = quint64(status.st_ino);
    stamp.links = quint64(status.st_nlink);
    stamp.size = qint64(status.st_size);
#if defined(Q_OS_MACOS)
    stamp.modifiedSeconds = qint64(status.st_mtimespec.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtimespec.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctimespec.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctimespec.tv_nsec);
#else
    stamp.modifiedSeconds = qint64(status.st_mtim.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtim.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctim.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctim.tv_nsec);
#endif
    return true;
}

NativeIdentity unixIdentity(const UnixStamp &stamp, char type)
{
    QByteArray key;
    key.reserve(64);
    key += type;
    key += ':';
    key += QByteArray::number(stamp.device, 16);
    key += ':';
    key += QByteArray::number(stamp.inode, 16);
    return NativeIdentity(std::move(key), stamp.links);
}

bool statEntry(
    int directory,
    const QString &component,
    UnixStamp &stamp,
    bool &exists,
    QString &error)
{
    exists = false;
    const QByteArray name = QFile::encodeName(component);
    struct stat status {};
    if (::fstatat(
            directory,
            name.constData(),
            &status,
            AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        error = nativeError(
            QStringLiteral("Cannot inspect an anchored file name"), errno);
        return false;
    }
    exists = true;
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        error = QStringLiteral(
            "The anchored file name has an unsafe type");
        return false;
    }
    stamp.device = quint64(status.st_dev);
    stamp.inode = quint64(status.st_ino);
    stamp.links = quint64(status.st_nlink);
    stamp.size = qint64(status.st_size);
#if defined(Q_OS_MACOS)
    stamp.modifiedSeconds = qint64(status.st_mtimespec.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtimespec.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctimespec.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctimespec.tv_nsec);
#else
    stamp.modifiedSeconds = qint64(status.st_mtim.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtim.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctim.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctim.tv_nsec);
#endif
    return true;
}

int renameNoReplaceNative(
    int sourceDirectory,
    const QByteArray &source,
    int destinationDirectory,
    const QByteArray &destination,
    bool &unsupported)
{
    unsupported = false;
#if defined(Q_OS_LINUX) && defined(SYS_renameat2) \
    && defined(RENAME_NOREPLACE)
    const int result = int(::syscall(
        SYS_renameat2,
        sourceDirectory,
        source.constData(),
        destinationDirectory,
        destination.constData(),
        RENAME_NOREPLACE));
    if (result == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL
#ifdef EOPNOTSUPP
        && errno != EOPNOTSUPP
#endif
    ) {
        return -1;
    }
    unsupported = true;
#elif defined(Q_OS_MACOS) && defined(RENAME_EXCL)
    const int result = ::renameatx_np(
        sourceDirectory,
        source.constData(),
        destinationDirectory,
        destination.constData(),
        RENAME_EXCL);
    if (result == 0) return 0;
    if (errno == ENOTSUP) unsupported = true;
#else
    Q_UNUSED(sourceDirectory)
    Q_UNUSED(source)
    Q_UNUSED(destinationDirectory)
    Q_UNUSED(destination)
    unsupported = true;
#endif
    return -1;
}

#endif

#ifdef Q_OS_WIN

class WindowsHandle
{
public:
    explicit WindowsHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle)
    {
    }

    ~WindowsHandle()
    {
        if (isValid()) ::CloseHandle(handle_);
    }

    WindowsHandle(WindowsHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }

    WindowsHandle &operator=(WindowsHandle &&other) noexcept
    {
        if (this != &other) {
            if (isValid()) ::CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    WindowsHandle(const WindowsHandle &) = delete;
    WindowsHandle &operator=(const WindowsHandle &) = delete;

    bool isValid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const { return handle_; }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE)
    {
        if (isValid()) ::CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct WindowsStamp
{
    quint64 volume = 0;
    std::array<unsigned char, 16> id {};
    quint64 links = 0;
    qint64 size = -1;
    qint64 modified = 0;
    qint64 changed = 0;

    bool operator==(const WindowsStamp &other) const
    {
        return volume == other.volume
            && id == other.id
            && links == other.links
            && size == other.size
            && modified == other.modified
            && changed == other.changed;
    }
};

bool sameWindowsObject(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return left.volume == right.volume
        && left.id == right.id;
}

bool sameWindowsIdentityAndData(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return sameWindowsObject(left, right)
        && left.links == right.links
        && left.size == right.size
        && left.modified == right.modified;
}

QString windowsError(const QString &operation, DWORD nativeError)
{
    return QStringLiteral("%1: system error %2")
        .arg(operation).arg(nativeError);
}

bool captureWindowsStamp(
    HANDLE handle, WindowsStamp &stamp, bool directory, QString &error)
{
    FILE_ATTRIBUTE_TAG_INFO attributes {};
    FILE_STANDARD_INFO standard {};
    FILE_BASIC_INFO basic {};
    FILE_ID_INFO identity {};
    BY_HANDLE_FILE_INFORMATION legacy {};
    if (!::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo,
            &attributes, sizeof(attributes))
        || !::GetFileInformationByHandleEx(
            handle, FileStandardInfo,
            &standard, sizeof(standard))
        || !::GetFileInformationByHandleEx(
            handle, FileBasicInfo,
            &basic, sizeof(basic))
        || !::GetFileInformationByHandleEx(
            handle, FileIdInfo,
            &identity, sizeof(identity))
        || !::GetFileInformationByHandle(handle, &legacy)) {
        error = windowsError(
            QStringLiteral("Cannot inspect an anchored filesystem entry"),
            ::GetLastError());
        return false;
    }
    const bool isDirectory =
        attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (isDirectory != directory
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || standard.EndOfFile.QuadPart < 0) {
        error = QStringLiteral(
            "The anchored filesystem entry has an unsafe type");
        return false;
    }
    stamp.volume = identity.VolumeSerialNumber;
    std::copy(
        std::begin(identity.FileId.Identifier),
        std::end(identity.FileId.Identifier),
        stamp.id.begin());
    stamp.links = legacy.nNumberOfLinks;
    stamp.size = standard.EndOfFile.QuadPart;
    stamp.modified = basic.LastWriteTime.QuadPart;
    stamp.changed = basic.ChangeTime.QuadPart;
    return true;
}

NativeIdentity windowsIdentity(const WindowsStamp &stamp, char type)
{
    QByteArray key;
    key.reserve(1 + int(sizeof(stamp.volume) + stamp.id.size()));
    key += type;
    key.append(
        reinterpret_cast<const char *>(&stamp.volume),
        int(sizeof(stamp.volume)));
    key.append(
        reinterpret_cast<const char *>(stamp.id.data()),
        int(stamp.id.size()));
    return NativeIdentity(std::move(key), stamp.links);
}

WindowsHandle openWindowsDirectoryHandle(
    const QString &path,
    bool allowChildMutation,
    QString &error,
    DWORD *nativeError = nullptr)
{
    DWORD access = FILE_LIST_DIRECTORY | FILE_TRAVERSE
        | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (allowChildMutation) {
        access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    }
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle.isValid()) {
        const DWORD failure = ::GetLastError();
        if (nativeError) *nativeError = failure;
        error = windowsError(
            QStringLiteral("Cannot open an anchored directory"),
            failure);
    } else if (nativeError) {
        *nativeError = ERROR_SUCCESS;
    }
    return handle;
}

#endif

} // namespace

namespace Detail {

struct DirectoryState
{
    std::shared_ptr<DirectoryState> ancestor;
#ifdef Q_OS_UNIX
    FileDescriptor descriptor;
    UnixStamp stamp;
#elif defined(Q_OS_WIN)
    std::vector<WindowsHandle> handles;
    WindowsStamp stamp;
#endif
    QString displayPath;
    NativeIdentity identity;
};

struct PinnedFileState
{
    EntryRef entry;
#ifdef Q_OS_UNIX
    FileDescriptor descriptor;
    UnixStamp stamp;
#elif defined(Q_OS_WIN)
    WindowsHandle handle;
    WindowsStamp stamp;
#endif
    NativeIdentity identity;
    qint64 size = -1;
    QByteArray sha256;
};

} // namespace Detail

namespace {

using PinnedChunkConsumer =
    std::function<bool(const char *, qsizetype, QString &)>;

bool streamPinnedFile(
    const Detail::PinnedFileState &state,
    const PinnedChunkConsumer &consume,
    QByteArray &digest,
    QString &error)
{
    error.clear();
    digest.clear();
    if (!consume || state.size < 0) {
        error = QStringLiteral("The anchored file cannot be read");
        return false;
    }

#ifdef Q_OS_UNIX
    UnixStamp before;
    if (!captureUnixStamp(
            state.descriptor.get(), before, false, error)) {
        return false;
    }
    if (!sameUnixIdentityAndData(before, state.stamp)) {
        error = QStringLiteral(
            "The anchored file changed before it was read");
        return false;
    }
#elif defined(Q_OS_WIN)
    WindowsStamp before;
    if (!captureWindowsStamp(
            state.handle.get(), before, false, error)) {
        return false;
    }
    if (!sameWindowsIdentityAndData(before, state.stamp)) {
        error = QStringLiteral(
            "The anchored file changed before it was read");
        return false;
    }
    LARGE_INTEGER zero {};
    LARGE_INTEGER savedPosition {};
    if (!::SetFilePointerEx(
            state.handle.get(), zero,
            &savedPosition, FILE_CURRENT)
        || !::SetFilePointerEx(
            state.handle.get(), zero,
            nullptr, FILE_BEGIN)) {
        error = windowsError(
            QStringLiteral("Cannot seek an anchored regular file"),
            ::GetLastError());
        return false;
    }
#else
    Q_UNUSED(state)
    Q_UNUSED(consume)
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, '\0');
    qint64 offset = 0;
    bool readSucceeded = true;
    while (offset < state.size) {
        const qsizetype requested = qsizetype(std::min<qint64>(
            chunk.size(), state.size - offset));
        qsizetype received = 0;
#ifdef Q_OS_UNIX
        ssize_t nativeRead;
        do {
            nativeRead = ::pread(
                state.descriptor.get(), chunk.data(),
                size_t(requested), off_t(offset));
        } while (nativeRead < 0 && errno == EINTR);
        if (nativeRead <= 0) {
            error = nativeRead < 0
                ? nativeError(
                      QStringLiteral("Cannot read an anchored regular file"),
                      errno)
                : QStringLiteral(
                      "The anchored regular file ended unexpectedly");
            readSucceeded = false;
            break;
        }
        received = qsizetype(nativeRead);
#elif defined(Q_OS_WIN)
        DWORD nativeRead = 0;
        if (!::ReadFile(
                state.handle.get(), chunk.data(),
                DWORD(requested), &nativeRead, nullptr)
            || nativeRead == 0) {
            error = windowsError(
                QStringLiteral("Cannot read an anchored regular file"),
                ::GetLastError());
            readSucceeded = false;
            break;
        }
        received = qsizetype(nativeRead);
#endif
        if (!consume(chunk.constData(), received, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot consume anchored file contents");
            }
            readSucceeded = false;
            break;
        }
        hash.addData(chunk.constData(), received);
        offset += received;
    }

    if (readSucceeded) {
        char trailing = 0;
#ifdef Q_OS_UNIX
        ssize_t trailingRead;
        do {
            trailingRead = ::pread(
                state.descriptor.get(), &trailing, 1,
                off_t(state.size));
        } while (trailingRead < 0 && errno == EINTR);
        if (trailingRead != 0) {
            error = trailingRead < 0
                ? nativeError(
                      QStringLiteral("Cannot finish reading an anchored file"),
                      errno)
                : QStringLiteral(
                      "The anchored regular file grew while being read");
            readSucceeded = false;
        }
#elif defined(Q_OS_WIN)
        DWORD trailingRead = 0;
        if (!::ReadFile(
                state.handle.get(), &trailing, 1,
                &trailingRead, nullptr)
            || trailingRead != 0) {
            error = trailingRead != 0
                ? QStringLiteral(
                      "The anchored regular file grew while being read")
                : windowsError(
                      QStringLiteral("Cannot finish reading an anchored file"),
                      ::GetLastError());
            readSucceeded = false;
        }
#endif
    }

#ifdef Q_OS_WIN
    if (!::SetFilePointerEx(
            state.handle.get(), savedPosition,
            nullptr, FILE_BEGIN)) {
        if (readSucceeded) {
            error = windowsError(
                QStringLiteral(
                    "Cannot restore an anchored file position"),
                ::GetLastError());
        }
        readSucceeded = false;
    }
#endif
    if (!readSucceeded) return false;

#ifdef Q_OS_UNIX
    UnixStamp after;
    if (!captureUnixStamp(
            state.descriptor.get(), after, false, error)) {
        return false;
    }
    const bool stable = sameUnixIdentityAndData(before, after);
#elif defined(Q_OS_WIN)
    WindowsStamp after;
    if (!captureWindowsStamp(
            state.handle.get(), after, false, error)) {
        return false;
    }
    const bool stable = sameWindowsIdentityAndData(before, after);
#endif
    digest = hash.result();
    if (!stable || offset != state.size || digest != state.sha256) {
        error = QStringLiteral(
            "The anchored regular file changed while being read");
        digest.clear();
        return false;
    }
    return true;
}

void appendCleanupError(
    QString &error, const MutationResult &cleanup)
{
    if (cleanup.effect == MutationEffect::AppliedDurable) return;
    if (!error.isEmpty()) error += QStringLiteral("; ");
    if (cleanup.effect == MutationEffect::AppliedNotDurable) {
        error += QStringLiteral(
            "incomplete anchored copy cleanup was not durable");
        if (!cleanup.error.isEmpty()) {
            error += QStringLiteral(": ") + cleanup.error;
        }
        return;
    }
    error += cleanup.error.isEmpty()
        ? QStringLiteral("cannot remove an incomplete anchored copy")
        : cleanup.error;
}

} // namespace

QDebug operator<<(QDebug debug, const NativeIdentity &identity)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "NativeIdentity(valid=" << identity.isValid()
                    << ", links=" << identity.linkCount_ << ')';
    return debug;
}

bool DirectoryAnchor::open(
    const QString &absolutePath,
    DirectoryAnchor &directory,
    QString &error)
{
    error.clear();
    directory = {};
    if (absolutePath.isEmpty() || !QDir::isAbsolutePath(absolutePath)) {
        error = QStringLiteral(
            "An anchored directory path must be absolute");
        return false;
    }
    const QString cleaned = QDir::cleanPath(
        QFileInfo(absolutePath).absoluteFilePath());

#ifdef Q_OS_UNIX
    FileDescriptor current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot open the filesystem root"), errno);
        return false;
    }
    const QStringList components = cleaned.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        const QByteArray encoded = QFile::encodeName(component);
        FileDescriptor next(::openat(
            current.get(), encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next.isValid()) {
            error = nativeError(
                QStringLiteral("Cannot open an anchored directory component"),
                errno);
            return false;
        }
        current = std::move(next);
    }
    UnixStamp stamp;
    if (!captureUnixStamp(current.get(), stamp, true, error)) return false;

    auto state = std::make_shared<Detail::DirectoryState>();
    state->descriptor = std::move(current);
    state->stamp = stamp;
    state->displayPath = cleaned;
    state->identity = unixIdentity(stamp, 'd');
    directory = DirectoryAnchor(std::move(state));
    return true;
#elif defined(Q_OS_WIN)
    const QString native = QDir::toNativeSeparators(cleaned);
    QString prefix;
    QString remainder;
    if (native.size() >= 3
        && native.at(1) == QLatin1Char(':')
        && (native.at(2) == QLatin1Char('\\')
            || native.at(2) == QLatin1Char('/'))) {
        prefix = native.left(3);
        remainder = native.mid(3);
    } else {
        error = QStringLiteral(
            "Only absolute local Windows paths can be anchored");
        return false;
    }

    auto state = std::make_shared<Detail::DirectoryState>();
    QString handleError;
    WindowsHandle rootHandle = openWindowsDirectoryHandle(
        prefix, false, handleError);
    if (!rootHandle.isValid()) {
        error = handleError;
        return false;
    }
    state->handles.push_back(std::move(rootHandle));
    const QStringList components = remainder.split(
        QLatin1Char('\\'), Qt::SkipEmptyParts);
    for (int index = 0; index < components.size(); ++index) {
        const QString &component = components.at(index);
        prefix = QDir::toNativeSeparators(
            QDir::cleanPath(QDir::fromNativeSeparators(prefix)
                + QLatin1Char('/') + component));
        WindowsHandle next = openWindowsDirectoryHandle(
            prefix, index == components.size() - 1,
            handleError);
        if (!next.isValid()) {
            error = handleError;
            return false;
        }
        state->handles.push_back(std::move(next));
    }
    if (!captureWindowsStamp(
            state->handles.back().get(), state->stamp, true, error)) {
        return false;
    }
    state->displayPath = cleaned;
    state->identity = windowsIdentity(state->stamp, 'd');
    directory = DirectoryAnchor(std::move(state));
    return true;
#else
    Q_UNUSED(cleaned)
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

NativeIdentity DirectoryAnchor::identity() const
{
    return state_ ? state_->identity : NativeIdentity();
}

bool DirectoryAnchor::openChild(
    const QString &component,
    DirectoryAnchor &directory,
    QString &error) const
{
    bool exists = false;
    if (!openChildIfExists(
            component, directory, exists, error)) {
        return false;
    }
    if (exists) return true;
    error = QStringLiteral(
        "The anchored child directory does not exist");
    return false;
}

bool DirectoryAnchor::openChildIfExists(
    const QString &component,
    DirectoryAnchor &directory,
    bool &exists,
    QString &error) const
{
    directory = {};
    exists = false;
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return false;
    }
    if (!validPortableComponent(component)) {
        error = QStringLiteral("The anchored directory name is unsafe");
        return false;
    }
    const QString displayPath =
        QDir(state_->displayPath).filePath(component);
    std::shared_ptr<Detail::DirectoryState> child;

#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(component);
    FileDescriptor descriptor(::openat(
        state_->descriptor.get(), encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.isValid()) {
        if (errno == ENOENT) return true;
        error = nativeError(
            QStringLiteral("Cannot open an anchored child directory"),
            errno);
        return false;
    }
    UnixStamp stamp;
    if (!captureUnixStamp(
            descriptor.get(), stamp, true, error)) {
        return false;
    }
    child = std::make_shared<Detail::DirectoryState>();
    child->ancestor = state_;
    child->descriptor = std::move(descriptor);
    child->stamp = stamp;
    child->displayPath = displayPath;
    child->identity = unixIdentity(stamp, 'd');
#elif defined(Q_OS_WIN)
    DWORD native = ERROR_SUCCESS;
    WindowsHandle handle = openWindowsDirectoryHandle(
        QDir::toNativeSeparators(displayPath), true, error, &native);
    if (!handle.isValid()) {
        if (native == ERROR_FILE_NOT_FOUND
            || native == ERROR_PATH_NOT_FOUND) {
            error.clear();
            return true;
        }
        return false;
    }
    WindowsStamp stamp;
    if (!captureWindowsStamp(
            handle.get(), stamp, true, error)) {
        return false;
    }
    child = std::make_shared<Detail::DirectoryState>();
    child->ancestor = state_;
    child->handles.push_back(std::move(handle));
    child->stamp = stamp;
    child->displayPath = displayPath;
    child->identity = windowsIdentity(stamp, 'd');
#else
    Q_UNUSED(displayPath)
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
    directory = DirectoryAnchor(std::move(child));
    exists = true;
    return true;
}

EntryRef DirectoryAnchor::entry(
    const QString &component, QString &error) const
{
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return {};
    }
    if (!validPortableComponent(component)) {
        error = QStringLiteral("The anchored file name is unsafe");
        return {};
    }
    return EntryRef(
        *this,
        component,
        QDir(state_->displayPath).filePath(component));
}

bool DirectoryAnchor::pathMatches(QString &error) const
{
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return false;
    }
    DirectoryAnchor current;
    if (!DirectoryAnchor::open(
            state_->displayPath, current, error)) {
        return false;
    }
    return current.identity() == identity();
}

bool DirectoryAnchor::sync(QString &error) const
{
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return false;
    }
#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
    if (anchoredFilesystemSyncFailureRequested(state_->displayPath)) {
        error = QStringLiteral(
            "Injected anchored directory synchronization failure");
        return false;
    }
#endif
#ifdef Q_OS_UNIX
    int result;
    do {
        result = ::fsync(state_->descriptor.get());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = nativeError(
            QStringLiteral("Cannot synchronize the anchored directory"),
            errno);
        return false;
    }
#elif defined(Q_OS_WIN)
    // Entry mutations use write-through file handles. Windows has no
    // documented directory-handle equivalent of fsync().
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
    return true;
}

PinnedFile::PinnedFile() = default;
PinnedFile::~PinnedFile() = default;
PinnedFile::PinnedFile(PinnedFile &&other) noexcept = default;
PinnedFile &PinnedFile::operator=(PinnedFile &&other) noexcept = default;

NativeIdentity PinnedFile::identity() const
{
    return state_ ? state_->identity : NativeIdentity();
}

qint64 PinnedFile::size() const
{
    return state_ ? state_->size : -1;
}

QByteArray PinnedFile::sha256() const
{
    return state_ ? state_->sha256 : QByteArray();
}

bool readAll(
    const PinnedFile &file,
    qint64 maximumSize,
    QByteArray &contents,
    QString &error)
{
    contents.clear();
    error.clear();
    if (!file.state_ || maximumSize < 0) {
        error = QStringLiteral("The anchored file cannot be read");
        return false;
    }
    if (file.state_->size > maximumSize
        || quint64(file.state_->size)
            > quint64(std::numeric_limits<qsizetype>::max())) {
        error = QStringLiteral("The anchored file is unexpectedly large");
        return false;
    }

    QByteArray result(file.state_->size, '\0');
    qsizetype offset = 0;
    QByteArray digest;
    const bool read = streamPinnedFile(
        *file.state_,
        [&result, &offset](
            const char *data, qsizetype size, QString &) {
            std::memcpy(result.data() + offset, data, size_t(size));
            offset += size;
            return true;
        },
        digest, error);
    if (!read) return false;
    contents = std::move(result);
    return true;
}

bool copyToNewFile(
    const PinnedFile &source,
    const EntryRef &destination,
    PinnedFile &copy,
    QString &error)
{
    copy = {};
    error.clear();
    if (!source.state_ || !destination.isValid()) {
        error = QStringLiteral(
            "An anchored copy endpoint is unavailable");
        return false;
    }

#ifdef Q_OS_UNIX
    const QByteArray name = QFile::encodeName(destination.component_);
    FileDescriptor output(::openat(
        destination.parent_.state_->descriptor.get(),
        name.constData(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR));
    if (!output.isValid()) {
        error = errno == EEXIST
            ? QStringLiteral("The anchored copy destination already exists")
            : nativeError(
                  QStringLiteral("Cannot create an anchored copy"), errno);
        return false;
    }
#elif defined(Q_OS_WIN)
    WindowsHandle output(::CreateFileW(
        reinterpret_cast<LPCWSTR>(destination.displayPath_.utf16()),
        GENERIC_READ | GENERIC_WRITE | DELETE
            | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!output.isValid()) {
        const DWORD native = ::GetLastError();
        error = (native == ERROR_ALREADY_EXISTS
                 || native == ERROR_FILE_EXISTS)
            ? QStringLiteral("The anchored copy destination already exists")
            : windowsError(
                  QStringLiteral("Cannot create an anchored copy"), native);
        return false;
    }
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif

    QByteArray copiedDigest;
    const bool copied = streamPinnedFile(
        *source.state_,
#ifdef Q_OS_UNIX
        [&output](const char *data, qsizetype size, QString &writeError) {
            qsizetype written = 0;
            while (written < size) {
                ssize_t result;
                do {
                    result = ::write(
                        output.get(), data + written,
                        size_t(size - written));
                } while (result < 0 && errno == EINTR);
                if (result <= 0) {
                    writeError = nativeError(
                        QStringLiteral("Cannot write an anchored copy"),
                        errno);
                    return false;
                }
                written += qsizetype(result);
            }
            return true;
        },
#elif defined(Q_OS_WIN)
        [&output](const char *data, qsizetype size, QString &writeError) {
            qsizetype written = 0;
            while (written < size) {
                DWORD nativeWritten = 0;
                const DWORD requested = DWORD(std::min<qsizetype>(
                    size - written,
                    qsizetype(std::numeric_limits<DWORD>::max())));
                if (!::WriteFile(
                        output.get(), data + written,
                        requested, &nativeWritten, nullptr)
                    || nativeWritten == 0) {
                    writeError = windowsError(
                        QStringLiteral("Cannot write an anchored copy"),
                        ::GetLastError());
                    return false;
                }
                written += qsizetype(nativeWritten);
            }
            return true;
        },
#endif
        copiedDigest, error);

    bool synchronized = false;
    if (copied) {
#ifdef Q_OS_UNIX
        int syncResult;
        do {
            syncResult = ::fsync(output.get());
        } while (syncResult != 0 && errno == EINTR);
        synchronized = syncResult == 0;
        if (!synchronized) {
            error = nativeError(
                QStringLiteral("Cannot synchronize an anchored copy"),
                errno);
        }
#elif defined(Q_OS_WIN)
        synchronized = ::FlushFileBuffers(output.get());
        if (!synchronized) {
            error = windowsError(
                QStringLiteral("Cannot synchronize an anchored copy"),
                ::GetLastError());
        }
#endif
    }

#ifdef Q_OS_UNIX
    UnixStamp copiedStamp;
    const bool inspected = captureUnixStamp(
        output.get(), copiedStamp, false, error);
    const NativeIdentity copiedIdentity = inspected
        ? unixIdentity(copiedStamp, 'f') : NativeIdentity();
#elif defined(Q_OS_WIN)
    WindowsStamp copiedStamp;
    const bool inspected = captureWindowsStamp(
        output.get(), copiedStamp, false, error);
    const NativeIdentity copiedIdentity = inspected
        ? windowsIdentity(copiedStamp, 'f') : NativeIdentity();
#endif

    PinnedFile candidate;
    if (inspected) {
        auto state = std::make_unique<Detail::PinnedFileState>();
        state->entry = destination;
#ifdef Q_OS_UNIX
        state->descriptor = std::move(output);
#elif defined(Q_OS_WIN)
        state->handle = std::move(output);
#endif
        state->stamp = copiedStamp;
        state->identity = copiedIdentity;
        state->size = copiedStamp.size;
        state->sha256 = copiedDigest;
        candidate.state_ = std::move(state);
    }

    const bool complete = copied && synchronized && inspected
        && copiedStamp.links == 1
        && copiedStamp.size == source.state_->size
        && copiedDigest == source.state_->sha256;
    bool verified = false;
    if (complete) {
        QByteArray verifiedDigest;
        QString verifyError;
        const PinnedChunkConsumer discard = [](
            const char *, qsizetype, QString &) { return true; };
        verified = streamPinnedFile(
            *candidate.state_, discard,
            verifiedDigest, verifyError);
        if (!verified) error = verifyError;
    }
    bool named = false;
    if (verified
        && !entryMatches(
            destination, candidate, named, error)) {
        verified = false;
    }
    if (verified && !named) {
        error = QStringLiteral(
            "The anchored copy destination was replaced");
        verified = false;
    }
    if (verified
        && !destination.parent_.sync(error)) {
        verified = false;
    }
    if (verified) {
        copy = std::move(candidate);
        return true;
    }

    if (error.isEmpty()) {
        error = QStringLiteral("Cannot complete an anchored copy");
    }
    if (candidate.isValid()) {
        appendCleanupError(error, remove(candidate));
    }
    return false;
}

bool pinRegularFile(
    const EntryRef &entry,
    PinnedFile &file,
    QString &error)
{
    error.clear();
    file = {};
    if (!entry.isValid()) {
        error = QStringLiteral("The anchored file reference is unavailable");
        return false;
    }

#ifdef Q_OS_UNIX
    const QByteArray name = QFile::encodeName(entry.component_);
    FileDescriptor descriptor(::openat(
        entry.parent_.state_->descriptor.get(),
        name.constData(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot open an anchored regular file"), errno);
        return false;
    }
    UnixStamp before;
    if (!captureUnixStamp(descriptor.get(), before, false, error))
        return false;
    if (before.links != 1) {
        error = QStringLiteral(
            "An anchored regular file must have exactly one link");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, '\0');
    qint64 offset = 0;
    while (offset < before.size) {
        const size_t requested = size_t(std::min<qint64>(
            chunk.size(), before.size - offset));
        ssize_t bytesRead;
        do {
            bytesRead = ::pread(
                descriptor.get(), chunk.data(), requested, off_t(offset));
        } while (bytesRead < 0 && errno == EINTR);
        if (bytesRead <= 0) {
            error = bytesRead == 0
                ? QStringLiteral("The anchored regular file ended early")
                : nativeError(
                      QStringLiteral("Cannot read an anchored regular file"),
                      errno);
            return false;
        }
        hash.addData(chunk.constData(), qsizetype(bytesRead));
        offset += qint64(bytesRead);
    }
    char trailing = 0;
    ssize_t trailingRead;
    do {
        trailingRead = ::pread(
            descriptor.get(), &trailing, 1, off_t(before.size));
    } while (trailingRead < 0 && errno == EINTR);
    if (trailingRead != 0) {
        error = trailingRead < 0
            ? nativeError(
                  QStringLiteral("Cannot finish reading an anchored file"),
                  errno)
            : QStringLiteral("The anchored regular file grew while reading");
        return false;
    }

    UnixStamp after;
    if (!captureUnixStamp(descriptor.get(), after, false, error))
        return false;
    if (!(after == before)) {
        error = QStringLiteral(
            "The anchored regular file changed while being pinned");
        return false;
    }

    auto state = std::make_unique<Detail::PinnedFileState>();
    state->entry = entry;
    state->descriptor = std::move(descriptor);
    state->stamp = before;
    state->identity = unixIdentity(before, 'f');
    state->size = before.size;
    state->sha256 = hash.result();
    file.state_ = std::move(state);
    return true;
#elif defined(Q_OS_WIN)
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(entry.displayPath_.utf16()),
        GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle.isValid()) {
        error = windowsError(
            QStringLiteral("Cannot open an anchored regular file"),
            ::GetLastError());
        return false;
    }
    WindowsStamp before;
    if (!captureWindowsStamp(handle.get(), before, false, error))
        return false;
    if (before.links != 1) {
        error = QStringLiteral(
            "An anchored regular file must have exactly one link");
        return false;
    }

    LARGE_INTEGER zero {};
    if (!::SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN)) {
        error = windowsError(
            QStringLiteral("Cannot seek an anchored regular file"),
            ::GetLastError());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, '\0');
    qint64 copied = 0;
    while (copied < before.size) {
        const DWORD requested = DWORD(std::min<qint64>(
            chunk.size(), before.size - copied));
        DWORD bytesRead = 0;
        if (!::ReadFile(
                handle.get(), chunk.data(), requested,
                &bytesRead, nullptr)
            || bytesRead == 0) {
            error = windowsError(
                QStringLiteral("Cannot read an anchored regular file"),
                ::GetLastError());
            return false;
        }
        hash.addData(chunk.constData(), qsizetype(bytesRead));
        copied += bytesRead;
    }
    char trailing = 0;
    DWORD trailingRead = 0;
    if (!::ReadFile(
            handle.get(), &trailing, 1, &trailingRead, nullptr)
        || trailingRead != 0) {
        error = trailingRead != 0
            ? QStringLiteral("The anchored regular file grew while reading")
            : windowsError(
                  QStringLiteral("Cannot finish reading an anchored file"),
                  ::GetLastError());
        return false;
    }
    WindowsStamp after;
    if (!captureWindowsStamp(handle.get(), after, false, error))
        return false;
    if (!(after == before)) {
        error = QStringLiteral(
            "The anchored regular file changed while being pinned");
        return false;
    }

    auto state = std::make_unique<Detail::PinnedFileState>();
    state->entry = entry;
    state->handle = std::move(handle);
    state->stamp = before;
    state->identity = windowsIdentity(before, 'f');
    state->size = before.size;
    state->sha256 = hash.result();
    file.state_ = std::move(state);
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool entryExists(
    const EntryRef &entry,
    bool &exists,
    QString &error)
{
    error.clear();
    exists = false;
    if (!entry.isValid()) {
        error = QStringLiteral(
            "The anchored file reference is unavailable");
        return false;
    }

#ifdef Q_OS_UNIX
    UnixStamp stamp;
    return statEntry(
        entry.parent_.state_->descriptor.get(),
        entry.component_, stamp, exists, error);
#elif defined(Q_OS_WIN)
    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(entry.displayPath_.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!named.isValid()) {
        const DWORD native = ::GetLastError();
        if (native == ERROR_FILE_NOT_FOUND
            || native == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = windowsError(
            QStringLiteral("Cannot inspect an anchored file name"), native);
        return false;
    }
    WindowsStamp stamp;
    if (!captureWindowsStamp(named.get(), stamp, false, error)) return false;
    exists = true;
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool entryMatches(
    const EntryRef &entry,
    const PinnedFile &file,
    bool &matches,
    QString &error)
{
    error.clear();
    matches = false;
    if (!entry.isValid() || !file.state_) {
        error = QStringLiteral("An anchored file identity is unavailable");
        return false;
    }

#ifdef Q_OS_UNIX
    UnixStamp pinnedNow;
    if (!captureUnixStamp(
            file.state_->descriptor.get(), pinnedNow, false, error)) {
        return false;
    }
    if (!(pinnedNow == file.state_->stamp)) return true;

    UnixStamp named;
    bool exists = false;
    if (!statEntry(
            entry.parent_.state_->descriptor.get(),
            entry.component_, named, exists, error)) {
        return false;
    }
    if (!exists) return true;
    matches = named == file.state_->stamp;
    return true;
#elif defined(Q_OS_WIN)
    WindowsStamp pinnedNow;
    if (!captureWindowsStamp(
            file.state_->handle.get(), pinnedNow, false, error)) {
        return false;
    }
    if (!sameWindowsIdentityAndData(
            pinnedNow, file.state_->stamp)) {
        return true;
    }

    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(entry.displayPath_.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!named.isValid()) {
        const DWORD native = ::GetLastError();
        if (native == ERROR_FILE_NOT_FOUND
            || native == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = windowsError(
            QStringLiteral("Cannot inspect an anchored file name"), native);
        return false;
    }
    WindowsStamp namedStamp;
    if (!captureWindowsStamp(named.get(), namedStamp, false, error))
        return false;
    matches = sameWindowsIdentityAndData(namedStamp, pinnedNow);
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

MutationResult moveNoReplace(
    PinnedFile &source,
    const EntryRef &destination)
{
    MutationResult result;
    if (!source.state_ || !destination.isValid()) {
        result.error = QStringLiteral(
            "An anchored move endpoint is unavailable");
        return result;
    }

    bool sourceMatches = false;
    if (!entryMatches(
            source.state_->entry, source,
            sourceMatches, result.error)) {
        return result;
    }
    if (!sourceMatches) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The anchored move source was replaced");
        return result;
    }

    const EntryRef original = source.state_->entry;
#ifdef Q_OS_UNIX
    const QByteArray sourceName = QFile::encodeName(original.component_);
    const QByteArray destinationName =
        QFile::encodeName(destination.component_);
    bool unsupported = false;
    if (renameNoReplaceNative(
            original.parent_.state_->descriptor.get(),
            sourceName,
            destination.parent_.state_->descriptor.get(),
            destinationName,
            unsupported) != 0) {
        const int moveError = errno;
        if (moveError == EEXIST) {
            result.effect = MutationEffect::Conflict;
            result.error = QStringLiteral(
                "The anchored move destination already exists");
            return result;
        }
        if (unsupported) {
            result.error = QStringLiteral(
                "The filesystem cannot move an anchored file without replacement");
        } else {
            result.error = nativeError(
                QStringLiteral("Cannot move an anchored file"), moveError);
        }
        return result;
    }

    reportAnchoredFilesystemTransition(
        "move-published",
        original.displayPath_,
        destination.displayPath_);

    UnixStamp movedHandle;
    if (!captureUnixStamp(
            source.state_->descriptor.get(),
            movedHandle, false, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    UnixStamp movedName;
    bool movedExists = false;
    QString inspectError;
    if (!statEntry(
            destination.parent_.state_->descriptor.get(),
            destination.component_, movedName, movedExists, inspectError)
        || !movedExists
        || !(movedName == movedHandle)
        || !sameUnixIdentityAndData(
            movedHandle, source.state_->stamp)) {
        result.effect = MutationEffect::Partial;
        result.error = inspectError.isEmpty()
            ? QStringLiteral(
                  "The anchored move destination changed after publication")
            : inspectError;
        return result;
    }

    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            *source.state_, discard,
            verifiedDigest, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }

    source.state_->entry = destination;
    source.state_->stamp = movedHandle;
    source.state_->identity = unixIdentity(movedHandle, 'f');
    const bool sourceSynced = original.parent_.sync(inspectError);
    QString destinationSyncError;
    const bool sameDirectory =
        original.parent_.identity() == destination.parent_.identity();
    const bool destinationSynced = sameDirectory
        || destination.parent_.sync(destinationSyncError);
    if (!sourceSynced || !destinationSynced) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = !inspectError.isEmpty()
            ? inspectError : destinationSyncError;
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    return result;
#elif defined(Q_OS_WIN)
    const QString name = QDir::toNativeSeparators(
        destination.displayPath_);
    const DWORD nameBytes = DWORD(name.size() * sizeof(wchar_t));
    // Keep room for the trailing NUL even though FileNameLength excludes it.
    const size_t bufferSize = sizeof(FILE_RENAME_INFO)
        + size_t(nameBytes);
    std::vector<unsigned char> storage(bufferSize, 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = nameBytes;
    std::memcpy(rename->FileName, name.utf16(), nameBytes);
    if (!::SetFileInformationByHandle(
            source.state_->handle.get(),
            FileRenameInfo,
            rename,
            DWORD(storage.size()))) {
        const DWORD native = ::GetLastError();
        const DWORD targetAttributes = ::GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(name.utf16()));
        result.effect = (native == ERROR_ALREADY_EXISTS
                         || native == ERROR_FILE_EXISTS
                         || (native == ERROR_ACCESS_DENIED
                             && targetAttributes
                                 != INVALID_FILE_ATTRIBUTES))
            ? MutationEffect::Conflict
            : MutationEffect::NoEffect;
        result.error = windowsError(
            result.effect == MutationEffect::Conflict
                ? QStringLiteral("The anchored move destination already exists")
                : QStringLiteral("Cannot move an anchored file"),
            native);
        return result;
    }
    reportAnchoredFilesystemTransition(
        "move-published",
        original.displayPath_,
        destination.displayPath_);
    WindowsStamp moved;
    if (!captureWindowsStamp(
            source.state_->handle.get(), moved, false, result.error)
        || !sameWindowsIdentityAndData(
            moved, source.state_->stamp)
        || windowsIdentity(moved, 'f')
            != source.state_->identity) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored file identity changed during its move");
        }
        return result;
    }
    source.state_->entry = destination;
    source.state_->stamp = moved;
    source.state_->identity = windowsIdentity(moved, 'f');
    result.effect = MutationEffect::AppliedDurable;
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

MutationResult remove(PinnedFile &file)
{
    MutationResult result;
    if (!file.state_) {
        result.error = QStringLiteral(
            "The anchored removal target is unavailable");
        return result;
    }

    bool matches = false;
    if (!entryMatches(
            file.state_->entry, file,
            matches, result.error)) {
        return result;
    }
    if (!matches) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The anchored removal target was replaced");
        return result;
    }

#ifdef Q_OS_UNIX
    const EntryRef original = file.state_->entry;
    QString componentError;
    const QString component = QStringLiteral(".gc-remove-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const EntryRef quarantine =
        original.parent_.entry(component, componentError);
    if (!quarantine.isValid()) {
        result.error = componentError;
        return result;
    }
    result = moveNoReplace(file, quarantine);
    if (!result.applied()) return result;

    bool quarantineMatches = false;
    QString matchError;
    if (!entryMatches(
            file.state_->entry, file,
            quarantineMatches, matchError)
        || !quarantineMatches) {
        result.effect = MutationEffect::Partial;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal quarantine was replaced")
            : matchError;
        return result;
    }

    reportAnchoredFilesystemTransition(
        "remove-quarantine-verified",
        file.state_->entry.displayPath_);
    quarantineMatches = false;
    matchError.clear();
    if (!entryMatches(
            file.state_->entry, file,
            quarantineMatches, matchError)
        || !quarantineMatches) {
        result.effect = MutationEffect::Partial;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal quarantine was replaced")
            : matchError;
        return result;
    }

    reportAnchoredFilesystemTransition(
        "remove-quarantine-finally-verified",
        file.state_->entry.displayPath_);

    const QByteArray quarantineName =
        QFile::encodeName(file.state_->entry.component_);
    int unlinkResult;
    do {
        unlinkResult = ::unlinkat(
            file.state_->entry.parent_.state_->descriptor.get(),
            quarantineName.constData(), 0);
    } while (unlinkResult != 0 && errno == EINTR);
    if (unlinkResult != 0) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral("Cannot remove an anchored quarantine file"),
            errno);
        return result;
    }
    const DirectoryAnchor parent = file.state_->entry.parent_;
    UnixStamp unlinked;
    QString verificationError;
    const bool captured = captureUnixStamp(
        file.state_->descriptor.get(), unlinked, false,
        verificationError);
    const bool removedPinnedFile = captured
        && unlinked.links == 0
        && sameUnixObjectAndData(unlinked, file.state_->stamp);
    QString syncError;
    const bool parentSynced = parent.sync(syncError);
    if (!removedPinnedFile) {
        result.effect = MutationEffect::Partial;
        result.error = verificationError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal did not unlink the pinned file's final name")
            : verificationError;
        return result;
    }
    file.state_.reset();
    if (!parentSynced) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = syncError;
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    result.error.clear();
    return result;
#elif defined(Q_OS_WIN)
    const EntryRef original = file.state_->entry;
    const WindowsStamp originalStamp = file.state_->stamp;
    struct WindowsDispositionInfoEx
    {
        DWORD flags = 0;
    };
    // Older SDKs omit FileDispositionInfoEx even when the runtime supports it.
    constexpr FILE_INFO_BY_HANDLE_CLASS dispositionInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
    constexpr DWORD dispositionDelete = 0x00000001;
    constexpr DWORD dispositionPosixSemantics = 0x00000002;
    WindowsDispositionInfoEx extendedDisposition {
        dispositionDelete | dispositionPosixSemantics};
    bool removalRequested = ::SetFileInformationByHandle(
        file.state_->handle.get(),
        dispositionInfoEx,
        &extendedDisposition,
        sizeof(extendedDisposition));
    if (!removalRequested) {
        const DWORD extendedError = ::GetLastError();
        const bool unsupported =
            extendedError == ERROR_INVALID_PARAMETER
            || extendedError == ERROR_INVALID_FUNCTION
            || extendedError == ERROR_NOT_SUPPORTED
            || extendedError == ERROR_CALL_NOT_IMPLEMENTED;
        if (!unsupported) {
            result.error = windowsError(
                QStringLiteral("Cannot remove an anchored file"),
                extendedError);
            return result;
        }
        FILE_DISPOSITION_INFO disposition {};
        disposition.DeleteFile = TRUE;
        removalRequested = ::SetFileInformationByHandle(
            file.state_->handle.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition));
        if (!removalRequested) {
            result.error = windowsError(
                QStringLiteral("Cannot remove an anchored file"),
                ::GetLastError());
            return result;
        }
    }
    file.state_.reset();

    QString pathError;
    if (!original.parent_.pathMatches(pathError)) {
        result.effect = MutationEffect::Partial;
        result.error = pathError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal parent changed before verification")
            : pathError;
        return result;
    }
    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(original.displayPath_.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!named.isValid()) {
        const DWORD native = ::GetLastError();
        if (native == ERROR_FILE_NOT_FOUND
            || native == ERROR_PATH_NOT_FOUND) {
            result.effect = MutationEffect::AppliedDurable;
            return result;
        }
        result.effect = MutationEffect::Partial;
        result.error = windowsError(
            QStringLiteral(
                "Cannot verify completion of an anchored removal"),
            native);
        return result;
    }
    WindowsStamp namedStamp;
    if (!captureWindowsStamp(
            named.get(), namedStamp, false, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    if (sameWindowsObject(namedStamp, originalStamp)) {
        result.effect = MutationEffect::Partial;
        result.error = QStringLiteral(
            "The anchored removal left the original file name visible");
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

} // namespace AnchoredFileSystem
