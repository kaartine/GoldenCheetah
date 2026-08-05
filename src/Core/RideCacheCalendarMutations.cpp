/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCache.h"

#include "Athlete.h"
#include "AtomicFileWriter.h"
#include "Context.h"
#include "PlanReplacementJournal.h"
#include "RideFileCacheIntegrity.h"
#include "RideCacheMutationScope.h"
#include "RideItem.h"

#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <memory>
#include <utility>

RideCache::OperationResult
RideCache::changeActivityIdentitiesAtomically(
    const QList<ActivityIdentityMutationRequest> &requests)
{
    OperationResult result;
    if (requests.isEmpty()) {
        result.success = true;
        result.cacheUpdated = true;
        result.cleanupComplete = true;
        return result;
    }
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }

    QString mutationError;
    RideCacheMutationScope mutation(this, mutationError);
    if (!mutation.ready()) {
        result.error = mutationError;
        return result;
    }

    const QPointer<RideCache> guardedCache(this);
    const QPointer<Context> guardedContext(context);
    const QPointer<Athlete> guardedAthlete(
        guardedContext ? guardedContext->athlete : nullptr);
    if (!guardedCache || !guardedContext || !guardedAthlete
        || guardedCache->context != guardedContext.data()
        || guardedContext->athlete != guardedAthlete.data()
        || guardedAthlete->rideCache != guardedCache.data()) {
        result.error = tr(
            "The activity collection is no longer available");
        return result;
    }

    struct DerivedMutation
    {
        QString sourcePath;
        QString targetPath;
        bool removeSource = true;
    };
    struct PendingIdentity
    {
        QPointer<RideItem> item;
        QString sourcePath;
        QString targetPath;
        QString sourceRoot;
        QString itemPath;
        QString sourceFileName;
        QString targetFileName;
        QDateTime sourceDateTime;
        QDateTime targetDateTime;
        QString originalDate;
        QString sourceLinkedFileName;
        QString targetLinkedFileName;
        bool planned = false;
        QList<DerivedMutation> derived;
        QStringList absentDerivedTargets;
    };

    QSet<RideItem*> requestedAddresses;
    for (const ActivityIdentityMutationRequest &request : requests) {
        if (!request.item
            || requestedAddresses.contains(request.item)) {
            result.error = tr(
                "An activity identity change target is duplicated or missing");
            return result;
        }
        requestedAddresses.insert(request.item);
    }

    QList<PendingIdentity> pending;
    pending.reserve(requests.size() * 2);
    QSet<RideItem*> linkedPeerAddresses;
    const QString athleteRoot =
        guardedAthlete->home->root().absolutePath();
    const QString canonicalCacheRoot =
        guardedAthlete->home->cache().canonicalPath();
    if (athleteRoot.isEmpty() || canonicalCacheRoot.isEmpty()) {
        result.error = tr(
            "The athlete activity directories are unavailable");
        return result;
    }

    for (const ActivityIdentityMutationRequest &request : requests) {
        RideItem *const item = request.item;
        if (!guardedCache->ownsLiveRide(item)
            || !request.targetDateTime.isValid()
            || item->fileName.isEmpty()
            || !item->dateTime.isValid()) {
            result.error = tr(
                "An activity identity change source or target is invalid");
            return result;
        }
        if (item->isDirty()) {
            result.error = tr(
                "A modified activity must be saved or reverted before it is moved: %1")
                               .arg(item->fileName);
            return result;
        }
        const QDir activeDirectory = item->planned
            ? guardedAthlete->home->planned()
            : guardedAthlete->home->activities();
        const QString canonicalSourceRoot =
            activeDirectory.canonicalPath();
        const QString canonicalItemRoot =
            QDir(item->path).canonicalPath();
        const QString sourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                activeDirectory.absolutePath(), item->fileName);
        const QString itemSourcePath =
            RideFileCacheIntegrity::activitySourcePath(
                item->path, item->fileName);
        if (canonicalSourceRoot.isEmpty()
            || canonicalItemRoot.isEmpty()
            || sourcePath.isEmpty()
            || itemSourcePath.isEmpty()
            || atomicFilePathKey(canonicalSourceRoot)
                != atomicFilePathKey(canonicalItemRoot)
            || atomicFilePathKey(sourcePath)
                != atomicFilePathKey(itemSourcePath)) {
            result.error = tr(
                "The activity source does not match its cache namespace: %1")
                               .arg(item->fileName);
            return result;
        }
        const QFileInfo sourceInfo(sourcePath);
        if (sourceInfo.isSymLink() || !sourceInfo.isFile()) {
            result.error = tr(
                "The activity source is not a regular file: %1")
                               .arg(item->fileName);
            return result;
        }

        const QFileInfo oldNameInfo(item->fileName);
        const QString targetFileName = QStringLiteral("%1.%2")
            .arg(request.targetDateTime.toString(
                     QStringLiteral("yyyy_MM_dd_HH_mm_ss")),
                 oldNameInfo.suffix());
        const QString targetPath =
            RideFileCacheIntegrity::activitySourcePath(
                activeDirectory.absolutePath(), targetFileName);
        if (targetPath.isEmpty()) {
            result.error = tr(
                "The activity target path is unsafe: %1")
                               .arg(targetFileName);
            return result;
        }
        if (atomicFilePathKey(sourcePath)
            == atomicFilePathKey(targetPath)) {
            continue;
        }
        for (RideItem *candidate :
             std::as_const(guardedCache->rides_)) {
            if (!guardedCache->ownsLiveRide(candidate)
                || requestedAddresses.contains(candidate)) {
                continue;
            }
            if (candidate->planned == item->planned
                && candidate->fileName == targetFileName) {
                result.error = tr(
                    "The activity target already exists in the cache: %1")
                                   .arg(targetFileName);
                return result;
            }
        }

        QString originalDate = item->getText(
            QStringLiteral("Original Date"), QString());
        if (!QDate::fromString(
                originalDate,
                QStringLiteral("yyyy/MM/dd")).isValid()) {
            originalDate = item->dateTime.date().toString(
                QStringLiteral("yyyy/MM/dd"));
        }

        PendingIdentity entry;
        entry.item = item;
        entry.sourcePath = sourcePath;
        entry.targetPath = targetPath;
        entry.sourceRoot = canonicalSourceRoot;
        entry.itemPath = item->path;
        entry.sourceFileName = item->fileName;
        entry.targetFileName = targetFileName;
        entry.sourceDateTime = item->dateTime;
        entry.targetDateTime = request.targetDateTime;
        entry.originalDate = originalDate;
        entry.sourceLinkedFileName = item->getLinkedFileName();
        entry.targetLinkedFileName = entry.sourceLinkedFileName;
        entry.planned = item->planned;

        const QString oldCpxPath =
            guardedCache->cpxCachePathForActivity(
                entry.sourceFileName, entry.planned);
        const QString newCpxPath =
            guardedCache->cpxCachePathForActivity(
                entry.targetFileName, entry.planned);
        struct DerivedCandidate
        {
            QString sourcePath;
            bool removeSource = true;
        };
        QList<DerivedCandidate> derivedCandidates;
        QSet<QString> derivedCandidateKeys;
        for (const QString &derivedPath :
             guardedCache->derivedFilePathsForRemoval(item)) {
            const QString key = atomicFilePathKey(derivedPath);
            if (derivedCandidateKeys.contains(key)) continue;
            derivedCandidateKeys.insert(key);
            derivedCandidates.append({derivedPath, true});
        }
        const QString oldBaseName =
            QFileInfo(entry.sourceFileName).baseName();
        for (const QString &extension :
             {QStringLiteral("cpi"), QStringLiteral("notes")}) {
            const QString legacyPath = QDir(canonicalCacheRoot).filePath(
                oldBaseName + QLatin1Char('.') + extension);
            const QString key = atomicFilePathKey(legacyPath);
            if (derivedCandidateKeys.contains(key)) continue;
            derivedCandidateKeys.insert(key);
            derivedCandidates.append({legacyPath, false});
        }

        for (const DerivedCandidate &candidate :
             std::as_const(derivedCandidates)) {
            const QString &derivedPath = candidate.sourcePath;
            const QFileInfo derivedInfo(derivedPath);
            QString derivedTargetPath;
            if (!oldCpxPath.isEmpty()
                && atomicFilePathKey(derivedPath)
                    == atomicFilePathKey(oldCpxPath)) {
                derivedTargetPath = newCpxPath;
            } else {
                derivedTargetPath = QDir(canonicalCacheRoot).filePath(
                    QFileInfo(entry.targetFileName).baseName()
                    + QLatin1Char('.') + derivedInfo.suffix());
            }
            if (derivedTargetPath.isEmpty()) {
                result.error = tr(
                    "A derived activity target path is unavailable");
                return result;
            }
            if (!derivedInfo.exists() && !derivedInfo.isSymLink()) {
                entry.absentDerivedTargets.append(derivedTargetPath);
                continue;
            }
            if (derivedInfo.isSymLink() || !derivedInfo.isFile()) {
                result.error = tr(
                    "A derived activity file is not regular: %1")
                                   .arg(derivedPath);
                return result;
            }
            entry.derived.append({
                derivedPath, derivedTargetPath,
                candidate.removeSource});
        }
        pending.append(entry);

        if (entry.sourceLinkedFileName.isEmpty()) continue;

        RideItem *const peer = guardedCache->getLinkedActivity(item);
        if (!guardedCache->ownsLiveRide(peer)
            || peer == item
            || peer->planned == item->planned
            || peer->fileName != entry.sourceLinkedFileName
            || peer->getLinkedFileName() != entry.sourceFileName
            || requestedAddresses.contains(peer)
            || linkedPeerAddresses.contains(peer)) {
            result.error = tr(
                "The linked activity relationship is not reciprocal and unique: %1")
                               .arg(entry.sourceFileName);
            return result;
        }
        if (peer->isDirty()) {
            result.error = tr(
                "A modified linked activity must be saved or reverted before its peer is moved: %1")
                               .arg(peer->fileName);
            return result;
        }
        for (RideItem *candidate :
             std::as_const(guardedCache->rides_)) {
            if (!guardedCache->ownsLiveRide(candidate)
                || candidate == item || candidate == peer) {
                continue;
            }
            const QString candidateLink =
                candidate->getLinkedFileName();
            if (candidateLink == entry.sourceFileName
                || candidateLink == entry.targetFileName) {
                result.error = tr(
                    "Another activity has an ambiguous link to the moved identity: %1")
                                   .arg(candidate->fileName);
                return result;
            }
        }

        const QDir peerDirectory = peer->planned
            ? guardedAthlete->home->planned()
            : guardedAthlete->home->activities();
        const QString canonicalPeerRoot =
            peerDirectory.canonicalPath();
        const QString canonicalPeerItemRoot =
            QDir(peer->path).canonicalPath();
        const QString peerPath =
            RideFileCacheIntegrity::activitySourcePath(
                peerDirectory.absolutePath(), peer->fileName);
        const QString peerItemPath =
            RideFileCacheIntegrity::activitySourcePath(
                peer->path, peer->fileName);
        const QFileInfo peerInfo(peerPath);
        if (canonicalPeerRoot.isEmpty()
            || canonicalPeerItemRoot.isEmpty()
            || peerPath.isEmpty() || peerItemPath.isEmpty()
            || atomicFilePathKey(canonicalPeerRoot)
                != atomicFilePathKey(canonicalPeerItemRoot)
            || atomicFilePathKey(peerPath)
                != atomicFilePathKey(peerItemPath)
            || peerInfo.isSymLink() || !peerInfo.isFile()
            || !peer->dateTime.isValid()) {
            result.error = tr(
                "The linked activity source is unavailable or unsafe: %1")
                               .arg(peer->fileName);
            return result;
        }

        QString peerOriginalDate = peer->getText(
            QStringLiteral("Original Date"), QString());
        if (!QDate::fromString(
                peerOriginalDate,
                QStringLiteral("yyyy/MM/dd")).isValid()) {
            peerOriginalDate = peer->dateTime.date().toString(
                QStringLiteral("yyyy/MM/dd"));
        }

        PendingIdentity peerEntry;
        peerEntry.item = peer;
        peerEntry.sourcePath = peerPath;
        peerEntry.targetPath = peerPath;
        peerEntry.sourceRoot = canonicalPeerRoot;
        peerEntry.itemPath = peer->path;
        peerEntry.sourceFileName = peer->fileName;
        peerEntry.targetFileName = peer->fileName;
        peerEntry.sourceDateTime = peer->dateTime;
        peerEntry.targetDateTime = peer->dateTime;
        peerEntry.originalDate = peerOriginalDate;
        peerEntry.sourceLinkedFileName = entry.sourceFileName;
        peerEntry.targetLinkedFileName = entry.targetFileName;
        peerEntry.planned = peer->planned;
        pending.append(peerEntry);
        linkedPeerAddresses.insert(peer);
    }

    if (pending.isEmpty()) {
        result.success = true;
        result.cacheUpdated = true;
        result.cleanupComplete = true;
        return result;
    }

    const auto pendingIsStable = [&] {
        if (!mutation.ownersStable() || !guardedCache
            || !guardedContext || !guardedAthlete
            || guardedCache->context != guardedContext.data()
            || guardedContext->athlete != guardedAthlete.data()
            || guardedAthlete->rideCache != guardedCache.data()) {
            return false;
        }
        for (const PendingIdentity &entry : pending) {
            RideItem *const item = entry.item.data();
            if (!guardedCache->ownsLiveRide(item)
                || item->fileName != entry.sourceFileName
                || item->path != entry.itemPath
                || item->dateTime != entry.sourceDateTime
                || item->planned != entry.planned
                || item->isDirty()
                || item->getLinkedFileName()
                    != entry.sourceLinkedFileName) {
                return false;
            }
        }
        return true;
    };
    if (!pendingIsStable()) {
        result.error = tr(
            "The activity collection changed before identity staging");
        return result;
    }

    PlanReplacement::Specification specification;
    specification.athleteRoot = athleteRoot;
    specification.scopeRoot = athleteRoot;
    struct TargetStage
    {
        QString description;
        bool activity = false;
        QString targetFileName;
        std::function<bool(const QString &, QString &)> stage;
    };
    QList<TargetStage> stages;
    QSet<QString> inputKeys;
    QSet<QString> removalKeys;
    QSet<QString> targetKeys;
    QSet<QString> absentTargetKeys;
    const auto appendInput = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (inputKeys.contains(key)) return;
        inputKeys.insert(key);
        specification.inputPaths.append(path);
    };
    const auto appendRemoval = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (removalKeys.contains(key)) return false;
        removalKeys.insert(key);
        specification.removalPaths.append(path);
        return true;
    };
    const auto appendTarget = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (targetKeys.contains(key)) return false;
        targetKeys.insert(key);
        specification.targetPaths.append(path);
        return true;
    };
    const auto appendAbsentTarget = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (absentTargetKeys.contains(key)) return false;
        absentTargetKeys.insert(key);
        specification.mustRemainAbsentPaths.append(path);
        return true;
    };

    for (const PendingIdentity &entry : pending) {
        if (!appendRemoval(entry.sourcePath)
            || !appendTarget(entry.targetPath)) {
            result.error = tr(
                "An activity identity transaction path is duplicated");
            return result;
        }
        appendInput(entry.sourcePath);
        const QPointer<RideItem> guardedItem(entry.item);
        const QString sourcePath = entry.sourcePath;
        const QString sourceFileName = entry.sourceFileName;
        const QString targetFileName = entry.targetFileName;
        const QDateTime targetDateTime = entry.targetDateTime;
        const QString originalDate = entry.originalDate;
        const QString linkedFileName =
            entry.targetLinkedFileName;
        const bool planned = entry.planned;
        stages.append({
            targetFileName, true, targetFileName,
            [guardedCache, guardedItem, sourcePath,
             sourceFileName, targetFileName, targetDateTime,
             originalDate, linkedFileName, planned,
             &pendingIsStable]
            (const QString &stagingPath, QString &error) {
                if (!guardedCache || !guardedItem
                    || !pendingIsStable()) {
                    error = QObject::tr(
                        "The activity changed before identity staging");
                    return false;
                }
                if (!guardedCache->stageActivityIdentityChange(
                        sourcePath, sourceFileName,
                        targetFileName, targetDateTime,
                        originalDate, linkedFileName, planned,
                        stagingPath, error)) {
                    return false;
                }
                if (!guardedItem || !pendingIsStable()) {
                    error = QObject::tr(
                        "The activity changed during identity staging");
                    return false;
                }
                return true;
            }});

        for (const DerivedMutation &derived : entry.derived) {
            if ((derived.removeSource
                 && !appendRemoval(derived.sourcePath))
                || !appendTarget(derived.targetPath)) {
                result.error = tr(
                    "A derived activity transaction path is duplicated");
                return result;
            }
            appendInput(derived.sourcePath);
            const QString sourcePath = derived.sourcePath;
            stages.append({
                derived.targetPath, false, {},
                [sourcePath, &pendingIsStable]
                (const QString &stagingPath, QString &error) {
                    if (!pendingIsStable()) {
                        error = QObject::tr(
                            "The activity changed before derived-file staging");
                        return false;
                    }
                    if (!QFile::copy(sourcePath, stagingPath)) {
                        error = QObject::tr(
                            "A derived activity file could not be staged: %1")
                                        .arg(sourcePath);
                        return false;
                    }
                    if (!pendingIsStable()) {
                        error = QObject::tr(
                            "The activity changed during derived-file staging");
                        return false;
                    }
                    return true;
                }});
        }
        for (const QString &path : entry.absentDerivedTargets) {
            if (!appendAbsentTarget(path)) {
                result.error = tr(
                    "A derived activity absence path is duplicated");
                return result;
            }
        }
    }

    QString journalError;
    std::shared_ptr<PlanReplacement::Journal> journal =
        PlanReplacement::Journal::prepare(
            specification, journalError);
    if (!journal) {
        result.error = journalError.isEmpty()
            ? tr("The activity identity transaction could not be prepared")
            : journalError;
        return result;
    }
    const auto rollback = [&](const QString &detail) {
        result.error = detail;
        QString cleanupError;
        if (!journal->cleanupAfterRollback(cleanupError)) {
            if (!result.error.isEmpty())
                result.error.append(QStringLiteral("; "));
            result.error.append(cleanupError);
        }
    };

    for (int index = 0; index < stages.size(); ++index) {
        QString stageError;
        if (!stages.at(index).stage(
                journal->stagingPath(index), stageError)) {
            rollback(stageError.isEmpty()
                ? tr("An activity identity target could not be staged: %1")
                      .arg(stages.at(index).description)
                : stageError);
            return result;
        }
        if (!journal->recordStaged(index, stageError)) {
            rollback(stageError);
            return result;
        }
        if (stages.at(index).activity
            && !guardedCache->validatePlannedActivityStage(
                journal->stagingPath(index),
                stages.at(index).targetFileName,
                stageError)) {
            rollback(stageError);
            return result;
        }
    }

    QString resetError;
    if (!mutation.beginReset(resetError)) {
        rollback(resetError);
        return result;
    }
    guardedCache->purgeDestroyedRowsInsideModelReset();
    if (!pendingIsStable()) {
        mutation.endReset();
        rollback(tr(
            "The activity collection changed before identity publication"));
        return result;
    }

    QString publishError;
    const bool publishClean =
        journal->publishAndCommit(publishError);
    bool committed = false;
    QString commitStateError;
    if (!journal->commitState(committed, commitStateError)) {
        mutation.endReset();
        result.error = commitStateError.isEmpty()
            ? publishError : commitStateError;
        return result;
    }
    if (!committed) {
        mutation.endReset();
        rollback(publishError);
        return result;
    }
    result.committed = true;
    if (!publishClean && !publishError.isEmpty())
        result.warnings.append(publishError);

    if (!pendingIsStable()) {
        mutation.endReset();
        result.error = tr(
            "The activity collection changed after identity files were committed");
    } else {
        guardedCache->invalidateStartupSnapshots();
        for (const PendingIdentity &entry : pending) {
            RideItem *const item = entry.item.data();
            item->close();
            item->dateTime = entry.targetDateTime;
            item->setFileName(
                entry.sourceRoot, entry.targetFileName);
            item->metadata_.insert(
                QStringLiteral("Year"),
                entry.targetDateTime.toString(
                    QStringLiteral("yyyy")));
            item->metadata_.insert(
                QStringLiteral("Month"),
                entry.targetDateTime.toString(
                    QStringLiteral("MMMM")));
            item->metadata_.insert(
                QStringLiteral("Weekday"),
                entry.targetDateTime.toString(
                    QStringLiteral("ddd")));
            item->metadata_.insert(
                QStringLiteral("Filename"),
                entry.targetFileName);
            item->metadata_.insert(
                QStringLiteral("Original Date"),
                entry.originalDate);
            if (entry.targetLinkedFileName.isEmpty()) {
                item->metadata_.remove(
                    QStringLiteral("Linked Filename"));
            } else {
                item->metadata_.insert(
                    QStringLiteral("Linked Filename"),
                    entry.targetLinkedFileName);
            }
            item->setDirty(false);
            item->isstale = true;
        }
        std::sort(
            guardedCache->rides_.begin(),
            guardedCache->rides_.end(),
            [](const RideItem *left, const RideItem *right) {
                return left->dateTime < right->dateTime;
        });
        mutation.endReset();
        result.cacheUpdated = mutation.ownersStable();
        if (result.cacheUpdated) {
            for (const PendingIdentity &entry : pending) {
                RideItem *const item = entry.item.data();
                if (!guardedCache->ownsLiveRide(item)
                    || item->fileName != entry.targetFileName
                    || item->path != entry.sourceRoot
                    || item->dateTime != entry.targetDateTime
                    || item->planned != entry.planned
                    || item->getLinkedFileName()
                        != entry.targetLinkedFileName
                    || item->isDirty()) {
                    result.cacheUpdated = false;
                    break;
                }
            }
        }
        if (!result.cacheUpdated) {
            result.error = tr(
                "The activity cache changed after identity files were committed");
        }
    }

    QString cleanupError;
    result.cleanupComplete =
        journal->cleanupAfterCommit(cleanupError);
    if (!cleanupError.isEmpty())
        result.warnings.append(cleanupError);
    result.affectedCount = pending.size();
    result.success = result.committed && result.cacheUpdated;

    if (result.cacheUpdated && guardedContext && guardedCache) {
        for (const PendingIdentity &entry : pending) {
            const QPointer<RideItem> item(entry.item);
            if (!item || !guardedCache->ownsLiveRide(item.data())) {
                result.cacheUpdated = false;
                result.success = false;
                result.error = tr(
                    "A moved activity disappeared during notification");
                break;
            }
            guardedContext->notifyRideChanged(item.data());
            if (guardedContext && guardedContext->ride == item.data())
                guardedContext->notifyRideSelected(item.data());
            if (!mutation.ownersStable()
                || !guardedCache || !guardedContext || !item
                || !guardedCache->ownsLiveRide(item.data())) {
                result.cacheUpdated = false;
                result.success = false;
                result.error = tr(
                    "A moved activity disappeared during notification after its files were committed");
                break;
            }
        }
    }
    return result;
}

RideCache::OperationResult
RideCache::moveActivity
(RideItem *item, const QDateTime &newDateTime)
{
    return changeActivityIdentitiesAtomically({{item, newDateTime}});
}

RideCache::OperationResult
RideCache::copyPlannedActivity
(RideItem *sourceItem, const QDate &newDate, QTime newTime)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(sourceItem)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }
    const QTime targetTime = newTime.isValid()
        ? newTime : sourceItem->dateTime.time();
    const PlannedReplacementResult replacement =
        replacePlannedActivityCopies(
            {}, {{sourceItem, newDate, targetTime}});

    result.committed = replacement.committed;
    result.cacheUpdated = replacement.cacheUpdated;
    result.cleanupComplete = replacement.cleanupComplete;
    result.warnings = replacement.warnings;
    result.success = replacement.committed
        && replacement.cacheUpdated
        && replacement.addedCount == 1;
    result.affectedCount = replacement.addedCount;
    result.error = replacement.error;
    if (!result.success && result.error.isEmpty()) {
        result.error = replacement.committed
            ? tr("The planned activity copy was committed, but its cache could not be updated")
            : tr("The planned activity copy did not complete");
    }

    return result;
}

RideCache::OperationResult
RideCache::copyPlannedActivities
(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (sourceItemsAndTargets.isEmpty()) {
        result.error = tr("No files specified");
        return result;
    }
    QList<PlannedActivityCopyRequest> copies;
    copies.reserve(sourceItemsAndTargets.size());
    for (const std::pair<RideItem*, QDate> &pair : sourceItemsAndTargets) {
        if (!ownsLiveRide(pair.first)) {
            result.error = tr(
                "A source activity is no longer in the activity list");
            return result;
        }
        copies.append({
            pair.first, pair.second, QTime()});
    }

    const PlannedReplacementResult replacement =
        replacePlannedActivityCopies({}, copies);
    result.committed = replacement.committed;
    result.cacheUpdated = replacement.cacheUpdated;
    result.cleanupComplete = replacement.cleanupComplete;
    result.warnings = replacement.warnings;
    result.success = replacement.committed
        && replacement.cacheUpdated
        && replacement.addedCount == copies.size();
    result.affectedCount = replacement.addedCount;
    result.error = replacement.error;
    if (!result.success && result.error.isEmpty()) {
        result.error = replacement.committed
            ? tr("The planned activity copies were committed, but their cache could not be updated")
            : tr("The planned activity copies did not complete");
    }

    return result;
}

RideCache::OperationResult
RideCache::shiftPlannedActivities
(const QDate &fromDate, int dayOffset)
{
    OperationResult result;
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!fromDate.isValid()) {
        result.error = tr("Invalid from date specified");
        return result;
    }
    if (dayOffset == 0) {
        result.success = true;
        result.cacheUpdated = true;
        result.cleanupComplete = true;
        result.affectedCount = 0;
        return result;
    }

    QList<RideItem*> itemsToShift;
    for (RideItem *item : rides()) {
        if (ownsLiveRide(item) && item->planned
            && item->dateTime.date() >= fromDate) {
            itemsToShift.append(item);
        }
    }
    if (itemsToShift.isEmpty()) {
        result.success = true;
        result.cacheUpdated = true;
        result.cleanupComplete = true;
        result.affectedCount = 0;
        return result;
    }

    // Prevent shifting any activity to before fromDate.
    int effectiveOffset = dayOffset;
    if (dayOffset < 0) {
        QDate earliestDate = itemsToShift.constFirst()->dateTime.date();
        for (RideItem *item : std::as_const(itemsToShift)) {
            if (item->dateTime.date() < earliestDate) {
                earliestDate = item->dateTime.date();
            }
        }
        int maxBackwardShift = fromDate.daysTo(earliestDate);
        if (-dayOffset > maxBackwardShift) {
            effectiveOffset = -maxBackwardShift;
        }
        if (effectiveOffset == 0) {
            result.success = true;
            result.cacheUpdated = true;
            result.cleanupComplete = true;
            result.affectedCount = 0;
            return result;
        }
    }

    // Avoid filename collisions by moving the farthest item first.
    if (effectiveOffset > 0) {
        std::sort(
            itemsToShift.begin(), itemsToShift.end(),
            [](const RideItem *left, const RideItem *right) {
                return left->dateTime > right->dateTime;
            });
    } else {
        std::sort(
            itemsToShift.begin(), itemsToShift.end(),
            [](const RideItem *left, const RideItem *right) {
                return left->dateTime < right->dateTime;
            });
    }

    QList<ActivityIdentityMutationRequest> requests;
    requests.reserve(itemsToShift.size());
    for (RideItem *item : std::as_const(itemsToShift)) {
        requests.append({
            item,
            item->dateTime.addDays(effectiveOffset)});
    }
    return changeActivityIdentitiesAtomically(requests);
}
