/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "PlanReplacementJournal.h"

#include "AnchoredFileSystem.h"
#include "AtomicFileWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
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

#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
void planReplacementTransitionReached(const char *transition);
#endif

namespace PlanReplacement {

namespace Detail {

constexpr int ManifestVersion = 1;
constexpr int MaximumEntries = 4096;
constexpr qsizetype MaximumNamespaceEntries = 4096;
constexpr qsizetype MaximumJournalEntries =
    qsizetype(MaximumEntries) * 4 + 256;
constexpr qint64 MaximumManifestSize = 16 * 1024 * 1024;
constexpr qint64 MaximumCommitMarkerSize = 128;
constexpr qint64 MaximumLockFileSize = 64 * 1024;

const QString ManifestName = QStringLiteral("manifest.json");
const QString CommitMarkerName = QStringLiteral("COMMITTED");

struct Entry
{
    QString relativePath;
    bool oldExists = false;
    AtomicFileSnapshot oldContents;
    bool hasNew = false;
    int stageIndex = -1;
    bool staged = false;
    AtomicFileSnapshot newContents;
};

struct Manifest
{
    int version = ManifestVersion;
    QString id;
    QString scopeRelativePath;
    QList<Entry> entries;
};

struct ResolvedEntry
{
    QString path;
    QString oldCopy;
    QString staging;
};

struct ObservedFile
{
    bool exists = false;
    AtomicFileSnapshot contents;
};

struct AnchoredJournalFile
{
    QString name;
    AnchoredFileSystem::EntryRef entry;
    std::shared_ptr<AnchoredFileSystem::PinnedFile> file;
};

} // namespace Detail

struct JournalState
{
    QString athleteRoot;
    QString scopePath;
    QString namespacePath;
    QString journalPath;
    QString manifestPath;
    QString commitMarkerPath;
    Detail::Manifest manifest;
    AtomicFileSnapshot manifestSnapshot;
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    Detail::AnchoredJournalFile manifestFile;
    Detail::AnchoredJournalFile commitMarkerFile;
    QList<Detail::AnchoredJournalFile> trackedDataFiles;
    QList<QPair<QString, AtomicFileSnapshot>> inputSnapshots;
    std::unique_ptr<AtomicFileLockSet> transactionLease;
    std::unique_ptr<AtomicFileLockSet> pathLocks;
    bool cleanupComplete = false;
};

namespace {

using Detail::Entry;
using Detail::AnchoredJournalFile;
using Detail::Manifest;
using Detail::ObservedFile;
using Detail::ResolvedEntry;

void appendError(QString &error, const QString &detail)
{
    if (detail.isEmpty()) return;
    if (!error.isEmpty()) error += QStringLiteral("; ");
    error += detail;
}

bool pathEntryExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool directorySnapshotsMatch(
    const QList<AnchoredFileSystem::DirectoryEntry> &left,
    const QList<AnchoredFileSystem::DirectoryEntry> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const AnchoredFileSystem::DirectoryEntry &leftEntry = left.at(index);
        const AnchoredFileSystem::DirectoryEntry &rightEntry = right.at(index);
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

bool validTransactionId(const QString &id)
{
    if (id.size() != 36 || id != id.toLower()) return false;
    const QUuid uuid(id);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == id;
}

QString oldCopyName(int index)
{
    return QStringLiteral("old-%1.copy")
        .arg(index, 4, 10, QLatin1Char('0'));
}

QString stagingName(int index)
{
    return QStringLiteral("new-%1.stage")
        .arg(index, 4, 10, QLatin1Char('0'));
}

QString transactionNamespacePath(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral(".gc-transactions/plan-replacement"));
}

QString transactionLeaseTarget(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral("linked-removal-transaction"));
}

bool normalizeAthleteRoot(
    const QString &candidate, QString &root, QString &error)
{
    error.clear();
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate)) {
        error = QStringLiteral("The athlete root must be an absolute path");
        return false;
    }

    const QString absolute = QDir::cleanPath(
        QFileInfo(candidate).absoluteFilePath());
    const QFileInfo info(absolute);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        error = QStringLiteral(
            "The athlete root is unavailable or uses a symbolic link");
        return false;
    }

    const QString canonical = QDir::cleanPath(info.canonicalFilePath());
    if (canonical.isEmpty()
        || atomicFilePathKey(canonical) != atomicFilePathKey(absolute)) {
        error = QStringLiteral(
            "The athlete root contains a symbolic-link component");
        return false;
    }

    root = canonical;
    return true;
}

bool validateRelativePath(const QString &path, QString &error)
{
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.isEmpty() || normalized == QStringLiteral(".")
        || QDir::isAbsolutePath(normalized)
        || normalized != QDir::cleanPath(normalized)) {
        error = QStringLiteral("The journal contains an invalid relative path");
        return false;
    }

    const QStringList components = normalized.split(
        QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &component : components) {
        if (component.isEmpty() || component == QStringLiteral(".")
            || component == QStringLiteral("..")) {
            error = QStringLiteral(
                "The journal contains a path-traversal component");
            return false;
        }
    }
    if (components.constFirst().compare(
            QStringLiteral(".gc-transactions"),
            Qt::CaseInsensitive) == 0) {
        error = QStringLiteral(
            "A plan path overlaps the transaction namespace");
        return false;
    }
    return true;
}

bool validateScopeRelativePath(
    const QString &path,
    QString &error)
{
    if (QDir::fromNativeSeparators(path)
        == QStringLiteral(".")) {
        return true;
    }
    return validateRelativePath(path, error);
}

bool validatePathComponents(
    const QString &root, const QString &relativePath, QString &error)
{
    QString cursor = root;
    const QStringList components = QDir::fromNativeSeparators(relativePath)
                                       .split(QLatin1Char('/'));
    for (int index = 0; index < components.size(); ++index) {
        cursor = QDir(cursor).filePath(components.at(index));
        const QFileInfo info(cursor);
        if (info.isSymLink()) {
            error = QStringLiteral(
                "A plan transaction path uses a symbolic-link component");
            return false;
        }
        if (index + 1 < components.size()
            && (!info.exists() || !info.isDir())) {
            error = QStringLiteral(
                "A plan transaction path has an unavailable parent directory");
            return false;
        }
    }
    return true;
}

bool makeRootRelativePath(
    const QString &root,
    const QString &candidate,
    QString &relativePath,
    QString &absolutePath,
    QString &error)
{
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate)) {
        error = QStringLiteral("Plan transaction paths must be absolute");
        return false;
    }

    absolutePath = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    relativePath = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(absolutePath));
    if (!validateRelativePath(relativePath, error)) return false;

    const QString resolved = QDir::cleanPath(
        QDir(root).filePath(relativePath));
    if (atomicFilePathKey(resolved) != atomicFilePathKey(absolutePath)) {
        error = QStringLiteral("A plan path escapes the athlete root");
        return false;
    }
    return validatePathComponents(root, relativePath, error);
}

bool makeScopeRelativePath(
    const QString &root,
    const QString &candidate,
    QString &relativePath,
    QString &absolutePath,
    QString &error)
{
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate)) {
        error = QStringLiteral("Plan transaction paths must be absolute");
        return false;
    }
    absolutePath = QDir::cleanPath(
        QFileInfo(candidate).absoluteFilePath());
    if (atomicFilePathKey(absolutePath)
        == atomicFilePathKey(root)) {
        relativePath = QStringLiteral(".");
        return true;
    }
    return makeRootRelativePath(
        root, candidate, relativePath, absolutePath, error);
}

bool resolveRootRelativePath(
    const QString &root,
    const QString &relativePath,
    QString &absolutePath,
    QString &error)
{
    if (!validateRelativePath(relativePath, error)) return false;
    absolutePath = QDir::cleanPath(QDir(root).filePath(relativePath));
    const QString roundTrip = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(absolutePath));
    if (roundTrip != QDir::fromNativeSeparators(relativePath)
        || !validatePathComponents(root, relativePath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("A plan path escapes the athlete root");
        }
        return false;
    }
    return true;
}

bool resolveScopeRelativePath(
    const QString &root,
    const QString &relativePath,
    QString &absolutePath,
    QString &error)
{
    if (QDir::fromNativeSeparators(relativePath)
        == QStringLiteral(".")) {
        absolutePath = root;
        return true;
    }
    return resolveRootRelativePath(
        root, relativePath, absolutePath, error);
}

bool pathIsWithinScope(
    const QString &relativePath, const QString &scopeRelativePath)
{
    if (QDir::fromNativeSeparators(scopeRelativePath)
        == QStringLiteral(".")) {
        return true;
    }
    return QDir::fromNativeSeparators(relativePath).startsWith(
        QDir::fromNativeSeparators(scopeRelativePath) + QLatin1Char('/'),
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
        Qt::CaseInsensitive
#else
        Qt::CaseSensitive
#endif
    );
}

QString relativePathKey(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(path);
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

bool validateExistingDirectory(const QString &path, QString &error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        error = QStringLiteral(
            "The plan transaction directory is unavailable or unsafe");
        return false;
    }
    return true;
}

bool openOrCreatePrivateFixedDirectory(
    const AnchoredFileSystem::DirectoryAnchor &parent,
    const QString &component,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error)
{
    if (!AnchoredFileSystem::validateCurrentUserControlledDirectory(
            parent, error)) {
        return false;
    }
    bool exists = false;
    if (!parent.openChildIfExists(
            component, directory, exists, error)) {
        return false;
    }
    if (exists) {
        if (!AnchoredFileSystem::validateCurrentUserOwnedDirectory(
                directory, error)
            || !AnchoredFileSystem::hardenPrivateDirectory(
                directory, error)) {
            return false;
        }
    } else {
        const AnchoredFileSystem::MutationResult creation =
            AnchoredFileSystem::createPrivateFixedChildDirectory(
                parent, component, directory);
        if (creation.effect
            != AnchoredFileSystem::MutationEffect::AppliedDurable) {
            error = creation.error.isEmpty()
                ? QStringLiteral(
                      "Cannot create a private plan transaction directory")
                : creation.error;
            if (!creation.verifiedRecoveryPath.isEmpty()) {
                appendError(
                    error,
                    QStringLiteral("recovery directory retained at %1")
                        .arg(creation.verifiedRecoveryPath));
            }
            return false;
        }
    }

    QString matchError;
    if (!parent.pathMatches(matchError)
        || !directory.pathMatches(matchError)) {
        error = matchError.isEmpty()
            ? QStringLiteral(
                  "The private plan transaction directory hierarchy changed")
            : matchError;
        return false;
    }
    return true;
}

bool openExistingPrivateDirectory(
    const AnchoredFileSystem::DirectoryAnchor &parent,
    const QString &component,
    AnchoredFileSystem::DirectoryAnchor &directory,
    bool &exists,
    QString &error,
    const AnchoredFileSystem::NativeIdentity *expectedIdentity = nullptr)
{
    if (!AnchoredFileSystem::validateCurrentUserControlledDirectory(
            parent, error)
        || !parent.openChildIfExists(
            component, directory, exists, error)) {
        return false;
    }
    if (!exists) return true;
    if (expectedIdentity
        && (directory.identity() != *expectedIdentity
            || directory.identity().linkCount()
                != expectedIdentity->linkCount())) {
        error = QStringLiteral(
            "The private plan transaction directory changed after enumeration");
        directory = {};
        return false;
    }
    if (!AnchoredFileSystem::validateCurrentUserOwnedDirectory(
            directory, error)
        || !AnchoredFileSystem::hardenPrivateDirectory(
            directory, error)) {
        return false;
    }

    QString matchError;
    if (!parent.pathMatches(matchError)
        || !directory.pathMatches(matchError)) {
        error = matchError.isEmpty()
            ? QStringLiteral(
                  "The private plan transaction directory hierarchy changed")
            : matchError;
        return false;
    }
    return true;
}

bool ensureTransactionNamespace(
    const QString &root,
    QString &namespacePath,
    AnchoredFileSystem::DirectoryAnchor &transactionsDirectory,
    AnchoredFileSystem::DirectoryAnchor &namespaceDirectory,
    QString &error)
{
    transactionsDirectory = {};
    namespaceDirectory = {};
    AnchoredFileSystem::DirectoryAnchor rootDirectory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            root, rootDirectory, error)) {
        error = QStringLiteral(
            "Cannot anchor the plan transaction root: %1").arg(error);
        return false;
    }
    if (!openOrCreatePrivateFixedDirectory(
            rootDirectory,
            QStringLiteral(".gc-transactions"),
            transactionsDirectory,
            error)) {
        error = QStringLiteral(
            "Cannot prepare the plan transaction directory: %1")
                    .arg(error);
        return false;
    }

    namespacePath = transactionNamespacePath(root);
    if (!openOrCreatePrivateFixedDirectory(
            transactionsDirectory,
            QStringLiteral("plan-replacement"),
            namespaceDirectory,
            error)) {
        error = QStringLiteral(
            "Cannot prepare the plan-replacement namespace: %1")
                    .arg(error);
        return false;
    }
    return true;
}

bool transactionNamespacesAreReady(
    const AnchoredFileSystem::DirectoryAnchor &transactionsDirectory,
    const AnchoredFileSystem::DirectoryAnchor &replacementNamespace,
    QString &error)
{
    QList<AnchoredFileSystem::DirectoryAnchor> namespaces {
        replacementNamespace};
    const QStringList otherComponents = {
        QStringLiteral("linked-save"),
        QStringLiteral("linked-removal")};
    for (const QString &component : otherComponents) {
        AnchoredFileSystem::DirectoryAnchor directory;
        bool exists = false;
        if (!openExistingPrivateDirectory(
                transactionsDirectory,
                component,
                directory,
                exists,
                error)) {
            return false;
        }
        if (exists) namespaces.append(directory);
    }

#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-readiness-namespaces-anchored");
#endif

    for (const AnchoredFileSystem::DirectoryAnchor &candidate : namespaces) {
        QString matchError;
        if (!transactionsDirectory.pathMatches(matchError)
            || !candidate.pathMatches(matchError)) {
            error = matchError.isEmpty()
                ? QStringLiteral(
                      "An activity transaction namespace was replaced")
                : matchError;
            return false;
        }
        QList<AnchoredFileSystem::DirectoryEntry> entries;
        if (!candidate.enumerateEntries(
                entries, Detail::MaximumNamespaceEntries, error)) {
            error = QStringLiteral(
                "Cannot inspect an activity transaction namespace: %1")
                        .arg(error);
            return false;
        }
        for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
            QString lockedId;
            if (entry.kind
                    == AnchoredFileSystem::DirectoryEntryKind::RegularFile
                && entry.identity.linkCount() == 1
                && atomicFileLockTargetName(entry.name, lockedId)
                && validTransactionId(lockedId)) {
                continue;
            }
            error = QStringLiteral(
                "Pending activity recovery must be completed before replacing a plan");
            return false;
        }
        if (!transactionsDirectory.pathMatches(matchError)
            || !candidate.pathMatches(matchError)) {
            error = matchError.isEmpty()
                ? QStringLiteral(
                      "An activity transaction namespace changed while being inspected")
                : matchError;
            return false;
        }
    }
    QString matchError;
    if (!transactionsDirectory.pathMatches(matchError)) {
        error = matchError.isEmpty()
            ? QStringLiteral(
                  "The activity transaction directory changed during readiness inspection")
            : matchError;
        return false;
    }
    for (const AnchoredFileSystem::DirectoryAnchor &candidate : namespaces) {
        if (!candidate.pathMatches(matchError)) {
            error = matchError.isEmpty()
                ? QStringLiteral(
                      "An activity transaction namespace changed during readiness inspection")
                : matchError;
            return false;
        }
    }
    return true;
}

bool journalDirectoryMatches(
    const JournalState &state, QString &error)
{
    if (!state.namespaceDirectory.isValid()
        || !state.journalDirectory.isValid()) {
        error = QStringLiteral(
            "The plan-replacement journal is no longer anchored");
        return false;
    }
    if (!state.namespaceDirectory.pathMatches(error)
        || !state.journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The plan-replacement journal directory was replaced");
        }
        return false;
    }
    return true;
}

bool pinAnchoredJournalFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const QString &name,
    qint64 maximumSize,
    AnchoredJournalFile &anchored,
    QString &error,
    const AnchoredFileSystem::NativeIdentity *expectedIdentity = nullptr)
{
    anchored = {};
    anchored.name = name;
    anchored.entry = directory.entry(name, error);
    anchored.file =
        std::make_shared<AnchoredFileSystem::PinnedFile>();
    if (!anchored.entry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            anchored.entry,
            *anchored.file,
            error,
            maximumSize)) {
        return false;
    }
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            anchored.entry,
            *anchored.file,
            matches,
            error)
        || !matches
        || anchored.file->identity().linkCount() != 1
        || (expectedIdentity
            && (anchored.file->identity() != *expectedIdentity
                || anchored.file->identity().linkCount()
                    != expectedIdentity->linkCount()))
        || !directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan-replacement journal file was replaced while being pinned");
        }
        return false;
    }
    return true;
}

const AnchoredJournalFile *findAnchoredJournalFile(
    const QList<AnchoredJournalFile> &files,
    const QString &name)
{
    for (const AnchoredJournalFile &file : files) {
        if (file.name == name) return &file;
    }
    return nullptr;
}

bool anchoredJournalFileMatches(
    const AnchoredJournalFile *file,
    const AtomicFileSnapshot &expected)
{
    return file && file->file && file->file->isValid()
        && file->file->size() == expected.size
        && file->file->sha256() == expected.digest;
}

bool anchoredJournalFileIsCurrent(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const AnchoredJournalFile &anchored,
    QString &error);

bool removeAnchoredJournalFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const AnchoredJournalFile &anchored,
    QString &error)
{
    if (!anchoredJournalFileIsCurrent(
            directory, anchored, error)) {
        return false;
    }
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(*anchored.file);
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        if (directory.pathMatches(error)) return true;
    } else if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        QString matchError;
        if (directory.sync(syncError)
            && directory.pathMatches(matchError)) {
            return true;
        }
        error = removal.error;
        appendError(error, syncError);
        appendError(error, matchError);
        return false;
    }
    if (error.isEmpty()) {
        error = removal.error.isEmpty()
            ? QStringLiteral(
                  "Cannot remove an anchored plan transaction file")
            : removal.error;
    }
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(removal.verifiedRecoveryPath));
    }
    return false;
}

bool anchoredJournalFileIsCurrent(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const AnchoredJournalFile &anchored,
    QString &error)
{
    if (!anchored.entry.isValid() || !anchored.file
        || !anchored.file->isValid()) {
        error = QStringLiteral(
            "A plan-replacement journal file is no longer anchored");
        return false;
    }
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            anchored.entry,
            *anchored.file,
            matches,
            error)
        || !matches
        || !directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan-replacement journal file was replaced");
        }
        return false;
    }
    return true;
}

bool trackedJournalFilesAreCurrent(
    const JournalState &state, QString &error)
{
    for (const AnchoredJournalFile &file : state.trackedDataFiles) {
        if (!anchoredJournalFileIsCurrent(
                state.journalDirectory, file, error)) {
            return false;
        }
    }
    return true;
}

bool readAnchoredControlFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const QString &name,
    qint64 maximumSize,
    QByteArray &contents,
    AtomicFileSnapshot &snapshot,
    AnchoredJournalFile &anchored,
    QString &error,
    const char *transition = nullptr)
{
    contents.clear();
    snapshot = {};
    if (!pinAnchoredJournalFile(
            directory, name, maximumSize, anchored, error)
        || !AnchoredFileSystem::readAll(
            *anchored.file,
            maximumSize,
            contents,
            error)) {
        return false;
    }
    snapshot.size = anchored.file->size();
    snapshot.digest = anchored.file->sha256();
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    if (transition) planReplacementTransitionReached(transition);
#else
    Q_UNUSED(transition)
#endif
    return anchoredJournalFileIsCurrent(
        directory, anchored, error);
}

bool inspectRegularFile(
    const QString &path,
    ObservedFile &observed,
    QString &error,
    qint64 maximumSize = -1)
{
    observed = {};
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return true;
    if (info.isSymLink() || !info.isFile()) {
        error = QStringLiteral(
            "A plan transaction path is not a regular file: %1").arg(path);
        return false;
    }
    if (maximumSize >= 0 && info.size() > maximumSize) {
        error = QStringLiteral(
            "A plan transaction file is unexpectedly large: %1").arg(path);
        return false;
    }
    if (!captureAtomicFileSnapshot(path, observed.contents, error)) {
        return false;
    }
    observed.exists = true;
    return true;
}

bool snapshotMatches(
    const ObservedFile &observed,
    const AtomicFileSnapshot &expected)
{
    return observed.exists
        && observed.contents.size == expected.size
        && observed.contents.digest == expected.digest;
}

bool validateExpectedSnapshot(
    const QString &path,
    bool expectedExists,
    const AtomicFileSnapshot &expected,
    QString &error)
{
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    if (observed.exists != expectedExists
        || (expectedExists && !snapshotMatches(observed, expected))) {
        error = QStringLiteral(
            "A plan transaction file changed unexpectedly: %1").arg(path);
        return false;
    }
    return true;
}

bool writeBytesAtomically(
    const QString &path,
    const QByteArray &contents,
    AtomicFileMode mode,
    AtomicFileSnapshot &snapshot,
    QString &error,
    const AtomicPreCommitValidation &validateBeforeCommit = {})
{
    if (!writeFileAtomically(
            path, contents, qSaveFileWriterFactory(), error,
            mode == AtomicFileMode::ReplaceExisting,
            false,
            validateBeforeCommit)) {
        return false;
    }
    if (!syncParentDirectory(path, error)) return false;
    return captureAtomicFileSnapshot(path, snapshot, error);
}

bool copyExpectedPinnedFileAtomically(
    const AnchoredFileSystem::DirectoryAnchor &sourceDirectory,
    const AnchoredJournalFile &source,
    const QString &targetPath,
    const AtomicFileSnapshot &expected,
    AtomicFileMode mode,
    QString &error)
{
    if (!anchoredJournalFileMatches(&source, expected)
        || !anchoredJournalFileIsCurrent(
            sourceDirectory, source, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan transaction source changed unexpectedly");
        }
        return false;
    }

    std::unique_ptr<AtomicFileWriter> writer =
        qSaveFileWriterFactory()(targetPath, mode);
    if (!writer || !writer->open()) {
        error = writer
            ? atomicFileError(
                  QStringLiteral("Cannot open a plan transaction target"),
                  *writer)
            : QStringLiteral("Cannot create a plan transaction writer");
        return false;
    }

    const bool copied = AnchoredFileSystem::streamContents(
        *source.file,
        [&writer](const char *data, qsizetype size, QString &streamError) {
            const QByteArray chunk = QByteArray::fromRawData(
                data, int(size));
            if (writer->write(chunk) == size) return true;
            streamError = atomicFileError(
                QStringLiteral("Cannot copy a complete plan transaction file"),
                *writer);
            return false;
        },
        error);
    if (!copied || !writer->flush()) {
        if (error.isEmpty()) {
            error = writer->errorString().isEmpty()
                ? QStringLiteral(
                      "Cannot flush a complete plan transaction file")
                : writer->errorString();
        }
        writer->cancelWriting();
        return false;
    }
    if (!anchoredJournalFileIsCurrent(
            sourceDirectory, source, error)) {
        writer->cancelWriting();
        return false;
    }
    if (!writer->commit()) {
        error = atomicFileError(
            QStringLiteral("Cannot publish a plan transaction file"),
            *writer);
        return false;
    }
    if (!syncParentDirectory(targetPath, error)
        || !atomicFileMatchesSnapshot(targetPath, expected, error)
        || !anchoredJournalFileIsCurrent(
            sourceDirectory, source, error)) {
        return false;
    }
    return true;
}

bool copyExpectedPathToAnchoredJournal(
    const QString &sourcePath,
    const AtomicFileSnapshot &expected,
    const AnchoredFileSystem::DirectoryAnchor &journalDirectory,
    const QString &journalName,
    AnchoredJournalFile &anchored,
    QString &error)
{
    anchored = {};
    const QFileInfo sourceInfo(sourcePath);
    AnchoredFileSystem::DirectoryAnchor sourceDirectory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            sourceInfo.absolutePath(), sourceDirectory, error)
        || !sourceDirectory.pathMatches(error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef sourceEntry =
        sourceDirectory.entry(sourceInfo.fileName(), error);
    AnchoredFileSystem::PinnedFile source;
    if (!sourceEntry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            sourceEntry, source, error, expected.size)
        || source.size() != expected.size
        || source.sha256() != expected.digest) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan transaction source changed unexpectedly");
        }
        return false;
    }
    bool sourceMatches = false;
    if (!AnchoredFileSystem::entryMatches(
            sourceEntry, source, sourceMatches, error)
        || !sourceMatches
        || !sourceDirectory.pathMatches(error)
        || !journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A plan transaction source or journal was replaced");
        }
        return false;
    }

    const AnchoredFileSystem::EntryRef destination =
        journalDirectory.entry(journalName, error);
    AnchoredFileSystem::PinnedFile copy;
    if (!destination.isValid()
        || !AnchoredFileSystem::copyToNewFile(
            source, destination, copy, error)
        || copy.size() != expected.size
        || copy.sha256() != expected.digest) {
        return false;
    }
    anchored.name = journalName;
    anchored.entry = destination;
    anchored.file =
        std::make_shared<AnchoredFileSystem::PinnedFile>(
            std::move(copy));
    if (anchored.file->identity().linkCount() != 1
        || !anchoredJournalFileIsCurrent(
            journalDirectory, anchored, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An anchored plan journal copy was replaced");
        }
        return false;
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

QJsonObject entryToJson(const Entry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("path"), entry.relativePath);
    object.insert(QStringLiteral("old_exists"), entry.oldExists);
    object.insert(
        QStringLiteral("old_size"),
        QString::number(entry.oldExists ? entry.oldContents.size : 0));
    object.insert(
        QStringLiteral("old_sha256"),
        entry.oldExists
            ? QString::fromLatin1(entry.oldContents.digest.toHex())
            : QString());
    object.insert(QStringLiteral("has_new"), entry.hasNew);
    object.insert(QStringLiteral("stage_index"), entry.stageIndex);
    object.insert(QStringLiteral("staged"), entry.staged);
    object.insert(
        QStringLiteral("new_size"),
        QString::number(entry.staged ? entry.newContents.size : 0));
    object.insert(
        QStringLiteral("new_sha256"),
        entry.staged
            ? QString::fromLatin1(entry.newContents.digest.toHex())
            : QString());
    return object;
}

QByteArray serializeManifest(const Manifest &manifest)
{
    QJsonArray entries;
    for (const Entry &entry : manifest.entries) {
        entries.append(entryToJson(entry));
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), manifest.version);
    root.insert(QStringLiteral("id"), manifest.id);
    root.insert(QStringLiteral("scope"), manifest.scopeRelativePath);
    root.insert(QStringLiteral("entries"), entries);
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

bool parseSizeAndDigest(
    const QJsonObject &object,
    const QString &sizeKey,
    const QString &digestKey,
    bool exists,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    const QJsonValue sizeValue = object.value(sizeKey);
    const QJsonValue digestValue = object.value(digestKey);
    if (!sizeValue.isString() || !digestValue.isString()) {
        error = QStringLiteral("A plan manifest snapshot has invalid types");
        return false;
    }
    const QString sizeText = sizeValue.toString();
    static const QRegularExpression sizeExpression(
        QStringLiteral("^(0|[1-9][0-9]*)$"));
    bool sizeValid = false;
    const qint64 size = sizeText.toLongLong(&sizeValid);
    const QString digestText = digestValue.toString();
    static const QRegularExpression digestExpression(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!sizeValid || size < 0
        || (exists && !digestExpression.match(digestText).hasMatch())
        || (!exists && (size != 0 || !digestText.isEmpty()))) {
        error = QStringLiteral("A plan manifest snapshot is invalid");
        return false;
    }
    snapshot.size = size;
    snapshot.digest = exists
        ? QByteArray::fromHex(digestText.toLatin1())
        : QByteArray();
    return true;
}

bool parseEntry(
    const QJsonValue &value, Entry &entry, QString &error)
{
    if (!value.isObject()) {
        error = QStringLiteral("A plan manifest entry is not an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    static const QSet<QString> keys = {
        QStringLiteral("path"),
        QStringLiteral("old_exists"),
        QStringLiteral("old_size"),
        QStringLiteral("old_sha256"),
        QStringLiteral("has_new"),
        QStringLiteral("stage_index"),
        QStringLiteral("staged"),
        QStringLiteral("new_size"),
        QStringLiteral("new_sha256")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("path")).isString()
        || !object.value(QStringLiteral("old_exists")).isBool()
        || !object.value(QStringLiteral("has_new")).isBool()
        || !object.value(QStringLiteral("stage_index")).isDouble()
        || !object.value(QStringLiteral("staged")).isBool()) {
        error = QStringLiteral("A plan manifest entry has an invalid schema");
        return false;
    }

    entry = {};
    entry.relativePath = object.value(QStringLiteral("path")).toString();
    entry.oldExists = object.value(QStringLiteral("old_exists")).toBool();
    entry.hasNew = object.value(QStringLiteral("has_new")).toBool();
    const double stageNumber =
        object.value(QStringLiteral("stage_index")).toDouble();
    entry.staged = object.value(QStringLiteral("staged")).toBool();
    if (!std::isfinite(stageNumber)
        || std::floor(stageNumber) != stageNumber
        || stageNumber < -1
        || stageNumber >= Detail::MaximumEntries
        || (!entry.oldExists && !entry.hasNew)
        || (!entry.hasNew && (stageNumber != -1 || entry.staged))) {
        error = QStringLiteral("A plan manifest staging index is invalid");
        return false;
    }
    entry.stageIndex = static_cast<int>(stageNumber);
    return validateRelativePath(entry.relativePath, error)
        && parseSizeAndDigest(
            object,
            QStringLiteral("old_size"),
            QStringLiteral("old_sha256"),
            entry.oldExists,
            entry.oldContents,
            error)
        && parseSizeAndDigest(
            object,
            QStringLiteral("new_size"),
            QStringLiteral("new_sha256"),
            entry.staged,
            entry.newContents,
            error);
}

bool parseManifest(
    const QByteArray &contents,
    const QString &expectedId,
    Manifest &manifest,
    QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        contents, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral("The plan transaction manifest is malformed");
        return false;
    }
    const QJsonObject object = document.object();
    static const QSet<QString> keys = {
        QStringLiteral("version"),
        QStringLiteral("id"),
        QStringLiteral("scope"),
        QStringLiteral("entries")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("version")).isDouble()
        || !object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("scope")).isString()
        || !object.value(QStringLiteral("entries")).isArray()) {
        error = QStringLiteral("The plan transaction manifest schema is invalid");
        return false;
    }

    manifest = {};
    manifest.version = object.value(QStringLiteral("version")).toInt(-1);
    manifest.id = object.value(QStringLiteral("id")).toString();
    manifest.scopeRelativePath = object.value(QStringLiteral("scope")).toString();
    const QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    if (manifest.version != Detail::ManifestVersion
        || manifest.id != expectedId
        || !validTransactionId(manifest.id)
        || entries.isEmpty()
        || entries.size() > Detail::MaximumEntries
        || !validateScopeRelativePath(
            manifest.scopeRelativePath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("The plan transaction manifest is invalid");
        }
        return false;
    }

    QSet<QString> pathKeys;
    QSet<int> stageIndices;
    for (const QJsonValue &value : entries) {
        Entry entry;
        if (!parseEntry(value, entry, error)
            || !pathIsWithinScope(
                entry.relativePath, manifest.scopeRelativePath)) {
            if (error.isEmpty()) {
                error = QStringLiteral("A plan path escapes its declared scope");
            }
            return false;
        }
        const QString key = relativePathKey(entry.relativePath);
        if (pathKeys.contains(key)) {
            error = QStringLiteral("The plan manifest contains duplicate paths");
            return false;
        }
        pathKeys.insert(key);
        if (entry.hasNew) {
            if (stageIndices.contains(entry.stageIndex)) {
                error = QStringLiteral(
                    "The plan manifest contains duplicate staging indices");
                return false;
            }
            stageIndices.insert(entry.stageIndex);
        }
        manifest.entries.append(entry);
    }
    if (stageIndices.isEmpty()) {
        error = QStringLiteral("A plan replacement has no target activities");
        return false;
    }
    for (int index = 0; index < stageIndices.size(); ++index) {
        if (!stageIndices.contains(index)) {
            error = QStringLiteral(
                "The plan manifest staging indices are not contiguous");
            return false;
        }
    }
    return true;
}

bool writeManifestFile(
    JournalState &state, bool replace, QString &error)
{
    const QByteArray contents = serializeManifest(state.manifest);
    if (contents.size() > Detail::MaximumManifestSize) {
        error = QStringLiteral("The plan transaction manifest is too large");
        return false;
    }
    if (!journalDirectoryMatches(state, error)
        || !trackedJournalFilesAreCurrent(state, error)) {
        return false;
    }

    const AnchoredJournalFile previous = state.manifestFile;
    const AnchoredFileSystem::EntryRef manifestEntry =
        state.journalDirectory.entry(Detail::ManifestName, error);
    if (!manifestEntry.isValid()) return false;
    if (!replace) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                manifestEntry, exists, error)) {
            return false;
        }
        if (exists) {
            error = QStringLiteral(
                "The plan transaction manifest appeared concurrently");
            return false;
        }
        AnchoredFileSystem::PinnedFile published;
        if (!AnchoredFileSystem::writeNewFile(
                contents, manifestEntry, published, error)) {
            return false;
        }
        AnchoredJournalFile manifest;
        manifest.name = Detail::ManifestName;
        manifest.entry = manifestEntry;
        manifest.file =
            std::make_shared<AnchoredFileSystem::PinnedFile>(
                std::move(published));
        const AtomicFileSnapshot expected = {
            static_cast<qint64>(contents.size()),
            QCryptographicHash::hash(
                contents, QCryptographicHash::Sha256)};
        if (!anchoredJournalFileMatches(&manifest, expected)
            || !anchoredJournalFileIsCurrent(
                state.journalDirectory, manifest, error)
            || !trackedJournalFilesAreCurrent(state, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The published plan transaction manifest changed unexpectedly");
            }
            return false;
        }
        state.manifestSnapshot = expected;
        state.manifestFile = std::move(manifest);
        return true;
    }

    const AtomicPreCommitValidation validateBeforeCommit =
        [&state, previous](QString &validationError) {
            if (!journalDirectoryMatches(state, validationError)) {
                return false;
            }
            return previous.name == Detail::ManifestName
                && anchoredJournalFileIsCurrent(
                    state.journalDirectory,
                    previous,
                    validationError)
                && trackedJournalFilesAreCurrent(
                    state, validationError);
        };

    AtomicFileSnapshot written;
    if (!writeBytesAtomically(
        state.manifestPath,
        contents,
        AtomicFileMode::ReplaceExisting,
        written,
        error,
        validateBeforeCommit)) {
        return false;
    }
    AnchoredJournalFile manifest;
    if (!journalDirectoryMatches(state, error)
        || !pinAnchoredJournalFile(
            state.journalDirectory,
            Detail::ManifestName,
            Detail::MaximumManifestSize,
            manifest,
            error)
        || manifest.file->size() != contents.size()
        || manifest.file->sha256()
            != QCryptographicHash::hash(
                contents, QCryptographicHash::Sha256)
        || !trackedJournalFilesAreCurrent(state, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The published plan transaction manifest changed unexpectedly");
        }
        return false;
    }
    state.manifestSnapshot = written;
    state.manifestFile = std::move(manifest);
    return true;
}

bool resolveManifestEntries(
    const JournalState &state,
    QList<ResolvedEntry> &resolved,
    QString &error)
{
    resolved.clear();
    QString scopePath;
    if (!resolveScopeRelativePath(
            state.athleteRoot,
            state.manifest.scopeRelativePath,
            scopePath,
            error)
        || !validateExistingDirectory(scopePath, error)) {
        return false;
    }
    const QString canonicalScope = QDir::cleanPath(
        QFileInfo(scopePath).canonicalFilePath());
    if (canonicalScope.isEmpty()
        || atomicFilePathKey(canonicalScope)
            != atomicFilePathKey(scopePath)) {
        error = QStringLiteral("The plan transaction scope is unsafe");
        return false;
    }

    QSet<QString> pathKeys;
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        ResolvedEntry paths;
        if (!pathIsWithinScope(
                entry.relativePath,
                state.manifest.scopeRelativePath)
            || !resolveRootRelativePath(
                state.athleteRoot,
                entry.relativePath,
                paths.path,
                error)) {
            if (error.isEmpty()) {
                error = QStringLiteral("A plan path escapes its declared scope");
            }
            return false;
        }
        const QString pathKey = atomicFilePathKey(paths.path);
        if (pathKeys.contains(pathKey)) {
            error = QStringLiteral("The plan transaction paths overlap");
            return false;
        }
        pathKeys.insert(pathKey);
        paths.oldCopy = QDir(state.journalPath).filePath(oldCopyName(index));
        if (entry.hasNew) {
            paths.staging = QDir(state.journalPath).filePath(
                stagingName(entry.stageIndex));
        }
        resolved.append(paths);
    }
    return true;
}

QStringList productionLockPaths(const QList<ResolvedEntry> &entries)
{
    QStringList paths;
    paths.reserve(entries.size());
    for (const ResolvedEntry &entry : entries) paths.append(entry.path);
    return paths;
}

bool loadManifestState(
    const QString &root,
    const QString &journalPath,
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory,
    AnchoredFileSystem::DirectoryAnchor journalDirectory,
    std::shared_ptr<JournalState> &state,
    QString &error)
{
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)) {
        error = QStringLiteral("A plan transaction has an invalid identifier");
        return false;
    }
    const QString expectedNamespace = transactionNamespacePath(root);
    if (atomicFilePathKey(QFileInfo(journalPath).absolutePath())
            != atomicFilePathKey(expectedNamespace)
        || !namespaceDirectory.isValid()
        || !journalDirectory.isValid()
        || !namespaceDirectory.pathMatches(error)
        || !journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared plan transaction is outside its anchored namespace");
        }
        return false;
    }

    const QString manifestPath = QDir(journalPath).filePath(
        Detail::ManifestName);
    QByteArray contents;
    AtomicFileSnapshot manifestSnapshot;
    AnchoredJournalFile manifestFile;
    if (!readAnchoredControlFile(
            journalDirectory,
            Detail::ManifestName,
            Detail::MaximumManifestSize,
            contents,
            manifestSnapshot,
            manifestFile,
            error,
            "plan-replacement-manifest-read")) {
        return false;
    }
    Manifest manifest;
    if (!parseManifest(contents, id, manifest, error)) return false;

    std::shared_ptr<JournalState> loaded(new JournalState);
    loaded->athleteRoot = root;
    loaded->namespacePath = transactionNamespacePath(root);
    loaded->journalPath = journalPath;
    loaded->manifestPath = manifestPath;
    loaded->commitMarkerPath = QDir(journalPath).filePath(
        Detail::CommitMarkerName);
    loaded->manifest = manifest;
    loaded->manifestSnapshot = manifestSnapshot;
    loaded->namespaceDirectory = std::move(namespaceDirectory);
    loaded->journalDirectory = std::move(journalDirectory);
    loaded->manifestFile = std::move(manifestFile);

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*loaded, resolved, error)) return false;
    loaded->pathLocks = std::make_unique<AtomicFileLockSet>();
    if (!loaded->pathLocks->lock(productionLockPaths(resolved), error)) {
        error = QStringLiteral("A plan activity file is already being changed: %1")
                    .arg(error);
        return false;
    }
    if (!journalDirectoryMatches(*loaded, error)
        || !anchoredJournalFileIsCurrent(
            loaded->journalDirectory,
            loaded->manifestFile,
            error)) {
        return false;
    }
    state = loaded;
    return true;
}

bool writeCommitMarkerFile(
    JournalState &state,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    if (!journalDirectoryMatches(state, error)) return false;
    const QByteArray contents = state.manifest.id.toLatin1() + '\n';
    AnchoredJournalFile marker;
    marker.name = Detail::CommitMarkerName;
    marker.entry = state.journalDirectory.entry(marker.name, error);
    bool exists = false;
    if (!marker.entry.isValid()
        || !AnchoredFileSystem::entryExists(
            marker.entry, exists, error)) {
        return false;
    }
    if (exists) {
        error = QStringLiteral(
            "The plan transaction commit marker already exists");
        return false;
    }

    AnchoredFileSystem::PinnedFile published;
    if (!AnchoredFileSystem::writeNewFile(
            contents, marker.entry, published, error)) {
        return false;
    }
    marker.file = std::make_shared<AnchoredFileSystem::PinnedFile>(
        std::move(published));
    snapshot.size = marker.file->size();
    snapshot.digest = marker.file->sha256();
    const AtomicFileSnapshot expected = {
        static_cast<qint64>(contents.size()),
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256)};
    if (!anchoredJournalFileMatches(&marker, expected)
        || !anchoredJournalFileIsCurrent(
            state.journalDirectory, marker, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The published plan commit marker changed unexpectedly");
        }
        return false;
    }
    state.commitMarkerFile = std::move(marker);
    return true;
}

bool readCommitMarker(
    JournalState &state,
    bool &committed,
    AtomicFileSnapshot *snapshot,
    QString &error)
{
    committed = false;
    state.commitMarkerFile = {};
    if (!journalDirectoryMatches(state, error)) return false;

    const AnchoredFileSystem::EntryRef markerEntry =
        state.journalDirectory.entry(Detail::CommitMarkerName, error);
    bool exists = false;
    if (!markerEntry.isValid()
        || !AnchoredFileSystem::entryExists(
            markerEntry, exists, error)) {
        return false;
    }
    if (!exists) {
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached(
            "plan-replacement-commit-marker-absence-observed");
#endif
        if (!AnchoredFileSystem::entryExists(
                markerEntry, exists, error)
            || !state.journalDirectory.pathMatches(error)) {
            return false;
        }
        if (exists) {
            error = QStringLiteral(
                "The plan transaction commit marker appeared concurrently");
            return false;
        }
        state.commitMarkerFile = {};
        return true;
    }

    QByteArray contents;
    AtomicFileSnapshot observed;
    AnchoredJournalFile marker;
    if (!readAnchoredControlFile(
            state.journalDirectory,
            Detail::CommitMarkerName,
            Detail::MaximumCommitMarkerSize,
            contents,
            observed,
            marker,
            error,
            "plan-replacement-commit-marker-read")) {
        return false;
    }
    if (contents != state.manifest.id.toLatin1() + '\n') {
        error = QStringLiteral("The plan transaction commit marker is invalid");
        return false;
    }
    committed = true;
    if (snapshot) *snapshot = observed;
    state.commitMarkerFile = std::move(marker);
    return true;
}

bool isKnownDataName(const QString &name)
{
    static const QRegularExpression expression(
        QStringLiteral("^(old-[0-9]{4}\\.copy|new-[0-9]{4}\\.stage)$"));
    return expression.match(name).hasMatch();
}

bool isKnownTemporaryName(const QString &name)
{
    static const QRegularExpression expression(
        QStringLiteral(
            "^\\.(manifest\\.json|COMMITTED|old-[0-9]{4}\\.copy|new-[0-9]{4}\\.stage)\\.[A-Za-z0-9]+\\.tmp$"));
    return expression.match(name).hasMatch();
}

qint64 knownTemporaryMaximumSize(const QString &name)
{
    if (name.startsWith(QStringLiteral(".manifest.json."))) {
        return Detail::MaximumManifestSize;
    }
    if (name.startsWith(QStringLiteral(".COMMITTED."))) {
        return Detail::MaximumCommitMarkerSize;
    }
    return -1;
}

bool isKnownLockName(const QString &name)
{
    QString base;
    if (!atomicFileLockTargetName(name, base)) return false;
    return base == Detail::ManifestName
        || base == Detail::CommitMarkerName
        || isKnownDataName(base);
}

bool expectedJournalDataName(
    const JournalState &state, const QString &name)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if ((entry.oldExists && name == oldCopyName(index))
            || (entry.hasNew && name == stagingName(entry.stageIndex))) {
            return true;
        }
    }
    return false;
}

bool inspectJournalDirectory(
    const JournalState &state,
    QList<AnchoredJournalFile> &files,
    QString &error)
{
    files.clear();
    if (!journalDirectoryMatches(state, error)) return false;

    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!state.journalDirectory.enumerateEntries(
            entries, Detail::MaximumJournalEntries, error)) {
        return false;
    }
    for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
        const QString &name = entry.name;
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::RegularFile
            || entry.identity.linkCount() != 1) {
            error = QStringLiteral(
                "The plan transaction journal contains an unsafe entry");
            return false;
        }

        qint64 maximumSize = -1;
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName) {
            maximumSize = name == Detail::ManifestName
                ? Detail::MaximumManifestSize
                : Detail::MaximumCommitMarkerSize;
        } else if (!expectedJournalDataName(state, name)) {
            if (!isKnownTemporaryName(name) && !isKnownLockName(name)) {
                error = QStringLiteral(
                    "The plan transaction journal contains an unknown file");
                return false;
            }
            maximumSize = isKnownLockName(name)
                ? Detail::MaximumLockFileSize
                : knownTemporaryMaximumSize(name);
        }

        AnchoredJournalFile anchored;
        if (!pinAnchoredJournalFile(
                state.journalDirectory,
                name,
                maximumSize,
                anchored,
                error,
                &entry.identity)) {
            return false;
        }
        files.append(std::move(anchored));
    }

#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-journal-inspected");
#endif
    QList<AnchoredFileSystem::DirectoryEntry> verifiedEntries;
    if (!state.journalDirectory.enumerateEntries(
            verifiedEntries, Detail::MaximumJournalEntries, error)
        || !directorySnapshotsMatch(entries, verifiedEntries)
        || !journalDirectoryMatches(state, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The plan transaction journal changed while being inspected");
        }
        return false;
    }

    const AnchoredJournalFile *manifest =
        findAnchoredJournalFile(files, Detail::ManifestName);
    if (!anchoredJournalFileMatches(manifest, state.manifestSnapshot)
        || !state.manifestFile.file
        || !state.manifestFile.file->isValid()
        || !manifest->file
        || manifest->file->identity()
            != state.manifestFile.file->identity()) {
        error = QStringLiteral(
            "The plan transaction manifest changed unexpectedly");
        return false;
    }
    for (const AnchoredJournalFile &tracked : state.trackedDataFiles) {
        const AnchoredJournalFile *observed =
            findAnchoredJournalFile(files, tracked.name);
        if (!observed || !observed->file || !tracked.file
            || observed->file->identity()
                != tracked.file->identity()) {
            error = QStringLiteral(
                "A tracked plan transaction file was replaced");
            return false;
        }
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const AnchoredJournalFile *oldCopy =
            findAnchoredJournalFile(files, oldCopyName(index));
        if (entry.oldExists) {
            if (!anchoredJournalFileMatches(
                    oldCopy, entry.oldContents)) {
                error = QStringLiteral(
                    "A preserved plan activity does not match its journal");
                return false;
            }
        } else if (oldCopy) {
            error = QStringLiteral(
                "An unexpected preserved plan activity exists");
            return false;
        }

        if (!entry.hasNew) continue;
        const AnchoredJournalFile *staged =
            findAnchoredJournalFile(
                files, stagingName(entry.stageIndex));
        if (entry.staged) {
            if (!anchoredJournalFileMatches(
                    staged, entry.newContents)) {
                error = QStringLiteral(
                    "A staged plan activity does not match its journal");
                return false;
            }
        }
    }
    return true;
}

bool removeObservedFile(
    const QString &path,
    const ObservedFile &observed,
    QString &error)
{
    if (!observed.exists) return true;
    ObservedFile current;
    if (!inspectRegularFile(path, current, error)) return false;
    if (!snapshotMatches(current, observed.contents)) {
        error = QStringLiteral(
            "A plan transaction file changed before cleanup");
        return false;
    }
    if (!QFile::remove(path)) {
        error = QStringLiteral("Cannot remove a plan transaction file: %1")
                    .arg(path);
        return false;
    }
    return syncParentDirectory(path, error);
}

bool verifyInputSnapshots(const JournalState &state, QString &error)
{
    for (const auto &input : state.inputSnapshots) {
        if (!validateExpectedSnapshot(
                input.first, true, input.second, error)) {
            error = QStringLiteral("A plan source changed during replacement: %1")
                        .arg(error);
            return false;
        }
    }
    return true;
}

bool allTargetsAreStaged(const JournalState &state, QString &error)
{
    int targetCount = 0;
    for (const Entry &entry : state.manifest.entries) {
        if (!entry.hasNew) continue;
        ++targetCount;
        if (!entry.staged) {
            error = QStringLiteral(
                "Every new plan activity must be staged before publication");
            return false;
        }
    }
    if (targetCount == 0) {
        error = QStringLiteral("A plan replacement has no target activities");
        return false;
    }
    return true;
}

bool verifyOldGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (!validateExpectedSnapshot(
                resolved.at(index).path,
                entry.oldExists,
                entry.oldContents,
                error)) {
            error = QStringLiteral("The existing plan changed: %1").arg(error);
            return false;
        }
    }
    return true;
}

bool verifyNewGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    if (!allTargetsAreStaged(state, error)) return false;
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (!validateExpectedSnapshot(
                resolved.at(index).path,
                entry.hasNew,
                entry.newContents,
                error)) {
            error = QStringLiteral("The committed plan is incomplete: %1")
                        .arg(error);
            return false;
        }
    }
    return true;
}

bool restoreOldGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    const QList<AnchoredJournalFile> &journalFiles,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        ObservedFile current;
        if (!inspectRegularFile(paths.path, current, error)) return false;

        if (entry.oldExists) {
            if (snapshotMatches(current, entry.oldContents)) continue;
            if (current.exists
                && (!entry.hasNew || !entry.staged
                    || !snapshotMatches(current, entry.newContents))) {
                error = QStringLiteral(
                    "A plan activity changed outside the recovery transaction");
                return false;
            }
            const AnchoredJournalFile *oldCopy =
                findAnchoredJournalFile(
                    journalFiles, oldCopyName(index));
            if (!oldCopy
                || !copyExpectedPinnedFileAtomically(
                    state.journalDirectory,
                    *oldCopy,
                    paths.path,
                    entry.oldContents,
                    current.exists
                        ? AtomicFileMode::ReplaceExisting
                        : AtomicFileMode::CreateNew,
                    error)) {
                return false;
            }
        } else {
            if (!current.exists) continue;
            if (!entry.hasNew || !entry.staged
                || !snapshotMatches(current, entry.newContents)) {
                error = QStringLiteral(
                    "An unexpected plan activity blocks recovery");
                return false;
            }
            if (!removeObservedFile(paths.path, current, error)) return false;
        }
    }
    return verifyOldGeneration(state, resolved, error);
}

int entryIndexForStage(const Manifest &manifest, int stageIndex)
{
    for (int index = 0; index < manifest.entries.size(); ++index) {
        const Entry &entry = manifest.entries.at(index);
        if (entry.hasNew && entry.stageIndex == stageIndex) return index;
    }
    return -1;
}

bool installNewEntry(
    const JournalState &state,
    const Entry &entry,
    const ResolvedEntry &paths,
    const AnchoredJournalFile &staged,
    QString &error)
{
    ObservedFile current;
    if (!inspectRegularFile(paths.path, current, error)) return false;
    if (snapshotMatches(current, entry.newContents)) return true;
    if (current.exists
        && (!entry.oldExists
            || !snapshotMatches(current, entry.oldContents))) {
        error = QStringLiteral(
            "A plan target changed outside the replacement transaction");
        return false;
    }
    return copyExpectedPinnedFileAtomically(
        state.journalDirectory,
        staged,
        paths.path,
        entry.newContents,
        current.exists
            ? AtomicFileMode::ReplaceExisting
            : AtomicFileMode::CreateNew,
        error);
}

bool removeOldOnlyEntry(
    const Entry &entry,
    const ResolvedEntry &paths,
    QString &error)
{
    ObservedFile current;
    if (!inspectRegularFile(paths.path, current, error)) return false;
    if (!current.exists) return true;
    if (!entry.oldExists || !snapshotMatches(current, entry.oldContents)) {
        error = QStringLiteral(
            "A plan removal target changed outside the transaction");
        return false;
    }
    return removeObservedFile(paths.path, current, error);
}

bool ensureNewGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    const QList<AnchoredJournalFile> &journalFiles,
    QString &error)
{
    if (!allTargetsAreStaged(state, error)) return false;
    int targetCount = 0;
    for (const Entry &entry : state.manifest.entries) {
        if (entry.hasNew) ++targetCount;
    }
    for (int stageIndex = 0; stageIndex < targetCount; ++stageIndex) {
        const int index = entryIndexForStage(state.manifest, stageIndex);
        const AnchoredJournalFile *staged = index < 0
            ? nullptr
            : findAnchoredJournalFile(
                  journalFiles, stagingName(stageIndex));
        if (index < 0 || !staged
            || !installNewEntry(
                state,
                state.manifest.entries.at(index),
                resolved.at(index),
                *staged,
                error)) {
            if (index < 0 && error.isEmpty()) {
                error = QStringLiteral(
                    "A plan transaction staging entry is missing");
            }
            return false;
        }
    }
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (entry.hasNew) continue;
        if (!removeOldOnlyEntry(entry, resolved.at(index), error)) {
            return false;
        }
    }
    return verifyNewGeneration(state, resolved, error);
}

bool publishNewGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    const QList<AnchoredJournalFile> &journalFiles,
    QString &error)
{
    if (!allTargetsAreStaged(state, error)
        || !verifyInputSnapshots(state, error)
        || !verifyOldGeneration(state, resolved, error)) {
        return false;
    }

    int targetCount = 0;
    for (const Entry &entry : state.manifest.entries) {
        if (entry.hasNew) ++targetCount;
    }
    for (int stageIndex = 0; stageIndex < targetCount; ++stageIndex) {
        const int index = entryIndexForStage(state.manifest, stageIndex);
        const AnchoredJournalFile *staged = index < 0
            ? nullptr
            : findAnchoredJournalFile(
                  journalFiles, stagingName(stageIndex));
        if (index < 0 || !staged
            || !installNewEntry(
                state,
                state.manifest.entries.at(index),
                resolved.at(index),
                *staged,
                error)) {
            if (index < 0 && error.isEmpty()) {
                error = QStringLiteral(
                    "A plan transaction staging entry is missing");
            }
            return false;
        }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached(
            "plan-replacement-target-published");
#endif
    }
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (entry.hasNew) continue;
        if (!removeOldOnlyEntry(entry, resolved.at(index), error)) {
            return false;
        }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached("plan-replacement-old-removed");
#endif
    }
    return verifyNewGeneration(state, resolved, error);
}

bool removeJournalDirectory(
    JournalState &state, bool committed, QString &error)
{
    QList<AnchoredJournalFile> files;
    if (!inspectJournalDirectory(state, files, error)) return false;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-cleanup-files-inspected");
#endif

    bool markerExists = false;
    AtomicFileSnapshot markerSnapshot;
    if (!readCommitMarker(
            state, markerExists, &markerSnapshot, error)) {
        return false;
    }
    if (markerExists != committed) {
        error = QStringLiteral(
            "The plan transaction commit state changed during cleanup");
        return false;
    }

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !(committed
            ? verifyNewGeneration(state, resolved, error)
            : verifyOldGeneration(state, resolved, error))) {
        return false;
    }
    state.trackedDataFiles.clear();

    const auto expectedCleanupName = [&state](const QString &name) {
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName) {
            return true;
        }
        for (int index = 0;
             index < state.manifest.entries.size();
             ++index) {
            const Entry &entry = state.manifest.entries.at(index);
            if ((entry.oldExists && name == oldCopyName(index))
                || (entry.hasNew && entry.staged
                    && name == stagingName(entry.stageIndex))) {
                return true;
            }
        }
        return false;
    };
    for (const AnchoredJournalFile &file : std::as_const(files)) {
        if (!expectedCleanupName(file.name)
            && !removeAnchoredJournalFile(
                state.journalDirectory, file, error)) {
            return false;
        }
    }

    const auto removeExpectedFile = [
            &state, &files, &error](
            const QString &name,
            const AtomicFileSnapshot &expected,
            bool required) {
        const AnchoredJournalFile *file =
            findAnchoredJournalFile(files, name);
        if (!file) {
            if (!required) return true;
            error = QStringLiteral(
                "A required plan transaction cleanup file is missing");
            return false;
        }
        if (!anchoredJournalFileMatches(file, expected)) {
            error = QStringLiteral(
                "A plan transaction cleanup file changed unexpectedly");
            return false;
        }
        return removeAnchoredJournalFile(
            state.journalDirectory, *file, error);
    };

    if (!removeExpectedFile(
            Detail::ManifestName,
            state.manifestSnapshot,
            true)) {
        return false;
    }
    state.manifestFile = {};
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-manifest-removed");
#endif

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (entry.oldExists) {
            if (!removeExpectedFile(
                    oldCopyName(index),
                    entry.oldContents,
                    true)) {
                return false;
            }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
            planReplacementTransitionReached("plan-replacement-cleanup-file");
#endif
        }
        if (entry.hasNew && entry.staged) {
            if (!removeExpectedFile(
                    stagingName(entry.stageIndex),
                    entry.newContents,
                    true)) {
                return false;
            }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
            planReplacementTransitionReached("plan-replacement-cleanup-file");
#endif
        }
    }
    if (markerExists) {
        const AnchoredJournalFile *marker =
            findAnchoredJournalFile(
                files, Detail::CommitMarkerName);
        if (!marker || !marker->file
            || !state.commitMarkerFile.file
            || marker->file->identity()
                != state.commitMarkerFile.file->identity()) {
            error = QStringLiteral(
                "The plan transaction commit marker changed before cleanup");
            return false;
        }
        if (!removeExpectedFile(
                Detail::CommitMarkerName,
                markerSnapshot,
                true)) {
            return false;
        }
        state.commitMarkerFile = {};
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached(
            "plan-replacement-commit-marker-removed");
#endif
    }
    files.clear();
    if (!journalDirectoryMatches(state, error)) return false;
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::removeEmptyDirectory(
            state.journalDirectory);
    if (removal.effect
        != AnchoredFileSystem::MutationEffect::AppliedDurable) {
        if (removal.effect
            == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
            QString syncError;
            if (state.namespaceDirectory.sync(syncError)) {
                state.cleanupComplete = true;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
                planReplacementTransitionReached(
                    "plan-replacement-directory-removed");
#endif
                return true;
            }
            error = removal.error;
            appendError(error, syncError);
            return false;
        }
        error = removal.error.isEmpty()
            ? QStringLiteral("Cannot remove the plan transaction directory")
            : removal.error;
        if (!removal.verifiedRecoveryPath.isEmpty()) {
            appendError(
                error,
                QStringLiteral("recovery directory retained at %1")
                    .arg(removal.verifiedRecoveryPath));
        }
        return false;
    }
    state.cleanupComplete = true;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-directory-removed");
#endif
    return true;
}

bool rollbackJournal(JournalState &state, QString &error)
{
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(state, journalFiles, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, nullptr, error)) return false;
    if (committed) {
        error = QStringLiteral("Cannot roll back a committed plan replacement");
        return false;
    }
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !restoreOldGeneration(
            state, resolved, journalFiles, error)) {
        return false;
    }
    journalFiles.clear();
    return removeJournalDirectory(state, false, error);
}

bool commitJournal(JournalState &state, QString &error)
{
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(state, journalFiles, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, nullptr, error)) return false;
    if (!committed) {
        error = QStringLiteral("Cannot complete an uncommitted plan replacement");
        return false;
    }
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !ensureNewGeneration(
            state, resolved, journalFiles, error)) {
        return false;
    }
    journalFiles.clear();
    return removeJournalDirectory(state, true, error);
}

bool maximumSizeForPreManifestEntry(
    const QString &name, qint64 &maximumSize)
{
    maximumSize = -1;
    if (name == Detail::CommitMarkerName) {
        maximumSize = Detail::MaximumCommitMarkerSize;
        return true;
    }
    if (isKnownLockName(name)) {
        maximumSize = Detail::MaximumLockFileSize;
        return true;
    }
    if (isKnownTemporaryName(name)) {
        maximumSize = knownTemporaryMaximumSize(name);
        return true;
    }
    return isKnownDataName(name);
}

bool removePreManifestJournal(
    const QString &journalPath,
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory,
    AnchoredFileSystem::DirectoryAnchor journalDirectory,
    QString &error)
{
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)
        || !namespaceDirectory.isValid()
        || !journalDirectory.isValid()
        || !namespaceDirectory.pathMatches(error)
        || !journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot anchor an incomplete plan transaction");
        }
        return false;
    }
    AtomicFileLockSet lock;
    if (!lock.lock({journalPath}, error)) return false;
    if (!namespaceDirectory.pathMatches(error)
        || !journalDirectory.pathMatches(error)) {
        return false;
    }

    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!journalDirectory.enumerateEntries(
            entries, Detail::MaximumJournalEntries, error)) {
        return false;
    }
    QList<AnchoredJournalFile> removable;
    for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
        qint64 maximumSize = -1;
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::RegularFile
            || entry.identity.linkCount() != 1
            || !maximumSizeForPreManifestEntry(
                entry.name, maximumSize)) {
            error = QStringLiteral(
                "An incomplete plan transaction contains an unknown entry");
            return false;
        }
        AnchoredJournalFile file;
        if (!pinAnchoredJournalFile(
                journalDirectory,
                entry.name,
                maximumSize,
                file,
                error,
                &entry.identity)) {
            return false;
        }
        removable.append(std::move(file));
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-pre-manifest-files-pinned");
#endif
    QList<AnchoredFileSystem::DirectoryEntry> verifiedEntries;
    if (!journalDirectory.enumerateEntries(
            verifiedEntries, Detail::MaximumJournalEntries, error)
        || !directorySnapshotsMatch(entries, verifiedEntries)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The incomplete plan transaction changed while being inspected");
        }
        return false;
    }
    for (const AnchoredJournalFile &file : std::as_const(removable)) {
        if (!removeAnchoredJournalFile(
                journalDirectory, file, error)) {
            return false;
        }
    }
    removable.clear();
    if (!journalDirectory.pathMatches(error)) return false;
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::removeEmptyDirectory(journalDirectory);
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        return true;
    }
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        if (namespaceDirectory.sync(syncError)) return true;
        error = removal.error;
        appendError(error, syncError);
        return false;
    }
    error = removal.error.isEmpty()
        ? QStringLiteral(
              "Cannot remove an incomplete plan transaction")
        : removal.error;
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery directory retained at %1")
                .arg(removal.verifiedRecoveryPath));
    }
    return false;
}

} // namespace

Journal::Journal(std::shared_ptr<JournalState> state)
    : state_(std::move(state))
{
}

std::shared_ptr<Journal> Journal::prepare(
    const Specification &specification, QString &error)
{
    error.clear();
    if (specification.targetPaths.isEmpty()) {
        error = QStringLiteral("A plan replacement requires target activities");
        return {};
    }

    QString root;
    if (!normalizeAthleteRoot(specification.athleteRoot, root, error)) {
        return {};
    }
    QString scopeRelative;
    QString scopeAbsolute;
    if (!makeScopeRelativePath(
            root,
            specification.scopeRoot,
            scopeRelative,
            scopeAbsolute,
            error)
        || !validateExistingDirectory(scopeAbsolute, error)) {
        return {};
    }
    const QString canonicalScope = QDir::cleanPath(
        QFileInfo(scopeAbsolute).canonicalFilePath());
    if (canonicalScope.isEmpty()
        || atomicFilePathKey(canonicalScope)
            != atomicFilePathKey(scopeAbsolute)) {
        error = QStringLiteral("The plan replacement scope is unsafe");
        return {};
    }

    std::shared_ptr<JournalState> state(new JournalState);
    state->athleteRoot = root;
    state->scopePath = scopeAbsolute;
    state->manifest.scopeRelativePath = scopeRelative;
    state->transactionLease = std::make_unique<AtomicFileLockSet>();
    if (!state->transactionLease->lock(
            {transactionLeaseTarget(root)}, error)) {
        error = QStringLiteral(
            "Another activity transaction is already active: %1").arg(error);
        return {};
    }
    AnchoredFileSystem::DirectoryAnchor transactionsDirectory;
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    if (!ensureTransactionNamespace(
            root,
            state->namespacePath,
            transactionsDirectory,
            namespaceDirectory,
            error)) {
        return {};
    }
    if (!transactionNamespacesAreReady(
            transactionsDirectory, namespaceDirectory, error)) {
        return {};
    }
    state->namespaceDirectory = namespaceDirectory;

    struct RequestedPath
    {
        QString relative;
        QString absolute;
        bool removed = false;
        bool target = false;
        int stageIndex = -1;
    };
    QList<RequestedPath> requested;
    QHash<QString, int> requestedByKey;
    const auto addRequested = [&](const QString &candidate,
                                  bool removed,
                                  bool target,
                                  int stageIndex) {
        QString relative;
        QString absolute;
        if (!makeRootRelativePath(
                root, candidate, relative, absolute, error)
            || !pathIsWithinScope(relative, scopeRelative)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A plan replacement path escapes its declared scope");
            }
            return false;
        }
        const QString key = atomicFilePathKey(absolute);
        const auto found = requestedByKey.constFind(key);
        if (found != requestedByKey.constEnd()) {
            RequestedPath &existing = requested[*found];
            if ((removed && existing.removed)
                || (target && existing.target)) {
                error = QStringLiteral(
                    "The plan replacement contains a duplicate path role");
                return false;
            }
            existing.removed = existing.removed || removed;
            existing.target = existing.target || target;
            if (target) existing.stageIndex = stageIndex;
            return true;
        }
        requestedByKey.insert(key, requested.size());
        requested.append({relative, absolute, removed, target, stageIndex});
        return true;
    };

    for (const QString &path : specification.removalPaths) {
        if (!addRequested(path, true, false, -1)) return {};
    }
    for (int index = 0; index < specification.targetPaths.size(); ++index) {
        if (!addRequested(
                specification.targetPaths.at(index),
                false,
                true,
                index)) {
            return {};
        }
    }
    if (requested.size() > Detail::MaximumEntries) {
        error = QStringLiteral("The plan replacement contains too many files");
        return {};
    }

    QStringList inputPaths;
    QSet<QString> inputKeys;
    for (const QString &candidate : specification.inputPaths) {
        QString relative;
        QString absolute;
        if (!makeRootRelativePath(
                root, candidate, relative, absolute, error)
            || !pathIsWithinScope(relative, scopeRelative)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A plan source escapes its declared scope");
            }
            return {};
        }
        const QString key = atomicFilePathKey(absolute);
        if (inputKeys.contains(key)) continue;
        inputKeys.insert(key);
        inputPaths.append(absolute);
    }

    QStringList lockPaths = inputPaths;
    for (const RequestedPath &path : std::as_const(requested)) {
        lockPaths.append(path.absolute);
    }
    state->pathLocks = std::make_unique<AtomicFileLockSet>();
    if (!state->pathLocks->lock(lockPaths, error)) {
        error = QStringLiteral("A plan activity file is already being changed: %1")
                    .arg(error);
        return {};
    }

    for (const QString &path : std::as_const(inputPaths)) {
        AtomicFileSnapshot snapshot;
        if (!captureAtomicFileSnapshot(path, snapshot, error)) return {};
        state->inputSnapshots.append(qMakePair(path, snapshot));
    }

    QList<RequestedPath> ordered;
    ordered.reserve(requested.size());
    for (int stageIndex = 0;
         stageIndex < specification.targetPaths.size();
         ++stageIndex) {
        const auto found = std::find_if(
            requested.cbegin(), requested.cend(),
            [stageIndex](const RequestedPath &path) {
                return path.target && path.stageIndex == stageIndex;
            });
        if (found == requested.cend()) {
            error = QStringLiteral("A plan replacement target is missing");
            return {};
        }
        ordered.append(*found);
    }
    for (const RequestedPath &path : std::as_const(requested)) {
        if (!path.target) ordered.append(path);
    }

    for (const RequestedPath &path : std::as_const(ordered)) {
        Entry entry;
        entry.relativePath = path.relative;
        entry.hasNew = path.target;
        entry.stageIndex = path.stageIndex;
        ObservedFile observed;
        if (!inspectRegularFile(path.absolute, observed, error)) return {};
        if (path.removed) {
            if (!observed.exists) {
                error = QStringLiteral(
                    "A plan removal target is unavailable");
                return {};
            }
            entry.oldExists = true;
            entry.oldContents = observed.contents;
        } else if (observed.exists) {
            error = QStringLiteral(
                "A plan replacement target already exists");
            return {};
        }
        state->manifest.entries.append(entry);
    }
    if (!verifyInputSnapshots(*state, error)) return {};

    state->manifest.id = QUuid::createUuid()
                             .toString(QUuid::WithoutBraces)
                             .toLower();
    state->journalPath = QDir(state->namespacePath).filePath(
        state->manifest.id);
    state->manifestPath = QDir(state->journalPath).filePath(
        Detail::ManifestName);
    state->commitMarkerPath = QDir(state->journalPath).filePath(
        Detail::CommitMarkerName);
    if (pathEntryExists(state->journalPath)) {
        error = QStringLiteral(
            "Cannot create the plan replacement journal because it already exists");
        return {};
    }
    const AnchoredFileSystem::MutationResult creation =
        AnchoredFileSystem::createPrivateChildDirectory(
            state->namespaceDirectory,
            state->manifest.id,
            state->journalDirectory);
    if (creation.effect
        != AnchoredFileSystem::MutationEffect::AppliedDurable) {
        error = creation.error.isEmpty()
            ? QStringLiteral(
                  "Cannot create the plan replacement journal")
            : creation.error;
        if (!creation.verifiedRecoveryPath.isEmpty()) {
            appendError(
                error,
                QStringLiteral("recovery directory retained at %1")
                    .arg(creation.verifiedRecoveryPath));
        } else if (state->journalDirectory.isValid()) {
            QString retainedError;
            if (state->journalDirectory.pathMatches(retainedError)) {
                appendError(
                    error,
                    QStringLiteral("recovery journal retained at %1")
                        .arg(state->journalPath));
            }
        }
        return {};
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-directory-created");
#endif
    if (!journalDirectoryMatches(*state, error)) return {};

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state, resolved, error)) return {};
    for (int index = 0; index < state->manifest.entries.size(); ++index) {
        const Entry &entry = state->manifest.entries.at(index);
        if (!entry.oldExists) continue;
        if (!journalDirectoryMatches(*state, error)) return {};
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached(
            "plan-replacement-before-old-copy");
#endif
        AnchoredJournalFile oldCopy;
        if (!copyExpectedPathToAnchoredJournal(
                resolved.at(index).path,
                entry.oldContents,
                state->journalDirectory,
                oldCopyName(index),
                oldCopy,
                error)) {
            appendError(
                error,
                QStringLiteral("recovery journal retained at %1")
                    .arg(state->journalPath));
            return {};
        }
        state->trackedDataFiles.append(std::move(oldCopy));
        if (!journalDirectoryMatches(*state, error)) return {};
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached("plan-replacement-old-copy-published");
#endif
        if (!trackedJournalFilesAreCurrent(*state, error)) return {};
    }
    if (!writeManifestFile(*state, false, error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1")
                .arg(state->journalPath));
        return {};
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-initial-manifest-published");
#endif
    return std::shared_ptr<Journal>(new Journal(state));
}

std::shared_ptr<Journal> Journal::openPrepared(
    const QString &athleteRoot,
    const QString &transactionId,
    QString &error)
{
    error.clear();
    if (!validTransactionId(transactionId)
        || athleteRoot.isEmpty()
        || !QDir::isAbsolutePath(athleteRoot)) {
        error = QStringLiteral(
            "The prepared plan transaction identity is invalid");
        return {};
    }

    const QString requestedRoot = QDir::cleanPath(
        QFileInfo(athleteRoot).absoluteFilePath());
    const QFileInfo requestedRootInfo(requestedRoot);
    if (!requestedRootInfo.exists()
        || !requestedRootInfo.isDir()) {
        error = QStringLiteral("The athlete root is unavailable");
        return {};
    }

    std::unique_ptr<AtomicFileLockSet> transactionLease(
        new AtomicFileLockSet);
    if (!transactionLease->lock(
            {transactionLeaseTarget(requestedRoot)}, error)) {
        error = QStringLiteral(
            "An activity transaction is already active: %1")
                    .arg(error);
        return {};
    }

    QString root;
    if (!normalizeAthleteRoot(athleteRoot, root, error))
        return {};
    AnchoredFileSystem::DirectoryAnchor rootDirectory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            root, rootDirectory, error)) {
        error = QStringLiteral(
            "Cannot anchor the prepared plan transaction root: %1")
                    .arg(error);
        return {};
    }

    AnchoredFileSystem::DirectoryAnchor transactionsDirectory;
    bool transactionsExist = false;
    if (!openExistingPrivateDirectory(
            rootDirectory,
            QStringLiteral(".gc-transactions"),
            transactionsDirectory,
            transactionsExist,
            error)
        || !transactionsExist) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared plan transaction is unavailable");
        }
        return {};
    }

    const QString namespacePath = transactionNamespacePath(root);
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    bool namespaceExists = false;
    if (!openExistingPrivateDirectory(
            transactionsDirectory,
            QStringLiteral("plan-replacement"),
            namespaceDirectory,
            namespaceExists,
            error)
        || !namespaceExists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared plan transaction is unavailable");
        }
        return {};
    }

    const QString journalPath =
        QDir(namespacePath).filePath(transactionId);
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    bool journalExists = false;
    if (!openExistingPrivateDirectory(
            namespaceDirectory,
            transactionId,
            journalDirectory,
            journalExists,
            error)
        || !journalExists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The prepared plan transaction is unavailable");
        }
        return {};
    }

    std::shared_ptr<JournalState> state;
    if (!loadManifestState(
            root,
            journalPath,
            std::move(namespaceDirectory),
            std::move(journalDirectory),
            state,
            error)) {
        return {};
    }
    state->transactionLease = std::move(transactionLease);
    return std::shared_ptr<Journal>(new Journal(state));
}

bool Journal::reconcileAll(const QString &athleteRoot, QString &error)
{
    error.clear();
    if (athleteRoot.isEmpty()
        || !QDir::isAbsolutePath(athleteRoot)) {
        error = QStringLiteral(
            "The athlete root must be an absolute path");
        return false;
    }
    const QString requestedRoot = QDir::cleanPath(
        QFileInfo(athleteRoot).absoluteFilePath());
    const QFileInfo requestedRootInfo(requestedRoot);
    if (!requestedRootInfo.exists()
        || !requestedRootInfo.isDir()) {
        error = QStringLiteral("The athlete root is unavailable");
        return false;
    }

    AtomicFileLockSet transactionLease;
    if (!transactionLease.lock(
            {transactionLeaseTarget(requestedRoot)}, error)) {
        error = QStringLiteral(
            "An activity transaction is already active: %1").arg(error);
        return false;
    }
    const QString requestedNamespace =
        transactionNamespacePath(requestedRoot);
    const QFileInfo requestedNamespaceInfo(requestedNamespace);
    if (!requestedNamespaceInfo.exists()
        && !requestedNamespaceInfo.isSymLink()) {
        return true;
    }

    QString root;
    if (!normalizeAthleteRoot(athleteRoot, root, error)) return false;

    AnchoredFileSystem::DirectoryAnchor rootDirectory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            root, rootDirectory, error)) {
        error = QStringLiteral(
            "Cannot anchor the plan transaction root: %1").arg(error);
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor transactionsDirectory;
    bool transactionsExist = false;
    if (!openExistingPrivateDirectory(
            rootDirectory,
            QStringLiteral(".gc-transactions"),
            transactionsDirectory,
            transactionsExist,
            error)) {
        return false;
    }
    if (!transactionsExist) {
        return true;
    }

    const QString namespacePath = transactionNamespacePath(root);
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    bool namespaceExists = false;
    if (!openExistingPrivateDirectory(
            transactionsDirectory,
            QStringLiteral("plan-replacement"),
            namespaceDirectory,
            namespaceExists,
            error)) {
        return false;
    }
    if (!namespaceExists) return true;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-recovery-namespace-anchored");
#endif

    QString matchError;
    if (!rootDirectory.pathMatches(matchError)
        || !transactionsDirectory.pathMatches(matchError)
        || !namespaceDirectory.pathMatches(matchError)) {
        error = matchError.isEmpty()
            ? QStringLiteral(
                  "The plan-replacement recovery namespace was replaced")
            : matchError;
        return false;
    }

    QList<AnchoredFileSystem::DirectoryEntry> entries;
    if (!namespaceDirectory.enumerateEntries(
            entries, Detail::MaximumNamespaceEntries, error)) {
        error = QStringLiteral(
            "Cannot inspect the plan-replacement recovery namespace: %1")
                    .arg(error);
        return false;
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached(
        "plan-replacement-recovery-namespace-enumerated");
#endif
    QList<AnchoredFileSystem::DirectoryEntry> verifiedEntries;
    if (!namespaceDirectory.enumerateEntries(
            verifiedEntries, Detail::MaximumNamespaceEntries, error)) {
        error = QStringLiteral(
            "Cannot verify the plan-replacement recovery namespace: %1")
                    .arg(error);
        return false;
    }
    if (!directorySnapshotsMatch(entries, verifiedEntries)) {
        error = QStringLiteral(
            "The plan-replacement recovery namespace changed after enumeration");
        return false;
    }
    entries = std::move(verifiedEntries);
    if (!namespaceDirectory.pathMatches(matchError)) {
        error = matchError.isEmpty()
            ? QStringLiteral(
                  "The plan-replacement recovery namespace changed while being enumerated")
            : matchError;
        return false;
    }

    const auto isValidLockGuard = [](
        const AnchoredFileSystem::DirectoryEntry &entry) {
        QString lockedId;
        return entry.kind
                == AnchoredFileSystem::DirectoryEntryKind::RegularFile
            && entry.identity.linkCount() == 1
            && atomicFileLockTargetName(entry.name, lockedId)
            && validTransactionId(lockedId);
    };
    for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
        if (isValidLockGuard(entry)) continue;
        if (entry.kind
                != AnchoredFileSystem::DirectoryEntryKind::Directory
            || !validTransactionId(entry.name)) {
            error = QStringLiteral(
                "Unknown entry in the plan transaction namespace: %1")
                            .arg(entry.name);
            return false;
        }
    }

    QStringList failures;
    for (const AnchoredFileSystem::DirectoryEntry &entry : entries) {
        if (!namespaceDirectory.pathMatches(matchError)) {
            failures.append(
                matchError.isEmpty()
                    ? QStringLiteral(
                          "The plan-replacement recovery namespace was replaced")
                    : matchError);
            break;
        }
        if (isValidLockGuard(entry)) continue;
        const QString &name = entry.name;
        const QString journalPath = QDir(namespacePath).filePath(name);
        QString transactionError;
        AnchoredFileSystem::DirectoryAnchor journalDirectory;
        bool journalExists = false;
        if (!openExistingPrivateDirectory(
                namespaceDirectory,
                name,
                journalDirectory,
                journalExists,
                transactionError,
                &entry.identity)
            || !journalExists
            || journalDirectory.identity() != entry.identity
            || journalDirectory.identity().linkCount()
                != entry.identity.linkCount()
            || !journalDirectory.pathMatches(transactionError)) {
            if (transactionError.isEmpty()) {
                transactionError = QStringLiteral(
                    "The plan-replacement journal changed after namespace enumeration");
            }
            failures.append(transactionError);
            continue;
        }

        bool manifestExists = false;
        {
            const AnchoredFileSystem::EntryRef manifestEntry =
                journalDirectory.entry(
                    Detail::ManifestName, transactionError);
            if (!manifestEntry.isValid()
                || !AnchoredFileSystem::entryExists(
                    manifestEntry, manifestExists, transactionError)) {
                failures.append(transactionError);
                continue;
            }
        }

        if (!manifestExists) {
            if (!removePreManifestJournal(
                    journalPath,
                    namespaceDirectory,
                    std::move(journalDirectory),
                    transactionError)) {
                failures.append(transactionError);
            }
            continue;
        }

        std::shared_ptr<JournalState> state;
        if (!loadManifestState(
                root,
                journalPath,
                namespaceDirectory,
                std::move(journalDirectory),
                state,
                transactionError)) {
            failures.append(transactionError);
            continue;
        }
        bool committed = false;
        if (!readCommitMarker(
                *state, committed, nullptr, transactionError)
            || !(committed
                ? commitJournal(*state, transactionError)
                : rollbackJournal(*state, transactionError))) {
            failures.append(transactionError);
        }
    }

    if (!namespaceDirectory.pathMatches(matchError)) {
        failures.append(
            matchError.isEmpty()
                ? QStringLiteral(
                      "The plan-replacement recovery namespace changed during recovery")
                : matchError);
    }

    if (failures.isEmpty()) {
        QList<AnchoredFileSystem::DirectoryEntry> remaining;
        QString enumerationError;
        if (!namespaceDirectory.enumerateEntries(
                remaining,
                Detail::MaximumNamespaceEntries,
                enumerationError)) {
            failures.append(enumerationError);
        } else {
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
            planReplacementTransitionReached(
                "plan-replacement-recovery-final-namespace-enumerated");
#endif
            QList<AnchoredFileSystem::DirectoryEntry> verifiedRemaining;
            if (!namespaceDirectory.enumerateEntries(
                    verifiedRemaining,
                    Detail::MaximumNamespaceEntries,
                    enumerationError)) {
                failures.append(enumerationError);
            } else if (!directorySnapshotsMatch(
                           remaining, verifiedRemaining)) {
                failures.append(QStringLiteral(
                    "The plan-replacement recovery namespace changed during final inspection"));
            } else {
                for (const AnchoredFileSystem::DirectoryEntry &entry :
                     verifiedRemaining) {
                    if (isValidLockGuard(entry)) continue;
                    failures.append(QStringLiteral(
                        "The plan-replacement recovery namespace still contains a transaction"));
                    break;
                }
            }
        }
    }

    QString hierarchyError;
    if (!rootDirectory.pathMatches(hierarchyError)
        || !transactionsDirectory.pathMatches(hierarchyError)
        || !namespaceDirectory.pathMatches(hierarchyError)
        || !namespaceDirectory.sync(hierarchyError)) {
        failures.append(
            hierarchyError.isEmpty()
                ? QStringLiteral(
                      "Cannot synchronize the plan-replacement journal namespace")
                : hierarchyError);
    }
    if (!failures.isEmpty()) {
        error = failures.join(QStringLiteral("; "));
        return false;
    }
    return true;
}

int Journal::targetCount() const
{
    if (!state_) return 0;
    return std::count_if(
        state_->manifest.entries.cbegin(),
        state_->manifest.entries.cend(),
        [](const Entry &entry) { return entry.hasNew; });
}

QString Journal::stagingPath(int targetIndex) const
{
    if (!state_ || targetIndex < 0 || targetIndex >= targetCount()) return {};
    return QDir(state_->journalPath).filePath(stagingName(targetIndex));
}

QString Journal::directoryPath() const
{
    return state_ ? state_->journalPath : QString();
}

bool Journal::recordStaged(int targetIndex, QString &error)
{
    error.clear();
    if (!state_ || targetIndex < 0 || targetIndex >= targetCount()) {
        error = QStringLiteral("The plan staging entry is unavailable");
        return false;
    }
    if (!journalDirectoryMatches(*state_, error)
        || !anchoredJournalFileIsCurrent(
            state_->journalDirectory,
            state_->manifestFile,
            error)
        || !trackedJournalFilesAreCurrent(*state_, error)) {
        return false;
    }
    const int entryIndex = entryIndexForStage(
        state_->manifest, targetIndex);
    if (entryIndex < 0) {
        error = QStringLiteral("The plan staging entry is missing");
        return false;
    }
    const QString path = stagingPath(targetIndex);
    QFile stagedFile(path);
    if (!stagedFile.open(QIODevice::ReadWrite)) {
        error = QStringLiteral("Cannot open a staged plan activity for sync: %1")
                    .arg(stagedFile.errorString());
        return false;
    }
    if (!syncFileDevice(stagedFile, error)) return false;
    stagedFile.close();
    if (!syncParentDirectory(path, error)) return false;

    AnchoredJournalFile staged;
    if (!pinAnchoredJournalFile(
            state_->journalDirectory,
            stagingName(targetIndex),
            -1,
            staged,
            error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("A staged plan activity is unavailable");
        }
        return false;
    }
    const AtomicFileSnapshot stagedSnapshot = {
        staged.file->size(), staged.file->sha256()};
    Entry &entry = state_->manifest.entries[entryIndex];
    if (entry.staged) {
        const AnchoredJournalFile *tracked =
            findAnchoredJournalFile(
                state_->trackedDataFiles,
                stagingName(targetIndex));
        if (!anchoredJournalFileMatches(
                &staged, entry.newContents)
            || (tracked
                && (!tracked->file
                    || tracked->file->identity()
                        != staged.file->identity()))) {
            error = QStringLiteral("A staged plan activity changed unexpectedly");
            return false;
        }
        return true;
    }
    entry.staged = true;
    entry.newContents = stagedSnapshot;
    if (!writeManifestFile(*state_, true, error)
        || !anchoredJournalFileIsCurrent(
            state_->journalDirectory, staged, error)) {
        return false;
    }
    state_->trackedDataFiles.append(staged);
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-stage-recorded");
#endif
    return trackedJournalFilesAreCurrent(*state_, error);
}

bool Journal::publishAndCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(
            *state_, journalFiles, error)) {
        return false;
    }
    const auto requiredForPublication = [this](const QString &name) {
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName) {
            return true;
        }
        for (int index = 0;
             index < state_->manifest.entries.size();
             ++index) {
            const Entry &entry = state_->manifest.entries.at(index);
            if ((entry.oldExists && name == oldCopyName(index))
                || (entry.hasNew && entry.staged
                    && name == stagingName(entry.stageIndex))) {
                return true;
            }
        }
        return false;
    };
    for (qsizetype index = journalFiles.size(); index > 0; --index) {
        const qsizetype fileIndex = index - 1;
        const AnchoredJournalFile &file = journalFiles.at(fileIndex);
        if (requiredForPublication(file.name)) continue;
        if (!removeAnchoredJournalFile(
                state_->journalDirectory, file, error)) {
            return false;
        }
        journalFiles.removeAt(fileIndex);
    }
    bool committed = false;
    if (!readCommitMarker(*state_, committed, nullptr, error)) return false;
    if (committed) return true;

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state_, resolved, error)
        || !publishNewGeneration(
            *state_, resolved, journalFiles, error)) {
        return false;
    }
    AtomicFileSnapshot markerSnapshot;
    if (!writeCommitMarkerFile(
            *state_, markerSnapshot, error)) {
        appendError(
            error,
            QStringLiteral(
                "plan replacement recovery is required before continuing"));
        return false;
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-commit-marker");
#endif
    if (!anchoredJournalFileIsCurrent(
            state_->journalDirectory,
            state_->commitMarkerFile,
            error)) {
        appendError(
            error,
            QStringLiteral(
                "plan replacement recovery is required before continuing"));
        return false;
    }
    return true;
}

bool Journal::cleanupAfterRollback(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    if (state_->cleanupComplete) return true;
    return rollbackJournal(*state_, error);
}

bool Journal::cleanupAfterCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    if (state_->cleanupComplete) return true;
    return commitJournal(*state_, error);
}

bool Journal::commitState(bool &committed, QString &error) const
{
    committed = false;
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The plan replacement journal is unavailable");
        return false;
    }
    return readCommitMarker(
        *state_, committed, nullptr, error);
}

bool Journal::hasCommitMarker() const
{
    bool committed = false;
    QString error;
    return commitState(committed, error) && committed;
}

} // namespace PlanReplacement
