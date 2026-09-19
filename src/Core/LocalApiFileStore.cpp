/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LocalApiFileStore.h"

#include <QCryptographicHash>
#include <QDir>
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

    LocalApiFileGeneration candidate;
    if (!openDirectory(
            directoryComponents, candidate.parent_, error)) {
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
