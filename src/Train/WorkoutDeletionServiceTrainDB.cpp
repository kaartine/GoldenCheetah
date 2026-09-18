/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WorkoutDeletionService.h"
#include "TrainDB.h"

WorkoutDeletionOperations workoutDeletionOperations(TrainDB &database)
{
    WorkoutDeletionOperations operations =
            defaultWorkoutDeletionFileOperations();
    operations.beginDatabaseTransaction = [&database] {
        return database.startLUW();
    };
    operations.deleteDatabaseWorkout = [&database](const QString &path) {
        return database.hasWorkout(path)
            && database.deleteWorkout(path)
            && !database.hasWorkout(path);
    };
    operations.commitDatabaseTransaction = [&database] {
        return database.endLUW();
    };
    operations.rollbackDatabaseTransaction = [&database] {
        database.rollbackLUW();
    };
    return operations;
}

WorkoutDeletionResult deleteWorkoutsAtomically(
        TrainDB &database,
        const QStringList &workoutPaths)
{
    return deleteWorkoutsAtomically(
            workoutPaths, workoutDeletionOperations(database));
}
