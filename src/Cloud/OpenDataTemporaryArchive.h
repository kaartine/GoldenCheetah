/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_TEMPORARY_ARCHIVE_H
#define GC_OPEN_DATA_TEMPORARY_ARCHIVE_H

#include <QString>
#include <QStringList>

#include <memory>

class QFile;
class QLockFile;

namespace OpenDataTemporaryArchive {

class WorkspaceGuard;

class TemporaryFile final
{
public:
    TemporaryFile(
        QString path,
        std::unique_ptr<QFile> device);
    ~TemporaryFile();

    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    QString path() const;
    QFile *device() const;

private:
    QString path_;
    std::unique_ptr<QFile> device_;
};

class Lease final
{
public:
    static std::shared_ptr<Lease> create(
        const QString &directory,
        QString &error);
    ~Lease();

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    QString path() const;
    QString directoryPath() const;
    std::unique_ptr<TemporaryFile> createFile(
        const QString &fileTemplate,
        QString &error) const;
    bool remove(QString &error);

private:
    Lease(
        QString root,
        QString directory,
        QString path,
        std::unique_ptr<QLockFile> lock,
        std::unique_ptr<WorkspaceGuard> workspaceGuard);

    QString root_;
    QString directory_;
    QString path_;
    std::unique_ptr<QLockFile> lock_;
    std::unique_ptr<WorkspaceGuard> workspaceGuard_;
};

int removeAbandoned(
    const QString &directory,
    QStringList &errors);

#ifdef GC_OPEN_DATA_TEMPORARY_ARCHIVE_TEST_HOOKS
bool createPrivateWorkspaceForTest(
    const QString &directory,
    const QString &workspaceName,
    QString &error);
bool rejectPrivateWorkspaceAfterCreateForTest(
    const QString &directory,
    const QString &workspaceName,
    QString &error);
bool hasPrivateSecurityForTest(
    const QString &path,
    bool directory);
#endif

} // namespace OpenDataTemporaryArchive

#endif
