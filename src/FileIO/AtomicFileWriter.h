/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_AtomicFileWriter_h
#define _GC_AtomicFileWriter_h

#include "AnchoredFileSystem.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
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
#include <cstdint>
#include <io.h>
#include <qt_windows.h>
#endif

#ifdef GC_DURABLE_FILESYSTEM_TEST_HOOKS
void durableFilesystemTransitionReached(const char *transition);
#endif

#ifdef GC_ATOMIC_FILE_WRITER_TEST_HOOKS
int atomicFileWriterNativeNoReplaceForcedError();
#endif

inline void reportDurableFilesystemTransition(const char *transition)
{
#ifdef GC_DURABLE_FILESYSTEM_TEST_HOOKS
    durableFilesystemTransitionReached(transition);
#else
    Q_UNUSED(transition);
#endif
}

inline bool syncFileDevice(QFileDevice &file, QString &error)
{
#ifdef Q_OS_UNIX
    const int descriptor = file.handle();
    if (descriptor < 0) {
        error = QStringLiteral(
            "Cannot sync the activity data: invalid file descriptor");
        return false;
    }
    int result;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = QStringLiteral("Cannot sync the activity data: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#elif defined(Q_OS_WIN)
    const intptr_t nativeHandle = ::_get_osfhandle(file.handle());
    if (nativeHandle == -1) {
        error = QStringLiteral(
            "Cannot sync the activity data: invalid file descriptor");
        return false;
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(nativeHandle);
    if (!::FlushFileBuffers(handle)) {
        error = QStringLiteral("Cannot sync the activity data: system error %1")
                    .arg(::GetLastError());
        return false;
    }
#else
    Q_UNUSED(file);
    Q_UNUSED(error);
#endif
    return true;
}

inline bool syncParentDirectory(const QString &path, QString &error)
{
#ifdef Q_OS_UNIX
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const QByteArray directory =
        QFile::encodeName(QFileInfo(path).absolutePath());
    const int descriptor = ::open(directory.constData(), flags);
    if (descriptor < 0) {
        error = QStringLiteral("Cannot open the activity directory for sync: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    int result;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const int syncError = errno;
    ::close(descriptor);
    if (result != 0) {
        error = QStringLiteral("Cannot sync the activity directory: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(syncError)));
        return false;
    }
#elif defined(Q_OS_WIN)
    // Windows has no documented directory-handle equivalent of fsync().
    // Durable protocols must use the write-through entry mutations below.
    Q_UNUSED(path);
    Q_UNUSED(error);
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

using AtomicDirectorySyncFunction =
    std::function<bool(const QString &path, QString &error)>;

#ifdef Q_OS_WIN
inline bool removeWindowsFilesystemEntryDurably(
    const QString &path, bool directory, QString &error)
{
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT
        | FILE_FLAG_WRITE_THROUGH
        | (directory ? FILE_FLAG_BACKUP_SEMANTICS : DWORD(0));
    const HANDLE handle = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = QStringLiteral(
                    "Cannot open the filesystem entry for durable removal: system error %1")
                    .arg(::GetLastError());
        return false;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes = {};
    if (!::GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes))) {
        const DWORD nativeError = ::GetLastError();
        ::CloseHandle(handle);
        error = QStringLiteral(
                    "Cannot inspect the filesystem entry for durable removal: system error %1")
                    .arg(nativeError);
        return false;
    }
    const bool isDirectory =
        attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (isDirectory != directory
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ::CloseHandle(handle);
        error = QStringLiteral(
            "Cannot durably remove an unsafe filesystem entry");
        return false;
    }

    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    if (!::SetFileInformationByHandle(
            handle,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
        const DWORD nativeError = ::GetLastError();
        ::CloseHandle(handle);
        error = QStringLiteral(
                    "Cannot durably remove the filesystem entry: system error %1")
                    .arg(nativeError);
        return false;
    }
    reportDurableFilesystemTransition(
        directory
            ? "directory-removal-requested"
            : "file-removal-requested");
    if (!::CloseHandle(handle)) {
        error = QStringLiteral(
                    "Cannot close the durably removed filesystem entry: system error %1")
                    .arg(::GetLastError());
        return false;
    }
    reportDurableFilesystemTransition(
        directory ? "directory-removed" : "file-removed");
    return true;
}
#endif

inline bool createDirectoryDurably(
    const QString &path,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory =
        syncParentDirectory)
{
    error.clear();
    if (!syncDirectory) {
        error = QStringLiteral(
            "Cannot create a directory without a durability barrier");
        return false;
    }

    const QFileInfo target(path);
    const QFileInfo parent(target.absolutePath());
    if (path.isEmpty() || target.exists() || target.isSymLink()) {
        error = QStringLiteral(
            "The durable directory target already exists or is unsafe");
        return false;
    }
    if (!parent.exists() || !parent.isDir() || parent.isSymLink()) {
        error = QStringLiteral(
            "The durable directory parent is unavailable or unsafe");
        return false;
    }

#ifdef Q_OS_WIN
    bool created = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        // A UUID-only name is also a valid empty pre-manifest journal. If the
        // process exits before publication, startup recovery can remove it.
        const QString temporaryPath = QDir(parent.absoluteFilePath()).filePath(
            QUuid::createUuid().toString(
                QUuid::WithoutBraces));
        if (!::CreateDirectoryW(
                reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
                nullptr)) {
            const DWORD nativeError = ::GetLastError();
            if (nativeError == ERROR_ALREADY_EXISTS
                || nativeError == ERROR_FILE_EXISTS) {
                continue;
            }
            error = QStringLiteral(
                        "Cannot create the durable directory staging entry: system error %1")
                        .arg(nativeError);
            return false;
        }
        reportDurableFilesystemTransition(
            "directory-staging-created");
        if (::MoveFileExW(
                reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
                reinterpret_cast<LPCWSTR>(path.utf16()),
                MOVEFILE_WRITE_THROUGH)) {
            created = true;
            break;
        }

        const DWORD nativeError = ::GetLastError();
        const bool stagingRemoved = ::RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(temporaryPath.utf16()));
        error = QStringLiteral(
                    "Cannot publish the durable directory entry: system error %1")
                    .arg(nativeError);
        if (!stagingRemoved) {
            error += QStringLiteral(
                "; cannot remove the directory staging entry");
        }
        return false;
    }
    if (!created) {
        error = QStringLiteral(
            "Cannot allocate a unique durable directory staging entry");
        return false;
    }
#else
    if (!QDir().mkdir(path)) {
        error = QStringLiteral("Cannot create the durable directory");
        return false;
    }
#endif
    reportDurableFilesystemTransition("directory-published");
    const bool synchronized = syncDirectory(path, error);
    if (synchronized) {
        reportDurableFilesystemTransition("parent-synchronized");
    }
    return synchronized;
}

inline bool removeFileDurably(
    const QString &path,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory =
        syncParentDirectory)
{
    error.clear();
    if (!syncDirectory) {
        error = QStringLiteral(
            "Cannot remove a file without a durability barrier");
        return false;
    }
    const QFileInfo entry(path);
    if (!entry.exists() || entry.isSymLink() || !entry.isFile()) {
        error = QStringLiteral(
            "The durable file removal target is unavailable or unsafe");
        return false;
    }

#ifdef Q_OS_WIN
    if (!removeWindowsFilesystemEntryDurably(
            path, false, error)) {
        return false;
    }
#else
    if (!QFile::remove(path)) {
        error = QStringLiteral("Cannot remove the durable file");
        return false;
    }
    reportDurableFilesystemTransition("file-removal-requested");
    reportDurableFilesystemTransition("file-removed");
#endif
    const bool synchronized = syncDirectory(path, error);
    if (synchronized) {
        reportDurableFilesystemTransition("parent-synchronized");
    }
    return synchronized;
}

inline bool removeDirectoryDurably(
    const QString &path,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory =
        syncParentDirectory)
{
    error.clear();
    if (!syncDirectory) {
        error = QStringLiteral(
            "Cannot remove a directory without a durability barrier");
        return false;
    }
    const QFileInfo entry(path);
    if (!entry.exists() || entry.isSymLink() || !entry.isDir()) {
        error = QStringLiteral(
            "The durable directory removal target is unavailable or unsafe");
        return false;
    }

#ifdef Q_OS_WIN
    if (!removeWindowsFilesystemEntryDurably(
            path, true, error)) {
        return false;
    }
#else
    if (!QDir().rmdir(path)) {
        error = QStringLiteral("Cannot remove the durable directory");
        return false;
    }
    reportDurableFilesystemTransition(
        "directory-removal-requested");
    reportDurableFilesystemTransition("directory-removed");
#endif
    const bool synchronized = syncDirectory(path, error);
    if (synchronized) {
        reportDurableFilesystemTransition("parent-synchronized");
    }
    return synchronized;
}

struct AtomicFileSnapshot
{
    qint64 size = 0;
    QByteArray digest;
};

inline bool atomicFileNameIsPortableComponent(
    const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".")
        || name == QStringLiteral("..")
        || QDir::isAbsolutePath(name)
        || QFileInfo(name).fileName() != name
        || name.endsWith(QLatin1Char(' '))
        || name.endsWith(QLatin1Char('.'))
        || name.toUtf8().size() > 240) {
        return false;
    }
    static const QString forbidden =
        QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : name) {
        const ushort value = character.unicode();
        if (value <= 0x1f || value == 0x7f
            || forbidden.contains(character)) {
            return false;
        }
    }

    const int dot = name.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? name : name.left(dot))
        .toUpper();
    static const QSet<QString> reserved = {
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

inline bool captureAtomicFileSnapshot(
    const QString &path, AtomicFileSnapshot &snapshot, QString &error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        error = QStringLiteral(
            "The source activity is unavailable or unsafe");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read the source activity: %1")
                    .arg(file.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 size = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            error = QStringLiteral("Cannot read the source activity: %1")
                        .arg(file.errorString());
            return false;
        }
        size += chunk.size();
        hash.addData(chunk);
    }
    snapshot.size = size;
    snapshot.digest = hash.result();
    return true;
}

inline bool atomicFileMatchesSnapshot(
    const QString &path, const AtomicFileSnapshot &expected, QString &error)
{
    AtomicFileSnapshot current;
    if (!captureAtomicFileSnapshot(path, current, error)) {
        return false;
    }
    if (current.size != expected.size
        || current.digest != expected.digest) {
        error = QStringLiteral(
            "The source activity changed while it was being saved");
        return false;
    }
    return true;
}

class AtomicFileWriter
{
public:
    virtual ~AtomicFileWriter() = default;

    virtual bool open() = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual bool flush() = 0;
    virtual bool commit() = 0;
    virtual void cancelWriting() = 0;
    virtual QString errorString() const = 0;
    // Unverified commits are observed but never rolled back by pathname.
    virtual bool verifiesCommittedResult() const { return false; }
    virtual QString temporaryPath() const { return {}; }
};

enum class AtomicFileMode {
    ReplaceExisting,
    CreateNew
};

using AtomicFileWriterFactory = std::function<std::unique_ptr<AtomicFileWriter>(
    const QString &path, AtomicFileMode mode)>;
using AtomicPublishFunction = std::function<bool(
    const QString &stagingPath, const QString &targetPath,
    bool &targetPublished, QString &error)>;
using PinnedAtomicPublishFunction = std::function<bool(
    AnchoredFileSystem::PinnedFile &staging,
    const AnchoredFileSystem::EntryRef &target,
    bool &targetPublished,
    QString &error)>;
using AtomicFinalizeFunction = std::function<bool(QString &error)>;
using AtomicPreCommitValidation =
    std::function<bool(QString &error)>;
using AtomicMoveFunction = std::function<bool(
    const QString &sourcePath, const QString &targetPath, QString &error)>;

class ReplaceAtomicFileWriter final : public AtomicFileWriter
{
public:
    explicit ReplaceAtomicFileWriter(
        const QString &targetPath,
        qint64 expectedSize = -1,
        QByteArray expectedDigest = {})
        : targetPath_(targetPath),
          file_(std::make_unique<QTemporaryFile>(
              QDir(QFileInfo(targetPath).absolutePath()).filePath(
                      QStringLiteral(".%1.XXXXXX.tmp")
                      .arg(QFileInfo(targetPath).fileName())))),
          expectedSize_(expectedSize),
          expectedDigest_(std::move(expectedDigest))
    {
    }

    bool open() override
    {
        error_.clear();
        const QString parentPath = QFileInfo(targetPath_).absolutePath();
        if (!AnchoredFileSystem::DirectoryAnchor::open(
                parentPath, parent_, error_)
            || !parent_.pathMatches(error_)) {
            error_ = QStringLiteral(
                "Cannot anchor the atomic replacement directory: %1")
                         .arg(error_);
            return false;
        }
        targetEntry_ = parent_.entry(
            QFileInfo(targetPath_).fileName(), error_);
        if (!targetEntry_.isValid()
            || !AnchoredFileSystem::pinRegularFile(
                targetEntry_, expectedTarget_, error_)) {
            error_ = QStringLiteral(
                "Cannot pin the existing atomic replacement target: %1")
                         .arg(error_);
            return false;
        }
        if (expectedSize_ >= 0
            && (expectedTarget_.size() != expectedSize_
                || expectedTarget_.sha256() != expectedDigest_)) {
            error_ = QStringLiteral(
                "The existing atomic replacement target changed");
            return false;
        }
        if (!file_ || !file_->open()) {
            error_ = file_
                ? file_->errorString()
                : QStringLiteral(
                      "Atomic replacement is no longer available");
            return false;
        }
        temporaryPath_ = file_->fileName();
        stagedHash_.reset();
        stagedSize_ = 0;
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        if (!file_) return -1;
        const qint64 written = file_->write(data);
        if (written <= 0) return written;
        if (stagedSize_ > std::numeric_limits<qint64>::max() - written) {
            error_ = QStringLiteral(
                "The atomic replacement is unexpectedly large");
            return -1;
        }
        stagedHash_.addData(
            QByteArrayView(data.constData(), qsizetype(written)));
        stagedSize_ += written;
        return written;
    }

    bool flush() override
    {
        if (!file_ || !file_->flush()) {
            return false;
        }
        error_.clear();
        return syncFileDevice(*file_, error_);
    }

    bool commit() override
    {
        if (!file_ || temporaryPath_.isEmpty()
            || !parent_.isValid() || !targetEntry_.isValid()
            || !expectedTarget_.isValid()) {
            error_ = QStringLiteral(
                "Atomic replacement is not open");
            return false;
        }

        AnchoredFileSystem::EntryRef stagingEntry = parent_.entry(
            QFileInfo(temporaryPath_).fileName(), error_);
        if (!stagingEntry.isValid()) {
            error_ = QStringLiteral(
                "Cannot reference the atomic replacement staging file: %1")
                         .arg(error_);
            return false;
        }

        AnchoredFileSystem::PinnedFile staging;
        AnchoredFileSystem::WriterPinHandoffState handoff;
        QString anchorError;
        if (!AnchoredFileSystem::pinRegularFileAfterWriterRelease(
                stagingEntry,
                file_->handle(),
                stagedSize_,
                stagedHash_.result(),
                [this]() {
                    file_->setAutoRemove(false);
                    file_.reset();
                },
                staging,
                handoff,
                anchorError)) {
            error_ = QStringLiteral(
                "Cannot pin the atomic replacement staging file: %1")
                         .arg(anchorError);
            if (handoff.writerReleased) {
                error_ += QStringLiteral(
                    "; writer released; staging retained for recovery");
            }
            return false;
        }

        const auto appendError = [this](const QString &detail) {
            if (detail.isEmpty()) return;
            if (!error_.isEmpty()) error_ += QStringLiteral("; ");
            error_ += detail;
        };
        const auto removePinned = [&appendError](
                AnchoredFileSystem::PinnedFile &file,
                const AnchoredFileSystem::DirectoryAnchor &parent,
                const QString &failure) {
            const AnchoredFileSystem::MutationResult removal =
                AnchoredFileSystem::remove(file);
            if (removal.effect
                == AnchoredFileSystem::MutationEffect::AppliedDurable) {
                return true;
            }
            if (removal.effect
                == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
                QString syncError;
                if (parent.sync(syncError)) return true;
                appendError(syncError);
            }
            appendError(removal.error.isEmpty() ? failure : removal.error);
            if (!removal.verifiedRecoveryPath.isEmpty()) {
                appendError(QStringLiteral("recovery file retained at %1")
                    .arg(removal.verifiedRecoveryPath));
            }
            return false;
        };

        error_.clear();
        const AnchoredFileSystem::MutationResult publication =
            AnchoredFileSystem::replaceExisting(
                staging, expectedTarget_);
        if (publication.effect
            == AnchoredFileSystem::MutationEffect::AppliedDurable) {
            return removePinned(
                expectedTarget_,
                parent_,
                QStringLiteral(
                    "cannot remove the previous atomic target"));
        }

        error_ = publication.error.isEmpty()
            ? QStringLiteral("Cannot publish the atomic replacement")
            : publication.error;
        if (!publication.verifiedRecoveryPath.isEmpty()) {
            appendError(QStringLiteral("recovery file retained at %1")
                .arg(publication.verifiedRecoveryPath));
        }
        if (publication.effect
                == AnchoredFileSystem::MutationEffect::NoEffect
            || publication.effect
                == AnchoredFileSystem::MutationEffect::Conflict) {
            removePinned(
                staging,
                parent_,
                QStringLiteral(
                    "cannot remove the rejected replacement staging file"));
        }
        return false;
    }

    void cancelWriting() override
    {
        file_.reset();
    }

    QString errorString() const override
    {
        if (!error_.isEmpty())
            return error_;
        return file_ ? file_->errorString() : QString();
    }

    QString temporaryPath() const override
    {
        return file_ ? file_->fileName() : temporaryPath_;
    }

    bool verifiesCommittedResult() const override { return true; }

private:
    QString targetPath_;
    std::unique_ptr<QTemporaryFile> file_;
    QString temporaryPath_;
    QString error_;
    AnchoredFileSystem::DirectoryAnchor parent_;
    AnchoredFileSystem::EntryRef targetEntry_;
    AnchoredFileSystem::PinnedFile expectedTarget_;
    QCryptographicHash stagedHash_{QCryptographicHash::Sha256};
    qint64 stagedSize_ = 0;
    qint64 expectedSize_ = -1;
    QByteArray expectedDigest_;
};

#ifdef Q_OS_UNIX
inline int atomicFileWriterNativeNoReplace(
    const QByteArray &temporary,
    const QByteArray &target,
    bool &unsupported)
{
    unsupported = false;
#if defined(Q_OS_LINUX) && defined(SYS_renameat2) \
    && defined(RENAME_NOREPLACE)
    int result = -1;
#ifdef GC_ATOMIC_FILE_WRITER_TEST_HOOKS
    const int forcedError =
        atomicFileWriterNativeNoReplaceForcedError();
    if (forcedError != 0) {
        errno = forcedError;
    } else
#endif
    {
        do {
            result = int(::syscall(
                SYS_renameat2,
                AT_FDCWD,
                temporary.constData(),
                AT_FDCWD,
                target.constData(),
                RENAME_NOREPLACE));
        } while (result != 0 && errno == EINTR);
    }
    if (result == 0) return 0;
    if (errno == ENOSYS || errno == EINVAL
#ifdef EOPNOTSUPP
        || errno == EOPNOTSUPP
#endif
    ) {
        unsupported = true;
    }
    return -1;
#elif defined(Q_OS_MACOS) && defined(RENAME_EXCL)
    int result;
    do {
        result = ::renameatx_np(
            AT_FDCWD,
            temporary.constData(),
            AT_FDCWD,
            target.constData(),
            RENAME_EXCL);
    } while (result != 0 && errno == EINTR);
    if (result == 0) return 0;
    unsupported = errno == ENOTSUP || errno == EINVAL;
    return -1;
#else
    Q_UNUSED(temporary)
    Q_UNUSED(target)
    unsupported = true;
    errno = ENOSYS;
    return -1;
#endif
}
#endif

inline bool publishAtomicNew(const QString &temporaryPath,
                             const QString &targetPath,
                             bool &temporaryMoved,
                             QString &error)
{
    temporaryMoved = false;
#ifdef Q_OS_UNIX
    const QByteArray temporary = QFile::encodeName(temporaryPath);
    const QByteArray target = QFile::encodeName(targetPath);
    bool nativeUnsupported = false;
    if (atomicFileWriterNativeNoReplace(
            temporary, target, nativeUnsupported) == 0) {
        temporaryMoved = true;
        return true;
    }
    const int nativeError = errno;
    if (!nativeUnsupported) {
        error = nativeError == EEXIST
            ? QStringLiteral("The target activity was created concurrently")
            : QStringLiteral("Cannot publish the new activity file: %1")
                  .arg(QString::fromLocal8Bit(
                      std::strerror(nativeError)));
        return false;
    }

    int result;
    do {
        result = ::link(temporary.constData(), target.constData());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = errno == EEXIST
            ? QStringLiteral("The target activity was created concurrently")
            : QStringLiteral("Cannot publish the new activity file: %1")
                  .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    temporaryMoved = true;
    do {
        result = ::unlink(temporary.constData());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = QStringLiteral("Cannot remove the activity staging file: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#elif defined(Q_OS_WIN)
    if (!::MoveFileExW(
            reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
            reinterpret_cast<LPCWSTR>(targetPath.utf16()),
            MOVEFILE_WRITE_THROUGH)) {
        const DWORD nativeError = ::GetLastError();
        error = nativeError == ERROR_ALREADY_EXISTS
                || nativeError == ERROR_FILE_EXISTS
            ? QStringLiteral("The target activity was created concurrently")
            : QStringLiteral(
                  "Cannot publish the new activity file: system error %1")
                  .arg(nativeError);
        return false;
    }
    temporaryMoved = true;
#else
    Q_UNUSED(temporaryPath);
    Q_UNUSED(targetPath);
    error = QStringLiteral(
        "Atomic new activity publication is unsupported on this platform");
    return false;
#endif
    return true;
}

inline bool publishPinnedAtomicNew(
    AnchoredFileSystem::PinnedFile &staging,
    const AnchoredFileSystem::EntryRef &target,
    bool &targetPublished,
    QString &error)
{
    targetPublished = false;
    error.clear();
    const AnchoredFileSystem::MutationResult result =
        AnchoredFileSystem::moveNoReplace(staging, target);
    if (result.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        targetPublished = true;
        return true;
    }

    targetPublished = result.applied()
        || (!result.verifiedRecoveryPath.isEmpty()
            && QDir::cleanPath(result.verifiedRecoveryPath)
                == QDir::cleanPath(target.displayPath()));
    if (result.effect == AnchoredFileSystem::MutationEffect::Conflict
        && result.error.startsWith(QStringLiteral(
            "The anchored move destination already exists"))) {
        error = QStringLiteral(
            "The target activity was created concurrently");
    } else {
        error = result.error.isEmpty()
            ? QStringLiteral("Cannot publish the pinned activity file")
            : result.error;
    }
    return false;
}

inline bool moveAtomicFile(
    const QString &sourcePath,
    const QString &targetPath,
    QString &error)
{
    error.clear();
    bool targetCreated = false;
    QString moveError;
    if (publishAtomicNew(
            sourcePath, targetPath, targetCreated, moveError)) {
        return true;
    }

    error = moveError.isEmpty()
        ? QStringLiteral("Cannot move the activity file atomically")
        : moveError;
    if (targetCreated) {
        const QFileInfo source(sourcePath);
        if ((!source.exists() && !source.isSymLink())
            || !QFile::remove(targetPath)) {
            if (!error.isEmpty()) error += QStringLiteral("; ");
            error += QStringLiteral(
                "cannot roll back the partial activity move");
        }
    }
    return false;
}

class NewAtomicFileWriter final : public AtomicFileWriter
{
public:
    explicit NewAtomicFileWriter(
        const QString &targetPath,
        PinnedAtomicPublishFunction publish = publishPinnedAtomicNew)
        : targetPath_(targetPath),
          file_(std::make_unique<QTemporaryFile>(
              QDir(QFileInfo(targetPath).absolutePath()).filePath(
                  QStringLiteral(".%1.XXXXXX.tmp")
                      .arg(QFileInfo(targetPath).fileName())))),
          publish_(std::move(publish))
    {
    }

    bool open() override
    {
        if (targetExists()) {
            error_ = QStringLiteral("The target activity already exists");
            return false;
        }
        if (!file_ || !file_->open()) {
            error_ = file_
                ? file_->errorString()
                : QStringLiteral(
                      "Atomic publication is no longer available");
            return false;
        }
        temporaryPath_ = file_->fileName();
        stagedHash_.reset();
        stagedSize_ = 0;
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        if (!file_) return -1;
        const qint64 written = file_->write(data);
        if (written <= 0) return written;
        if (stagedSize_ > std::numeric_limits<qint64>::max() - written) {
            error_ = QStringLiteral(
                "The atomic publication is unexpectedly large");
            return -1;
        }
        stagedHash_.addData(
            QByteArrayView(data.constData(), qsizetype(written)));
        stagedSize_ += written;
        return written;
    }

    bool flush() override
    {
        if (!file_ || !file_->flush()) {
            return false;
        }
        error_.clear();
        return syncFileDevice(*file_, error_);
    }

    bool commit() override
    {
        if (!file_ || temporaryPath_.isEmpty()) {
            error_ = QStringLiteral(
                "Atomic publication is not open");
            return false;
        }

        AnchoredFileSystem::DirectoryAnchor parent;
        AnchoredFileSystem::EntryRef stagingEntry;
        AnchoredFileSystem::EntryRef targetEntry;
        AnchoredFileSystem::PinnedFile stagingPin;
        QString anchorError;
        const QString parentPath = QFileInfo(targetPath_).absolutePath();
        if (!AnchoredFileSystem::DirectoryAnchor::open(
                parentPath, parent, anchorError)
            || !parent.pathMatches(anchorError)) {
            error_ = QStringLiteral(
                "Cannot anchor the atomic publication directory: %1")
                         .arg(anchorError);
            return false;
        }
        stagingEntry = parent.entry(
            QFileInfo(temporaryPath_).fileName(), anchorError);
        targetEntry = parent.entry(
            QFileInfo(targetPath_).fileName(), anchorError);
        if (!stagingEntry.isValid() || !targetEntry.isValid()) {
            error_ = QStringLiteral(
                "Cannot pin the atomic publication staging file: %1")
                         .arg(anchorError);
            return false;
        }

        AnchoredFileSystem::WriterPinHandoffState handoff;
        const bool stagingPinned =
            AnchoredFileSystem::pinRegularFileAfterWriterRelease(
                stagingEntry,
                file_->handle(),
                stagedSize_,
                stagedHash_.result(),
                [this]() {
                    file_->setAutoRemove(false);
                    file_.reset();
                },
                stagingPin,
                handoff,
                anchorError);
        if (!stagingPinned) {
            error_ = QStringLiteral(
                "Cannot pin the atomic publication staging file: %1")
                         .arg(anchorError);
            if (handoff.writerReleased) {
                error_ += QStringLiteral(
                    "; writer released; no identity-verified "
                    "recovery path is available");
            }
            return false;
        }
        QString finalParentError;
        if (!parent.pathMatches(finalParentError)) {
            error_ = finalParentError.isEmpty()
                ? QStringLiteral(
                      "The atomic publication directory was replaced")
                : QStringLiteral(
                      "Cannot verify the atomic publication directory: %1")
                      .arg(finalParentError);
            error_ += QStringLiteral(
                "; writer released; no identity-verified "
                "recovery path is available");
            return false;
        }

        bool targetPublished = false;
        const bool published = publish_ && publish_(
            stagingPin, targetEntry,
            targetPublished, error_);
        const auto appendError = [&](const QString &detail) {
            if (detail.isEmpty()) return;
            if (!error_.isEmpty()) error_ += QStringLiteral("; ");
            error_ += detail;
        };
        const auto removePinned = [&parent, &appendError](
                AnchoredFileSystem::PinnedFile &file,
                const QString &failure) {
            const AnchoredFileSystem::MutationResult removal =
                AnchoredFileSystem::remove(file);
            if (removal.effect
                == AnchoredFileSystem::MutationEffect::AppliedDurable) {
                return true;
            }
            if (removal.effect
                == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
                QString syncError;
                if (parent.sync(syncError)) return true;
                appendError(syncError);
            }
            appendError(removal.error.isEmpty() ? failure : removal.error);
            if (!removal.verifiedRecoveryPath.isEmpty()) {
                appendError(QStringLiteral("recovery file retained at %1")
                                .arg(removal.verifiedRecoveryPath));
            }
            return false;
        };

        AnchoredFileSystem::PinnedFile targetPin;
        bool targetOwned = false;
        if (targetPublished) {
            QString targetError;
            bool targetExists = false;
            if (!AnchoredFileSystem::entryExists(
                    targetEntry, targetExists, targetError)) {
                appendError(targetError);
            } else if (!targetExists) {
                appendError(QStringLiteral(
                    "the partially published activity is unavailable"));
            } else if (!AnchoredFileSystem::pinRegularFile(
                           targetEntry, targetPin, targetError)) {
                appendError(QStringLiteral(
                    "cannot verify the partially published activity: %1")
                                .arg(targetError));
            } else {
                targetOwned =
                    targetPin.identity() == stagingPin.identity()
                    && targetPin.size() == stagingPin.size()
                    && targetPin.sha256() == stagingPin.sha256();
                if (!targetOwned) {
                    appendError(QStringLiteral(
                        "the partially published activity was replaced and retained"));
                }
            }
        }

        if (published && targetPublished && targetOwned) {
            QString publishedParentError;
            if (parent.pathMatches(publishedParentError)) return true;
            appendError(publishedParentError.isEmpty()
                ? QStringLiteral(
                      "the atomic publication directory was replaced")
                : QStringLiteral(
                      "cannot verify the atomic publication directory: %1")
                      .arg(publishedParentError));
        }
        if (!publish_ && error_.isEmpty()) {
            error_ = QStringLiteral("Cannot publish the new activity file");
        } else if (published && !targetPublished && error_.isEmpty()) {
            error_ = QStringLiteral(
                "The activity publisher did not create the target");
        } else if (published && targetPublished && error_.isEmpty()) {
            error_ = QStringLiteral(
                "The published activity identity could not be verified");
        }

        if (targetOwned) {
            removePinned(
                targetPin,
                QStringLiteral(
                    "cannot remove the partially published activity"));
        }

        QString stagingError;
        bool stagingExists = false;
        if (!AnchoredFileSystem::entryExists(
                stagingEntry, stagingExists, stagingError)) {
            appendError(stagingError);
        } else if (stagingExists) {
            bool stagingMatches = false;
            if (!AnchoredFileSystem::entryMatches(
                    stagingEntry,
                    stagingPin,
                    stagingMatches,
                    stagingError)) {
                appendError(stagingError);
            } else if (!stagingMatches) {
                appendError(QStringLiteral(
                    "the activity staging file was replaced and retained"));
            } else {
                removePinned(
                    stagingPin,
                    QStringLiteral(
                        "cannot remove the activity staging file"));
            }
        }
        return false;
    }

    void cancelWriting() override
    {
        file_.reset();
    }

    QString errorString() const override
    {
        if (!error_.isEmpty())
            return error_;
        return file_ ? file_->errorString() : QString();
    }

    bool verifiesCommittedResult() const override { return true; }

    QString temporaryPath() const override
    {
        return file_ ? file_->fileName() : temporaryPath_;
    }

private:
    bool targetExists() const
    {
        const QFileInfo target(targetPath_);
        return target.exists() || target.isSymLink();
    }

    QString targetPath_;
    std::unique_ptr<QTemporaryFile> file_;
    QString temporaryPath_;
    PinnedAtomicPublishFunction publish_;
    QString error_;
    QCryptographicHash stagedHash_{QCryptographicHash::Sha256};
    qint64 stagedSize_ = 0;
};

inline AtomicFileWriterFactory qSaveFileWriterFactory()
{
    return [](const QString &path, AtomicFileMode mode) {
        if (mode == AtomicFileMode::CreateNew) {
            return std::unique_ptr<AtomicFileWriter>(
                new NewAtomicFileWriter(path));
        }
        return std::unique_ptr<AtomicFileWriter>(
            new ReplaceAtomicFileWriter(path));
    };
}

inline QString atomicFileError(const QString &operation,
                               const AtomicFileWriter &writer)
{
    const QString detail = writer.errorString();
    return detail.isEmpty()
        ? operation
        : QStringLiteral("%1: %2").arg(operation, detail);
}

inline void appendAtomicFileError(QString &error, const QString &detail)
{
    if (!error.isEmpty()) {
        error += QStringLiteral("; ");
    }
    error += detail;
}

using StagedFilePublication = QPair<QString, QString>;

inline QString atomicFileLockPath(const QString &path)
{
    const QFileInfo target(path);
    return target.absoluteDir().filePath(
        QStringLiteral(".%1.lock").arg(target.fileName()));
}

inline bool atomicFileLockTargetName(
    const QString &entryName, QString &targetName)
{
    QString lockName = entryName;
    const QString removalGuardSuffix = QStringLiteral(".rmlock");
    while (lockName.endsWith(removalGuardSuffix)) {
        lockName.chop(removalGuardSuffix.size());
    }
    if (lockName.size() <= 6
        || !lockName.startsWith(QLatin1Char('.'))
        || !lockName.endsWith(QStringLiteral(".lock"))) {
        targetName.clear();
        return false;
    }
    targetName = lockName.mid(1, lockName.size() - 6);
    return true;
}

inline QString atomicFilePathKey(const QString &path)
{
    QString key = QFileInfo(path).absoluteFilePath();
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    key = key.normalized(QString::NormalizationForm_C).toCaseFolded();
#endif
    return key;
}

class AtomicFileLockSet
{
public:
    bool lock(const QStringList &paths, QString &error)
    {
        error.clear();
        QSet<QString> lockKeys;
        QStringList lockPaths;
        for (const QString &path : paths) {
            if (path.isEmpty()) {
                error = QStringLiteral(
                    "Cannot lock an activity without a file name");
                return false;
            }
            const QString lockPath = atomicFileLockPath(
                QFileInfo(path).absoluteFilePath());
            const QString lockKey = atomicFilePathKey(lockPath);
            if (!lockKeys.contains(lockKey)) {
                lockKeys.insert(lockKey);
                lockPaths.append(lockPath);
            }
        }

        std::sort(lockPaths.begin(), lockPaths.end());
        for (const QString &lockPath : std::as_const(lockPaths)) {
            std::unique_ptr<QLockFile> lock(new QLockFile(lockPath));
            lock->setStaleLockTime(0);
            if (!lock->tryLock(0)) {
                locks_.clear();
                error = QStringLiteral(
                    "An activity file is already being saved");
                return false;
            }
            locks_.push_back(std::move(lock));
        }
        return true;
    }

private:
    std::vector<std::unique_ptr<QLockFile>> locks_;
};

inline bool publishStagedFileSet(
    const QList<StagedFilePublication> &files,
    QString &error,
    const AtomicPublishFunction &publish = publishAtomicNew,
    const AtomicFinalizeFunction &finalize = AtomicFinalizeFunction())
{
    error.clear();
    if (files.isEmpty()) {
        error = QStringLiteral("No staged activity files to publish");
        return false;
    }

    QSet<QString> stagingKeys;
    QSet<QString> targetKeys;
    QStringList targets;
    bool duplicateStaging = false;
    bool duplicateTarget = false;
    for (const StagedFilePublication &file : files) {
        if (file.first.isEmpty() || file.second.isEmpty()) {
            error = QStringLiteral("An activity publication path is empty");
            return false;
        }

        const QString stagingKey = atomicFilePathKey(file.first);
        const QString targetKey = atomicFilePathKey(file.second);
        duplicateStaging = duplicateStaging
            || stagingKeys.contains(stagingKey);
        duplicateTarget = duplicateTarget
            || targetKeys.contains(targetKey);
        stagingKeys.insert(stagingKey);
        targetKeys.insert(targetKey);
        targets.append(QFileInfo(file.second).absoluteFilePath());
    }

    if (duplicateStaging) {
        error = QStringLiteral("Duplicate activity staging path");
        return false;
    }
    for (const QString &stagingKey : std::as_const(stagingKeys)) {
        if (targetKeys.contains(stagingKey)) {
            error = QStringLiteral(
                "Activity staging and target paths overlap");
            return false;
        }
    }

    struct StagedInput
    {
        StagedFilePublication paths;
        AnchoredFileSystem::DirectoryAnchor stagingParent;
        AnchoredFileSystem::DirectoryAnchor targetParent;
        AnchoredFileSystem::EntryRef stagingEntry;
        AnchoredFileSystem::EntryRef targetEntry;
        AnchoredFileSystem::PinnedFile staged;
    };
    struct PublishedTarget
    {
        AnchoredFileSystem::DirectoryAnchor parent;
        AnchoredFileSystem::PinnedFile file;
    };
    std::vector<StagedInput> stagedInputs;
    std::vector<PublishedTarget> published;
    stagedInputs.reserve(static_cast<std::size_t>(files.size()));
    published.reserve(static_cast<std::size_t>(files.size()));

    const auto removePinned = [
            &error](
            AnchoredFileSystem::PinnedFile &file,
            const AnchoredFileSystem::DirectoryAnchor &parent,
            const QString &failure,
            const QString &conflict) {
        const AnchoredFileSystem::MutationResult removal =
            AnchoredFileSystem::remove(file);
        if (removal.effect
            == AnchoredFileSystem::MutationEffect::AppliedDurable) {
            return;
        }
        if (removal.effect
            == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
            QString syncError;
            if (parent.sync(syncError)) return;
            if (!removal.error.isEmpty()) {
                appendAtomicFileError(error, removal.error);
            }
            appendAtomicFileError(error, syncError);
            return;
        }
        appendAtomicFileError(
            error,
            removal.effect == AnchoredFileSystem::MutationEffect::Conflict
                ? conflict
                : (removal.error.isEmpty() ? failure : removal.error));
        if (!removal.verifiedRecoveryPath.isEmpty()) {
            appendAtomicFileError(
                error,
                QStringLiteral("recovery file retained at %1")
                    .arg(removal.verifiedRecoveryPath));
        }
    };
    const auto cleanupStaging = [&]() {
        for (StagedInput &input : stagedInputs) {
            bool exists = false;
            QString cleanupError;
            if (!AnchoredFileSystem::entryExists(
                    input.stagingEntry, exists, cleanupError)) {
                appendAtomicFileError(error, cleanupError);
                continue;
            }
            if (!exists) continue;

            bool matches = false;
            if (!AnchoredFileSystem::entryMatches(
                    input.stagingEntry,
                    input.staged,
                    matches,
                    cleanupError)) {
                appendAtomicFileError(error, cleanupError);
                continue;
            }
            if (!matches) {
                appendAtomicFileError(
                    error,
                    QStringLiteral(
                        "the activity staging file was replaced and retained"));
                continue;
            }
            removePinned(
                input.staged,
                input.stagingParent,
                QStringLiteral("cannot remove an activity staging file"),
                QStringLiteral(
                    "the activity staging file was replaced and retained"));
        }
    };
    const auto cleanup = [&]() {
        for (PublishedTarget &target : published) {
            removePinned(
                target.file,
                target.parent,
                QStringLiteral(
                    "cannot remove a partially published activity"),
                QStringLiteral(
                    "the partially published activity was replaced and retained"));
        }
        cleanupStaging();
    };

    for (const StagedFilePublication &file : files) {
        StagedInput input;
        input.paths = file;
        QString anchorError;
        if (!AnchoredFileSystem::DirectoryAnchor::open(
                QFileInfo(file.first).absolutePath(),
                input.stagingParent,
                anchorError)
            || !AnchoredFileSystem::DirectoryAnchor::open(
                QFileInfo(file.second).absolutePath(),
                input.targetParent,
                anchorError)
            || !input.stagingParent.pathMatches(anchorError)
            || !input.targetParent.pathMatches(anchorError)) {
            appendAtomicFileError(
                error,
                anchorError.isEmpty()
                    ? QStringLiteral(
                          "cannot anchor a staged activity publication")
                    : anchorError);
            cleanupStaging();
            return false;
        }
        input.stagingEntry = input.stagingParent.entry(
            QFileInfo(file.first).fileName(), anchorError);
        input.targetEntry = input.targetParent.entry(
            QFileInfo(file.second).fileName(), anchorError);
        if (!input.stagingEntry.isValid()
            || !input.targetEntry.isValid()
            || !AnchoredFileSystem::pinRegularFile(
                input.stagingEntry, input.staged, anchorError)) {
            appendAtomicFileError(
                error,
                anchorError.isEmpty()
                    ? QStringLiteral("A staged activity file is unavailable")
                    : anchorError);
            cleanupStaging();
            return false;
        }
        stagedInputs.push_back(std::move(input));
    }

    if (!publish) {
        error = QStringLiteral("No staged activity files to publish");
        cleanupStaging();
        return false;
    }

    if (duplicateTarget) {
        error = QStringLiteral("Duplicate activity publication target");
        cleanupStaging();
        return false;
    }

    std::sort(targets.begin(), targets.end());
    std::vector<std::unique_ptr<QLockFile>> locks;
    locks.reserve(static_cast<std::size_t>(targets.size()));
    for (const QString &target : targets) {
        std::unique_ptr<QLockFile> lock(
            new QLockFile(atomicFileLockPath(target)));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            error = QStringLiteral(
                "An activity publication target is already being saved");
            cleanupStaging();
            return false;
        }
        locks.push_back(std::move(lock));
    }

    for (const StagedInput &input : stagedInputs) {
        bool targetExists = false;
        QString targetError;
        if (!AnchoredFileSystem::entryExists(
                input.targetEntry, targetExists, targetError)) {
            appendAtomicFileError(error, targetError);
            cleanupStaging();
            return false;
        }
        if (targetExists) {
            error = QStringLiteral(
                "An activity publication target already exists");
            cleanupStaging();
            return false;
        }
    }

    const auto capturePublishedTarget = [
            &published](StagedInput &input, QString &targetError) {
        AnchoredFileSystem::PinnedFile target;
        if (!AnchoredFileSystem::pinRegularFile(
                input.targetEntry, target, targetError)
            || target.identity() != input.staged.identity()
            || target.size() != input.staged.size()
            || target.sha256() != input.staged.sha256()) {
            if (targetError.isEmpty()) {
                targetError = QStringLiteral(
                    "the published activity identity is ambiguous and was "
                    "retained");
            }
            return false;
        }
        published.push_back(PublishedTarget{
            input.targetParent,
            std::move(target)});
        return true;
    };

    for (StagedInput &input : stagedInputs) {
        bool stagingMatches = false;
        QString publicationError;
        if (!AnchoredFileSystem::entryMatches(
                input.stagingEntry,
                input.staged,
                stagingMatches,
                publicationError)
            || !stagingMatches
            || !input.stagingParent.pathMatches(publicationError)
            || !input.targetParent.pathMatches(publicationError)) {
            if (publicationError.isEmpty()) {
                publicationError = QStringLiteral(
                    "a staged activity changed before publication");
            }
            appendAtomicFileError(error, publicationError);
            cleanup();
            return false;
        }

        bool targetPublished = false;
        QString publishError;
        if (!publish(input.paths.first, input.paths.second,
                     targetPublished, publishError)) {
            if (targetPublished) {
                QString targetError;
                if (!capturePublishedTarget(input, targetError)) {
                    appendAtomicFileError(
                        publishError,
                        targetError);
                }
            }
            appendAtomicFileError(error, publishError);
            cleanup();
            return false;
        }
        if (!targetPublished) {
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "the activity publisher did not create the target"));
            cleanup();
            return false;
        }

        QString targetError;
        if (!capturePublishedTarget(input, targetError)
            || !input.targetParent.pathMatches(targetError)) {
            appendAtomicFileError(
                error,
                targetError);
            cleanup();
            return false;
        }
    }

    for (const StagedInput &input : stagedInputs) {
        QString syncError;
        if (!input.stagingParent.pathMatches(syncError)
            || !input.targetParent.pathMatches(syncError)
            || !input.stagingParent.sync(syncError)
            || !input.targetParent.sync(syncError)) {
            error = syncError.isEmpty()
                ? QStringLiteral(
                      "cannot sync an anchored activity publication")
                : syncError;
            cleanup();
            return false;
        }
    }

    if (finalize) {
        QString finalizationError;
        if (!finalize(finalizationError)) {
            if (finalizationError.isEmpty()) {
                finalizationError = QStringLiteral(
                    "Cannot finalize the staged activity files");
            }
            appendAtomicFileError(error, finalizationError);
            cleanup();
            return false;
        }
        if (!finalizationError.isEmpty()) {
            appendAtomicFileError(error, finalizationError);
        }
    }

    return true;
}

inline bool writeFileAtomically(const QString &path,
                                const QByteArray &data,
                                const AtomicFileWriterFactory &factory,
                                QString &error,
                                bool allowTargetReplacement = true,
                                bool targetLockHeld = false,
                                const AtomicPreCommitValidation
                                    &validateBeforeCommit = {})
{
    error.clear();

    if (path.isEmpty()) {
        error = QStringLiteral("Cannot save an activity without a file name");
        return false;
    }

    std::unique_ptr<QLockFile> targetLock;
    if (!targetLockHeld) {
        targetLock.reset(new QLockFile(atomicFileLockPath(path)));
        targetLock->setStaleLockTime(0);
        if (!targetLock->tryLock(0)) {
            error = QStringLiteral("The activity file is already being saved");
            return false;
        }
    }

    const QFileInfo targetInfo(path);
    if (targetInfo.isSymLink()) {
        error = QStringLiteral("Cannot save an activity through a symbolic link");
        return false;
    }

    const bool hadOriginal = targetInfo.exists();
    if (hadOriginal && !allowTargetReplacement) {
        error = QStringLiteral("The target activity already exists");
        return false;
    }

    if (hadOriginal) {
        QFile originalFile(path);
        if (!originalFile.open(QIODevice::ReadOnly)) {
            error = QStringLiteral(
                        "Cannot read the existing activity before saving: %1")
                        .arg(originalFile.errorString());
            return false;
        }
        while (!originalFile.atEnd()) {
            const QByteArray chunk = originalFile.read(1024 * 1024);
            if (chunk.isEmpty()
                && originalFile.error() != QFileDevice::NoError) {
                error = QStringLiteral(
                            "Cannot read the complete existing activity: %1")
                            .arg(originalFile.errorString());
                return false;
            }
        }
    }

    const AtomicFileMode mode = hadOriginal
        ? AtomicFileMode::ReplaceExisting
        : AtomicFileMode::CreateNew;
    std::unique_ptr<AtomicFileWriter> writer = factory
        ? factory(path, mode)
        : std::unique_ptr<AtomicFileWriter>();
    if (!writer) {
        error = QStringLiteral("Cannot create the atomic activity writer");
        return false;
    }

    if (!writer->open()) {
        error = atomicFileError(QStringLiteral("Cannot open the activity file"),
                                *writer);
        return false;
    }

    const qint64 written = writer->write(data);
    if (written != static_cast<qint64>(data.size())) {
        error = atomicFileError(
            QStringLiteral("Cannot write the complete activity file"), *writer);
        writer->cancelWriting();
        return false;
    }

    if (!writer->flush()) {
        error = atomicFileError(QStringLiteral("Cannot flush the activity file"),
                                *writer);
        writer->cancelWriting();
        return false;
    }

    if (validateBeforeCommit
        && !validateBeforeCommit(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The activity changed before atomic publication");
        }
        writer->cancelWriting();
        return false;
    }

    if (!writer->commit()) {
        error = atomicFileError(QStringLiteral("Cannot commit the activity file"),
                                *writer);
        return false;
    }

    if (writer->verifiesCommittedResult()) {
        return true;
    }

    QString syncError;
    if (!syncParentDirectory(path, syncError)) {
        error = syncError;
        writer.reset();
        appendAtomicFileError(
            error,
            QStringLiteral(
                "the unverified committed activity pathname was retained"));
        return false;
    }

    QFile committedFile(path);
    QByteArray committed;
    if (!committedFile.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot verify the committed activity file: %1")
                    .arg(committedFile.errorString());
    } else {
        committed = committedFile.readAll();
        if (committedFile.error() != QFileDevice::NoError) {
            error = QStringLiteral(
                        "Cannot read the committed activity file: %1")
                        .arg(committedFile.errorString());
        } else if (committed != data) {
            error = QStringLiteral(
                "Committed activity contents do not match the requested data");
        }
    }

    if (!error.isEmpty()) {
        committedFile.close();
        writer.reset();
        appendAtomicFileError(
            error,
            QStringLiteral(
                "the unverified committed activity pathname was retained"));
        return false;
    }
    return true;
}

using ActivitySaveStep = std::function<bool(QString &error)>;
using ActivityCacheUpdate = std::function<void()>;

inline bool publishActivityBeforeCacheUpdate(
    const ActivitySaveStep &publish,
    const ActivityCacheUpdate &updateCache,
    QString &error)
{
    error.clear();
    if (!publish || !updateCache) {
        error = QStringLiteral(
            "Cannot complete the activity publication");
        return false;
    }

    if (!publish(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot publish the activity");
        }
        return false;
    }

    updateCache();
    return true;
}

inline bool completeActivitySave(const ActivitySaveStep &persist,
                                 const ActivitySaveStep &finalize,
                                 const std::function<void()> &markClean,
                                 QString &error,
                                 const ActivitySaveStep &rollback = ActivitySaveStep())
{
    error.clear();

    if (!persist(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot persist the activity");
        }
        return false;
    }

    if (!finalize(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot finalize the activity save");
        }
        if (rollback) {
            QString rollbackError;
            if (!rollback(rollbackError)) {
                if (rollbackError.isEmpty()) {
                    rollbackError =
                        QStringLiteral("Cannot roll back the activity save");
                }
                if (!error.isEmpty()) error += QStringLiteral("; ");
                error += rollbackError;
            }
        }
        return false;
    }

    markClean();
    return true;
}

inline void appendActivityRollbackError(QString &error, const QString &detail)
{
    if (!error.isEmpty()) {
        error += QStringLiteral("; ");
    }
    error += detail;
}

inline bool finalizeActivityFileReplacement(const QString &sourcePath,
                                            const QString &targetPath,
                                            bool keepSourceBackup,
                                            QString &error,
                                            const AtomicDirectorySyncFunction &syncDirectory =
                                                syncParentDirectory)
{
    error.clear();

    if (sourcePath == targetPath) {
        return true;
    }

    if (!syncDirectory) {
        error = QStringLiteral(
            "Cannot sync the activity directory during finalization");
        return false;
    }

    const auto syncRollback = [&]() {
        QString rollbackSyncError;
        if (!syncDirectory(sourcePath, rollbackSyncError)) {
            if (rollbackSyncError.isEmpty()) {
                rollbackSyncError = QStringLiteral(
                    "cannot sync the restored activity directory");
            }
            appendActivityRollbackError(error, rollbackSyncError);
        }
    };

    if (!keepSourceBackup) {
        const QString rollbackPath =
            sourcePath + QStringLiteral(".rollback-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(sourcePath, rollbackPath)) {
            error = QStringLiteral("Cannot remove the superseded activity");
            return false;
        }

        if (!syncDirectory(sourcePath, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot sync the activity directory during finalization");
            }
            if (!QFile::rename(rollbackPath, sourcePath)) {
                appendActivityRollbackError(
                    error,
                    QStringLiteral("cannot restore the original activity"));
            } else {
                syncRollback();
            }
            return false;
        }

        // The source removal is durable at this point. Cleanup cannot safely
        // turn a successful save back into a failure.
        if (QFile::remove(rollbackPath)) {
            QString cleanupSyncError;
            syncDirectory(sourcePath, cleanupSyncError);
        }
        return true;
    }

    const QString backupPath = sourcePath + QStringLiteral(".bak");
    QString previousBackupPath;
    const QFileInfo previousBackup(backupPath);
    const bool hadPreviousBackup =
        previousBackup.exists() || previousBackup.isSymLink();
    if (hadPreviousBackup) {
        previousBackupPath =
            backupPath + QStringLiteral(".rollback-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(backupPath, previousBackupPath)) {
            error = QStringLiteral(
                "Cannot preserve the previous activity backup");
            return false;
        }
    }

    if (!QFile::rename(sourcePath, backupPath)) {
        error = QStringLiteral("Cannot back up the original activity");
        if (hadPreviousBackup) {
            if (!QFile::rename(previousBackupPath, backupPath)) {
                appendActivityRollbackError(
                    error,
                    QStringLiteral(
                        "cannot restore the previous activity backup"));
            } else {
                syncRollback();
            }
        }
        return false;
    }

    if (!syncDirectory(sourcePath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot sync the activity directory during finalization");
        }
        if (!QFile::rename(backupPath, sourcePath)) {
            appendActivityRollbackError(
                error,
                QStringLiteral("cannot restore the original activity"));
        }
        if (hadPreviousBackup
            && !QFile::rename(previousBackupPath, backupPath)) {
            appendActivityRollbackError(
                error,
                QStringLiteral("cannot restore the previous activity backup"));
        }
        syncRollback();
        return false;
    }

    // The new backup is durable. Failure to delete an older hidden rollback
    // copy must not cause the committed target to be removed by the caller.
    if (hadPreviousBackup && QFile::remove(previousBackupPath)) {
        QString cleanupSyncError;
        syncDirectory(sourcePath, cleanupSyncError);
    }
    return true;
}

#endif // _GC_AtomicFileWriter_h
