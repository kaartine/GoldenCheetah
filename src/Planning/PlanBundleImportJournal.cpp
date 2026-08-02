/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "PlanBundleImportJournal.h"

#include "AtomicFileWriter.h"
#include "PlanReplacementJournal.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <exception>
#include <utility>

namespace PlanBundleImport {
namespace {

bool normalizeRoot(
    const QString &candidate,
    QString &root,
    QString &error)
{
    root.clear();
    if (candidate.isEmpty()
        || !QDir::isAbsolutePath(candidate)) {
        error = QStringLiteral(
            "A plan import root must be an absolute path");
        return false;
    }
    const QFileInfo info(candidate);
    if (!info.exists() || !info.isDir()) {
        error = QStringLiteral(
            "A plan import root is unavailable");
        return false;
    }
    const QString canonical = QDir::cleanPath(
        info.canonicalFilePath());
    if (canonical.isEmpty()) {
        error = QStringLiteral(
            "A plan import root cannot be resolved");
        return false;
    }
    root = canonical;
    return true;
}

bool databaseIsUsable(TrainDB *database, QString &error)
{
    if (!database
        || QThread::currentThread() != database->thread()) {
        error = QStringLiteral(
            "Plan import recovery must run on the workout database thread");
        return false;
    }
    const TrainDB::SchemaStatus status =
        database->schemaStatus();
    if (status != TrainDB::SchemaStatus::current
        && status != TrainDB::SchemaStatus::migrationReady) {
        error = QStringLiteral(
            "The workout database is unavailable for plan import");
        return false;
    }
    return true;
}

QString planJournalPath(
    const QString &athleteRoot,
    const QString &transactionId)
{
    return QDir(athleteRoot).filePath(
        QStringLiteral(
            ".gc-transactions/plan-replacement/%1")
            .arg(transactionId));
}

bool planJournalIdentity(
    const QString &athleteRoot,
    const QString &path,
    QString &transactionId,
    QString &error)
{
    transactionId.clear();
    const QFileInfo info(path);
    const QString id = info.fileName();
    const QString expected = planJournalPath(
        athleteRoot, id);
    if (id.isEmpty() || info.isSymLink() || !info.isDir()
        || atomicFilePathKey(info.absoluteFilePath())
            != atomicFilePathKey(expected)) {
        error = QStringLiteral(
            "The staged plan journal is outside the athlete transaction namespace");
        return false;
    }
    transactionId = id;
    return true;
}

bool workoutTargetPath(
    const QString &workoutRoot,
    const QString &targetName,
    QString &targetPath,
    QString &error)
{
    targetPath.clear();
    if (!atomicFileNameIsPortableComponent(targetName)) {
        error = QStringLiteral(
            "A plan import workout target name is invalid");
        return false;
    }
    targetPath = QDir(workoutRoot).filePath(targetName);
    const QFileInfo parent(QFileInfo(targetPath).absolutePath());
    if (!parent.exists() || !parent.isDir()
        || atomicFilePathKey(parent.canonicalFilePath())
            != atomicFilePathKey(workoutRoot)) {
        error = QStringLiteral(
            "The workout library changed during plan import");
        targetPath.clear();
        return false;
    }
    return true;
}

bool targetMatchesWorkout(
    const QString &targetPath,
    const TrainDB::PlanImportWorkout &workout,
    bool &matches,
    QString &error)
{
    matches = false;
    AtomicFileSnapshot snapshot;
    if (!captureAtomicFileSnapshot(
            targetPath, snapshot, error)) {
        return false;
    }
    matches = snapshot.size == workout.contents.size()
        && snapshot.digest == workout.digest;
    if (!matches) {
        error = QStringLiteral(
            "A plan import workout target has different contents");
    }
    return matches;
}

bool publishWorkouts(
    const TrainDB::PlanImportJournal &journal,
    AtomicFileLockSet &locks,
    QString &error)
{
    QString normalizedWorkoutRoot;
    if (!normalizeRoot(
            journal.workoutRoot,
            normalizedWorkoutRoot, error)
        || atomicFilePathKey(normalizedWorkoutRoot)
            != atomicFilePathKey(journal.workoutRoot)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The workout library changed during plan recovery");
        }
        return false;
    }

    QStringList targetPaths;
    QSet<QString> targetKeys;
    targetPaths.reserve(journal.workouts.size());
    for (const TrainDB::PlanImportWorkout &workout :
         journal.workouts) {
        QString targetPath;
        if (!workoutTargetPath(
                normalizedWorkoutRoot,
                workout.targetFileName,
                targetPath, error)) {
            return false;
        }
        const QString key = atomicFilePathKey(targetPath);
        if (targetKeys.contains(key)
            || workout.digest.size()
                != QCryptographicHash::hashLength(
                    QCryptographicHash::Sha256)
            || QCryptographicHash::hash(
                   workout.contents,
                   QCryptographicHash::Sha256)
                != workout.digest) {
            error = QStringLiteral(
                "A plan import workout payload is invalid");
            return false;
        }
        targetKeys.insert(key);
        targetPaths.append(targetPath);
    }

    if (!targetPaths.isEmpty()
        && !locks.lock(targetPaths, error)) {
        error = QStringLiteral(
            "A workout import target is already being changed: %1")
                    .arg(error);
        return false;
    }

    for (qsizetype index = 0;
         index < journal.workouts.size(); ++index) {
        const TrainDB::PlanImportWorkout &workout =
            journal.workouts.at(index);
        const QString &targetPath = targetPaths.at(index);
        const QFileInfo targetInfo(targetPath);
        if (targetInfo.isSymLink()
            || (targetInfo.exists() && !targetInfo.isFile())) {
            error = QStringLiteral(
                "A plan import workout target is unsafe");
            return false;
        }
        if (targetInfo.exists()) {
            bool matches = false;
            if (!targetMatchesWorkout(
                    targetPath, workout, matches, error)
                || !matches) {
                return false;
            }
            continue;
        }

        NewAtomicFileWriter writer(targetPath);
        if (!writer.open()
            || writer.write(workout.contents)
                != workout.contents.size()
            || !writer.flush()
            || !writer.commit()) {
            error = atomicFileError(
                QStringLiteral(
                    "Cannot publish a plan import workout"),
                writer);
            return false;
        }
        if (!syncParentDirectory(targetPath, error))
            return false;
        bool matches = false;
        if (!targetMatchesWorkout(
                targetPath, workout, matches, error)
            || !matches) {
            return false;
        }
    }
    return true;
}

bool loadDecision(
    TrainDB *database,
    const QString &athleteRoot,
    TrainDB::PlanImportJournal &journal,
    bool &found,
    QString &error)
{
    if (!databaseIsUsable(database, error)) return false;
    return database->loadPlanImportJournal(
        athleteRoot, journal, found, error);
}

} // namespace

Journal::Journal(
    TrainDB *database,
    TrainDB::PlanImportJournal journal)
    : database_(database),
      journal_(std::move(journal))
{
}

std::shared_ptr<Journal> Journal::create(
    TrainDB *database,
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    QString &error)
{
    error.clear();
    if (!databaseIsUsable(database, error)) return {};

    TrainDB::PlanImportJournal journal;
    if (!normalizeRoot(
            athleteRoot, journal.athleteRoot, error)
        || !normalizeRoot(
            workoutRoot, journal.workoutRoot, error)) {
        return {};
    }
    TrainDB::PlanImportJournal existing;
    bool found = false;
    if (!database->loadPlanImportJournal(
            journal.athleteRoot,
            existing, found, error)) {
        return {};
    }
    if (found) {
        error = QStringLiteral(
            "A previous plan import must be recovered before another can start");
        return {};
    }

    journal.id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QSet<QString> targetKeys;
    for (TrainDB::PlanImportWorkout workout : workouts) {
        if (workout.contents.isEmpty()) {
            error = QStringLiteral(
                "A plan import workout is empty");
            return {};
        }
        QString targetPath;
        if (!workoutTargetPath(
                journal.workoutRoot,
                workout.targetFileName,
                targetPath, error)) {
            return {};
        }
        const QString key = atomicFilePathKey(targetPath);
        const QFileInfo targetInfo(targetPath);
        if (targetKeys.contains(key)
            || targetInfo.exists() || targetInfo.isSymLink()) {
            error = QStringLiteral(
                "A plan import workout target already exists or is duplicated");
            return {};
        }
        targetKeys.insert(key);
        workout.digest = QCryptographicHash::hash(
            workout.contents, QCryptographicHash::Sha256);
        journal.workouts.append(std::move(workout));
    }
    return std::shared_ptr<Journal>(
        new Journal(database, std::move(journal)));
}

bool Journal::commitDecision(
    const QString &planJournalPath,
    bool &committed,
    QString &error)
{
    committed = false;
    error.clear();
    if (!databaseIsUsable(database_.data(), error))
        return false;
    QString transactionId;
    if (!planJournalIdentity(
            journal_.athleteRoot,
            planJournalPath,
            transactionId, error)) {
        return false;
    }
    journal_.planJournalId = transactionId;
    if (!database_->commitPlanImportJournal(
            journal_, committed, error)) {
        return false;
    }
    return committed;
}

bool Journal::completeRecord(
    const DatabaseCompletion &completeDatabase,
    QString &error)
{
    error.clear();
    if (!databaseIsUsable(database_.data(), error)
        || !completeDatabase) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The plan import database completion is unavailable");
        }
        return false;
    }
    AtomicFileLockSet workoutLocks;
    if (!publishWorkouts(
            journal_, workoutLocks, error)) {
        return false;
    }
    bool databaseCompleted = false;
    try {
        databaseCompleted = completeDatabase(journal_, error);
    } catch (const QString &detail) {
        error = detail;
    } catch (const std::exception &exception) {
        error = QString::fromLocal8Bit(exception.what());
    } catch (...) {
        error = QStringLiteral(
            "The plan import database transaction failed");
    }
    if (!databaseCompleted) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The plan import database transaction failed");
        }
        return false;
    }
    if (!databaseIsUsable(database_.data(), error))
        return false;

    TrainDB::PlanImportJournal remaining;
    bool found = false;
    if (!database_->loadPlanImportJournal(
            journal_.athleteRoot,
            remaining, found, error)) {
        return false;
    }
    if (found) {
        error = QStringLiteral(
            "The plan import decision was not retired atomically with its workout rows");
        return false;
    }
    return true;
}

bool Journal::completePublishedPlan(
    const DatabaseCompletion &completeDatabase,
    QString &error)
{
    TrainDB::PlanImportJournal loaded;
    bool found = false;
    if (!loadDecision(
            database_.data(), journal_.athleteRoot,
            loaded, found, error)) {
        return false;
    }
    if (!found) return true;
    if (loaded.id != journal_.id
        || loaded.planJournalId != journal_.planJournalId
        || atomicFilePathKey(loaded.workoutRoot)
            != atomicFilePathKey(journal_.workoutRoot)) {
        error = QStringLiteral(
            "The durable plan import decision changed unexpectedly");
        return false;
    }
    journal_ = std::move(loaded);
    return completeRecord(completeDatabase, error);
}

bool Journal::reconcileAll(
    TrainDB *database,
    const QString &athleteRoot,
    const QString &workoutRoot,
    const DatabaseCompletion &completeDatabase,
    QString &error)
{
    error.clear();
    if (!databaseIsUsable(database, error)) return false;
    QString normalizedAthleteRoot;
    QString normalizedWorkoutRoot;
    if (!normalizeRoot(
            athleteRoot, normalizedAthleteRoot, error)
        || !normalizeRoot(
            workoutRoot, normalizedWorkoutRoot, error)) {
        return false;
    }

    TrainDB::PlanImportJournal journalRecord;
    bool found = false;
    if (!database->loadPlanImportJournal(
            normalizedAthleteRoot,
            journalRecord, found, error)) {
        return false;
    }
    if (!found) return true;
    if (atomicFilePathKey(journalRecord.athleteRoot)
            != atomicFilePathKey(normalizedAthleteRoot)
        || atomicFilePathKey(journalRecord.workoutRoot)
            != atomicFilePathKey(normalizedWorkoutRoot)) {
        error = QStringLiteral(
            "The plan import roots changed before recovery");
        return false;
    }

    std::shared_ptr<PlanReplacement::Journal> plan =
        PlanReplacement::Journal::openPrepared(
            normalizedAthleteRoot,
            journalRecord.planJournalId,
            error);
    if (!plan || !plan->publishAndCommit(error)
        || !plan->hasCommitMarker()) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The staged plan generation could not be committed");
        }
        return false;
    }

    Journal coordinator(database, journalRecord);
    if (!coordinator.completeRecord(
            completeDatabase, error)) {
        return false;
    }

    QString cleanupError;
    if (!plan->cleanupAfterCommit(cleanupError)) {
        qWarning().noquote()
            << "Committed plan import cleanup will be retried:"
            << cleanupError;
    }
    return true;
}

} // namespace PlanBundleImport
