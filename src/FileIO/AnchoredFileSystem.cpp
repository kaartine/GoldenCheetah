/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AnchoredFileSystem.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <sys/acl.h>
#include <sys/event.h>
#include <sys/param.h>
#include <stdio.h>
#endif
#if defined(Q_OS_LINUX)
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#endif
#endif

#ifdef Q_OS_WIN
#include <array>
#include <cstddef>
#include <io.h>
#include <qt_windows.h>
#include <aclapi.h>
#endif

#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
void anchoredFilesystemTransitionReached(
    const char *transition,
    const QString &primary,
    const QString &secondary);
bool anchoredFilesystemSyncFailureRequested(const QString &path);
bool anchoredFilesystemFileUnlinkFailureRequested(const QString &path);
bool anchoredFilesystemUseLegacyWindowsDelete();
#ifdef GC_ANCHORED_FILESYSTEM_ZERO_ID_TEST_HOOK
bool anchoredFilesystemForceZeroWindowsFileId();
#endif
#endif

#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
void anchoredFilesystemOpenStateChanged(int delta);
bool anchoredFilesystemDurableGenerationUnavailableRequested();
#endif

namespace AnchoredFileSystem {
namespace {

using PinnedChunkConsumer = PinnedFileChunkConsumer;

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
    int release() { return std::exchange(descriptor_, -1); }

private:
    int descriptor_ = -1;
};

bool expandTrustedRootDirectoryAlias(
    int rootDescriptor,
    QString &path,
    QStringList &components,
    QString &error)
{
    if (components.isEmpty()) return true;

    const QByteArray encoded = QFile::encodeName(
        components.constFirst());
    struct stat before {};
    if (::fstatat(
            rootDescriptor, encoded.constData(), &before,
            AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISLNK(before.st_mode)) {
        return true;
    }
    if (before.st_uid != 0) {
        error = QStringLiteral(
            "The anchored root directory alias is not trusted");
        return false;
    }

    QByteArray target(4096, '\0');
    ssize_t length;
    do {
        length = ::readlinkat(
            rootDescriptor, encoded.constData(),
            target.data(), size_t(target.size()));
    } while (length == -1 && errno == EINTR);
    if (length < 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot resolve the anchored root directory alias"),
            errno);
        return false;
    }
    if (length >= target.size()) {
        error = QStringLiteral(
            "The anchored root directory alias target is too long");
        return false;
    }
    target.truncate(int(length));

    struct stat after {};
    if (::fstatat(
            rootDescriptor, encoded.constData(), &after,
            AT_SYMLINK_NOFOLLOW) != 0
        || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino
        || !S_ISLNK(after.st_mode)
        || after.st_uid != 0) {
        error = QStringLiteral(
            "The anchored root directory alias changed while resolving it");
        return false;
    }

    const QString decoded = QFile::decodeName(target);
    if (decoded.isEmpty()) {
        error = QStringLiteral(
            "The anchored root directory alias target is empty");
        return false;
    }
    QString expanded = QDir::isAbsolutePath(decoded)
        ? decoded
        : QDir::root().filePath(decoded);
    for (int index = 1; index < components.size(); ++index) {
        expanded = QDir(expanded).filePath(
            components.at(index));
    }
    path = QDir::cleanPath(expanded);
    if (!QDir::isAbsolutePath(path)) {
        error = QStringLiteral(
            "The anchored root directory alias target is not absolute");
        return false;
    }
    components = path.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    return true;
}

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
#if defined(Q_OS_MACOS) || defined(Q_OS_FREEBSD)
    qint64 birthSeconds = 0;
    qint64 birthNanoseconds = 0;
#endif
    QByteArray immutableGeneration;

    bool operator==(const UnixStamp &other) const
    {
        return device == other.device
            && inode == other.inode
            && links == other.links
            && size == other.size
            && modifiedSeconds == other.modifiedSeconds
            && modifiedNanoseconds == other.modifiedNanoseconds
            && changedSeconds == other.changedSeconds
            && changedNanoseconds == other.changedNanoseconds
            && immutableGeneration == other.immutableGeneration;
    }
};

bool sameUnixObject(
    const UnixStamp &left, const UnixStamp &right)
{
    return left.device == right.device
        && left.inode == right.inode
        && left.immutableGeneration == right.immutableGeneration;
}

QByteArray unixImmutableGeneration(
    int directory,
    const char *path,
    int flags,
    const struct stat &status)
{
    QByteArray generation;
#if defined(Q_OS_MACOS)
    Q_UNUSED(directory)
    Q_UNUSED(path)
    Q_UNUSED(flags)
    if (status.st_birthtimespec.tv_sec != 0
        || status.st_birthtimespec.tv_nsec != 0
        || status.st_gen != 0) {
        generation = QByteArrayLiteral("b:")
            + QByteArray::number(
                qint64(status.st_birthtimespec.tv_sec))
            + ':'
            + QByteArray::number(
                qint64(status.st_birthtimespec.tv_nsec))
            + QByteArrayLiteral(":g:")
            + QByteArray::number(quint64(status.st_gen));
    }
#elif defined(Q_OS_FREEBSD)
    Q_UNUSED(directory)
    Q_UNUSED(path)
    Q_UNUSED(flags)
    if (status.st_birthtim.tv_sec != 0
        || status.st_birthtim.tv_nsec != 0) {
        generation = QByteArrayLiteral("b:")
            + QByteArray::number(qint64(status.st_birthtim.tv_sec))
            + ':'
            + QByteArray::number(qint64(status.st_birthtim.tv_nsec));
    }
#elif defined(Q_OS_LINUX)
    Q_UNUSED(status)
#if defined(SYS_statx) && defined(STATX_BTIME)
    struct statx extended {};
    if (::syscall(
            SYS_statx, directory, path, flags,
            STATX_BTIME, &extended) == 0
        && (extended.stx_mask & STATX_BTIME)
        && (extended.stx_btime.tv_sec != 0
            || extended.stx_btime.tv_nsec != 0)) {
        generation = QByteArrayLiteral("b:")
            + QByteArray::number(qint64(extended.stx_btime.tv_sec))
            + ':'
            + QByteArray::number(
                qint64(extended.stx_btime.tv_nsec));
    }
#endif
#else
    Q_UNUSED(directory)
    Q_UNUSED(path)
    Q_UNUSED(flags)
    Q_UNUSED(status)
#endif
    return generation;
}

bool sameUnixObjectAndData(
    const UnixStamp &left, const UnixStamp &right)
{
    return sameUnixObject(left, right)
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
#if defined(Q_OS_MACOS) || defined(Q_OS_FREEBSD)
#if defined(Q_OS_MACOS)
    stamp.modifiedSeconds = qint64(status.st_mtimespec.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtimespec.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctimespec.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctimespec.tv_nsec);
    stamp.birthSeconds = qint64(status.st_birthtimespec.tv_sec);
    stamp.birthNanoseconds = qint64(status.st_birthtimespec.tv_nsec);
#elif defined(Q_OS_FREEBSD)
    stamp.modifiedSeconds = qint64(status.st_mtim.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtim.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctim.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctim.tv_nsec);
    stamp.birthSeconds = qint64(status.st_birthtim.tv_sec);
    stamp.birthNanoseconds = qint64(status.st_birthtim.tv_nsec);
#endif
#else
    stamp.modifiedSeconds = qint64(status.st_mtim.tv_sec);
    stamp.modifiedNanoseconds = qint64(status.st_mtim.tv_nsec);
    stamp.changedSeconds = qint64(status.st_ctim.tv_sec);
    stamp.changedNanoseconds = qint64(status.st_ctim.tv_nsec);
#endif
    constexpr int EmptyPath = 0x1000;
    stamp.immutableGeneration = unixImmutableGeneration(
        descriptor, "", EmptyPath, status);
    return true;
}

QByteArray unixDurableGeneration(int descriptor, const UnixStamp &stamp)
{
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
    if (anchoredFilesystemDurableGenerationUnavailableRequested()) {
        return {};
    }
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_FREEBSD)
    if (stamp.birthSeconds <= 0
        && stamp.birthNanoseconds <= 0) {
        return {};
    }
    return QByteArrayLiteral("birth:")
        + QByteArray::number(stamp.birthSeconds) + ':'
        + QByteArray::number(stamp.birthNanoseconds);
#elif defined(Q_OS_LINUX) && defined(SYS_statx) \
    && defined(STATX_BTIME) && defined(AT_EMPTY_PATH) \
    && defined(AT_STATX_SYNC_AS_STAT)
    Q_UNUSED(stamp)
    struct statx status {};
    const int result = int(::syscall(
        SYS_statx,
        descriptor,
        "",
        AT_EMPTY_PATH | AT_STATX_SYNC_AS_STAT,
        STATX_BTIME,
        &status));
    if (result == 0 && (status.stx_mask & STATX_BTIME)
        && (status.stx_btime.tv_sec > 0
            || status.stx_btime.tv_nsec > 0)) {
        return QByteArrayLiteral("birth:")
            + QByteArray::number(status.stx_btime.tv_sec) + ':'
            + QByteArray::number(status.stx_btime.tv_nsec);
    }
#else
    Q_UNUSED(descriptor)
    Q_UNUSED(stamp)
#endif
    return {};
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
    return NativeIdentity(
        std::move(key), stamp.links, stamp.immutableGeneration);
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
    stamp.immutableGeneration = unixImmutableGeneration(
        directory, name.constData(), AT_SYMLINK_NOFOLLOW, status);
    return true;
}

bool statDirectoryEntry(
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
            QStringLiteral("Cannot inspect an anchored directory name"),
            errno);
        return false;
    }
    exists = true;
    if (!S_ISDIR(status.st_mode) || status.st_size < 0) {
        error = QStringLiteral(
            "The anchored directory name has an unsafe type");
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
    stamp.immutableGeneration = unixImmutableGeneration(
        directory, name.constData(), AT_SYMLINK_NOFOLLOW, status);
    return true;
}

bool entryNameExists(
    int directory,
    const QString &component,
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
            AT_SYMLINK_NOFOLLOW) == 0) {
        exists = true;
        return true;
    }
    if (errno == ENOENT) return true;
    error = nativeError(
        QStringLiteral("Cannot inspect an anchored filesystem name"),
        errno);
    return false;
}

#if defined(Q_OS_LINUX)

constexpr qsizetype MaximumAccountLookupBuffer = 1024 * 1024;

qsizetype accountLookupBufferSize(int configurationName)
{
    const long configured = ::sysconf(configurationName);
    if (configured >= 1024
        && configured <= MaximumAccountLookupBuffer) {
        return qsizetype(configured);
    }
    return 16 * 1024;
}

bool growAccountLookupBuffer(QByteArray &storage, QString &error)
{
    if (storage.size() >= MaximumAccountLookupBuffer) {
        error = QStringLiteral(
            "An account database record exceeds the supported size");
        return false;
    }
    storage.resize(std::min<qsizetype>(
        MaximumAccountLookupBuffer, storage.size() * 2));
    return true;
}

bool lookupUserById(
    uid_t id,
    struct passwd &account,
    QByteArray &storage,
    QString &error)
{
    storage.resize(accountLookupBufferSize(_SC_GETPW_R_SIZE_MAX));
    for (;;) {
        struct passwd *result = nullptr;
        const int lookup = ::getpwuid_r(
            id, &account, storage.data(), size_t(storage.size()),
            &result);
        if (lookup == ERANGE) {
            if (!growAccountLookupBuffer(storage, error)) return false;
            continue;
        }
        if (lookup != 0) {
            error = nativeError(
                QStringLiteral("Cannot inspect the current user account"),
                lookup);
            return false;
        }
        if (!result) {
            error = QStringLiteral(
                "The current user account cannot be resolved");
            return false;
        }
        return true;
    }
}

bool lookupGroupById(
    gid_t id,
    struct group &groupEntry,
    QByteArray &storage,
    QString &error)
{
    storage.resize(accountLookupBufferSize(_SC_GETGR_R_SIZE_MAX));
    for (;;) {
        struct group *result = nullptr;
        const int lookup = ::getgrgid_r(
            id, &groupEntry, storage.data(), size_t(storage.size()),
            &result);
        if (lookup == ERANGE) {
            if (!growAccountLookupBuffer(storage, error)) return false;
            continue;
        }
        if (lookup != 0) {
            error = nativeError(
                QStringLiteral("Cannot inspect the anchored parent group"),
                lookup);
            return false;
        }
        if (!result) {
            error = QStringLiteral(
                "The anchored parent group cannot be resolved");
            return false;
        }
        return true;
    }
}

bool lookupUserByName(
    const QByteArray &name,
    struct passwd &account,
    QByteArray &storage,
    QString &error)
{
    storage.resize(accountLookupBufferSize(_SC_GETPW_R_SIZE_MAX));
    for (;;) {
        struct passwd *result = nullptr;
        const int lookup = ::getpwnam_r(
            name.constData(), &account,
            storage.data(), size_t(storage.size()), &result);
        if (lookup == ERANGE) {
            if (!growAccountLookupBuffer(storage, error)) return false;
            continue;
        }
        if (lookup != 0) {
            error = nativeError(
                QStringLiteral(
                    "Cannot inspect an anchored parent group member"),
                lookup);
            return false;
        }
        if (!result) {
            error = QStringLiteral(
                "An anchored parent group member cannot be resolved");
            return false;
        }
        return true;
    }
}

bool unixWriterUidIsExcluded(uid_t uid)
{
    return uid == 0 || uid == ::geteuid();
}

bool unixGroupWritersAreRestricted(gid_t groupId, QString &error)
{
    struct passwd currentAccount {};
    QByteArray accountStorage;
    if (!lookupUserById(
            ::geteuid(), currentAccount, accountStorage, error)) {
        return false;
    }

    struct group groupEntry {};
    QByteArray groupStorage;
    if (!lookupGroupById(
            groupId, groupEntry, groupStorage, error)) {
        return false;
    }

    const QByteArray accountName(
        currentAccount.pw_name ? currentAccount.pw_name : "");
    const QByteArray groupName(
        groupEntry.gr_name ? groupEntry.gr_name : "");
    if (groupId != currentAccount.pw_gid
        || accountName.isEmpty()
        || groupName != accountName) {
        error = QStringLiteral(
            "The anchored parent group is not the current user's private group");
        return false;
    }

    std::vector<QByteArray> explicitMembers;
    for (char **member = groupEntry.gr_mem;
         member && *member; ++member) {
        explicitMembers.emplace_back(*member);
    }
    for (const QByteArray &member : explicitMembers) {
        struct passwd account {};
        QByteArray memberStorage;
        if (!lookupUserByName(
                member, account, memberStorage, error)) {
            return false;
        }
        if (!unixWriterUidIsExcluded(account.pw_uid)) {
            error = QStringLiteral(
                "The anchored parent group is writable by account %1")
                    .arg(QString::fromLocal8Bit(member));
            return false;
        }
    }
    return true;
}

#endif

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

int exchangeNamesNative(
    int firstDirectory,
    const QByteArray &first,
    int secondDirectory,
    const QByteArray &second,
    bool &unsupported)
{
    unsupported = false;
#if defined(Q_OS_LINUX) && defined(SYS_renameat2) \
    && defined(RENAME_EXCHANGE)
    const int result = int(::syscall(
        SYS_renameat2,
        firstDirectory,
        first.constData(),
        secondDirectory,
        second.constData(),
        RENAME_EXCHANGE));
    if (result == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL
#ifdef EOPNOTSUPP
        && errno != EOPNOTSUPP
#endif
    ) {
        return -1;
    }
    unsupported = true;
#elif defined(Q_OS_MACOS) && defined(RENAME_SWAP)
    const int result = ::renameatx_np(
        firstDirectory,
        first.constData(),
        secondDirectory,
        second.constData(),
        RENAME_SWAP);
    if (result == 0) return 0;
    if (errno == ENOTSUP || errno == EINVAL) unsupported = true;
#else
    Q_UNUSED(firstDirectory)
    Q_UNUSED(first)
    Q_UNUSED(secondDirectory)
    Q_UNUSED(second)
    unsupported = true;
#endif
    return -1;
}

#if defined(Q_OS_MACOS)
bool macDirectoryHasExtendedAcl(
    int descriptor, bool &hasAcl, QString &error)
{
    hasAcl = false;
    errno = 0;
    acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    if (!acl) {
        if (errno == ENOENT) return true;
        error = nativeError(
            QStringLiteral(
                "Cannot inspect anchored directory access controls"),
            errno);
        return false;
    }
    ::acl_free(acl);
    hasAcl = true;
    return true;
}

bool removeMacDirectoryExtendedAcl(
    int descriptor, QString &error)
{
    bool hasAcl = false;
    if (!macDirectoryHasExtendedAcl(
            descriptor, hasAcl, error)) {
        return false;
    }
    if (hasAcl) {
        filesec_t security = ::filesec_init();
        if (!security) {
            error = nativeError(
                QStringLiteral(
                    "Cannot allocate directory access controls"),
                errno);
            return false;
        }
        if (::filesec_set_property(
                security,
                FILESEC_ACL,
                _FILESEC_REMOVE_ACL) != 0) {
            const int setError = errno;
            ::filesec_free(security);
            error = nativeError(
                QStringLiteral(
                    "Cannot prepare inherited access-control removal"),
                setError);
            return false;
        }
        const int setResult = ::fchmodx_np(descriptor, security);
        const int setError = errno;
        ::filesec_free(security);
        if (setResult != 0) {
            error = nativeError(
                QStringLiteral(
                    "Cannot remove inherited directory access controls"),
                setError);
            return false;
        }
    }
    if (!macDirectoryHasExtendedAcl(
            descriptor, hasAcl, error)) {
        return false;
    }
    if (hasAcl) {
        error = QStringLiteral(
            "The anchored child directory retained extended access controls");
        return false;
    }
    return true;
}
#endif

#if defined(Q_OS_LINUX)
bool linuxDirectoryAccessAclIsAbsent(
    int descriptor, QString &error)
{
    errno = 0;
    const ssize_t size = ::fgetxattr(
        descriptor, "system.posix_acl_access", nullptr, 0);
    if (size >= 0) {
        error = QStringLiteral(
            "The anchored parent has an extended POSIX access control list");
        return false;
    }
    if (errno == ENODATA || errno == ENOTSUP
#ifdef EOPNOTSUPP
        || errno == EOPNOTSUPP
#endif
    ) {
        return true;
    }
    error = nativeError(
        QStringLiteral(
            "Cannot inspect anchored directory access controls"),
        errno);
    return false;
}

bool linuxDirectoryDefaultAclIsAbsent(
    int descriptor, QString &error)
{
    errno = 0;
    const ssize_t size = ::fgetxattr(
        descriptor, "system.posix_acl_default", nullptr, 0);
    if (size >= 0) {
        error = QStringLiteral(
            "The anchored parent has an inheritable POSIX access control list");
        return false;
    }
    if (errno == ENODATA || errno == ENOTSUP
#ifdef EOPNOTSUPP
        || errno == EOPNOTSUPP
#endif
    ) {
        return true;
    }
    error = nativeError(
        QStringLiteral(
            "Cannot inspect anchored directory access controls"),
        errno);
    return false;
}

bool linuxDirectoryAclsAreAbsent(int descriptor, QString &error)
{
    static const char *const attributes[] = {
        "system.posix_acl_access",
        "system.posix_acl_default"};
    for (const char *attribute : attributes) {
        errno = 0;
        const ssize_t size = ::fgetxattr(
            descriptor, attribute, nullptr, 0);
        if (size >= 0) {
            error = QStringLiteral(
                "The anchored directory retained a POSIX access control list");
            return false;
        }
        if (errno != ENODATA && errno != ENOTSUP
#ifdef EOPNOTSUPP
            && errno != EOPNOTSUPP
#endif
        ) {
            error = nativeError(
                QStringLiteral(
                    "Cannot inspect anchored directory access controls"),
                errno);
            return false;
        }
    }
    return true;
}

bool removeLinuxDirectoryAcls(int descriptor, QString &error)
{
    static const char *const attributes[] = {
        "system.posix_acl_access",
        "system.posix_acl_default"};
    for (const char *attribute : attributes) {
        if (::fremovexattr(descriptor, attribute) != 0
            && errno != ENODATA && errno != ENOTSUP
#ifdef EOPNOTSUPP
            && errno != EOPNOTSUPP
#endif
        ) {
            error = nativeError(
                QStringLiteral(
                    "Cannot remove inherited directory access controls"),
                errno);
            return false;
        }
    }
    return linuxDirectoryAclsAreAbsent(descriptor, error);
}
#endif

#endif

#ifdef Q_OS_WIN

QString extendedWindowsPath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(
        QDir::cleanPath(path));
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    }
    if (native.size() >= 3
        && native.at(1) == QLatin1Char(':')
        && native.at(2) == QLatin1Char('\\')) {
        return QStringLiteral("\\\\?\\") + native;
    }
    return native;
}

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

bool currentWindowsUserSid(QByteArray &storage, PSID &sid)
{
    HANDLE rawToken = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    WindowsHandle token(rawToken);
    DWORD required = 0;
    ::GetTokenInformation(
        token.get(), TokenUser, nullptr, 0, &required);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return false;
    }
    storage.resize(int(required));
    if (!::GetTokenInformation(
            token.get(), TokenUser, storage.data(),
            required, &required)) {
        return false;
    }
    auto *user = reinterpret_cast<TOKEN_USER *>(storage.data());
    if (!::IsValidSid(user->User.Sid)) return false;
    sid = user->User.Sid;
    return true;
}

class WindowsPrivateDirectorySecurity final
{
public:
    WindowsPrivateDirectorySecurity()
    {
        PSID userSid = nullptr;
        if (!currentWindowsUserSid(userStorage_, userSid)) return;
        const DWORD aclSize = DWORD(
            sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE)
            - sizeof(DWORD) + ::GetLengthSid(userSid));
        aclStorage_.resize(int(aclSize));
        auto *acl = reinterpret_cast<PACL>(aclStorage_.data());
        if (!::InitializeAcl(acl, aclSize, ACL_REVISION)
            || !::AddAccessAllowedAceEx(
                acl, ACL_REVISION,
                CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
                FILE_ALL_ACCESS, userSid)
            || !::InitializeSecurityDescriptor(
                &descriptor_, SECURITY_DESCRIPTOR_REVISION)
            || !::SetSecurityDescriptorOwner(
                &descriptor_, userSid, FALSE)
            || !::SetSecurityDescriptorDacl(
                &descriptor_, TRUE, acl, FALSE)
            || !::SetSecurityDescriptorControl(
                &descriptor_, SE_DACL_PROTECTED,
                SE_DACL_PROTECTED)) {
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    bool isValid() const { return valid_; }
    PSID ownerSid()
    {
        if (!valid_) return nullptr;
        auto *user = reinterpret_cast<TOKEN_USER *>(
            userStorage_.data());
        return user->User.Sid;
    }
    PACL acl()
    {
        return valid_
            ? reinterpret_cast<PACL>(aclStorage_.data())
            : nullptr;
    }
    SECURITY_ATTRIBUTES *attributes()
    {
        return valid_ ? &attributes_ : nullptr;
    }

private:
    QByteArray userStorage_;
    QByteArray aclStorage_;
    SECURITY_DESCRIPTOR descriptor_ {};
    SECURITY_ATTRIBUTES attributes_ {};
    bool valid_ = false;
};

bool windowsHandleSupportsPersistentAcls(HANDLE handle)
{
    DWORD fileSystemFlags = 0;
    return handle
        && handle != INVALID_HANDLE_VALUE
        && ::GetVolumeInformationByHandleW(
            handle, nullptr, 0, nullptr, nullptr,
            &fileSystemFlags, nullptr, 0)
        && (fileSystemFlags & FILE_PERSISTENT_ACLS);
}

QString windowsError(const QString &operation, DWORD nativeError);

bool inspectWindowsDirectoryPrivateSecurity(
    HANDLE handle,
    bool &privateSecurity,
    QString &error)
{
    error.clear();
    privateSecurity = false;
    DWORD fileSystemFlags = 0;
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        error = QStringLiteral(
            "The anchored Windows directory handle is unavailable");
        return false;
    }
    if (!::GetVolumeInformationByHandleW(
            handle, nullptr, 0, nullptr, nullptr,
            &fileSystemFlags, nullptr, 0)) {
        error = windowsError(
            QStringLiteral(
                "Cannot inspect anchored Windows access-control support"),
            ::GetLastError());
        return false;
    }
    if (!(fileSystemFlags & FILE_PERSISTENT_ACLS)) return true;

    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(userStorage, userSid)) {
        error = QStringLiteral(
            "Cannot resolve the current Windows directory owner");
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (securityResult != ERROR_SUCCESS) {
        if (descriptor) ::LocalFree(descriptor);
        error = windowsError(
            QStringLiteral(
                "Cannot inspect anchored Windows directory security"),
            securityResult);
        return false;
    }

    if (!descriptor || !owner || !::IsValidSid(owner)
        || (acl && !::IsValidAcl(acl))) {
        if (descriptor) ::LocalFree(descriptor);
        error = QStringLiteral(
            "Windows returned an invalid directory security descriptor");
        return false;
    }
    if (!::EqualSid(owner, userSid)
        || !acl || acl->AceCount != 1) {
        if (descriptor) ::LocalFree(descriptor);
        return true;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!::GetSecurityDescriptorControl(
            descriptor, &control, &revision)) {
        const DWORD nativeError = ::GetLastError();
        ::LocalFree(descriptor);
        error = windowsError(
            QStringLiteral(
                "Cannot inspect Windows security descriptor controls"),
            nativeError);
        return false;
    }
    if (!(control & SE_DACL_PROTECTED)) {
        ::LocalFree(descriptor);
        return true;
    }

    void *rawAce = nullptr;
    if (!::GetAce(acl, 0, &rawAce)) {
        const DWORD nativeError = ::GetLastError();
        ::LocalFree(descriptor);
        error = windowsError(
            QStringLiteral("Cannot inspect a Windows directory access rule"),
            nativeError);
        return false;
    }
    auto *header = static_cast<ACE_HEADER *>(rawAce);
    auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    constexpr BYTE inheritanceFlags =
        OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE
        | INHERITED_ACE | INHERIT_ONLY_ACE
        | NO_PROPAGATE_INHERIT_ACE;
    privateSecurity = header->AceType == ACCESS_ALLOWED_ACE_TYPE
        && (header->AceFlags & inheritanceFlags)
            == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
        && ::IsValidSid(&ace->SidStart)
        && ::EqualSid(&ace->SidStart, userSid)
        && (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
    ::LocalFree(descriptor);
    return true;
}

bool windowsDirectoryHandleHasPrivateSecurity(HANDLE handle)
{
    bool privateSecurity = false;
    QString ignored;
    return inspectWindowsDirectoryPrivateSecurity(
               handle, privateSecurity, ignored)
        && privateSecurity;
}

struct WindowsStamp
{
    quint64 volume = 0;
    std::array<unsigned char, 16> id {};
    quint64 links = 0;
    qint64 size = -1;
    qint64 modified = 0;
    qint64 changed = 0;
    qint64 created = 0;

    bool operator==(const WindowsStamp &other) const
    {
        return volume == other.volume
            && id == other.id
            && links == other.links
            && size == other.size
            && modified == other.modified
            && changed == other.changed
            && created == other.created;
    }
};

bool sameWindowsFileId(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return left.volume == right.volume
        && left.id == right.id;
}

bool sameWindowsObject(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return sameWindowsFileId(left, right)
        && left.created == right.created;
}

bool sameWindowsIdentityAndExtent(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return sameWindowsObject(left, right)
        && left.links == right.links
        && left.size == right.size;
}

bool sameWindowsIdentityAndData(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return sameWindowsObject(left, right)
        && left.links == right.links
        && left.size == right.size
        && left.modified == right.modified
        && left.changed == right.changed;
}

bool sameWindowsMoveIdentityAndData(
    const WindowsStamp &left, const WindowsStamp &right)
{
    return sameWindowsObject(left, right)
        && left.links == right.links
        && left.size == right.size
        && left.modified == right.modified;
}

bool sameWindowsRenameResultAndData(
    const WindowsStamp &left, const WindowsStamp &right)
{
    // Name tunneling can replace creation time after a successful rename.
    return sameWindowsFileId(left, right)
        && left.links == right.links
        && left.size == right.size
        && left.modified == right.modified;
}

QString windowsError(const QString &operation, DWORD nativeError)
{
    return QStringLiteral("%1: system error %2")
        .arg(operation).arg(nativeError);
}

bool createWindowsWellKnownSid(
    WELL_KNOWN_SID_TYPE type,
    QByteArray &storage,
    PSID &sid)
{
    DWORD size = SECURITY_MAX_SID_SIZE;
    storage.resize(int(size));
    sid = storage.data();
    return ::CreateWellKnownSid(
        type, nullptr, sid, &size) && ::IsValidSid(sid);
}

bool windowsAceTypeAllowsAccess(BYTE type)
{
    return type == ACCESS_ALLOWED_ACE_TYPE
        || type == ACCESS_ALLOWED_OBJECT_ACE_TYPE
        || type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
        || type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE
        || type == ACCESS_ALLOWED_COMPOUND_ACE_TYPE;
}

bool windowsAllowedAceMaskAndSid(
    const ACE_HEADER *header,
    ACCESS_MASK &mask,
    PSID &sid)
{
    mask = 0;
    sid = nullptr;
    if (!header
        || header->AceSize
            < sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(
        header);
    std::memcpy(
        &mask, bytes + sizeof(ACE_HEADER), sizeof(mask));

    size_t sidOffset = 0;
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE
        || header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
        sidOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
    } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE
               || header->AceType
                   == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
        if (header->AceSize
            < offsetof(ACCESS_ALLOWED_OBJECT_ACE, ObjectType)) {
            return false;
        }
        const auto *object = reinterpret_cast<
            const ACCESS_ALLOWED_OBJECT_ACE *>(header);
        sidOffset = offsetof(
            ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
        if (object->Flags & ACE_OBJECT_TYPE_PRESENT)
            sidOffset += sizeof(GUID);
        if (object->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT)
            sidOffset += sizeof(GUID);
    } else {
        return false;
    }

    constexpr size_t minimumSidSize = 8;
    if (sidOffset > header->AceSize
        || header->AceSize - sidOffset < minimumSidSize) {
        return false;
    }
    auto *candidate = const_cast<unsigned char *>(bytes + sidOffset);
    const auto *layout = reinterpret_cast<const SID *>(candidate);
    const DWORD required = ::GetSidLengthRequired(
        layout->SubAuthorityCount);
    if (required > header->AceSize - sidOffset
        || !::IsValidSid(candidate)) {
        return false;
    }
    sid = candidate;
    return true;
}

bool windowsDirectoryHandleIsCurrentUserControlled(
    HANDLE handle, QString &error)
{
    if (!windowsHandleSupportsPersistentAcls(handle)) {
        error = QStringLiteral(
            "The anchored parent filesystem does not support persistent access controls");
        return false;
    }

    QByteArray userStorage;
    QByteArray systemStorage;
    QByteArray administratorsStorage;
    PSID userSid = nullptr;
    PSID systemSid = nullptr;
    PSID administratorsSid = nullptr;
    if (!currentWindowsUserSid(userStorage, userSid)
        || !createWindowsWellKnownSid(
            WinLocalSystemSid, systemStorage, systemSid)
        || !createWindowsWellKnownSid(
            WinBuiltinAdministratorsSid,
            administratorsStorage, administratorsSid)) {
        error = windowsError(
            QStringLiteral(
                "Cannot resolve trusted Windows directory identities"),
            ::GetLastError());
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (securityResult != ERROR_SUCCESS) {
        if (descriptor) ::LocalFree(descriptor);
        error = windowsError(
            QStringLiteral(
                "Cannot inspect anchored parent access controls"),
            securityResult);
        return false;
    }

    bool controlled = owner
        && ::IsValidSid(owner)
        && ::EqualSid(owner, userSid);
    if (!controlled) {
        error = QStringLiteral(
            "The anchored parent directory is not owned by the current user");
    } else if (!acl) {
        controlled = false;
        error = QStringLiteral(
            "The anchored parent directory has an unrestricted access list");
    } else if (!::IsValidAcl(acl)) {
        controlled = false;
        error = QStringLiteral(
            "The anchored parent directory has an invalid access list");
    }

    constexpr ACCESS_MASK dangerousAccess =
        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY
        | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER
        | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0;
         controlled && index < acl->AceCount; ++index) {
        void *rawAce = nullptr;
        if (!::GetAce(acl, index, &rawAce) || !rawAce) {
            controlled = false;
            error = QStringLiteral(
                "Cannot inspect an anchored parent access entry");
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        if (!windowsAceTypeAllowsAccess(header->AceType)
            || (header->AceFlags & INHERIT_ONLY_ACE)) {
            continue;
        }

        ACCESS_MASK mask = 0;
        PSID sid = nullptr;
        if (!windowsAllowedAceMaskAndSid(
                header, mask, sid)) {
            controlled = false;
            error = QStringLiteral(
                "The anchored parent has an unsupported access entry");
            break;
        }
        if (!(mask & dangerousAccess)) continue;
        if (!::EqualSid(sid, userSid)
            && !::EqualSid(sid, systemSid)
            && !::EqualSid(sid, administratorsSid)) {
            controlled = false;
            error = QStringLiteral(
                "The anchored parent grants directory mutation to another identity");
        }
    }

    if (descriptor) ::LocalFree(descriptor);
    return controlled;
}

bool windowsDirectoryHandleOwnerMatches(
    HANDLE handle, PSID expectedOwner, bool &matches, QString &error)
{
    matches = false;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    if (securityResult != ERROR_SUCCESS) {
        if (descriptor) ::LocalFree(descriptor);
        error = windowsError(
            QStringLiteral(
                "Cannot inspect an anchored Windows directory owner"),
            securityResult);
        return false;
    }
    matches = owner && expectedOwner
        && ::IsValidSid(owner)
        && ::IsValidSid(expectedOwner)
        && ::EqualSid(owner, expectedOwner);
    if (descriptor) ::LocalFree(descriptor);
    return true;
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
#ifdef GC_ANCHORED_FILESYSTEM_ZERO_ID_TEST_HOOK
    if (anchoredFilesystemForceZeroWindowsFileId()) {
        stamp.id.fill(0);
    }
#endif
    if (std::all_of(
            stamp.id.begin(), stamp.id.end(),
            [](unsigned char value) { return value == 0; })) {
        error = QStringLiteral(
            "The anchored filesystem does not provide a stable file identity");
        return false;
    }
    stamp.links = legacy.nNumberOfLinks;
    stamp.size = standard.EndOfFile.QuadPart;
    stamp.modified = basic.LastWriteTime.QuadPart;
    stamp.changed = basic.ChangeTime.QuadPart;
    stamp.created = basic.CreationTime.QuadPart;
    return true;
}

QByteArray windowsDurableGeneration(const WindowsStamp &stamp)
{
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
    if (anchoredFilesystemDurableGenerationUnavailableRequested()) {
        return {};
    }
#endif
    return stamp.created > 0
        ? QByteArrayLiteral("birth:")
            + QByteArray::number(stamp.created)
        : QByteArray();
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
    QByteArray immutableGeneration;
    if (stamp.created > 0) {
        immutableGeneration.reserve(int(sizeof(stamp.created)));
        immutableGeneration.append(
            reinterpret_cast<const char *>(&stamp.created),
            int(sizeof(stamp.created)));
    }
    return NativeIdentity(
        std::move(key), stamp.links, std::move(immutableGeneration));
}

bool hashWindowsRegularFile(
    HANDLE handle,
    qint64 maximumSize,
    WindowsStamp &stamp,
    QByteArray &digest,
    QString &error,
    const PinnedFileReadControl &readControl = {})
{
    WindowsStamp before;
    if (!captureWindowsStamp(handle, before, false, error)) return false;
    if (before.links != 1) {
        error = QStringLiteral(
            "An anchored regular file must have exactly one link");
        return false;
    }
    if (maximumSize >= 0 && before.size > maximumSize) {
        error = QStringLiteral(
            "The anchored regular file is unexpectedly large");
        return false;
    }

    LARGE_INTEGER zero {};
    if (!::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) {
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
        if (readControl
            && !readControl(qint64(requested), error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored file read was cancelled");
            }
            return false;
        }
        DWORD bytesRead = 0;
        if (!::ReadFile(
                handle, chunk.data(), requested,
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
    if ((readControl && !readControl(1, error))
        || !::ReadFile(
            handle, &trailing, 1, &trailingRead, nullptr)
        || trailingRead != 0) {
        if (error.isEmpty()) {
            error = trailingRead != 0
                ? QStringLiteral(
                      "The anchored regular file grew while reading")
                : windowsError(
                      QStringLiteral(
                          "Cannot finish reading an anchored file"),
                      ::GetLastError());
        }
        return false;
    }

    WindowsStamp after;
    if (!captureWindowsStamp(handle, after, false, error)) return false;
    if (!(after == before)) {
        error = QStringLiteral(
            "The anchored regular file changed while being pinned");
        return false;
    }
    stamp = before;
    digest = hash.result();
    return true;
}

WindowsHandle openWindowsDirectoryHandle(
    const QString &path,
    bool allowChildMutation,
    QString &error,
    DWORD *nativeError = nullptr)
{
    const QString extendedPath = extendedWindowsPath(path);
    DWORD access = FILE_LIST_DIRECTORY | FILE_TRAVERSE
        | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (allowChildMutation) {
        access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    }
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedPath.utf16()),
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

WindowsHandle openWindowsDirectoryBridge(
    const QString &path,
    QString &error,
    DWORD *nativeError = nullptr)
{
    const QString extendedPath = extendedWindowsPath(path);
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedPath.utf16()),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE
            | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
            QStringLiteral("Cannot bridge an anchored directory handle"),
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
    DirectoryState()
    {
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
        anchoredFilesystemOpenStateChanged(1);
#endif
    }
    ~DirectoryState()
    {
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
        anchoredFilesystemOpenStateChanged(-1);
#endif
    }
    std::shared_ptr<DirectoryState> ancestor;
#ifdef Q_OS_UNIX
    FileDescriptor descriptor;
    UnixStamp stamp;
#elif defined(Q_OS_WIN)
    std::vector<WindowsHandle> handles;
    WindowsStamp stamp;
    mutable std::mutex enumerationMutex;
#endif
    QString component;
    QString displayPath;
    NativeIdentity identity;
};

struct FileGenerationGuardState
{
#if defined(Q_OS_LINUX)
    FileDescriptor notifications;
    int watch = -1;
#elif defined(Q_OS_MACOS)
    FileDescriptor notifications;
    FileDescriptor watchedFile;
    std::vector<FileDescriptor> watchedDirectories;
#elif defined(Q_OS_WIN)
    WindowsHandle watchedFile;
    NativeIdentity identity;
#endif
    bool compromised = false;
};

struct PinnedFileState
{
    PinnedFileState()
    {
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
        anchoredFilesystemOpenStateChanged(1);
#endif
    }
    ~PinnedFileState()
    {
#ifdef GC_ANCHORED_FILESYSTEM_DUR007_TEST_HOOKS
        anchoredFilesystemOpenStateChanged(-1);
#endif
    }
    EntryRef entry;
#ifdef Q_OS_UNIX
    FileDescriptor descriptor;
    UnixStamp stamp;
#elif defined(Q_OS_WIN)
    WindowsHandle handle;
    WindowsStamp stamp;
#endif
    NativeIdentity identity;
    QByteArray durableGeneration;
    qint64 size = -1;
    QByteArray sha256;
};

struct PrivateDirectoryOperations
{
    static MutationResult create(
        const DirectoryAnchor &parent,
        const QString &component,
        DirectoryAnchor &directory);
    static MutationResult createFixed(
        const DirectoryAnchor &parent,
        const QString &component,
        DirectoryAnchor &directory);
};

} // namespace Detail

namespace {

struct DirectoryEntryObservation
{
    QString name;
    DirectoryEntryKind kind = DirectoryEntryKind::RegularFile;
    NativeIdentity identity;
#ifdef Q_OS_UNIX
    UnixStamp stamp;
#elif defined(Q_OS_WIN)
    WindowsStamp stamp;
#endif
};

void sortDirectoryEntryObservations(
    QList<DirectoryEntryObservation> &entries)
{
    std::sort(
        entries.begin(), entries.end(),
        [](const DirectoryEntryObservation &left,
           const DirectoryEntryObservation &right) {
            return QString::compare(
                left.name, right.name, Qt::CaseSensitive) < 0;
        });
}

bool directoryEntryObservationsMatch(
    const QList<DirectoryEntryObservation> &left,
    const QList<DirectoryEntryObservation> &right)
{
    if (left.size() != right.size()) return false;
    for (int index = 0; index < left.size(); ++index) {
        const DirectoryEntryObservation &leftEntry = left.at(index);
        const DirectoryEntryObservation &rightEntry = right.at(index);
        if (leftEntry.name != rightEntry.name
            || leftEntry.kind != rightEntry.kind
            || leftEntry.identity != rightEntry.identity
            || !(leftEntry.stamp == rightEntry.stamp)) {
            return false;
        }
    }
    return true;
}

bool validateDirectoryEntryObservations(
    QList<DirectoryEntryObservation> &entries,
    QString &error)
{
    sortDirectoryEntryObservations(entries);
    for (int index = 1; index < entries.size(); ++index) {
        if (entries.at(index - 1).name == entries.at(index).name) {
            error = QStringLiteral(
                "The anchored directory contains duplicate names");
            return false;
        }
    }
    return true;
}

#ifdef Q_OS_UNIX

bool captureUnixDirectoryEntryObservation(
    int directory,
    const QByteArray &encodedName,
    const QString &name,
    DirectoryEntryObservation &observation,
    QString &error)
{
    struct stat status {};
    int statusResult = -1;
    do {
        statusResult = ::fstatat(
            directory, encodedName.constData(), &status,
            AT_SYMLINK_NOFOLLOW);
    } while (statusResult != 0 && errno == EINTR);
    if (statusResult != 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot inspect an anchored directory entry"),
            errno);
        return false;
    }

    char identityType = 0;
    if (S_ISREG(status.st_mode)) {
        observation.kind = DirectoryEntryKind::RegularFile;
        identityType = 'f';
    } else if (S_ISDIR(status.st_mode)) {
        observation.kind = DirectoryEntryKind::Directory;
        identityType = 'd';
    } else {
        error = QStringLiteral(
            "The anchored directory contains an unsafe entry type: %1")
                .arg(name);
        return false;
    }
    if (status.st_size < 0) {
        error = QStringLiteral(
            "The anchored directory entry has an invalid size");
        return false;
    }

    UnixStamp stamp;
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
    stamp.immutableGeneration = unixImmutableGeneration(
        directory, encodedName.constData(),
        AT_SYMLINK_NOFOLLOW, status);
    observation.name = name;
    observation.stamp = stamp;
    observation.identity = unixIdentity(stamp, identityType);
    return true;
}

bool enumerateUnixDirectoryPass(
    const Detail::DirectoryState &state,
    QList<DirectoryEntryObservation> &entries,
    qsizetype maximumEntries,
    QString &error)
{
    entries.clear();
    int duplicateDescriptor = -1;
    do {
        duplicateDescriptor = ::openat(
            state.descriptor.get(), ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (duplicateDescriptor < 0 && errno == EINTR);
    if (duplicateDescriptor < 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot open an anchored directory enumeration stream"),
            errno);
        return false;
    }

    DIR *rawStream = ::fdopendir(duplicateDescriptor);
    if (!rawStream) {
        const int failure = errno;
        ::close(duplicateDescriptor);
        error = nativeError(
            QStringLiteral(
                "Cannot initialize an anchored directory enumeration stream"),
            failure);
        return false;
    }
    std::unique_ptr<DIR, int (*)(DIR *)> stream(
        rawStream, &::closedir);

    for (;;) {
        errno = 0;
        const struct dirent *entry = ::readdir(stream.get());
        if (!entry) {
            if (errno == EINTR) continue;
            if (errno != 0) {
                error = nativeError(
                    QStringLiteral(
                        "Cannot enumerate an anchored directory"),
                    errno);
                entries.clear();
                return false;
            }
            break;
        }

        const QByteArray encodedName(entry->d_name);
        if (encodedName == QByteArrayLiteral(".")
            || encodedName == QByteArrayLiteral("..")) {
            continue;
        }
        const QString name = QFile::decodeName(encodedName);
        if (QFile::encodeName(name) != encodedName
            || !validPortableComponent(name)) {
            error = QStringLiteral(
                "The anchored directory contains an unsafe name");
            entries.clear();
            return false;
        }

        DirectoryEntryObservation observation;
        if (!captureUnixDirectoryEntryObservation(
                ::dirfd(stream.get()), encodedName, name,
                observation, error)) {
            entries.clear();
            return false;
        }
        if (entries.size() >= maximumEntries) {
            error = QStringLiteral(
                "The anchored directory exceeds its entry budget");
            entries.clear();
            return false;
        }
        entries.append(std::move(observation));
    }
    if (!validateDirectoryEntryObservations(entries, error)) {
        entries.clear();
        return false;
    }
    return true;
}

#elif defined(Q_OS_WIN)

bool enumerateWindowsDirectoryPass(
    const Detail::DirectoryState &state,
    QList<DirectoryEntryObservation> &entries,
    qsizetype maximumEntries,
    QString &error)
{
    entries.clear();
    constexpr DWORD bufferSize = 64 * 1024;
    QByteArray buffer(int(bufferSize), Qt::Uninitialized);
    bool restart = true;
    for (;;) {
        const FILE_INFO_BY_HANDLE_CLASS informationClass = restart
            ? FileIdExtdDirectoryRestartInfo
            : FileIdExtdDirectoryInfo;
        if (!::GetFileInformationByHandleEx(
                state.handles.back().get(), informationClass,
                buffer.data(), bufferSize)) {
            const DWORD failure = ::GetLastError();
            if (failure == ERROR_NO_MORE_FILES) break;
            error = windowsError(
                QStringLiteral(
                    "Cannot enumerate an anchored Windows directory"),
                failure);
            entries.clear();
            return false;
        }
        restart = false;

        size_t offset = 0;
        for (;;) {
            constexpr size_t fixedSize =
                offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
            if (offset > size_t(buffer.size())
                || size_t(buffer.size()) - offset < fixedSize) {
                error = QStringLiteral(
                    "Windows returned an invalid anchored directory entry");
                entries.clear();
                return false;
            }
            const auto *nativeEntry = reinterpret_cast<
                const FILE_ID_EXTD_DIR_INFO *>(
                    buffer.constData() + offset);
            const size_t nameBytes = nativeEntry->FileNameLength;
            if ((nameBytes % sizeof(wchar_t)) != 0
                || nameBytes > size_t(buffer.size())
                    - offset - fixedSize) {
                error = QStringLiteral(
                    "Windows returned an invalid anchored directory name");
                entries.clear();
                return false;
            }
            const QString name = QString::fromWCharArray(
                nativeEntry->FileName,
                int(nameBytes / sizeof(wchar_t)));
            if (name != QStringLiteral(".")
                && name != QStringLiteral("..")) {
                if (!validPortableComponent(name)
                    || (nativeEntry->FileAttributes
                        & (FILE_ATTRIBUTE_REPARSE_POINT
                           | FILE_ATTRIBUTE_DEVICE))) {
                    error = QStringLiteral(
                        "The anchored directory contains an unsafe entry: %1")
                            .arg(name);
                    entries.clear();
                    return false;
                }

                const bool isDirectory =
                    nativeEntry->FileAttributes
                    & FILE_ATTRIBUTE_DIRECTORY;
                const QString path = QDir(state.displayPath).filePath(name);
                WindowsHandle handle(::CreateFileW(
                    reinterpret_cast<LPCWSTR>(
                        extendedWindowsPath(path).utf16()),
                    FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT
                        | (isDirectory
                            ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                    nullptr));
                if (!handle.isValid()) {
                    error = windowsError(
                        QStringLiteral(
                            "Cannot open an anchored Windows directory entry"),
                        ::GetLastError());
                    entries.clear();
                    return false;
                }

                WindowsStamp stamp;
                if (!captureWindowsStamp(
                        handle.get(), stamp, isDirectory, error)
                    || !std::equal(
                        std::begin(nativeEntry->FileId.Identifier),
                        std::end(nativeEntry->FileId.Identifier),
                        stamp.id.begin())) {
                    if (error.isEmpty()) {
                        error = QStringLiteral(
                            "An anchored Windows directory entry changed while being enumerated");
                    }
                    entries.clear();
                    return false;
                }

                DirectoryEntryObservation observation;
                observation.name = name;
                observation.kind = isDirectory
                    ? DirectoryEntryKind::Directory
                    : DirectoryEntryKind::RegularFile;
                observation.stamp = stamp;
                observation.identity = windowsIdentity(
                    stamp, isDirectory ? 'd' : 'f');
                if (entries.size() >= maximumEntries) {
                    error = QStringLiteral(
                        "The anchored directory exceeds its entry budget");
                    entries.clear();
                    return false;
                }
                entries.append(std::move(observation));
            }

            const ULONG next = nativeEntry->NextEntryOffset;
            if (next == 0) break;
            if (next < fixedSize
                || size_t(next) > size_t(buffer.size()) - offset) {
                error = QStringLiteral(
                    "Windows returned an invalid anchored directory sequence");
                entries.clear();
                return false;
            }
            offset += next;
        }
    }
    if (!validateDirectoryEntryObservations(entries, error)) {
        entries.clear();
        return false;
    }
    return true;
}

#endif

#ifdef Q_OS_WIN
WindowsHandle openWindowsMutationHandle(
    const Detail::PinnedFileState &state,
    bool &conflict,
    QString &error)
{
    conflict = false;
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(state.entry.displayPath()).utf16()),
        DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle.isValid()) {
        const DWORD native = ::GetLastError();
        conflict = native == ERROR_FILE_NOT_FOUND
            || native == ERROR_PATH_NOT_FOUND;
        error = windowsError(
            QStringLiteral("Cannot open an anchored file for mutation"),
            native);
        return WindowsHandle();
    }

    WindowsStamp stamp;
    if (!captureWindowsStamp(handle.get(), stamp, false, error))
        return WindowsHandle();
    if (!sameWindowsIdentityAndData(stamp, state.stamp)
        || windowsIdentity(stamp, 'f') != state.identity) {
        conflict = true;
        error = QStringLiteral(
            "The anchored mutation target was replaced");
        return WindowsHandle();
    }
    return handle;
}

bool windowsMovedEntryMatches(
    const EntryRef &entry,
    const Detail::PinnedFileState &state,
    bool &matches,
    WindowsStamp &matchedStamp,
    QString &error)
{
    error.clear();
    matches = false;
    matchedStamp = {};
    WindowsStamp pinnedNow;
    if (!captureWindowsStamp(
            state.handle.get(), pinnedNow, false, error)) {
        return false;
    }
    if (!sameWindowsMoveIdentityAndData(pinnedNow, state.stamp)
        || windowsIdentity(pinnedNow, 'f') != state.identity) {
        return true;
    }

    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath()).utf16()),
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
    if (!captureWindowsStamp(
            named.get(), namedStamp, false, error)) {
        return false;
    }
    matches = sameWindowsMoveIdentityAndData(namedStamp, pinnedNow)
        && windowsIdentity(namedStamp, 'f') == state.identity;
    if (matches) matchedStamp = pinnedNow;
    return true;
}
#endif

bool streamPinnedFile(
    const Detail::PinnedFileState &state,
    const PinnedFileChunkConsumer &consume,
    QByteArray &digest,
    QString &error,
    const PinnedFileReadControl &readControl = {})
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
    if (!sameWindowsMoveIdentityAndData(before, state.stamp)) {
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
        if (readControl
            && !readControl(qint64(requested), error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored file read was cancelled");
            }
            readSucceeded = false;
            break;
        }
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
        hash.addData(QByteArrayView(chunk.constData(), received));
        offset += received;
    }

    if (readSucceeded) {
        char trailing = 0;
        if (readControl && !readControl(1, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored file read was cancelled");
            }
            readSucceeded = false;
        }
        if (readSucceeded) {
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
                          QStringLiteral(
                              "Cannot finish reading an anchored file"),
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
                          QStringLiteral(
                              "Cannot finish reading an anchored file"),
                          ::GetLastError());
                readSucceeded = false;
            }
#endif
        }
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
    const bool stable = sameWindowsMoveIdentityAndData(before, after);
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

#ifdef Q_OS_WIN
bool makeWindowsOutputPinImmutable(
    Detail::PinnedFileState &state,
    bool &cleanupIsIdentityBound,
    QString &error)
{
    cleanupIsIdentityBound = true;
    WindowsHandle bridge(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(state.entry.displayPath()).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    WindowsStamp bridgeStamp;
    if (!bridge.isValid()
        || !captureWindowsStamp(
            bridge.get(), bridgeStamp, false, error)
        || !sameWindowsIdentityAndExtent(bridgeStamp, state.stamp)
        || windowsIdentity(bridgeStamp, 'f') != state.identity) {
        if (error.isEmpty()) {
            error = bridge.isValid()
                ? QStringLiteral(
                    "The anchored output changed before its pin was finalized")
                : windowsError(
                    QStringLiteral(
                        "Cannot bridge an anchored output pin"),
                    ::GetLastError());
        }
        return false;
    }

    state.handle.reset();
    reportAnchoredFilesystemTransition(
        "output-pin-writer-released",
        state.entry.displayPath());
    WindowsHandle immutable(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(state.entry.displayPath()).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!immutable.isValid()) {
        const DWORD native = ::GetLastError();
        cleanupIsIdentityBound = false;
        error = windowsError(
            QStringLiteral(
                "Cannot finalize an anchored output pin"),
            native);
        return false;
    }

    WindowsStamp immutableStamp;
    if (!captureWindowsStamp(
            immutable.get(), immutableStamp, false, error)
        || !sameWindowsIdentityAndExtent(
            immutableStamp, state.stamp)
        || windowsIdentity(immutableStamp, 'f') != state.identity) {
        cleanupIsIdentityBound = false;
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored output was replaced while its pin was finalized");
        }
        return false;
    }

    state.stamp = immutableStamp;
    state.identity = windowsIdentity(immutableStamp, 'f');
    state.handle = std::move(immutable);
    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            state, discard, verifiedDigest, error)
        || verifiedDigest != state.sha256) {
        cleanupIsIdentityBound = false;
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The finalized anchored output contents changed");
        }
        return false;
    }
    return true;
}
#endif

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

QByteArray NativeIdentity::persistentFingerprint() const
{
    if (!isValid() || immutableGeneration_.isEmpty()) return {};
    QByteArray evidence = QByteArray::number(key_.size());
    evidence += ':';
    evidence += key_;
    evidence += immutableGeneration_;
    return QCryptographicHash::hash(
        evidence, QCryptographicHash::Sha256);
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
    QString cleaned = QDir::cleanPath(
        QFileInfo(absolutePath).absoluteFilePath());

#ifdef Q_OS_UNIX
    FileDescriptor current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot open the filesystem root"), errno);
        return false;
    }
    QStringList components = cleaned.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!expandTrustedRootDirectoryAlias(
            current.get(), cleaned, components, error)) {
        return false;
    }
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

QString DirectoryAnchor::displayPath() const
{
    return state_ ? state_->displayPath : QString();
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
    child->component = component;
    directory = DirectoryAnchor(std::move(child));
    exists = true;
    return true;
}

bool DirectoryAnchor::enumerateEntries(
    QList<DirectoryEntry> &entries,
    qsizetype maximumEntries,
    QString &error) const
{
    entries.clear();
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored directory is unavailable");
        return false;
    }
    if (maximumEntries < 0) {
        error = QStringLiteral(
            "The anchored directory entry budget is invalid");
        return false;
    }

    QList<DirectoryEntryObservation> first;
    QList<DirectoryEntryObservation> second;
#ifdef Q_OS_UNIX
    UnixStamp before;
    UnixStamp after;
    if (!captureUnixStamp(
            state_->descriptor.get(), before, true, error)
        || !enumerateUnixDirectoryPass(
            *state_, first, maximumEntries, error)) {
        return false;
    }
    reportAnchoredFilesystemTransition(
        "directory-enumeration-first-pass", state_->displayPath);
    if (!enumerateUnixDirectoryPass(
            *state_, second, maximumEntries, error)
        || !captureUnixStamp(
            state_->descriptor.get(), after, true, error)) {
        return false;
    }
    if (!(before == after)
        || !directoryEntryObservationsMatch(first, second)) {
        error = QStringLiteral(
            "The anchored directory changed while being enumerated");
        return false;
    }
#elif defined(Q_OS_WIN)
    const std::lock_guard<std::mutex> lock(
        state_->enumerationMutex);
    WindowsStamp before;
    WindowsStamp after;
    if (!captureWindowsStamp(
            state_->handles.back().get(), before, true, error)
        || !enumerateWindowsDirectoryPass(
            *state_, first, maximumEntries, error)) {
        return false;
    }
    reportAnchoredFilesystemTransition(
        "directory-enumeration-first-pass", state_->displayPath);
    if (!enumerateWindowsDirectoryPass(
            *state_, second, maximumEntries, error)
        || !captureWindowsStamp(
            state_->handles.back().get(), after, true, error)) {
        return false;
    }
    if (!(before == after)
        || !directoryEntryObservationsMatch(first, second)) {
        error = QStringLiteral(
            "The anchored directory changed while being enumerated");
        return false;
    }
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif

    entries.reserve(first.size());
    for (const DirectoryEntryObservation &observation : first) {
        entries.append({
            observation.name,
            observation.kind,
            observation.identity});
    }
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

bool EntryRef::stableAccessPath(QString &path, QString &error) const
{
    path.clear();
    error.clear();
    if (!isValid()) {
        error = QStringLiteral("The anchored file is unavailable");
        return false;
    }
#if defined(Q_OS_LINUX)
    path = QStringLiteral("/proc/self/fd/%1/%2")
        .arg(parent_.state_->descriptor.get())
        .arg(component_);
    return true;
#elif defined(Q_OS_MACOS)
    char directoryPath[MAXPATHLEN] = {};
    if (::fcntl(
            parent_.state_->descriptor.get(), F_GETPATH,
            directoryPath) != 0) {
        error = nativeError(
            QStringLiteral("Cannot resolve an anchored directory"),
            errno);
        return false;
    }
    path = QDir(QFile::decodeName(directoryPath)).filePath(component_);
    return true;
#elif defined(Q_OS_WIN)
    if (!parent_.pathMatches(error)) return false;
    path = displayPath_;
    return true;
#else
    if (!parent_.pathMatches(error)) return false;
    path = displayPath_;
    return true;
#endif
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

FileGenerationGuard::FileGenerationGuard() = default;
FileGenerationGuard::~FileGenerationGuard() = default;
FileGenerationGuard::FileGenerationGuard(
    FileGenerationGuard &&other) noexcept = default;
FileGenerationGuard &FileGenerationGuard::operator=(
    FileGenerationGuard &&other) noexcept = default;

bool FileGenerationGuard::unchanged(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The file-generation guard is unavailable");
        return false;
    }
    if (state_->compromised) {
        error = QStringLiteral("The guarded file generation changed");
        return false;
    }

#if defined(Q_OS_LINUX)
    alignas(struct inotify_event) char events[4096];
    for (;;) {
        const ssize_t received = ::read(
            state_->notifications.get(), events, sizeof(events));
        if (received > 0) {
            for (ssize_t offset = 0; offset < received;) {
                const auto *event = reinterpret_cast<const inotify_event *>(
                    events + offset);
                if (event->wd == state_->watch
                    || (event->mask & IN_Q_OVERFLOW)) {
                    state_->compromised = true;
                }
                offset += sizeof(inotify_event) + event->len;
            }
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (received == 0) break;
        error = nativeError(
            QStringLiteral("Cannot inspect the file-generation guard"),
            errno);
        return false;
    }
#elif defined(Q_OS_MACOS)
    struct kevent event {};
    const struct timespec timeout {0, 0};
    const int received = ::kevent(
        state_->notifications.get(), nullptr, 0,
        &event, 1, &timeout);
    if (received < 0) {
        error = nativeError(
            QStringLiteral("Cannot inspect the file-generation guard"),
            errno);
        return false;
    }
    if (received > 0) state_->compromised = true;
#elif defined(Q_OS_WIN)
    WindowsStamp current;
    if (!captureWindowsStamp(
            state_->watchedFile.get(), current, false, error)) {
        return false;
    }
    if (windowsIdentity(current, 'f') != state_->identity)
        state_->compromised = true;
#else
    error = QStringLiteral(
        "File-generation guards are unsupported on this platform");
    return false;
#endif

    if (state_->compromised) {
        error = QStringLiteral("The guarded file generation changed");
        return false;
    }
    return true;
}

NativeIdentity PinnedFile::identity() const
{
    return state_ ? state_->identity : NativeIdentity();
}

QByteArray PinnedFile::durableGeneration() const
{
    return state_ ? state_->durableGeneration : QByteArray();
}

qint64 PinnedFile::size() const
{
    return state_ ? state_->size : -1;
}

QByteArray PinnedFile::sha256() const
{
    return state_ ? state_->sha256 : QByteArray();
}

QString PinnedFile::verifiedPath(
    const EntryRef &entry,
    const PinnedFileReadControl &readControl) const
{
    if (!state_ || !entry.isValid()) return {};
    QString error;
    const auto entryStillNamesPinnedFile = [&]() {
        error.clear();
        if (!entry.parent_.pathMatches(error)) return false;
        reportAnchoredFilesystemTransition(
            "recovery-path-parent-verified", entry.displayPath_);
#ifdef Q_OS_UNIX
        UnixStamp pinned;
        if (!captureUnixStamp(
                state_->descriptor.get(), pinned, false, error)
            || !(pinned == state_->stamp)) {
            return false;
        }
        UnixStamp named;
        bool exists = false;
        if (!statEntry(
                entry.parent_.state_->descriptor.get(),
                entry.component_, named, exists, error)
            || !exists
            || !(named == pinned)) {
            return false;
        }
#elif defined(Q_OS_WIN)
        WindowsStamp pinned;
        if (!captureWindowsStamp(
                state_->handle.get(), pinned, false, error)
            || !sameWindowsMoveIdentityAndData(
                pinned, state_->stamp)) {
            return false;
        }
        WindowsHandle named(::CreateFileW(
            reinterpret_cast<LPCWSTR>(
                extendedWindowsPath(entry.displayPath_).utf16()),
            FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        WindowsStamp namedStamp;
        if (!named.isValid()
            || !captureWindowsStamp(
                named.get(), namedStamp, false, error)
            || !sameWindowsMoveIdentityAndData(namedStamp, pinned)) {
            return false;
        }
#else
        return false;
#endif
        return entry.parent_.pathMatches(error);
    };
    if (!entryStillNamesPinnedFile()) return {};

    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            *state_, discard, verifiedDigest, error, readControl)) {
        return {};
    }
    reportAnchoredFilesystemTransition(
        "recovery-path-digest-verified", entry.displayPath_);
    if (!entryStillNamesPinnedFile()) return {};
    QByteArray finalDigest;
    if (!streamPinnedFile(
            *state_, discard, finalDigest, error, readControl)) {
        return {};
    }
    if (!entryStillNamesPinnedFile()) return {};
    return entry.displayPath_;
}

QString verifiedRecoveryPath(
    const PinnedFile &file,
    const EntryRef &entry,
    const PinnedFileReadControl &readControl)
{
    return file.verifiedPath(entry, readControl);
}

bool readAll(
    const PinnedFile &file,
    qint64 maximumSize,
    QByteArray &contents,
    QString &error,
    const PinnedFileReadControl &readControl)
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
        digest, error, readControl);
    if (!read) return false;
    contents = std::move(result);
    return true;
}

bool streamContents(
    const PinnedFile &file,
    const PinnedFileChunkConsumer &consume,
    QString &error)
{
    error.clear();
    if (!file.state_) {
        error = QStringLiteral("The anchored file cannot be read");
        return false;
    }
    QByteArray digest;
    return streamPinnedFile(
        *file.state_, consume, digest, error);
}

bool writeNewFile(
    const QByteArray &contents,
    const EntryRef &destination,
    PinnedFile &file,
    QString &error)
{
    file = {};
    error.clear();
    if (!destination.isValid()) {
        error = QStringLiteral(
            "An anchored write destination is unavailable");
        return false;
    }
    if (!destination.parent_.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored write parent was replaced");
        }
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
            ? QStringLiteral("The anchored write destination already exists")
            : nativeError(
                  QStringLiteral("Cannot create an anchored file"), errno);
        return false;
    }
#elif defined(Q_OS_WIN)
    WindowsHandle output(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(destination.displayPath_).utf16()),
        GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!output.isValid()) {
        const DWORD native = ::GetLastError();
        error = (native == ERROR_ALREADY_EXISTS
                 || native == ERROR_FILE_EXISTS)
            ? QStringLiteral("The anchored write destination already exists")
            : windowsError(
                  QStringLiteral("Cannot create an anchored file"), native);
        return false;
    }
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif

    qsizetype written = 0;
    bool writeComplete = true;
    while (written < contents.size()) {
#ifdef Q_OS_UNIX
        ssize_t result;
        do {
            result = ::write(
                output.get(),
                contents.constData() + written,
                size_t(contents.size() - written));
        } while (result < 0 && errno == EINTR);
        if (result <= 0) {
            error = nativeError(
                QStringLiteral("Cannot write an anchored file"), errno);
            writeComplete = false;
            break;
        }
        written += qsizetype(result);
#elif defined(Q_OS_WIN)
        DWORD nativeWritten = 0;
        const DWORD requested = DWORD(std::min<qsizetype>(
            contents.size() - written,
            qsizetype(std::numeric_limits<DWORD>::max())));
        if (!::WriteFile(
                output.get(),
                contents.constData() + written,
                requested,
                &nativeWritten,
                nullptr)
            || nativeWritten == 0) {
            error = windowsError(
                QStringLiteral("Cannot write an anchored file"),
                ::GetLastError());
            writeComplete = false;
            break;
        }
        written += qsizetype(nativeWritten);
#endif
    }

    bool synchronized = false;
    if (writeComplete) {
#ifdef Q_OS_UNIX
        int syncResult;
        do {
            syncResult = ::fsync(output.get());
        } while (syncResult != 0 && errno == EINTR);
        synchronized = syncResult == 0;
        if (!synchronized) {
            error = nativeError(
                QStringLiteral("Cannot synchronize an anchored file"),
                errno);
        }
#elif defined(Q_OS_WIN)
        synchronized = ::FlushFileBuffers(output.get());
        if (!synchronized) {
            error = windowsError(
                QStringLiteral("Cannot synchronize an anchored file"),
                ::GetLastError());
        }
#endif
    }

#ifdef Q_OS_UNIX
    UnixStamp writtenStamp;
    const bool inspected = captureUnixStamp(
        output.get(), writtenStamp, false, error);
    const NativeIdentity writtenIdentity = inspected
        ? unixIdentity(writtenStamp, 'f') : NativeIdentity();
#elif defined(Q_OS_WIN)
    WindowsStamp writtenStamp;
    const bool inspected = captureWindowsStamp(
        output.get(), writtenStamp, false, error);
    const NativeIdentity writtenIdentity = inspected
        ? windowsIdentity(writtenStamp, 'f') : NativeIdentity();
#endif
    const QByteArray expectedDigest = QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256);

    PinnedFile candidate;
    if (inspected) {
        auto state = std::make_unique<Detail::PinnedFileState>();
        state->entry = destination;
#ifdef Q_OS_UNIX
        state->descriptor = std::move(output);
#elif defined(Q_OS_WIN)
        state->handle = std::move(output);
#endif
        state->stamp = writtenStamp;
        state->identity = writtenIdentity;
#ifdef Q_OS_UNIX
        state->durableGeneration = unixDurableGeneration(
            state->descriptor.get(), writtenStamp);
#elif defined(Q_OS_WIN)
        state->durableGeneration = windowsDurableGeneration(writtenStamp);
#endif
        state->size = writtenStamp.size;
        state->sha256 = expectedDigest;
        candidate.state_ = std::move(state);
    }

    const bool complete = writeComplete && synchronized && inspected
        && writtenStamp.links == 1
        && writtenStamp.size == contents.size();
    bool verified = false;
    bool cleanupIsIdentityBound = true;
    if (complete) {
        QByteArray verifiedDigest;
        QString verifyError;
        const PinnedChunkConsumer discard = [](
            const char *, qsizetype, QString &) { return true; };
        verified = streamPinnedFile(
            *candidate.state_, discard, verifiedDigest, verifyError)
            && verifiedDigest == expectedDigest;
        if (!verified) {
            error = verifyError.isEmpty()
                ? QStringLiteral(
                      "The anchored file contents changed while being written")
                : verifyError;
        }
    }
    bool named = false;
    if (verified
        && !entryMatches(destination, candidate, named, error)) {
        verified = false;
    }
    if (verified && !named) {
        error = QStringLiteral(
            "The anchored write destination was replaced");
        verified = false;
    }
    if (verified) {
        const bool synchronized = destination.parent_.sync(error);
        QString parentError;
        const bool parentMatches =
            destination.parent_.pathMatches(parentError);
        if (!synchronized || !parentMatches) {
            if (!parentError.isEmpty()) {
                if (!error.isEmpty()) error += QStringLiteral("; ");
                error += parentError;
            }
            verified = false;
        }
    }
#ifdef Q_OS_WIN
    if (verified
        && !makeWindowsOutputPinImmutable(
            *candidate.state_, cleanupIsIdentityBound, error)) {
        verified = false;
    }
    if (verified) {
        bool finalizedNameMatches = false;
        QString finalError;
        if (!entryMatches(
                destination, candidate,
                finalizedNameMatches, finalError)
            || !finalizedNameMatches
            || !destination.parent_.pathMatches(finalError)) {
            error = finalError.isEmpty()
                ? QStringLiteral(
                    "The anchored write destination changed while its pin was finalized")
                : finalError;
            verified = false;
        }
    }
#endif
    if (verified) {
        file = std::move(candidate);
        return true;
    }

    if (error.isEmpty()) {
        error = QStringLiteral("Cannot complete an anchored file write");
    }
    if (candidate.isValid()) {
        if (cleanupIsIdentityBound) {
            appendCleanupError(error, remove(candidate));
        } else {
            error += QStringLiteral("; unverified output retained at %1")
                .arg(destination.displayPath());
        }
    }
    return false;
}

bool tryLockExclusive(
    PinnedFile &file,
    int timeoutMilliseconds,
    QString &error)
{
    error.clear();
    if (!file.state_ || timeoutMilliseconds <= 0) {
        error = QStringLiteral(
            "The anchored lock inputs are invalid");
        return false;
    }
    if (!file.state_->entry.parent_.pathMatches(error))
        return false;

    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
#ifdef Q_OS_UNIX
        int result;
        do {
            result = ::flock(
                file.state_->descriptor.get(), LOCK_EX | LOCK_NB);
        } while (result != 0 && errno == EINTR);
        if (result == 0) break;
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            error = nativeError(
                QStringLiteral("Cannot lock an anchored file"), errno);
            return false;
        }
#elif defined(Q_OS_WIN)
        OVERLAPPED overlap{};
        if (::LockFileEx(
                file.state_->handle.get(),
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0,
                MAXDWORD,
                MAXDWORD,
                &overlap)) {
            break;
        }
        const DWORD native = ::GetLastError();
        if (native != ERROR_LOCK_VIOLATION
            && native != ERROR_IO_PENDING) {
            error = windowsError(
                QStringLiteral("Cannot lock an anchored file"), native);
            return false;
        }
#else
        error = QStringLiteral(
            "Anchored filesystem operations are unsupported on this platform");
        return false;
#endif
        const qint64 remaining =
            qint64(timeoutMilliseconds) - elapsed.elapsed();
        if (remaining <= 0) {
            error = QStringLiteral("The anchored file is already locked");
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(
            std::min<qint64>(10, remaining)));
    }

    bool named = false;
    if (!entryMatches(file.state_->entry, file, named, error)
        || !named
        || !file.state_->entry.parent_.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored lock file or its parent was replaced");
        }
        file = {};
        return false;
    }
    return true;
}

bool copyToNewFile(
    const PinnedFile &source,
    const EntryRef &destination,
    PinnedFile &copy,
    QString &error,
    bool retainDestinationOnFailure)
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
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(destination.displayPath_).utf16()),
        GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
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
#ifdef Q_OS_UNIX
        state->durableGeneration = unixDurableGeneration(
            state->descriptor.get(), copiedStamp);
#elif defined(Q_OS_WIN)
        state->durableGeneration = windowsDurableGeneration(copiedStamp);
#endif
        state->size = copiedStamp.size;
        state->sha256 = copiedDigest;
        candidate.state_ = std::move(state);
    }

    const bool complete = copied && synchronized && inspected
        && copiedStamp.links == 1
        && copiedStamp.size == source.state_->size
        && copiedDigest == source.state_->sha256;
    bool verified = false;
    bool cleanupIsIdentityBound = true;
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
#ifdef Q_OS_WIN
    if (verified
        && !makeWindowsOutputPinImmutable(
            *candidate.state_, cleanupIsIdentityBound, error)) {
        verified = false;
    }
    if (verified) {
        bool finalizedNameMatches = false;
        QString finalError;
        if (!entryMatches(
                destination, candidate,
                finalizedNameMatches, finalError)
            || !finalizedNameMatches
            || !destination.parent_.pathMatches(finalError)) {
            error = finalError.isEmpty()
                ? QStringLiteral(
                    "The anchored copy destination changed while its pin was finalized")
                : finalError;
            verified = false;
        }
    }
#endif
    if (verified) {
        copy = std::move(candidate);
        return true;
    }

    if (error.isEmpty()) {
        error = QStringLiteral("Cannot complete an anchored copy");
    }
    if (candidate.isValid()) {
        if (retainDestinationOnFailure) {
            copy = std::move(candidate);
            error += QStringLiteral("; incomplete copy retained at %1")
                .arg(destination.displayPath());
        } else if (cleanupIsIdentityBound) {
            appendCleanupError(error, remove(candidate));
        } else {
            error += QStringLiteral("; unverified copy retained at %1")
                .arg(destination.displayPath());
        }
    } else if (retainDestinationOnFailure) {
        error += QStringLiteral("; incomplete copy may remain at %1")
            .arg(destination.displayPath());
    }
    return false;
}

bool pinRegularFile(
    const EntryRef &entry,
    PinnedFile &file,
    QString &error,
    qint64 maximumSize,
    const PinnedFileReadControl &readControl)
{
    error.clear();
    file = {};
    if (!entry.isValid()) {
        error = QStringLiteral("The anchored file reference is unavailable");
        return false;
    }

#ifdef Q_OS_UNIX
    const QByteArray name = QFile::encodeName(entry.component_);
    if (maximumSize >= 0) {
        UnixStamp named;
        bool exists = false;
        if (!statEntry(
                entry.parent_.state_->descriptor.get(),
                entry.component_, named, exists, error)) {
            return false;
        }
        if (!exists) {
            error = QStringLiteral("The anchored regular file is missing");
            return false;
        }
        if (named.links != 1) {
            error = QStringLiteral(
                "An anchored regular file must have exactly one link");
            return false;
        }
        if (named.size > maximumSize) {
            error = QStringLiteral(
                "The anchored regular file is unexpectedly large");
            return false;
        }
    }
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
    if (maximumSize >= 0 && before.size > maximumSize) {
        error = QStringLiteral(
            "The anchored regular file is unexpectedly large");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, '\0');
    qint64 offset = 0;
    while (offset < before.size) {
        const size_t requested = size_t(std::min<qint64>(
            chunk.size(), before.size - offset));
        if (readControl
            && !readControl(qint64(requested), error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored file read was cancelled");
            }
            return false;
        }
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
        hash.addData(QByteArrayView(
            chunk.constData(), qsizetype(bytesRead)));
        offset += qint64(bytesRead);
    }
    char trailing = 0;
    if (readControl && !readControl(1, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored file read was cancelled");
        }
        return false;
    }
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
    state->durableGeneration = unixDurableGeneration(
        state->descriptor.get(), before);
    state->size = before.size;
    state->sha256 = hash.result();
    file.state_ = std::move(state);
    return true;
#elif defined(Q_OS_WIN)
    if (maximumSize >= 0) {
        WindowsHandle metadata(::CreateFileW(
            reinterpret_cast<LPCWSTR>(
                extendedWindowsPath(entry.displayPath_).utf16()),
            FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!metadata.isValid()) {
            error = windowsError(
                QStringLiteral("Cannot inspect an anchored regular file"),
                ::GetLastError());
            return false;
        }
        WindowsStamp named;
        if (!captureWindowsStamp(
                metadata.get(), named, false, error)) {
            return false;
        }
        if (named.links != 1) {
            error = QStringLiteral(
                "An anchored regular file must have exactly one link");
            return false;
        }
        if (named.size > maximumSize) {
            error = QStringLiteral(
                "The anchored regular file is unexpectedly large");
            return false;
        }
    }
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath_).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
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
    WindowsStamp stamp;
    QByteArray digest;
    if (!hashWindowsRegularFile(
            handle.get(), maximumSize, stamp, digest, error,
            readControl)) {
        return false;
    }

    auto state = std::make_unique<Detail::PinnedFileState>();
    state->entry = entry;
    state->handle = std::move(handle);
    state->stamp = stamp;
    state->identity = windowsIdentity(stamp, 'f');
    state->durableGeneration = windowsDurableGeneration(stamp);
    state->size = stamp.size;
    state->sha256 = std::move(digest);
    file.state_ = std::move(state);
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool pinRegularFileIdentity(
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
    UnixStamp stamp;
    if (!captureUnixStamp(descriptor.get(), stamp, false, error))
        return false;
    if (stamp.links != 1) {
        error = QStringLiteral(
            "An anchored regular file must have exactly one link");
        return false;
    }
    auto state = std::make_unique<Detail::PinnedFileState>();
    state->entry = entry;
    state->descriptor = std::move(descriptor);
    state->stamp = stamp;
    state->identity = unixIdentity(stamp, 'f');
    state->size = stamp.size;
    file.state_ = std::move(state);
    return true;
#elif defined(Q_OS_WIN)
    WindowsHandle handle(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath_).utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.isValid()) {
        error = windowsError(
            QStringLiteral("Cannot open an anchored regular file"),
            ::GetLastError());
        return false;
    }
    WindowsStamp stamp;
    if (!captureWindowsStamp(handle.get(), stamp, false, error))
        return false;
    if (stamp.links != 1) {
        error = QStringLiteral(
            "An anchored regular file must have exactly one link");
        return false;
    }
    auto state = std::make_unique<Detail::PinnedFileState>();
    state->entry = entry;
    state->handle = std::move(handle);
    state->stamp = stamp;
    state->identity = windowsIdentity(stamp, 'f');
    state->size = stamp.size;
    file.state_ = std::move(state);
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool guardFileGeneration(
    const EntryRef &entry,
    const PinnedFile &file,
    FileGenerationGuard &guard,
    QString &error)
{
    guard = {};
    error.clear();
    if (!entry.isValid() || !file.state_) {
        error = QStringLiteral("A guarded file generation is unavailable");
        return false;
    }

    bool matches = false;
    if (!entryIdentityMatches(entry, file, matches, error) || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral("The guarded file generation changed");
        }
        return false;
    }

    auto state = std::make_unique<Detail::FileGenerationGuardState>();
#if defined(Q_OS_LINUX)
    state->notifications = FileDescriptor(::inotify_init1(
        IN_CLOEXEC | IN_NONBLOCK));
    if (!state->notifications.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot create a file-generation guard"), errno);
        return false;
    }
    reportAnchoredFilesystemTransition(
        "file-generation-guard-before-watch", entry.displayPath_);
    const QByteArray path = QByteArrayLiteral("/proc/self/fd/")
        + QByteArray::number(file.state_->descriptor.get());
    state->watch = ::inotify_add_watch(
        state->notifications.get(), path.constData(),
        IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT);
    if (state->watch < 0) {
        error = nativeError(
            QStringLiteral("Cannot watch a file generation"), errno);
        return false;
    }
    reportAnchoredFilesystemTransition(
        "file-generation-guard-watch-installed", entry.displayPath_);
#elif defined(Q_OS_MACOS)
    state->watchedFile = FileDescriptor(::dup(file.state_->descriptor.get()));
    state->notifications = FileDescriptor(::kqueue());
    if (!state->watchedFile.isValid()
        || !state->notifications.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot create a file-generation guard"), errno);
        return false;
    }

    FileDescriptor current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current.isValid()) {
        error = nativeError(
            QStringLiteral("Cannot anchor a file-generation guard"), errno);
        return false;
    }
    state->watchedDirectories.push_back(std::move(current));
    const QStringList components =
        entry.parent_.state_->displayPath.split(
            QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        const QByteArray encoded = QFile::encodeName(component);
        FileDescriptor next(::openat(
            state->watchedDirectories.back().get(),
            encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next.isValid()) {
            error = nativeError(
                QStringLiteral("Cannot anchor a file-generation path"),
                errno);
            return false;
        }
        state->watchedDirectories.push_back(std::move(next));
    }
    UnixStamp guardedParent;
    if (!captureUnixStamp(
            state->watchedDirectories.back().get(),
            guardedParent, true, error)
        || unixIdentity(guardedParent, 'd')
            != entry.parent_.state_->identity) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The guarded file path generation changed");
        }
        return false;
    }

    std::vector<struct kevent> changes(
        state->watchedDirectories.size() + 1);
    EV_SET(
        &changes[0], state->watchedFile.get(), EVFILT_VNODE,
        EV_ADD | EV_CLEAR,
        NOTE_DELETE | NOTE_LINK | NOTE_RENAME | NOTE_REVOKE,
        0, nullptr);
    for (size_t index = 0;
         index < state->watchedDirectories.size(); ++index) {
        EV_SET(
            &changes[index + 1],
            state->watchedDirectories.at(index).get(), EVFILT_VNODE,
            EV_ADD | EV_CLEAR,
            NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
            0, nullptr);
    }
    if (::kevent(
            state->notifications.get(), changes.data(),
            int(changes.size()),
            nullptr, 0, nullptr) != 0) {
        error = nativeError(
            QStringLiteral("Cannot watch a file generation"), errno);
        return false;
    }
#elif defined(Q_OS_WIN)
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(), file.state_->handle.get(),
            ::GetCurrentProcess(), &duplicate,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = windowsError(
            QStringLiteral("Cannot create a file-generation guard"),
            ::GetLastError());
        return false;
    }
    state->watchedFile = WindowsHandle(duplicate);
    state->identity = file.state_->identity;
#else
    error = QStringLiteral(
        "File-generation guards are unsupported on this platform");
    return false;
#endif

    guard.state_ = std::move(state);
    if (!guard.unchanged(error)
        || !entryIdentityMatches(entry, file, matches, error)
        || !matches) {
        guard = {};
        if (error.isEmpty()) {
            error = QStringLiteral("The guarded file generation changed");
        }
        return false;
    }
    return true;
}

bool pinRegularFileAfterWriterRelease(
    const EntryRef &entry,
    qintptr writerDescriptor,
    qint64 expectedSize,
    const QByteArray &expectedSha256,
    const std::function<void()> &releaseWriter,
    PinnedFile &file,
    WriterPinHandoffState &handoff,
    QString &error)
{
    error.clear();
    file = {};
    handoff = {};
    if (!releaseWriter
        || writerDescriptor < 0
        || writerDescriptor > std::numeric_limits<int>::max()
        || expectedSize < 0
        || expectedSha256.size() != 32) {
        error = QStringLiteral(
            "The anchored file writer cannot be verified");
        return false;
    }
    if (!entry.isValid()) {
        error = QStringLiteral("The anchored file reference is unavailable");
        return false;
    }

    const auto release = [&]() {
        if (handoff.writerReleased) return;
        releaseWriter();
        handoff.writerReleased = true;
        reportAnchoredFilesystemTransition(
            "output-pin-writer-released", entry.displayPath());
    };

#ifdef Q_OS_WIN
    const qintptr nativeWriter = ::_get_osfhandle(int(writerDescriptor));
    if (nativeWriter == -1) {
        error = QStringLiteral(
            "The anchored file writer handle is unavailable");
        return false;
    }
    WindowsStamp writerStamp;
    if (!captureWindowsStamp(
            reinterpret_cast<HANDLE>(nativeWriter),
            writerStamp, false, error)) {
        return false;
    }
    if (writerStamp.links != 1 || writerStamp.size != expectedSize) {
        error = QStringLiteral(
            "The anchored writer output has an unexpected extent");
        release();
        return false;
    }
    const NativeIdentity writerIdentity =
        windowsIdentity(writerStamp, 'f');
    reportAnchoredFilesystemTransition(
        "writer-pin-identity-captured", entry.displayPath());

    WindowsHandle bridge(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath()).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!bridge.isValid()) {
        error = windowsError(
            QStringLiteral("Cannot bridge an anchored file writer"),
            ::GetLastError());
        release();
        return false;
    }
    WindowsStamp bridgeStamp;
    QByteArray bridgeDigest;
    if (!hashWindowsRegularFile(
            bridge.get(), expectedSize,
            bridgeStamp, bridgeDigest, error)) {
        release();
        return false;
    }
    if (!sameWindowsIdentityAndData(writerStamp, bridgeStamp)
        || bridgeStamp.size != expectedSize
        || bridgeDigest != expectedSha256) {
        error = QStringLiteral(
            "The anchored writer output changed before it was pinned");
        release();
        return false;
    }

    release();

    PinnedFile immutable;
    if (!pinRegularFile(
            entry, immutable, error, expectedSize)) {
        return false;
    }
    if (immutable.identity() != writerIdentity
        || immutable.identity().linkCount() != writerStamp.links
        || immutable.size() != expectedSize
        || immutable.sha256() != expectedSha256) {
        error = QStringLiteral(
            "The anchored file changed while its writer was released");
        return false;
    }
    file = std::move(immutable);
    return true;
#elif defined(Q_OS_UNIX)
    UnixStamp writerStamp;
    if (!captureUnixStamp(
            int(writerDescriptor), writerStamp, false, error)) {
        return false;
    }
    if (writerStamp.links != 1 || writerStamp.size != expectedSize) {
        error = QStringLiteral(
            "The anchored writer output has an unexpected extent");
        release();
        return false;
    }
    const NativeIdentity writerIdentity = unixIdentity(writerStamp, 'f');
    reportAnchoredFilesystemTransition(
        "writer-pin-identity-captured", entry.displayPath());

    PinnedFile candidate;
    if (!pinRegularFile(
            entry, candidate, error, expectedSize)) {
        release();
        return false;
    }
    if (candidate.identity() != writerIdentity
        || candidate.identity().linkCount() != writerStamp.links
        || candidate.size() != expectedSize
        || candidate.sha256() != expectedSha256) {
        error = QStringLiteral(
            "The anchored writer output changed before it was pinned");
        release();
        return false;
    }

    release();
    bool matches = false;
    if (!entryMatches(entry, candidate, matches, error)) {
        return false;
    }
    if (!matches) {
        error = QStringLiteral(
            "The anchored file changed while its writer was released");
        return false;
    }
    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            *candidate.state_, discard, verifiedDigest, error)
        || verifiedDigest != expectedSha256) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored file changed while its writer was released");
        }
        return false;
    }
    file = std::move(candidate);
    return true;
#else
    Q_UNUSED(writerDescriptor)
    Q_UNUSED(expectedSize)
    Q_UNUSED(expectedSha256)
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
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath_).utf16()),
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
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath_).utf16()),
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

bool entryIdentityMatches(
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
    UnixStamp named;
    bool exists = false;
    if (!statEntry(
            entry.parent_.state_->descriptor.get(),
            entry.component_, named, exists, error)) {
        return false;
    }
    matches = exists
        && sameUnixObject(pinnedNow, named)
        && file.state_->identity == unixIdentity(pinnedNow, 'f');
    return true;
#elif defined(Q_OS_WIN)
    WindowsStamp pinnedNow;
    if (!captureWindowsStamp(
            file.state_->handle.get(), pinnedNow, false, error)) {
        return false;
    }
    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(entry.displayPath_).utf16()),
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
    matches = sameWindowsObject(pinnedNow, namedStamp)
        && file.state_->identity == windowsIdentity(pinnedNow, 'f');
    return true;
#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool validateCurrentUserOwnedDirectory(
    const DirectoryAnchor &directory,
    QString &error)
{
    error.clear();
    if (!directory.state_) {
        error = QStringLiteral(
            "The anchored directory is unavailable");
        return false;
    }
    if (!directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored directory path was replaced");
        }
        return false;
    }

#ifdef Q_OS_UNIX
    struct stat status {};
    if (::fstat(
            directory.state_->descriptor.get(), &status) != 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot inspect anchored directory ownership"),
            errno);
        return false;
    }
    if (!S_ISDIR(status.st_mode)
        || quint64(status.st_dev) != directory.state_->stamp.device
        || quint64(status.st_ino) != directory.state_->stamp.inode) {
        error = QStringLiteral(
            "The anchored directory identity changed while checking ownership");
        return false;
    }
    if (status.st_uid != ::geteuid()) {
        error = QStringLiteral(
            "The anchored directory is not owned by the current user");
        return false;
    }
    return true;

#elif defined(Q_OS_WIN)
    const QString nativePath = QDir::toNativeSeparators(
        directory.state_->displayPath);
    WindowsHandle bridge = openWindowsDirectoryBridge(
        nativePath, error);
    WindowsStamp bridgeStamp;
    if (!bridge.isValid()
        || !captureWindowsStamp(
            bridge.get(), bridgeStamp, true, error)
        || !sameWindowsObject(
            bridgeStamp, directory.state_->stamp)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored directory identity changed while checking ownership");
        }
        return false;
    }
    QByteArray userStorage;
    PSID userSid = nullptr;
    bool ownerMatches = false;
    if (!currentWindowsUserSid(userStorage, userSid)
        || !windowsDirectoryHandleOwnerMatches(
            bridge.get(), userSid, ownerMatches, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot resolve the current Windows directory owner");
        }
        return false;
    }
    if (!ownerMatches) {
        error = QStringLiteral(
            "The anchored directory is not owned by the current user");
        return false;
    }
    return true;

#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool validateCurrentUserControlledDirectory(
    const DirectoryAnchor &directory,
    QString &error)
{
    if (!validateCurrentUserOwnedDirectory(directory, error))
        return false;

#ifdef Q_OS_UNIX
    struct stat status {};
    if (::fstat(
            directory.state_->descriptor.get(), &status) != 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot inspect anchored parent permissions"),
            errno);
        return false;
    }
    if (status.st_mode & S_IWOTH) {
        error = QStringLiteral(
            "The anchored parent directory is writable by other users");
        return false;
    }
    if (status.st_mode & S_IWGRP) {
#if defined(Q_OS_LINUX)
        if (!unixGroupWritersAreRestricted(status.st_gid, error)) {
            return false;
        }
#else
        error = QStringLiteral(
            "The anchored parent directory is group-writable");
        return false;
#endif
    }
#if defined(Q_OS_LINUX)
    if (!linuxDirectoryAccessAclIsAbsent(
            directory.state_->descriptor.get(), error)) {
        return false;
    }
#endif
#if defined(Q_OS_MACOS)
    bool hasExtendedAcl = false;
    if (!macDirectoryHasExtendedAcl(
            directory.state_->descriptor.get(),
            hasExtendedAcl, error)) {
        return false;
    }
    if (hasExtendedAcl) {
        error = QStringLiteral(
            "The anchored parent has extended access controls");
        return false;
    }
#endif
    if (!directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored parent path changed while checking access controls");
        }
        return false;
    }
    return true;

#elif defined(Q_OS_WIN)
    const QString nativePath = QDir::toNativeSeparators(
        directory.state_->displayPath);
    WindowsHandle bridge = openWindowsDirectoryBridge(
        nativePath, error);
    WindowsStamp bridgeStamp;
    if (!bridge.isValid()
        || !captureWindowsStamp(
            bridge.get(), bridgeStamp, true, error)
        || !sameWindowsObject(
            bridgeStamp, directory.state_->stamp)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored parent identity changed while checking access controls");
        }
        return false;
    }
    if (!windowsDirectoryHandleIsCurrentUserControlled(
            bridge.get(), error)) {
        return false;
    }
    if (!directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The anchored parent path changed while checking access controls");
        }
        return false;
    }
    return true;

#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif
}

bool hardenPrivateDirectory(
    DirectoryAnchor &directory,
    QString &error)
{
    error.clear();
    if (!validateCurrentUserOwnedDirectory(directory, error))
        return false;

#ifdef Q_OS_UNIX
    UnixStamp before;
    if (!captureUnixStamp(
            directory.state_->descriptor.get(), before,
            true, error)
        || !sameUnixObject(before, directory.state_->stamp)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private directory identity changed before hardening");
        }
        return false;
    }
    if (::fchmod(directory.state_->descriptor.get(), S_IRWXU) != 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot harden anchored directory permissions"),
            errno);
        return false;
    }
#if defined(Q_OS_LINUX)
    if (!removeLinuxDirectoryAcls(
            directory.state_->descriptor.get(), error)
        || ::fchmod(
            directory.state_->descriptor.get(), S_IRWXU) != 0) {
        if (error.isEmpty()) {
            error = nativeError(
                QStringLiteral(
                    "Cannot finalize private directory permissions"),
                errno);
        }
        return false;
    }
#endif
#if defined(Q_OS_MACOS)
    if (!removeMacDirectoryExtendedAcl(
            directory.state_->descriptor.get(), error)
        || ::fchmod(
            directory.state_->descriptor.get(), S_IRWXU) != 0) {
        if (error.isEmpty()) {
            error = nativeError(
                QStringLiteral(
                    "Cannot finalize private directory permissions"),
                errno);
        }
        return false;
    }
#endif
    struct stat status {};
    if (::fstat(
            directory.state_->descriptor.get(), &status) != 0) {
        error = nativeError(
            QStringLiteral(
                "Cannot verify hardened directory permissions"),
            errno);
        return false;
    }
    if (!S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid()
        || (status.st_mode & 0777) != S_IRWXU) {
        error = QStringLiteral(
            "The anchored directory is not private");
        return false;
    }
    UnixStamp hardened;
    if (!captureUnixStamp(
            directory.state_->descriptor.get(), hardened,
            true, error)
        || !sameUnixObject(hardened, before)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private directory identity changed while hardening");
        }
        return false;
    }
    directory.state_->stamp = hardened;
    directory.state_->identity = unixIdentity(hardened, 'd');

#elif defined(Q_OS_WIN)
    const QString nativePath = QDir::toNativeSeparators(
        directory.state_->displayPath);
    QString bridgeError;
    WindowsHandle bridge = openWindowsDirectoryBridge(
        nativePath, bridgeError);
    WindowsStamp bridgeStamp;
    if (!bridge.isValid()
        || !captureWindowsStamp(
            bridge.get(), bridgeStamp, true, bridgeError)
        || !sameWindowsObject(
            bridgeStamp, directory.state_->stamp)) {
        error = bridgeError.isEmpty()
            ? QStringLiteral(
                  "The private directory identity changed before hardening")
            : bridgeError;
        return false;
    }

    // Reapplying an inheritable DACL can update existing children's ChangeTime.
    // Keep repeated hardening observational when security is already exact.
    bool wasPrivate = false;
    if (!inspectWindowsDirectoryPrivateSecurity(
            bridge.get(), wasPrivate, error)) {
        return false;
    }
    WindowsStamp inspectedSecurityStamp;
    if (!captureWindowsStamp(
            bridge.get(), inspectedSecurityStamp, true, error)
        || !sameWindowsObject(inspectedSecurityStamp, bridgeStamp)
        || (wasPrivate
            && !sameWindowsIdentityAndData(
                inspectedSecurityStamp, bridgeStamp))) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private directory changed while its security was inspected");
        }
        return false;
    }

    if (!wasPrivate) {
        WindowsPrivateDirectorySecurity privateSecurity;
        if (!privateSecurity.isValid()) {
            error = QStringLiteral(
                "Cannot prepare private Windows directory security");
            return false;
        }
        bool ownerMatches = false;
        if (!windowsDirectoryHandleOwnerMatches(
                bridge.get(), privateSecurity.ownerSid(),
                ownerMatches, error)
            || !ownerMatches) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored directory is not owned by the current user");
            }
            return false;
        }
        const DWORD mutationAccess = FILE_READ_ATTRIBUTES | READ_CONTROL
            | WRITE_DAC | SYNCHRONIZE;
        WindowsHandle mutation(::CreateFileW(
            reinterpret_cast<LPCWSTR>(
                extendedWindowsPath(nativePath).utf16()),
            mutationAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS
                | FILE_FLAG_OPEN_REPARSE_POINT
                | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (!mutation.isValid()) {
            error = windowsError(
                QStringLiteral(
                    "Cannot open an anchored directory for hardening"),
                ::GetLastError());
            return false;
        }
        WindowsStamp mutationStamp;
        if (!captureWindowsStamp(
                mutation.get(), mutationStamp, true, error)
            || !sameWindowsObject(mutationStamp, bridgeStamp)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The private directory identity changed while opening it for hardening");
            }
            return false;
        }
        const SECURITY_INFORMATION securityInformation =
            DACL_SECURITY_INFORMATION
            | PROTECTED_DACL_SECURITY_INFORMATION;
        const DWORD securityResult = ::SetSecurityInfo(
            mutation.get(), SE_FILE_OBJECT,
            securityInformation, nullptr, nullptr,
            privateSecurity.acl(), nullptr);
        if (securityResult != ERROR_SUCCESS) {
            error = windowsError(
                QStringLiteral(
                    "Cannot harden a Windows directory access list"),
                securityResult);
            return false;
        }
        bool mutationPrivate = false;
        if (!inspectWindowsDirectoryPrivateSecurity(
                mutation.get(), mutationPrivate, error)) {
            return false;
        }
        if (!mutationPrivate) {
            error = QStringLiteral(
                "The anchored Windows directory is not private");
            return false;
        }
    }

    WindowsStamp beforeFinalSecurity;
    bool finalPrivate = false;
    WindowsStamp hardened;
    if (!captureWindowsStamp(
            bridge.get(), beforeFinalSecurity, true, error)
        || !sameWindowsObject(beforeFinalSecurity, bridgeStamp)
        || (wasPrivate
            && !sameWindowsIdentityAndData(
                beforeFinalSecurity, inspectedSecurityStamp))
        || !inspectWindowsDirectoryPrivateSecurity(
            bridge.get(), finalPrivate, error)
        || !finalPrivate
        || !captureWindowsStamp(
            bridge.get(), hardened, true, error)
        || !sameWindowsIdentityAndData(
            hardened, beforeFinalSecurity)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private directory changed while hardening");
        }
        return false;
    }
    directory.state_->stamp = hardened;
    directory.state_->identity = windowsIdentity(hardened, 'd');

#else
    error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return false;
#endif

    if (!directory.sync(error)) return false;
    if (!directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private directory path changed after hardening");
        }
        return false;
    }
    return true;
}

MutationResult Detail::PrivateDirectoryOperations::create(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory)
{
    MutationResult result;
    directory = {};
    if (!parent.state_) {
        result.error = QStringLiteral(
            "The anchored parent directory is unavailable");
        return result;
    }
    if (!validPortableComponent(component)) {
        result.error = QStringLiteral(
            "The anchored child directory name is unsafe");
        return result;
    }

    QString matchError;
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored child directory parent was replaced")
            : matchError;
        return result;
    }

    const QString finalPath =
        QDir(parent.state_->displayPath).filePath(component);
    QString stagingComponent;
    QString stagingPath;
    DirectoryAnchor staging;

    const auto appendCleanupFailure = [&result](
        const MutationResult &cleanup) {
        if (cleanup.effect == MutationEffect::AppliedDurable) return;
        result.effect = MutationEffect::Partial;
        if (result.verifiedRecoveryPath.isEmpty()) {
            result.verifiedRecoveryPath =
                cleanup.verifiedRecoveryPath;
        }
        result.removalRequested = result.removalRequested
            || cleanup.removalRequested;
        if (!result.error.isEmpty()) result.error += QStringLiteral("; ");
        result.error += cleanup.error.isEmpty()
            ? QStringLiteral(
                  "cannot remove an unpublished private directory")
            : cleanup.error;
    };
    const auto cleanupStaging = [&appendCleanupFailure](
        DirectoryAnchor &candidate) {
        if (!candidate.isValid()) return;
        appendCleanupFailure(removeEmptyDirectory(candidate));
    };
#ifdef Q_OS_UNIX
    struct stat parentStatus {};
    if (::fstat(
            parent.state_->descriptor.get(), &parentStatus) != 0) {
        result.error = nativeError(
            QStringLiteral(
                "Cannot verify private directory parent permissions"),
            errno);
        return result;
    }
    if (!S_ISDIR(parentStatus.st_mode)
        || parentStatus.st_uid != ::geteuid()
        || (parentStatus.st_mode & 0777) != S_IRWXU) {
        result.error = QStringLiteral(
            "The anchored parent directory is not private");
        return result;
    }
#if defined(Q_OS_LINUX)
    if (!linuxDirectoryAclsAreAbsent(
            parent.state_->descriptor.get(), result.error)) {
        return result;
    }
#endif
#if defined(Q_OS_MACOS)
    bool parentHasExtendedAcl = false;
    if (!macDirectoryHasExtendedAcl(
            parent.state_->descriptor.get(),
            parentHasExtendedAcl, result.error)) {
        return result;
    }
    if (parentHasExtendedAcl) {
        result.error = QStringLiteral(
            "The anchored parent has inheritable extended access controls");
        return result;
    }
#endif
    bool destinationExists = false;
    if (!entryNameExists(
            parent.state_->descriptor.get(), component,
            destinationExists, result.error)) {
        return result;
    }
    if (destinationExists) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The private child directory already exists");
        return result;
    }

    FileDescriptor descriptor;
    for (int attempt = 0; attempt < 32; ++attempt) {
        stagingComponent = QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .toLower();
        if (stagingComponent == component) continue;
        const QByteArray encoded = QFile::encodeName(stagingComponent);
        if (::mkdirat(
                parent.state_->descriptor.get(),
                encoded.constData(), S_IRWXU) == 0) {
            stagingPath = QDir(parent.state_->displayPath)
                .filePath(stagingComponent);
            descriptor = FileDescriptor(::openat(
                parent.state_->descriptor.get(), encoded.constData(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            break;
        }
        if (errno != EEXIST) {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot create a private anchored child directory"),
                errno);
            return result;
        }
        stagingComponent.clear();
    }
    if (stagingComponent.isEmpty()) {
        result.error = QStringLiteral(
            "Cannot reserve a private anchored child directory name");
        return result;
    }
    if (!descriptor.isValid()) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral(
                "Cannot anchor a newly created private directory"),
            errno);
        return result;
    }

    UnixStamp stamp;
    if (!captureUnixStamp(descriptor.get(), stamp, true, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    auto state = std::make_shared<Detail::DirectoryState>();
    state->ancestor = parent.state_;
    state->descriptor = std::move(descriptor);
    state->stamp = stamp;
    state->component = stagingComponent;
    state->displayPath = stagingPath;
    state->identity = unixIdentity(stamp, 'd');
    staging = DirectoryAnchor(std::move(state));

    if (::fchmod(staging.state_->descriptor.get(), S_IRWXU) != 0) {
        result.error = nativeError(
            QStringLiteral(
                "Cannot make an anchored child directory private"),
            errno);
        cleanupStaging(staging);
        return result;
    }
#if defined(Q_OS_LINUX)
    if (!removeLinuxDirectoryAcls(
            staging.state_->descriptor.get(), result.error)
        || ::fchmod(
            staging.state_->descriptor.get(), S_IRWXU) != 0) {
        if (result.error.isEmpty()) {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot finalize private directory permissions"),
                errno);
        }
        cleanupStaging(staging);
        return result;
    }
#endif
#if defined(Q_OS_MACOS)
    if (!removeMacDirectoryExtendedAcl(
            staging.state_->descriptor.get(), result.error)
        || ::fchmod(
            staging.state_->descriptor.get(), S_IRWXU) != 0) {
        if (result.error.isEmpty()) {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot finalize private directory permissions"),
                errno);
        }
        cleanupStaging(staging);
        return result;
    }
#endif
    struct stat privateStatus {};
    if (::fstat(
            staging.state_->descriptor.get(), &privateStatus) != 0) {
        result.error = nativeError(
            QStringLiteral(
                "Cannot verify private directory permissions"),
            errno);
        cleanupStaging(staging);
        return result;
    }
    if (!S_ISDIR(privateStatus.st_mode)
        || privateStatus.st_uid != ::geteuid()
        || (privateStatus.st_mode & 0777) != S_IRWXU) {
        result.error = QStringLiteral(
            "The anchored child directory is not private");
        cleanupStaging(staging);
        return result;
    }
    if (!captureUnixStamp(
            staging.state_->descriptor.get(),
            staging.state_->stamp, true, result.error)) {
        cleanupStaging(staging);
        return result;
    }
    staging.state_->identity =
        unixIdentity(staging.state_->stamp, 'd');
    if (!staging.sync(result.error)) {
        cleanupStaging(staging);
        return result;
    }

    reportAnchoredFilesystemTransition(
        "private-directory-staging-anchored",
        stagingPath,
        finalPath);
    reportAnchoredFilesystemTransition(
        "private-directory-before-publish",
        stagingPath,
        finalPath);
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The private directory parent changed before publication")
            : matchError;
        cleanupStaging(staging);
        return result;
    }

    UnixStamp namedStaging;
    bool stagingExists = false;
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(), stagingComponent,
            namedStaging, stagingExists, result.error)
        || !stagingExists
        || !sameUnixObject(namedStaging, staging.state_->stamp)) {
        result.effect = MutationEffect::Conflict;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The private directory staging name was replaced");
        }
        cleanupStaging(staging);
        return result;
    }

    const QByteArray stagingName = QFile::encodeName(stagingComponent);
    const QByteArray finalName = QFile::encodeName(component);
    bool unsupported = false;
    if (renameNoReplaceNative(
            parent.state_->descriptor.get(), stagingName,
            parent.state_->descriptor.get(), finalName,
            unsupported) != 0) {
        const int renameError = errno;
        result.effect = renameError == EEXIST
            ? MutationEffect::Conflict : MutationEffect::NoEffect;
        if (renameError == EEXIST) {
            result.error = QStringLiteral(
                "The private child directory already exists");
        } else if (unsupported) {
            result.error = QStringLiteral(
                "The filesystem cannot publish a private directory without replacement");
        } else {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot publish an anchored private directory"),
                renameError);
        }
        cleanupStaging(staging);
        return result;
    }

    UnixStamp moved;
    UnixStamp namedFinal;
    bool finalExists = false;
    bool oldNameExists = false;
    QString verificationError;
    const bool movedCaptured = captureUnixStamp(
        staging.state_->descriptor.get(), moved,
        true, verificationError);
    const bool movedIdentityMatches = movedCaptured
        && sameUnixObject(moved, staging.state_->stamp);
    if (!movedIdentityMatches
        || !statDirectoryEntry(
            parent.state_->descriptor.get(), component,
            namedFinal, finalExists, verificationError)
        || !finalExists
        || !sameUnixObject(namedFinal, moved)
        || !entryNameExists(
            parent.state_->descriptor.get(), stagingComponent,
            oldNameExists, verificationError)
        || oldNameExists) {
        staging.state_->component = component;
        staging.state_->displayPath = finalPath;
        if (movedIdentityMatches) {
            staging.state_->stamp = moved;
            staging.state_->identity = unixIdentity(moved, 'd');
        }
        directory = std::move(staging);
        result.effect = MutationEffect::Partial;
        result.error = verificationError.isEmpty()
            ? QStringLiteral(
                  "The private directory changed during publication")
            : verificationError;
        return result;
    }
    staging.state_->component = component;
    staging.state_->displayPath = finalPath;
    staging.state_->stamp = moved;
    staging.state_->identity = unixIdentity(moved, 'd');
    directory = std::move(staging);

#elif defined(Q_OS_WIN)
    const QString nativeParent = QDir::toNativeSeparators(
        parent.state_->displayPath);
    QString parentSecurityError;
    WindowsHandle parentSecurity = openWindowsDirectoryBridge(
        nativeParent, parentSecurityError);
    WindowsStamp parentSecurityStamp;
    if (!parentSecurity.isValid()
        || !captureWindowsStamp(
            parentSecurity.get(), parentSecurityStamp,
            true, parentSecurityError)
        || !sameWindowsObject(
            parentSecurityStamp, parent.state_->stamp)
        || !windowsDirectoryHandleHasPrivateSecurity(
            parentSecurity.get())) {
        result.error = parentSecurityError.isEmpty()
            ? QStringLiteral(
                  "The anchored parent directory is not private")
            : parentSecurityError;
        return result;
    }
    const QString nativeFinal = extendedWindowsPath(finalPath);
    const DWORD finalAttributes = ::GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeFinal.utf16()));
    if (finalAttributes != INVALID_FILE_ATTRIBUTES) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The private child directory already exists");
        return result;
    }
    const DWORD attributesError = ::GetLastError();
    if (attributesError != ERROR_FILE_NOT_FOUND
        && attributesError != ERROR_PATH_NOT_FOUND) {
        result.error = windowsError(
            QStringLiteral(
                "Cannot inspect a private child directory name"),
            attributesError);
        return result;
    }

    WindowsPrivateDirectorySecurity privateSecurity;
    if (!privateSecurity.isValid()) {
        result.error = QStringLiteral(
            "Cannot prepare private Windows directory security");
        return result;
    }
    for (int attempt = 0; attempt < 32; ++attempt) {
        stagingComponent = QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .toLower();
        if (stagingComponent == component) continue;
        stagingPath = QDir(parent.state_->displayPath)
            .filePath(stagingComponent);
        const QString nativeStaging = extendedWindowsPath(stagingPath);
        if (::CreateDirectoryW(
                reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
                privateSecurity.attributes())) {
            break;
        }
        const DWORD createError = ::GetLastError();
        if (createError != ERROR_ALREADY_EXISTS
            && createError != ERROR_FILE_EXISTS) {
            result.error = windowsError(
                QStringLiteral(
                    "Cannot create a private anchored child directory"),
                createError);
            return result;
        }
        stagingComponent.clear();
        stagingPath.clear();
    }
    if (stagingComponent.isEmpty()) {
        result.error = QStringLiteral(
            "Cannot reserve a private anchored child directory name");
        return result;
    }

    QString openError;
    const QString nativeStaging = extendedWindowsPath(stagingPath);
    WindowsHandle observation(::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE
            | FILE_READ_ATTRIBUTES | FILE_ADD_FILE
            | FILE_ADD_SUBDIRECTORY | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!observation.isValid()) {
        result.effect = MutationEffect::Partial;
        result.error = windowsError(
            QStringLiteral(
                "Cannot anchor a newly created private directory"),
            ::GetLastError());
        return result;
    }
    WindowsStamp stamp;
    if (!captureWindowsStamp(
            observation.get(), stamp, true, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    auto state = std::make_shared<Detail::DirectoryState>();
    state->ancestor = parent.state_;
    state->handles.push_back(std::move(observation));
    state->stamp = stamp;
    state->component = stagingComponent;
    state->displayPath = stagingPath;
    state->identity = windowsIdentity(stamp, 'd');
    staging = DirectoryAnchor(std::move(state));
    if (!windowsDirectoryHandleHasPrivateSecurity(
            staging.state_->handles.back().get())) {
        result.error = QStringLiteral(
            "The anchored child directory is not private");
        cleanupStaging(staging);
        return result;
    }

    reportAnchoredFilesystemTransition(
        "private-directory-staging-anchored",
        stagingPath,
        finalPath);
    reportAnchoredFilesystemTransition(
        "private-directory-before-publish",
        stagingPath,
        finalPath);
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The private directory parent changed before publication")
            : matchError;
        cleanupStaging(staging);
        return result;
    }

    QString bridgeError;
    WindowsHandle bridge = openWindowsDirectoryBridge(
        nativeStaging, bridgeError);
    WindowsStamp bridgeStamp;
    if (!bridge.isValid()
        || !captureWindowsStamp(
            bridge.get(), bridgeStamp, true, bridgeError)
        || !sameWindowsObject(
            bridgeStamp, staging.state_->stamp)) {
        result.effect = MutationEffect::Conflict;
        result.error = bridgeError.isEmpty()
            ? QStringLiteral(
                  "The private directory staging name was replaced")
            : bridgeError;
        cleanupStaging(staging);
        return result;
    }

    staging.state_->handles.clear();
    WindowsHandle mutation(::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
        DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE
            | FILE_READ_ATTRIBUTES | FILE_ADD_FILE
            | FILE_ADD_SUBDIRECTORY | READ_CONTROL
            | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    const DWORD mutationOpenError = mutation.isValid()
        ? ERROR_SUCCESS : ::GetLastError();
    WindowsStamp mutationStamp;
    const bool mutationOpened = mutation.isValid();
    const bool mutationStampCaptured = mutationOpened
        && captureWindowsStamp(
            mutation.get(), mutationStamp, true, result.error);
    const bool mutationIdentityMatched = mutationStampCaptured
        && sameWindowsObject(mutationStamp, bridgeStamp);
    const bool mutationSecurityMatched = mutationIdentityMatched
        && windowsDirectoryHandleHasPrivateSecurity(mutation.get());
    if (!mutationSecurityMatched) {
        mutation.reset();
        WindowsHandle restored = openWindowsDirectoryHandle(
            nativeStaging, true, openError);
        if (restored.isValid()) {
            WindowsStamp restoredStamp;
            if (captureWindowsStamp(
                    restored.get(), restoredStamp,
                    true, openError)
                && sameWindowsObject(restoredStamp, bridgeStamp)) {
                staging.state_->handles.push_back(std::move(restored));
                staging.state_->stamp = restoredStamp;
                staging.state_->identity =
                    windowsIdentity(restoredStamp, 'd');
            }
        }
        if (staging.state_->handles.empty()) {
            staging.state_->handles.push_back(std::move(bridge));
            staging.state_->stamp = bridgeStamp;
            staging.state_->identity =
                windowsIdentity(bridgeStamp, 'd');
        }
        result.effect = mutationOpened && mutationStampCaptured
            ? MutationEffect::Conflict : MutationEffect::NoEffect;
        if (result.error.isEmpty()) {
            result.error = !mutationOpened
                ? windowsError(
                      QStringLiteral(
                          "Cannot open a private directory for publication"),
                      mutationOpenError)
                : !mutationIdentityMatched
                ? QStringLiteral(
                      "The private directory changed before publication")
                : QStringLiteral(
                      "The private directory security changed before publication");
        }
        if (staging.state_->handles.size() == 1)
            cleanupStaging(staging);
        return result;
    }
    bridge.reset();
    staging.state_->handles.push_back(std::move(mutation));

    const DWORD nameBytes = DWORD(
        nativeFinal.size() * sizeof(wchar_t));
    const size_t bufferSize = sizeof(FILE_RENAME_INFO)
        + size_t(nameBytes);
    std::vector<unsigned char> storage(bufferSize, 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = nameBytes;
    std::memcpy(rename->FileName, nativeFinal.utf16(), nameBytes);
    if (!::SetFileInformationByHandle(
            staging.state_->handles.back().get(),
            FileRenameInfo,
            rename,
            DWORD(storage.size()))) {
        const DWORD renameError = ::GetLastError();
        bridgeError.clear();
        WindowsHandle restoreBridge = openWindowsDirectoryBridge(
            nativeStaging, bridgeError);
        WindowsStamp restoreStamp;
        const bool canRestore = restoreBridge.isValid()
            && captureWindowsStamp(
                restoreBridge.get(), restoreStamp,
                true, bridgeError)
            && sameWindowsObject(restoreStamp, mutationStamp);
        if (canRestore) {
            staging.state_->handles.clear();
            WindowsHandle restored = openWindowsDirectoryHandle(
                nativeStaging, true, openError);
            WindowsStamp restoredStamp;
            if (restored.isValid()
                && captureWindowsStamp(
                    restored.get(), restoredStamp,
                    true, openError)
                && sameWindowsObject(restoredStamp, restoreStamp)) {
                staging.state_->handles.push_back(std::move(restored));
                staging.state_->stamp = restoredStamp;
                staging.state_->identity =
                    windowsIdentity(restoredStamp, 'd');
            } else {
                staging.state_->handles.push_back(
                    std::move(restoreBridge));
            }
        }
        const DWORD targetAttributes = ::GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(nativeFinal.utf16()));
        const MutationEffect renameEffect =
            (renameError == ERROR_ALREADY_EXISTS
             || renameError == ERROR_FILE_EXISTS
             || (renameError == ERROR_ACCESS_DENIED
                 && targetAttributes != INVALID_FILE_ATTRIBUTES))
                ? MutationEffect::Conflict : MutationEffect::NoEffect;
        result.effect = canRestore
            ? renameEffect : MutationEffect::Partial;
        result.error = windowsError(
            renameEffect == MutationEffect::Conflict
                ? QStringLiteral(
                      "The private child directory already exists")
                : QStringLiteral(
                      "Cannot publish an anchored private directory"),
            renameError);
        if (!canRestore && !bridgeError.isEmpty()) {
            result.error += QStringLiteral("; ") + bridgeError;
        }
        if (canRestore && staging.state_->handles.size() == 1)
            cleanupStaging(staging);
        return result;
    }

    WindowsStamp moved;
    const bool movedCaptured = captureWindowsStamp(
        staging.state_->handles.back().get(),
        moved, true, result.error);
    const bool movedIdentityMatches = movedCaptured
        && sameWindowsObject(moved, mutationStamp);
    if (!movedIdentityMatches) {
        staging.state_->component = component;
        staging.state_->displayPath = finalPath;
        directory = std::move(staging);
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The private directory identity changed during publication");
        }
        return result;
    }
    staging.state_->component = component;
    staging.state_->displayPath = finalPath;
    staging.state_->stamp = moved;
    staging.state_->identity = windowsIdentity(moved, 'd');

    bridgeError.clear();
    WindowsHandle finalBridge = openWindowsDirectoryBridge(
        nativeFinal, bridgeError);
    WindowsStamp finalBridgeStamp;
    if (!finalBridge.isValid()
        || !captureWindowsStamp(
            finalBridge.get(), finalBridgeStamp,
            true, bridgeError)
        || !sameWindowsObject(finalBridgeStamp, moved)
        || !windowsDirectoryHandleHasPrivateSecurity(
            finalBridge.get())) {
        directory = std::move(staging);
        result.effect = MutationEffect::Partial;
        result.error = bridgeError.isEmpty()
            ? QStringLiteral(
                  "The private directory destination was replaced")
            : bridgeError;
        return result;
    }
    staging.state_->handles.clear();
    WindowsHandle immutable = openWindowsDirectoryHandle(
        nativeFinal, true, openError);
    WindowsStamp immutableStamp;
    if (!immutable.isValid()
        || !captureWindowsStamp(
            immutable.get(), immutableStamp, true, openError)
        || !sameWindowsObject(immutableStamp, finalBridgeStamp)) {
        staging.state_->handles.push_back(std::move(finalBridge));
        staging.state_->stamp = finalBridgeStamp;
        staging.state_->identity =
            windowsIdentity(finalBridgeStamp, 'd');
        directory = std::move(staging);
        result.effect = MutationEffect::Partial;
        result.error = openError.isEmpty()
            ? QStringLiteral(
                  "The private directory changed while finalizing its anchor")
            : openError;
        return result;
    }
    staging.state_->handles.push_back(std::move(immutable));
    staging.state_->stamp = immutableStamp;
    staging.state_->identity = windowsIdentity(immutableStamp, 'd');
    directory = std::move(staging);

#else
    Q_UNUSED(finalPath)
    Q_UNUSED(stagingComponent)
    Q_UNUSED(stagingPath)
    Q_UNUSED(staging)
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif

    reportAnchoredFilesystemTransition(
        "private-directory-published",
        finalPath);
    QString finalError;
    if (!parent.pathMatches(finalError)
        || !directory.pathMatches(finalError)) {
        result.effect = MutationEffect::Partial;
        result.error = finalError.isEmpty()
            ? QStringLiteral(
                  "The private directory changed after publication")
            : finalError;
        return result;
    }
    if (!parent.sync(finalError)) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = finalError;
        return result;
    }
    reportAnchoredFilesystemTransition(
        "private-directory-before-final-name-check",
        finalPath);
    finalError.clear();
    if (!parent.pathMatches(finalError)
        || !directory.pathMatches(finalError)) {
        result.effect = MutationEffect::Partial;
        result.error = finalError.isEmpty()
            ? QStringLiteral(
                  "The private directory changed after synchronization")
            : finalError;
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    return result;
}

MutationResult Detail::PrivateDirectoryOperations::createFixed(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory)
{
    MutationResult result;
    directory = {};
    if (!parent.state_) {
        result.error = QStringLiteral(
            "The anchored parent directory is unavailable");
        return result;
    }
    if (!validPortableComponent(component)) {
        result.error = QStringLiteral(
            "The anchored child directory name is unsafe");
        return result;
    }

    QString matchError;
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored child directory parent was replaced")
            : matchError;
        return result;
    }

    const QString finalPath =
        QDir(parent.state_->displayPath).filePath(component);

#ifdef Q_OS_UNIX
    if (!validateCurrentUserControlledDirectory(
            parent, result.error)) {
        return result;
    }
#if defined(Q_OS_LINUX)
    if (!linuxDirectoryDefaultAclIsAbsent(
            parent.state_->descriptor.get(), result.error)) {
        return result;
    }
#endif

    const QByteArray encoded = QFile::encodeName(component);
    if (::mkdirat(
            parent.state_->descriptor.get(),
            encoded.constData(), S_IRWXU) != 0) {
        const int createError = errno;
        if (createError == EEXIST) {
            result.effect = MutationEffect::Conflict;
            result.error = QStringLiteral(
                "The private child directory already exists");
        } else {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot create a fixed private anchored child directory"),
                createError);
        }
        return result;
    }

    FileDescriptor descriptor(::openat(
        parent.state_->descriptor.get(), encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.isValid()) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral(
                "Cannot anchor a newly created fixed private directory"),
            errno);
        return result;
    }

    UnixStamp created;
    if (!captureUnixStamp(
            descriptor.get(), created, true, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    auto state = std::make_shared<Detail::DirectoryState>();
    state->ancestor = parent.state_;
    state->descriptor = std::move(descriptor);
    state->stamp = created;
    state->component = component;
    state->displayPath = finalPath;
    state->identity = unixIdentity(created, 'd');
    directory = DirectoryAnchor(std::move(state));

    if (::fchmod(
            directory.state_->descriptor.get(), S_IRWXU) != 0) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral(
                "Cannot make a fixed anchored child directory private"),
            errno);
        return result;
    }
#if defined(Q_OS_LINUX)
    if (!removeLinuxDirectoryAcls(
            directory.state_->descriptor.get(), result.error)
        || ::fchmod(
            directory.state_->descriptor.get(), S_IRWXU) != 0) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot finalize fixed private directory permissions"),
                errno);
        }
        return result;
    }
#endif
#if defined(Q_OS_MACOS)
    if (!removeMacDirectoryExtendedAcl(
            directory.state_->descriptor.get(), result.error)
        || ::fchmod(
            directory.state_->descriptor.get(), S_IRWXU) != 0) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot finalize fixed private directory permissions"),
                errno);
        }
        return result;
    }
#endif

    struct stat privateStatus {};
    if (::fstat(
            directory.state_->descriptor.get(), &privateStatus) != 0) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral(
                "Cannot verify fixed private directory permissions"),
            errno);
        return result;
    }
    if (!S_ISDIR(privateStatus.st_mode)
        || privateStatus.st_uid != ::geteuid()
        || (privateStatus.st_mode & 0777) != S_IRWXU) {
        result.effect = MutationEffect::Partial;
        result.error = QStringLiteral(
            "The fixed anchored child directory is not private");
        return result;
    }
    UnixStamp hardened;
    if (!captureUnixStamp(
            directory.state_->descriptor.get(),
            hardened, true, result.error)
        || !sameUnixObject(hardened, created)) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed private directory identity changed while hardening");
        }
        return result;
    }
    directory.state_->stamp = hardened;
    directory.state_->identity = unixIdentity(hardened, 'd');

    UnixStamp named;
    bool namedExists = false;
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(), component,
            named, namedExists, result.error)
        || !namedExists
        || !sameUnixObject(named, hardened)) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed private directory name was replaced after creation");
        }
        return result;
    }

#elif defined(Q_OS_WIN)
    if (!validateCurrentUserControlledDirectory(
            parent, result.error)) {
        return result;
    }

    WindowsPrivateDirectorySecurity privateSecurity;
    if (!privateSecurity.isValid()) {
        result.error = QStringLiteral(
            "Cannot prepare private Windows directory security");
        return result;
    }
    const QString nativeFinal = extendedWindowsPath(finalPath);
    if (!::CreateDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeFinal.utf16()),
            privateSecurity.attributes())) {
        const DWORD createError = ::GetLastError();
        if (createError == ERROR_ALREADY_EXISTS
            || createError == ERROR_FILE_EXISTS) {
            result.effect = MutationEffect::Conflict;
            result.error = QStringLiteral(
                "The private child directory already exists");
        } else {
            result.error = windowsError(
                QStringLiteral(
                    "Cannot create a fixed private anchored child directory"),
                createError);
        }
        return result;
    }

    WindowsHandle observation(::CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeFinal.utf16()),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE
            | FILE_READ_ATTRIBUTES | FILE_ADD_FILE
            | FILE_ADD_SUBDIRECTORY | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!observation.isValid()) {
        result.effect = MutationEffect::Partial;
        result.error = windowsError(
            QStringLiteral(
                "Cannot anchor a newly created fixed private directory"),
            ::GetLastError());
        return result;
    }

    WindowsStamp created;
    if (!captureWindowsStamp(
            observation.get(), created, true, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    auto state = std::make_shared<Detail::DirectoryState>();
    state->ancestor = parent.state_;
    state->handles.push_back(std::move(observation));
    state->stamp = created;
    state->component = component;
    state->displayPath = finalPath;
    state->identity = windowsIdentity(created, 'd');
    directory = DirectoryAnchor(std::move(state));

    if (!windowsDirectoryHandleHasPrivateSecurity(
            directory.state_->handles.back().get())) {
        result.effect = MutationEffect::Partial;
        result.error = QStringLiteral(
            "The fixed anchored Windows child directory is not private");
        return result;
    }
    WindowsStamp verified;
    if (!captureWindowsStamp(
            directory.state_->handles.back().get(),
            verified, true, result.error)
        || !sameWindowsObject(verified, created)) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed private directory identity changed after creation");
        }
        return result;
    }
    directory.state_->stamp = verified;
    directory.state_->identity = windowsIdentity(verified, 'd');

#else
    Q_UNUSED(finalPath)
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif

    reportAnchoredFilesystemTransition(
        "private-fixed-directory-anchored", finalPath);
    QString finalError;
    if (!parent.pathMatches(finalError)
        || !directory.pathMatches(finalError)) {
        result.effect = MutationEffect::Partial;
        result.error = finalError.isEmpty()
            ? QStringLiteral(
                  "The fixed private directory changed after creation")
            : finalError;
        return result;
    }

    reportAnchoredFilesystemTransition(
        "private-fixed-directory-before-child-sync", finalPath);
    if (!directory.sync(finalError)) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = finalError;
        return result;
    }
    reportAnchoredFilesystemTransition(
        "private-fixed-directory-child-synced", finalPath);
    if (!parent.sync(finalError)) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = finalError;
        return result;
    }

    reportAnchoredFilesystemTransition(
        "private-fixed-directory-before-final-name-check", finalPath);
    finalError.clear();
    if (!parent.pathMatches(finalError)
        || !directory.pathMatches(finalError)) {
        result.effect = MutationEffect::Partial;
        result.error = finalError.isEmpty()
            ? QStringLiteral(
                  "The fixed private directory changed after synchronization")
            : finalError;
        return result;
    }

#ifdef Q_OS_UNIX
    struct stat finalStatus {};
    UnixStamp finalNamed;
    bool finalNamedExists = false;
    const int finalStatusResult = ::fstat(
        directory.state_->descriptor.get(), &finalStatus);
    const int finalStatusError = errno;
    if (finalStatusResult != 0
        || !S_ISDIR(finalStatus.st_mode)
        || finalStatus.st_uid != ::geteuid()
        || (finalStatus.st_mode & 0777) != S_IRWXU) {
        result.effect = MutationEffect::Partial;
        result.error = finalStatusResult != 0
            ? nativeError(
                  QStringLiteral(
                      "Cannot verify final fixed private directory permissions"),
                  finalStatusError)
            : QStringLiteral(
                  "The final fixed anchored child directory is not private");
        return result;
    }
#if defined(Q_OS_LINUX)
    if (!linuxDirectoryAclsAreAbsent(
            directory.state_->descriptor.get(), result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
#endif
#if defined(Q_OS_MACOS)
    bool childHasExtendedAcl = false;
    if (!macDirectoryHasExtendedAcl(
            directory.state_->descriptor.get(),
            childHasExtendedAcl, result.error)
        || childHasExtendedAcl) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed private child retained extended access controls");
        }
        return result;
    }
#endif
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(), component,
            finalNamed, finalNamedExists, result.error)
        || !finalNamedExists
        || !sameUnixObject(
            finalNamed, directory.state_->stamp)) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed private directory name changed after synchronization");
        }
        return result;
    }
#elif defined(Q_OS_WIN)
    WindowsStamp finalStamp;
    if (!captureWindowsStamp(
            directory.state_->handles.back().get(),
            finalStamp, true, result.error)
        || !sameWindowsObject(finalStamp, directory.state_->stamp)
        || !windowsDirectoryHandleHasPrivateSecurity(
            directory.state_->handles.back().get())) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The fixed anchored Windows child directory changed after synchronization");
        }
        return result;
    }
    directory.state_->stamp = finalStamp;
    directory.state_->identity = windowsIdentity(finalStamp, 'd');
#endif

    result.effect = MutationEffect::AppliedDurable;
    return result;
}

MutationResult createPrivateChildDirectory(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory)
{
    return Detail::PrivateDirectoryOperations::create(
        parent, component, directory);
}

MutationResult createPrivateFixedChildDirectory(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory)
{
    return Detail::PrivateDirectoryOperations::createFixed(
        parent, component, directory);
}

MutationResult replaceExisting(
    PinnedFile &replacement,
    PinnedFile &expectedTarget)
{
    return replaceExisting(replacement, expectedTarget, {});
}

MutationResult replaceExisting(
    PinnedFile &replacement,
    PinnedFile &expectedTarget,
    const EntryRef &displacedTarget)
{
    MutationResult result;
    if (!replacement.state_ || !expectedTarget.state_) {
        result.error = QStringLiteral(
            "An anchored replacement endpoint is unavailable");
        return result;
    }
    if (replacement.identity() == expectedTarget.identity()) {
        result.error = QStringLiteral(
            "An anchored replacement requires distinct files");
        return result;
    }

    const EntryRef staging = replacement.state_->entry;
    const EntryRef target = expectedTarget.state_->entry;
    if (!staging.isValid() || !target.isValid()
        || staging.parent_.identity() != target.parent_.identity()
        || staging.component_ == target.component_) {
        result.error = QStringLiteral(
            "An anchored replacement requires distinct sibling names");
        return result;
    }

    bool stagingMatches = false;
    bool targetMatches = false;
    if (!entryMatches(
            staging, replacement, stagingMatches, result.error)
        || !stagingMatches) {
        result.effect = MutationEffect::Conflict;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored replacement staging file was replaced");
        }
        return result;
    }
    if (!entryMatches(
            target, expectedTarget, targetMatches, result.error)
        || !targetMatches) {
        result.effect = MutationEffect::Conflict;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored replacement target was replaced");
        }
        return result;
    }
    if (!staging.parent_.pathMatches(result.error)) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored replacement parent was replaced");
        }
        return result;
    }

#ifdef Q_OS_UNIX
    if (displacedTarget.isValid()
        && (displacedTarget.parent_.identity()
                != staging.parent_.identity()
            || displacedTarget.component_ != staging.component_)) {
        result.error = QStringLiteral(
            "The anchored Unix replacement recovery name must be its staging name");
        return result;
    }
    const auto refreshHeld = [](PinnedFile &file, QString &error) {
        UnixStamp current;
        if (!captureUnixStamp(
                file.state_->descriptor.get(), current, false, error)
            || !sameUnixIdentityAndData(
                current, file.state_->stamp)
            || current.links != 1) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "An anchored replacement file changed unexpectedly");
            }
            return false;
        }
        file.state_->stamp = current;
        file.state_->identity = unixIdentity(current, 'f');
        return true;
    };
    const auto nameMatches = [](
        const EntryRef &entry,
        const PinnedFile &file,
        bool &matches,
        QString &error) {
        matches = false;
        UnixStamp held;
        if (!captureUnixStamp(
                file.state_->descriptor.get(), held, false, error)) {
            return false;
        }
        UnixStamp named;
        bool exists = false;
        if (!statEntry(
                entry.parent_.state_->descriptor.get(),
                entry.component_, named, exists, error)) {
            return false;
        }
        matches = exists && named == held;
        return true;
    };

    reportAnchoredFilesystemTransition(
        "replace-before-publish",
        staging.displayPath_,
        target.displayPath_);
    const QByteArray stagingName = QFile::encodeName(staging.component_);
    const QByteArray targetName = QFile::encodeName(target.component_);
    bool unsupported = false;
    const int exchangeResult = exchangeNamesNative(
        staging.parent_.state_->descriptor.get(),
        stagingName,
        target.parent_.state_->descriptor.get(),
        targetName,
        unsupported);
    const int exchangeError = exchangeResult == 0 ? 0 : errno;
    if (exchangeResult == 0) {
        reportAnchoredFilesystemTransition(
            "replace-published",
            staging.displayPath_,
            target.displayPath_);
    }

    QString inspectionError;
    const bool replacementHeld = refreshHeld(
        replacement, inspectionError);
    const bool targetHeld = refreshHeld(
        expectedTarget, inspectionError);
    bool targetIsReplacement = false;
    bool stagingIsExpectedTarget = false;
    const bool targetInspected = replacementHeld
        && nameMatches(
            target, replacement,
            targetIsReplacement, inspectionError);
    const bool stagingInspected = targetHeld
        && nameMatches(
            staging, expectedTarget,
            stagingIsExpectedTarget, inspectionError);

    if (targetInspected && stagingInspected
        && targetIsReplacement && stagingIsExpectedTarget) {
        replacement.state_->entry = target;
        expectedTarget.state_->entry = staging;
        QString syncError;
        if (!staging.parent_.sync(syncError)) {
            result.effect = MutationEffect::AppliedNotDurable;
            result.error = syncError;
            return result;
        }
        QString parentError;
        bool finalTargetMatches = false;
        bool finalStagingMatches = false;
        if (!staging.parent_.pathMatches(parentError)
            || !entryMatches(
                target, replacement,
                finalTargetMatches, parentError)
            || !entryMatches(
                staging, expectedTarget,
                finalStagingMatches, parentError)
            || !finalTargetMatches || !finalStagingMatches) {
            result.effect = MutationEffect::Partial;
            result.error = parentError.isEmpty()
                ? QStringLiteral(
                    "The anchored replacement changed after publication")
                : parentError;
            result.verifiedRecoveryPath =
                replacement.verifiedPath(target);
            return result;
        }
        result.effect = MutationEffect::AppliedDurable;
        return result;
    }

    bool stagingIsReplacement = false;
    bool targetIsExpectedTarget = false;
    QString unchangedError;
    const bool stagingChecked = replacementHeld
        && nameMatches(
            staging, replacement,
            stagingIsReplacement, unchangedError);
    const bool targetChecked = targetHeld
        && nameMatches(
            target, expectedTarget,
            targetIsExpectedTarget, unchangedError);
    if (exchangeResult != 0
        && stagingChecked && targetChecked
        && stagingIsReplacement && targetIsExpectedTarget) {
        result.error = unsupported
            ? QStringLiteral(
                "The filesystem cannot exchange anchored file names")
            : nativeError(
                QStringLiteral(
                    "Cannot exchange anchored replacement files"),
                exchangeError);
        return result;
    }

    if (exchangeResult != 0) {
        result.effect = MutationEffect::Conflict;
        result.error = inspectionError.isEmpty()
            ? QStringLiteral(
                "An anchored replacement endpoint changed before publication")
            : inspectionError;
        return result;
    }

    result.effect = MutationEffect::Partial;
    result.error = inspectionError.isEmpty()
        ? QStringLiteral(
              "An anchored replacement endpoint changed after publication; "
              "no unverified rollback was attempted")
        : inspectionError;
    QString syncError;
    if (!staging.parent_.sync(syncError)
        && !syncError.isEmpty()) {
        result.error += QStringLiteral("; ") + syncError;
    }
    result.verifiedRecoveryPath =
        replacement.verifiedPath(target);
    if (result.verifiedRecoveryPath.isEmpty()) {
        result.verifiedRecoveryPath =
            expectedTarget.verifiedPath(staging);
    }
    return result;
#elif defined(Q_OS_WIN)
    const WindowsStamp replacementStamp = replacement.state_->stamp;
    const NativeIdentity replacementIdentity = replacement.identity();
    const qint64 replacementSize = replacement.size();
    const QByteArray replacementDigest = replacement.sha256();
    const WindowsStamp targetStamp = expectedTarget.state_->stamp;
    const NativeIdentity targetIdentity = expectedTarget.identity();
    const qint64 targetSize = expectedTarget.size();
    const QByteArray targetDigest = expectedTarget.sha256();
    const auto exactFile = [](
        const PinnedFile &file,
        const NativeIdentity &identity,
        qint64 size,
        const QByteArray &digest) {
        return file.isValid()
            && file.identity() == identity
            && file.size() == size
            && file.sha256() == digest;
    };
    const auto replacementPublicationMatches = [](
        const PinnedFile &file,
        const WindowsStamp &originalReplacement,
        const WindowsStamp &originalTarget,
        qint64 size,
        const QByteArray &digest) {
        // ReplaceFileW retains the replacement file ID but preserves the
        // replaced target's creation time.
        return file.isValid()
            && sameWindowsFileId(
                file.state_->stamp, originalReplacement)
            && file.state_->stamp.created == originalTarget.created
            && file.size() == size
            && file.sha256() == digest;
    };
    const auto refreshExpectedTarget = [&expectedTarget](QString &error) {
        WindowsStamp current;
        if (!captureWindowsStamp(
                expectedTarget.state_->handle.get(),
                current, false, error)
            || !sameWindowsMoveIdentityAndData(
                current, expectedTarget.state_->stamp)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The expected replacement target changed unexpectedly");
            }
            return false;
        }
        expectedTarget.state_->stamp = current;
        expectedTarget.state_->identity = windowsIdentity(current, 'f');
        return true;
    };

    EntryRef backup = displacedTarget;
    if (backup.isValid()) {
        bool exists = true;
        if (backup.parent_.identity() != staging.parent_.identity()
            || backup.component_ == staging.component_
            || backup.component_ == target.component_
            || !entryExists(backup, exists, result.error)
            || exists) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral(
                    "The anchored replacement recovery name is unavailable");
            }
            return result;
        }
    } else {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const QString component = QStringLiteral(".gc-replace-%1.tmp")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            QString entryError;
            EntryRef candidate = staging.parent_.entry(component, entryError);
            bool exists = true;
            if (candidate.isValid()
                && entryExists(candidate, exists, entryError)
                && !exists) {
                backup = candidate;
                break;
            }
        }
    }
    if (!backup.isValid()) {
        result.error = QStringLiteral(
            "Cannot reserve an anchored replacement backup name");
        return result;
    }

    replacement.state_->handle.reset();
    replacement = {};
    reportAnchoredFilesystemTransition(
        "replace-before-publish",
        staging.displayPath_,
        target.displayPath_);
    const QString nativeTarget = extendedWindowsPath(target.displayPath_);
    const QString nativeStaging = extendedWindowsPath(staging.displayPath_);
    const QString nativeBackup = extendedWindowsPath(backup.displayPath_);
    const BOOL replaced = ::ReplaceFileW(
        reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
        reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
        reinterpret_cast<LPCWSTR>(nativeBackup.utf16()),
        0,
        nullptr,
        nullptr);
    const DWORD replaceError = replaced
        ? ERROR_SUCCESS : ::GetLastError();
    PinnedFile published;
    PinnedFile displaced;
    PinnedFile remainingStaging;
    QString pinError;
    const bool publishedPinned = pinRegularFile(
        target, published, pinError);
    const bool displacedPinned = pinRegularFile(
        backup, displaced, pinError);
    QString stagingError;
    const bool remainingStagingPinned = pinRegularFile(
        staging, remainingStaging, stagingError);
    if (!replaced) {
        bool backupExists = false;
        QString existenceError;
        const bool backupInspected = entryExists(
            backup, backupExists, existenceError);
        const bool unchanged = publishedPinned
            && remainingStagingPinned
            && exactFile(
                published,
                targetIdentity,
                targetSize,
                targetDigest)
            && exactFile(
                remainingStaging,
                replacementIdentity,
                replacementSize,
                replacementDigest)
            && backupInspected && !backupExists;
        if (unchanged) {
            replacement = std::move(remainingStaging);
            QString refreshError;
            refreshExpectedTarget(refreshError);
            result.error = windowsError(
                QStringLiteral(
                    "Cannot publish an anchored replacement"),
                replaceError);
            return result;
        }
        const bool targetUnchanged = publishedPinned
            && exactFile(
                published,
                targetIdentity,
                targetSize,
                targetDigest)
            && backupInspected && !backupExists;
        if (targetUnchanged) {
            QString refreshError;
            refreshExpectedTarget(refreshError);
            result.effect = MutationEffect::Conflict;
            result.error = windowsError(
                QStringLiteral(
                    "The anchored replacement staging file changed "
                    "before publication"),
                replaceError);
            if (!refreshError.isEmpty()) {
                result.error += QStringLiteral("; ") + refreshError;
            }
            return result;
        }
    }
    if (replaced || (publishedPinned && displacedPinned)) {
        reportAnchoredFilesystemTransition(
            "replace-published",
            staging.displayPath_,
            target.displayPath_);
    }

    const bool expectedPublication = publishedPinned && displacedPinned
        && replacementPublicationMatches(
            published,
            replacementStamp,
            targetStamp,
            replacementSize,
            replacementDigest)
        && exactFile(
            displaced,
            targetIdentity,
            targetSize,
            targetDigest);
    if (expectedPublication) {
        replacement = std::move(published);
        expectedTarget = std::move(displaced);
        QString syncError;
        if (!staging.parent_.sync(syncError)) {
            result.effect = MutationEffect::AppliedNotDurable;
            result.error = syncError;
            return result;
        }
        bool finalTargetMatches = false;
        bool finalBackupMatches = false;
        QString finalError;
        if (!staging.parent_.pathMatches(finalError)
            || !entryMatches(
                target, replacement,
                finalTargetMatches, finalError)
            || !entryMatches(
                backup, expectedTarget,
                finalBackupMatches, finalError)
            || !finalTargetMatches || !finalBackupMatches) {
            result.effect = MutationEffect::Partial;
            result.error = finalError.isEmpty()
                ? QStringLiteral(
                    "The anchored replacement changed after publication")
                : finalError;
            result.verifiedRecoveryPath =
                replacement.verifiedPath(target);
            return result;
        }
        result.effect = replaced
            ? MutationEffect::AppliedDurable
            : MutationEffect::AppliedNotDurable;
        if (!replaced) {
            result.error = windowsError(
                QStringLiteral(
                    "Windows reported an incomplete anchored replacement"),
                replaceError);
        }
        return result;
    }

    if (!publishedPinned || !displacedPinned) {
        result.effect = MutationEffect::Partial;
        result.error = pinError.isEmpty()
            ? QStringLiteral(
                "Cannot identify files after an anchored replacement")
            : pinError;
        return result;
    }

    reportAnchoredFilesystemTransition(
        "replace-before-rollback",
        target.displayPath_,
        staging.displayPath_);
    const MutationResult restoreStaging = moveNoReplace(
        published, staging);
    if (!restoreStaging.applied()) {
        result.effect = MutationEffect::Partial;
        result.error = restoreStaging.error.isEmpty()
            ? QStringLiteral(
                "Cannot retain the rejected replacement staging file")
            : restoreStaging.error;
        result.verifiedRecoveryPath =
            published.verifiedPath(target);
        return result;
    }
    const MutationResult restoreTarget = moveNoReplace(
        displaced, target);
    if (!restoreTarget.applied()) {
        result.effect = MutationEffect::Partial;
        result.error = restoreTarget.error.isEmpty()
            ? QStringLiteral(
                "Cannot restore the displaced replacement target")
            : restoreTarget.error;
        result.verifiedRecoveryPath =
            displaced.verifiedPath(backup);
        return result;
    }

    QString refreshError;
    refreshExpectedTarget(refreshError);
    const bool originalReplacementRestored =
        exactFile(
            published,
            replacementIdentity,
            replacementSize,
            replacementDigest)
        || replacementPublicationMatches(
            published,
            replacementStamp,
            targetStamp,
            replacementSize,
            replacementDigest);
    if (originalReplacementRestored) {
        replacement = std::move(published);
    }
    QString syncError;
    const bool rollbackSynced = staging.parent_.sync(syncError);
    bool restoredStagingMatches = false;
    bool restoredTargetMatches = false;
    QString rollbackError;
    const PinnedFile &restoredStaging = originalReplacementRestored
        ? replacement : published;
    const bool rollbackVerified = entryMatches(
            staging, restoredStaging,
            restoredStagingMatches, rollbackError)
        && entryMatches(
            target, displaced,
            restoredTargetMatches, rollbackError)
        && restoredStagingMatches && restoredTargetMatches;
    if (!rollbackSynced || !rollbackVerified) {
        result.effect = MutationEffect::Partial;
        result.error = !rollbackError.isEmpty()
            ? rollbackError : syncError;
        return result;
    }
    result.effect = MutationEffect::Conflict;
    result.error = QStringLiteral(
        "An anchored replacement endpoint changed before publication");
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

MutationResult moveNoReplace(
    PinnedFile &source,
    const EntryRef &destination,
    const PinnedFileReadControl &readControl)
{
    MutationResult result;
    if (!source.state_ || !destination.isValid()) {
        result.error = QStringLiteral(
            "An anchored move endpoint is unavailable");
        return result;
    }

    reportAnchoredFilesystemTransition(
        "move-before-publish",
        source.state_->entry.displayPath_,
        destination.displayPath_);
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
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
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
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }

    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            *source.state_, discard,
            verifiedDigest, result.error, readControl)) {
        result.effect = MutationEffect::Partial;
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }

    source.state_->entry = destination;
    source.state_->stamp = movedHandle;
    source.state_->identity = unixIdentity(movedHandle, 'f');
    bool finalDestinationMatches = false;
    QString finalDestinationError;
    if (!entryMatches(
            destination, source,
            finalDestinationMatches, finalDestinationError)
        || !finalDestinationMatches) {
        result.effect = MutationEffect::Partial;
        result.error = finalDestinationError.isEmpty()
            ? QStringLiteral(
                  "The anchored move destination changed after verification")
            : finalDestinationError;
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }
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
    reportAnchoredFilesystemTransition(
        "move-before-final-name-check",
        original.displayPath_,
        destination.displayPath_);
    finalDestinationMatches = false;
    finalDestinationError.clear();
    if (!entryMatches(
            destination, source,
            finalDestinationMatches, finalDestinationError)
        || !finalDestinationMatches) {
        result.effect = MutationEffect::Partial;
        result.error = finalDestinationError.isEmpty()
            ? QStringLiteral(
                  "The anchored move destination changed after synchronization")
            : finalDestinationError;
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    return result;
#elif defined(Q_OS_WIN)
    bool mutationConflict = false;
    WindowsHandle mutation = openWindowsMutationHandle(
        *source.state_, mutationConflict, result.error);
    if (!mutation.isValid()) {
        result.effect = mutationConflict
            ? MutationEffect::Conflict
            : MutationEffect::NoEffect;
        return result;
    }
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
            mutation.get(),
            FileRenameInfo,
            rename,
            DWORD(storage.size()))) {
        const DWORD native = ::GetLastError();
        const DWORD targetAttributes = ::GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(
                extendedWindowsPath(name).utf16()));
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
            mutation.get(), moved, false, result.error)
        || !sameWindowsRenameResultAndData(
            moved, source.state_->stamp)) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored file identity changed during its move");
        }
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }
    source.state_->entry = destination;
    source.state_->stamp = moved;
    source.state_->identity = windowsIdentity(moved, 'f');
    source.state_->durableGeneration = windowsDurableGeneration(moved);
    QByteArray verifiedDigest;
    const PinnedChunkConsumer discard = [](
        const char *, qsizetype, QString &) { return true; };
    if (!streamPinnedFile(
            *source.state_, discard, verifiedDigest, result.error,
            readControl)
        || verifiedDigest != source.state_->sha256) {
        result.effect = MutationEffect::Partial;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored file contents changed during its move");
        }
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }
    bool destinationMatches = false;
    WindowsStamp finalDestinationStamp;
    QString destinationError;
    reportAnchoredFilesystemTransition(
        "move-before-final-name-check",
        original.displayPath_,
        destination.displayPath_);
    if (!windowsMovedEntryMatches(
            destination, *source.state_,
            destinationMatches, finalDestinationStamp,
            destinationError)
        || !destinationMatches) {
        result.effect = MutationEffect::Partial;
        result.error = destinationError.isEmpty()
            ? QStringLiteral(
                  "The anchored move destination changed after verification")
            : destinationError;
        result.verifiedRecoveryPath =
            source.verifiedPath(destination, readControl);
        return result;
    }
    source.state_->stamp = finalDestinationStamp;
    source.state_->identity =
        windowsIdentity(finalDestinationStamp, 'f');
    source.state_->durableGeneration =
        windowsDurableGeneration(finalDestinationStamp);
    result.effect = MutationEffect::AppliedDurable;
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

QString removalQuarantineName(
    const NativeIdentity &identity,
    const QString &originalComponent)
{
    if (!identity.isValid()
        || !validPortableComponent(originalComponent)) {
        return {};
    }
    QByteArray ownership = identity.serializedKey();
    ownership += '\0';
    ownership += QFile::encodeName(originalComponent);
    return QStringLiteral(".gc-remove-%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            ownership, QCryptographicHash::Sha256).toHex()));
}

MutationResult remove(
    PinnedFile &file,
    const PinnedFileReadControl &readControl)
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
    const bool alreadyQuarantined =
        original.component_.startsWith(QStringLiteral(".gc-remove-"));
    const QString component = alreadyQuarantined
        ? original.component_
        : removalQuarantineName(
            file.state_->identity, original.component_);
    const EntryRef quarantine =
        original.parent_.entry(component, componentError);
    if (!quarantine.isValid()) {
        result.error = componentError;
        return result;
    }
    reportAnchoredFilesystemTransition(
        "remove-before-quarantine",
        original.displayPath_);
    QString preflightParentError;
    if (!original.parent_.pathMatches(preflightParentError)) {
        result.error = preflightParentError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal parent changed before quarantine")
            : preflightParentError;
        return result;
    }
    if (!alreadyQuarantined) {
        result = moveNoReplace(file, quarantine, readControl);
        if (!result.applied()) return result;
    } else {
        result.effect = MutationEffect::AppliedDurable;
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
        result.verifiedRecoveryPath =
            file.verifiedPath(file.state_->entry, readControl);
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
        result.verifiedRecoveryPath =
            file.verifiedPath(file.state_->entry, readControl);
        return result;
    }

    reportAnchoredFilesystemTransition(
        "remove-quarantine-finally-verified",
        file.state_->entry.displayPath_);

    QString parentError;
    if (!file.state_->entry.parent_.pathMatches(parentError)) {
        result.effect = MutationEffect::Partial;
        result.error = parentError.isEmpty()
            ? QStringLiteral(
                  "The anchored removal parent changed before unlinking")
            : parentError;
        result.verifiedRecoveryPath =
            file.verifiedPath(file.state_->entry, readControl);
        return result;
    }

    const QByteArray quarantineName =
        QFile::encodeName(file.state_->entry.component_);
    int unlinkResult;
    bool injectUnlinkFailure = false;
#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
    injectUnlinkFailure =
        anchoredFilesystemFileUnlinkFailureRequested(
            file.state_->entry.displayPath_);
#endif
    if (injectUnlinkFailure) {
        errno = EACCES;
        unlinkResult = -1;
    } else {
        do {
            unlinkResult = ::unlinkat(
                file.state_->entry.parent_.state_->descriptor.get(),
                quarantineName.constData(), 0);
        } while (unlinkResult != 0 && errno == EINTR);
    }
    if (unlinkResult != 0) {
        result.effect = MutationEffect::Partial;
        result.error = nativeError(
            QStringLiteral("Cannot remove an anchored quarantine file"),
            errno);
        result.verifiedRecoveryPath =
            file.verifiedPath(file.state_->entry, readControl);
        return result;
    }
    result.removalRequested = true;
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
    bool mutationConflict = false;
    WindowsHandle mutation = openWindowsMutationHandle(
        *file.state_, mutationConflict, result.error);
    if (!mutation.isValid()) {
        result.effect = mutationConflict
            ? MutationEffect::Conflict
            : MutationEffect::NoEffect;
        return result;
    }
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
    bool useLegacyDisposition = false;
#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
    useLegacyDisposition =
        anchoredFilesystemUseLegacyWindowsDelete();
#endif
    bool removalRequested = false;
    DWORD extendedError = ERROR_NOT_SUPPORTED;
    if (!useLegacyDisposition) {
        removalRequested = ::SetFileInformationByHandle(
            mutation.get(),
            dispositionInfoEx,
            &extendedDisposition,
            sizeof(extendedDisposition));
        if (!removalRequested)
            extendedError = ::GetLastError();
    }
    if (!removalRequested) {
        const bool unsupported =
            useLegacyDisposition
            || extendedError == ERROR_INVALID_PARAMETER
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
            mutation.get(),
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
    result.removalRequested = true;
    file.state_.reset();
    mutation.reset();

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
        reinterpret_cast<LPCWSTR>(
            extendedWindowsPath(original.displayPath_).utf16()),
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

MutationResult removeEmptyDirectory(DirectoryAnchor &directory)
{
    MutationResult result;
    if (!directory.state_
        || !directory.state_->ancestor
        || directory.state_->component.isEmpty()) {
        result.error = QStringLiteral(
            "The anchored directory removal target is unavailable");
        return result;
    }

    DirectoryAnchor parent(directory.state_->ancestor);
    QString matchError;
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored directory removal parent was replaced")
            : matchError;
        return result;
    }
    if (!directory.pathMatches(matchError)) {
        result.effect = MutationEffect::Conflict;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored directory removal target was replaced")
            : matchError;
        return result;
    }

#ifdef Q_OS_UNIX
    UnixStamp pinned;
    if (!captureUnixStamp(
            directory.state_->descriptor.get(),
            pinned,
            true,
            result.error)) {
        return result;
    }
    if (!sameUnixObject(pinned, directory.state_->stamp)) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The anchored directory identity changed before removal");
        return result;
    }

    UnixStamp named;
    bool exists = false;
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(),
            directory.state_->component,
            named,
            exists,
            result.error)) {
        return result;
    }
    if (!exists || !sameUnixObject(named, pinned)) {
        result.effect = MutationEffect::Conflict;
        result.error = QStringLiteral(
            "The anchored directory removal target was replaced");
        return result;
    }

    reportAnchoredFilesystemTransition(
        "remove-directory-before-quarantine",
        directory.state_->displayPath);

    const QString originalComponent = directory.state_->component;
    const QString quarantineComponent =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    const QByteArray originalName =
        QFile::encodeName(originalComponent);
    const QByteArray quarantineName =
        QFile::encodeName(quarantineComponent);
    bool unsupported = false;
    const int renameResult = renameNoReplaceNative(
        parent.state_->descriptor.get(),
        originalName,
        parent.state_->descriptor.get(),
        quarantineName,
        unsupported);
    const int renameError = errno;
    if (renameResult != 0) {
        if (renameError == EEXIST || renameError == ENOENT) {
            result.effect = MutationEffect::Conflict;
            result.error = QStringLiteral(
                "The anchored directory changed before quarantine");
        } else if (unsupported) {
            result.error = QStringLiteral(
                "The filesystem cannot quarantine an anchored directory without replacement");
        } else {
            result.error = nativeError(
                QStringLiteral(
                    "Cannot quarantine an anchored directory"),
                renameError);
        }
        return result;
    }

    const auto partialAfterQuarantine = [&parent](
        QString failure,
        const QString &recoveryPath = QString()) {
        MutationResult partial;
        partial.effect = MutationEffect::Partial;
        partial.error = std::move(failure);
        partial.verifiedRecoveryPath = recoveryPath;
        QString syncError;
        if (!parent.sync(syncError)) {
            if (!partial.error.isEmpty())
                partial.error += QStringLiteral("; ");
            partial.error += syncError;
        }
        return partial;
    };

    UnixStamp quarantined;
    bool quarantineExists = false;
    QString quarantineError;
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(),
            quarantineComponent,
            quarantined,
            quarantineExists,
            quarantineError)
        || !quarantineExists
        || !sameUnixObject(quarantined, pinned)) {
        return partialAfterQuarantine(
            quarantineError.isEmpty()
                ? QStringLiteral(
                      "The anchored directory quarantine contains an unexpected entry")
                : quarantineError);
    }

    UnixStamp pinnedAfterRename;
    if (!captureUnixStamp(
            directory.state_->descriptor.get(),
            pinnedAfterRename,
            true,
            quarantineError)
        || !sameUnixObject(pinnedAfterRename, pinned)) {
        return partialAfterQuarantine(
            quarantineError.isEmpty()
                ? QStringLiteral(
                      "The anchored directory identity changed during quarantine")
                : quarantineError);
    }

    directory.state_->component = quarantineComponent;
    directory.state_->displayPath = QDir(
        parent.state_->displayPath).filePath(quarantineComponent);
    directory.state_->stamp = pinnedAfterRename;
    reportAnchoredFilesystemTransition(
        "remove-directory-finally-verified",
        directory.state_->displayPath);

    quarantineExists = false;
    quarantineError.clear();
    if (!statDirectoryEntry(
            parent.state_->descriptor.get(),
            quarantineComponent,
            quarantined,
            quarantineExists,
            quarantineError)
        || !quarantineExists
        || !sameUnixObject(quarantined, pinnedAfterRename)) {
        return partialAfterQuarantine(
            quarantineError.isEmpty()
                ? QStringLiteral(
                      "The anchored directory quarantine was replaced")
                : quarantineError,
            directory.pathMatches(quarantineError)
                ? directory.state_->displayPath : QString());
    }

    int unlinkResult;
    do {
        unlinkResult = ::unlinkat(
            parent.state_->descriptor.get(),
            quarantineName.constData(),
            AT_REMOVEDIR);
    } while (unlinkResult != 0 && errno == EINTR);
    if (unlinkResult != 0) {
        const int unlinkError = errno;
        return partialAfterQuarantine(
            nativeError(
                QStringLiteral(
                    "Cannot remove an anchored quarantined directory"),
                unlinkError),
            directory.pathMatches(quarantineError)
                ? directory.state_->displayPath : QString());
    }
    result.removalRequested = true;

    UnixStamp unlinked;
    QString verificationError;
    const bool captured = captureUnixStamp(
        directory.state_->descriptor.get(),
        unlinked,
        true,
        verificationError);
    bool quarantineStillExists = false;
    QString postRemovalError;
    const bool quarantineInspected = entryNameExists(
        parent.state_->descriptor.get(),
        quarantineComponent,
        quarantineStillExists,
        postRemovalError);
#if defined(Q_OS_MACOS)
    // macOS reports one directory link both before and after rmdir(). The
    // successful syscall and anchored name check provide the removal proof.
    const bool finalDirectoryLinkRemoved = true;
#else
    const bool finalDirectoryLinkRemoved = unlinked.links == 0;
#endif
    const bool removedPinnedDirectory = captured
        && quarantineInspected
        && !quarantineStillExists
        && finalDirectoryLinkRemoved
        && sameUnixObject(unlinked, pinned);
    bool originalExists = false;
    QString originalError;
    const bool originalInspected = entryNameExists(
        parent.state_->descriptor.get(),
        originalComponent,
        originalExists,
        originalError);
    QString parentMatchError;
    const bool parentStillMatches = parent.pathMatches(parentMatchError);
    QString syncError;
    const bool parentSynced = parent.sync(syncError);
    if (!removedPinnedDirectory) {
        result.effect = MutationEffect::Partial;
        if (!verificationError.isEmpty()) {
            result.error = verificationError;
        } else if (!postRemovalError.isEmpty()) {
            result.error = postRemovalError;
        } else if (quarantineStillExists) {
            result.error = QStringLiteral(
                "The anchored directory quarantine name was repopulated during removal");
        } else {
            result.error = QStringLiteral(
                "The anchored directory removal did not unlink the pinned directory");
        }
        if (!parentSynced) {
            result.error += QStringLiteral("; ") + syncError;
        }
        return result;
    }
    directory.state_.reset();
    if (!originalInspected || originalExists || !parentStillMatches) {
        result.effect = MutationEffect::Partial;
        if (!originalInspected) {
            result.error = originalError;
        } else if (originalExists) {
            result.error = QStringLiteral(
                "The anchored directory's original name was repopulated during removal");
        } else {
            result.error = parentMatchError.isEmpty()
                ? QStringLiteral(
                      "The anchored directory removal parent changed before verification")
                : parentMatchError;
        }
        if (!parentSynced) {
            if (!result.error.isEmpty())
                result.error += QStringLiteral("; ");
            result.error += syncError;
        }
        return result;
    }
    if (!parentSynced) {
        result.effect = MutationEffect::AppliedNotDurable;
        result.error = syncError;
        return result;
    }
    result.effect = MutationEffect::AppliedDurable;
    return result;
#elif defined(Q_OS_WIN)
    if (directory.state_.use_count() != 1
        || directory.state_->handles.size() != 1) {
        result.error = QStringLiteral(
            "The anchored directory still has active references");
        return result;
    }

    const QString displayPath = directory.state_->displayPath;
    const QString path = extendedWindowsPath(displayPath);
    const WindowsStamp expected = directory.state_->stamp;
    const auto restoreObservation = [
        &directory, &path, &displayPath, &expected](QString failure) {
        MutationResult restoredResult;
        QString observationError;
        WindowsHandle observation = openWindowsDirectoryHandle(
            path, true, observationError);
        if (!observation.isValid()) {
            directory.state_.reset();
            restoredResult.effect = MutationEffect::Conflict;
            restoredResult.error = std::move(failure);
            if (!observationError.isEmpty()) {
                restoredResult.error += QStringLiteral("; ")
                    + observationError;
            }
            return restoredResult;
        }

        WindowsStamp restored;
        if (!captureWindowsStamp(
                observation.get(), restored, true, observationError)
            || !sameWindowsObject(restored, expected)) {
            directory.state_.reset();
            restoredResult.effect = MutationEffect::Conflict;
            restoredResult.error = std::move(failure);
            restoredResult.error += QStringLiteral("; ");
            restoredResult.error += observationError.isEmpty()
                ? QStringLiteral(
                      "The anchored directory changed while restoring its observation handle")
                : observationError;
            return restoredResult;
        }

        directory.state_->handles.push_back(std::move(observation));
        directory.state_->stamp = restored;
        directory.state_->identity = windowsIdentity(restored, 'd');
        restoredResult.error = std::move(failure);
        restoredResult.verifiedRecoveryPath = displayPath;
        return restoredResult;
    };

    directory.state_->handles.clear();
    WindowsHandle mutation(::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!mutation.isValid()) {
        const DWORD native = ::GetLastError();
        return restoreObservation(windowsError(
            QStringLiteral(
                "Cannot open an anchored directory for removal"),
            native));
    }

    WindowsStamp opened;
    if (!captureWindowsStamp(
            mutation.get(), opened, true, result.error)
        || !sameWindowsObject(opened, expected)) {
        directory.state_.reset();
        result.effect = MutationEffect::Conflict;
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "The anchored directory removal target was replaced");
        }
        return result;
    }

    reportAnchoredFilesystemTransition(
        "remove-directory-finally-verified",
        displayPath);

    struct WindowsDispositionInfoEx
    {
        DWORD flags = 0;
    };
    constexpr FILE_INFO_BY_HANDLE_CLASS dispositionInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
    constexpr DWORD dispositionDelete = 0x00000001;
    constexpr DWORD dispositionPosixSemantics = 0x00000002;
    WindowsDispositionInfoEx extendedDisposition {
        dispositionDelete | dispositionPosixSemantics};
    bool useLegacyDisposition = false;
#ifdef GC_ANCHORED_FILESYSTEM_TEST_HOOKS
    useLegacyDisposition =
        anchoredFilesystemUseLegacyWindowsDelete();
#endif
    bool removalRequested = false;
    DWORD removalError = ERROR_NOT_SUPPORTED;
    if (!useLegacyDisposition) {
        removalRequested = ::SetFileInformationByHandle(
            mutation.get(),
            dispositionInfoEx,
            &extendedDisposition,
            sizeof(extendedDisposition));
        removalError = removalRequested
            ? ERROR_SUCCESS : ::GetLastError();
    }
    const bool extendedUnsupported =
        useLegacyDisposition
        || removalError == ERROR_INVALID_PARAMETER
        || removalError == ERROR_INVALID_FUNCTION
        || removalError == ERROR_NOT_SUPPORTED
        || removalError == ERROR_CALL_NOT_IMPLEMENTED;
    if (!removalRequested && extendedUnsupported) {
        FILE_DISPOSITION_INFO disposition {};
        disposition.DeleteFile = TRUE;
        removalRequested = ::SetFileInformationByHandle(
            mutation.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition));
        removalError = removalRequested
            ? ERROR_SUCCESS : ::GetLastError();
    }
    if (!removalRequested) {
        const QString dispositionError = windowsError(
            QStringLiteral("Cannot remove an anchored empty directory"),
            removalError);
        mutation.reset();
        return restoreObservation(dispositionError);
    }

    result.removalRequested = true;
    directory.state_.reset();
    mutation.reset();
    reportAnchoredFilesystemTransition(
        "remove-directory-disposition-completed",
        displayPath);
    if (!parent.pathMatches(matchError)) {
        result.effect = MutationEffect::Partial;
        result.error = matchError.isEmpty()
            ? QStringLiteral(
                  "The anchored directory removal parent changed before verification")
            : matchError;
        return result;
    }

    WindowsHandle named(::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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
                "Cannot verify completion of an anchored directory removal"),
            native);
        return result;
    }
    WindowsStamp namedStamp;
    if (!captureWindowsStamp(
            named.get(), namedStamp, true, result.error)) {
        result.effect = MutationEffect::Partial;
        return result;
    }
    if (sameWindowsObject(namedStamp, expected)) {
        result.effect = MutationEffect::Partial;
        result.error = QStringLiteral(
            "The anchored directory removal left the original name visible");
        result.verifiedRecoveryPath =
            QDir::fromNativeSeparators(path);
        return result;
    }
    result.effect = MutationEffect::Partial;
    result.error = QStringLiteral(
        "The anchored directory's original name was repopulated during removal");
    return result;
#else
    result.error = QStringLiteral(
        "Anchored filesystem operations are unsupported on this platform");
    return result;
#endif
}

} // namespace AnchoredFileSystem
