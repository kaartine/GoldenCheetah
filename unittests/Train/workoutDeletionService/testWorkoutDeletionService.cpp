/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "WorkoutDeletionService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

struct FakeDatabase
{
    bool beginResult = true;
    bool commitResult = true;
    QString deleteFailure;
    QStringList rows;
    QStringList deleted;
    QStringList events;
    QStringList snapshot;
    int beginCalls = 0;
    int commitCalls = 0;
    int rollbackCalls = 0;

    WorkoutDeletionOperations operations()
    {
        WorkoutDeletionOperations result =
                defaultWorkoutDeletionFileOperations();
        result.beginDatabaseTransaction = [this] {
            events.append(QStringLiteral("db-begin"));
            ++beginCalls;
            if (beginResult) snapshot = rows;
            return beginResult;
        };
        result.deleteDatabaseWorkout = [this](const QString &path) {
            events.append(QStringLiteral("db-delete:%1").arg(path));
            deleted.append(path);
            if (path == deleteFailure) return false;
            rows.removeAll(path);
            return true;
        };
        result.commitDatabaseTransaction = [this] {
            events.append(QStringLiteral("db-commit"));
            ++commitCalls;
            return commitResult;
        };
        result.rollbackDatabaseTransaction = [this] {
            events.append(QStringLiteral("db-rollback"));
            ++rollbackCalls;
            rows = snapshot;
        };
        return result;
    }
};

QStringList stagedEntries(const QTemporaryDir &directory)
{
    return QDir(directory.path()).entryList(
            QStringList() << QStringLiteral(".gc-delete-*"),
            QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}

} // namespace

class TestWorkoutDeletionService : public QObject
{
    Q_OBJECT

private slots:
    void sidecarPathUsesCompleteBaseName();
    void nonCourseWorkoutDoesNotOwnSidecar();
    void deletesWorkoutAndSidecarAfterCommit();
    void preservesDatabaseWhenWorkoutStagingFails();
    void restoresWorkoutWhenSidecarStagingFails();
    void restoresFilesWhenTransactionCannotStart();
    void restoresWholeSelectionWhenDatabaseDeleteFails();
    void restoresFilesAndRowsWhenCommitFails();
    void reportsRollbackFailuresWithoutHidingPrimaryFailure();
    void reportsCleanupFailureOnlyAfterSuccessfulCommit();
    void rejectsMissingAndSymbolicLinkInputsBeforeDatabaseWork();
    void deDuplicatesSelectionWithoutChangingOrder();
    void preservesRelativeDatabaseKey();
};

void TestWorkoutDeletionService::sidecarPathUsesCompleteBaseName()
{
    QCOMPARE(workoutDeletionSidecarPath(QStringLiteral("/tmp/trail.mtb.crs")),
             QStringLiteral("/tmp/trail.mtb.gcmtb.json"));
    QCOMPARE(workoutDeletionSidecarPath(QStringLiteral("/tmp/trail.crs")),
             QStringLiteral("/tmp/trail.gcmtb.json"));
}

void TestWorkoutDeletionService::nonCourseWorkoutDoesNotOwnSidecar()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("trail.erg"));
    const QString course = directory.filePath(QStringLiteral("trail.crs"));
    const QString sidecar = directory.filePath(
            QStringLiteral("trail.gcmtb.json"));
    QVERIFY(workoutDeletionSidecarPath(workout).isEmpty());
    QVERIFY(writeFile(workout, QByteArrayLiteral("workout")));
    QVERIFY(writeFile(course, QByteArrayLiteral("course")));
    QVERIFY(writeFile(sidecar, QByteArrayLiteral("course metadata")));

    FakeDatabase database;
    database.rows << workout;
    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << workout, database.operations());

    QVERIFY2(result.succeeded, qPrintable(result.errorMessage));
    QVERIFY(!QFileInfo::exists(workout));
    QVERIFY(QFileInfo::exists(course));
    QCOMPARE(readFile(sidecar), QByteArrayLiteral("course metadata"));
}

void TestWorkoutDeletionService::deletesWorkoutAndSidecarAfterCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("trail.crs"));
    const QString sidecar = workoutDeletionSidecarPath(workout);
    QVERIFY(writeFile(workout, QByteArrayLiteral("course")));
    QVERIFY(writeFile(sidecar, QByteArrayLiteral("metadata")));

    FakeDatabase database;
    database.rows << workout;
    WorkoutDeletionOperations operations = database.operations();
    int renameCount = 0;
    const auto realRename = operations.renameFile;
    operations.renameFile = [&renameCount, realRename](
                                const QString &from,
                                const QString &to,
                                QString &error) {
        ++renameCount;
        if (QFileInfo(from).absolutePath()
                != QFileInfo(to).absolutePath()) {
            error = QStringLiteral("cross-directory staging attempted");
            return false;
        }
        return realRename(from, to, error);
    };
    const auto realRemove = operations.removeFile;
    operations.removeFile = [&database, realRemove](
                                const QString &path, QString &error) {
        database.events.append(QStringLiteral("file-remove:%1").arg(path));
        return realRemove(path, error);
    };

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(QStringList() << workout, operations);

    QVERIFY2(result.succeeded, qPrintable(result.errorMessage));
    QCOMPARE(result.deletedWorkouts, QStringList() << workout);
    QVERIFY(result.cleanupFailures.isEmpty());
    QVERIFY(!QFileInfo::exists(workout));
    QVERIFY(!QFileInfo::exists(sidecar));
    QVERIFY(stagedEntries(directory).isEmpty());
    QVERIFY(database.rows.isEmpty());
    QCOMPARE(database.beginCalls, 1);
    QCOMPARE(database.commitCalls, 1);
    QCOMPARE(database.rollbackCalls, 0);
    QCOMPARE(renameCount, 2);
    const int commitIndex = database.events.indexOf(QStringLiteral("db-commit"));
    QVERIFY(commitIndex >= 0);
    for (int index = 0; index < database.events.size(); ++index) {
        if (database.events.at(index).startsWith(QStringLiteral("file-remove:"))) {
            QVERIFY(index > commitIndex);
        }
    }
}

void TestWorkoutDeletionService::preservesDatabaseWhenWorkoutStagingFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.erg"));
    const QString second = directory.filePath(QStringLiteral("second.erg"));
    QVERIFY(writeFile(first, QByteArrayLiteral("first")));
    QVERIFY(writeFile(second, QByteArrayLiteral("second")));

    FakeDatabase database;
    database.rows << first << second;
    WorkoutDeletionOperations operations = database.operations();
    const auto realRename = operations.renameFile;
    operations.renameFile = [second, realRename](
                                const QString &from,
                                const QString &to,
                                QString &error) {
        if (from == second) {
            error = QStringLiteral("injected staging failure");
            return false;
        }
        return realRename(from, to, error);
    };

    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << first << second, operations);

    QVERIFY(!result.succeeded);
    QCOMPARE(result.failedPath, second);
    QVERIFY(result.errorMessage.contains(QStringLiteral("injected staging failure")));
    QCOMPARE(database.beginCalls, 0);
    QCOMPARE(database.rows, QStringList({first, second}));
    QCOMPARE(readFile(first), QByteArrayLiteral("first"));
    QCOMPARE(readFile(second), QByteArrayLiteral("second"));
    QVERIFY(stagedEntries(directory).isEmpty());
}

void TestWorkoutDeletionService::restoresWorkoutWhenSidecarStagingFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("trail.crs"));
    const QString sidecar = workoutDeletionSidecarPath(workout);
    QVERIFY(writeFile(workout, QByteArrayLiteral("course")));
    QVERIFY(writeFile(sidecar, QByteArrayLiteral("metadata")));

    FakeDatabase database;
    database.rows << workout;
    WorkoutDeletionOperations operations = database.operations();
    const auto realRename = operations.renameFile;
    operations.renameFile = [sidecar, realRename](
                                const QString &from,
                                const QString &to,
                                QString &error) {
        if (from == sidecar) {
            error = QStringLiteral("sidecar is busy");
            return false;
        }
        return realRename(from, to, error);
    };

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(QStringList() << workout, operations);

    QVERIFY(!result.succeeded);
    QCOMPARE(result.failedPath, sidecar);
    QCOMPARE(database.beginCalls, 0);
    QCOMPARE(readFile(workout), QByteArrayLiteral("course"));
    QCOMPARE(readFile(sidecar), QByteArrayLiteral("metadata"));
    QVERIFY(stagedEntries(directory).isEmpty());
}

void TestWorkoutDeletionService::restoresFilesWhenTransactionCannotStart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("tempo.mrc"));
    QVERIFY(writeFile(workout, QByteArrayLiteral("tempo")));

    FakeDatabase database;
    database.rows << workout;
    database.beginResult = false;

    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << workout, database.operations());

    QVERIFY(!result.succeeded);
    QVERIFY(result.errorMessage.contains(QStringLiteral("transaction"),
                                         Qt::CaseInsensitive));
    QCOMPARE(database.beginCalls, 1);
    QCOMPARE(database.deleted.size(), 0);
    QCOMPARE(database.rollbackCalls, 0);
    QCOMPARE(readFile(workout), QByteArrayLiteral("tempo"));
    QVERIFY(stagedEntries(directory).isEmpty());
}

void TestWorkoutDeletionService::restoresWholeSelectionWhenDatabaseDeleteFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.erg"));
    const QString second = directory.filePath(QStringLiteral("second.erg"));
    QVERIFY(writeFile(first, QByteArrayLiteral("first")));
    QVERIFY(writeFile(second, QByteArrayLiteral("second")));

    FakeDatabase database;
    database.rows << first << second;
    database.deleteFailure = second;

    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << first << second, database.operations());

    QVERIFY(!result.succeeded);
    QCOMPARE(result.failedPath, second);
    QCOMPARE(database.beginCalls, 1);
    QCOMPARE(database.commitCalls, 0);
    QCOMPARE(database.rollbackCalls, 1);
    QCOMPARE(database.rows, QStringList({first, second}));
    QCOMPARE(readFile(first), QByteArrayLiteral("first"));
    QCOMPARE(readFile(second), QByteArrayLiteral("second"));
    QVERIFY(stagedEntries(directory).isEmpty());
}

void TestWorkoutDeletionService::restoresFilesAndRowsWhenCommitFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("vo2.crs"));
    const QString sidecar = workoutDeletionSidecarPath(workout);
    QVERIFY(writeFile(workout, QByteArrayLiteral("vo2")));
    QVERIFY(writeFile(sidecar, QByteArrayLiteral("metadata")));

    FakeDatabase database;
    database.rows << workout;
    database.commitResult = false;

    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << workout, database.operations());

    QVERIFY(!result.succeeded);
    QVERIFY(result.errorMessage.contains(QStringLiteral("commit"),
                                         Qt::CaseInsensitive));
    QCOMPARE(database.rollbackCalls, 1);
    QCOMPARE(database.rows, QStringList() << workout);
    QCOMPARE(readFile(workout), QByteArrayLiteral("vo2"));
    QCOMPARE(readFile(sidecar), QByteArrayLiteral("metadata"));
    QVERIFY(stagedEntries(directory).isEmpty());
}

void TestWorkoutDeletionService::reportsRollbackFailuresWithoutHidingPrimaryFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("threshold.erg"));
    QVERIFY(writeFile(workout, QByteArrayLiteral("threshold")));

    FakeDatabase database;
    database.rows << workout;
    database.commitResult = false;
    WorkoutDeletionOperations operations = database.operations();
    const auto realRename = operations.renameFile;
    operations.renameFile = [workout, realRename](
                                const QString &from,
                                const QString &to,
                                QString &error) {
        if (to == workout && from != workout) {
            error = QStringLiteral("injected rollback failure");
            return false;
        }
        return realRename(from, to, error);
    };

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(QStringList() << workout, operations);

    QVERIFY(!result.succeeded);
    QVERIFY(result.errorMessage.contains(QStringLiteral("commit"),
                                         Qt::CaseInsensitive));
    QCOMPARE(result.rollbackFailures, QStringList() << workout);
    QVERIFY(!QFileInfo::exists(workout));
    QCOMPARE(stagedEntries(directory).size(), 1);
    QCOMPARE(database.rows, QStringList() << workout);
}

void TestWorkoutDeletionService::reportsCleanupFailureOnlyAfterSuccessfulCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workout = directory.filePath(QStringLiteral("sprint.erg"));
    QVERIFY(writeFile(workout, QByteArrayLiteral("sprint")));

    FakeDatabase database;
    database.rows << workout;
    WorkoutDeletionOperations operations = database.operations();
    operations.removeFile = [&database](const QString &, QString &error) {
        database.events.append(QStringLiteral("file-remove-failed"));
        error = QStringLiteral("injected cleanup failure");
        return false;
    };

    const WorkoutDeletionResult result =
            deleteWorkoutsAtomically(QStringList() << workout, operations);

    QVERIFY(result.succeeded);
    QCOMPARE(result.deletedWorkouts, QStringList() << workout);
    QCOMPARE(result.cleanupFailures, QStringList() << workout);
    QVERIFY(result.warningMessage.contains(QStringLiteral("cleanup"),
                                           Qt::CaseInsensitive));
    QVERIFY(database.rows.isEmpty());
    const int commitIndex = database.events.indexOf(QStringLiteral("db-commit"));
    const int removeIndex = database.events.indexOf(
            QStringLiteral("file-remove-failed"));
    QVERIFY(commitIndex >= 0);
    QVERIFY(removeIndex > commitIndex);
    QCOMPARE(stagedEntries(directory).size(), 1);
}

void TestWorkoutDeletionService::rejectsMissingAndSymbolicLinkInputsBeforeDatabaseWork()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missing = directory.filePath(QStringLiteral("missing.erg"));

    FakeDatabase missingDatabase;
    missingDatabase.rows << missing;
    const WorkoutDeletionResult missingResult = deleteWorkoutsAtomically(
            QStringList() << missing, missingDatabase.operations());
    QVERIFY(!missingResult.succeeded);
    QCOMPARE(missingResult.failedPath, missing);
    QCOMPARE(missingDatabase.beginCalls, 0);

    const QString target = directory.filePath(QStringLiteral("target.erg"));
    const QString link = directory.filePath(QStringLiteral("link.erg"));
    QVERIFY(writeFile(target, QByteArrayLiteral("target")));
    if (!QFile::link(target, link)) {
        QSKIP("This filesystem does not support symbolic links");
    }

    FakeDatabase linkDatabase;
    linkDatabase.rows << link;
    const WorkoutDeletionResult linkResult = deleteWorkoutsAtomically(
            QStringList() << link, linkDatabase.operations());
    QVERIFY(!linkResult.succeeded);
    QCOMPARE(linkResult.failedPath, link);
    QCOMPARE(linkDatabase.beginCalls, 0);
    QVERIFY(QFileInfo::exists(link));
    QVERIFY(QFileInfo::exists(target));
}

void TestWorkoutDeletionService::deDuplicatesSelectionWithoutChangingOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.erg"));
    const QString second = directory.filePath(QStringLiteral("second.erg"));
    QVERIFY(writeFile(first, QByteArrayLiteral("first")));
    QVERIFY(writeFile(second, QByteArrayLiteral("second")));

    FakeDatabase database;
    database.rows << first << second;
    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << second << first << second,
            database.operations());

    QVERIFY2(result.succeeded, qPrintable(result.errorMessage));
    QCOMPARE(result.deletedWorkouts, QStringList({second, first}));
    QCOMPARE(database.deleted, QStringList({second, first}));
}

void TestWorkoutDeletionService::preservesRelativeDatabaseKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString previousDirectory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(directory.path()));

    const QString workout = QStringLiteral("relative.erg");
    QVERIFY(writeFile(workout, QByteArrayLiteral("relative")));
    FakeDatabase database;
    database.rows << workout;

    const WorkoutDeletionResult result = deleteWorkoutsAtomically(
            QStringList() << workout, database.operations());

    QVERIFY(QDir::setCurrent(previousDirectory));
    QVERIFY2(result.succeeded, qPrintable(result.errorMessage));
    QCOMPARE(result.deletedWorkouts, QStringList() << workout);
    QCOMPARE(database.deleted, QStringList() << workout);
    QVERIFY(!QFileInfo(directory.filePath(workout)).exists());
}

QTEST_APPLESS_MAIN(TestWorkoutDeletionService)

#include "testWorkoutDeletionService.moc"
