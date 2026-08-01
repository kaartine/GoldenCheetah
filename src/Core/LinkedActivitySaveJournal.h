/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_LinkedActivitySaveJournal_h
#define _GC_LinkedActivitySaveJournal_h

#include <QList>
#include <QString>

#include <memory>

namespace LinkedActivitySave {

struct EntrySpecification
{
    QString sourcePath;
    QString targetPath;
    QString backupPath;
    bool keepSourceBackup = false;
};

struct Specification
{
    QString athleteRoot;
    QList<EntrySpecification> entries;
};

struct JournalState;

class Journal : public std::enable_shared_from_this<Journal>
{
public:
    static std::shared_ptr<Journal> prepare(
        const Specification &specification, QString &error);
    static bool reconcileAll(const QString &athleteRoot, QString &error);

    int entryCount() const;
    QString stagingPath(int index) const;
    QString directoryPath() const;

    bool recordStaged(int index, QString &error);
    bool publishAndCommit(QString &error);
    bool cleanupAfterRollback(QString &error);
    bool cleanupAfterCommit(QString &error);
    bool hasCommitMarker() const;

private:
    explicit Journal(std::shared_ptr<JournalState> state);

    std::shared_ptr<JournalState> state_;
};

} // namespace LinkedActivitySave

#endif
