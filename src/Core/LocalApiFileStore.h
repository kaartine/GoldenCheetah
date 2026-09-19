/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_LOCALAPIFILESTORE_H
#define GC_LOCALAPIFILESTORE_H

#include "FileIO/AnchoredFileSystem.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

class LocalApiFileGeneration
{
public:
    LocalApiFileGeneration() = default;

    LocalApiFileGeneration(LocalApiFileGeneration &&) noexcept = default;
    LocalApiFileGeneration &operator=(
        LocalApiFileGeneration &&) noexcept = default;

    LocalApiFileGeneration(const LocalApiFileGeneration &) = delete;
    LocalApiFileGeneration &operator=(
        const LocalApiFileGeneration &) = delete;

    bool isValid() const;
    qint64 size() const;

    // Reads only through the pinned native handle and publishes bytes after
    // identity, change-event, and digest validation all succeed.
    bool readAll(QByteArray &contents, QString &error);

private:
    bool validateIdentity(QString &error);

    AnchoredFileSystem::DirectoryAnchor parent_;
    AnchoredFileSystem::EntryRef entry_;
    AnchoredFileSystem::PinnedFile file_;
    AnchoredFileSystem::FileGenerationGuard guard_;

    friend class LocalApiFileStore;
};

class LocalApiFileStore
{
public:
    explicit LocalApiFileStore(QString absoluteRootPath);

    bool captureRegularFile(
        const QStringList &directoryComponents,
        const QString &fileComponent,
        LocalApiFileGeneration &generation,
        QString &error,
        qint64 maximumSize) const;

    bool captureRegularFile(
        const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
        const QStringList &directoryComponents,
        const QString &fileComponent,
        LocalApiFileGeneration &generation,
        QString &error,
        qint64 maximumSize) const;

    bool captureListedRegularFile(
        const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
        const QStringList &directoryComponents,
        const AnchoredFileSystem::DirectoryEntry &listedEntry,
        LocalApiFileGeneration &generation,
        QString &error,
        qint64 maximumSize) const;

    // A missing directory or file is a successful empty result. Existing
    // unsafe, aliased, non-regular, changed, or oversized entries fail.
    bool captureRegularFileIfExists(
        const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
        const QStringList &directoryComponents,
        const QString &fileComponent,
        LocalApiFileGeneration &generation,
        bool &exists,
        QString &error,
        qint64 maximumSize) const;

    bool openDirectory(
        const QStringList &components,
        AnchoredFileSystem::DirectoryAnchor &directory,
        QString &error) const;

    bool openDirectory(
        const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
        const QStringList &components,
        AnchoredFileSystem::DirectoryAnchor &directory,
        QString &error) const;

private:
    QString rootPath_;
};

// A process-private, auto-removed bridge for legacy readers that accept only
// path-backed QFile objects or path strings. Only already verified bytes may
// be written here.
class LocalApiFileSnapshotDirectory
{
public:
    bool isValid() const { return directory_.isValid(); }
    QString path() const { return directory_.path(); }
    bool writeFile(
        const QString &component,
        const QByteArray &contents,
        QString &path,
        QString &error);

private:
    QTemporaryDir directory_;
};

#endif
