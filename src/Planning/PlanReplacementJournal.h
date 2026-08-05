/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_PlanReplacementJournal_h
#define _GC_PlanReplacementJournal_h

#include <QString>
#include <QStringList>

#include <memory>

namespace PlanReplacement {

struct Specification
{
    QString athleteRoot;
    QString scopeRoot;
    QStringList inputPaths;
    QStringList removalPaths;
    QStringList targetPaths;
    QStringList mustRemainAbsentPaths;
};

struct JournalState;

class Journal
{
public:
    static std::shared_ptr<Journal> prepare(
        const Specification &specification, QString &error);
    static std::shared_ptr<Journal> openPrepared(
        const QString &athleteRoot,
        const QString &transactionId,
        QString &error);
    static bool reconcileAll(const QString &athleteRoot, QString &error);

    int targetCount() const;
    QString stagingPath(int targetIndex) const;
    QString directoryPath() const;

    bool recordStaged(int targetIndex, QString &error);
    bool publishAndCommit(QString &error);
    bool cleanupAfterRollback(QString &error);
    bool cleanupAfterCommit(QString &error);
    bool commitState(bool &committed, QString &error) const;
    bool hasCommitMarker() const;

private:
    explicit Journal(std::shared_ptr<JournalState> state);

    std::shared_ptr<JournalState> state_;
};

} // namespace PlanReplacement

#endif
