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
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

namespace AnchoredFileSystem {
namespace {

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

bool sameWindowsIdentityAndData(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return left.volume == right.volume
        && left.id == right.id
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
    QString &error)
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
        error = windowsError(
            QStringLiteral("Cannot open an anchored directory"),
            ::GetLastError());
    }
    return handle;
}

#endif

} // namespace

namespace Detail {

struct DirectoryState
{
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

bool DirectoryAnchor::sync(QString &error) const
{
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return false;
    }
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

#ifdef Q_OS_UNIX
    const EntryRef original = source.state_->entry;
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

    UnixStamp moved;
    bool movedExists = false;
    QString inspectError;
    if (!statEntry(
            destination.parent_.state_->descriptor.get(),
            destination.component_, moved, movedExists, inspectError)
        || !movedExists
        || unixIdentity(moved, 'f') != source.state_->identity) {
        bool restoreUnsupported = false;
        const int restored = renameNoReplaceNative(
            destination.parent_.state_->descriptor.get(),
            destinationName,
            original.parent_.state_->descriptor.get(),
            sourceName,
            restoreUnsupported);
        result.effect = restored == 0
            ? MutationEffect::Conflict
            : MutationEffect::Partial;
        result.error = QStringLiteral(
            "An unexpected file occupied the anchored move source");
        if (restored != 0) {
            result.error += QStringLiteral(
                "; the unexpected file was retained at the destination");
        }
        return result;
    }

    source.state_->entry = destination;
    source.state_->stamp = moved;
    source.state_->identity = unixIdentity(moved, 'f');
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
    WindowsStamp moved;
    if (!captureWindowsStamp(
            source.state_->handle.get(), moved, false, result.error)
        || windowsIdentity(moved, 'f') != source.state_->identity) {
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
    QString component;
    EntryRef quarantine;
    QString componentError;
    for (int attempt = 0; attempt < 16; ++attempt) {
        component = QStringLiteral(".gc-remove-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        quarantine = original.parent_.entry(component, componentError);
        if (!quarantine.isValid()) {
            result.error = componentError;
            return result;
        }
        MutationResult moved = moveNoReplace(file, quarantine);
        if (moved.effect == MutationEffect::Conflict
            && QFileInfo::exists(quarantine.displayPath())) {
            continue;
        }
        if (!moved.applied()) return moved;
        result = moved;
        break;
    }
    if (!result.applied()) {
        result.error = QStringLiteral(
            "Cannot allocate an anchored removal quarantine");
        return result;
    }

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
    file.state_.reset();
    QString syncError;
    if (!parent.sync(syncError)) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = syncError;
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    result.error.clear();
    return result;
#elif defined(Q_OS_WIN)
    FILE_DISPOSITION_INFO disposition {};
    disposition.DeleteFile = TRUE;
    if (!::SetFileInformationByHandle(
            file.state_->handle.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
        result.error = windowsError(
            QStringLiteral("Cannot remove an anchored file"),
            ::GetLastError());
        return result;
    }
    file.state_.reset();
    result.effect = MutationEffect::AppliedDurable;
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

} // namespace AnchoredFileSystem
