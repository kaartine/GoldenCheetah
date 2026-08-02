/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_LinkedActivityRemovalJournal_h
#define _GC_LinkedActivityRemovalJournal_h

#include "AtomicFileWriter.h"

#include <QString>
#include <QStringList>

#include <memory>

namespace LinkedActivityRemoval {

struct Specification
{
    QString athleteRoot;
    QString sourcePath;
    QString backupPath;
    QString peerPath; // Empty for an unlinked activity deletion.
    QStringList derivedPaths;
};

struct JournalState;

class Journal : public std::enable_shared_from_this<Journal>
{
public:
    static std::shared_ptr<Journal> prepare(
        const Specification &specification, QString &error);
    static bool reconcileAll(const QString &athleteRoot, QString &error);

    QString transactionId() const;
    QString backupStagingPath() const;
    QString sourceTombstonePath() const;
    QString previousBackupPath() const;
    QString peerStagingPath() const;
    QString directoryPath() const;

    AtomicFileWriterFactory peerWriterFactory(
        const AtomicFileWriterFactory &delegate);

    bool validateOriginalStorage(QString &error) const;
    bool markCommitted(QString &error);
    bool hasCommitMarker() const;
    bool cleanupAfterRollback(QString &error);
    bool cleanupAfterCommit(QString &error);
    QStringList recoveryPaths() const;

private:
    explicit Journal(std::shared_ptr<JournalState> state);

    std::shared_ptr<JournalState> state_;
};

} // namespace LinkedActivityRemoval

#endif
