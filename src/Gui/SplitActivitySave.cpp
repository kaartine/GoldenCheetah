/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "SplitActivitySave.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace {

QString portablePathKey(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath())
        .normalized(QString::NormalizationForm_C)
        .toCaseFolded();
}

bool syncChangedDirectories(
    const QStringList &paths,
    const AtomicDirectorySyncFunction &syncDirectory,
    QString &error)
{
    QSet<QString> synced;
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString directory =
            QFileInfo(path).absolutePath();
        const QString key = atomicFilePathKey(directory);
        if (synced.contains(key)) continue;
        synced.insert(key);

        QString syncError;
        if (!syncDirectory(path, syncError)) {
            if (syncError.isEmpty()) {
                syncError = QStringLiteral(
                    "Cannot sync an activity directory");
            }
            appendAtomicFileError(error, syncError);
            return false;
        }
    }
    return true;
}

void syncRollbackDirectories(
    const QStringList &paths,
    const AtomicDirectorySyncFunction &syncDirectory,
    QString &error)
{
    QString syncError;
    if (!syncChangedDirectories(paths, syncDirectory, syncError)) {
        appendAtomicFileError(error, syncError);
    }
}

bool restorePreviousBackup(
    const QString &previousBackupPath,
    const QString &backupPath,
    const AtomicDirectorySyncFunction &syncDirectory,
    const AtomicMoveFunction &move,
    QString &error)
{
    if (previousBackupPath.isEmpty()) return true;

    QString moveError;
    if (!move(previousBackupPath, backupPath, moveError)) {
        appendAtomicFileError(
            error,
            moveError.isEmpty()
                ? QStringLiteral(
                      "cannot restore the previous activity backup")
                : moveError);
        return false;
    }
    syncRollbackDirectories(
        { previousBackupPath, backupPath }, syncDirectory, error);
    return true;
}

bool validSplitFileName(const QString &fileName)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9]{4}_[0-9]{2}_[0-9]{2}_[0-9]{2}_[0-9]{2}_[0-9]{2}\\.json$"));
    if (!pattern.match(fileName).hasMatch()) return false;

    const QString timestamp = fileName.left(fileName.size() - 5);
    const QDateTime parsed = QDateTime::fromString(
        timestamp, QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    return parsed.isValid()
        && parsed.toString(QStringLiteral("yyyy_MM_dd_HH_mm_ss"))
            == timestamp;
}


constexpr qsizetype MaximumSplitOutputs = 1000;
constexpr qsizetype MaximumSplitNamespaceEntries = 1024;
constexpr qsizetype MaximumSplitJournalEntries =
    MaximumSplitOutputs * 2 + 16;
constexpr qint64 MaximumSplitManifestSize = 1024 * 1024;
constexpr qint64 MaximumSplitPayloadSize = 256LL * 1024 * 1024;
constexpr qint64 MaximumSplitPayloadAggregate = 1024LL * 1024 * 1024;
constexpr qint64 MaximumSplitCleanupRecordSize = 64 * 1024;
// Successful source recovery can read an accepted payload ten times across
// durable moves, verification and quarantine-backed cleanup. Producing a
// verified recovery path after the final cleanup fails can add two passes.
// Metadata is bounded separately, including four reads of every cleanup
// record.
constexpr qint64 MaximumSplitRecoveryMetadataReadWork =
    4LL * (MaximumSplitOutputs + 1) * MaximumSplitCleanupRecordSize
    + 8LL * MaximumSplitManifestSize;
constexpr qint64 MaximumSplitReadWork =
    12LL * MaximumSplitPayloadAggregate
    + MaximumSplitRecoveryMetadataReadWork;
constexpr qint64 MaximumSplitRecoveryOperations = 200000;
constexpr qint64 MaximumSplitRecoveryMilliseconds = 30000;
constexpr qsizetype MaximumSplitPathComponents = 16;
constexpr qsizetype MaximumSplitPathLength = 4096;

const QString SplitTransactionsName =
    QStringLiteral(".gc-transactions");
const QString SplitNamespaceName =
    QStringLiteral("split-activity");
const QString SplitManifestName =
    QStringLiteral("manifest.json");
const QString SplitIntentName =
    QStringLiteral("intent.json");
const QString SplitCommittingMarkerName =
    QStringLiteral("COMMITTING");
const QString SplitCommitMarkerName =
    QStringLiteral("COMMITTED");

struct SplitReadBudget
{
    explicit SplitReadBudget(bool recovery = false)
        : remaining(MaximumSplitReadWork),
          remainingOperations(MaximumSplitRecoveryOperations),
          deadlineMilliseconds(
              recovery ? MaximumSplitRecoveryMilliseconds : -1)
    {
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
        if (recovery) {
            const qint64 byteOverride =
                splitActivitySaveRecoveryByteLimitForTest();
            const qint64 operationOverride =
                splitActivitySaveRecoveryOperationLimitForTest();
            const qint64 deadlineOverride =
                splitActivitySaveRecoveryDeadlineForTest();
            if (byteOverride >= 0) remaining = byteOverride;
            if (operationOverride >= 0) {
                remainingOperations = operationOverride;
            }
            if (deadlineOverride >= 0) {
                deadlineMilliseconds = deadlineOverride;
            }
            splitActivitySaveRecoveryBudgetStarted();
        }
#endif
        timer.start();
    }

    qint64 remaining;
    qint64 remainingOperations;
    qint64 deadlineMilliseconds;
    QElapsedTimer timer;

    bool check(QString &error) const
    {
        qint64 elapsed = timer.elapsed();
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
        const qint64 elapsedOverride =
            splitActivitySaveRecoveryElapsedMillisecondsForTest();
        if (elapsedOverride >= 0) elapsed = elapsedOverride;
#endif
        if (deadlineMilliseconds >= 0
            && elapsed > deadlineMilliseconds) {
            error = QStringLiteral(
                "Split recovery exceeds its elapsed-time budget");
            return false;
        }
        return true;
    }

    bool consumeOperation(QString &error)
    {
        if (!check(error) || remainingOperations <= 0) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Split recovery exceeds its operation budget");
            }
            return false;
        }
        --remainingOperations;
        return true;
    }

    bool consumeBytes(qint64 amount, QString &error)
    {
        if (!check(error)) return false;
        if (amount < 0 || amount > remaining) {
            error = QStringLiteral(
                "Split recovery exceeds its content-read budget");
            return false;
        }
        remaining -= amount;
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
        splitActivitySaveRecoveryBytesConsumed(amount);
#endif
        return true;
    }

    qint64 maximumFor(qint64 requested) const
    {
        const qint64 perFile = requested < 0
            ? MaximumSplitPayloadSize
            : std::min(requested, MaximumSplitPayloadSize);
        return std::min(perFile, remaining);
    }

    AnchoredFileSystem::PinnedFileReadControl readControl()
    {
        return [this](qint64 amount, QString &error) {
            return consumeBytes(amount, error);
        };
    }
};

struct SplitJournalFile
{
    QString relativePath;
    bool exists = false;
    AtomicFileSnapshot contents;
    AnchoredFileSystem::NativeIdentity originalIdentity;
    QByteArray originalGeneration;
    QString storedName;
    AnchoredFileSystem::NativeIdentity storedIdentity;
    QByteArray storedGeneration;
    QString archiveStagePath;
    AnchoredFileSystem::NativeIdentity archiveStageIdentity;
    QByteArray archiveStageGeneration;
    QString retiredPath;
};

struct SplitJournalOutput
{
    QString relativePath;
    QString storedName;
    AtomicFileSnapshot contents;
    AnchoredFileSystem::NativeIdentity storedIdentity;
    QByteArray storedGeneration;
    QString publicationStagePath;
    AnchoredFileSystem::NativeIdentity publicationStageIdentity;
    QByteArray publicationStageGeneration;
};

struct SplitJournalManifest
{
    QString id;
    bool keepOriginal = false;
    SplitJournalFile source;
    SplitJournalFile backup;
    QList<SplitJournalOutput> outputs;
};

struct SplitJournalIntent
{
    QString id;
    QStringList artifactPaths;
};

struct SplitCleanupRecord
{
    QString relativePath;
    AnchoredFileSystem::NativeIdentity identity;
    QByteArray generation;
    AtomicFileSnapshot contents;
};

struct ResolvedSplitPath
{
    QString relativePath;
    QString absolutePath;
    AnchoredFileSystem::DirectoryAnchor parent;
    AnchoredFileSystem::EntryRef entry;
};

struct ResolvedSplitTransaction
{
    ResolvedSplitPath source;
    ResolvedSplitPath backup;
    ResolvedSplitPath sourceArchiveStage;
    ResolvedSplitPath retiredSource;
    ResolvedSplitPath retiredBackup;
    QList<ResolvedSplitPath> outputs;
    QList<ResolvedSplitPath> outputStages;
};

struct SplitNamespace
{
    QString root;
    QString namespacePath;
    AnchoredFileSystem::DirectoryAnchor rootDirectory;
    AnchoredFileSystem::DirectoryAnchor transactionsDirectory;
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    bool exists = false;
};

struct SplitJournal
{
    QString id;
    QString path;
    AnchoredFileSystem::DirectoryAnchor directory;
};

struct PreparedSplitOutput
{
    SplitActivityOutput request;
    SplitJournalOutput journal;
    ResolvedSplitPath target;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> staged;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> publicationStage;
};

QString portableComponentKey(const QString &component)
{
    return component.normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString relativePathKey(const QString &path)
{
    return QDir::fromNativeSeparators(path)
        .normalized(QString::NormalizationForm_C)
        .toCaseFolded();
}

bool validTransactionId(const QString &id)
{
    if (id.size() != 36 || id != id.toLower()) return false;
    const QUuid uuid(id);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == id;
}

bool validSplitRelativePath(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.isEmpty()
        || normalized.size() > MaximumSplitPathLength
        || QDir::isAbsolutePath(normalized)
        || normalized != path || normalized != QDir::cleanPath(normalized)) {
        return false;
    }

    const QString reservedKey =
        portableComponentKey(SplitTransactionsName);
    const QStringList components =
        normalized.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (components.size() > MaximumSplitPathComponents) return false;
    for (const QString &component : components) {
        if (!atomicFileNameIsPortableComponent(component)
            || portableComponentKey(component) == reservedKey) {
            return false;
        }
    }
    return true;
}

bool splitPathsCollide(const QString &left, const QString &right)
{
    const QString leftKey = relativePathKey(left);
    const QString rightKey = relativePathKey(right);
    return leftKey == rightKey
        || leftKey.startsWith(rightKey + QLatin1Char('/'))
        || rightKey.startsWith(leftKey + QLatin1Char('/'));
}

bool splitPathsAreDisjoint(
    const QList<QString> &paths,
    SplitReadBudget *budget,
    QString &error)
{
    QList<QString> keys;
    keys.reserve(paths.size());
    for (const QString &path : paths) {
        if (budget && !budget->consumeOperation(error)) return false;
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
        splitActivitySavePathValidationStep();
#endif
        keys.append(relativePathKey(path));
    }
    bool comparisonBudgetAvailable = true;
    std::sort(
        keys.begin(),
        keys.end(),
        [budget, &comparisonBudgetAvailable, &error](
                const QString &left, const QString &right) {
            if (budget && comparisonBudgetAvailable
                && !budget->consumeOperation(error)) {
                comparisonBudgetAvailable = false;
            }
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
            splitActivitySavePathValidationStep();
#endif
            return left < right;
        });
    if (!comparisonBudgetAvailable) return false;
    for (qsizetype index = 1; index < keys.size(); ++index) {
        if (budget && !budget->consumeOperation(error)) return false;
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
        splitActivitySavePathValidationStep();
#endif
        const QString &previous = keys.at(index - 1);
        const QString &current = keys.at(index);
        if (current == previous
            || current.startsWith(previous + QLatin1Char('/'))) {
            error = QStringLiteral(
                "The split transaction contains colliding paths");
            return false;
        }
    }
    return !budget || budget->check(error);
}

bool makeSplitRelativePath(
    const QString &root,
    const QString &path,
    QString &relative,
    QString &error)
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        error = QStringLiteral("A split transaction path must be absolute");
        return false;
    }
    const QString absolute =
        QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    relative = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(absolute));
    if (!validSplitRelativePath(relative)
        || atomicFilePathKey(QDir(root).filePath(relative))
            != atomicFilePathKey(absolute)) {
        error = QStringLiteral(
            "A split transaction path escapes the athlete root");
        return false;
    }
    return true;
}

bool normalizeSplitRoot(
    const QString &candidate, QString &root, QString &error)
{
    error.clear();
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate)) {
        error = QStringLiteral(
            "The split transaction root must be absolute");
        return false;
    }

    const QString absolute =
        QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    const QFileInfo info(absolute);
    const QString canonical =
        QDir::cleanPath(info.canonicalFilePath());
    if (!info.exists() || !info.isDir() || info.isSymLink()
        || canonical.isEmpty()
        || atomicFilePathKey(absolute) != atomicFilePathKey(canonical)) {
        error = QStringLiteral(
            "The split transaction root contains an unsafe path component");
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor directory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            absolute, directory, error)
        || !directory.pathMatches(error)
        || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
            directory, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The split transaction root cannot be anchored");
        }
        return false;
    }
    root = absolute;
    return true;
}

bool directorySnapshotsMatch(
    const QList<AnchoredFileSystem::DirectoryEntry> &left,
    const QList<AnchoredFileSystem::DirectoryEntry> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const auto &leftEntry = left.at(index);
        const auto &rightEntry = right.at(index);
        if (leftEntry.name != rightEntry.name
            || leftEntry.kind != rightEntry.kind
            || leftEntry.identity != rightEntry.identity
            || leftEntry.identity.linkCount()
                != rightEntry.identity.linkCount()) {
            return false;
        }
    }
    return true;
}

bool openPrivateSplitDirectory(
    const AnchoredFileSystem::DirectoryAnchor &parent,
    const QString &component,
    bool create,
    AnchoredFileSystem::DirectoryAnchor &directory,
    bool &exists,
    QString &error)
{
    directory = {};
    exists = false;
    if (!parent.pathMatches(error)
        || !parent.openChildIfExists(
            component, directory, exists, error)) {
        return false;
    }

    if (!exists) {
        if (!create) return true;
        if (!AnchoredFileSystem::validateCurrentUserControlledDirectory(
                parent, error)) {
            return false;
        }
        const AnchoredFileSystem::MutationResult creation =
            AnchoredFileSystem::createPrivateFixedChildDirectory(
                parent, component, directory);
        if (creation.effect
            != AnchoredFileSystem::MutationEffect::AppliedDurable) {
            if (creation.effect
                    == AnchoredFileSystem::MutationEffect::AppliedNotDurable
                && parent.sync(error)
                && parent.pathMatches(error)
                && directory.pathMatches(error)) {
                exists = true;
            } else {
                error = creation.error.isEmpty()
                    ? QStringLiteral(
                          "Cannot create a private split journal directory")
                    : creation.error;
                return false;
            }
        } else {
            exists = true;
        }
    }

    const AnchoredFileSystem::NativeIdentity securedIdentity =
        directory.identity();
    bool verifiedExists = false;
    AnchoredFileSystem::DirectoryAnchor verifiedDirectory;
    if (!AnchoredFileSystem::validateCurrentUserOwnedDirectory(
            directory, error)
        || !AnchoredFileSystem::hardenPrivateDirectory(directory, error)
        || !parent.pathMatches(error)
        || !directory.pathMatches(error)
        || !parent.openChildIfExists(
            component, verifiedDirectory, verifiedExists, error)
        || !verifiedExists
        || verifiedDirectory.identity() != securedIdentity
        || !verifiedDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The private split journal directory changed");
        }
        return false;
    }
    directory = verifiedDirectory;
    return true;
}

bool openSplitNamespace(
    const QString &root,
    bool create,
    SplitNamespace &result,
    QString &error)
{
    result = {};
    result.root = root;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            root, result.rootDirectory, error)
        || !result.rootDirectory.pathMatches(error)
        || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
            result.rootDirectory, error)) {
        return false;
    }

    bool transactionsExist = false;
    if (!openPrivateSplitDirectory(
            result.rootDirectory,
            SplitTransactionsName,
            create,
            result.transactionsDirectory,
            transactionsExist,
            error)) {
        return false;
    }
    if (!transactionsExist) return true;

    bool namespaceExists = false;
    if (!openPrivateSplitDirectory(
            result.transactionsDirectory,
            SplitNamespaceName,
            create,
            result.namespaceDirectory,
            namespaceExists,
            error)) {
        return false;
    }
    if (!namespaceExists) return true;

    result.namespacePath = QDir(root).filePath(
        SplitTransactionsName + QLatin1Char('/')
        + SplitNamespaceName);
    result.exists = true;
    return true;
}

QString splitTransactionLeaseTarget(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral("split-activity-transaction"));
}

void reportSplitTransition(const char *transition)
{
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
    splitActivitySaveDurableTransitionReached(transition);
#else
    Q_UNUSED(transition)
#endif
}

bool mutationAppliedDurably(
    const AnchoredFileSystem::MutationResult &result,
    const QList<AnchoredFileSystem::DirectoryAnchor> &parents,
    QString &error)
{
    if (result.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        for (const auto &parent : parents) {
            if (!parent.pathMatches(error)) return false;
        }
        return true;
    }
    if (result.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        for (const auto &parent : parents) {
            if (!parent.sync(error) || !parent.pathMatches(error)) {
                return false;
            }
        }
        return true;
    }
    error = result.error.isEmpty()
        ? QStringLiteral("An anchored split transaction mutation failed")
        : result.error;
    if (!result.verifiedRecoveryPath.isEmpty()) {
        appendAtomicFileError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(result.verifiedRecoveryPath));
    }
    return false;
}

bool movePinnedFile(
    AnchoredFileSystem::PinnedFile &source,
    const ResolvedSplitPath &sourcePath,
    const AnchoredFileSystem::EntryRef &destination,
    const AnchoredFileSystem::DirectoryAnchor &destinationParent,
    SplitReadBudget &budget,
    QString &error)
{
#ifdef GC_SPLIT_ACTIVITY_SAVE_TEST_HOOKS
    if (!splitActivitySaveMoveAllowed(
            sourcePath.absolutePath,
            destination.displayPath())) {
        error = QStringLiteral(
            "Injected cross-filesystem split move rejection");
        return false;
    }
#endif
    if (!budget.consumeOperation(error)) return false;
    const auto readControl = budget.readControl();
    const AnchoredFileSystem::MutationResult result =
        AnchoredFileSystem::moveNoReplace(
            source, destination, readControl);
    return mutationAppliedDurably(
        result,
        {sourcePath.parent, destinationParent},
        error);
}

bool removePinnedFile(
    AnchoredFileSystem::PinnedFile &file,
    const AnchoredFileSystem::DirectoryAnchor &parent,
    SplitReadBudget &budget,
    QString &error)
{
    if (!budget.consumeOperation(error)) return false;
    const auto readControl = budget.readControl();
    const AnchoredFileSystem::MutationResult result =
        AnchoredFileSystem::remove(file, readControl);
    return mutationAppliedDurably(result, {parent}, error);
}

bool resolveAnchoredSplitPath(
    const AnchoredFileSystem::DirectoryAnchor &rootDirectory,
    const QString &root,
    const QString &relative,
    ResolvedSplitPath &resolved,
    QString &error)
{
    resolved = {};
    if (!validSplitRelativePath(relative)) {
        error = QStringLiteral(
            "A split journal contains an invalid relative path");
        return false;
    }

    const QStringList components = relative.split(
        QLatin1Char('/'), Qt::KeepEmptyParts);
    AnchoredFileSystem::DirectoryAnchor parent = rootDirectory;
    QString parentPath = root;
    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        AnchoredFileSystem::DirectoryAnchor child;
        if (!parent.openChild(components.at(index), child, error)
            || !parent.pathMatches(error)
            || !child.pathMatches(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A split path has an unsafe intermediate component");
            }
            return false;
        }
        parent = child;
        parentPath = QDir(parentPath).filePath(components.at(index));
    }

    resolved.relativePath = relative;
    resolved.absolutePath = QDir(root).filePath(relative);
    resolved.parent = parent;
    resolved.entry = parent.entry(components.constLast(), error);
    if (!resolved.entry.isValid()
        || atomicFilePathKey(resolved.entry.displayPath())
            != atomicFilePathKey(resolved.absolutePath)
        || !parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A split path did not resolve to its anchored name");
        }
        return false;
    }
    return true;
}

bool resolveAnchoredSplitSibling(
    const QString &root,
    const QString &relative,
    const ResolvedSplitPath &sibling,
    ResolvedSplitPath &resolved,
    QString &error)
{
    resolved = {};
    if (!validSplitRelativePath(relative)
        || relativePathKey(QFileInfo(relative).path())
            != relativePathKey(
                QFileInfo(sibling.relativePath).path())) {
        error = QStringLiteral(
            "A split journal path leaves its anchored parent");
        return false;
    }

    resolved.relativePath = relative;
    resolved.absolutePath = QDir(root).filePath(relative);
    resolved.parent = sibling.parent;
    resolved.entry = resolved.parent.entry(
        QFileInfo(relative).fileName(), error);
    if (!resolved.entry.isValid()
        || atomicFilePathKey(resolved.entry.displayPath())
            != atomicFilePathKey(resolved.absolutePath)
        || !resolved.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A split sibling path did not resolve to its anchored name");
        }
        return false;
    }
    return true;
}

bool splitParentsAreCurrent(
    const ResolvedSplitTransaction &resolved,
    QString &error)
{
    QHash<QString, AnchoredFileSystem::NativeIdentity> identities;
    QList<ResolvedSplitPath> paths = resolved.outputs;
    paths.append(resolved.outputStages);
    paths.prepend(resolved.source);
    if (resolved.backup.entry.isValid()) paths.append(resolved.backup);
    if (resolved.sourceArchiveStage.entry.isValid()) {
        paths.append(resolved.sourceArchiveStage);
    }
    if (resolved.retiredSource.entry.isValid()) {
        paths.append(resolved.retiredSource);
    }
    if (resolved.retiredBackup.entry.isValid()) {
        paths.append(resolved.retiredBackup);
    }

    for (const ResolvedSplitPath &path : paths) {
        const QString parentKey = atomicFilePathKey(
            QFileInfo(path.absolutePath).absolutePath());
        const auto existing = identities.constFind(parentKey);
        if (existing != identities.constEnd()) {
            if (existing.value() != path.parent.identity()) {
                error = QStringLiteral(
                    "Split paths disagree about a parent directory identity");
                return false;
            }
            continue;
        }
        if (!path.parent.pathMatches(error)) return false;
        identities.insert(parentKey, path.parent.identity());
    }
    return true;
}

bool hasExactKeys(
    const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return keys.size() == expected.size()
        && std::all_of(
            keys.cbegin(), keys.cend(),
            [&expected](const QString &key) {
                return expected.contains(key);
            });
}

QString identityToString(
    const AnchoredFileSystem::NativeIdentity &identity)
{
    return identity.isValid()
        ? QString::fromLatin1(identity.serializedKey().toHex())
        : QString();
}

bool identityFromJson(
    const QJsonValue &value,
    bool required,
    AnchoredFileSystem::NativeIdentity &identity,
    QString &error)
{
    identity = {};
    if (!value.isString()) {
        error = QStringLiteral(
            "A split journal identity has an invalid type");
        return false;
    }
    const QString text = value.toString();
    if (!required && text.isEmpty()) return true;
    static const QRegularExpression expression(
        QStringLiteral("^[0-9a-f]{2,1024}$"));
    if (!expression.match(text).hasMatch()
        || (text.size() % 2) != 0) {
        error = QStringLiteral(
            "A split journal identity is malformed");
        return false;
    }
    const QByteArray key = QByteArray::fromHex(text.toLatin1());
    if (key.isEmpty() || key.at(0) != 'f'
        || QString::fromLatin1(key.toHex()) != text) {
        error = QStringLiteral(
            "A split journal identity is invalid");
        return false;
    }
    identity = AnchoredFileSystem::NativeIdentity(key, 1);
    return true;
}

bool generationFromJson(
    const QJsonValue &value,
    bool required,
    QByteArray &generation,
    QString &error)
{
    generation.clear();
    if (!value.isString()) {
        error = QStringLiteral(
            "A split journal generation has an invalid type");
        return false;
    }
    const QString text = value.toString();
    if (!required && text.isEmpty()) return true;
    static const QRegularExpression expression(
        QStringLiteral("^[0-9a-f]{2,512}$"));
    if (!expression.match(text).hasMatch()
        || (text.size() % 2) != 0) {
        error = QStringLiteral(
            "A split journal generation is malformed");
        return false;
    }
    generation = QByteArray::fromHex(text.toLatin1());
    if (generation.isEmpty()
        || QString::fromLatin1(generation.toHex()) != text) {
        error = QStringLiteral(
            "A split journal generation is invalid");
        return false;
    }
    return true;
}

QJsonObject splitFileToJson(const SplitJournalFile &file)
{
    QJsonObject object;
    object.insert(QStringLiteral("path"), file.relativePath);
    object.insert(QStringLiteral("exists"), file.exists);
    object.insert(
        QStringLiteral("size"),
        QString::number(file.exists ? file.contents.size : 0));
    object.insert(
        QStringLiteral("sha256"),
        file.exists
            ? QString::fromLatin1(file.contents.digest.toHex())
            : QString());
    object.insert(
        QStringLiteral("original_identity"),
        identityToString(file.originalIdentity));
    object.insert(
        QStringLiteral("original_generation"),
        QString::fromLatin1(file.originalGeneration.toHex()));
    object.insert(QStringLiteral("stored"), file.storedName);
    object.insert(
        QStringLiteral("stored_identity"),
        identityToString(file.storedIdentity));
    object.insert(
        QStringLiteral("stored_generation"),
        QString::fromLatin1(file.storedGeneration.toHex()));
    object.insert(
        QStringLiteral("archive_stage_path"),
        file.archiveStagePath);
    object.insert(
        QStringLiteral("archive_stage_identity"),
        identityToString(file.archiveStageIdentity));
    object.insert(
        QStringLiteral("archive_stage_generation"),
        QString::fromLatin1(file.archiveStageGeneration.toHex()));
    object.insert(QStringLiteral("retired_path"), file.retiredPath);
    return object;
}

QByteArray serializeSplitManifest(
    const SplitJournalManifest &manifest)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 6);
    root.insert(QStringLiteral("id"), manifest.id);
    root.insert(QStringLiteral("state"), QStringLiteral("prepared"));
    root.insert(QStringLiteral("keepOriginal"), manifest.keepOriginal);
    root.insert(QStringLiteral("source"), splitFileToJson(manifest.source));
    root.insert(QStringLiteral("backup"), splitFileToJson(manifest.backup));

    QJsonArray outputs;
    for (const SplitJournalOutput &output : manifest.outputs) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), output.relativePath);
        object.insert(QStringLiteral("stored"), output.storedName);
        object.insert(
            QStringLiteral("size"),
            QString::number(output.contents.size));
        object.insert(
            QStringLiteral("sha256"),
            QString::fromLatin1(output.contents.digest.toHex()));
        object.insert(
            QStringLiteral("stored_identity"),
            identityToString(output.storedIdentity));
        object.insert(
            QStringLiteral("stored_generation"),
            QString::fromLatin1(output.storedGeneration.toHex()));
        object.insert(
            QStringLiteral("publication_stage_path"),
            output.publicationStagePath);
        object.insert(
            QStringLiteral("publication_stage_identity"),
            identityToString(output.publicationStageIdentity));
        object.insert(
            QStringLiteral("publication_stage_generation"),
            QString::fromLatin1(
                output.publicationStageGeneration.toHex()));
        outputs.append(object);
    }
    root.insert(QStringLiteral("outputs"), outputs);
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

SplitJournalIntent splitIntentForManifest(
    const SplitJournalManifest &manifest)
{
    SplitJournalIntent intent;
    intent.id = manifest.id;
    if (!manifest.keepOriginal) {
        intent.artifactPaths.append(
            manifest.source.archiveStagePath);
    }
    for (const SplitJournalOutput &output : manifest.outputs) {
        intent.artifactPaths.append(output.publicationStagePath);
    }
    return intent;
}

QByteArray serializeSplitIntent(const SplitJournalIntent &intent)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("id"), intent.id);
    QJsonArray artifacts;
    for (const QString &path : intent.artifactPaths) {
        artifacts.append(path);
    }
    root.insert(QStringLiteral("artifacts"), artifacts);
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

QString splitCleanupRecordName(qsizetype index)
{
    return QStringLiteral("cleanup-%1.json")
        .arg(index, 4, 10, QLatin1Char('0'));
}

QByteArray serializeSplitCleanupRecord(
    const QString &id,
    qsizetype index,
    const SplitCleanupRecord &record)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 2);
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("index"), double(index));
    root.insert(QStringLiteral("path"), record.relativePath);
    root.insert(
        QStringLiteral("identity"),
        identityToString(record.identity));
    root.insert(
        QStringLiteral("generation"),
        QString::fromLatin1(record.generation.toHex()));
    root.insert(
        QStringLiteral("size"),
        QString::number(record.contents.size));
    root.insert(
        QStringLiteral("sha256"),
        QString::fromLatin1(record.contents.digest.toHex()));
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

bool parseSizeAndDigest(
    const QJsonObject &object,
    bool exists,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    const QJsonValue sizeValue = object.value(QStringLiteral("size"));
    const QJsonValue digestValue =
        object.value(QStringLiteral("sha256"));
    if (!sizeValue.isString() || !digestValue.isString()) {
        error = QStringLiteral(
            "A split journal snapshot has invalid types");
        return false;
    }

    const QString sizeText = sizeValue.toString();
    static const QRegularExpression sizeExpression(
        QStringLiteral("^(0|[1-9][0-9]*)$"));
    static const QRegularExpression digestExpression(
        QStringLiteral("^[0-9a-f]{64}$"));
    bool sizeValid = false;
    const qint64 size = sizeText.toLongLong(&sizeValid);
    const QString digestText = digestValue.toString();
    if (!sizeExpression.match(sizeText).hasMatch()
        || !sizeValid || size < 0
        || (exists
            && !digestExpression.match(digestText).hasMatch())
        || (!exists && (size != 0 || !digestText.isEmpty()))) {
        error = QStringLiteral(
            "A split journal snapshot is invalid");
        return false;
    }
    snapshot.size = size;
    snapshot.digest = exists
        ? QByteArray::fromHex(digestText.toLatin1())
        : QByteArray();
    return true;
}

bool parseSplitIntent(
    const QByteArray &contents,
    const QString &expectedId,
    SplitJournalIntent &intent,
    QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "The split transaction intent is malformed");
        return false;
    }
    const QJsonObject root = document.object();
    static const QSet<QString> keys = {
        QStringLiteral("schema"),
        QStringLiteral("id"),
        QStringLiteral("artifacts")};
    if (!hasExactKeys(root, keys)
        || !root.value(QStringLiteral("schema")).isDouble()
        || root.value(QStringLiteral("schema")).toDouble() != 1
        || !root.value(QStringLiteral("id")).isString()
        || root.value(QStringLiteral("id")).toString() != expectedId
        || !validTransactionId(expectedId)
        || !root.value(QStringLiteral("artifacts")).isArray()) {
        error = QStringLiteral(
            "The split transaction intent schema is invalid");
        return false;
    }

    const QJsonArray artifacts =
        root.value(QStringLiteral("artifacts")).toArray();
    if (artifacts.isEmpty()
        || artifacts.size() > MaximumSplitOutputs + 1) {
        error = QStringLiteral(
            "The split transaction intent has an invalid artifact set");
        return false;
    }
    const QRegularExpression sourcePattern(
        QStringLiteral("^\\.gc-split-%1-source\\.stage$")
            .arg(QRegularExpression::escape(expectedId)));
    const QRegularExpression outputPattern(
        QStringLiteral(
            "^\\.gc-split-%1-output-([0-9]{4})\\.stage$")
            .arg(QRegularExpression::escape(expectedId)));
    QSet<QString> paths;
    bool sourceSeen = false;
    int expectedOutput = 0;
    intent = {};
    intent.id = expectedId;
    for (qsizetype index = 0; index < artifacts.size(); ++index) {
        if (!artifacts.at(index).isString()) {
            error = QStringLiteral(
                "The split transaction intent path is invalid");
            return false;
        }
        const QString path = artifacts.at(index).toString();
        const QString name = QFileInfo(path).fileName();
        const QRegularExpressionMatch outputMatch =
            outputPattern.match(name);
        const bool source = sourcePattern.match(name).hasMatch();
        bool outputIndexValid = false;
        const int outputIndex = outputMatch.hasMatch()
            ? outputMatch.captured(1).toInt(&outputIndexValid)
            : -1;
        const QString key = relativePathKey(path);
        if (!validSplitRelativePath(path)
            || paths.contains(key)
            || (source
                && (sourceSeen || index != 0))
            || (!source
                && (!outputMatch.hasMatch()
                    || !outputIndexValid
                    || outputIndex != expectedOutput))) {
            error = QStringLiteral(
                "The split transaction intent path is unsafe");
            return false;
        }
        if (source) {
            sourceSeen = true;
        } else {
            ++expectedOutput;
        }
        paths.insert(key);
        intent.artifactPaths.append(path);
    }
    if (expectedOutput == 0) {
        error = QStringLiteral(
            "The split transaction intent has no outputs");
        return false;
    }
    return true;
}

bool parseSplitCleanupRecord(
    const QByteArray &contents,
    const QString &expectedId,
    qsizetype expectedIndex,
    const QString &expectedPath,
    SplitCleanupRecord &record,
    QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "A split cleanup record is malformed");
        return false;
    }
    const QJsonObject root = document.object();
    static const QSet<QString> keys = {
        QStringLiteral("schema"),
        QStringLiteral("id"),
        QStringLiteral("index"),
        QStringLiteral("path"),
        QStringLiteral("identity"),
        QStringLiteral("generation"),
        QStringLiteral("size"),
        QStringLiteral("sha256")};
    const double index =
        root.value(QStringLiteral("index")).toDouble(-1);
    if (!hasExactKeys(root, keys)
        || !root.value(QStringLiteral("schema")).isDouble()
        || root.value(QStringLiteral("schema")).toDouble() != 2
        || !root.value(QStringLiteral("id")).isString()
        || root.value(QStringLiteral("id")).toString() != expectedId
        || !root.value(QStringLiteral("index")).isDouble()
        || !std::isfinite(index)
        || index != double(expectedIndex)
        || !root.value(QStringLiteral("path")).isString()
        || root.value(QStringLiteral("path")).toString()
            != expectedPath) {
        error = QStringLiteral(
            "A split cleanup record has an invalid schema");
        return false;
    }
    record = {};
    record.relativePath = expectedPath;
    return identityFromJson(
            root.value(QStringLiteral("identity")),
            true,
            record.identity,
            error)
        && generationFromJson(
            root.value(QStringLiteral("generation")),
            true,
            record.generation,
            error)
        && parseSizeAndDigest(root, true, record.contents, error);
}

bool parseSplitJournalFile(
    const QJsonValue &value,
    bool pathRequired,
    SplitJournalFile &file,
    QString &error)
{
    if (!value.isObject()) {
        error = QStringLiteral(
            "A split journal file record is not an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    static const QSet<QString> keys = {
        QStringLiteral("path"),
        QStringLiteral("exists"),
        QStringLiteral("size"),
        QStringLiteral("sha256"),
        QStringLiteral("original_identity"),
        QStringLiteral("original_generation"),
        QStringLiteral("stored"),
        QStringLiteral("stored_identity"),
        QStringLiteral("stored_generation"),
        QStringLiteral("archive_stage_path"),
        QStringLiteral("archive_stage_identity"),
        QStringLiteral("archive_stage_generation"),
        QStringLiteral("retired_path")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("path")).isString()
        || !object.value(QStringLiteral("exists")).isBool()
        || !object.value(QStringLiteral("stored")).isString()
        || !object.value(
            QStringLiteral("archive_stage_path")).isString()
        || !object.value(QStringLiteral("retired_path")).isString()) {
        error = QStringLiteral(
            "A split journal file record has an invalid schema");
        return false;
    }

    file = {};
    file.relativePath =
        object.value(QStringLiteral("path")).toString();
    file.exists =
        object.value(QStringLiteral("exists")).toBool();
    file.storedName =
        object.value(QStringLiteral("stored")).toString();
    file.archiveStagePath = object.value(
        QStringLiteral("archive_stage_path")).toString();
    file.retiredPath =
        object.value(QStringLiteral("retired_path")).toString();
    if ((pathRequired
            && !validSplitRelativePath(file.relativePath))
        || (!pathRequired && !file.relativePath.isEmpty())
        || (!file.archiveStagePath.isEmpty()
            && !validSplitRelativePath(file.archiveStagePath))
        || (!file.retiredPath.isEmpty()
            && !validSplitRelativePath(file.retiredPath))) {
        error = QStringLiteral(
            "A split journal file path is invalid");
        return false;
    }
    return parseSizeAndDigest(
            object, file.exists, file.contents, error)
        && identityFromJson(
            object.value(QStringLiteral("original_identity")),
            file.exists,
            file.originalIdentity,
            error)
        && generationFromJson(
            object.value(QStringLiteral("original_generation")),
            false,
            file.originalGeneration,
            error)
        && identityFromJson(
            object.value(QStringLiteral("stored_identity")),
            file.exists,
            file.storedIdentity,
            error)
        && generationFromJson(
            object.value(QStringLiteral("stored_generation")),
            file.exists,
            file.storedGeneration,
            error)
        && identityFromJson(
            object.value(QStringLiteral("archive_stage_identity")),
            !file.archiveStagePath.isEmpty(),
            file.archiveStageIdentity,
            error)
        && generationFromJson(
            object.value(QStringLiteral("archive_stage_generation")),
            !file.archiveStagePath.isEmpty(),
            file.archiveStageGeneration,
            error);
}

bool parseSplitManifest(
    const QByteArray &contents,
    const QString &expectedId,
    SplitJournalManifest &manifest,
    SplitReadBudget &budget,
    QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "The split transaction manifest is malformed");
        return false;
    }

    const QJsonObject object = document.object();
    static const QSet<QString> keys = {
        QStringLiteral("schema"),
        QStringLiteral("id"),
        QStringLiteral("state"),
        QStringLiteral("keepOriginal"),
        QStringLiteral("source"),
        QStringLiteral("backup"),
        QStringLiteral("outputs")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("schema")).isDouble()
        || !object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("state")).isString()
        || !object.value(QStringLiteral("keepOriginal")).isBool()
        || !object.value(QStringLiteral("outputs")).isArray()) {
        error = QStringLiteral(
            "The split transaction manifest schema is invalid");
        return false;
    }

    const double schema =
        object.value(QStringLiteral("schema")).toDouble();
    manifest = {};
    manifest.id = object.value(QStringLiteral("id")).toString();
    manifest.keepOriginal =
        object.value(QStringLiteral("keepOriginal")).toBool();
    if (!std::isfinite(schema) || schema != 6
        || manifest.id != expectedId
        || !validTransactionId(manifest.id)
        || object.value(QStringLiteral("state")).toString()
            != QStringLiteral("prepared")
        || !parseSplitJournalFile(
            object.value(QStringLiteral("source")),
            true,
            manifest.source,
            error)
        || !parseSplitJournalFile(
            object.value(QStringLiteral("backup")),
            !manifest.keepOriginal,
            manifest.backup,
            error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The split transaction manifest is invalid");
        }
        return false;
    }

    if (!manifest.source.exists
        || manifest.source.originalGeneration.isEmpty()
        || manifest.source.storedName
            != QStringLiteral("source.old")
        || (manifest.keepOriginal
            && (!manifest.source.archiveStagePath.isEmpty()
                || manifest.source.archiveStageIdentity.isValid()
                || !manifest.source.archiveStageGeneration.isEmpty()
                || !manifest.source.retiredPath.isEmpty()
                || manifest.backup.exists
                || !manifest.backup.relativePath.isEmpty()
                || !manifest.backup.storedName.isEmpty()
                || manifest.backup.originalIdentity.isValid()
                || !manifest.backup.originalGeneration.isEmpty()
                || manifest.backup.storedIdentity.isValid()
                || !manifest.backup.storedGeneration.isEmpty()
                || !manifest.backup.archiveStagePath.isEmpty()
                || manifest.backup.archiveStageIdentity.isValid()
                || !manifest.backup.archiveStageGeneration.isEmpty()
                || !manifest.backup.retiredPath.isEmpty()))
        || (!manifest.keepOriginal
            && (manifest.source.archiveStagePath.isEmpty()
                || !manifest.source.archiveStageIdentity.isValid()
                || manifest.source.archiveStageGeneration.isEmpty()
                || manifest.source.retiredPath.isEmpty()
                || !manifest.backup.archiveStagePath.isEmpty()
                || manifest.backup.archiveStageIdentity.isValid()
                || !manifest.backup.archiveStageGeneration.isEmpty()
                || manifest.backup.retiredPath.isEmpty()
                || (manifest.backup.exists
                    && (manifest.backup.storedName
                            != QStringLiteral("backup.old")
                        || manifest.backup.originalGeneration.isEmpty()))
                || (!manifest.backup.exists
                    && (!manifest.backup.storedName.isEmpty()
                        || manifest.backup.originalIdentity.isValid()
                        || !manifest.backup.originalGeneration.isEmpty()
                        || manifest.backup.storedIdentity.isValid()
                        || !manifest.backup.storedGeneration.isEmpty()))))) {
        error = QStringLiteral(
            "The split transaction source or backup record is invalid");
        return false;
    }

    qint64 aggregatePayload = manifest.source.contents.size;
    if (manifest.source.contents.size > MaximumSplitPayloadSize
        || (manifest.backup.exists
            && manifest.backup.contents.size > MaximumSplitPayloadSize)
        || (manifest.backup.exists
            && aggregatePayload
                > MaximumSplitPayloadAggregate
                    - manifest.backup.contents.size)) {
        error = QStringLiteral(
            "The split manifest exceeds its content-read budget");
        return false;
    }
    if (manifest.backup.exists) {
        aggregatePayload += manifest.backup.contents.size;
    }

    QSet<QString> identityKeys;
    const auto addIdentity = [&identityKeys](
            const AnchoredFileSystem::NativeIdentity &identity) {
        const QString key = identityToString(identity);
        if (key.isEmpty() || identityKeys.contains(key)) {
            return false;
        }
        identityKeys.insert(key);
        return true;
    };
    if (!addIdentity(manifest.source.originalIdentity)
        || !addIdentity(manifest.source.storedIdentity)
        || (!manifest.keepOriginal
            && !addIdentity(manifest.source.archiveStageIdentity))
        || (manifest.backup.exists
            && (!addIdentity(manifest.backup.originalIdentity)
                || !addIdentity(manifest.backup.storedIdentity)))) {
        error = QStringLiteral(
            "The split manifest contains colliding file identities");
        return false;
    }

    const QJsonArray outputs =
        object.value(QStringLiteral("outputs")).toArray();
    if (outputs.isEmpty()
        || outputs.size() > MaximumSplitOutputs) {
        error = QStringLiteral(
            "The split transaction output set is invalid");
        return false;
    }

    QList<QString> productionPaths;
    productionPaths.append(manifest.source.relativePath);
    if (!manifest.keepOriginal) {
        productionPaths.append(manifest.backup.relativePath);
    }

    for (qsizetype index = 0; index < outputs.size(); ++index) {
        if (!budget.consumeOperation(error)) return false;
        const QJsonValue value = outputs.at(index);
        if (!value.isObject()) {
            error = QStringLiteral(
                "A split transaction output is not an object");
            return false;
        }
        const QJsonObject outputObject = value.toObject();
        static const QSet<QString> outputKeys = {
            QStringLiteral("path"),
            QStringLiteral("stored"),
            QStringLiteral("size"),
            QStringLiteral("sha256"),
            QStringLiteral("stored_identity"),
            QStringLiteral("stored_generation"),
            QStringLiteral("publication_stage_path"),
            QStringLiteral("publication_stage_identity"),
            QStringLiteral("publication_stage_generation")};
        if (!hasExactKeys(outputObject, outputKeys)
            || !outputObject.value(QStringLiteral("path")).isString()
            || !outputObject.value(QStringLiteral("stored")).isString()) {
            error = QStringLiteral(
                "A split transaction output has an invalid schema");
            return false;
        }

        SplitJournalOutput output;
        output.relativePath =
            outputObject.value(QStringLiteral("path")).toString();
        output.storedName =
            outputObject.value(QStringLiteral("stored")).toString();
        output.publicationStagePath = outputObject.value(
            QStringLiteral("publication_stage_path")).toString();
        const QString expectedStored =
            QStringLiteral("output-%1.new")
                .arg(index, 4, 10, QLatin1Char('0'));
        const QString sourceParent =
            QFileInfo(manifest.source.relativePath).path();
        const QFileInfo outputPath(output.relativePath);
        const QString expectedPublicationStage =
            QDir::fromNativeSeparators(
                QDir(outputPath.path()).filePath(
                    QStringLiteral(
                        ".gc-split-%1-output-%2.stage")
                        .arg(manifest.id)
                        .arg(index, 4, 10, QLatin1Char('0'))));
        if (!validSplitRelativePath(output.relativePath)
            || output.storedName != expectedStored
            || !validSplitRelativePath(output.publicationStagePath)
            || output.publicationStagePath
                != expectedPublicationStage
            || relativePathKey(
                QFileInfo(output.publicationStagePath).path())
                != relativePathKey(outputPath.path())
            || relativePathKey(outputPath.path())
                != relativePathKey(sourceParent)
            || !validSplitFileName(outputPath.fileName())
            || !parseSizeAndDigest(
                outputObject, true, output.contents, error)
            || !identityFromJson(
                outputObject.value(
                    QStringLiteral("stored_identity")),
                true,
                output.storedIdentity,
                error)
            || !generationFromJson(
                outputObject.value(
                    QStringLiteral("stored_generation")),
                true,
                output.storedGeneration,
                error)
            || !identityFromJson(
                outputObject.value(
                    QStringLiteral("publication_stage_identity")),
                true,
                output.publicationStageIdentity,
                error)
            || !generationFromJson(
                outputObject.value(
                    QStringLiteral("publication_stage_generation")),
                true,
                output.publicationStageGeneration,
                error)
            || !addIdentity(output.storedIdentity)
            || !addIdentity(output.publicationStageIdentity)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A split transaction output is malformed");
            }
            return false;
        }
        if (output.contents.size > MaximumSplitPayloadSize
            || aggregatePayload
                > MaximumSplitPayloadAggregate - output.contents.size) {
            error = QStringLiteral(
                "The split manifest exceeds its content-read budget");
            return false;
        }
        aggregatePayload += output.contents.size;
        productionPaths.append(output.relativePath);
        productionPaths.append(output.publicationStagePath);
        manifest.outputs.append(output);
    }

    if (!manifest.keepOriginal) {
        const QString backupParent = relativePathKey(
            QFileInfo(manifest.backup.relativePath).path());
        const QString expectedArchiveStage =
            QDir::fromNativeSeparators(
                QDir(QFileInfo(
                    manifest.backup.relativePath).path()).filePath(
                        QStringLiteral(
                            ".gc-split-%1-source.stage")
                            .arg(manifest.id)));
        const QString expectedRetiredBackup =
            QDir::fromNativeSeparators(
                QDir(QFileInfo(
                    manifest.backup.relativePath).path()).filePath(
                        QStringLiteral(
                            ".gc-split-%1-backup.old")
                            .arg(manifest.id)));
        const QString expectedRetiredSource =
            QDir::fromNativeSeparators(
                QDir(QFileInfo(
                    manifest.source.relativePath).path()).filePath(
                        QStringLiteral(
                            ".gc-split-%1-source.committed")
                            .arg(manifest.id)));
        if (manifest.source.archiveStagePath != expectedArchiveStage
            || manifest.source.retiredPath != expectedRetiredSource
            || manifest.backup.retiredPath != expectedRetiredBackup
            || relativePathKey(QFileInfo(
                manifest.source.archiveStagePath).path())
                    != backupParent
            || relativePathKey(QFileInfo(
                manifest.backup.retiredPath).path())
                    != backupParent) {
            error = QStringLiteral(
                "The split archive stages leave the backup directory");
            return false;
        }
        const QList<QString> archivePaths = {
            manifest.source.archiveStagePath,
            manifest.source.retiredPath,
            manifest.backup.retiredPath};
        for (const QString &candidate : archivePaths) {
            if (candidate.isEmpty()) continue;
            productionPaths.append(candidate);
        }
    }
    return splitPathsAreDisjoint(productionPaths, &budget, error);
}

bool pinnedFileIsCurrent(
    const ResolvedSplitPath &path,
    const AnchoredFileSystem::PinnedFile &file,
    QString &error)
{
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            path.entry, file, matches, error)
        || !matches
        || file.identity().linkCount() != 1
        || !path.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An anchored split file changed unexpectedly");
        }
        return false;
    }
    return true;
}

bool pinResolvedFile(
    const ResolvedSplitPath &path,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &file,
    bool &exists,
    QString &error,
    qint64 maximumSize = MaximumSplitPayloadSize,
    SplitReadBudget *budget = nullptr)
{
    file.reset();
    exists = false;
    if (budget && !budget->consumeOperation(error)) {
        return false;
    }
    if (!AnchoredFileSystem::entryExists(
            path.entry, exists, error)) {
        return false;
    }
    if (!exists) return true;

    const qint64 effectiveMaximum = budget
        ? budget->maximumFor(maximumSize)
        : std::min(maximumSize, MaximumSplitPayloadSize);
    const AnchoredFileSystem::PinnedFileReadControl readControl =
        budget ? budget->readControl()
               : AnchoredFileSystem::PinnedFileReadControl();
    file = std::make_unique<AnchoredFileSystem::PinnedFile>();
    if (!AnchoredFileSystem::pinRegularFile(
            path.entry, *file, error, effectiveMaximum, readControl)
        || !pinnedFileIsCurrent(path, *file, error)) {
        return false;
    }
    return true;
}

bool identityMatchesOneOf(
    const AnchoredFileSystem::NativeIdentity &observed,
    const AnchoredFileSystem::NativeIdentity &first,
    const AnchoredFileSystem::NativeIdentity &second =
        AnchoredFileSystem::NativeIdentity())
{
    return observed == first
        || (second.isValid() && observed == second);
}

bool pinExpectedResolvedFile(
    const ResolvedSplitPath &path,
    const AnchoredFileSystem::NativeIdentity &firstIdentity,
    const AnchoredFileSystem::NativeIdentity &secondIdentity,
    const AtomicFileSnapshot &expected,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &file,
    bool &exists,
    QString &error,
    SplitReadBudget *budget = nullptr,
    const QByteArray &requiredGeneration = QByteArray())
{
    if (!pinResolvedFile(
            path, file, exists, error, expected.size, budget)) {
        return false;
    }
    if (!exists) return true;
    if (!identityMatchesOneOf(
            file->identity(), firstIdentity, secondIdentity)
        || (!requiredGeneration.isEmpty()
            && (file->durableGeneration().isEmpty()
                || file->durableGeneration() != requiredGeneration))
        || file->size() != expected.size
        || file->sha256() != expected.digest) {
        error = !requiredGeneration.isEmpty()
                && file->durableGeneration() != requiredGeneration
            ? QStringLiteral(
                  "A split transaction pathname has a foreign file generation")
            : QStringLiteral(
                  "A split transaction pathname contains a foreign file");
        return false;
    }
    return true;
}

bool resolveSplitTransaction(
    const AnchoredFileSystem::DirectoryAnchor &rootDirectory,
    const QString &root,
    const SplitJournalManifest &manifest,
    ResolvedSplitTransaction &resolved,
    QString &error)
{
    resolved = {};
    if (!resolveAnchoredSplitPath(
            rootDirectory,
            root,
            manifest.source.relativePath,
            resolved.source,
            error)) {
        return false;
    }
    if (!manifest.keepOriginal
        && !resolveAnchoredSplitPath(
            rootDirectory,
            root,
            manifest.backup.relativePath,
            resolved.backup,
            error)) {
        return false;
    }
    if (!manifest.keepOriginal
        && (!resolveAnchoredSplitSibling(
                root,
                manifest.source.archiveStagePath,
                resolved.backup,
                resolved.sourceArchiveStage,
                error)
            || !resolveAnchoredSplitSibling(
                    root,
                    manifest.source.retiredPath,
                    resolved.source,
                    resolved.retiredSource,
                    error)
            || !resolveAnchoredSplitSibling(
                    root,
                    manifest.backup.retiredPath,
                    resolved.backup,
                    resolved.retiredBackup,
                    error))) {
        return false;
    }
    for (const SplitJournalOutput &output : manifest.outputs) {
        ResolvedSplitPath path;
        ResolvedSplitPath stage;
        if (!resolveAnchoredSplitSibling(
                root,
                output.relativePath,
                resolved.source,
                path,
                error)
            || !resolveAnchoredSplitSibling(
                root,
                output.publicationStagePath,
                resolved.source,
                stage,
                error)) {
            return false;
        }
        resolved.outputs.append(path);
        resolved.outputStages.append(stage);
    }
    return splitParentsAreCurrent(resolved, error);
}

QStringList splitProductionLockPaths(
    const ResolvedSplitTransaction &resolved)
{
    QStringList paths;
    paths.append(resolved.source.absolutePath);
    if (resolved.backup.entry.isValid()) {
        paths.append(resolved.backup.absolutePath);
    }
    for (const ResolvedSplitPath &output : resolved.outputs) {
        paths.append(output.absolutePath);
    }
    for (const ResolvedSplitPath &stage : resolved.outputStages) {
        paths.append(stage.absolutePath);
    }
    if (resolved.sourceArchiveStage.entry.isValid()) {
        paths.append(resolved.sourceArchiveStage.absolutePath);
    }
    if (resolved.retiredSource.entry.isValid()) {
        paths.append(resolved.retiredSource.absolutePath);
    }
    if (resolved.retiredBackup.entry.isValid()) {
        paths.append(resolved.retiredBackup.absolutePath);
    }
    return paths;
}

bool pinJournalFile(
    const SplitJournal &journal,
    const QString &name,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &file,
    bool &exists,
    QString &error,
    qint64 maximumSize = -1,
    const AnchoredFileSystem::NativeIdentity *expectedIdentity = nullptr,
    SplitReadBudget *budget = nullptr)
{
    file.reset();
    exists = false;
    if (budget && !budget->consumeOperation(error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef entry =
        journal.directory.entry(name, error);
    if (!entry.isValid()
        || !AnchoredFileSystem::entryExists(
            entry, exists, error)) {
        return false;
    }
    if (!exists) return true;

    file = std::make_unique<AnchoredFileSystem::PinnedFile>();
    bool matches = false;
    const qint64 effectiveMaximum = budget
        ? budget->maximumFor(maximumSize)
        : maximumSize;
    const AnchoredFileSystem::PinnedFileReadControl readControl =
        budget ? budget->readControl()
               : AnchoredFileSystem::PinnedFileReadControl();
    if (!AnchoredFileSystem::pinRegularFile(
            entry, *file, error, effectiveMaximum, readControl)) {
        const qint64 requestedMaximum = maximumSize < 0
            ? MaximumSplitPayloadSize
            : std::min(maximumSize, MaximumSplitPayloadSize);
        if (budget && effectiveMaximum < requestedMaximum) {
            budget->consumeBytes(requestedMaximum, error);
        }
        return false;
    }
    if (!AnchoredFileSystem::entryMatches(
            entry, *file, matches, error)
        || !matches
        || file->identity().linkCount() != 1
        || (expectedIdentity
            && file->identity() != *expectedIdentity)
        || !journal.directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A split journal file changed while being pinned");
        }
        return false;
    }
    return true;
}

bool writeNewJournalFile(
    const SplitJournal &journal,
    const QString &name,
    const QByteArray &contents,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &file,
    QString &error)
{
    const QString temporaryName = name + QStringLiteral(".tmp");
    const AnchoredFileSystem::EntryRef entry =
        journal.directory.entry(name, error);
    const AnchoredFileSystem::EntryRef temporary =
        journal.directory.entry(temporaryName, error);
    if (!entry.isValid() || !temporary.isValid()) return false;

    bool exists = false;
    if (!AnchoredFileSystem::entryExists(
            entry, exists, error)) {
        return false;
    }
    if (exists) {
        error = QStringLiteral(
            "A split journal file appeared concurrently");
        return false;
    }
    bool temporaryExists = false;
    if (!AnchoredFileSystem::entryExists(
            temporary, temporaryExists, error)) {
        return false;
    }
    if (temporaryExists) {
        error = QStringLiteral(
            "An interrupted split journal publication requires recovery: %1")
                .arg(temporary.displayPath());
        return false;
    }

    auto staged = std::make_unique<AnchoredFileSystem::PinnedFile>();
    if (!AnchoredFileSystem::writeNewFile(
            contents, temporary, *staged, error)
        || staged->identity().linkCount() != 1
        || staged->size() != contents.size()
        || staged->sha256()
            != QCryptographicHash::hash(
                contents, QCryptographicHash::Sha256)
        || !journal.directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot publish an anchored split journal file");
        }
        return false;
    }
    reportSplitTransition("split-journal-temp-synchronized");

    const AnchoredFileSystem::MutationResult publication =
        AnchoredFileSystem::moveNoReplace(*staged, entry);
    if (!mutationAppliedDurably(
            publication, {journal.directory}, error)
        || !journal.directory.pathMatches(error)) {
        return false;
    }
    file = std::move(staged);
    reportSplitTransition("split-journal-file-published");
    return true;
}

bool createSplitJournal(
    const SplitNamespace &nameSpace,
    const QString &id,
    SplitJournal &journal,
    QString &error)
{
    journal = {};
    if (!validTransactionId(id)) {
        error = QStringLiteral(
            "Cannot reserve an invalid split transaction identifier");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    const AnchoredFileSystem::MutationResult creation =
        AnchoredFileSystem::createPrivateFixedChildDirectory(
            nameSpace.namespaceDirectory, id, directory);
    if (!mutationAppliedDurably(
            creation,
            {nameSpace.namespaceDirectory},
            error)
        || !directory.pathMatches(error)) {
        return false;
    }
    journal.id = id;
    journal.path = QDir(nameSpace.namespacePath).filePath(id);
    journal.directory = directory;
    reportSplitTransition("split-transaction-directory-created");
    return true;
}

struct ObservedSplitJournalFile
{
    QString name;
    AnchoredFileSystem::NativeIdentity identity;
};

bool splitRemovalQuarantineName(const QString &name)
{
    static const QRegularExpression expression(
        QStringLiteral(
            "^\\.gc-remove-(?:[0-9a-f]{64}|"
            "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
            "[0-9a-f]{4}-[0-9a-f]{12})$"));
    return expression.match(name).hasMatch();
}

QString splitJournalTemporaryBaseName(const QString &name)
{
    return name.endsWith(QStringLiteral(".tmp"))
        ? name.left(name.size() - 4)
        : QString();
}

bool knownSplitJournalFileName(const QString &name)
{
    const QString base = splitJournalTemporaryBaseName(name);
    const QString candidate = base.isEmpty() ? name : base;
    static const QRegularExpression cleanupName(
        QStringLiteral("^cleanup-[0-9]{4}\\.json$"));
    if (!base.isEmpty()) {
        return candidate == SplitManifestName
            || candidate == SplitIntentName
            || candidate == SplitCommittingMarkerName
            || candidate == SplitCommitMarkerName
            || cleanupName.match(candidate).hasMatch();
    }
    if (candidate == SplitManifestName
        || candidate == SplitIntentName
        || candidate == SplitCommittingMarkerName
        || candidate == SplitCommitMarkerName
        || name == QStringLiteral("source.old")
        || name == QStringLiteral("backup.old")) {
        return true;
    }
    static const QRegularExpression dataName(
        QStringLiteral("^output-[0-9]{4}\\.new$"));
    return dataName.match(candidate).hasMatch()
        || cleanupName.match(candidate).hasMatch()
        || splitRemovalQuarantineName(name);
}

bool pinExpectedRemovalQuarantine(
    const AnchoredFileSystem::DirectoryAnchor &parent,
    const QString &originalComponent,
    const AnchoredFileSystem::NativeIdentity &identity,
    const AtomicFileSnapshot &contents,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &file,
    bool &exists,
    QString &error,
    SplitReadBudget *budget = nullptr,
    const QByteArray &requiredGeneration = QByteArray())
{
    file.reset();
    exists = false;
    if ((budget && !budget->consumeOperation(error))
        || !parent.pathMatches(error)) {
        return false;
    }
    const QString name =
        AnchoredFileSystem::removalQuarantineName(
            identity, originalComponent);
    if (name.isEmpty()) {
        error = QStringLiteral(
            "Cannot derive the split removal quarantine name");
        return false;
    }
    const AnchoredFileSystem::EntryRef quarantine =
        parent.entry(name, error);
    if (!quarantine.isValid()
        || !AnchoredFileSystem::entryExists(
            quarantine, exists, error)) {
        return false;
    }
    if (!exists) return true;
    auto candidate =
        std::make_unique<AnchoredFileSystem::PinnedFile>();
    bool matches = false;
    const AnchoredFileSystem::PinnedFileReadControl readControl =
        budget ? budget->readControl()
               : AnchoredFileSystem::PinnedFileReadControl();
    if (!AnchoredFileSystem::pinRegularFile(
            quarantine, *candidate, error,
            budget ? budget->maximumFor(contents.size)
                   : std::min(contents.size, MaximumSplitPayloadSize),
            readControl)
        || candidate->identity() != identity
        || (!requiredGeneration.isEmpty()
            && candidate->durableGeneration()
                != requiredGeneration)
        || candidate->identity().linkCount() != 1
        || candidate->size() != contents.size
        || candidate->sha256() != contents.digest
        || !AnchoredFileSystem::entryMatches(
            quarantine, *candidate, matches, error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A split removal quarantine changed while being pinned");
        }
        return false;
    }
    file = std::move(candidate);
    exists = true;
    return parent.pathMatches(error);
}

bool inspectSplitJournal(
    const SplitJournal &journal,
    std::vector<ObservedSplitJournalFile> &files,
    SplitReadBudget &budget,
    QString &error)
{
    files.clear();
    if (!journal.directory.pathMatches(error)) return false;

    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!journal.directory.enumerateEntries(
            entries, MaximumSplitJournalEntries, error)) {
        return false;
    }
    files.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::RegularFile
            || entry.identity.linkCount() != 1
            || !knownSplitJournalFileName(entry.name)) {
            error = QStringLiteral(
                "The split transaction journal contains an unsafe entry");
            return false;
        }

        files.push_back({entry.name, entry.identity});
    }

    QList<AnchoredFileSystem::DirectoryEntry> verified;
    if (!journal.directory.enumerateEntries(
            verified, MaximumSplitJournalEntries, error)
        || !directorySnapshotsMatch(entries, verified)
        || !journal.directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The split journal changed during inspection");
        }
        return false;
    }
    return true;
}

bool cleanupInterruptedJournalPublications(
    SplitJournal &journal,
    SplitReadBudget &budget,
    QString &error)
{
    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!journal.directory.enumerateEntries(
            entries, MaximumSplitJournalEntries, error)) {
        return false;
    }

    for (const auto &entry : entries) {
        const QString base = splitJournalTemporaryBaseName(entry.name);
        if (base.isEmpty()) continue;
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::RegularFile
            || entry.identity.linkCount() != 1
            || !knownSplitJournalFileName(entry.name)) {
            error = QStringLiteral(
                "An interrupted split journal publication is unsafe: %1")
                    .arg(entry.name);
            return false;
        }

        const qint64 requestedMaximum =
            (base == SplitManifestName || base == SplitIntentName)
            ? MaximumSplitManifestSize
            : ((base == SplitCommittingMarkerName
                    || base == SplitCommitMarkerName)
                ? 128 : MaximumSplitCleanupRecordSize);
        std::unique_ptr<AnchoredFileSystem::PinnedFile> temporary;
        bool exists = false;
        if (!pinJournalFile(
                journal, entry.name, temporary, exists, error,
                requestedMaximum, &entry.identity, &budget)
            || !exists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot validate interrupted journal temporary file %1")
                        .arg(entry.name);
            }
            return false;
        }
        if (!removePinnedFile(
                *temporary, journal.directory, budget, error)) {
            return false;
        }
        reportSplitTransition(
            "split-recovery-journal-temp-retired");
    }
    return journal.directory.pathMatches(error);
}

bool cleanupSplitJournal(
    const SplitNamespace &nameSpace,
    SplitJournal &journal,
    SplitReadBudget &budget,
    QString &error,
    const SplitJournalManifest *manifest = nullptr)
{
    std::vector<ObservedSplitJournalFile> files;
    if (!inspectSplitJournal(journal, files, budget, error)) return false;

    std::stable_sort(
        files.begin(),
        files.end(),
        [](const ObservedSplitJournalFile &left,
           const ObservedSplitJournalFile &right) {
            const int leftOrder =
                left.name == SplitManifestName ? 1 : 0;
            const int rightOrder =
                right.name == SplitManifestName ? 1 : 0;
            return leftOrder < rightOrder;
        });

    struct ExpectedPayload {
        AnchoredFileSystem::NativeIdentity identity;
        QByteArray generation;
        AtomicFileSnapshot contents;
    };
    QHash<QString, ExpectedPayload> expectedPayloads;
    if (manifest) {
        const auto addExpectedPayload =
            [&expectedPayloads, &error](
                    const QString &name,
                    const ExpectedPayload &payload) {
                const QString quarantine =
                    AnchoredFileSystem::removalQuarantineName(
                        payload.identity, name);
                if (name.isEmpty() || quarantine.isEmpty()
                    || expectedPayloads.contains(name)
                    || expectedPayloads.contains(quarantine)) {
                    error = QStringLiteral(
                        "The split manifest contains colliding payload cleanup names");
                    return false;
                }
                expectedPayloads.insert(name, payload);
                expectedPayloads.insert(quarantine, payload);
                return true;
            };
        if (!addExpectedPayload(
                manifest->source.storedName,
                {manifest->source.storedIdentity,
                 manifest->source.storedGeneration,
                 manifest->source.contents})) {
            return false;
        }
        if (manifest->backup.exists) {
            if (!addExpectedPayload(
                    manifest->backup.storedName,
                    {manifest->backup.storedIdentity,
                     manifest->backup.storedGeneration,
                     manifest->backup.contents})) {
                return false;
            }
        }
        for (const SplitJournalOutput &output : manifest->outputs) {
            if (!addExpectedPayload(
                    output.storedName,
                    {output.storedIdentity,
                     output.storedGeneration,
                     output.contents})) {
                return false;
            }
        }
    }
    static const QRegularExpression payloadName(
        QStringLiteral("^output-[0-9]{4}\\.new$"));

    for (ObservedSplitJournalFile &observed : files) {
        const auto expected = expectedPayloads.constFind(observed.name);
        const bool isPayload = expected != expectedPayloads.constEnd();
        const bool payloadLike = observed.name == QStringLiteral("source.old")
            || observed.name == QStringLiteral("backup.old")
            || payloadName.match(observed.name).hasMatch();
        if (manifest && payloadLike && !isPayload) {
            error = QStringLiteral(
                "An unrecognized split journal payload was preserved: %1")
                    .arg(observed.name);
            return false;
        }
        std::unique_ptr<AnchoredFileSystem::PinnedFile> current;
        bool exists = false;
        const qint64 maximumSize = isPayload
            ? expected->contents.size
            : (observed.name == SplitManifestName
                    || observed.name == SplitIntentName
                ? MaximumSplitManifestSize
                : (observed.name == SplitCommittingMarkerName
                        || observed.name == SplitCommitMarkerName
                    ? 128 : MaximumSplitCleanupRecordSize));
        const bool pinned = pinJournalFile(
                journal,
                observed.name,
                current,
                exists,
                error,
                maximumSize,
                &observed.identity,
                &budget);
        if (!pinned
            || !exists
            || (isPayload
                && (current->identity() != expected->identity
                    || current->durableGeneration() != expected->generation
                    || current->size() != expected->contents.size
                    || current->sha256() != expected->contents.digest))) {
            if (isPayload && !error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot validate split journal payload %1: %2")
                        .arg(observed.name, error);
            } else if (error.isEmpty()) {
                error = isPayload
                    ? QStringLiteral(
                        "A split journal payload does not match its manifest identity, generation, size, and digest")
                    : QStringLiteral(
                        "A split journal file changed before cleanup");
            }
            return false;
        }
        if (!removePinnedFile(
                *current, journal.directory, budget, error)) {
            return false;
        }
        if (observed.name == SplitManifestName) {
            reportSplitTransition(
                "split-recovery-manifest-removed");
        } else {
            reportSplitTransition(
                "split-recovery-journal-file-removed");
        }
    }
    files.clear();

    if (!journal.directory.pathMatches(error)
        || !nameSpace.namespaceDirectory.pathMatches(error)) {
        return false;
    }
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::removeEmptyDirectory(journal.directory);
    if (!mutationAppliedDurably(
            removal,
            {nameSpace.namespaceDirectory},
            error)) {
        return false;
    }
    reportSplitTransition(
        "split-recovery-directory-removed");
    return true;
}

void appendSplitCleanupError(
    const SplitNamespace &nameSpace,
    SplitJournal &journal,
    QString &error)
{
    QString cleanupError;
    SplitReadBudget cleanupBudget;
    if (!cleanupSplitJournal(
            nameSpace, journal, cleanupBudget, cleanupError)) {
        appendAtomicFileError(error, cleanupError);
        appendAtomicFileError(
            error,
            QStringLiteral("split recovery journal retained at %1")
                .arg(journal.path));
    }
}

bool readSplitManifest(
    const SplitJournal &journal,
    SplitJournalManifest &manifest,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &manifestFile,
    bool &exists,
    SplitReadBudget &budget,
    QString &error)
{
    if (!pinJournalFile(
            journal,
            SplitManifestName,
            manifestFile,
            exists,
            error,
            MaximumSplitManifestSize,
            nullptr,
            &budget)) {
        return false;
    }
    if (!exists) return true;
    if (manifestFile->size() <= 0) {
        error = QStringLiteral(
            "The split transaction manifest is empty");
        return false;
    }

    QByteArray contents;
    const auto readControl = budget.readControl();
    if (!budget.consumeOperation(error)
        || !AnchoredFileSystem::readAll(
            *manifestFile,
            MaximumSplitManifestSize,
            contents,
            error,
            readControl)
        || !parseSplitManifest(
            contents, journal.id, manifest, budget, error)) {
        return false;
    }
    return true;
}

bool readSplitIntent(
    const SplitJournal &journal,
    SplitJournalIntent &intent,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &intentFile,
    bool &exists,
    SplitReadBudget &budget,
    QString &error)
{
    if (!pinJournalFile(
            journal,
            SplitIntentName,
            intentFile,
            exists,
            error,
            MaximumSplitManifestSize,
            nullptr,
            &budget)) {
        return false;
    }
    if (!exists) return true;
    if (intentFile->size() <= 0) {
        error = QStringLiteral(
            "The split transaction intent is empty");
        return false;
    }
    QByteArray contents;
    const auto readControl = budget.readControl();
    return budget.consumeOperation(error)
        && AnchoredFileSystem::readAll(
            *intentFile,
            MaximumSplitManifestSize,
            contents,
            error,
            readControl)
        && parseSplitIntent(
            contents, journal.id, intent, error);
}

bool readSplitCleanupRecord(
    const SplitJournal &journal,
    qsizetype index,
    const QString &expectedPath,
    SplitCleanupRecord &record,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &recordFile,
    bool &exists,
    SplitReadBudget &budget,
    QString &error)
{
    if (!pinJournalFile(
            journal,
            splitCleanupRecordName(index),
            recordFile,
            exists,
            error,
            MaximumSplitCleanupRecordSize,
            nullptr,
            &budget)) {
        return false;
    }
    if (!exists) return true;
    QByteArray contents;
    const auto readControl = budget.readControl();
    return budget.consumeOperation(error)
        && AnchoredFileSystem::readAll(
            *recordFile, MaximumSplitCleanupRecordSize, contents, error,
            readControl)
        && parseSplitCleanupRecord(
            contents,
            journal.id,
            index,
            expectedPath,
            record,
            error);
}

bool publishSplitCleanupRecord(
    const SplitJournal &journal,
    qsizetype index,
    const QString &relativePath,
    const AnchoredFileSystem::PinnedFile &artifact,
    AnchoredFileSystem::NativeIdentity &recordIdentity,
    QString &error)
{
    SplitCleanupRecord record;
    record.relativePath = relativePath;
    record.identity = artifact.identity();
    record.generation = artifact.durableGeneration();
    record.contents = {artifact.size(), artifact.sha256()};
    if (record.generation.isEmpty()) {
        error = QStringLiteral(
            "The split artifact filesystem cannot provide durable generation evidence");
        return false;
    }
    const QByteArray contents = serializeSplitCleanupRecord(
        journal.id, index, record);
    std::unique_ptr<AnchoredFileSystem::PinnedFile> recordFile;
    if (contents.size() > MaximumSplitCleanupRecordSize
        || !writeNewJournalFile(
            journal, splitCleanupRecordName(index),
            contents, recordFile, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot durably record split artifact ownership");
        }
        return false;
    }
    recordIdentity = recordFile->identity();
    return true;
}

bool readSplitPhaseMarker(
    const SplitJournal &journal,
    const QString &name,
    bool &committed,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &marker,
    SplitReadBudget &budget,
    QString &error)
{
    bool exists = false;
    committed = false;
    if (!pinJournalFile(
            journal,
            name,
            marker,
            exists,
            error,
            128,
            nullptr,
            &budget)) {
        return false;
    }
    if (!exists) return true;

    QByteArray contents;
    const auto readControl = budget.readControl();
    if (!budget.consumeOperation(error)
        || !AnchoredFileSystem::readAll(
            *marker, 128, contents, error, readControl)
        || contents != journal.id.toLatin1() + '\n') {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The split transaction phase marker is invalid");
        }
        return false;
    }
    committed = true;
    return true;
}

bool readSplitCommitMarker(
    const SplitJournal &journal,
    bool &committed,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &marker,
    SplitReadBudget &budget,
    QString &error)
{
    return readSplitPhaseMarker(
        journal, SplitCommitMarkerName,
        committed, marker, budget, error);
}

enum class SplitBackupGeneration
{
    Absent,
    Source,
    Prior
};

bool rollbackSplitTransaction(
    const SplitNamespace &nameSpace,
    SplitJournal &journal,
    const SplitJournalManifest &manifest,
    const ResolvedSplitTransaction &resolved,
    SplitReadBudget &budget,
    QString &error)
{
    if (!splitParentsAreCurrent(resolved, error)
        || !journal.directory.pathMatches(error)) {
        return false;
    }

    struct OutputState {
        bool finalExists = false;
        bool stageExists = false;
        bool quarantineExists = false;
    };
    std::vector<OutputState> outputStates(
        static_cast<std::size_t>(manifest.outputs.size()));
    for (qsizetype index = 0; index < manifest.outputs.size(); ++index) {
        const SplitJournalOutput &output = manifest.outputs.at(index);
        OutputState &state =
            outputStates[static_cast<std::size_t>(index)];
        std::unique_ptr<AnchoredFileSystem::PinnedFile> finalFile;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> stageFile;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> quarantineFile;
        if (!pinExpectedResolvedFile(
                resolved.outputs.at(index),
                output.publicationStageIdentity,
                {}, output.contents,
                finalFile, state.finalExists, error, &budget,
                output.publicationStageGeneration)
            || !pinExpectedResolvedFile(
                resolved.outputStages.at(index),
                output.publicationStageIdentity,
                {}, output.contents,
                stageFile, state.stageExists, error, &budget,
                output.publicationStageGeneration)) {
            return false;
        }
        if (!state.finalExists && !state.stageExists
            && !pinExpectedRemovalQuarantine(
                resolved.outputStages.at(index).parent,
                QFileInfo(output.publicationStagePath).fileName(),
                output.publicationStageIdentity,
                output.contents,
                quarantineFile,
                state.quarantineExists,
                error,
                &budget,
                output.publicationStageGeneration)) {
            return false;
        }
        const int locations = int(state.finalExists)
            + int(state.stageExists)
            + int(state.quarantineExists);
        if (locations > 1) {
            error = QStringLiteral(
                "A split output identity exists at multiple names");
            return false;
        }
        if (state.finalExists) {
            error = QStringLiteral(
                "A production output without a COMMITTING marker was preserved at %1")
                    .arg(resolved.outputs.at(index).absolutePath);
            return false;
        }
    }

    std::unique_ptr<AnchoredFileSystem::PinnedFile> source;
    bool sourceExists = false;
    if (!pinExpectedResolvedFile(
            resolved.source,
                manifest.source.originalIdentity,
                {}, manifest.source.contents,
                source, sourceExists, error, &budget,
                manifest.source.originalGeneration)) {
        return false;
    }
    std::unique_ptr<AnchoredFileSystem::PinnedFile> retiredSource;
    bool retiredSourceExists = false;
    if (!manifest.keepOriginal
        && !pinExpectedResolvedFile(
            resolved.retiredSource,
            manifest.source.originalIdentity,
            {}, manifest.source.contents,
            retiredSource, retiredSourceExists, error, &budget,
            manifest.source.originalGeneration)) {
        return false;
    }
    const int sourceLocations = int(sourceExists)
        + int(retiredSourceExists);
    if (sourceLocations != 1) {
        error = sourceLocations > 1
            ? QStringLiteral(
                  "The split source identity exists at multiple names")
            : QStringLiteral(
                  "The original split source is unavailable for rollback");
        return false;
    }

    std::unique_ptr<AnchoredFileSystem::PinnedFile> backup;
    bool backupExists = false;
    SplitBackupGeneration backupGeneration =
        SplitBackupGeneration::Absent;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> archiveStage;
    bool archiveStageExists = false;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> archiveQuarantine;
    bool archiveQuarantineExists = false;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> retiredBackup;
    bool retiredBackupExists = false;
    if (!manifest.keepOriginal) {
        if (!pinResolvedFile(
                resolved.backup, backup, backupExists, error,
                manifest.backup.exists
                    ? std::max(
                        manifest.source.contents.size,
                        manifest.backup.contents.size)
                    : manifest.source.contents.size,
                &budget)) {
            return false;
        }
        if (backupExists) {
            if (backup->identity()
                    == manifest.source.archiveStageIdentity
                && backup->durableGeneration()
                    == manifest.source.archiveStageGeneration
                && backup->size() == manifest.source.contents.size
                && backup->sha256()
                    == manifest.source.contents.digest) {
                backupGeneration = SplitBackupGeneration::Source;
            } else if (manifest.backup.exists
                && backup->identity()
                    == manifest.backup.originalIdentity
                && backup->durableGeneration()
                    == manifest.backup.originalGeneration
                && backup->size() == manifest.backup.contents.size
                && backup->sha256()
                    == manifest.backup.contents.digest) {
                backupGeneration = SplitBackupGeneration::Prior;
            } else {
                error = QStringLiteral(
                    "The split backup pathname contains a foreign file");
                return false;
            }
        }
        if (!pinExpectedResolvedFile(
                resolved.sourceArchiveStage,
                manifest.source.archiveStageIdentity,
                {}, manifest.source.contents,
                archiveStage, archiveStageExists, error, &budget,
                manifest.source.archiveStageGeneration)) {
            return false;
        }
        if (backupGeneration != SplitBackupGeneration::Source
            && !archiveStageExists
            && !pinExpectedRemovalQuarantine(
                resolved.sourceArchiveStage.parent,
                QFileInfo(
                    manifest.source.archiveStagePath).fileName(),
                manifest.source.archiveStageIdentity,
                manifest.source.contents,
                archiveQuarantine,
                archiveQuarantineExists,
                error,
                &budget,
                manifest.source.archiveStageGeneration)) {
            return false;
        }
        const int archiveLocations =
            int(backupGeneration == SplitBackupGeneration::Source)
            + int(archiveStageExists)
            + int(archiveQuarantineExists);
        if (archiveLocations > 1) {
            error = QStringLiteral(
                "The split archive identity exists at multiple names");
            return false;
        }
        if (manifest.backup.exists
            && !pinExpectedResolvedFile(
                resolved.retiredBackup,
                manifest.backup.originalIdentity,
                {}, manifest.backup.contents,
                retiredBackup, retiredBackupExists, error, &budget,
                manifest.backup.originalGeneration)) {
            return false;
        }
        if (manifest.backup.exists
            && backupGeneration != SplitBackupGeneration::Prior
            && !retiredBackupExists) {
            error = QStringLiteral(
                "The previous split backup is unavailable for rollback");
            return false;
        }
    }

    if (!manifest.keepOriginal && retiredSourceExists) {
        if (!movePinnedFile(
                *retiredSource,
                resolved.retiredSource,
                resolved.source.entry,
                resolved.source.parent,
                budget,
                error)) {
            return false;
        }
        source = std::move(retiredSource);
        retiredSourceExists = false;
        sourceExists = true;
        reportSplitTransition(
            "split-recovery-source-restored");
    }

    if (!manifest.keepOriginal
        && backupGeneration == SplitBackupGeneration::Source) {
        if (!movePinnedFile(
                *backup,
                resolved.backup,
                resolved.sourceArchiveStage.entry,
                resolved.sourceArchiveStage.parent,
                budget,
                error)) {
            return false;
        }
        archiveStage = std::move(backup);
        archiveStageExists = true;
        backupExists = false;
        backupGeneration = SplitBackupGeneration::Absent;
        reportSplitTransition(
            "split-recovery-archive-unpublished");
    }
    if (!manifest.keepOriginal && manifest.backup.exists
        && backupGeneration != SplitBackupGeneration::Prior) {
        if (!movePinnedFile(
                *retiredBackup,
                resolved.retiredBackup,
                resolved.backup.entry,
                resolved.backup.parent,
                budget,
                error)) {
            return false;
        }
        backup = std::move(retiredBackup);
        retiredBackupExists = false;
        backupExists = true;
        backupGeneration = SplitBackupGeneration::Prior;
        reportSplitTransition(
            "split-recovery-backup-restored");
    }

    for (qsizetype index = 0; index < manifest.outputs.size(); ++index) {
        OutputState &state =
            outputStates[static_cast<std::size_t>(index)];
        bool retired = false;
        if (state.stageExists) {
            std::unique_ptr<AnchoredFileSystem::PinnedFile> stage;
            bool exists = false;
            const SplitJournalOutput &expected =
                manifest.outputs.at(index);
            if (!pinExpectedResolvedFile(
                    resolved.outputStages.at(index),
                    expected.publicationStageIdentity,
                    {}, expected.contents,
                    stage, exists, error, &budget,
                    expected.publicationStageGeneration)
                || !exists
                || !removePinnedFile(
                    *stage,
                    resolved.outputStages.at(index).parent,
                    budget,
                    error)) {
                return false;
            }
        }
        retired = state.stageExists;
        if (state.quarantineExists) {
            std::unique_ptr<AnchoredFileSystem::PinnedFile> quarantine;
            bool exists = false;
            const SplitJournalOutput &expected =
                manifest.outputs.at(index);
            if (!pinExpectedRemovalQuarantine(
                    resolved.outputStages.at(index).parent,
                    QFileInfo(expected.publicationStagePath).fileName(),
                    expected.publicationStageIdentity,
                    expected.contents,
                    quarantine,
                    exists,
                    error,
                    &budget,
                    expected.publicationStageGeneration)
                || !exists
                || !removePinnedFile(
                    *quarantine,
                    resolved.outputStages.at(index).parent,
                    budget,
                    error)) {
                return false;
            }
        }
        retired = retired || state.quarantineExists;
        if (retired) {
            reportSplitTransition(
                "split-recovery-output-stage-retired");
        }
    }
    if (!manifest.keepOriginal) {
        if (archiveStageExists
            && !removePinnedFile(
                *archiveStage,
                resolved.sourceArchiveStage.parent,
                budget,
                error)) {
            return false;
        }
        if (archiveQuarantineExists
            && !removePinnedFile(
                *archiveQuarantine,
                resolved.sourceArchiveStage.parent,
                budget,
                error)) {
            return false;
        }
        if (archiveStageExists || archiveQuarantineExists) {
            reportSplitTransition(
                "split-recovery-archive-stage-retired");
        }
    }

    for (const ResolvedSplitPath &target : resolved.outputs) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                target.entry, exists, error)
            || exists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A split output remains after rollback");
            }
            return false;
        }
    }
    if (!pinnedFileIsCurrent(resolved.source, *source, error)) {
        return false;
    }
    if (!manifest.keepOriginal) {
        std::unique_ptr<AnchoredFileSystem::PinnedFile> verifiedBackup;
        bool verifiedBackupExists = false;
        if (manifest.backup.exists) {
            if (!pinExpectedResolvedFile(
                    resolved.backup,
                    manifest.backup.originalIdentity,
                    {}, manifest.backup.contents,
                    verifiedBackup, verifiedBackupExists, error, &budget,
                    manifest.backup.originalGeneration)
                || !verifiedBackupExists) {
                return false;
            }
        } else if (!pinResolvedFile(
                       resolved.backup,
                       verifiedBackup,
                       verifiedBackupExists,
                       error,
                       MaximumSplitPayloadSize,
                       &budget)
                   || verifiedBackupExists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "An unexpected split backup remains after rollback");
            }
            return false;
        }
    }

    source.reset();
    retiredSource.reset();
    backup.reset();
    archiveStage.reset();
    archiveQuarantine.reset();
    retiredBackup.reset();
    return cleanupSplitJournal(
        nameSpace, journal, budget, error, &manifest);
}

bool verifyCommittedSplitTransaction(
    const SplitJournalManifest &manifest,
    const ResolvedSplitTransaction &resolved,
    SplitReadBudget &budget,
    QString &error)
{
    if (!splitParentsAreCurrent(resolved, error)) return false;

    for (qsizetype index = 0;
         index < manifest.outputs.size();
         ++index) {
        std::unique_ptr<AnchoredFileSystem::PinnedFile> output;
        bool exists = false;
        const SplitJournalOutput &expected =
            manifest.outputs.at(index);
        if (!pinExpectedResolvedFile(
                resolved.outputs.at(index),
                expected.publicationStageIdentity,
                {},
                expected.contents,
                output,
                exists,
                error,
                &budget,
                expected.publicationStageGeneration)
            || !exists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A committed split output is missing");
            }
            return false;
        }
    }

    std::unique_ptr<AnchoredFileSystem::PinnedFile> source;
    bool sourceExists = false;
    if (!pinExpectedResolvedFile(
            resolved.source,
        manifest.source.originalIdentity,
        {},
        manifest.source.contents,
            source,
        sourceExists,
        error,
        &budget,
        manifest.source.originalGeneration)) {
        return false;
    }
    if (manifest.keepOriginal) {
        if (!sourceExists) {
            error = QStringLiteral(
                "A committed keep-original split lost its source");
            return false;
        }
        return true;
    }
    std::unique_ptr<AnchoredFileSystem::PinnedFile> retiredSource;
    bool retiredSourceExists = false;
    if (!pinExpectedResolvedFile(
            resolved.retiredSource,
        manifest.source.originalIdentity,
        {},
        manifest.source.contents,
            retiredSource,
        retiredSourceExists,
        error,
        &budget,
        manifest.source.originalGeneration)) {
        return false;
    }
    if (int(sourceExists) + int(retiredSourceExists) != 1) {
        error = QStringLiteral(
            "A split source is not in exactly one commit location");
        return false;
    }

    std::unique_ptr<AnchoredFileSystem::PinnedFile> backup;
    bool backupExists = false;
    if (!pinExpectedResolvedFile(
            resolved.backup,
        manifest.source.archiveStageIdentity,
        {},
        manifest.source.contents,
            backup,
        backupExists,
        error,
        &budget,
        manifest.source.archiveStageGeneration)
        || !backupExists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A committed split archive is missing");
        }
        return false;
    }
    if (manifest.backup.exists) {
        std::unique_ptr<AnchoredFileSystem::PinnedFile> retired;
        bool retiredExists = false;
        if (!pinExpectedResolvedFile(
                resolved.retiredBackup,
            manifest.backup.originalIdentity,
            {},
            manifest.backup.contents,
                retired,
            retiredExists,
            error,
            &budget,
            manifest.backup.originalGeneration)
            || !retiredExists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The previous split backup was not retired");
            }
            return false;
        }
    }
    return true;
}

bool retireCommittedIdentityIfCurrent(
    const ResolvedSplitPath &path,
    const AnchoredFileSystem::NativeIdentity &identity,
    const QByteArray &generation,
    const AtomicFileSnapshot &contents,
    bool &retired,
    SplitReadBudget &budget,
    QString &error)
{
    retired = false;
    bool exists = false;
    if (!AnchoredFileSystem::entryExists(path.entry, exists, error)
        || !path.parent.pathMatches(error)) {
        return false;
    }
    if (exists) {
        auto file =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        QString pinError;
        if (!budget.consumeOperation(error)) {
            return false;
        }
        bool readBudgetFailed = false;
        const AnchoredFileSystem::PinnedFileReadControl readControl =
            [&budget, &readBudgetFailed](qint64 amount, QString &readError) {
                const bool allowed = budget.consumeBytes(amount, readError);
                readBudgetFailed = readBudgetFailed || !allowed;
                return allowed;
            };
        const bool pinned = AnchoredFileSystem::pinRegularFile(
                path.entry, *file, pinError,
                contents.size, readControl);
        if (!pinned && readBudgetFailed) {
            error = pinError;
            return false;
        }
        if (pinned) {
            if (file->identity() == identity
                && !generation.isEmpty()
                && file->durableGeneration() == generation
                && file->size() == contents.size
                && file->sha256() == contents.digest) {
                retired = removePinnedFile(
                    *file, path.parent, budget, error);
                return retired;
            }
        }
        // A later generation or unsafe replacement is production data, not
        // transaction-owned cleanup material.
        if (!path.parent.pathMatches(error)) return false;
    }

    std::unique_ptr<AnchoredFileSystem::PinnedFile> quarantine;
    bool quarantineExists = false;
    if (!pinExpectedRemovalQuarantine(
            path.parent,
            QFileInfo(path.relativePath).fileName(),
            identity,
            contents,
            quarantine,
            quarantineExists,
            error,
            &budget,
            generation)) {
        return false;
    }
    if (!quarantineExists) return true;
    retired = removePinnedFile(
        *quarantine, path.parent, budget, error);
    return retired;
}

bool cleanupCommittedSplitProduction(
    const SplitJournalManifest &manifest,
    const ResolvedSplitTransaction &resolved,
    SplitReadBudget &budget,
    QString &error)
{
    if (manifest.keepOriginal) return true;

    bool sourceRetired = false;
    if (!retireCommittedIdentityIfCurrent(
            resolved.retiredSource,
            manifest.source.originalIdentity,
            manifest.source.originalGeneration,
            manifest.source.contents,
            sourceRetired,
            budget,
            error)) {
        return false;
    }
    if (sourceRetired) {
        reportSplitTransition("split-committed-source-retired");
    }

    bool backupRetired = false;
    if (manifest.backup.exists
        && !retireCommittedIdentityIfCurrent(
            resolved.retiredBackup,
            manifest.backup.originalIdentity,
            manifest.backup.originalGeneration,
            manifest.backup.contents,
            backupRetired,
            budget,
            error)) {
        return false;
    }
    if (backupRetired) {
        reportSplitTransition(
            "split-committed-prior-backup-retired");
    }
    return true;
}

bool cleanupUnpreparedSplitArtifacts(
    const SplitNamespace &nameSpace,
    SplitJournal &journal,
    const SplitJournalIntent &intent,
    bool lockArtifacts,
    SplitReadBudget &budget,
    QString &error)
{
    QStringList lockPaths;
    for (const QString &path : intent.artifactPaths) {
        if (!validSplitRelativePath(path)) {
            error = QStringLiteral(
                "A split intent contains an invalid artifact path");
            return false;
        }
        lockPaths.append(QDir(nameSpace.root).filePath(path));
    }
    AtomicFileLockSet locks;
    if (lockArtifacts && !locks.lock(lockPaths, error)) {
        error = QStringLiteral(
            "An unprepared split artifact is still active: %1")
                    .arg(error);
        return false;
    }

    for (qsizetype index = 0;
         index < intent.artifactPaths.size(); ++index) {
        ResolvedSplitPath artifact;
        if (!resolveAnchoredSplitPath(
                nameSpace.rootDirectory,
                nameSpace.root,
                intent.artifactPaths.at(index),
                artifact,
                error)) {
            return false;
        }
        SplitCleanupRecord record;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> recordFile;
        bool recordExists = false;
        if (!readSplitCleanupRecord(
                journal,
                index,
                artifact.relativePath,
                record,
                recordFile,
                recordExists,
                budget,
                error)) {
            return false;
        }

        std::unique_ptr<AnchoredFileSystem::PinnedFile> staged;
        bool stagedExists = false;
        if (recordExists) {
            if (!pinExpectedResolvedFile(
                    artifact,
                    record.identity,
                    {},
                    record.contents,
                    staged,
                    stagedExists,
                    error,
                    &budget,
                    record.generation)) {
                return false;
            }
        } else if (!pinResolvedFile(
                       artifact,
                       staged,
                       stagedExists,
                       error,
                       MaximumSplitPayloadSize,
                       &budget)) {
            return false;
        }

        if (!recordExists && stagedExists) {
            error = QStringLiteral(
                "Unproven split artifact retained at %1; remove or preserve it manually before retrying recovery")
                    .arg(artifact.absolutePath);
            return false;
        }

        std::unique_ptr<AnchoredFileSystem::PinnedFile> quarantine;
        bool quarantineExists = false;
        if (recordExists && !stagedExists
            && !pinExpectedRemovalQuarantine(
                artifact.parent,
                QFileInfo(artifact.relativePath).fileName(),
                record.identity,
                record.contents,
                quarantine,
                quarantineExists,
                error,
                &budget,
                record.generation)) {
            return false;
        }
        if (stagedExists && quarantineExists) {
            error = QStringLiteral(
                "An unprepared split artifact exists at multiple names");
            return false;
        }
        if (!stagedExists && !quarantineExists) continue;

        AnchoredFileSystem::PinnedFile *removal = stagedExists
            ? staged.get()
            : quarantine.get();
        if (!removePinnedFile(
                *removal, artifact.parent, budget, error)) {
            return false;
        }
        reportSplitTransition(
            "split-unprepared-artifact-retired");
    }
    return true;
}

bool synchronizeSplitProductionParents(
    const ResolvedSplitTransaction &resolved,
    QString &error);

bool completeForwardSplitTransaction(
    SplitJournal &journal,
    const SplitJournalManifest &manifest,
    const ResolvedSplitTransaction &resolved,
    SplitReadBudget &budget,
    QString &error)
{
    if (!splitParentsAreCurrent(resolved, error)
        || !journal.directory.pathMatches(error)) {
        return false;
    }

    for (qsizetype index = 0; index < manifest.outputs.size(); ++index) {
        const SplitJournalOutput &expected = manifest.outputs.at(index);
        std::unique_ptr<AnchoredFileSystem::PinnedFile> finalFile;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> stageFile;
        bool finalExists = false;
        bool stageExists = false;
        if (!pinExpectedResolvedFile(
                resolved.outputs.at(index),
                expected.publicationStageIdentity,
                {}, expected.contents,
                finalFile, finalExists, error, &budget,
                expected.publicationStageGeneration)) {
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "production output preserved at %1; recovery journal retained at %2")
                    .arg(resolved.outputs.at(index).absolutePath,
                         journal.path));
            return false;
        }
        if (!pinExpectedResolvedFile(
                resolved.outputStages.at(index),
                expected.publicationStageIdentity,
                {}, expected.contents,
                stageFile, stageExists, error, &budget,
                expected.publicationStageGeneration)) {
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "split output stage preserved at %1; recovery journal retained at %2")
                    .arg(resolved.outputStages.at(index).absolutePath,
                         journal.path));
            return false;
        }
        if (finalExists && stageExists) {
            error = QStringLiteral(
                "A split output exists at both stage and production paths");
            return false;
        }
        if (!finalExists && !stageExists) {
            error = QStringLiteral(
                "A forward-only split output is unavailable; recovery retained at %1")
                    .arg(journal.path);
            return false;
        }
        if (stageExists) {
            reportSplitTransition("split-before-output-published");
            if (!movePinnedFile(
                    *stageFile,
                    resolved.outputStages.at(index),
                    resolved.outputs.at(index).entry,
                    resolved.outputs.at(index).parent,
                    budget,
                    error)) {
                appendAtomicFileError(
                    error,
                    QStringLiteral(
                        "production output preserved at %1; recovery journal retained at %2")
                        .arg(resolved.outputs.at(index).absolutePath,
                             journal.path));
                return false;
            }
            reportSplitTransition("split-output-published");
        }
    }

    if (!manifest.keepOriginal) {
        std::unique_ptr<AnchoredFileSystem::PinnedFile> backup;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> archiveStage;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> retiredBackup;
        bool backupExists = false;
        bool archiveStageExists = false;
        bool retiredBackupExists = false;
        const qint64 backupMaximum = manifest.backup.exists
            ? std::max(
                manifest.source.contents.size,
                manifest.backup.contents.size)
            : manifest.source.contents.size;
        if (!pinResolvedFile(
                resolved.backup, backup, backupExists, error,
                backupMaximum, &budget)
            || !pinExpectedResolvedFile(
                resolved.sourceArchiveStage,
                manifest.source.archiveStageIdentity,
                {}, manifest.source.contents,
                archiveStage, archiveStageExists, error, &budget,
                manifest.source.archiveStageGeneration)) {
            return false;
        }
        SplitBackupGeneration generation =
            SplitBackupGeneration::Absent;
        if (backupExists) {
            if (backup->identity()
                    == manifest.source.archiveStageIdentity
                && backup->durableGeneration()
                    == manifest.source.archiveStageGeneration
                && backup->size() == manifest.source.contents.size
                && backup->sha256()
                    == manifest.source.contents.digest) {
                generation = SplitBackupGeneration::Source;
            } else if (manifest.backup.exists
                && backup->identity()
                    == manifest.backup.originalIdentity
                && backup->durableGeneration()
                    == manifest.backup.originalGeneration
                && backup->size() == manifest.backup.contents.size
                && backup->sha256()
                    == manifest.backup.contents.digest) {
                generation = SplitBackupGeneration::Prior;
            } else {
                const bool reusedGeneration =
                    (backup->identity()
                            == manifest.source.archiveStageIdentity
                        && backup->durableGeneration()
                            != manifest.source.archiveStageGeneration)
                    || (manifest.backup.exists
                        && backup->identity()
                            == manifest.backup.originalIdentity
                        && backup->durableGeneration()
                            != manifest.backup.originalGeneration);
                error = reusedGeneration
                    ? QStringLiteral(
                          "A foreign backup generation was preserved at %1")
                          .arg(resolved.backup.absolutePath)
                    : QStringLiteral(
                          "A foreign backup was preserved at %1")
                          .arg(resolved.backup.absolutePath);
                return false;
            }
        }
        if (manifest.backup.exists
            && !pinExpectedResolvedFile(
                resolved.retiredBackup,
                manifest.backup.originalIdentity,
                {}, manifest.backup.contents,
                retiredBackup, retiredBackupExists,
                error, &budget,
                manifest.backup.originalGeneration)) {
            return false;
        }
        if (generation == SplitBackupGeneration::Prior
            && retiredBackupExists) {
            error = QStringLiteral(
                "The prior backup exists at two forward-recovery paths");
            return false;
        }
        if (manifest.backup.exists
            && generation != SplitBackupGeneration::Prior
            && !retiredBackupExists) {
            error = QStringLiteral(
                "The prior backup is unavailable during forward recovery");
            return false;
        }
        if (!manifest.backup.exists && retiredBackupExists) {
            error = QStringLiteral(
                "An unexpected retired backup was preserved");
            return false;
        }
        if (generation == SplitBackupGeneration::Source
            && archiveStageExists) {
            error = QStringLiteral(
                "The source archive exists at two forward-recovery paths");
            return false;
        }
        if (generation != SplitBackupGeneration::Source
            && !archiveStageExists) {
            error = QStringLiteral(
                "The source archive is unavailable during forward recovery");
            return false;
        }

        if (generation == SplitBackupGeneration::Prior) {
            if (!movePinnedFile(
                    *backup,
                    resolved.backup,
                    resolved.retiredBackup.entry,
                    resolved.retiredBackup.parent,
                    budget,
                    error)) {
                return false;
            }
            retiredBackup = std::move(backup);
            retiredBackupExists = true;
            backupExists = false;
            generation = SplitBackupGeneration::Absent;
            reportSplitTransition("split-prior-backup-retired");
        }
        if (generation != SplitBackupGeneration::Source) {
            if (!movePinnedFile(
                    *archiveStage,
                    resolved.sourceArchiveStage,
                    resolved.backup.entry,
                    resolved.backup.parent,
                    budget,
                    error)) {
                return false;
            }
            backup = std::move(archiveStage);
            archiveStageExists = false;
            backupExists = true;
            generation = SplitBackupGeneration::Source;
            reportSplitTransition("split-source-archived");
        }
    }

    if (!synchronizeSplitProductionParents(resolved, error)) {
        return false;
    }

    if (!manifest.keepOriginal) {
        std::unique_ptr<AnchoredFileSystem::PinnedFile> source;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> retiredSource;
        bool sourceExists = false;
        bool retiredSourceExists = false;
        if (!pinExpectedResolvedFile(
                resolved.source,
                manifest.source.originalIdentity,
                {}, manifest.source.contents,
                source, sourceExists, error, &budget,
                manifest.source.originalGeneration)
            || !pinExpectedResolvedFile(
                resolved.retiredSource,
                manifest.source.originalIdentity,
                {}, manifest.source.contents,
                retiredSource, retiredSourceExists,
                error, &budget,
                manifest.source.originalGeneration)) {
            return false;
        }
        if (int(sourceExists) + int(retiredSourceExists) != 1) {
            error = QStringLiteral(
                "The split source is not in exactly one forward-recovery location");
            return false;
        }
        if (sourceExists) {
            if (!movePinnedFile(
                    *source,
                    resolved.source,
                    resolved.retiredSource.entry,
                    resolved.retiredSource.parent,
                    budget,
                    error)) {
                return false;
            }
            reportSplitTransition("split-source-retired-for-commit");
        }
    }

    if (!verifyCommittedSplitTransaction(
            manifest, resolved, budget, error)) {
        return false;
    }

    bool committed = false;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> marker;
    if (!readSplitCommitMarker(
            journal, committed, marker, budget, error)) {
        return false;
    }
    if (!committed) {
        const QByteArray markerContents =
            manifest.id.toLatin1() + '\n';
        if (!writeNewJournalFile(
                journal,
                SplitCommitMarkerName,
                markerContents,
                marker,
                error)) {
            return false;
        }
    }
    reportSplitTransition("split-committed-marker-published");
    return true;
}

bool recoverSplitJournal(
    const SplitNamespace &nameSpace,
    SplitJournal &journal,
    SplitReadBudget &budget,
    QString &error)
{
    if (!budget.consumeOperation(error)) return false;
    if (!cleanupInterruptedJournalPublications(
            journal, budget, error)) {
        return false;
    }
    SplitJournalManifest manifest;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> manifestFile;
    bool manifestExists = false;
    if (!readSplitManifest(
            journal,
            manifest,
            manifestFile,
            manifestExists,
            budget,
            error)) {
        return false;
    }
    if (!manifestExists) {
        SplitJournalIntent intent;
        std::unique_ptr<AnchoredFileSystem::PinnedFile> intentFile;
        bool intentExists = false;
        if (!readSplitIntent(
                journal,
                intent,
            intentFile,
            intentExists,
            budget,
            error)) {
            return false;
        }
        intentFile.reset();
        return (!intentExists
                || cleanupUnpreparedSplitArtifacts(
                    nameSpace, journal, intent, true, budget, error))
            && cleanupSplitJournal(
                nameSpace, journal, budget, error);
    }

    ResolvedSplitTransaction resolved;
    if (!resolveSplitTransaction(
            nameSpace.rootDirectory,
            nameSpace.root,
            manifest,
            resolved,
            error)) {
        return false;
    }

    AtomicFileLockSet fileLocks;
    if (!fileLocks.lock(
            splitProductionLockPaths(resolved), error)) {
        error = QStringLiteral(
            "A split transaction file is still active: %1")
                    .arg(error);
        return false;
    }
    if (!splitParentsAreCurrent(resolved, error)
        || !journal.directory.pathMatches(error)) {
        return false;
    }

    bool committed = false;
    bool committing = false;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> marker;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> committingMarker;
    if (!readSplitCommitMarker(
            journal, committed, marker, budget, error)
        || !readSplitPhaseMarker(
            journal, SplitCommittingMarkerName,
            committing, committingMarker, budget, error)) {
        return false;
    }
    manifestFile.reset();
    marker.reset();
    committingMarker.reset();

    if (committed) {
        if (!cleanupCommittedSplitProduction(
                manifest, resolved, budget, error)) {
            return false;
        }
        return cleanupSplitJournal(
            nameSpace, journal, budget, error, &manifest);
    }
    if (committing) {
        if (!completeForwardSplitTransaction(
                journal, manifest, resolved, budget, error)
            || !cleanupCommittedSplitProduction(
                manifest, resolved, budget, error)) {
            return false;
        }
        return cleanupSplitJournal(
            nameSpace, journal, budget, error, &manifest);
    }
    return rollbackSplitTransaction(
        nameSpace, journal, manifest, resolved, budget, error);
}

bool reconcileSplitTransactionsLocked(
    const QString &root,
    SplitReadBudget &budget,
    QString &error)
{
    SplitNamespace nameSpace;
    if (!openSplitNamespace(
            root, false, nameSpace, error)) {
        return false;
    }
    if (!nameSpace.exists) return true;

    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!nameSpace.namespaceDirectory.enumerateEntries(
            entries, MaximumSplitNamespaceEntries, error)) {
        return false;
    }
    for (const auto &entry : entries) {
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::Directory
            || !validTransactionId(entry.name)) {
            error = QStringLiteral(
                "The split transaction namespace has an unsafe entry");
            return false;
        }

        AnchoredFileSystem::DirectoryAnchor directory;
        bool exists = false;
        if (!nameSpace.namespaceDirectory.openChildIfExists(
                entry.name, directory, exists, error)
            || !exists
            || directory.identity() != entry.identity
            || directory.identity().linkCount()
                != entry.identity.linkCount()
            || !AnchoredFileSystem::validateCurrentUserOwnedDirectory(
                directory, error)
            || !AnchoredFileSystem::hardenPrivateDirectory(
                directory, error)
            || !nameSpace.namespaceDirectory.pathMatches(error)
            || !directory.pathMatches(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A split transaction directory changed after enumeration");
            }
            return false;
        }

        SplitJournal journal;
        journal.id = entry.name;
        journal.path =
            QDir(nameSpace.namespacePath).filePath(entry.name);
        journal.directory = directory;
        if (!recoverSplitJournal(
                nameSpace, journal, budget, error)) {
            return false;
        }
    }
    return true;
}

bool preparedJournalFilesAreCurrent(
    const SplitJournal &journal,
    const QHash<QString, AnchoredFileSystem::NativeIdentity> &expected,
    const QHash<QString, QByteArray> &expectedGenerations,
    QString &error)
{
    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!journal.directory.enumerateEntries(
            entries, MaximumSplitJournalEntries, error)
        || entries.size() != expected.size()) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared split journal contains unexpected files");
        }
        return false;
    }
    for (const auto &entry : entries) {
        const auto identity = expected.constFind(entry.name);
        if (identity == expected.constEnd()
            || entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::RegularFile
            || entry.identity.linkCount() != 1
            || entry.identity != identity.value()) {
            error = QStringLiteral(
                "A prepared split journal file changed unexpectedly");
            return false;
        }
        const auto generation = expectedGenerations.constFind(entry.name);
        if (generation != expectedGenerations.constEnd()) {
            std::unique_ptr<AnchoredFileSystem::PinnedFile> current;
            bool exists = false;
            if (!pinJournalFile(
                    journal,
                    entry.name,
                    current,
                    exists,
                    error,
                    MaximumSplitPayloadSize,
                    &entry.identity)
                || !exists
                || generation.value().isEmpty()
                || current->durableGeneration() != generation.value()) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "A prepared split journal generation changed unexpectedly");
                }
                return false;
            }
        }
    }
    return journal.directory.pathMatches(error);
}

bool synchronizeSplitProductionParents(
    const ResolvedSplitTransaction &resolved,
    QString &error)
{
    QList<ResolvedSplitPath> paths = resolved.outputs;
    paths.append(resolved.source);
    if (resolved.backup.entry.isValid()) {
        paths.append(resolved.backup);
    }

    QSet<QString> synchronized;
    reportSplitTransition(
        "split-before-output-parents-synchronized");
    for (const ResolvedSplitPath &path : paths) {
        const QString key = atomicFilePathKey(
            QFileInfo(path.absolutePath).absolutePath());
        if (synchronized.contains(key)) continue;
        synchronized.insert(key);
        if (!path.parent.pathMatches(error)
            || !path.parent.sync(error)
            || !path.parent.pathMatches(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot synchronize a split target parent directory");
            }
            return false;
        }
    }
    reportSplitTransition(
        "split-output-parents-synchronized");
    return true;
}

bool saveSplitActivityFilesDurably(
    const QDir &activitiesDirectory,
    const QString &sourcePath,
    const QString &backupPath,
    const QList<SplitActivityOutput> &outputs,
    bool keepOriginal,
    QStringList &publishedFileNames,
    QString &error)
{
    error.clear();
    publishedFileNames.clear();

    if (outputs.isEmpty()) {
        error = QStringLiteral(
            "No split activity files to save");
        return false;
    }
    if (outputs.size() > MaximumSplitOutputs) {
        error = QStringLiteral(
            "A split activity cannot produce more than 1000 files");
        return false;
    }

    const QString activitiesPath = QDir::cleanPath(
        activitiesDirectory.absolutePath());
    const QFileInfo activitiesInfo(activitiesPath);
    if (!activitiesInfo.exists() || !activitiesInfo.isDir()
        || activitiesInfo.isSymLink()
        || activitiesInfo.fileName().isEmpty()) {
        error = QStringLiteral(
            "The activity directory is unavailable or unsafe");
        return false;
    }

    QString root;
    if (!normalizeSplitRoot(
            activitiesInfo.absolutePath(), root, error)) {
        return false;
    }

    const QFileInfo requestedSource(sourcePath);
    const QString source = QDir::cleanPath(
        requestedSource.absoluteFilePath());
    const QString requestedSourceDirectory =
        QDir::cleanPath(requestedSource.absolutePath());
    if (requestedSource.fileName().isEmpty()
        || QDir::fromNativeSeparators(requestedSourceDirectory)
            != QDir::fromNativeSeparators(activitiesPath)) {
        error = QStringLiteral(
            "The source activity is outside the activity directory");
        return false;
    }

    SplitJournalManifest manifest;
    manifest.id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    manifest.keepOriginal = keepOriginal;
    if (!makeSplitRelativePath(
            root,
            source,
            manifest.source.relativePath,
            error)) {
        return false;
    }
    manifest.source.exists = true;
    manifest.source.storedName =
        QStringLiteral("source.old");

    QString backup;
    if (!keepOriginal) {
        const QFileInfo requestedBackup(backupPath);
        backup = QDir::cleanPath(
            requestedBackup.absoluteFilePath());
        if (requestedBackup.fileName().isEmpty()
            || !makeSplitRelativePath(
                root,
                backup,
                manifest.backup.relativePath,
                error)
            || splitPathsCollide(
                manifest.source.relativePath,
                manifest.backup.relativePath)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The split source and backup paths overlap");
            }
            return false;
        }
        const QString backupParent =
            QFileInfo(manifest.backup.relativePath).path();
        manifest.source.archiveStagePath =
            QDir::fromNativeSeparators(
                QDir(backupParent).filePath(
                    QStringLiteral(".gc-split-%1-source.stage")
                        .arg(manifest.id)));
        manifest.source.retiredPath =
            QDir::fromNativeSeparators(
                QDir(QFileInfo(
                    manifest.source.relativePath).path()).filePath(
                        QStringLiteral(
                            ".gc-split-%1-source.committed")
                            .arg(manifest.id)));
        manifest.backup.retiredPath =
            QDir::fromNativeSeparators(
                QDir(backupParent).filePath(
                    QStringLiteral(".gc-split-%1-backup.old")
                        .arg(manifest.id)));
    }

    std::vector<PreparedSplitOutput> prepared;
    prepared.reserve(
        static_cast<std::size_t>(outputs.size()));
    QList<QString> productionPaths;
    productionPaths.append(manifest.source.relativePath);
    if (!keepOriginal) {
        productionPaths.append(manifest.backup.relativePath);
        productionPaths.append(manifest.source.archiveStagePath);
        productionPaths.append(manifest.source.retiredPath);
        productionPaths.append(manifest.backup.retiredPath);
    }
    for (qsizetype index = 0; index < outputs.size(); ++index) {
        const SplitActivityOutput &output = outputs.at(index);
        if (!output.stage
            || !validSplitFileName(output.fileName)) {
            error = QStringLiteral(
                "Invalid split activity file name");
            return false;
        }

        PreparedSplitOutput item;
        item.request = output;
        item.journal.relativePath =
            QDir::fromNativeSeparators(
                QDir(root).relativeFilePath(
                    QDir(activitiesPath).filePath(
                        output.fileName)));
        item.journal.storedName =
            QStringLiteral("output-%1.new")
                .arg(index, 4, 10, QLatin1Char('0'));
        item.journal.publicationStagePath =
            QDir::fromNativeSeparators(
                QDir(QFileInfo(item.journal.relativePath).path())
                    .filePath(
                        QStringLiteral(
                            ".gc-split-%1-output-%2.stage")
                            .arg(manifest.id)
                            .arg(index, 4, 10, QLatin1Char('0'))));
        if (!validSplitRelativePath(
                item.journal.relativePath)
            || !validSplitRelativePath(
                item.journal.publicationStagePath)) {
            error = QStringLiteral(
                "A split output path is invalid");
            return false;
        }
        productionPaths.append(item.journal.relativePath);
        productionPaths.append(item.journal.publicationStagePath);
        manifest.outputs.append(item.journal);
        prepared.push_back(std::move(item));
    }
    if (!splitPathsAreDisjoint(productionPaths, nullptr, error)) {
        return false;
    }

    AtomicFileLockSet lease;
    if (!lease.lock(
            {splitTransactionLeaseTarget(root)}, error)) {
        error = QStringLiteral(
            "Another split activity transaction is active: %1")
                    .arg(error);
        return false;
    }
    SplitReadBudget startupRecoveryBudget(true);
    if (!reconcileSplitTransactionsLocked(
            root, startupRecoveryBudget, error)) {
        return false;
    }

    SplitNamespace nameSpace;
    if (!openSplitNamespace(
            root, true, nameSpace, error)
        || !nameSpace.exists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot prepare the split transaction namespace");
        }
        return false;
    }

    ResolvedSplitTransaction resolved;
    if (!resolveSplitTransaction(
            nameSpace.rootDirectory,
            root,
            manifest,
            resolved,
            error)) {
        return false;
    }
    for (qsizetype index = 0; index < prepared.size(); ++index) {
        prepared[static_cast<std::size_t>(index)].target =
            resolved.outputs.at(index);
    }

    AtomicFileLockSet fileLocks;
    if (!fileLocks.lock(
            splitProductionLockPaths(resolved), error)
        || !splitParentsAreCurrent(resolved, error)) {
        return false;
    }

    SplitReadBudget preparationBudget;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> sourceFile;
    bool sourceExists = false;
    if (!pinResolvedFile(
            resolved.source,
            sourceFile,
            sourceExists,
            error,
            MaximumSplitPayloadSize,
            &preparationBudget)
        || !sourceExists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The split source is unavailable or unsafe");
        }
        return false;
    }
    manifest.source.contents = {
        sourceFile->size(), sourceFile->sha256()};
    manifest.source.originalIdentity =
        sourceFile->identity();
    manifest.source.originalGeneration =
        sourceFile->durableGeneration();
    if (manifest.source.originalGeneration.isEmpty()) {
        error = QStringLiteral(
            "The split source filesystem cannot provide durable generation evidence");
        return false;
    }
    qint64 aggregatePayload = manifest.source.contents.size;

    std::unique_ptr<AnchoredFileSystem::PinnedFile> backupFile;
    bool backupExists = false;
    if (!keepOriginal) {
        if (!pinResolvedFile(
                resolved.backup,
                backupFile,
                backupExists,
                error,
                MaximumSplitPayloadSize,
                &preparationBudget)) {
            return false;
        }
        manifest.backup.exists = backupExists;
        if (backupExists) {
            manifest.backup.contents = {
                backupFile->size(), backupFile->sha256()};
            manifest.backup.originalIdentity =
                backupFile->identity();
            manifest.backup.originalGeneration =
                backupFile->durableGeneration();
            if (manifest.backup.originalGeneration.isEmpty()) {
                error = QStringLiteral(
                    "The split backup filesystem cannot provide durable generation evidence");
                return false;
            }
            manifest.backup.storedName =
                QStringLiteral("backup.old");
            if (aggregatePayload
                    > MaximumSplitPayloadAggregate
                        - manifest.backup.contents.size) {
                error = QStringLiteral(
                    "The split transaction exceeds its aggregate content limit");
                return false;
            }
            aggregatePayload += manifest.backup.contents.size;
        }
    }

    for (const ResolvedSplitPath &target : resolved.outputs) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                target.entry, exists, error)) {
            return false;
        }
        if (exists) {
            error = QStringLiteral(
                "A split activity target already exists");
            return false;
        }
    }
    if (!keepOriginal) {
        bool retiredSourceExists = false;
        if (!AnchoredFileSystem::entryExists(
                resolved.retiredSource.entry,
                retiredSourceExists,
                error)
            || retiredSourceExists) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A split source retirement path already exists");
            }
            return false;
        }
    }

    SplitJournal journal;
    if (!createSplitJournal(
            nameSpace, manifest.id, journal, error)) {
        return false;
    }

    const SplitJournalIntent intent =
        splitIntentForManifest(manifest);
    const QByteArray intentContents =
        serializeSplitIntent(intent);
    std::unique_ptr<AnchoredFileSystem::PinnedFile> intentFile;
    if (intentContents.size() > MaximumSplitManifestSize
        || !writeNewJournalFile(
            journal,
            SplitIntentName,
            intentContents,
            intentFile,
            error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot publish the split transaction intent");
        }
        intentFile.reset();
        appendSplitCleanupError(nameSpace, journal, error);
        return false;
    }
    reportSplitTransition("split-intent-published");
    QHash<QString, AnchoredFileSystem::NativeIdentity>
        preparedJournalIdentities;
    QHash<QString, QByteArray> preparedJournalGenerations;
    preparedJournalIdentities.insert(
        SplitIntentName, intentFile->identity());

    std::unique_ptr<AnchoredFileSystem::PinnedFile> storedSource;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> storedBackup;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> sourceArchiveStage;
    SplitReadBudget transactionRecoveryBudget;
    const auto cleanupBeforeManifest = [&]() {
        sourceArchiveStage.reset();
        storedSource.reset();
        storedBackup.reset();
        for (PreparedSplitOutput &output : prepared) {
            output.publicationStage.reset();
            output.staged.reset();
        }
        intentFile.reset();
        QString artifactCleanupError;
        if (!cleanupUnpreparedSplitArtifacts(
                nameSpace,
                journal,
                intent,
                false,
                transactionRecoveryBudget,
                artifactCleanupError)) {
            appendAtomicFileError(error, artifactCleanupError);
            appendAtomicFileError(
                error,
                QStringLiteral("split recovery journal retained at %1")
                    .arg(journal.path));
            return false;
        }
        appendSplitCleanupError(
            nameSpace, journal, error);
        return false;
    };

    {
        const AnchoredFileSystem::EntryRef destination =
            journal.directory.entry(
                manifest.source.storedName, error);
        storedSource =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        if (!destination.isValid()
            || !AnchoredFileSystem::copyToNewFile(
                *sourceFile,
                destination,
                *storedSource,
                error)
            || storedSource->size()
                != manifest.source.contents.size
            || storedSource->sha256()
                != manifest.source.contents.digest) {
            return cleanupBeforeManifest();
        }
        manifest.source.storedIdentity =
            storedSource->identity();
        manifest.source.storedGeneration =
            storedSource->durableGeneration();
        if (manifest.source.storedGeneration.isEmpty()) {
            error = QStringLiteral(
                "The split journal filesystem cannot provide durable generation evidence");
            return cleanupBeforeManifest();
        }
        preparedJournalIdentities.insert(
            manifest.source.storedName,
            manifest.source.storedIdentity);
        preparedJournalGenerations.insert(
            manifest.source.storedName,
            manifest.source.storedGeneration);
        reportSplitTransition(
            "split-source-snapshot-published");
        storedSource.reset();
    }

    if (manifest.backup.exists) {
        const AnchoredFileSystem::EntryRef destination =
            journal.directory.entry(
                manifest.backup.storedName, error);
        storedBackup =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        if (!destination.isValid()
            || !AnchoredFileSystem::copyToNewFile(
                *backupFile,
                destination,
                *storedBackup,
                error)
            || storedBackup->size()
                != manifest.backup.contents.size
            || storedBackup->sha256()
                != manifest.backup.contents.digest) {
            return cleanupBeforeManifest();
        }
        manifest.backup.storedIdentity =
            storedBackup->identity();
        manifest.backup.storedGeneration =
            storedBackup->durableGeneration();
        if (manifest.backup.storedGeneration.isEmpty()) {
            error = QStringLiteral(
                "The split journal filesystem cannot provide durable generation evidence");
            return cleanupBeforeManifest();
        }
        preparedJournalIdentities.insert(
            manifest.backup.storedName,
            manifest.backup.storedIdentity);
        preparedJournalGenerations.insert(
            manifest.backup.storedName,
            manifest.backup.storedGeneration);
        reportSplitTransition(
            "split-prior-backup-snapshot-published");
        storedBackup.reset();
    }

    if (!keepOriginal) {
        sourceArchiveStage =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        if (!AnchoredFileSystem::copyToNewFile(
                *sourceFile,
                resolved.sourceArchiveStage.entry,
                *sourceArchiveStage,
                error,
                true)) {
            return cleanupBeforeManifest();
        }
        manifest.source.archiveStageIdentity =
            sourceArchiveStage->identity();
        manifest.source.archiveStageGeneration =
            sourceArchiveStage->durableGeneration();
        if (manifest.source.archiveStageGeneration.isEmpty()) {
            error = QStringLiteral(
                "The split archive filesystem cannot provide durable generation evidence");
            QString removalError;
            if (!removePinnedFile(
                    *sourceArchiveStage,
                    resolved.sourceArchiveStage.parent,
                    transactionRecoveryBudget,
                    removalError)) {
                appendAtomicFileError(error, removalError);
            }
            return cleanupBeforeManifest();
        }
        AnchoredFileSystem::NativeIdentity recordIdentity;
        if (!publishSplitCleanupRecord(
                journal,
                0,
                manifest.source.archiveStagePath,
                *sourceArchiveStage,
                recordIdentity,
                error)) {
            return cleanupBeforeManifest();
        }
        preparedJournalIdentities.insert(
            splitCleanupRecordName(0), recordIdentity);
        reportSplitTransition(
            "split-source-archive-stage-published");
    }

    for (qsizetype index = 0; index < prepared.size(); ++index) {
        PreparedSplitOutput &item =
            prepared[static_cast<std::size_t>(index)];
        QByteArray stagedContents;
        QString stageError;
        if (!item.request.stage(stagedContents, stageError)) {
            error = stageError.isEmpty()
                ? QStringLiteral(
                      "Cannot stage a split activity file")
                : stageError;
            return cleanupBeforeManifest();
        }
        if (stagedContents.size() > MaximumSplitPayloadSize) {
            error = QStringLiteral(
                "A split output exceeds the per-file content limit");
            return cleanupBeforeManifest();
        }
        if (!preparationBudget.consumeOperation(error)
            || !preparationBudget.consumeBytes(
                stagedContents.size(), error)) {
            return cleanupBeforeManifest();
        }
        if (aggregatePayload
                > MaximumSplitPayloadAggregate
                    - stagedContents.size()) {
            error = QStringLiteral(
                "The split transaction exceeds its aggregate content limit");
            return cleanupBeforeManifest();
        }
        aggregatePayload += stagedContents.size();
        const AnchoredFileSystem::EntryRef storedEntry =
            journal.directory.entry(
                item.journal.storedName, error);
        item.staged =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        if (!storedEntry.isValid()
            || !AnchoredFileSystem::writeNewFile(
                stagedContents,
                storedEntry,
                *item.staged,
                error)) {
            return cleanupBeforeManifest();
        }
        reportSplitTransition(
            "split-output-input-synchronized");

        item.journal.contents = {
            item.staged->size(), item.staged->sha256()};
        item.journal.storedIdentity =
            item.staged->identity();
        item.journal.storedGeneration =
            item.staged->durableGeneration();
        if (item.journal.storedGeneration.isEmpty()) {
            error = QStringLiteral(
                "The split journal filesystem cannot provide durable generation evidence");
            return cleanupBeforeManifest();
        }
        preparedJournalIdentities.insert(
            item.journal.storedName,
            item.journal.storedIdentity);
        preparedJournalGenerations.insert(
            item.journal.storedName,
            item.journal.storedGeneration);
        item.publicationStage =
            std::make_unique<AnchoredFileSystem::PinnedFile>();
        if (!AnchoredFileSystem::copyToNewFile(
                *item.staged,
                resolved.outputStages.at(index).entry,
                *item.publicationStage,
                error,
                true)) {
            return cleanupBeforeManifest();
        }
        item.journal.publicationStageIdentity =
            item.publicationStage->identity();
        item.journal.publicationStageGeneration =
            item.publicationStage->durableGeneration();
        if (item.journal.publicationStageGeneration.isEmpty()) {
            error = QStringLiteral(
                "The split output filesystem cannot provide durable generation evidence");
            QString removalError;
            if (!removePinnedFile(
                    *item.publicationStage,
                    resolved.outputStages.at(index).parent,
                    transactionRecoveryBudget,
                    removalError)) {
                appendAtomicFileError(error, removalError);
            }
            return cleanupBeforeManifest();
        }
        manifest.outputs[index] = item.journal;
        const qsizetype cleanupIndex = keepOriginal
            ? index : index + 1;
        AnchoredFileSystem::NativeIdentity recordIdentity;
        if (!publishSplitCleanupRecord(
                journal,
                cleanupIndex,
                item.journal.publicationStagePath,
                *item.publicationStage,
                recordIdentity,
                error)) {
            return cleanupBeforeManifest();
        }
        preparedJournalIdentities.insert(
            splitCleanupRecordName(cleanupIndex),
            recordIdentity);
        reportSplitTransition(
            "split-output-staged");
        item.publicationStage.reset();
        item.staged.reset();
    }

    if (!pinnedFileIsCurrent(
            resolved.source, *sourceFile, error)
        || (manifest.backup.exists
            && !pinnedFileIsCurrent(
                resolved.backup, *backupFile, error))
        || (!keepOriginal
            && !pinnedFileIsCurrent(
                resolved.sourceArchiveStage,
                *sourceArchiveStage,
                error))
        || !splitParentsAreCurrent(resolved, error)) {
        return cleanupBeforeManifest();
    }
    for (qsizetype index = 0; index < prepared.size(); ++index) {
        const SplitJournalOutput &expected = manifest.outputs.at(index);
        std::unique_ptr<AnchoredFileSystem::PinnedFile> stage;
        bool exists = false;
        if (!pinExpectedResolvedFile(
                resolved.outputStages.at(index),
                expected.publicationStageIdentity,
                {}, expected.contents,
                stage, exists, error, &preparationBudget,
                expected.publicationStageGeneration)
            || !exists) {
            return cleanupBeforeManifest();
        }
    }
    for (const ResolvedSplitPath &target : resolved.outputs) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                target.entry, exists, error)) {
            return cleanupBeforeManifest();
        }
        if (exists) {
            error = QStringLiteral(
                "A split activity target appeared while staging");
            return cleanupBeforeManifest();
        }
    }
    if (!preparedJournalFilesAreCurrent(
            journal,
            preparedJournalIdentities,
            preparedJournalGenerations,
            error)) {
        return cleanupBeforeManifest();
    }

    const QByteArray manifestContents =
        serializeSplitManifest(manifest);
    if (manifestContents.size()
            > MaximumSplitManifestSize) {
        error = QStringLiteral(
            "The split transaction manifest is too large");
        return cleanupBeforeManifest();
    }
    std::unique_ptr<AnchoredFileSystem::PinnedFile> manifestFile;
    if (!writeNewJournalFile(
            journal,
            SplitManifestName,
            manifestContents,
            manifestFile,
            error)) {
        manifestFile.reset();
        return cleanupBeforeManifest();
    }
    reportSplitTransition(
        "split-prepared-manifest-published");

    const auto releaseTransactionPins = [&]() {
        manifestFile.reset();
        intentFile.reset();
        storedSource.reset();
        storedBackup.reset();
        sourceArchiveStage.reset();
        sourceFile.reset();
        backupFile.reset();
        for (PreparedSplitOutput &output : prepared) {
            output.publicationStage.reset();
            output.staged.reset();
        }
    };
    std::unique_ptr<AnchoredFileSystem::PinnedFile> committingMarker;
    const QByteArray markerContents =
        manifest.id.toLatin1() + '\n';
    if (!writeNewJournalFile(
            journal,
            SplitCommittingMarkerName,
            markerContents,
            committingMarker,
            error)) {
        releaseTransactionPins();
        appendAtomicFileError(
            error,
            QStringLiteral("split recovery journal retained at %1")
                .arg(journal.path));
        return false;
    }
    reportSplitTransition("split-committing-marker-published");

    releaseTransactionPins();
    committingMarker.reset();
    if (!completeForwardSplitTransaction(
            journal, manifest, resolved,
            transactionRecoveryBudget, error)) {
        appendAtomicFileError(
            error,
            QStringLiteral(
                "forward-only split recovery retained at %1")
                .arg(journal.path));
        return false;
    }

    QString cleanupError;
    if (!cleanupCommittedSplitProduction(
            manifest, resolved,
            transactionRecoveryBudget, cleanupError)
        || !cleanupSplitJournal(
            nameSpace, journal,
            transactionRecoveryBudget, cleanupError, &manifest)) {
        if (!cleanupError.isEmpty()) {
            appendAtomicFileError(error, cleanupError);
        }
        appendAtomicFileError(
            error,
            QStringLiteral(
                "committed split recovery will resume at startup"));
    } else {
        reportSplitTransition(
            "split-transaction-cleaned");
    }

    for (const SplitActivityOutput &output : outputs) {
        publishedFileNames.append(output.fileName);
    }
    return true;
}

} // namespace

namespace SplitActivityTransaction {

bool reconcileAll(const QString &athleteRoot, QString &error)
{
    error.clear();
    QString root;
    if (!normalizeSplitRoot(
            athleteRoot, root, error)) {
        return false;
    }

    AtomicFileLockSet lease;
    if (!lease.lock(
            {splitTransactionLeaseTarget(root)}, error)) {
        error = QStringLiteral(
            "A split activity transaction is already active: %1")
                    .arg(error);
        return false;
    }
    SplitReadBudget budget(true);
    return reconcileSplitTransactionsLocked(root, budget, error);
}

} // namespace SplitActivityTransaction

bool archiveSplitActivitySource(
    const QString &sourcePath,
    const QString &backupPath,
    QString &error,
    const AtomicDirectorySyncFunction &syncDirectory,
    bool locksHeld,
    const AtomicMoveFunction &move)
{
    error.clear();
    if (!syncDirectory || !move) {
        error = QStringLiteral(
            "Cannot archive an activity without durable file operations");
        return false;
    }

    const QFileInfo requestedSource(sourcePath);
    const QFileInfo requestedBackup(backupPath);
    const QString sourceDirectory =
        QDir(requestedSource.absolutePath()).canonicalPath();
    const QString backupDirectory =
        QDir(requestedBackup.absolutePath()).canonicalPath();
    if (requestedSource.fileName().isEmpty()
        || requestedBackup.fileName().isEmpty()
        || sourceDirectory.isEmpty()
        || backupDirectory.isEmpty()) {
        error = QStringLiteral("Invalid activity archive paths");
        return false;
    }

    const QString source =
        QDir(sourceDirectory).filePath(requestedSource.fileName());
    const QString backup =
        QDir(backupDirectory).filePath(requestedBackup.fileName());
    if (portablePathKey(source) == portablePathKey(backup)) {
        error = QStringLiteral("Invalid activity archive paths");
        return false;
    }

    AtomicFileLockSet locks;
    if (!locksHeld && !locks.lock({ source, backup }, error)) {
        return false;
    }

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() || !sourceInfo.isFile()
        || sourceInfo.isSymLink()) {
        error = QStringLiteral(
            "The source activity is unavailable or unsafe");
        return false;
    }

    const QFileInfo backupDirectoryInfo(backupDirectory);
    if (!backupDirectoryInfo.exists()
        || !backupDirectoryInfo.isDir()
        || backupDirectoryInfo.isSymLink()) {
        error = QStringLiteral(
            "The activity backup directory is unavailable or unsafe");
        return false;
    }

    const QFileInfo priorBackupInfo(backup);
    const bool hadPriorBackup =
        priorBackupInfo.exists() || priorBackupInfo.isSymLink();
    if (hadPriorBackup
        && (priorBackupInfo.isSymLink()
            || !priorBackupInfo.isFile())) {
        error = QStringLiteral(
            "The existing activity backup is unsafe");
        return false;
    }

    QString previousBackupPath;
    if (hadPriorBackup) {
        previousBackupPath =
            backup + QStringLiteral(".rollback-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);

        QString preserveError;
        if (!move(backup, previousBackupPath, preserveError)) {
            error = QStringLiteral(
                "Cannot preserve the previous activity backup");
            if (!preserveError.isEmpty()) {
                appendAtomicFileError(error, preserveError);
            }
            return false;
        }

        if (!syncChangedDirectories(
                { backup, previousBackupPath },
                syncDirectory, error)) {
            restorePreviousBackup(
                previousBackupPath, backup,
                syncDirectory, move, error);
            return false;
        }
    }

    QString archiveError;
    if (!move(source, backup, archiveError)) {
        error = QStringLiteral(
            "Cannot archive the original activity");
        if (!archiveError.isEmpty()) {
            appendAtomicFileError(error, archiveError);
        }
        restorePreviousBackup(
            previousBackupPath, backup,
            syncDirectory, move, error);
        return false;
    }

    QString syncError;
    if (!syncChangedDirectories(
            { source, backup }, syncDirectory, syncError)) {
        error = syncError;

        QString rollbackError;
        const bool sourceRestored =
            move(backup, source, rollbackError);
        if (!sourceRestored) {
            appendAtomicFileError(
                error,
                rollbackError.isEmpty()
                    ? QStringLiteral(
                          "cannot restore the original activity")
                    : rollbackError);
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "the original remains archived; split files were kept"));
            syncRollbackDirectories(
                { source, backup, previousBackupPath },
                syncDirectory, error);
            return true;
        }

        restorePreviousBackup(
            previousBackupPath, backup,
            syncDirectory, move, error);
        syncRollbackDirectories(
            { source, backup, previousBackupPath },
            syncDirectory, error);
        return false;
    }

    if (!previousBackupPath.isEmpty()) {
        if (!QFile::remove(previousBackupPath)) {
            appendAtomicFileError(
                error,
                QStringLiteral(
                    "cannot remove the previous activity backup"));
        } else {
            QString cleanupError;
            if (!syncChangedDirectories(
                    { previousBackupPath },
                    syncDirectory, cleanupError)) {
                appendAtomicFileError(error, cleanupError);
            }
        }
    }
    return true;
}


bool saveSplitActivityFiles(
    const QDir &activitiesDirectory,
    const QString &sourcePath,
    const QString &backupPath,
    const QList<SplitActivityOutput> &outputs,
    bool keepOriginal,
    QStringList &publishedFileNames,
    QString &error,
    const AtomicPublishFunction &publish,
    const SplitActivityArchiveFunction &archive)
{
    if (publish || archive) {
        publishedFileNames.clear();
        error = QStringLiteral(
            "Custom split callbacks cannot bypass durable recovery");
        return false;
    }
    return saveSplitActivityFilesDurably(
        activitiesDirectory,
        sourcePath,
        backupPath,
        outputs,
        keepOriginal,
        publishedFileNames,
        error);
}
