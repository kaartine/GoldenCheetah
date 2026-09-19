/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LocalApiFileStore.h"
#include "PortableFileName.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <utility>

namespace {

bool generationMatches(
    AnchoredFileSystem::DirectoryAnchor &parent,
    AnchoredFileSystem::EntryRef &entry,
    AnchoredFileSystem::PinnedFile &file,
    AnchoredFileSystem::FileGenerationGuard &guard,
    QString &error)
{
    if (!parent.pathMatches(error) || !guard.unchanged(error)) return false;

    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            entry, file, matches, error)
        || !matches
        || !parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The Local API file generation changed");
        }
        return false;
    }
    return true;
}

} // namespace

bool LocalApiFileGeneration::isValid() const
{
    return parent_.isValid()
        && entry_.isValid()
        && file_.isValid()
        && guard_.isValid();
}

qint64 LocalApiFileGeneration::size() const
{
    return file_.size();
}

bool LocalApiFileGeneration::readAll(
    QByteArray &contents, QString &error)
{
    contents.clear();
    error.clear();
    if (!validateIdentity(error)) return false;

    QByteArray candidate;
    if (!AnchoredFileSystem::readAll(
            file_, file_.size(), candidate, error)
        || QCryptographicHash::hash(
               candidate, QCryptographicHash::Sha256)
            != file_.sha256()
        || !validateIdentity(error)) {
        candidate.clear();
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The Local API file contents changed");
        }
        return false;
    }
    contents = std::move(candidate);
    return true;
}

bool LocalApiFileGeneration::validateIdentity(QString &error)
{
    error.clear();
    if (!isValid()) {
        error = QStringLiteral(
            "The Local API file generation is unavailable");
        return false;
    }
    return generationMatches(parent_, entry_, file_, guard_, error);
}

LocalApiFileStore::LocalApiFileStore(QString absoluteRootPath)
{
    if (QDir::isAbsolutePath(absoluteRootPath)) {
        rootPath_ = QDir::cleanPath(std::move(absoluteRootPath));
    }
}

bool LocalApiFileStore::openDirectory(
    const QStringList &components,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error) const
{
    directory = {};
    error.clear();
    if (rootPath_.isEmpty()) {
        error = QStringLiteral("The Local API root path is not absolute");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor current;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            rootPath_, current, error)
        || !current.pathMatches(error)) {
        return false;
    }

    return openDirectory(current, components, directory, error);
}

bool LocalApiFileStore::openDirectory(
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &components,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error) const
{
    directory = {};
    error.clear();
    if (!baseDirectory.isValid()
        || !baseDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The Local API base directory is unavailable");
        }
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor current = baseDirectory;
    for (const QString &component : components) {
        AnchoredFileSystem::DirectoryAnchor child;
        if (!current.openChild(component, child, error)) return false;
        current = std::move(child);
    }
    if (!current.pathMatches(error)) return false;

    directory = std::move(current);
    return true;
}

bool LocalApiFileStore::captureRegularFile(
    const QStringList &directoryComponents,
    const QString &fileComponent,
    LocalApiFileGeneration &generation,
    QString &error,
    qint64 maximumSize) const
{
    error.clear();
    generation = {};
    if (maximumSize < 0) {
        error = QStringLiteral(
            "The Local API file size budget is invalid");
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor root;
    if (!openDirectory({}, root, error)) {
        return false;
    }
    return captureRegularFile(
        root, directoryComponents, fileComponent,
        generation, error, maximumSize);
}

bool LocalApiFileStore::captureRegularFile(
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    const QString &fileComponent,
    LocalApiFileGeneration &generation,
    QString &error,
    qint64 maximumSize) const
{
    error.clear();
    generation = {};
    if (maximumSize < 0) {
        error = QStringLiteral(
            "The Local API file size budget is invalid");
        return false;
    }

    LocalApiFileGeneration candidate;
    if (!openDirectory(
            baseDirectory, directoryComponents,
            candidate.parent_, error)) {
        return false;
    }
    candidate.entry_ = candidate.parent_.entry(fileComponent, error);
    if (!candidate.entry_.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            candidate.entry_, candidate.file_, error, maximumSize)
        || !AnchoredFileSystem::guardFileGeneration(
            candidate.entry_, candidate.file_, candidate.guard_, error)
        || !candidate.validateIdentity(error)) {
        return false;
    }

    generation = std::move(candidate);
    return true;
}

bool LocalApiFileStore::captureListedRegularFile(
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    const AnchoredFileSystem::DirectoryEntry &listedEntry,
    LocalApiFileGeneration &generation,
    QString &error,
    qint64 maximumSize) const
{
    generation = {};
    error.clear();
    LocalApiFileGeneration candidate;
    if (listedEntry.kind
            != AnchoredFileSystem::DirectoryEntryKind::RegularFile
        || !listedEntry.identity.isValid()
        || !captureRegularFile(
            baseDirectory, directoryComponents, listedEntry.name,
            candidate, error, maximumSize)
        || candidate.file_.identity() != listedEntry.identity) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The listed Local API file generation changed");
        }
        return false;
    }
    generation = std::move(candidate);
    return true;
}

bool LocalApiFileStore::captureRegularFileIfExists(
    const AnchoredFileSystem::DirectoryAnchor &baseDirectory,
    const QStringList &directoryComponents,
    const QString &fileComponent,
    LocalApiFileGeneration &generation,
    bool &exists,
    QString &error,
    qint64 maximumSize) const
{
    generation = {};
    exists = false;
    error.clear();
    if (maximumSize < 0
        || !baseDirectory.isValid()
        || !baseDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = maximumSize < 0
                ? QStringLiteral(
                    "The Local API file size budget is invalid")
                : QStringLiteral(
                    "The Local API base directory is unavailable");
        }
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor parent = baseDirectory;
    for (const QString &component : directoryComponents) {
        AnchoredFileSystem::DirectoryAnchor child;
        bool childExists = false;
        if (!parent.openChildIfExists(
                component, child, childExists, error)) {
            return false;
        }
        if (!childExists) return parent.pathMatches(error);
        parent = std::move(child);
    }
    if (!parent.pathMatches(error)) return false;

    LocalApiFileGeneration candidate;
    candidate.parent_ = parent;
    candidate.entry_ = parent.entry(fileComponent, error);
    if (!candidate.entry_.isValid()
        || !AnchoredFileSystem::entryExists(
            candidate.entry_, exists, error)) {
        return false;
    }
    if (!exists) return parent.pathMatches(error);
    if (!AnchoredFileSystem::pinRegularFile(
            candidate.entry_, candidate.file_, error, maximumSize)
        || !AnchoredFileSystem::guardFileGeneration(
            candidate.entry_, candidate.file_, candidate.guard_, error)
        || !candidate.validateIdentity(error)) {
        exists = false;
        return false;
    }
    generation = std::move(candidate);
    return true;
}

bool LocalApiFileSnapshotDirectory::writeFile(
    const QString &component,
    const QByteArray &contents,
    QString &path,
    QString &error)
{
    path.clear();
    error.clear();
    if (!directory_.isValid()
        || !PortableFileName::isValid(component)) {
        error = QStringLiteral(
            "The Local API snapshot destination is invalid");
        return false;
    }

    const QString candidate = directory_.filePath(component);
    QFile file(candidate);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(contents) != contents.size()
        || !file.flush()) {
        error = file.errorString();
        file.close();
        QFile::remove(candidate);
        return false;
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        error = file.errorString();
        QFile::remove(candidate);
        return false;
    }
    path = candidate;
    return true;
}
