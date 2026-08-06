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
#include "FileIO/AnchoredFileSystem.h"

#include <QPointer>

#include <functional>
#include <memory>

namespace PlanBundleImport {

using DatabaseCompletion = std::function<bool(
    const TrainDB::PlanImportJournal &journal,
    QString &error)>;
using CancellationCheck = std::function<bool()>;
// A validator passed to BoundDatabaseCompletion is a synchronous capability:
// retained copies expire when the completion callback returns.
using PublishedValidation = std::function<bool(QString &error)>;
using BoundDatabaseCompletion = std::function<bool(
    const TrainDB::PlanImportJournal &journal,
    const PublishedValidation &validatePublished,
    QString &error)>;

struct PublishedTargets;
struct DecisionMarker;

class Journal
{
public:
    static std::shared_ptr<Journal> create(
        TrainDB *database,
        const QString &athleteRoot,
        const QString &workoutRoot,
        const QList<TrainDB::PlanImportWorkout> &workouts,
        QString &error);
    static std::shared_ptr<Journal> createStandalone(
        TrainDB *database,
        const QString &athleteRoot,
        const QString &workoutRoot,
        const QList<TrainDB::PlanImportWorkout> &workouts,
        QString &error);
    static std::shared_ptr<Journal> createStandalonePrepared(
        const QString &athleteRoot,
        const QString &workoutRoot,
        const QList<TrainDB::PlanImportWorkout> &workouts,
        QString &error);
    static std::shared_ptr<Journal> createStandalonePrepared(
        const QString &athleteRoot,
        const QString &workoutRoot,
        const AnchoredFileSystem::DirectoryAnchor &workoutRootAnchor,
        const QList<TrainDB::PlanImportWorkout> &workouts,
        QString &error);

    static bool reconcileAll(
        TrainDB *database,
        const QString &athleteRoot,
        const QString &workoutRoot,
        const BoundDatabaseCompletion &completeDatabase,
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
    bool commitStandaloneDecision(
        bool &committed,
        QString &error);
    bool commitStandaloneDecision(
        TrainDB *database,
        bool &committed,
        QString &error);
    bool commitStandaloneDecision(
        const QString &databasePath,
        const CancellationCheck &cancelled,
        bool &committed,
        QString &error);
    bool commitStandaloneDecision(
        const QString &databasePath,
        const CancellationCheck &cancelled,
        const std::shared_ptr<TrainDatabaseFileGeneration>
            &databaseGeneration,
        bool &committed,
        QString &error);
    bool publishPreparedFiles(
        const CancellationCheck &cancelled,
        QString &error);
    bool completePublishedDatabase(
        const DatabaseCompletion &completeDatabase,
        QString &error);
    bool completePublishedDatabaseBound(
        const BoundDatabaseCompletion &completeDatabase,
        QString &error);
    bool bindDatabase(TrainDB *database, QString &error);
    bool completePublishedPlan(
        const BoundDatabaseCompletion &completeDatabase,
        QString &error);
    bool completePublishedPlan(
        const DatabaseCompletion &completeDatabase,
        QString &error);

private:
    Journal(
        TrainDB *database,
        TrainDB::PlanImportJournal journal,
        bool standalone = false,
        bool decisionCommitted = false);

    bool completeRecord(
        const BoundDatabaseCompletion &completeDatabase,
        QString &error);
    bool prepareDecisionMarker(
        const QString &databasePath,
        const std::shared_ptr<TrainDatabaseFileGeneration>
            &databaseGeneration,
        QString &error);
    bool loadDecisionMarker(
        const QString &databasePath,
        const std::shared_ptr<TrainDatabaseFileGeneration>
            &databaseGeneration,
        QString &error);
    bool decisionGenerationMatches(QString &error) const;
    bool retireDecisionMarker(QString &error);
    void discardUncommittedDecisionMarker();

    QPointer<TrainDB> database_;
    TrainDB::PlanImportJournal journal_;
    bool standalone_ = false;
    bool decisionCommitted_ = false;
    AnchoredFileSystem::DirectoryAnchor workoutRootAnchor_;
    std::shared_ptr<PublishedTargets> publishedTargets_;
    QString databasePath_;
    std::shared_ptr<TrainDatabaseFileGeneration> databaseGeneration_;
    QByteArray databaseFingerprint_;
    std::shared_ptr<DecisionMarker> decisionMarker_;
};

} // namespace PlanBundleImport

#endif // _GC_PlanBundleImportJournal_h
