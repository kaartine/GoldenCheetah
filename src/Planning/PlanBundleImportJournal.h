/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_PlanBundleImportJournal_h
#define _GC_PlanBundleImportJournal_h

#include "TrainDB.h"

#include <QPointer>

#include <functional>
#include <memory>

namespace PlanBundleImport {

using DatabaseCompletion = std::function<bool(
    const TrainDB::PlanImportJournal &journal,
    QString &error)>;

class Journal
{
public:
    static std::shared_ptr<Journal> create(
        TrainDB *database,
        const QString &athleteRoot,
        const QString &workoutRoot,
        const QList<TrainDB::PlanImportWorkout> &workouts,
        QString &error);

    static bool reconcileAll(
        TrainDB *database,
        const QString &athleteRoot,
        const QString &workoutRoot,
        const DatabaseCompletion &completeDatabase,
        QString &error);

    bool commitDecision(
        const QString &planJournalPath,
        bool &committed,
        QString &error);
    bool completePublishedPlan(
        const DatabaseCompletion &completeDatabase,
        QString &error);

private:
    Journal(
        TrainDB *database,
        TrainDB::PlanImportJournal journal);

    bool completeRecord(
        const DatabaseCompletion &completeDatabase,
        QString &error);

    QPointer<TrainDB> database_;
    TrainDB::PlanImportJournal journal_;
};

} // namespace PlanBundleImport

#endif // _GC_PlanBundleImportJournal_h
