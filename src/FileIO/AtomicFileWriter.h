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

#include <QByteArray>
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
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <cstdint>
#include <io.h>
#include <qt_windows.h>
#endif

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
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

struct AtomicFileSnapshot
{
    qint64 size = 0;
    QByteArray digest;
};

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
using AtomicFinalizeFunction = std::function<bool(QString &error)>;
using AtomicMoveFunction = std::function<bool(
    const QString &sourcePath, const QString &targetPath, QString &error)>;

inline bool publishAtomicReplacement(const QString &temporaryPath,
                                     const QString &targetPath,
                                     QString &error)
{
#ifdef Q_OS_UNIX
    const QByteArray temporary = QFile::encodeName(temporaryPath);
    const QByteArray target = QFile::encodeName(targetPath);
    int result;
    do {
        result = ::rename(temporary.constData(), target.constData());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = QStringLiteral("Cannot publish the activity file: %1")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#elif defined(Q_OS_WIN)
    if (!::MoveFileExW(
            reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
            reinterpret_cast<LPCWSTR>(targetPath.utf16()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = QStringLiteral(
                    "Cannot publish the activity file: system error %1")
                    .arg(::GetLastError());
        return false;
    }
#else
    Q_UNUSED(temporaryPath);
    Q_UNUSED(targetPath);
    error = QStringLiteral(
        "Atomic activity replacement is unsupported on this platform");
    return false;
#endif
    return true;
}

class ReplaceAtomicFileWriter final : public AtomicFileWriter
{
public:
    explicit ReplaceAtomicFileWriter(const QString &targetPath)
        : targetPath_(targetPath),
          file_(QDir(QFileInfo(targetPath).absolutePath()).filePath(
              QStringLiteral(".%1.XXXXXX.tmp")
                  .arg(QFileInfo(targetPath).fileName())))
    {
    }

    bool open() override
    {
        if (!file_.open()) {
            error_ = file_.errorString();
            return false;
        }
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        return file_.write(data);
    }

    bool flush() override
    {
        if (!file_.flush()) {
            return false;
        }
        error_.clear();
        return syncFileDevice(file_, error_);
    }

    bool commit() override
    {
        const QString temporaryPath = file_.fileName();
        file_.close();
        if (!publishAtomicReplacement(
                temporaryPath, targetPath_, error_)) {
            file_.remove();
            return false;
        }
        file_.setAutoRemove(false);
        return true;
    }

    void cancelWriting() override
    {
        file_.close();
        file_.remove();
    }

    QString errorString() const override
    {
        return error_.isEmpty() ? file_.errorString() : error_;
    }

private:
    QString targetPath_;
    QTemporaryFile file_;
    QString error_;
};

inline bool publishAtomicNew(const QString &temporaryPath,
                             const QString &targetPath,
                             bool &temporaryMoved,
                             QString &error)
{
    temporaryMoved = false;
#ifdef Q_OS_UNIX
    const QByteArray temporary = QFile::encodeName(temporaryPath);
    const QByteArray target = QFile::encodeName(targetPath);
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
        AtomicPublishFunction publish = publishAtomicNew)
        : targetPath_(targetPath),
          file_(QDir(QFileInfo(targetPath).absolutePath()).filePath(
              QStringLiteral(".%1.XXXXXX.tmp")
                  .arg(QFileInfo(targetPath).fileName()))),
          publish_(std::move(publish))
    {
    }

    bool open() override
    {
        if (targetExists()) {
            error_ = QStringLiteral("The target activity already exists");
            return false;
        }
        if (!file_.open()) {
            error_ = file_.errorString();
            return false;
        }
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        return file_.write(data);
    }

    bool flush() override
    {
        if (!file_.flush()) {
            return false;
        }
        error_.clear();
        return syncFileDevice(file_, error_);
    }

    bool commit() override
    {
        const QString temporaryPath = file_.fileName();
        file_.close();
        bool targetPublished = false;
        if (!publish_ || !publish_(
                temporaryPath, targetPath_, targetPublished, error_)) {
            if (!publish_ && error_.isEmpty()) {
                error_ = QStringLiteral(
                    "Cannot publish the new activity file");
            }
            const auto appendError = [&](const QString &detail) {
                if (!error_.isEmpty()) error_ += QStringLiteral("; ");
                error_ += detail;
            };
            bool directoryChanged = false;
            if (targetPublished) {
                const QFileInfo target(targetPath_);
                if (target.exists() || target.isSymLink()) {
                    if (QFile::remove(targetPath_)) {
                        directoryChanged = true;
                    } else {
                        appendError(QStringLiteral(
                            "cannot remove the partially published activity"));
                    }
                }
            }
            const QFileInfo staging(temporaryPath);
            if (staging.exists() || staging.isSymLink()) {
                if (file_.remove()) {
                    directoryChanged = true;
                } else {
                    appendError(QStringLiteral(
                        "cannot remove the activity staging file"));
                }
            }
            if (directoryChanged) {
                QString syncError;
                if (!syncParentDirectory(targetPath_, syncError)) {
                    appendError(syncError);
                }
            }
            return false;
        }
        if (targetPublished) {
            file_.setAutoRemove(false);
        }
        return true;
    }

    void cancelWriting() override
    {
        file_.close();
        file_.remove();
    }

    QString errorString() const override
    {
        return error_.isEmpty() ? file_.errorString() : error_;
    }

private:
    bool targetExists() const
    {
        const QFileInfo target(targetPath_);
        return target.exists() || target.isSymLink();
    }

    QString targetPath_;
    QTemporaryFile file_;
    AtomicPublishFunction publish_;
    QString error_;
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

inline void cleanupStagedFiles(
    const QList<StagedFilePublication> &files, QString &error)
{
    QSet<QString> syncedDirectories;
    for (const StagedFilePublication &file : files) {
        const QFileInfo staging(file.first);
        const QString directory = staging.absolutePath();
        if ((staging.exists() || staging.isSymLink())
            && !QFile::remove(file.first)) {
            appendAtomicFileError(
                error,
                QStringLiteral("cannot remove an activity staging file"));
        }

        if (syncedDirectories.contains(directory)) continue;
        syncedDirectories.insert(directory);
        QString syncError;
        if (!syncParentDirectory(file.first, syncError)) {
            appendAtomicFileError(error, syncError);
        }
    }
}

inline bool publishStagedFileSet(
    const QList<StagedFilePublication> &files,
    QString &error,
    const AtomicPublishFunction &publish = publishAtomicNew,
    const AtomicFinalizeFunction &finalize = AtomicFinalizeFunction())
{
    error.clear();
    if (files.isEmpty() || !publish) {
        error = QStringLiteral("No staged activity files to publish");
        cleanupStagedFiles(files, error);
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
    if (duplicateTarget) {
        error = QStringLiteral("Duplicate activity publication target");
        cleanupStagedFiles(files, error);
        return false;
    }

    for (const StagedFilePublication &file : files) {
        const QFileInfo staging(file.first);
        if (staging.isSymLink() || !staging.exists()
            || !staging.isFile()) {
            error = QStringLiteral("A staged activity file is unavailable");
            cleanupStagedFiles(files, error);
            return false;
        }
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
            cleanupStagedFiles(files, error);
            return false;
        }
        locks.push_back(std::move(lock));
    }

    for (const StagedFilePublication &file : files) {
        const QFileInfo target(file.second);
        if (target.exists() || target.isSymLink()) {
            error = QStringLiteral(
                "An activity publication target already exists");
            cleanupStagedFiles(files, error);
            return false;
        }
    }

    QStringList published;
    const auto cleanup = [&]() {
        QStringList changedPaths;
        for (const QString &target : published) {
            const QFileInfo current(target);
            if ((current.exists() || current.isSymLink())
                && !QFile::remove(target)) {
                appendAtomicFileError(
                    error,
                    QStringLiteral("cannot remove a partially published activity"));
            } else {
                changedPaths.append(target);
            }
        }
        for (const StagedFilePublication &file : files) {
            const QFileInfo staging(file.first);
            if ((staging.exists() || staging.isSymLink())
                && !QFile::remove(file.first)) {
                appendAtomicFileError(
                    error,
                    QStringLiteral("cannot remove an activity staging file"));
            } else {
                changedPaths.append(file.first);
            }
        }

        QSet<QString> syncedDirectories;
        for (const QString &path : changedPaths) {
            const QString directory = QFileInfo(path).absolutePath();
            if (syncedDirectories.contains(directory)) continue;
            syncedDirectories.insert(directory);
            QString syncError;
            if (!syncParentDirectory(path, syncError)) {
                appendAtomicFileError(error, syncError);
            }
        }
    };

    for (const StagedFilePublication &file : files) {
        bool temporaryMoved = false;
        QString publishError;
        if (!publish(file.first, file.second,
                     temporaryMoved, publishError)) {
            if (temporaryMoved) {
                published.append(file.second);
            }
            appendAtomicFileError(error, publishError);
            cleanup();
            return false;
        }
        published.append(file.second);
    }

    QSet<QString> syncedDirectories;
    for (const StagedFilePublication &file : files) {
        const QStringList paths = { file.first, file.second };
        for (const QString &path : paths) {
            const QString directory = QFileInfo(path).absolutePath();
            if (syncedDirectories.contains(directory)) continue;
            syncedDirectories.insert(directory);
            QString syncError;
            if (!syncParentDirectory(path, syncError)) {
                error = syncError;
                cleanup();
                return false;
            }
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


inline bool restoreAtomicFile(const QString &path, bool hadOriginal,
                              const QByteArray &original, QString &error)
{
    if (!hadOriginal) {
        const QFileInfo current(path);
        if (!current.exists() && !current.isSymLink()) {
            return true;
        }
        if (!QFile::remove(path)) {
            appendAtomicFileError(
                error,
                QStringLiteral("cannot remove the unverified activity file"));
            return false;
        }
        QString syncError;
        if (!syncParentDirectory(path, syncError)) {
            appendAtomicFileError(error, syncError);
            return false;
        }
        return true;
    }

    ReplaceAtomicFileWriter restore(path);
    if (!restore.open()) {
        appendAtomicFileError(
            error,
            QStringLiteral("cannot restore the previous activity: %1")
                .arg(restore.errorString()));
        return false;
    }
    if (restore.write(original)
        != static_cast<qint64>(original.size())) {
        appendAtomicFileError(
            error,
            QStringLiteral("cannot rewrite the previous activity: %1")
                .arg(restore.errorString()));
        restore.cancelWriting();
        return false;
    }
    if (!restore.flush()) {
        appendAtomicFileError(
            error,
            QStringLiteral("cannot flush the restored activity: %1")
                .arg(restore.errorString()));
        restore.cancelWriting();
        return false;
    }
    QString syncError;
    if (!restore.commit()) {
        appendAtomicFileError(
            error,
            QStringLiteral("cannot commit the restored activity: %1")
                .arg(restore.errorString()));
        return false;
    }
    if (!syncParentDirectory(path, syncError)) {
        appendAtomicFileError(error, syncError);
        return false;
    }
    return true;
}

inline bool writeFileAtomically(const QString &path,
                                const QByteArray &data,
                                const AtomicFileWriterFactory &factory,
                                QString &error,
                                bool allowTargetReplacement = true,
                                bool targetLockHeld = false)
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

    QByteArray original;
    if (hadOriginal) {
        QFile originalFile(path);
        if (!originalFile.open(QIODevice::ReadOnly)) {
            error = QStringLiteral(
                        "Cannot read the existing activity before saving: %1")
                        .arg(originalFile.errorString());
            return false;
        }
        original = originalFile.readAll();
        if (originalFile.error() != QFileDevice::NoError) {
            error = QStringLiteral(
                        "Cannot read the complete existing activity: %1")
                        .arg(originalFile.errorString());
            return false;
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

    if (!writer->commit()) {
        error = atomicFileError(QStringLiteral("Cannot commit the activity file"),
                                *writer);
        return false;
    }

    QString syncError;
    if (!syncParentDirectory(path, syncError)) {
        error = syncError;
        writer.reset();
        restoreAtomicFile(path, hadOriginal, original, error);
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
        restoreAtomicFile(path, hadOriginal, original, error);
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

using AtomicDirectorySyncFunction =
    std::function<bool(const QString &path, QString &error)>;

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
