/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_WorkoutImportBatch_h
#define _GC_WorkoutImportBatch_h

#include <QString>
#include <QStringList>

#include <functional>

class Context;

struct WorkoutImportBatchResult
{
    bool succeeded = false;
    QString errorTitle;
    QString errorMessage;
    QStringList failedFiles;
    QStringList rollbackFailures;
};

using WorkoutImportBatchSuccess = std::function<void()>;

WorkoutImportBatchResult runWorkoutImportDialogBatch(
    Context *context,
    const QStringList &videos,
    const QStringList &workouts,
    const QStringList &videoSyncs,
    bool overwrite,
    const WorkoutImportBatchSuccess &onSuccess = {});

#endif // _GC_WorkoutImportBatch_h
