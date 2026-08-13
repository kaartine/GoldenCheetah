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
#include "AnchoredFileSystem.h"
#include "PlanReplacementJournal.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <exception>
#include <utility>
#include <vector>

#ifdef GC_PLAN_BUNDLE_IMPORT_TEST_HOOKS
extern void planBundleImportPublicationChunkTestHook();
extern void planBundleImportPublicationMutationTestHook();
#endif

namespace PlanBundleImport {

struct PublishedTargets
{
    struct Target
    {
        AnchoredFileSystem::EntryRef entry;
        AnchoredFileSystem::PinnedFile file;
        qint64 expectedSize = -1;
        QByteArray expectedDigest;
    };

    bool validate(QString &error) const
    {
        error.clear();
        if (!root.isValid() || !root.pathMatches(error)) return false;
        for (const Target &target : targets) {
            bool matches = false;
            if (!target.entry.isValid()
                || !target.file.isValid()
                || target.file.size() != target.expectedSize
                || target.file.sha256() != target.expectedDigest
                || !AnchoredFileSystem::entryMatches(
                    target.entry, target.file, matches, error)
                || !matches) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "A published workout was replaced before database completion");
                }
                return false;
            }
        }
        return root.pathMatches(error);
    }

    AnchoredFileSystem::DirectoryAnchor root;
    AtomicFileLockSet locks;
    std::vector<Target> targets;
};

struct DecisionMarker
{
    bool validate(const QByteArray &expected, QString &error) const
    {
        bool matches = false;
        return entry.isValid()
            && file.isValid()
            && payload == expected
            && file.size() == expected.size()
            && file.sha256() == QCryptographicHash::hash(
                expected, QCryptographicHash::Sha256)
            && AnchoredFileSystem::entryMatches(
                entry, file, matches, error)
            && matches;
    }

    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile file;
    QByteArray payload;
};

namespace {

const QString StandaloneRootIdentityFile =
    QStringLiteral(".gc-workout-root-id");
const QString DecisionMarkerPrefix =
    QStringLiteral(".gc-plan-import-decision-");
const QString ImportArtifactPrefix =
    QStringLiteral(".gc-plan-import-");
const QByteArray DecisionMarkerMagic("GCPI1");
constexpr qint64 MaximumDecisionMarkerSize = 256;

QString decisionMarkerName(const QString &athleteRoot)
{
    return DecisionMarkerPrefix
        + QString::fromLatin1(QCryptographicHash::hash(
            athleteRoot.toUtf8(),
            QCryptographicHash::Sha256).toHex());
}

QString publicationArtifactName(
    const QString &journalId,
    qsizetype sequence,
    bool predecessor)
{
    return QStringLiteral(".gc-plan-import-%1-%2.%3")
        .arg(journalId)
        .arg(sequence)
        .arg(predecessor
            ? QStringLiteral("old")
            : QStringLiteral("new"));
}

QString publicationStagingName(
    const TrainDB::PlanImportJournal &journal,
    qsizetype sequence,
    bool replaceExisting)
{
#ifdef Q_OS_WIN
    Q_UNUSED(replaceExisting)
    return publicationArtifactName(journal.id, sequence, false);
#else
    return publicationArtifactName(
        journal.id, sequence, replaceExisting);
#endif
}

QString predecessorArtifactName(
    const TrainDB::PlanImportJournal &journal,
    qsizetype sequence)
{
    return publicationArtifactName(journal.id, sequence, true);
}

QByteArray pathDigest(const QString &path)
{
    return QCryptographicHash::hash(
        path.toUtf8(), QCryptographicHash::Sha256);
}

QByteArray decisionMarkerPayload(
    const TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &root,
    const QByteArray &databaseFingerprint)
{
    const QByteArray rootFingerprint =
        root.identity().persistentFingerprint();
    if (QUuid(journal.id).isNull()
        || pathDigest(journal.athleteRoot).size() != 32
        || rootFingerprint.size() != 32
        || databaseFingerprint.size() != 32) {
        return {};
    }
    return DecisionMarkerMagic + '\n'
        + journal.id.toLatin1() + '\n'
        + pathDigest(journal.athleteRoot).toHex() + '\n'
        + rootFingerprint.toHex() + '\n'
        + databaseFingerprint.toHex() + '\n';
}

bool decisionGenerationMatchesSnapshot(
    const TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &workoutRoot,
    const QString &databasePath,
    const std::shared_ptr<TrainDatabaseFileGeneration>
        &databaseGeneration,
    const QByteArray &databaseFingerprint,
    const std::shared_ptr<DecisionMarker> &decisionMarker,
    QString &error)
{
    if (databasePath.isEmpty()
        || !databaseGeneration
        || databaseFingerprint.size() != 32
        || TrainDB::databaseFileGenerationFingerprint(
               databaseGeneration) != databaseFingerprint
        || !TrainDB::databaseFileGenerationMatches(
            databasePath, databaseGeneration, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import database generation changed");
        }
        return false;
    }
    const QByteArray expected = decisionMarkerPayload(
        journal, workoutRoot, databaseFingerprint);
    if (!decisionMarker
        || expected.isEmpty()
        || !decisionMarker->validate(expected, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import decision marker changed");
        }
        return false;
    }
    return workoutRoot.pathMatches(error);
}

struct PublishedValidationState
{
    bool validate(QString &error)
    {
        error.clear();
        if (!active.load(std::memory_order_acquire)) {
            error = QStringLiteral(
                "Published workout validation is only valid during synchronous database completion");
            return false;
        }
        if (QThread::currentThreadId() != ownerThreadId) {
            error = QStringLiteral(
                "Published workout validation must run synchronously on the bound database thread");
            return false;
        }
        called = true;
        TrainDB *boundDatabase = database.data();
        if (!boundDatabase || !boundDatabase->hasActiveLUW()) {
            error = QStringLiteral(
                "Published workout validation must run inside the bound database transaction");
            succeeded = false;
            return false;
        }
        succeeded = targets
            && targets->validate(error)
            && decisionGenerationMatchesSnapshot(
                journal, workoutRoot, databasePath,
                databaseGeneration, databaseFingerprint,
                decisionMarker, error);
        return succeeded;
    }

    void expire()
    {
        active.store(false, std::memory_order_release);
        database.clear();
        targets.reset();
        journal = {};
        workoutRoot = {};
        databasePath.clear();
        databaseGeneration.reset();
        databaseFingerprint.clear();
        decisionMarker.reset();
    }

    std::atomic_bool active {true};
    bool called = false;
    bool succeeded = false;
    Qt::HANDLE ownerThreadId = nullptr;
    QPointer<TrainDB> database;
    std::shared_ptr<PublishedTargets> targets;
    TrainDB::PlanImportJournal journal;
    AnchoredFileSystem::DirectoryAnchor workoutRoot;
    QString databasePath;
    std::shared_ptr<TrainDatabaseFileGeneration> databaseGeneration;
    QByteArray databaseFingerprint;
    std::shared_ptr<DecisionMarker> decisionMarker;
};

bool parseDecisionMarkerPayload(
    const QByteArray &payload,
    QString &journalId,
    QByteArray &athleteDigest,
    QByteArray &rootFingerprint,
    QByteArray &databaseFingerprint)
{
    journalId.clear();
    athleteDigest.clear();
    rootFingerprint.clear();
    databaseFingerprint.clear();
    const QList<QByteArray> lines = payload.split('\n');
    if (lines.size() != 6
        || lines.at(0) != DecisionMarkerMagic
        || !lines.at(5).isEmpty()) {
        return false;
    }
    journalId = QString::fromLatin1(lines.at(1));
    athleteDigest = QByteArray::fromHex(lines.at(2));
    rootFingerprint = QByteArray::fromHex(lines.at(3));
    databaseFingerprint = QByteArray::fromHex(lines.at(4));
    return !QUuid(journalId).isNull()
        && athleteDigest.size() == 32
        && rootFingerprint.size() == 32
        && databaseFingerprint.size() == 32
        && lines.at(2) == athleteDigest.toHex()
        && lines.at(3) == rootFingerprint.toHex()
        && lines.at(4) == databaseFingerprint.toHex();
}

QString standaloneRootIdentityToken(
    const QString &marker,
    const AnchoredFileSystem::NativeIdentity &rootIdentity,
    const AnchoredFileSystem::NativeIdentity &markerIdentity)
{
    const QByteArray rootFingerprint =
        rootIdentity.persistentFingerprint().toHex();
    const QByteArray markerFingerprint =
        markerIdentity.persistentFingerprint().toHex();
    if (rootFingerprint.size() != 64
        || markerFingerprint.size() != 64) {
        return {};
    }
    return marker
        + QLatin1Char('.') + QString::fromLatin1(rootFingerprint)
        + QLatin1Char('.') + QString::fromLatin1(markerFingerprint);
}

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

bool anchorNamesRoot(
    const AnchoredFileSystem::DirectoryAnchor &anchor,
    const QString &rootPath,
    QString &error)
{
    if (!anchor.isValid() || !anchor.pathMatches(error)) return false;
    AnchoredFileSystem::DirectoryAnchor namedRoot;
    return AnchoredFileSystem::DirectoryAnchor::open(
               rootPath, namedRoot, error)
        && namedRoot.pathMatches(error)
        && namedRoot.identity() == anchor.identity();
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
    if (!atomicFileNameIsPortableComponent(targetName)
        || targetName.startsWith(
            ImportArtifactPrefix, Qt::CaseInsensitive)) {
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

bool standaloneRootIdentity(
    const AnchoredFileSystem::DirectoryAnchor &root,
    bool create,
    bool requireControlledRoot,
    QString &identity,
    bool &found,
    QString &error)
{
    identity.clear();
    found = false;
    if (!root.isValid()
        || !root.pathMatches(error)
        || (requireControlledRoot
            && !AnchoredFileSystem::validateCurrentUserControlledDirectory(
                root, error))) {
        return false;
    }
    const AnchoredFileSystem::EntryRef marker =
        root.entry(StandaloneRootIdentityFile, error);
    bool exists = false;
    if (!marker.isValid()
        || !AnchoredFileSystem::entryExists(
            marker, exists, error)) {
        return false;
    }

    QByteArray contents;
    AnchoredFileSystem::PinnedFile pinned;
    if (exists) {
        if (!AnchoredFileSystem::pinRegularFile(
                marker, pinned, error, 36)
            || !AnchoredFileSystem::readAll(
                pinned, 36, contents, error)) {
            return false;
        }
        bool matches = false;
        if (!AnchoredFileSystem::entryMatches(
                marker, pinned, matches, error)
            || !matches) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The workout root identity changed unexpectedly");
            }
            return false;
        }
    } else if (create) {
        identity = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        contents = identity.toLatin1();
        if (!AnchoredFileSystem::writeNewFile(
                contents, marker, pinned, error)) {
            return false;
        }
    } else {
        return true;
    }

    const QString stored = QString::fromLatin1(contents);
    const QUuid parsed(stored);
    if (parsed.isNull()
        || parsed.toString(QUuid::WithoutBraces).toLower()
            != stored) {
        error = QStringLiteral(
            "The workout root identity is invalid");
        return false;
    }
    identity = standaloneRootIdentityToken(
        stored, root.identity(), pinned.identity());
    if (identity.isEmpty()) {
        error = QStringLiteral(
            "The workout root identity is unavailable");
        return false;
    }
    found = true;
    return root.pathMatches(error);
}

bool standaloneRootIdentity(
    const QString &workoutRoot,
    bool create,
    bool requireControlledRoot,
    QString &identity,
    bool &found,
    QString &error)
{
    AnchoredFileSystem::DirectoryAnchor root;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            workoutRoot, root, error)) {
        return false;
    }
    return standaloneRootIdentity(
        root, create, requireControlledRoot,
        identity, found, error);
}

bool openDecisionMarker(
    const TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &root,
    const QByteArray &databaseFingerprint,
    std::shared_ptr<DecisionMarker> &marker,
    QString &error)
{
    marker.reset();
    const QByteArray expected = decisionMarkerPayload(
        journal, root, databaseFingerprint);
    const AnchoredFileSystem::EntryRef entry = root.entry(
        decisionMarkerName(journal.athleteRoot), error);
    bool exists = false;
    auto loaded = std::make_shared<DecisionMarker>();
    if (expected.isEmpty()
        || !entry.isValid()
        || !AnchoredFileSystem::entryExists(entry, exists, error)
        || !exists
        || !AnchoredFileSystem::pinRegularFile(
            entry, loaded->file, error, MaximumDecisionMarkerSize)
        || !AnchoredFileSystem::readAll(
            loaded->file, MaximumDecisionMarkerSize,
            loaded->payload, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import decision marker is unavailable");
        }
        return false;
    }
    loaded->entry = entry;
    if (!loaded->validate(expected, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import decision marker changed");
        }
        return false;
    }
    marker = std::move(loaded);
    return true;
}

bool createDecisionMarker(
    const TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &root,
    const QByteArray &databaseFingerprint,
    std::shared_ptr<DecisionMarker> &marker,
    QString &error)
{
    marker.reset();
    const QByteArray payload = decisionMarkerPayload(
        journal, root, databaseFingerprint);
    const AnchoredFileSystem::EntryRef entry = root.entry(
        decisionMarkerName(journal.athleteRoot), error);
    bool exists = false;
    if (payload.isEmpty()
        || !entry.isValid()
        || !AnchoredFileSystem::entryExists(entry, exists, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import decision marker is invalid");
        }
        return false;
    }
    if (exists) {
        return openDecisionMarker(
            journal, root, databaseFingerprint, marker, error);
    }

    auto created = std::make_shared<DecisionMarker>();
    created->entry = entry;
    created->payload = payload;
    if (!AnchoredFileSystem::writeNewFile(
            payload, entry, created->file, error)
        || !created->validate(payload, error)
        || !root.sync(error)
        || !root.pathMatches(error)) {
        if (created->file.isValid())
            AnchoredFileSystem::remove(created->file);
        return false;
    }
    marker = std::move(created);
    return true;
}

bool removeDecisionMarker(
    const AnchoredFileSystem::DirectoryAnchor &root,
    std::shared_ptr<DecisionMarker> &marker,
    QString &error)
{
    if (!marker) return true;
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(marker->file);
    if (!removal.applied()) {
        error = removal.error.isEmpty()
            ? QStringLiteral(
                "The durable plan import decision marker could not be retired")
            : removal.error;
        return false;
    }
    bool exists = true;
    if (!root.sync(error)
        || !AnchoredFileSystem::entryExists(
            marker->entry, exists, error)
        || exists
        || !root.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable plan import decision marker retirement is not durable");
        }
        return false;
    }
    marker.reset();
    return true;
}

struct ObservedDecisionMarker
{
    QString journalId;
    QByteArray databaseFingerprint;
    std::shared_ptr<DecisionMarker> marker;
};

bool decisionMarkersForAthlete(
    const AnchoredFileSystem::DirectoryAnchor &root,
    const QString &athleteRoot,
    QList<ObservedDecisionMarker> &markers,
    QString &error)
{
    markers.clear();
    const AnchoredFileSystem::EntryRef markerEntry = root.entry(
        decisionMarkerName(athleteRoot), error);
    bool exists = false;
    if (!markerEntry.isValid()
        || !AnchoredFileSystem::entryExists(
            markerEntry, exists, error)) {
        return false;
    }
    if (!exists) return root.pathMatches(error);
    const QByteArray expectedAthleteDigest = pathDigest(athleteRoot);
    const QByteArray expectedRootFingerprint =
        root.identity().persistentFingerprint();
    auto marker = std::make_shared<DecisionMarker>();
    marker->entry = markerEntry;
    if (!AnchoredFileSystem::pinRegularFile(
            markerEntry, marker->file,
            error, MaximumDecisionMarkerSize)
        || !AnchoredFileSystem::readAll(
            marker->file, MaximumDecisionMarkerSize,
            marker->payload, error)) {
        return false;
    }
    QString journalId;
    QByteArray athleteDigest;
    QByteArray rootFingerprint;
    QByteArray databaseFingerprint;
    bool matches = false;
    if (!parseDecisionMarkerPayload(
            marker->payload, journalId, athleteDigest,
            rootFingerprint, databaseFingerprint)
        || athleteDigest != expectedAthleteDigest
        || rootFingerprint != expectedRootFingerprint
        || !marker->validate(marker->payload, error)
        || !AnchoredFileSystem::entryMatches(
            markerEntry, marker->file, matches, error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A durable plan import decision marker is invalid");
        }
        return false;
    }
    ObservedDecisionMarker observed;
    observed.journalId = journalId;
    observed.databaseFingerprint = databaseFingerprint;
    observed.marker = std::move(marker);
    markers.append(std::move(observed));
    return root.pathMatches(error);
}

bool captureAnchoredTarget(
    const AnchoredFileSystem::EntryRef &entry,
    qint64 maximumSize,
    const PlanBundleImport::CancellationCheck &cancelled,
    bool &exists,
    AtomicFileSnapshot &snapshot,
    AnchoredFileSystem::PinnedFile &pinned,
    QString &error)
{
    exists = false;
    snapshot = {};
    pinned = {};
    if (!entry.isValid()
        || !AnchoredFileSystem::entryExists(entry, exists, error)) {
        return false;
    }
    if (!exists) return true;

    bool matches = false;
    if (!AnchoredFileSystem::pinRegularFile(
            entry, pinned, error, maximumSize,
            [&cancelled](qint64, QString &controlError) {
                bool requested = false;
                if (cancelled) {
                    try {
                        requested = cancelled();
                    } catch (...) {
                        requested = true;
                    }
                }
                if (!requested) return true;
                controlError = QStringLiteral(
                    "The workout predecessor validation was cancelled");
                return false;
            })
        || !AnchoredFileSystem::entryMatches(
            entry, pinned, matches, error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan import workout target changed unexpectedly");
        }
        return false;
    }
    snapshot.size = pinned.size();
    snapshot.digest = pinned.sha256();
    return true;
}

bool preparePublicationStaging(
    const AnchoredFileSystem::EntryRef &entry,
    const QByteArray &contents,
    const QByteArray &digest,
    AnchoredFileSystem::PinnedFile &file,
    QString &error)
{
    bool exists = false;
    AtomicFileSnapshot snapshot;
    if (!captureAnchoredTarget(
            entry, contents.size(), {}, exists,
            snapshot, file, error)) {
        return false;
    }
    if (exists) {
        if (snapshot.size == contents.size()
            && snapshot.digest == digest) {
            return true;
        }
        error = QStringLiteral(
            "A persisted plan import staging file was substituted");
        return false;
    }
    return AnchoredFileSystem::writeNewFile(
        contents, entry, file, error);
}

bool removePinnedArtifact(
    const AnchoredFileSystem::DirectoryAnchor &root,
    const AnchoredFileSystem::EntryRef &entry,
    AnchoredFileSystem::PinnedFile &file,
    qint64 expectedSize,
    const QByteArray &expectedDigest,
    const QByteArray &expectedIdentity,
    QString &error)
{
    bool matches = false;
    if (!entry.isValid()
        || !file.isValid()
        || file.size() != expectedSize
        || file.sha256() != expectedDigest
        || (!expectedIdentity.isEmpty()
            && file.identity().persistentFingerprint()
                != expectedIdentity)
        || !AnchoredFileSystem::entryMatches(
            entry, file, matches, error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A persisted plan import artifact was substituted");
        }
        return false;
    }
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(file);
    if (!removal.applied()) {
        error = removal.error.isEmpty()
            ? QStringLiteral(
                "A persisted plan import artifact could not be removed")
            : removal.error;
        return false;
    }
    bool exists = true;
    if (!root.sync(error)
        || !AnchoredFileSystem::entryExists(entry, exists, error)
        || exists
        || !root.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A persisted plan import artifact removal is not durable");
        }
        return false;
    }
    return true;
}

bool removeExactArtifactIfPresent(
    const AnchoredFileSystem::DirectoryAnchor &root,
    const AnchoredFileSystem::EntryRef &entry,
    qint64 expectedSize,
    const QByteArray &expectedDigest,
    const QByteArray &expectedIdentity,
    QString &error)
{
    bool exists = false;
    AtomicFileSnapshot snapshot;
    AnchoredFileSystem::PinnedFile file;
    if (!captureAnchoredTarget(
            entry, expectedSize, {}, exists,
            snapshot, file, error)) {
        return false;
    }
    if (!exists) return true;
    if (snapshot.size != expectedSize
        || snapshot.digest != expectedDigest) {
        error = QStringLiteral(
            "A persisted plan import artifact was substituted");
        return false;
    }
    return removePinnedArtifact(
        root, entry, file, expectedSize, expectedDigest,
        expectedIdentity, error);
}

bool publishWorkouts(
    const TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &workoutRootAnchor,
    std::shared_ptr<PublishedTargets> &publishedTargets,
    const PlanBundleImport::CancellationCheck &cancelled,
    const PlanBundleImport::PublishedValidation &validateDecision,
    QString &error)
{
    publishedTargets.reset();
    const auto cancellationRequested = [&cancelled] {
        if (!cancelled) return false;
        try {
            return cancelled();
        } catch (...) {
            return true;
        }
    };
    if (cancellationRequested()) {
        error = QStringLiteral("The workout publication was cancelled");
        return false;
    }
    if (validateDecision && !validateDecision(error))
        return false;
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
    auto targets = std::make_shared<PublishedTargets>();
    if (workoutRootAnchor.isValid()) {
        if (!anchorNamesRoot(
                workoutRootAnchor, normalizedWorkoutRoot, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The live workout-root generation changed");
            }
            return false;
        }
        targets->root = workoutRootAnchor;
    } else {
        if (!AnchoredFileSystem::DirectoryAnchor::open(
                normalizedWorkoutRoot, targets->root, error)
            || !targets->root.pathMatches(error)) {
            return false;
        }
    }

    QStringList targetPaths;
    QList<AnchoredFileSystem::EntryRef> targetEntries;
    QSet<QString> targetKeys;
    targetPaths.reserve(journal.workouts.size());
    targetEntries.reserve(journal.workouts.size());
    for (const TrainDB::PlanImportWorkout &workout :
         journal.workouts) {
        if (cancellationRequested()) {
            error = QStringLiteral("The workout publication was cancelled");
            return false;
        }
        QString targetPath;
        if (!workoutTargetPath(
                normalizedWorkoutRoot,
                workout.targetFileName,
                targetPath, error)) {
            return false;
        }
        const QString key = atomicFilePathKey(targetPath);
        QCryptographicHash payloadDigest(QCryptographicHash::Sha256);
        constexpr qsizetype ValidationChunkSize = 256 * 1024;
        for (qsizetype offset = 0;
             offset < workout.contents.size();
             offset += ValidationChunkSize) {
#ifdef GC_PLAN_BUNDLE_IMPORT_TEST_HOOKS
            ::planBundleImportPublicationChunkTestHook();
#endif
            if (cancellationRequested()) {
                error = QStringLiteral(
                    "The workout publication was cancelled");
                return false;
            }
            const qsizetype size = qMin(
                ValidationChunkSize,
                workout.contents.size() - offset);
            payloadDigest.addData(QByteArrayView(
                workout.contents.constData() + offset, size));
        }
        if (targetKeys.contains(key)
            || workout.digest.size()
                != QCryptographicHash::hashLength(
                    QCryptographicHash::Sha256)
            || payloadDigest.result() != workout.digest) {
            error = QStringLiteral(
                "A plan import workout payload is invalid");
            return false;
        }
        targetKeys.insert(key);
        targetPaths.append(targetPath);
        const AnchoredFileSystem::EntryRef targetEntry =
            targets->root.entry(workout.targetFileName, error);
        if (!targetEntry.isValid()) return false;
        targetEntries.append(targetEntry);
    }

    if (!targetPaths.isEmpty()
        && !targets->locks.lock(targetPaths, error)) {
        error = QStringLiteral(
            "A workout import target is already being changed: %1")
                    .arg(error);
        return false;
    }

    enum class PublicationAction {
        alreadyPublished,
        create,
        replace
    };
    struct PublicationTarget
    {
        PublicationAction action = PublicationAction::create;
        AnchoredFileSystem::EntryRef entry;
        AnchoredFileSystem::PinnedFile current;
    };
    std::vector<PublicationTarget> publicationTargets;
    publicationTargets.reserve(size_t(journal.workouts.size()));

    // Validate the complete generation while every target lock is held.
    // No target may be changed until all predecessors are known to match.
    for (qsizetype index = 0;
         index < journal.workouts.size(); ++index) {
        if (cancellationRequested()) {
            error = QStringLiteral("The workout publication was cancelled");
            return false;
        }
        const TrainDB::PlanImportWorkout &workout =
            journal.workouts.at(index);
        const AnchoredFileSystem::EntryRef &targetEntry =
            targetEntries.at(index);
        bool targetExists = false;
        AtomicFileSnapshot current;
        AnchoredFileSystem::PinnedFile currentFile;
        const qint64 maximumPredecessorSize = workout.replaceExisting
            ? qMax<qint64>(workout.previousSize, workout.contents.size())
            : workout.contents.size();
        if (!captureAnchoredTarget(
                targetEntry, maximumPredecessorSize, cancelled,
                targetExists, current, currentFile, error)) {
            return false;
        }
        if (targetExists
            && current.size == workout.contents.size()
            && current.digest == workout.digest) {
            PublicationTarget target;
            target.action = PublicationAction::alreadyPublished;
            target.entry = targetEntry;
            target.current = std::move(currentFile);
            publicationTargets.push_back(std::move(target));
            continue;
        }
        if (targetExists && !workout.replaceExisting) {
            error = QStringLiteral(
                "A plan import workout target already exists");
            return false;
        }
        if (workout.replaceExisting
            && (!targetExists
                || current.size != workout.previousSize
                || current.digest != workout.previousDigest
                || currentFile.identity().persistentFingerprint()
                    != workout.previousIdentity)) {
            error = QStringLiteral(
                "A plan import workout predecessor changed");
            return false;
        }
        PublicationTarget target;
        target.action = workout.replaceExisting
            ? PublicationAction::replace
            : PublicationAction::create;
        target.entry = targetEntry;
        target.current = std::move(currentFile);
        publicationTargets.push_back(std::move(target));
    }

    targets->targets.reserve(size_t(journal.workouts.size()));
    for (qsizetype index = 0;
         index < journal.workouts.size(); ++index) {
        if (cancellationRequested()) {
            error = QStringLiteral("The workout publication was cancelled");
            return false;
        }
        PublicationTarget &publication = publicationTargets.at(size_t(index));
        const PublicationAction action = publication.action;
        const TrainDB::PlanImportWorkout &workout =
            journal.workouts.at(index);
        if (action == PublicationAction::alreadyPublished) {
            if (validateDecision && !validateDecision(error))
                return false;
            const AnchoredFileSystem::EntryRef predecessorEntry =
                targets->root.entry(
                    predecessorArtifactName(journal, index), error);
            const AnchoredFileSystem::EntryRef stagingEntry =
                targets->root.entry(
                    publicationStagingName(
                        journal, index, workout.replaceExisting),
                    error);
            if (!predecessorEntry.isValid()
                || !stagingEntry.isValid()) {
                return false;
            }
            if (workout.replaceExisting
                && !removeExactArtifactIfPresent(
                    targets->root, predecessorEntry,
                    workout.previousSize,
                    workout.previousDigest,
                    workout.previousIdentity, error)) {
                return false;
            }
#ifdef Q_OS_WIN
            if (workout.replaceExisting
                && !removeExactArtifactIfPresent(
                    targets->root, stagingEntry,
                    workout.contents.size(),
                    workout.digest, {}, error)) {
                return false;
            }
#endif
            if (!workout.replaceExisting
                && !removeExactArtifactIfPresent(
                    targets->root, stagingEntry,
                    workout.contents.size(),
                    workout.digest, {}, error)) {
                return false;
            }
            if (validateDecision && !validateDecision(error))
                return false;
            PublishedTargets::Target target;
            target.entry = publication.entry;
            target.file = std::move(publication.current);
            target.expectedSize = workout.contents.size();
            target.expectedDigest = workout.digest;
            targets->targets.push_back(std::move(target));
            continue;
        }
#ifdef GC_PLAN_BUNDLE_IMPORT_TEST_HOOKS
        ::planBundleImportPublicationMutationTestHook();
#endif
        if (validateDecision && !validateDecision(error))
            return false;
        if (!targets->root.pathMatches(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The workout library changed before publication");
            }
            return false;
        }
        const QString temporaryName = publicationStagingName(
            journal, index, workout.replaceExisting);
        const AnchoredFileSystem::EntryRef temporaryEntry =
            targets->root.entry(temporaryName, error);
        const AnchoredFileSystem::EntryRef predecessorEntry =
            targets->root.entry(
                predecessorArtifactName(journal, index), error);
        AnchoredFileSystem::PinnedFile temporary;
        if (!temporaryEntry.isValid()
            || !predecessorEntry.isValid()
            || !preparePublicationStaging(
                temporaryEntry, workout.contents,
                workout.digest, temporary, error)) {
            return false;
        }
        const auto removeTemporary = [&temporary, &error] {
            if (!temporary.isValid()) return;
            const AnchoredFileSystem::MutationResult cleanup =
                AnchoredFileSystem::remove(temporary);
            if (!cleanup.applied() && !cleanup.error.isEmpty()) {
                if (!error.isEmpty()) error += QStringLiteral("; ");
                error += cleanup.error;
            }
        };
        if (cancellationRequested()) {
            error = QStringLiteral("The workout publication was cancelled");
            removeTemporary();
            return false;
        }
        if (!targets->root.pathMatches(error)) {
            removeTemporary();
            return false;
        }
        if (validateDecision && !validateDecision(error)) {
            removeTemporary();
            return false;
        }
        AnchoredFileSystem::MutationResult mutation =
            action == PublicationAction::replace
                ? AnchoredFileSystem::replaceExisting(
                    temporary, publication.current,
                    predecessorEntry)
                : AnchoredFileSystem::moveNoReplace(
                    temporary, publication.entry);
        if (!mutation.applied()) {
            error = mutation.error.isEmpty()
                ? QStringLiteral("Cannot publish a plan import workout")
                : mutation.error;
            if (mutation.effect
                != AnchoredFileSystem::MutationEffect::Partial) {
                removeTemporary();
            }
            return false;
        }
        if (!targets->root.sync(error)) return false;
        if (validateDecision && !validateDecision(error))
            return false;
        if (action == PublicationAction::replace
            && !removePinnedArtifact(
                targets->root, predecessorEntry,
                publication.current,
                workout.previousSize,
                workout.previousDigest,
                workout.previousIdentity, error)) {
            return false;
        }
        if (validateDecision && !validateDecision(error))
            return false;

        PublishedTargets::Target target;
        target.entry = publication.entry;
        target.file = std::move(temporary);
        target.expectedSize = workout.contents.size();
        target.expectedDigest = workout.digest;
        if (!target.file.isValid()
            || target.file.size() != target.expectedSize
            || target.file.sha256() != target.expectedDigest) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A plan import workout target has different contents");
            }
            return false;
        }
        targets->targets.push_back(std::move(target));
    }
    if (!targets->validate(error)) return false;
    if (validateDecision && !validateDecision(error))
        return false;
    publishedTargets = std::move(targets);
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

bool prepareJournalRecord(
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    TrainDB::PlanImportJournal &journal,
    AnchoredFileSystem::DirectoryAnchor &workoutRootAnchor,
    QString &error)
{
    journal = {};
    workoutRootAnchor = {};
    if (!normalizeRoot(
            athleteRoot, journal.athleteRoot, error)
        || !normalizeRoot(
            workoutRoot, journal.workoutRoot, error)) {
        return false;
    }
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            journal.workoutRoot, workoutRootAnchor, error)
        || !anchorNamesRoot(
            workoutRootAnchor, journal.workoutRoot, error)) {
        return false;
    }

    journal.id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QSet<QString> targetKeys;
    for (TrainDB::PlanImportWorkout workout : workouts) {
        if (!workout.previousIdentity.isEmpty()) {
            error = QStringLiteral(
                "A plan import predecessor identity must be captured from the workout root");
            return false;
        }
        if (workout.contents.isEmpty()) {
            error = QStringLiteral("A plan import workout is empty");
            return false;
        }
        QString targetPath;
        if (!workoutTargetPath(
                journal.workoutRoot,
                workout.targetFileName,
                targetPath, error)) {
            return false;
        }
        const QString key = atomicFilePathKey(targetPath);
        bool targetExists = false;
        AtomicFileSnapshot targetSnapshot;
        AnchoredFileSystem::PinnedFile targetFile;
        const AnchoredFileSystem::EntryRef targetEntry =
            workoutRootAnchor.entry(workout.targetFileName, error);
        const qint64 maximumTargetSize = workout.replaceExisting
            && workout.previousSize >= 0
            ? qMax<qint64>(
                workout.previousSize, workout.contents.size())
            : workout.contents.size();
        if (targetKeys.contains(key)
            || !targetEntry.isValid()
            || !captureAnchoredTarget(
                targetEntry, maximumTargetSize, {},
                targetExists, targetSnapshot, targetFile, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A plan import workout target is duplicated");
            }
            return false;
        }
        if ((!workout.replaceExisting && targetExists)
            || (workout.replaceExisting
                && (!targetExists
                    || targetSnapshot.size != workout.previousSize
                    || targetSnapshot.digest
                        != workout.previousDigest))) {
            error = workout.replaceExisting
                ? QStringLiteral(
                    "A plan import workout predecessor changed")
                : QStringLiteral(
                    "A plan import workout target already exists");
            return false;
        }
        targetKeys.insert(key);
        if (workout.replaceExisting) {
            workout.previousIdentity =
                targetFile.identity().persistentFingerprint();
            if (workout.previousIdentity.size()
                != QCryptographicHash::hashLength(
                    QCryptographicHash::Sha256)) {
                error = QStringLiteral(
                    "The plan import workout predecessor identity is unavailable");
                return false;
            }
        }
        workout.digest = QCryptographicHash::hash(
            workout.contents, QCryptographicHash::Sha256);
        journal.workouts.append(std::move(workout));
    }
    return workoutRootAnchor.pathMatches(error);
}

bool capturePreparedPredecessorIdentities(
    TrainDB::PlanImportJournal &journal,
    const AnchoredFileSystem::DirectoryAnchor &workoutRootAnchor,
    QString &error)
{
    for (TrainDB::PlanImportWorkout &workout : journal.workouts) {
        if (!workout.replaceExisting) continue;
        const AnchoredFileSystem::EntryRef targetEntry =
            workoutRootAnchor.entry(workout.targetFileName, error);
        bool targetExists = false;
        AtomicFileSnapshot targetSnapshot;
        AnchoredFileSystem::PinnedFile targetFile;
        const qint64 maximumTargetSize = qMax<qint64>(
            workout.previousSize, workout.contents.size());
        if (!targetEntry.isValid()
            || !captureAnchoredTarget(
                targetEntry, maximumTargetSize, {},
                targetExists, targetSnapshot, targetFile, error)
            || !targetExists
            || targetSnapshot.size != workout.previousSize
            || targetSnapshot.digest != workout.previousDigest) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A prepared plan import workout predecessor changed");
            }
            return false;
        }
        workout.previousIdentity =
            targetFile.identity().persistentFingerprint();
        if (workout.previousIdentity.size()
            != QCryptographicHash::hashLength(
                QCryptographicHash::Sha256)) {
            error = QStringLiteral(
                "The prepared plan import predecessor identity is unavailable");
            return false;
        }
    }
    return workoutRootAnchor.pathMatches(error);
}

bool assemblePreparedJournalRecord(
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    TrainDB::PlanImportJournal &journal,
    QString &error)
{
    journal = {};
    if (!normalizeRoot(
            athleteRoot, journal.athleteRoot, error)
        || !normalizeRoot(
            workoutRoot, journal.workoutRoot, error)) {
        return false;
    }
    journal.id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QSet<QString> targetKeys;
    for (const TrainDB::PlanImportWorkout &workout : workouts) {
        QString targetPath;
        const bool predecessorValid = workout.replaceExisting
            ? workout.previousSize >= 0
                && workout.previousDigest.size()
                    == QCryptographicHash::hashLength(
                        QCryptographicHash::Sha256)
                && workout.previousIdentity.isEmpty()
            : workout.previousSize == -1
                && workout.previousDigest.isEmpty()
                && workout.previousIdentity.isEmpty();
        if (workout.contents.isEmpty()
            || workout.digest.size()
                != QCryptographicHash::hashLength(
                    QCryptographicHash::Sha256)
            || !predecessorValid
            || !workoutTargetPath(
                journal.workoutRoot,
                workout.targetFileName,
                targetPath, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A prepared plan import workout is invalid");
            }
            return false;
        }
        const QString key = atomicFilePathKey(targetPath);
        if (targetKeys.contains(key)) {
            error = QStringLiteral(
                "A prepared plan import workout target is duplicated");
            return false;
        }
        targetKeys.insert(key);
        journal.workouts.append(workout);
    }
    return true;
}

} // namespace

Journal::Journal(
    TrainDB *database,
    TrainDB::PlanImportJournal journal,
    bool standalone,
    bool decisionCommitted)
    : database_(database),
      journal_(std::move(journal)),
      standalone_(standalone),
      decisionCommitted_(decisionCommitted)
{
}

bool Journal::prepareDecisionMarker(
    const QString &databasePath,
    const std::shared_ptr<TrainDatabaseFileGeneration> &databaseGeneration,
    QString &error)
{
    const QByteArray fingerprint =
        TrainDB::databaseFileGenerationFingerprint(databaseGeneration);
    if (databasePath.isEmpty()
        || !databaseGeneration
        || fingerprint.size() != 32
        || !workoutRootAnchor_.isValid()
        || !TrainDB::databaseFileGenerationMatches(
            databasePath, databaseGeneration, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The workout database generation is unavailable for the durable decision");
        }
        return false;
    }
    std::shared_ptr<DecisionMarker> marker;
    if (!createDecisionMarker(
            journal_, workoutRootAnchor_, fingerprint,
            marker, error)) {
        return false;
    }
    databasePath_ = QFileInfo(databasePath).absoluteFilePath();
    databaseGeneration_ = databaseGeneration;
    databaseFingerprint_ = fingerprint;
    decisionMarker_ = std::move(marker);
    return decisionGenerationMatches(error);
}

bool Journal::loadDecisionMarker(
    const QString &databasePath,
    const std::shared_ptr<TrainDatabaseFileGeneration> &databaseGeneration,
    QString &error)
{
    const QByteArray fingerprint =
        TrainDB::databaseFileGenerationFingerprint(databaseGeneration);
    if (fingerprint.size() != 32
        || !TrainDB::databaseFileGenerationMatches(
            databasePath, databaseGeneration, error)) {
        return false;
    }
    std::shared_ptr<DecisionMarker> marker;
    if (!openDecisionMarker(
            journal_, workoutRootAnchor_, fingerprint,
            marker, error)) {
        return false;
    }
    databasePath_ = QFileInfo(databasePath).absoluteFilePath();
    databaseGeneration_ = databaseGeneration;
    databaseFingerprint_ = fingerprint;
    decisionMarker_ = std::move(marker);
    return true;
}

bool Journal::decisionGenerationMatches(QString &error) const
{
    return decisionGenerationMatchesSnapshot(
        journal_, workoutRootAnchor_, databasePath_,
        databaseGeneration_, databaseFingerprint_,
        decisionMarker_, error);
}

bool Journal::retireDecisionMarker(QString &error)
{
    return removeDecisionMarker(
        workoutRootAnchor_, decisionMarker_, error);
}

void Journal::discardUncommittedDecisionMarker()
{
    QString cleanupError;
    if (!removeDecisionMarker(
            workoutRootAnchor_, decisionMarker_, cleanupError)
        && !cleanupError.isEmpty()) {
        qWarning().noquote()
            << "Uncommitted plan import decision marker cleanup will be retried:"
            << cleanupError;
    }
    databasePath_.clear();
    databaseGeneration_.reset();
    databaseFingerprint_.clear();
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
    AnchoredFileSystem::DirectoryAnchor workoutRootAnchor;
    if (!prepareJournalRecord(
            athleteRoot, workoutRoot, workouts,
            journal, workoutRootAnchor, error)) return {};
    bool found = false;
    if (!database->planImportJournalExists(
            journal.athleteRoot,
            found, error)) {
        return {};
    }
    if (found) {
        error = QStringLiteral(
            "A previous plan import must be recovered before another can start");
        return {};
    }
    QList<ObservedDecisionMarker> markers;
    if (!decisionMarkersForAthlete(
            workoutRootAnchor, journal.athleteRoot,
            markers, error)) {
        return {};
    }
    if (!markers.isEmpty()) {
        error = QStringLiteral(
            "A durable plan import marker must be recovered before another can start");
        return {};
    }

    auto coordinator = std::shared_ptr<Journal>(
        new Journal(database, std::move(journal)));
    coordinator->workoutRootAnchor_ = std::move(workoutRootAnchor);
    return coordinator;
}

std::shared_ptr<Journal> Journal::createStandalonePrepared(
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    QString &error)
{
    AnchoredFileSystem::DirectoryAnchor root;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            workoutRoot, root, error)) {
        return {};
    }
    return createStandalonePrepared(
        athleteRoot, workoutRoot, root, workouts, error);
}

std::shared_ptr<Journal> Journal::createStandalonePrepared(
    const QString &athleteRoot,
    const QString &workoutRoot,
    const AnchoredFileSystem::DirectoryAnchor &workoutRootAnchor,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    QString &error)
{
    error.clear();
    TrainDB::PlanImportJournal journal;
    if (!assemblePreparedJournalRecord(
            athleteRoot, workoutRoot, workouts,
            journal, error)) return {};
    QString identity;
    bool found = false;
    if (!anchorNamesRoot(
            workoutRootAnchor, journal.workoutRoot, error)
        || !standaloneRootIdentity(
            workoutRootAnchor,
            true, true, identity, found, error)
        || !found
        || !capturePreparedPredecessorIdentities(
            journal, workoutRootAnchor, error)) {
        return {};
    }
    journal.planJournalId = identity;
    QList<ObservedDecisionMarker> markers;
    if (!decisionMarkersForAthlete(
            workoutRootAnchor, journal.athleteRoot,
            markers, error)) {
        return {};
    }
    if (!markers.isEmpty()) {
        error = QStringLiteral(
            "A durable plan import marker must be recovered before another can start");
        return {};
    }
    auto coordinator = std::shared_ptr<Journal>(
        new Journal(nullptr, std::move(journal), true));
    coordinator->workoutRootAnchor_ = workoutRootAnchor;
    return coordinator;
}

std::shared_ptr<Journal> Journal::createStandalone(
    TrainDB *database,
    const QString &athleteRoot,
    const QString &workoutRoot,
    const QList<TrainDB::PlanImportWorkout> &workouts,
    QString &error)
{
    std::shared_ptr<Journal> coordinator = create(
        database, athleteRoot, workoutRoot, workouts, error);
    if (!coordinator) return {};
    AnchoredFileSystem::DirectoryAnchor root;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            coordinator->journal_.workoutRoot, root, error)
        || !root.pathMatches(error)) {
        return {};
    }
    QString identity;
    bool found = false;
    if (!standaloneRootIdentity(
            root,
            true, true, identity, found, error)
        || !found) {
        return {};
    }
    coordinator->journal_.planJournalId = identity;
    coordinator->standalone_ = true;
    coordinator->workoutRootAnchor_ = std::move(root);
    return coordinator;
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
    const QString databasePath = database_->databaseFilePath();
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        databasePath, error);
    if (!generation
        || !prepareDecisionMarker(
            databasePath, generation, error)) {
        return false;
    }
    const bool succeeded = database_->commitPreparedPlanImportJournal(
        journal_, committed, error);
    decisionCommitted_ = committed;
    if (!succeeded || !committed) {
        if (!committed) discardUncommittedDecisionMarker();
        return false;
    }
    return decisionGenerationMatches(error);
}

bool Journal::commitStandaloneDecision(
    bool &committed,
    QString &error)
{
    committed = false;
    error.clear();
    if (!databaseIsUsable(database_.data(), error))
        return false;
    QString identity;
    bool found = false;
    const bool identityValid = workoutRootAnchor_.isValid()
        ? standaloneRootIdentity(
            workoutRootAnchor_, false, true,
            identity, found, error)
        : standaloneRootIdentity(
            journal_.workoutRoot, false, true,
            identity, found, error);
    if (!standalone_
        || !identityValid
        || !found || identity != journal_.planJournalId) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The standalone workout root identity changed before commit");
        }
        return false;
    }
    const QString databasePath = database_->databaseFilePath();
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        databasePath, error);
    if (!generation
        || !prepareDecisionMarker(
            databasePath, generation, error)) {
        return false;
    }
    const bool succeeded = database_->commitPreparedPlanImportJournal(
        journal_, committed, error);
    decisionCommitted_ = committed;
    if (!succeeded || !committed) {
        if (!committed) discardUncommittedDecisionMarker();
        return false;
    }
    return decisionGenerationMatches(error);
}

bool Journal::commitStandaloneDecision(
    TrainDB *database,
    bool &committed,
    QString &error)
{
    committed = false;
    error.clear();
    if (database_ && database_.data() != database) {
        error = QStringLiteral(
            "The workout database owner changed before commit");
        return false;
    }
    database_ = database;
    if (!databaseIsUsable(database_.data(), error)
        || !standalone_
        || journal_.planJournalId.isEmpty()) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared standalone workout decision is invalid");
        }
        return false;
    }
    QString identity;
    bool found = false;
    const bool identityValid = workoutRootAnchor_.isValid()
        ? standaloneRootIdentity(
            workoutRootAnchor_, false, true,
            identity, found, error)
        : standaloneRootIdentity(
            journal_.workoutRoot, false, true,
            identity, found, error);
    if (!identityValid
        || !found || identity != journal_.planJournalId) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The standalone workout root identity changed before commit");
        }
        return false;
    }
    const QString databasePath = database_->databaseFilePath();
    const auto generation = TrainDB::captureDatabaseFileGeneration(
        databasePath, error);
    if (!generation
        || !prepareDecisionMarker(
            databasePath, generation, error)) {
        return false;
    }
    const bool succeeded = database_->commitPreparedPlanImportJournal(
        journal_, committed, error);
    decisionCommitted_ = committed;
    if (!succeeded || !committed) {
        if (!committed) discardUncommittedDecisionMarker();
        return false;
    }
    return decisionGenerationMatches(error);
}

bool Journal::commitStandaloneDecision(
    const QString &databasePath,
    const CancellationCheck &cancelled,
    bool &committed,
    QString &error)
{
    return commitStandaloneDecision(
        databasePath, cancelled, {}, committed, error);
}

bool Journal::commitStandaloneDecision(
    const QString &databasePath,
    const CancellationCheck &cancelled,
    const std::shared_ptr<TrainDatabaseFileGeneration> &databaseGeneration,
    bool &committed,
    QString &error)
{
    committed = false;
    error.clear();
    if (database_ || !standalone_
        || journal_.planJournalId.isEmpty()) {
        error = QStringLiteral(
            "The prepared standalone workout decision is invalid");
        return false;
    }
    QString identity;
    bool found = false;
    const bool identityValid = workoutRootAnchor_.isValid()
        ? standaloneRootIdentity(
            workoutRootAnchor_, false, true,
            identity, found, error)
        : standaloneRootIdentity(
            journal_.workoutRoot, false, true,
            identity, found, error);
    if (!identityValid
        || !found || identity != journal_.planJournalId) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The standalone workout root identity changed before commit");
        }
        return false;
    }
    std::shared_ptr<TrainDatabaseFileGeneration> effectiveGeneration =
        databaseGeneration;
    if (!effectiveGeneration) {
        effectiveGeneration = TrainDB::captureDatabaseFileGeneration(
            databasePath, error);
        if (!effectiveGeneration) return false;
    }
    if (!prepareDecisionMarker(
            databasePath, effectiveGeneration, error)) {
        return false;
    }
    if (!TrainDB::commitPreparedPlanImportJournalAtPath(
            databasePath, journal_, cancelled, effectiveGeneration,
            committed, error,
            [this](QString &validationError) {
                QString identity;
                bool found = false;
                const bool valid = workoutRootAnchor_.isValid()
                    ? standaloneRootIdentity(
                        workoutRootAnchor_, false, true,
                        identity, found, validationError)
                    : standaloneRootIdentity(
                        journal_.workoutRoot, false, true,
                        identity, found, validationError);
                if (valid
                    && found
                    && identity == journal_.planJournalId) {
                    return true;
                }
                if (validationError.isEmpty()) {
                    validationError = QStringLiteral(
                        "The standalone workout root identity changed before commit");
                }
                return false;
            })) {
        decisionCommitted_ = committed;
        if (!committed) discardUncommittedDecisionMarker();
        return false;
    }
    decisionCommitted_ = committed;
    if (!committed) {
        discardUncommittedDecisionMarker();
        return false;
    }
    return decisionGenerationMatches(error);
}

bool Journal::publishPreparedFiles(
    const CancellationCheck &cancelled,
    QString &error)
{
    error.clear();
    publishedTargets_.reset();
    if (!decisionCommitted_) {
        error = QStringLiteral(
            "The durable plan import decision is unavailable");
        return false;
    }
    if (!decisionGenerationMatches(error)) return false;
    if (standalone_) {
        QString identity;
        bool found = false;
        const bool validIdentity = workoutRootAnchor_.isValid()
            ? standaloneRootIdentity(
                workoutRootAnchor_, false, true,
                identity, found, error)
            : standaloneRootIdentity(
                journal_.workoutRoot,
                false, true, identity, found, error)
            ;
        if (!validIdentity
            || !found || identity != journal_.planJournalId) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The standalone workout root identity changed before publication");
            }
            return false;
        }
    }
    std::shared_ptr<PublishedTargets> targets;
    const PublishedValidation validateDecision =
        [this](QString &validationError) {
            return decisionGenerationMatches(validationError);
        };
    if (!publishWorkouts(
            journal_, workoutRootAnchor_, targets,
            cancelled, validateDecision, error)) {
        return false;
    }
    publishedTargets_ = std::move(targets);
    return true;
}

bool Journal::completePublishedDatabase(
    const DatabaseCompletion &,
    QString &error)
{
    error = QStringLiteral(
        "Durable plan import recovery requires a bound database completion");
    return false;
}

bool Journal::completePublishedDatabaseBound(
    const BoundDatabaseCompletion &completeDatabase,
    QString &error)
{
    error.clear();
    if (!databaseIsUsable(database_.data(), error)
        || !completeDatabase || !publishedTargets_) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The plan import database completion is unavailable");
        }
        return false;
    }
    if (!decisionGenerationMatches(error)) return false;

    auto validation = std::make_shared<PublishedValidationState>();
    validation->ownerThreadId = QThread::currentThreadId();
    validation->database = database_;
    validation->targets = publishedTargets_;
    validation->journal = journal_;
    validation->workoutRoot = workoutRootAnchor_;
    validation->databasePath = databasePath_;
    validation->databaseGeneration = databaseGeneration_;
    validation->databaseFingerprint = databaseFingerprint_;
    validation->decisionMarker = decisionMarker_;
    const PublishedValidation validatePublished =
        [validation](QString &validationError) {
            return validation->validate(validationError);
        };
    bool databaseCompleted = false;
    try {
        databaseCompleted = completeDatabase(
            journal_, validatePublished, error);
    } catch (const QString &detail) {
        error = detail;
    } catch (const std::exception &exception) {
        error = QString::fromLocal8Bit(exception.what());
    } catch (...) {
        error = QStringLiteral(
            "The plan import database transaction failed");
    }
    validation->expire();
    if (!databaseCompleted || !validation->called
        || !validation->succeeded) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The published workouts were not validated in the database transaction");
        }
        return false;
    }

    TrainDB *boundDatabase = database_.data();
    if (!boundDatabase
        || QThread::currentThread() != boundDatabase->thread()) {
        error = QStringLiteral(
            "The workout database was destroyed during plan import completion");
        return false;
    }
    bool found = false;
    if (!boundDatabase->planImportJournalExists(
            journal_.athleteRoot, found, error)) {
        return false;
    }
    if (found) {
        error = QStringLiteral(
            "The plan import decision was not retired atomically with its workout rows");
        return false;
    }
    if (!decisionGenerationMatches(error)) return false;
    decisionCommitted_ = false;
    publishedTargets_.reset();
    if (!retireDecisionMarker(error)) return false;
    databaseGeneration_.reset();
    databaseFingerprint_.clear();
    databasePath_.clear();
    return true;
}

bool Journal::bindDatabase(TrainDB *database, QString &error)
{
    error.clear();
    if (database_ && database_.data() != database) {
        error = QStringLiteral(
            "The workout database owner changed before completion");
        return false;
    }
    if (!databaseIsUsable(database, error)) return false;
    if (!databasePath_.isEmpty()
        && QFileInfo(database->databaseFilePath()).absoluteFilePath()
            != databasePath_) {
        error = QStringLiteral(
            "The workout database path changed before completion");
        return false;
    }
    database_ = database;
    return decisionGenerationMatches(error);
}

bool Journal::completeRecord(
    const BoundDatabaseCompletion &completeDatabase,
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
    return publishPreparedFiles({}, error)
        && completePublishedDatabaseBound(completeDatabase, error);
}

bool Journal::completePublishedPlan(
    const BoundDatabaseCompletion &completeDatabase,
    QString &error)
{
    TrainDB::PlanImportJournal loaded;
    bool found = false;
    if (!loadDecision(
            database_.data(), journal_.athleteRoot,
            loaded, found, error)) {
        return false;
    }
    if (!found) {
        if (!decisionMarker_) return true;
        if (!decisionGenerationMatches(error)) return false;
        decisionCommitted_ = false;
        return retireDecisionMarker(error);
    }
    if (loaded.id != journal_.id
        || loaded.planJournalId != journal_.planJournalId
        || atomicFilePathKey(loaded.workoutRoot)
            != atomicFilePathKey(journal_.workoutRoot)) {
        error = QStringLiteral(
            "The durable plan import decision changed unexpectedly");
        return false;
    }
    journal_ = std::move(loaded);
    decisionCommitted_ = true;
    return completeRecord(completeDatabase, error);
}

bool Journal::completePublishedPlan(
    const DatabaseCompletion &completeDatabase,
    QString &error)
{
    Q_UNUSED(completeDatabase)
    error = QStringLiteral(
        "Durable plan import completion requires a bound database transaction");
    return false;
}

bool Journal::reconcileAll(
    TrainDB *database,
    const QString &athleteRoot,
    const QString &workoutRoot,
    const BoundDatabaseCompletion &completeDatabase,
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
    AnchoredFileSystem::DirectoryAnchor workoutRootAnchor;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            normalizedWorkoutRoot, workoutRootAnchor, error)
        || !anchorNamesRoot(
            workoutRootAnchor, normalizedWorkoutRoot, error)) {
        return false;
    }
    const QString databasePath = database->databaseFilePath();
    const auto databaseGeneration =
        TrainDB::captureDatabaseFileGeneration(databasePath, error);
    const QByteArray databaseFingerprint =
        TrainDB::databaseFileGenerationFingerprint(databaseGeneration);
    if (!databaseGeneration || databaseFingerprint.size() != 32)
        return false;
    QList<ObservedDecisionMarker> observedMarkers;
    if (!decisionMarkersForAthlete(
            workoutRootAnchor, normalizedAthleteRoot,
            observedMarkers, error)) {
        return false;
    }

    TrainDB::PlanImportJournal journalRecord;
    bool found = false;
    if (!database->loadPlanImportJournal(
            normalizedAthleteRoot,
            journalRecord, found, error)) {
        return false;
    }
    if (!found) {
        for (ObservedDecisionMarker &observed : observedMarkers) {
            if (observed.databaseFingerprint
                != databaseFingerprint) {
                error = QStringLiteral(
                    "A durable plan import decision belongs to an unavailable workout database generation");
                return false;
            }
            if (!TrainDB::databaseFileGenerationMatches(
                    databasePath, databaseGeneration, error)
                || !removeDecisionMarker(
                    workoutRootAnchor, observed.marker, error)) {
                return false;
            }
        }
        return TrainDB::databaseFileGenerationMatches(
            databasePath, databaseGeneration, error);
    }
    if (atomicFilePathKey(journalRecord.athleteRoot)
            != atomicFilePathKey(normalizedAthleteRoot)
        || atomicFilePathKey(journalRecord.workoutRoot)
            != atomicFilePathKey(normalizedWorkoutRoot)) {
        error = QStringLiteral(
            "The plan import roots changed before recovery");
        return false;
    }

    if (observedMarkers.size() != 1
        || observedMarkers.first().journalId != journalRecord.id
        || observedMarkers.first().databaseFingerprint
            != databaseFingerprint) {
        error = QStringLiteral(
            "The durable plan import decision marker does not match the database journal");
        return false;
    }

    QString rootIdentity;
    bool rootIdentityFound = false;
    if (!standaloneRootIdentity(
            normalizedWorkoutRoot,
            false, false,
            rootIdentity, rootIdentityFound, error)) {
        return false;
    }
    const bool standalone = rootIdentityFound
        && rootIdentity == journalRecord.planJournalId;
    std::shared_ptr<PlanReplacement::Journal> plan;
    if (!standalone) {
        plan = PlanReplacement::Journal::openPrepared(
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
    }

    Journal coordinator(database, journalRecord, standalone, true);
    coordinator.workoutRootAnchor_ = workoutRootAnchor;
    if (!coordinator.loadDecisionMarker(
            databasePath, databaseGeneration, error)) {
        return false;
    }
    if (!coordinator.completeRecord(
            completeDatabase, error)) {
        return false;
    }

    QString cleanupError;
    if (plan && !plan->cleanupAfterCommit(cleanupError)) {
        qWarning().noquote()
            << "Committed plan import cleanup will be retried:"
            << cleanupError;
    }
    return true;
}

bool Journal::reconcileAll(
    TrainDB *,
    const QString &,
    const QString &,
    const DatabaseCompletion &,
    QString &error)
{
    error = QStringLiteral(
        "Durable plan import recovery requires a bound database completion");
    return false;
}

} // namespace PlanBundleImport
