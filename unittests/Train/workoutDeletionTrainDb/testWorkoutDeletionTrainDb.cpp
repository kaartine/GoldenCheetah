/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "TrainDB.h"
#include "WorkoutDeletionService.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

bool trainDbPlanImportCommitReportedFailure()
{
    return false;
}

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool insertWorkout(TrainDB &database, const QString &path)
{
    ErgFileBase workout;
    workout.filename(path);
    workout.name(QStringLiteral("Deletion test"));
    workout.format(path.endsWith(QStringLiteral(".crs"), Qt::CaseInsensitive)
                           ? ErgFileFormat::crs
                           : ErgFileFormat::erg);
    return database.importWorkout(path, workout);
}

} // namespace

class TestWorkoutDeletionTrainDb : public QObject
{
    Q_OBJECT

private slots:
    void commitsFileAndDatabaseDeletion();
    void restoresFileWhenDatabaseRowDoesNotExist();
    void preservesDatabaseRowWhenFileCannotBeStaged();
};

void TestWorkoutDeletionTrainDb::commitsFileAndDatabaseDeletion()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TrainDB database{QDir(home.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);

    const QString workout = home.filePath(QStringLiteral("workout.crs"));
    const QString sidecar = workoutDeletionSidecarPath(workout);
    QVERIFY(writeFile(workout, QByteArrayLiteral("workout")));
    QVERIFY(writeFile(sidecar, QByteArrayLiteral("metadata")));
    QVERIFY(insertWorkout(database, workout));
    QVERIFY(database.hasWorkout(workout));

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(database, QStringList() << workout);

    QVERIFY2(result.succeeded, qPrintable(result.errorMessage));
    QVERIFY(!database.hasWorkout(workout));
    QVERIFY(!QFileInfo::exists(workout));
    QVERIFY(!QFileInfo::exists(sidecar));
}

void TestWorkoutDeletionTrainDb::restoresFileWhenDatabaseRowDoesNotExist()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TrainDB database{QDir(home.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);

    const QString workout = home.filePath(QStringLiteral("orphan.erg"));
    QVERIFY(writeFile(workout, QByteArrayLiteral("keep me")));
    QVERIFY(!database.hasWorkout(workout));

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(database, QStringList() << workout);

    QVERIFY(!result.succeeded);
    QCOMPARE(result.failedPath, workout);
    QVERIFY(QFileInfo::exists(workout));
    QVERIFY(!database.hasWorkout(workout));
}

void TestWorkoutDeletionTrainDb::preservesDatabaseRowWhenFileCannotBeStaged()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TrainDB database{QDir(home.path())};
    QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);

    const QString workout = home.filePath(QStringLiteral("missing.erg"));
    QVERIFY(insertWorkout(database, workout));
    QVERIFY(database.hasWorkout(workout));
    QVERIFY(!QFileInfo::exists(workout));

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(database, QStringList() << workout);

    QVERIFY(!result.succeeded);
    QCOMPARE(result.failedPath, workout);
    QVERIFY(database.hasWorkout(workout));
}

QTEST_GUILESS_MAIN(TestWorkoutDeletionTrainDb)

#include "testWorkoutDeletionTrainDb.moc"
