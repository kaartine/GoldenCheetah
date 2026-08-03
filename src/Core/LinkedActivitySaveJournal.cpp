/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LinkedActivitySaveJournal.h"

#include "AnchoredFileSystem.h"
#include "AtomicFileWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
void linkedActivitySaveTransitionReached(const char *transition);
#endif

namespace LinkedActivitySave {

namespace Detail {

constexpr int ManifestVersion = 1;
constexpr int MaximumEntries = 64;
constexpr qint64 MaximumManifestSize = 4 * 1024 * 1024;
constexpr qint64 MaximumCommitMarkerSize = 128;
constexpr qint64 MaximumRetirementIntentSize = 128;
constexpr qint64 MaximumLockFileSize = 64 * 1024;

const QString ManifestName = QStringLiteral("manifest.json");
const QString CommitMarkerName = QStringLiteral("COMMITTED");

struct ExpectedFile
{
    QString relativePath;
    bool exists = false;
    AtomicFileSnapshot contents;
};

struct Entry
{
    ExpectedFile source;
    ExpectedFile backup;
    QString targetRelativePath;
    bool keepSourceBackup = false;
    bool staged = false;
    AtomicFileSnapshot stagedContents;
};

struct Manifest
{
    int version = ManifestVersion;
    QString id;
    QList<Entry> entries;
};

struct ResolvedEntry
{
    QString source;
    QString target;
    QString backup;
    QString sourceCopy;
    QString backupCopy;
    QString staging;
};

struct ObservedFile
{
    bool exists = false;
    AtomicFileSnapshot contents;
    AnchoredFileSystem::NativeIdentity parentIdentity;
    AnchoredFileSystem::NativeIdentity identity;
};

struct AnchoredActivityFile
{
    AnchoredFileSystem::DirectoryAnchor parent;
    AnchoredFileSystem::EntryRef entry;
    AnchoredFileSystem::PinnedFile file;
};

struct AnchoredJournalFile
{
    QString name;
    AnchoredFileSystem::EntryRef entry;
    std::shared_ptr<AnchoredFileSystem::PinnedFile> file;
};

struct AnchoredRetirementSource : AnchoredActivityFile
{
    struct Intent
    {
        QString path;
        AtomicFileSnapshot contents;
        AnchoredFileSystem::DirectoryAnchor parent;
        AnchoredFileSystem::EntryRef entry;
        AnchoredFileSystem::PinnedFile file;
        AnchoredFileSystem::MutationEffect mutationEffect =
            AnchoredFileSystem::MutationEffect::NoEffect;
        QString verifiedRecoveryPath;
        bool expected = false;
    } intent;

    AnchoredFileSystem::MutationEffect mutationEffect =
        AnchoredFileSystem::MutationEffect::NoEffect;
    QString verifiedRecoveryPath;
    bool completionVerified = false;
};

} // namespace Detail

struct JournalState
{
    QString athleteRoot;
    QString namespacePath;
    QString journalPath;
    QString manifestPath;
    QString commitMarkerPath;
    Detail::Manifest manifest;
    AtomicFileSnapshot manifestSnapshot;
    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    AnchoredFileSystem::DirectoryAnchor athleteRootDirectory;
    std::vector<std::unique_ptr<Detail::AnchoredRetirementSource>>
        retirementSources;
    std::unique_ptr<AtomicFileLockSet> transactionLease;
    std::unique_ptr<AtomicFileLockSet> pathLocks;
    AnchoredFileSystem::MutationEffect journalRemovalEffect =
        AnchoredFileSystem::MutationEffect::NoEffect;
    QString journalRemovalRecoveryPath;
    bool terminalCleanupCompleted = false;
};

namespace {

using Detail::Entry;
using Detail::ExpectedFile;
using Detail::AnchoredActivityFile;
using Detail::AnchoredJournalFile;
using Detail::AnchoredRetirementSource;
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

bool anchorJournalDirectory(JournalState &state, QString &error)
{
    state.namespaceDirectory = {};
    state.journalDirectory = {};
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            state.namespacePath,
            state.namespaceDirectory,
            error)
        || !state.namespaceDirectory.openChild(
            state.manifest.id,
            state.journalDirectory,
            error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot anchor the linked-save journal directory");
        }
        return false;
    }
    return true;
}

bool journalDirectoryMatches(
    const JournalState &state, QString &error)
{
    if (!state.namespaceDirectory.isValid()
        || !state.journalDirectory.isValid()) {
        error = QStringLiteral(
            "The linked-save journal directory is no longer anchored");
        return false;
    }
    if (!state.namespaceDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The linked-save journal namespace was replaced");
        }
        return false;
    }
    if (!state.journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The linked-save journal directory was moved or replaced");
        }
        return false;
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

QString sourceCopyName(int index)
{
    return QStringLiteral("source-%1.old").arg(index, 4, 10, QLatin1Char('0'));
}

QString backupCopyName(int index)
{
    return QStringLiteral("backup-%1.old").arg(index, 4, 10, QLatin1Char('0'));
}

QString stagingName(int index)
{
    return QStringLiteral("new-%1.stage").arg(index, 4, 10, QLatin1Char('0'));
}

QString retirementIntentName(int index)
{
    return QStringLiteral("retirement-%1.pending")
        .arg(index, 4, 10, QLatin1Char('0'));
}

QString transactionNamespacePath(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral(".gc-transactions/linked-save"));
}

QString removalNamespacePath(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
}

QString planReplacementNamespacePath(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral(".gc-transactions/plan-replacement"));
}

QString transactionLeaseTarget(const QString &root)
{
    // Keep the established target so linked deletion and linked save share
    // one athlete-wide lease.
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
            "An activity path overlaps the transaction namespace");
        return false;
    }
    return true;
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
                "A journal path uses a symbolic-link component");
            return false;
        }
        if (index + 1 < components.size()
            && (!info.exists() || !info.isDir())) {
            error = QStringLiteral(
                "A journal path has an unavailable parent directory");
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
        error = QStringLiteral("Activity transaction paths must be absolute");
        return false;
    }

    absolutePath = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    relativePath = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(absolutePath));
    if (!validateRelativePath(relativePath, error)) return false;

    const QString resolved = QDir::cleanPath(
        QDir(root).filePath(relativePath));
    if (atomicFilePathKey(resolved) != atomicFilePathKey(absolutePath)) {
        error = QStringLiteral("An activity path escapes the athlete root");
        return false;
    }
    return validatePathComponents(root, relativePath, error);
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
            error = QStringLiteral("A journal path escapes the athlete root");
        }
        return false;
    }
    return true;
}

bool anchorActivityFile(
    const AnchoredFileSystem::DirectoryAnchor &athleteRoot,
    const QString &relativePath,
    AnchoredActivityFile &anchored,
    QString &error)
{
    anchored = {};
    if (!validateRelativePath(relativePath, error)) return false;
    const QStringList components = QDir::fromNativeSeparators(relativePath)
                                       .split(QLatin1Char('/'));
    if (components.isEmpty()) {
        error = QStringLiteral("An activity source path is unavailable");
        return false;
    }

    anchored.parent = athleteRoot;
    for (int index = 0; index + 1 < components.size(); ++index) {
        AnchoredFileSystem::DirectoryAnchor child;
        if (!anchored.parent.openChild(
                components.at(index), child, error)) {
            error = QStringLiteral(
                "Cannot anchor an activity file parent: %1").arg(error);
            return false;
        }
        anchored.parent = std::move(child);
    }
    anchored.entry = anchored.parent.entry(
        components.constLast(), error);
    if (!anchored.entry.isValid()) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot anchor an activity file name");
        }
        return false;
    }
    if (!anchored.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("An activity file parent was replaced");
        }
        return false;
    }

    bool exists = false;
    if (!AnchoredFileSystem::entryExists(
            anchored.entry, exists, error)) {
        return false;
    }
    if (exists) {
        if (!AnchoredFileSystem::pinRegularFile(
                anchored.entry, anchored.file, error)) {
            error = QStringLiteral("Cannot pin an activity file: %1")
                        .arg(error);
            return false;
        }
        bool matches = false;
        if (!anchored.parent.pathMatches(error)
            || !AnchoredFileSystem::entryMatches(
                anchored.entry, anchored.file, matches, error)
            || !matches) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "An activity file was replaced while being pinned");
            }
            return false;
        }
    }
    return true;
}

bool anchorRetirementSource(
    const AnchoredFileSystem::DirectoryAnchor &athleteRoot,
    const QString &relativePath,
    std::unique_ptr<AnchoredRetirementSource> &source,
    QString &error)
{
    source = std::make_unique<AnchoredRetirementSource>();
    if (!anchorActivityFile(
            athleteRoot, relativePath, *source, error)) {
        source.reset();
        return false;
    }
    return true;
}

bool pinnedFileMatches(
    const AnchoredFileSystem::PinnedFile &file,
    const AtomicFileSnapshot &snapshot)
{
    return file.isValid()
        && file.size() == snapshot.size
        && file.sha256() == snapshot.digest;
}

bool ensureAthleteRootAnchored(JournalState &state, QString &error)
{
    if (!state.athleteRootDirectory.isValid()
        && !AnchoredFileSystem::DirectoryAnchor::open(
            state.athleteRoot,
            state.athleteRootDirectory,
            error)) {
        error = QStringLiteral("Cannot anchor the athlete root: %1").arg(error);
        return false;
    }
    if (!state.athleteRootDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("The athlete root was replaced");
        }
        return false;
    }
    return true;
}

bool observeActivityFile(
    JournalState &state,
    const QString &relativePath,
    ObservedFile &observed,
    QString &error)
{
    observed = {};
    AnchoredActivityFile anchored;
    if (!ensureAthleteRootAnchored(state, error)
        || !anchorActivityFile(
            state.athleteRootDirectory,
            relativePath,
            anchored,
            error)) {
        return false;
    }
    observed.parentIdentity = anchored.parent.identity();
    if (!anchored.file.isValid()) return true;

    observed.exists = true;
    observed.identity = anchored.file.identity();
    observed.contents.size = anchored.file.size();
    observed.contents.digest = anchored.file.sha256();
    return true;
}

bool anchorObservedActivityFile(
    JournalState &state,
    const QString &relativePath,
    const ObservedFile &observed,
    AnchoredActivityFile &anchored,
    QString &error)
{
    if (!ensureAthleteRootAnchored(state, error)
        || !anchorActivityFile(
            state.athleteRootDirectory,
            relativePath,
            anchored,
            error)) {
        return false;
    }
    if (!observed.parentIdentity.isValid()
        || anchored.parent.identity() != observed.parentIdentity) {
        error = QStringLiteral(
            "An activity file parent changed before it could be anchored");
        return false;
    }
    if (anchored.file.isValid() != observed.exists) {
        error = QStringLiteral(
            "An activity file changed before it could be anchored");
        return false;
    }
    if (observed.exists
        && (!observed.identity.isValid()
            || anchored.file.identity() != observed.identity
            || !pinnedFileMatches(anchored.file, observed.contents))) {
        error = QStringLiteral(
            "An activity file changed while it was being anchored");
        return false;
    }
    return true;
}

bool completeAnchoredActivityRemoval(
    AnchoredActivityFile &anchored,
    const QString &failure,
    QString &error)
{
    if (!anchored.parent.pathMatches(error)) {
        if (error.isEmpty()) error = failure;
        return false;
    }
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(anchored.file);
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        return true;
    }
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        if (anchored.parent.sync(syncError)) return true;
        error = removal.error.isEmpty() ? failure : removal.error;
        appendError(error, syncError);
        return false;
    }
    error = removal.error.isEmpty() ? failure : removal.error;
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(removal.verifiedRecoveryPath));
    }
    return false;
}

bool pinExpectedJournalFile(
    const JournalState &state,
    const QString &path,
    const AtomicFileSnapshot &expected,
    AnchoredFileSystem::PinnedFile &file,
    QString &error)
{
    if (!journalDirectoryMatches(state, error)
        || atomicFilePathKey(QFileInfo(path).absolutePath())
            != atomicFilePathKey(state.journalPath)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A linked-save publication source escapes its journal");
        }
        return false;
    }
    const AnchoredFileSystem::EntryRef entry =
        state.journalDirectory.entry(QFileInfo(path).fileName(), error);
    if (!entry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            entry, file, error, expected.size)
        || !pinnedFileMatches(file, expected)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A linked-save publication source changed unexpectedly");
        }
        return false;
    }
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            entry, file, matches, error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A linked-save publication source was replaced");
        }
        return false;
    }
    return true;
}

bool publishPinnedFile(
    const AnchoredFileSystem::PinnedFile &source,
    AnchoredActivityFile &destination,
    const QString &temporaryName,
    QString &error)
{
    if (!destination.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An activity publication parent was replaced");
        }
        return false;
    }
    AnchoredActivityFile temporary;
    temporary.parent = destination.parent;
    temporary.entry = temporary.parent.entry(temporaryName, error);
    if (!temporary.entry.isValid()
        || !AnchoredFileSystem::copyToNewFile(
            source, temporary.entry, temporary.file, error)) {
        return false;
    }
    if (!destination.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An activity publication parent was replaced");
        }
        return false;
    }

    const AnchoredFileSystem::MutationResult publication =
        AnchoredFileSystem::moveNoReplace(
            temporary.file, destination.entry);
    bool published = publication.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable;
    if (publication.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        published = destination.parent.sync(syncError);
        if (!published) {
            error = publication.error;
            appendError(error, syncError);
        }
    }
    if (published) {
        bool matches = false;
        if (!destination.parent.pathMatches(error)
            || !AnchoredFileSystem::entryMatches(
                destination.entry,
                temporary.file,
                matches,
                error)
            || !matches) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "An anchored activity publication was replaced");
            }
            return false;
        }
        return true;
    }

    if (error.isEmpty()) {
        error = publication.error.isEmpty()
            ? QStringLiteral("Cannot publish an anchored activity file")
            : publication.error;
    }
    if (!publication.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(publication.verifiedRecoveryPath));
    }
    if (publication.effect
            != AnchoredFileSystem::MutationEffect::Partial
        && temporary.file.isValid()) {
        QString cleanupError;
        if (!completeAnchoredActivityRemoval(
                temporary,
                QStringLiteral(
                    "Cannot remove an unpublished activity staging file"),
                cleanupError)) {
            appendError(error, cleanupError);
        }
    }
    return false;
}

bool publishPinnedActivityFile(
    const AnchoredFileSystem::PinnedFile &source,
    AnchoredActivityFile &destination,
    QString &error)
{
    const QString temporaryName = QStringLiteral(".gc-linked-save-%1.tmp")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    return publishPinnedFile(
        source, destination, temporaryName, error);
}

bool pinTemporaryContents(
    const QByteArray &contents,
    QTemporaryFile &temporary,
    AnchoredFileSystem::PinnedFile &source,
    QString &error)
{
    if (!temporary.open()
        || temporary.write(contents) != contents.size()
        || !temporary.flush()
        || !syncFileDevice(temporary, error)) {
        if (error.isEmpty()) error = temporary.errorString();
        return false;
    }
    const QString path = temporary.fileName();
    temporary.close();

    AnchoredFileSystem::DirectoryAnchor parent;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            QFileInfo(path).absolutePath(), parent, error)
        || !parent.pathMatches(error)) {
        return false;
    }
    const AnchoredFileSystem::EntryRef entry = parent.entry(
        QFileInfo(path).fileName(), error);
    bool matches = false;
    if (!entry.isValid()
        || !AnchoredFileSystem::pinRegularFile(
            entry, source, error, contents.size())
        || !AnchoredFileSystem::entryMatches(
            entry, source, matches, error)
        || !matches
        || source.size() != contents.size()
        || source.sha256()
            != QCryptographicHash::hash(
                contents, QCryptographicHash::Sha256)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot pin the anchored publication contents");
        }
        return false;
    }
    return true;
}

bool publishAnchoredJournalFile(
    JournalState &state,
    const QString &name,
    const QByteArray &contents,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    if (!journalDirectoryMatches(state, error)) return false;

    QTemporaryFile temporarySource;
    AnchoredFileSystem::PinnedFile source;
    if (!pinTemporaryContents(
            contents, temporarySource, source, error)) {
        return false;
    }

    AnchoredActivityFile destination;
    destination.parent = state.journalDirectory;
    destination.entry = destination.parent.entry(name, error);
    bool exists = false;
    if (!destination.entry.isValid()
        || !AnchoredFileSystem::entryExists(
            destination.entry, exists, error)) {
        return false;
    }
    if (exists) {
        error = QStringLiteral(
            "An anchored journal publication target already exists");
        return false;
    }

    const QString temporaryName = QStringLiteral(".%1.%2.tmp")
        .arg(name, QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!publishPinnedFile(
            source, destination, temporaryName, error)
        || !journalDirectoryMatches(state, error)) {
        return false;
    }
    snapshot.size = source.size();
    snapshot.digest = source.sha256();
    return true;
}

bool replaceObservedActivityFile(
    JournalState &state,
    const QString &journalSourcePath,
    const AtomicFileSnapshot &expectedSource,
    const QString &destinationRelativePath,
    const ObservedFile &observedDestination,
    const char *transition,
    QString &error)
{
    AnchoredFileSystem::PinnedFile source;
    if (!pinExpectedJournalFile(
            state,
            journalSourcePath,
            expectedSource,
            source,
            error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    if (transition) linkedActivitySaveTransitionReached(transition);
#else
    Q_UNUSED(transition)
#endif
    AnchoredActivityFile destination;
    if (!anchorObservedActivityFile(
            state,
            destinationRelativePath,
            observedDestination,
            destination,
            error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached(
        "linked-save-activity-destination-anchored");
#endif
    if (destination.file.isValid()
        && !completeAnchoredActivityRemoval(
            destination,
            QStringLiteral(
                "Cannot remove the previous activity generation"),
            error)) {
        return false;
    }
    return publishPinnedActivityFile(source, destination, error);
}

bool removeObservedActivityFile(
    JournalState &state,
    const QString &relativePath,
    const ObservedFile &observed,
    const char *transition,
    QString &error)
{
    if (!observed.exists) return true;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    if (transition) linkedActivitySaveTransitionReached(transition);
#else
    Q_UNUSED(transition)
#endif
    AnchoredActivityFile anchored;
    if (!anchorObservedActivityFile(
            state, relativePath, observed, anchored, error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached(
        "linked-save-activity-destination-anchored");
#endif
    return completeAnchoredActivityRemoval(
        anchored,
        QStringLiteral("Cannot remove a transaction-owned activity file"),
        error);
}

bool validateExistingDirectory(const QString &path, QString &error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        error = QStringLiteral(
            "The activity transaction directory is unavailable or unsafe");
        return false;
    }
    return true;
}

bool makeDirectoryPrivate(const QString &path, QString &error)
{
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(path, privatePermissions)) {
        error = QStringLiteral(
            "Cannot make the activity transaction directory private");
        return false;
    }

#ifdef Q_OS_UNIX
    QFileInfo info(path);
    info.refresh();
    const QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther
        | QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QFileDevice::Permissions permissions = info.permissions();
    if ((permissions & nonOwnerPermissions) != QFileDevice::Permissions()
        || !permissions.testFlag(QFileDevice::ReadOwner)
        || !permissions.testFlag(QFileDevice::WriteOwner)
        || !permissions.testFlag(QFileDevice::ExeOwner)) {
        error = QStringLiteral(
            "The activity transaction directory is not private");
        return false;
    }
#endif
    return true;
}

bool ensurePrivateDirectory(const QString &path, QString &error)
{
    const QFileInfo existing(path);
    if (existing.exists() || existing.isSymLink()) {
        return validateExistingDirectory(path, error)
            && makeDirectoryPrivate(path, error);
    }

    const QFileInfo parent(existing.absolutePath());
    if (!parent.exists() || !parent.isDir() || parent.isSymLink()) {
        error = QStringLiteral(
            "Cannot create an activity transaction under an unsafe directory");
        return false;
    }
    if (!QDir().mkdir(path)) {
        error = QStringLiteral("Cannot create the activity transaction directory");
        return false;
    }
    if (!makeDirectoryPrivate(path, error)) {
        QDir().rmdir(path);
        return false;
    }
    QString syncError;
    if (!syncParentDirectory(path, syncError)) {
        error = syncError;
        return false;
    }
    return true;
}

bool ensureTransactionNamespace(
    const QString &root, QString &namespacePath, QString &error)
{
    const QString transactions = QDir(root).filePath(
        QStringLiteral(".gc-transactions"));
    if (!ensurePrivateDirectory(transactions, error)) return false;
    namespacePath = transactionNamespacePath(root);
    return ensurePrivateDirectory(namespacePath, error);
}

bool namespaceHasPendingEntries(
    const QString &path, bool &pending, QString &error)
{
    pending = false;
    const QFileInfo namespaceInfo(path);
    if (!namespaceInfo.exists() && !namespaceInfo.isSymLink()) return true;
    if (!ensurePrivateDirectory(path, error)) return false;

    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        QString lockedId;
        if (entry.isFile() && !entry.isSymLink()
            && atomicFileLockTargetName(name, lockedId)) {
            if (validTransactionId(lockedId)) continue;
        }
        pending = true;
        return true;
    }
    return true;
}

bool transactionNamespacesAreReady(
    const QString &root, const QString &saveNamespace, QString &error)
{
    bool pending = false;
    if (!namespaceHasPendingEntries(saveNamespace, pending, error)) {
        return false;
    }
    if (!pending
        && !namespaceHasPendingEntries(
            removalNamespacePath(root), pending, error)) {
        return false;
    }
    if (!pending
        && !namespaceHasPendingEntries(
            planReplacementNamespacePath(root), pending, error)) {
        return false;
    }
    if (pending) {
        error = QStringLiteral(
            "Pending linked activity recovery must be completed before starting another transaction");
        return false;
    }
    return true;
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
            "A transaction path is not a regular file: %1").arg(path);
        return false;
    }
    if (maximumSize >= 0 && info.size() > maximumSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(path);
        return false;
    }
    observed.exists = true;
    if (!captureAtomicFileSnapshot(path, observed.contents, error)) {
        return false;
    }
    if (maximumSize >= 0 && observed.contents.size > maximumSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(path);
        return false;
    }
    return true;
}

bool snapshotMatches(
    const ObservedFile &observed, const AtomicFileSnapshot &expected)
{
    return observed.exists
        && observed.contents.size == expected.size
        && observed.contents.digest == expected.digest;
}

bool validateExpectedSnapshot(
    const QString &path,
    const AtomicFileSnapshot &expected,
    QString &error)
{
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    if (!snapshotMatches(observed, expected)) {
        error = QStringLiteral(
            "A transaction file does not match its journal: %1").arg(path);
        return false;
    }
    return true;
}

QJsonObject expectedFileToJson(const ExpectedFile &expected)
{
    QJsonObject object;
    object.insert(QStringLiteral("path"), expected.relativePath);
    object.insert(QStringLiteral("exists"), expected.exists);
    object.insert(
        QStringLiteral("size"),
        QString::number(expected.exists ? expected.contents.size : 0));
    object.insert(
        QStringLiteral("sha256"),
        expected.exists
            ? QString::fromLatin1(expected.contents.digest.toHex())
            : QString());
    return object;
}

QJsonObject stagedFileToJson(const Entry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("ready"), entry.staged);
    object.insert(
        QStringLiteral("size"),
        QString::number(entry.staged ? entry.stagedContents.size : 0));
    object.insert(
        QStringLiteral("sha256"),
        entry.staged
            ? QString::fromLatin1(entry.stagedContents.digest.toHex())
            : QString());
    return object;
}

QByteArray serializeManifest(const Manifest &manifest)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), manifest.version);
    root.insert(QStringLiteral("id"), manifest.id);
    QJsonArray entries;
    for (const Entry &entry : manifest.entries) {
        QJsonObject object;
        object.insert(
            QStringLiteral("source"), expectedFileToJson(entry.source));
        object.insert(
            QStringLiteral("target"), entry.targetRelativePath);
        object.insert(
            QStringLiteral("backup"), expectedFileToJson(entry.backup));
        object.insert(
            QStringLiteral("keepSourceBackup"), entry.keepSourceBackup);
        object.insert(
            QStringLiteral("staged"), stagedFileToJson(entry));
        entries.append(object);
    }
    root.insert(QStringLiteral("entries"), entries);
    QByteArray document = QJsonDocument(root).toJson(QJsonDocument::Compact);
    document.append('\n');
    return document;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    if (keys.size() != expected.size()) return false;
    for (const QString &key : keys) {
        if (!expected.contains(key)) return false;
    }
    return true;
}

bool parseSnapshotFields(
    const QJsonObject &object,
    bool exists,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    const QString sizeText = object.value(QStringLiteral("size")).toString();
    bool sizeValid = false;
    const qint64 size = sizeText.toLongLong(&sizeValid, 10);
    if (!sizeValid || size < 0 || QString::number(size) != sizeText) {
        error = QStringLiteral("A journal file size is invalid");
        return false;
    }
    const QString digestText =
        object.value(QStringLiteral("sha256")).toString();
    if (!exists) {
        if (size != 0 || !digestText.isEmpty()) {
            error = QStringLiteral(
                "An absent journal file has unexpected contents");
            return false;
        }
        snapshot = {};
        return true;
    }
    if (digestText.size() != 64 || digestText != digestText.toLower()) {
        error = QStringLiteral("A journal SHA-256 digest is invalid");
        return false;
    }
    const QByteArray digest = QByteArray::fromHex(digestText.toLatin1());
    if (digest.size() != 32
        || QString::fromLatin1(digest.toHex()) != digestText) {
        error = QStringLiteral("A journal SHA-256 digest is invalid");
        return false;
    }
    snapshot.size = size;
    snapshot.digest = digest;
    return true;
}

bool parseExpectedFile(
    const QJsonValue &value, ExpectedFile &expected, QString &error)
{
    if (!value.isObject()) {
        error = QStringLiteral("A journal file record is not an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> keys = {
        QStringLiteral("path"),
        QStringLiteral("exists"),
        QStringLiteral("size"),
        QStringLiteral("sha256")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("path")).isString()
        || !object.value(QStringLiteral("exists")).isBool()
        || !object.value(QStringLiteral("size")).isString()
        || !object.value(QStringLiteral("sha256")).isString()) {
        error = QStringLiteral("A journal file record has an invalid schema");
        return false;
    }
    expected = {};
    expected.relativePath = QDir::fromNativeSeparators(
        object.value(QStringLiteral("path")).toString());
    if (!validateRelativePath(expected.relativePath, error)) return false;
    expected.exists = object.value(QStringLiteral("exists")).toBool();
    return parseSnapshotFields(
        object, expected.exists, expected.contents, error);
}

bool parseStagedFile(
    const QJsonValue &value, Entry &entry, QString &error)
{
    if (!value.isObject()) {
        error = QStringLiteral("A staged journal record is not an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> keys = {
        QStringLiteral("ready"),
        QStringLiteral("size"),
        QStringLiteral("sha256")};
    if (!hasExactKeys(object, keys)
        || !object.value(QStringLiteral("ready")).isBool()
        || !object.value(QStringLiteral("size")).isString()
        || !object.value(QStringLiteral("sha256")).isString()) {
        error = QStringLiteral("A staged journal record has an invalid schema");
        return false;
    }
    entry.staged = object.value(QStringLiteral("ready")).toBool();
    return parseSnapshotFields(
        object, entry.staged, entry.stagedContents, error);
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
        error = QStringLiteral("The linked-save journal is not valid JSON");
        return false;
    }
    const QJsonObject root = document.object();
    const QSet<QString> keys = {
        QStringLiteral("version"),
        QStringLiteral("id"),
        QStringLiteral("entries")};
    if (!hasExactKeys(root, keys)
        || !root.value(QStringLiteral("version")).isDouble()
        || root.value(QStringLiteral("version")).toDouble()
            != Detail::ManifestVersion
        || !root.value(QStringLiteral("id")).isString()
        || !root.value(QStringLiteral("entries")).isArray()) {
        error = QStringLiteral("The linked-save journal schema is invalid");
        return false;
    }

    manifest = {};
    manifest.version = Detail::ManifestVersion;
    manifest.id = root.value(QStringLiteral("id")).toString();
    if (!validTransactionId(manifest.id) || manifest.id != expectedId) {
        error = QStringLiteral("The linked-save journal id is invalid");
        return false;
    }

    const QJsonArray values = root.value(QStringLiteral("entries")).toArray();
    if (values.size() < 2 || values.size() > Detail::MaximumEntries) {
        error = QStringLiteral("The linked-save journal entry count is invalid");
        return false;
    }
    for (const QJsonValue &value : values) {
        if (!value.isObject()) {
            error = QStringLiteral("A linked-save journal entry is invalid");
            return false;
        }
        const QJsonObject object = value.toObject();
        const QSet<QString> entryKeys = {
            QStringLiteral("source"),
            QStringLiteral("target"),
            QStringLiteral("backup"),
            QStringLiteral("keepSourceBackup"),
            QStringLiteral("staged")};
        if (!hasExactKeys(object, entryKeys)
            || !object.value(QStringLiteral("target")).isString()
            || !object.value(QStringLiteral("keepSourceBackup")).isBool()) {
            error = QStringLiteral("A linked-save journal entry schema is invalid");
            return false;
        }
        Entry entry;
        if (!parseExpectedFile(
                object.value(QStringLiteral("source")), entry.source, error)
            || !parseExpectedFile(
                object.value(QStringLiteral("backup")), entry.backup, error)
            || !parseStagedFile(
                object.value(QStringLiteral("staged")), entry, error)) {
            return false;
        }
        entry.targetRelativePath = QDir::fromNativeSeparators(
            object.value(QStringLiteral("target")).toString());
        entry.keepSourceBackup =
            object.value(QStringLiteral("keepSourceBackup")).toBool();
        if (!entry.source.exists
            || !validateRelativePath(entry.targetRelativePath, error)
            || (entry.keepSourceBackup
                && entry.backup.relativePath.isEmpty())) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A linked-save journal entry is inconsistent");
            }
            return false;
        }
        manifest.entries.append(entry);
    }
    return true;
}

bool readSmallRegularFile(
    const QString &path,
    qint64 maximumSize,
    QByteArray &contents,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        error = QStringLiteral("A required transaction file is missing: %1")
                    .arg(path);
        return false;
    }
    if (info.size() > maximumSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(path);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read a transaction file: %1")
                    .arg(file.errorString());
        return false;
    }
    contents = file.read(maximumSize + 1);
    if (contents.size() > maximumSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(path);
        return false;
    }
    if (file.error() != QFileDevice::NoError || !file.atEnd()) {
        error = QStringLiteral("Cannot read a complete transaction file: %1")
                    .arg(path);
        return false;
    }
    snapshot.size = contents.size();
    snapshot.digest = QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256);
    QFileInfo after(path);
    if (!after.exists() || !after.isFile() || after.isSymLink()
        || after.size() != snapshot.size) {
        error = QStringLiteral("A transaction file changed while being read");
        return false;
    }
    return true;
}

bool writeBytesAtomically(
    const QString &path,
    const QByteArray &contents,
    AtomicFileMode mode,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    std::unique_ptr<AtomicFileWriter> writer =
        qSaveFileWriterFactory()(path, mode);
    if (!writer || !writer->open()) {
        error = writer
            ? atomicFileError(
                  QStringLiteral("Cannot create an activity transaction file"),
                  *writer)
            : QStringLiteral("Cannot create an activity transaction writer");
        return false;
    }
    if (writer->write(contents) != contents.size()) {
        error = atomicFileError(
            QStringLiteral("Cannot write an activity transaction file"),
            *writer);
        writer->cancelWriting();
        return false;
    }
    if (!writer->flush()) {
        error = atomicFileError(
            QStringLiteral("Cannot flush an activity transaction file"),
            *writer);
        writer->cancelWriting();
        return false;
    }
    if (!writer->commit()) {
        error = atomicFileError(
            QStringLiteral("Cannot publish an activity transaction file"),
            *writer);
        return false;
    }
    if (!syncParentDirectory(path, error)) return false;
    if (!captureAtomicFileSnapshot(path, snapshot, error)) return false;
    const AtomicFileSnapshot expected = {
        static_cast<qint64>(contents.size()),
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256)};
    if (snapshot.size != expected.size
        || snapshot.digest != expected.digest) {
        error = QStringLiteral(
            "A published transaction file does not match its contents");
        return false;
    }
    return true;
}

bool copyExpectedFileAtomically(
    const QString &sourcePath,
    const QString &targetPath,
    const AtomicFileSnapshot &expected,
    AtomicFileMode mode,
    QString &error)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()
        || sourceInfo.isSymLink()) {
        error = QStringLiteral("A transaction source file is unavailable");
        return false;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read an activity transaction file: %1")
                    .arg(source.errorString());
        return false;
    }

    std::unique_ptr<AtomicFileWriter> writer =
        qSaveFileWriterFactory()(targetPath, mode);
    if (!writer || !writer->open()) {
        error = writer
            ? atomicFileError(
                  QStringLiteral("Cannot create an activity transaction file"),
                  *writer)
            : QStringLiteral("Cannot create an activity transaction writer");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 size = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            error = QStringLiteral("Cannot read an activity transaction file: %1")
                        .arg(source.errorString());
            writer->cancelWriting();
            return false;
        }
        size += chunk.size();
        hash.addData(chunk);
        if (writer->write(chunk) != chunk.size()) {
            error = atomicFileError(
                QStringLiteral("Cannot write an activity transaction file"),
                *writer);
            writer->cancelWriting();
            return false;
        }
    }
    if (size != expected.size || hash.result() != expected.digest) {
        error = QStringLiteral(
            "An activity transaction source changed while being copied");
        writer->cancelWriting();
        return false;
    }
    if (!writer->flush()) {
        error = atomicFileError(
            QStringLiteral("Cannot flush an activity transaction file"),
            *writer);
        writer->cancelWriting();
        return false;
    }
    if (!writer->commit()) {
        error = atomicFileError(
            QStringLiteral("Cannot publish an activity transaction file"),
            *writer);
        return false;
    }
    if (!syncParentDirectory(targetPath, error)
        || !validateExpectedSnapshot(targetPath, expected, error)) {
        return false;
    }
    return true;
}

bool writeManifestFile(
    JournalState &state, bool replace, QString &error)
{
    const QByteArray contents = serializeManifest(state.manifest);
    if (contents.size() > Detail::MaximumManifestSize) {
        error = QStringLiteral("The linked-save journal is too large");
        return false;
    }
    return writeBytesAtomically(
        state.manifestPath,
        contents,
        replace
            ? AtomicFileMode::ReplaceExisting
            : AtomicFileMode::CreateNew,
        state.manifestSnapshot,
        error);
}

bool resolveManifestEntries(
    const JournalState &state,
    QList<ResolvedEntry> &resolved,
    QString &error)
{
    resolved.clear();
    QSet<QString> sourceKeys;
    QSet<QString> targetKeys;
    QSet<QString> backupKeys;
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        ResolvedEntry paths;
        if (!resolveRootRelativePath(
                state.athleteRoot,
                entry.source.relativePath,
                paths.source,
                error)
            || !resolveRootRelativePath(
                state.athleteRoot,
                entry.targetRelativePath,
                paths.target,
                error)
            || !resolveRootRelativePath(
                state.athleteRoot,
                entry.backup.relativePath,
                paths.backup,
                error)) {
            return false;
        }
        paths.sourceCopy = QDir(state.journalPath).filePath(
            sourceCopyName(index));
        paths.backupCopy = QDir(state.journalPath).filePath(
            backupCopyName(index));
        paths.staging = QDir(state.journalPath).filePath(
            stagingName(index));

        const QString sourceKey = atomicFilePathKey(paths.source);
        const QString targetKey = atomicFilePathKey(paths.target);
        const QString backupKey = atomicFilePathKey(paths.backup);
        if (sourceKeys.contains(sourceKey)
            || targetKeys.contains(targetKey)
            || backupKeys.contains(backupKey)) {
            error = QStringLiteral("Linked-save transaction paths are duplicated");
            return false;
        }
        sourceKeys.insert(sourceKey);
        targetKeys.insert(targetKey);
        backupKeys.insert(backupKey);
        resolved.append(paths);
    }

    for (int index = 0; index < resolved.size(); ++index) {
        const QString sourceKey = atomicFilePathKey(resolved.at(index).source);
        const QString targetKey = atomicFilePathKey(resolved.at(index).target);
        const QString backupKey = atomicFilePathKey(resolved.at(index).backup);
        for (int other = 0; other < resolved.size(); ++other) {
            if (other == index) continue;
            if (targetKey == atomicFilePathKey(resolved.at(other).source)
                || targetKey == atomicFilePathKey(resolved.at(other).backup)
                || sourceKey == atomicFilePathKey(resolved.at(other).backup)
                || backupKey == atomicFilePathKey(resolved.at(other).target)) {
                error = QStringLiteral("Linked-save transaction paths overlap");
                return false;
            }
        }
        if (backupKey == sourceKey || backupKey == targetKey) {
            error = QStringLiteral("Linked-save backup paths overlap activity files");
            return false;
        }
    }
    return true;
}

QStringList productionLockPaths(const QList<ResolvedEntry> &entries)
{
    QStringList paths;
    for (const ResolvedEntry &entry : entries) {
        paths << entry.source << entry.target << entry.backup;
    }
    return paths;
}

bool loadManifestState(
    const QString &root,
    const QString &journalPath,
    std::shared_ptr<JournalState> &state,
    QString &error)
{
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)) {
        error = QStringLiteral("The linked-save transaction id is invalid");
        return false;
    }
    const QString expectedNamespace = transactionNamespacePath(root);
    if (atomicFilePathKey(QFileInfo(journalPath).absolutePath())
        != atomicFilePathKey(expectedNamespace)) {
        error = QStringLiteral(
            "The transaction directory is outside its namespace");
        return false;
    }
    if (!ensurePrivateDirectory(journalPath, error)) return false;

    std::shared_ptr<JournalState> loaded(new JournalState);
    loaded->athleteRoot = root;
    loaded->namespacePath = expectedNamespace;
    loaded->journalPath = journalPath;
    loaded->manifestPath = QDir(journalPath).filePath(Detail::ManifestName);
    loaded->commitMarkerPath = QDir(journalPath).filePath(
        Detail::CommitMarkerName);

    QByteArray contents;
    if (!readSmallRegularFile(
            loaded->manifestPath,
            Detail::MaximumManifestSize,
            contents,
            loaded->manifestSnapshot,
            error)
        || !parseManifest(contents, id, loaded->manifest, error)
        || !anchorJournalDirectory(*loaded, error)) {
        return false;
    }
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*loaded, resolved, error)) return false;

    loaded->pathLocks = std::make_unique<AtomicFileLockSet>();
    if (!loaded->pathLocks->lock(productionLockPaths(resolved), error)) {
        error = QStringLiteral("A linked activity file is already being saved: %1")
                    .arg(error);
        return false;
    }

    AtomicFileSnapshot after;
    if (!captureAtomicFileSnapshot(loaded->manifestPath, after, error)
        || after.size != loaded->manifestSnapshot.size
        || after.digest != loaded->manifestSnapshot.digest) {
        error = QStringLiteral(
            "The linked-save journal changed while it was being locked");
        return false;
    }
    state = std::move(loaded);
    return true;
}

bool markerSnapshot(
    const JournalState &state, AtomicFileSnapshot &snapshot)
{
    const QByteArray contents = state.manifest.id.toLatin1() + '\n';
    snapshot.size = contents.size();
    snapshot.digest = QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256);
    return true;
}

QByteArray retirementIntentContents(
    const JournalState &state, int index)
{
    return state.manifest.id.toLatin1()
        + ':' + QByteArray::number(index) + '\n';
}

AtomicFileSnapshot retirementIntentSnapshot(
    const JournalState &state, int index)
{
    const QByteArray contents = retirementIntentContents(state, index);
    return {
        static_cast<qint64>(contents.size()),
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256)};
}

bool readCommitMarker(
    const JournalState &state, bool &committed, QString &error)
{
    committed = false;
    const QFileInfo info(state.commitMarkerPath);
    if (!info.exists() && !info.isSymLink()) return true;
    if (!info.isFile() || info.isSymLink()) {
        error = QStringLiteral("Cannot read the transaction commit marker");
        return false;
    }
    if (info.size() > Detail::MaximumCommitMarkerSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(state.commitMarkerPath);
        return false;
    }
    QByteArray contents;
    AtomicFileSnapshot observed;
    if (!readSmallRegularFile(
            state.commitMarkerPath,
            Detail::MaximumCommitMarkerSize,
            contents,
            observed,
            error)) {
        return false;
    }
    if (contents != state.manifest.id.toLatin1() + '\n') {
        error = QStringLiteral("The transaction commit marker is invalid");
        return false;
    }
    committed = true;
    return true;
}

bool isKnownDataName(const QString &name)
{
    static const QRegularExpression expression(
        QStringLiteral(
            "^(source-[0-9]{4}\\.old|backup-[0-9]{4}\\.old|new-[0-9]{4}\\.stage)$"));
    return expression.match(name).hasMatch();
}

bool retirementIntentIndex(const QString &name, int &index)
{
    index = -1;
    static const QRegularExpression expression(
        QStringLiteral("^retirement-([0-9]{4})\\.pending$"));
    const QRegularExpressionMatch match = expression.match(name);
    if (!match.hasMatch()) return false;
    bool converted = false;
    index = match.captured(1).toInt(&converted);
    return converted;
}

bool isKnownTemporaryName(const QString &name)
{
    if (!name.startsWith(QLatin1Char('.'))) return false;
    static const QRegularExpression expression(
        QStringLiteral(
            "^\\.(manifest\\.json|COMMITTED|source-[0-9]{4}\\.old|backup-[0-9]{4}\\.old|new-[0-9]{4}\\.stage)\\.[A-Za-z0-9]+\\.tmp$"));
    return expression.match(name).hasMatch();
}

bool retirementIntentTemporaryIndex(const QString &name, int &index)
{
    index = -1;
    static const QRegularExpression expression(
        QStringLiteral(
            "^\\.retirement-([0-9]{4})\\.pending\\.[A-Za-z0-9]+\\.tmp$"));
    const QRegularExpressionMatch match = expression.match(name);
    if (!match.hasMatch()) return false;
    bool converted = false;
    index = match.captured(1).toInt(&converted);
    return converted;
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
        if (name == sourceCopyName(index)
            || name == backupCopyName(index)
            || name == stagingName(index)) {
            return true;
        }
    }
    return false;
}

bool pinAnchoredJournalFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const QString &name,
    qint64 maximumSize,
    QList<AnchoredJournalFile> &files,
    QString &error)
{
    AnchoredJournalFile anchored;
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
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A linked-save journal file was replaced while being pinned");
        }
        return false;
    }
    files.append(std::move(anchored));
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
    return file && file->file
        && pinnedFileMatches(*file->file, expected);
}

bool removeAnchoredJournalFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const AnchoredJournalFile &anchored,
    QString &error)
{
    if (!directory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The linked-save journal directory was moved or replaced");
        }
        return false;
    }
    if (!anchored.entry.isValid() || !anchored.file
        || !anchored.file->isValid()) {
        error = QStringLiteral(
            "A linked-save journal cleanup file is not anchored");
        return false;
    }
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            anchored.entry,
            *anchored.file,
            matches,
            error)
        || !matches) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A linked-save journal cleanup file was replaced and retained");
        }
        return false;
    }

    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(*anchored.file);
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        return true;
    }
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        if (directory.sync(syncError)) return true;
        error = removal.error;
        appendError(error, syncError);
        return false;
    }
    error = removal.error.isEmpty()
        ? QStringLiteral("Cannot remove an anchored linked-save journal file")
        : removal.error;
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(removal.verifiedRecoveryPath));
    }
    return false;
}

bool removeExpectedAnchoredJournalFile(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const QList<AnchoredJournalFile> &files,
    const QString &name,
    const AtomicFileSnapshot &expected,
    bool required,
    QString &error)
{
    const AnchoredJournalFile *anchored =
        findAnchoredJournalFile(files, name);
    if (!anchored) {
        if (!required) return true;
        error = QStringLiteral(
            "A required linked-save journal cleanup file is missing");
        return false;
    }
    if (!anchoredJournalFileMatches(anchored, expected)) {
        error = QStringLiteral(
            "A linked-save journal cleanup file changed unexpectedly");
        return false;
    }
    return removeAnchoredJournalFile(directory, *anchored, error);
}

bool inspectJournalDirectory(
    const JournalState &state,
    QList<AnchoredJournalFile> &files,
    QString &error)
{
    files.clear();
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)) return false;
    const QFileInfoList entries = QDir(state.journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &info : entries) {
        const QString name = info.fileName();
        if (info.isSymLink() || !info.isFile()) {
            error = QStringLiteral(
                "The linked-save journal contains an unsafe entry");
            return false;
        }
        int intentIndex = -1;
        if (retirementIntentIndex(name, intentIndex)) {
            if (intentIndex < 0
                || intentIndex >= state.manifest.entries.size()) {
                error = QStringLiteral(
                    "A source-retirement intent index is invalid");
                return false;
            }
            if (atomicFilePathKey(resolved.at(intentIndex).source)
                == atomicFilePathKey(resolved.at(intentIndex).target)) {
                error = QStringLiteral(
                    "A source-retirement intent names an entry that does not retire its source");
                return false;
            }
            ObservedFile observed;
            if (!inspectRegularFile(
                    info.absoluteFilePath(),
                    observed,
                    error,
                    Detail::MaximumRetirementIntentSize)) {
                return false;
            }
            const AtomicFileSnapshot expected =
                retirementIntentSnapshot(state, intentIndex);
            if (!snapshotMatches(observed, expected)) {
                error = QStringLiteral(
                    "The linked-save journal contains an invalid source-retirement intent");
                return false;
            }
            const AnchoredRetirementSource *source =
                state.retirementSources.size()
                        == size_t(state.manifest.entries.size())
                    ? state.retirementSources.at(size_t(intentIndex)).get()
                    : nullptr;
            if (!source || !source->intent.expected
                || atomicFilePathKey(source->intent.path)
                    != atomicFilePathKey(info.absoluteFilePath())
                || source->intent.contents.size != expected.size
                || source->intent.contents.digest != expected.digest) {
                error = QStringLiteral(
                    "An incomplete source retirement requires manual recovery");
                return false;
            }
            if (!pinAnchoredJournalFile(
                    state.journalDirectory,
                    name,
                    Detail::MaximumRetirementIntentSize,
                    files,
                    error)) {
                return false;
            }
            continue;
        }
        bool activeIntentRecovery = false;
        for (const std::unique_ptr<AnchoredRetirementSource> &source :
             state.retirementSources) {
            if (!source || !source->intent.expected
                || source->intent.verifiedRecoveryPath.isEmpty()
                || atomicFilePathKey(
                       source->intent.verifiedRecoveryPath)
                    != atomicFilePathKey(info.absoluteFilePath())) {
                continue;
            }
            if (!source->intent.file.isValid()
                || !pinnedFileMatches(
                    source->intent.file, source->intent.contents)
                || !source->intent.parent.pathMatches(error)) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "A source-retirement intent recovery file changed unexpectedly");
                }
                return false;
            }
            AnchoredFileSystem::EntryRef recoveryEntry =
                source->intent.parent.entry(name, error);
            bool matches = false;
            if (!recoveryEntry.isValid()
                || !AnchoredFileSystem::entryMatches(
                    recoveryEntry,
                    source->intent.file,
                    matches,
                    error)
                || !matches) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "A source-retirement intent recovery file was replaced");
                }
                return false;
            }
            activeIntentRecovery = true;
            break;
        }
        if (activeIntentRecovery) continue;

        int temporaryIntentIndex = -1;
        const bool retirementIntentTemporary =
            retirementIntentTemporaryIndex(
                name, temporaryIntentIndex);
        if (retirementIntentTemporary) {
            if (temporaryIntentIndex < 0
                || temporaryIntentIndex
                    >= state.manifest.entries.size()) {
                error = QStringLiteral(
                    "A source-retirement intent index is invalid");
                return false;
            }
            if (atomicFilePathKey(
                    resolved.at(temporaryIntentIndex).source)
                == atomicFilePathKey(
                    resolved.at(temporaryIntentIndex).target)) {
                error = QStringLiteral(
                    "A source-retirement intent names an entry that does not retire its source");
                return false;
            }
        }
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName
            || expectedJournalDataName(state, name)) {
            const qint64 maximumSize = name == Detail::ManifestName
                ? Detail::MaximumManifestSize
                : (name == Detail::CommitMarkerName
                       ? Detail::MaximumCommitMarkerSize
                       : -1);
            if (!pinAnchoredJournalFile(
                    state.journalDirectory,
                    name,
                    maximumSize,
                    files,
                    error)) {
                return false;
            }
            continue;
        }
        if (!isKnownTemporaryName(name)
            && !retirementIntentTemporary
            && !isKnownLockName(name)) {
            error = QStringLiteral(
                "The linked-save journal contains an unknown file");
            return false;
        }
        const qint64 maximumSize = isKnownLockName(name)
            ? Detail::MaximumLockFileSize
            : (retirementIntentTemporary
                   ? Detail::MaximumRetirementIntentSize
                   : knownTemporaryMaximumSize(name));
        ObservedFile observed;
        if (!inspectRegularFile(
                info.absoluteFilePath(), observed, error, maximumSize)) {
            return false;
        }
        if (!pinAnchoredJournalFile(
                state.journalDirectory,
                name,
                maximumSize,
                files,
                error)) {
            return false;
        }
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const AnchoredJournalFile *sourceCopy =
            findAnchoredJournalFile(files, sourceCopyName(index));
        if (!anchoredJournalFileMatches(
                sourceCopy, entry.source.contents)) {
            error = QStringLiteral(
                "An activity source copy does not match its journal");
            return false;
        }
        const AnchoredJournalFile *backupCopy =
            findAnchoredJournalFile(files, backupCopyName(index));
        if (entry.backup.exists
            && !anchoredJournalFileMatches(
                backupCopy, entry.backup.contents)) {
            error = QStringLiteral(
                "An activity backup copy does not match its journal");
            return false;
        }
        if (!entry.backup.exists && backupCopy) {
            error = QStringLiteral(
                "An unexpected backup copy exists in the journal");
            return false;
        }

        const AnchoredJournalFile *staged =
            findAnchoredJournalFile(files, stagingName(index));
        if (entry.staged) {
            if (!anchoredJournalFileMatches(
                    staged, entry.stagedContents)) {
                error = QStringLiteral(
                    "A staged linked activity does not match its journal");
                return false;
            }
        }
    }
    return journalDirectoryMatches(state, error);
}

void resetRetirementIntent(AnchoredRetirementSource::Intent &intent)
{
    intent.file = {};
    intent.entry = {};
    intent.parent = {};
    intent.path.clear();
    intent.contents = {};
    intent.mutationEffect =
        AnchoredFileSystem::MutationEffect::NoEffect;
    intent.verifiedRecoveryPath.clear();
    intent.expected = false;
}

bool anchorRetirementIntent(
    AnchoredRetirementSource::Intent &intent,
    QString &error)
{
    if (!intent.expected || intent.path.isEmpty()) {
        error = QStringLiteral(
            "The source-retirement intent is unavailable");
        return false;
    }
    if (!intent.parent.isValid()
        && !AnchoredFileSystem::DirectoryAnchor::open(
            QFileInfo(intent.path).absolutePath(),
            intent.parent,
            error)) {
        error = QStringLiteral(
            "Cannot anchor a source-retirement intent: %1").arg(error);
        return false;
    }
    if (!intent.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The source-retirement journal directory was replaced");
        }
        return false;
    }
    if (!intent.entry.isValid()) {
        intent.entry = intent.parent.entry(
            QFileInfo(intent.path).fileName(), error);
        if (!intent.entry.isValid()) return false;
    }
    return true;
}

bool pinRetirementIntent(
    AnchoredRetirementSource::Intent &intent,
    QString &error)
{
    if (!anchorRetirementIntent(intent, error)) return false;
    AnchoredFileSystem::PinnedFile file;
    if (!AnchoredFileSystem::pinRegularFile(
            intent.entry,
            file,
            error,
            Detail::MaximumRetirementIntentSize)) {
        return false;
    }
    if (!pinnedFileMatches(file, intent.contents)) {
        error = QStringLiteral(
            "The source-retirement intent changed unexpectedly");
        return false;
    }
    intent.file = std::move(file);
    return true;
}

bool retirementIntentMatches(
    AnchoredRetirementSource::Intent &intent,
    QString &error)
{
    if (!intent.expected || !intent.file.isValid()
        || !anchorRetirementIntent(intent, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The source-retirement intent is unavailable");
        }
        return false;
    }
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            intent.entry, intent.file, matches, error)) {
        return false;
    }
    if (!matches) {
        error = QStringLiteral(
            "The source-retirement intent was removed or replaced before source mutation");
        return false;
    }
    return true;
}

bool beginRetirementIntent(
    JournalState &state,
    int index,
    AnchoredRetirementSource &source,
    QString &error)
{
    AnchoredRetirementSource::Intent &intent = source.intent;
    if (intent.expected) {
        error = QStringLiteral(
            "A source-retirement intent is already pending");
        return false;
    }
    intent.path = QDir(state.journalPath).filePath(
        retirementIntentName(index));
    intent.contents = retirementIntentSnapshot(state, index);
    intent.expected = true;

    AtomicFileSnapshot written;
    if (!writeBytesAtomically(
            intent.path,
            retirementIntentContents(state, index),
            AtomicFileMode::CreateNew,
            written,
            error)) {
        return false;
    }
    if (written.size != intent.contents.size
        || written.digest != intent.contents.digest) {
        error = QStringLiteral(
            "A published source-retirement intent is invalid");
        return false;
    }
    return pinRetirementIntent(intent, error);
}

bool verifyRetirementIntentAbsent(
    AnchoredRetirementSource::Intent &intent,
    QString &error)
{
    if (!anchorRetirementIntent(intent, error)) return false;
    bool exists = false;
    if (!AnchoredFileSystem::entryExists(
            intent.entry, exists, error)) {
        return false;
    }
    if (exists) {
        error = QStringLiteral(
            "A completed source-retirement intent still exists");
        return false;
    }
    return true;
}

bool completeRetirementIntent(
    AnchoredRetirementSource &source,
    QString &error)
{
    AnchoredRetirementSource::Intent &intent = source.intent;
    if (!intent.expected) return true;
    if (!anchorRetirementIntent(intent, error)) return false;

    if (intent.mutationEffect
        == AnchoredFileSystem::MutationEffect::NoEffect) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                intent.entry, exists, error)) {
            return false;
        }
        if (!exists) {
            if (!intent.parent.sync(error)) return false;
            intent.file = {};
            intent.mutationEffect =
                AnchoredFileSystem::MutationEffect::AppliedDurable;
        }
    }

    if (intent.mutationEffect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        if (!intent.parent.sync(error)) return false;
        intent.mutationEffect =
            AnchoredFileSystem::MutationEffect::AppliedDurable;
    } else if (intent.mutationEffect
                   == AnchoredFileSystem::MutationEffect::Partial
               && !intent.file.isValid()) {
        QString verificationError;
        if (verifyRetirementIntentAbsent(
                intent, verificationError)) {
            intent.mutationEffect =
                AnchoredFileSystem::MutationEffect::AppliedDurable;
            intent.verifiedRecoveryPath.clear();
        }
    }

    if (intent.mutationEffect
        != AnchoredFileSystem::MutationEffect::AppliedDurable) {
        if (!intent.file.isValid()) {
            bool exists = false;
            if (!AnchoredFileSystem::entryExists(
                    intent.entry, exists, error)) {
                return false;
            }
            if (!exists) {
                if (!intent.parent.sync(error)) return false;
                intent.mutationEffect =
                    AnchoredFileSystem::MutationEffect::AppliedDurable;
            } else if (!pinRetirementIntent(intent, error)) {
                return false;
            }
        }
        if (intent.mutationEffect
                != AnchoredFileSystem::MutationEffect::AppliedDurable
            && intent.file.isValid()) {
            const AnchoredFileSystem::MutationResult removed =
                AnchoredFileSystem::remove(intent.file);
            intent.mutationEffect = removed.effect;
            intent.verifiedRecoveryPath = removed.verifiedRecoveryPath;
            if (removed.effect
                == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
                if (!intent.parent.sync(error)) return false;
                intent.mutationEffect =
                    AnchoredFileSystem::MutationEffect::AppliedDurable;
            } else if (removed.effect
                           == AnchoredFileSystem::MutationEffect::Partial
                       && !intent.file.isValid()) {
                QString verificationError;
                if (verifyRetirementIntentAbsent(
                        intent, verificationError)) {
                    intent.mutationEffect =
                        AnchoredFileSystem::MutationEffect::AppliedDurable;
                    intent.verifiedRecoveryPath.clear();
                }
            }
            if (intent.mutationEffect
                != AnchoredFileSystem::MutationEffect::AppliedDurable) {
                error = removed.error.isEmpty()
                    ? QStringLiteral(
                        "Cannot durably remove a source-retirement intent")
                    : removed.error;
                if (!intent.verifiedRecoveryPath.isEmpty()) {
                    appendError(
                        error,
                        QStringLiteral("recovery file retained at %1")
                            .arg(intent.verifiedRecoveryPath));
                }
                return false;
            }
        }
    }
    if (intent.mutationEffect
            != AnchoredFileSystem::MutationEffect::AppliedDurable
        || !verifyRetirementIntentAbsent(intent, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot complete a source-retirement intent");
        }
        return false;
    }
    resetRetirementIntent(intent);
    return true;
}

bool completeRetirementIntents(JournalState &state, QString &error)
{
    for (const std::unique_ptr<AnchoredRetirementSource> &source :
         state.retirementSources) {
        if (source && !completeRetirementIntent(*source, error)) {
            return false;
        }
    }
    return true;
}

bool retirementRequired(const QList<ResolvedEntry> &resolved)
{
    for (const ResolvedEntry &paths : resolved) {
        if (atomicFilePathKey(paths.source)
            != atomicFilePathKey(paths.target)) {
            return true;
        }
    }
    return false;
}

void releaseTransactionResources(JournalState &state)
{
    state.retirementSources.clear();
    state.athleteRootDirectory = {};
    state.journalDirectory = {};
    state.namespaceDirectory = {};
    state.pathLocks.reset();
    state.transactionLease.reset();
}

bool preparedRetirementSourcesMatch(
    const JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    if (!state.athleteRootDirectory.isValid()) {
        error = QStringLiteral(
            "The anchored activity sources are unavailable");
        return false;
    }
    if (!state.athleteRootDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("The athlete root was replaced");
        }
        return false;
    }
    if (!retirementRequired(resolved)) return true;
    if (state.retirementSources.size()
        != size_t(state.manifest.entries.size())) {
        error = QStringLiteral(
            "The anchored activity sources are unavailable");
        return false;
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        if (atomicFilePathKey(resolved.at(index).source)
            == atomicFilePathKey(resolved.at(index).target)) {
            continue;
        }
        const std::unique_ptr<AnchoredRetirementSource> &source =
            state.retirementSources.at(size_t(index));
        if (!source || !source->file.isValid()
            || !pinnedFileMatches(
                source->file,
                state.manifest.entries.at(index).source.contents)) {
            error = QStringLiteral(
                "An anchored activity source does not match its journal");
            return false;
        }
        if (!source->parent.pathMatches(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "An activity source parent was replaced");
            }
            return false;
        }
        bool matches = false;
        if (!AnchoredFileSystem::entryMatches(
                source->entry, source->file, matches, error)) {
            return false;
        }
        if (!matches) {
            error = QStringLiteral(
                "An activity source was replaced after preparation");
            return false;
        }
    }
    return true;
}

bool capturePreparedRetirementSources(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    state.retirementSources.clear();
    state.athleteRootDirectory = {};
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            state.athleteRoot,
            state.athleteRootDirectory,
            error)) {
        error = QStringLiteral("Cannot anchor the athlete root: %1").arg(error);
        return false;
    }
    if (!retirementRequired(resolved)) {
        return preparedRetirementSourcesMatch(state, resolved, error);
    }

    state.retirementSources.resize(size_t(state.manifest.entries.size()));
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (atomicFilePathKey(resolved.at(index).source)
            == atomicFilePathKey(resolved.at(index).target)) {
            continue;
        }
        std::unique_ptr<AnchoredRetirementSource> source;
        if (!anchorRetirementSource(
                state.athleteRootDirectory,
                entry.source.relativePath,
                source,
                error)) {
            return false;
        }
        if (!pinnedFileMatches(source->file, entry.source.contents)) {
            error = QStringLiteral(
                "An activity source changed while it was being pinned");
            return false;
        }
        state.retirementSources[size_t(index)] = std::move(source);
    }
    return preparedRetirementSourcesMatch(state, resolved, error);
}

bool captureRecoveryRetirementSources(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    state.retirementSources.clear();
    state.athleteRootDirectory = {};
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            state.athleteRoot,
            state.athleteRootDirectory,
            error)) {
        error = QStringLiteral("Cannot anchor the athlete root: %1").arg(error);
        return false;
    }
    if (!state.athleteRootDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("The athlete root was replaced");
        }
        return false;
    }
    if (!retirementRequired(resolved)) return true;
    state.retirementSources.resize(size_t(state.manifest.entries.size()));
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (atomicFilePathKey(resolved.at(index).source)
            == atomicFilePathKey(resolved.at(index).target)) {
            continue;
        }
        std::unique_ptr<AnchoredRetirementSource> source;
        if (!anchorRetirementSource(
                state.athleteRootDirectory,
                entry.source.relativePath,
                source,
                error)) {
            return false;
        }
        if (source->file.isValid()) {
            error = QStringLiteral(
                "A committed linked activity source was recreated after publication");
            return false;
        }
        state.retirementSources[size_t(index)] = std::move(source);
    }
    return true;
}

bool verifyRetiredSourceAbsent(
    AnchoredRetirementSource &source, QString &error)
{
    if (!source.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An activity source parent changed during retirement");
        }
        return false;
    }
    bool exists = false;
    if (!AnchoredFileSystem::entryExists(
            source.entry, exists, error)) {
        return false;
    }
    if (exists) {
        error = QStringLiteral(
            "An activity source name was repopulated during retirement");
        return false;
    }
    source.completionVerified = true;
    return true;
}

bool retireAnchoredSource(
    AnchoredRetirementSource &source,
    const char *validatedTransition,
    const char *retiredTransition,
    QString &error)
{
    if (!source.parent.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("An activity source parent was replaced");
        }
        return false;
    }
    if (!source.file.isValid()) {
        bool exists = false;
        if (!AnchoredFileSystem::entryExists(
                source.entry, exists, error)) {
            return false;
        }
        if (!exists) {
            source.completionVerified = true;
            return true;
        }
        error = QStringLiteral(
            "An activity source appeared during transaction recovery");
        return false;
    }

    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            source.entry, source.file, matches, error)) {
        return false;
    }
    if (!matches) {
        error = QStringLiteral(
            "An activity source was replaced before retirement");
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    if (validatedTransition) {
        linkedActivitySaveTransitionReached(validatedTransition);
    }
#else
    Q_UNUSED(validatedTransition)
#endif
    if (source.intent.expected
        && !retirementIntentMatches(source.intent, error)) {
        return false;
    }

    const AnchoredFileSystem::MutationResult removed =
        AnchoredFileSystem::remove(source.file);
    source.mutationEffect = removed.effect;
    source.verifiedRecoveryPath = removed.verifiedRecoveryPath;
    source.completionVerified = false;
    if (removed.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        if (retiredTransition) {
            linkedActivitySaveTransitionReached(retiredTransition);
        }
#else
        Q_UNUSED(retiredTransition)
#endif
        return verifyRetiredSourceAbsent(source, error);
    }
    error = removed.error.isEmpty()
        ? QStringLiteral("Cannot retire an anchored activity source")
        : removed.error;
    if (!removed.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery file retained at %1")
                .arg(removed.verifiedRecoveryPath));
    }
    return false;
}

bool retireJournalSource(
    JournalState &state,
    int index,
    const char *validatedTransition,
    const char *retiredTransition,
    QString &error)
{
    if (index < 0
        || index >= state.manifest.entries.size()
        || state.retirementSources.size()
            != size_t(state.manifest.entries.size())) {
        error = QStringLiteral(
            "The anchored activity source is unavailable");
        return false;
    }
    AnchoredRetirementSource *source =
        state.retirementSources.at(size_t(index)).get();
    if (!source) {
        error = QStringLiteral(
            "The anchored activity source is unavailable");
        return false;
    }

    const bool intentPublished = source->file.isValid();
    if (intentPublished) {
        if (!beginRetirementIntent(state, index, *source, error)) {
            return false;
        }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        linkedActivitySaveTransitionReached(
            "linked-save-retirement-intent-published");
#endif
    }
    if (!retireAnchoredSource(
            *source,
            validatedTransition,
            retiredTransition,
            error)) {
        return false;
    }
    if (!completeRetirementIntent(*source, error)) return false;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    if (intentPublished) {
        linkedActivitySaveTransitionReached(
            "linked-save-retirement-intent-removed");
    }
#endif
    return true;
}

bool resolveIncompleteRetirements(
    JournalState &state, QString &error)
{
    for (const std::unique_ptr<AnchoredRetirementSource> &sourcePtr :
         state.retirementSources) {
        if (!sourcePtr) continue;
        AnchoredRetirementSource &source = *sourcePtr;
        if (source.mutationEffect
            == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
            if (!source.parent.sync(error)) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "Cannot synchronize an incomplete source retirement");
                }
                return false;
            }
            source.mutationEffect =
                AnchoredFileSystem::MutationEffect::AppliedDurable;
        } else if (source.mutationEffect
                   == AnchoredFileSystem::MutationEffect::Partial) {
            if (!source.file.isValid()) {
                QString verificationError;
                if (verifyRetiredSourceAbsent(
                        source, verificationError)) {
                    source.mutationEffect =
                        AnchoredFileSystem::MutationEffect::AppliedDurable;
                    source.verifiedRecoveryPath.clear();
                } else {
                    error = QStringLiteral(
                        "An incomplete source retirement requires manual recovery");
                    appendError(error, verificationError);
                    if (!source.verifiedRecoveryPath.isEmpty()) {
                        appendError(
                            error,
                            QStringLiteral("recovery file retained at %1")
                                .arg(source.verifiedRecoveryPath));
                    }
                    return false;
                }
            } else {
                const AnchoredFileSystem::MutationResult cleanup =
                    AnchoredFileSystem::remove(source.file);
                source.mutationEffect = cleanup.effect;
                source.verifiedRecoveryPath = cleanup.verifiedRecoveryPath;
                if (cleanup.effect
                    == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
                    if (!source.parent.sync(error)) return false;
                    source.mutationEffect =
                        AnchoredFileSystem::MutationEffect::AppliedDurable;
                } else if (cleanup.effect
                           != AnchoredFileSystem::MutationEffect::AppliedDurable) {
                    error = cleanup.error.isEmpty()
                        ? QStringLiteral(
                            "Cannot resolve an incomplete source retirement")
                        : cleanup.error;
                    if (!cleanup.verifiedRecoveryPath.isEmpty()) {
                        appendError(
                            error,
                            QStringLiteral("recovery file retained at %1")
                                .arg(cleanup.verifiedRecoveryPath));
                    }
                    return false;
                }
            }
        }
        if (source.mutationEffect
                == AnchoredFileSystem::MutationEffect::AppliedDurable
            && !source.completionVerified
            && !verifyRetiredSourceAbsent(source, error)) {
            return false;
        }
    }
    return true;
}

bool verifyRetirementSourcesAbsent(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    if (!retirementRequired(resolved)) return true;
    if (state.retirementSources.size()
            != size_t(state.manifest.entries.size())
        || !state.athleteRootDirectory.isValid()) {
        error = QStringLiteral(
            "The anchored activity sources are unavailable");
        return false;
    }
    if (!state.athleteRootDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("The athlete root was replaced");
        }
        return false;
    }
    for (int index = 0; index < resolved.size(); ++index) {
        if (atomicFilePathKey(resolved.at(index).source)
            == atomicFilePathKey(resolved.at(index).target)) {
            continue;
        }
        AnchoredRetirementSource *source =
            state.retirementSources.at(size_t(index)).get();
        if (!source || !verifyRetiredSourceAbsent(*source, error)) {
            if (!source && error.isEmpty()) {
                error = QStringLiteral(
                    "The anchored activity source is unavailable");
            }
            return false;
        }
    }
    return true;
}

bool observeProductionEntry(
    JournalState &state,
    const Entry &entry,
    const ResolvedEntry &paths,
    ObservedFile &source,
    ObservedFile &target,
    ObservedFile &backup,
    QString &error)
{
    if (!observeActivityFile(
            state, entry.source.relativePath, source, error)) {
        return false;
    }
    if (atomicFilePathKey(paths.source) == atomicFilePathKey(paths.target)) {
        target = source;
    } else if (!observeActivityFile(
                   state, entry.targetRelativePath, target, error)) {
        return false;
    }
    if (!observeActivityFile(
            state, entry.backup.relativePath, backup, error)) {
        return false;
    }

    const bool sourceIsTarget =
        atomicFilePathKey(paths.source) == atomicFilePathKey(paths.target);
    const bool sourceKnown = !source.exists
        || snapshotMatches(source, entry.source.contents)
        || (sourceIsTarget && entry.staged
            && snapshotMatches(source, entry.stagedContents));
    const bool targetKnown = !target.exists
        || snapshotMatches(target, entry.source.contents)
        || (entry.staged
            && snapshotMatches(target, entry.stagedContents));
    const bool backupKnown = !backup.exists
        || (entry.backup.exists
            && snapshotMatches(backup, entry.backup.contents))
        || snapshotMatches(backup, entry.source.contents);
    if (!sourceKnown || !targetKnown || !backupKnown) {
        error = QStringLiteral(
            "A linked activity file changed outside its transaction");
        return false;
    }
    return true;
}

bool verifyOldGeneration(
    const JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (!validateExpectedSnapshot(
                paths.source, entry.source.contents, error)) {
            return false;
        }
        if (atomicFilePathKey(paths.source)
                != atomicFilePathKey(paths.target)
            && pathEntryExists(paths.target)) {
            error = QStringLiteral(
                "A rolled-back linked activity target still exists");
            return false;
        }
        ObservedFile backup;
        if (!inspectRegularFile(paths.backup, backup, error)) return false;
        if (entry.backup.exists) {
            if (!snapshotMatches(backup, entry.backup.contents)) {
                error = QStringLiteral(
                    "A previous activity backup was not restored");
                return false;
            }
        } else if (backup.exists) {
            error = QStringLiteral(
                "An activity backup created by the transaction still exists");
            return false;
        }
    }
    return true;
}

bool verifyNewGeneration(
    const JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (!entry.staged
            || !validateExpectedSnapshot(
                paths.target, entry.stagedContents, error)) {
            return false;
        }
        if (atomicFilePathKey(paths.source)
                != atomicFilePathKey(paths.target)
            && pathEntryExists(paths.source)) {
            error = QStringLiteral(
                "A committed linked activity source still exists");
            return false;
        }
        ObservedFile backup;
        if (!inspectRegularFile(paths.backup, backup, error)) return false;
        if (entry.keepSourceBackup) {
            if (!snapshotMatches(backup, entry.source.contents)) {
                error = QStringLiteral(
                    "A converted activity backup was not published");
                return false;
            }
        } else if (entry.backup.exists) {
            if (!snapshotMatches(backup, entry.backup.contents)) {
                error = QStringLiteral(
                    "An unrelated activity backup changed during saving");
                return false;
            }
        } else if (backup.exists) {
            error = QStringLiteral(
                "An unexpected activity backup was created during saving");
            return false;
        }
    }
    return true;
}

bool restoreOldGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        ObservedFile source;
        ObservedFile target;
        ObservedFile backup;
        if (!observeProductionEntry(
                state,
                state.manifest.entries.at(index),
                resolved.at(index),
                source,
                target,
                backup,
                error)) {
            return false;
        }
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        ObservedFile source;
        ObservedFile target;
        ObservedFile backup;
        if (!observeProductionEntry(
                state,
                entry, paths, source, target, backup, error)) {
            return false;
        }

        const bool sourceNeedsRestore =
            !snapshotMatches(source, entry.source.contents);
        if (sourceNeedsRestore
            && !replaceObservedActivityFile(
                state,
                paths.sourceCopy,
                entry.source.contents,
                entry.source.relativePath,
                source,
                "linked-save-before-source-restore",
                error)) {
            return false;
        }
        if (entry.backup.exists) {
            const bool backupNeedsRestore =
                !snapshotMatches(backup, entry.backup.contents);
            if (backupNeedsRestore
                && !replaceObservedActivityFile(
                    state,
                    paths.backupCopy,
                    entry.backup.contents,
                    entry.backup.relativePath,
                    backup,
                    "linked-save-before-backup-restore",
                    error)) {
                return false;
            }
        } else if (backup.exists) {
            if (!removeObservedActivityFile(
                    state,
                    entry.backup.relativePath,
                    backup,
                    "linked-save-before-backup-remove",
                    error)) {
                return false;
            }
        }

        if (atomicFilePathKey(paths.source)
                != atomicFilePathKey(paths.target)) {
            ObservedFile currentTarget;
            if (!observeActivityFile(
                    state,
                    entry.targetRelativePath,
                    currentTarget,
                    error)) {
                return false;
            }
            if (currentTarget.exists) {
                if (!removeObservedActivityFile(
                        state,
                        entry.targetRelativePath,
                        currentTarget,
                        "linked-save-before-target-remove",
                        error)) {
                    return false;
                }
            }
        }
    }
    return verifyOldGeneration(state, resolved, error);
}

bool ensureNewGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    if (!captureRecoveryRetirementSources(state, resolved, error)) {
        return false;
    }
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (!entry.staged
            || !validateExpectedSnapshot(
                paths.staging, entry.stagedContents, error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "A committed linked activity has no staged generation");
            }
            return false;
        }
        ObservedFile source;
        ObservedFile target;
        ObservedFile backup;
        if (!observeProductionEntry(
                state,
                entry, paths, source, target, backup, error)) {
            return false;
        }
        const bool targetNeedsPublication =
            !snapshotMatches(target, entry.stagedContents);
        if (targetNeedsPublication
            && !replaceObservedActivityFile(
                state,
                paths.staging,
                entry.stagedContents,
                entry.targetRelativePath,
                target,
                "linked-save-before-recovery-target-publish",
                error)) {
            return false;
        }
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        ObservedFile source;
        ObservedFile target;
        ObservedFile backup;
        if (!observeProductionEntry(
                state,
                entry, paths, source, target, backup, error)) {
            return false;
        }
        if (entry.keepSourceBackup) {
            const bool backupNeedsPublication =
                !snapshotMatches(backup, entry.source.contents);
            if (backupNeedsPublication
                && !replaceObservedActivityFile(
                    state,
                    paths.sourceCopy,
                    entry.source.contents,
                    entry.backup.relativePath,
                    backup,
                    "linked-save-before-recovery-backup-publish",
                    error)) {
                return false;
            }
        }
    }
    return verifyNewGeneration(state, resolved, error);
}

bool publishNewGeneration(
    JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    if (!preparedRetirementSourcesMatch(state, resolved, error)
        || !verifyOldGeneration(state, resolved, error)) {
        return false;
    }
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (!entry.staged
            || !validateExpectedSnapshot(
                resolved.at(index).staging,
                entry.stagedContents,
                error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Every linked activity must be staged before publication");
            }
            return false;
        }
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        ObservedFile source;
        ObservedFile target;
        ObservedFile backup;
        if (!observeProductionEntry(
                state,
                entry, paths, source, target, backup, error)
            || !replaceObservedActivityFile(
                state,
                paths.staging,
                entry.stagedContents,
                entry.targetRelativePath,
                target,
                nullptr,
                error)) {
            return false;
        }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        linkedActivitySaveTransitionReached("linked-save-target-published");
#endif
    }

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (entry.keepSourceBackup) {
            ObservedFile source;
            ObservedFile target;
            ObservedFile backup;
            if (!observeProductionEntry(
                    state,
                    entry, paths, source, target, backup, error)
                || !replaceObservedActivityFile(
                    state,
                    paths.sourceCopy,
                    entry.source.contents,
                    entry.backup.relativePath,
                    backup,
                    nullptr,
                    error)) {
                return false;
            }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
            linkedActivitySaveTransitionReached("linked-save-backup-published");
#endif
        }
        if (atomicFilePathKey(paths.source)
                != atomicFilePathKey(paths.target)) {
            if (!retireJournalSource(
                    state,
                    index,
                    "linked-save-source-retirement-validated",
                    "linked-save-source-retired",
                    error)) {
                return false;
            }
        }
    }
    return verifyNewGeneration(state, resolved, error);
}

bool removeJournalDirectory(
    JournalState &state,
    bool committed,
    QString &error)
{
    if (state.terminalCleanupCompleted) return true;
    if (state.journalRemovalEffect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        if (!state.namespaceDirectory.isValid()
            || !state.namespaceDirectory.sync(error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "Cannot synchronize the removed linked-save journal");
            }
            return false;
        }
        state.journalRemovalEffect =
            AnchoredFileSystem::MutationEffect::AppliedDurable;
        state.terminalCleanupCompleted = true;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        linkedActivitySaveTransitionReached("linked-save-directory-removed");
#endif
        return true;
    }
    if (state.journalRemovalEffect
        != AnchoredFileSystem::MutationEffect::NoEffect) {
        error = QStringLiteral(
            "Incomplete linked-save journal removal requires manual recovery");
        if (!state.journalRemovalRecoveryPath.isEmpty()) {
            appendError(
                error,
                QStringLiteral("recovery directory retained at %1")
                    .arg(state.journalRemovalRecoveryPath));
        }
        return false;
    }
    if (!journalDirectoryMatches(state, error)) return false;

    QList<AnchoredJournalFile> files;
    if (!inspectJournalDirectory(state, files, error)) return false;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-journal-files-pinned");
#endif

    const auto expectedCleanupName = [&](const QString &name) {
        if (name == Detail::ManifestName
            || (committed && name == Detail::CommitMarkerName)) {
            return true;
        }
        for (int index = 0; index < state.manifest.entries.size(); ++index) {
            const Entry &entry = state.manifest.entries.at(index);
            if (name == sourceCopyName(index)
                || (entry.backup.exists
                    && name == backupCopyName(index))
                || (entry.staged && name == stagingName(index))) {
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

    AtomicFileSnapshot marker;
    if (committed) {
        markerSnapshot(state, marker);
        if (!anchoredJournalFileMatches(
                findAnchoredJournalFile(files, Detail::CommitMarkerName),
                marker)) {
            error = QStringLiteral(
                "The linked-save commit marker changed before cleanup");
            return false;
        }
    }

    if (!removeExpectedAnchoredJournalFile(
            state.journalDirectory,
            files,
            Detail::ManifestName,
            state.manifestSnapshot,
            true,
            error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-manifest-removed");
#endif

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        if (!removeExpectedAnchoredJournalFile(
                state.journalDirectory,
                files,
                sourceCopyName(index),
                entry.source.contents,
                true,
                error)) {
            return false;
        }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        linkedActivitySaveTransitionReached("linked-save-cleanup-file");
#endif
        if (entry.backup.exists
            && !removeExpectedAnchoredJournalFile(
                state.journalDirectory,
                files,
                backupCopyName(index),
                entry.backup.contents,
                true,
                error)) {
            return false;
        }
        if (entry.backup.exists) {
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
            linkedActivitySaveTransitionReached("linked-save-cleanup-file");
#endif
        }
        if (entry.staged
            && !removeExpectedAnchoredJournalFile(
                state.journalDirectory,
                files,
                stagingName(index),
                entry.stagedContents,
                true,
                error)) {
            return false;
        }
        if (entry.staged) {
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
            linkedActivitySaveTransitionReached("linked-save-cleanup-file");
#endif
        }
    }

    if (committed
        && !removeExpectedAnchoredJournalFile(
            state.journalDirectory,
            files,
            Detail::CommitMarkerName,
            marker,
            true,
            error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    if (committed) {
        linkedActivitySaveTransitionReached("linked-save-commit-marker-removed");
    }
#endif

    files.clear();
    const QFileInfoList remaining = QDir(state.journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System);
    if (!remaining.isEmpty()) {
        error = QStringLiteral(
            "The linked-save journal contains files that cannot be removed");
        return false;
    }
    if (!journalDirectoryMatches(state, error)) return false;
    const AnchoredFileSystem::MutationResult removed =
        AnchoredFileSystem::removeEmptyDirectory(state.journalDirectory);
    state.journalRemovalEffect = removed.effect;
    state.journalRemovalRecoveryPath = removed.verifiedRecoveryPath;
    if (state.journalRemovalEffect
        == AnchoredFileSystem::MutationEffect::AppliedNotDurable) {
        QString syncError;
        if (!state.namespaceDirectory.isValid()
            || !state.namespaceDirectory.sync(syncError)) {
            error = removed.error.isEmpty()
                ? QStringLiteral(
                    "Cannot synchronize the removed linked-save journal")
                : removed.error;
            appendError(error, syncError);
            return false;
        }
        state.journalRemovalEffect =
            AnchoredFileSystem::MutationEffect::AppliedDurable;
    }
    if (state.journalRemovalEffect
        != AnchoredFileSystem::MutationEffect::AppliedDurable) {
        error = removed.error.isEmpty()
            ? QStringLiteral("Cannot remove the completed linked-save journal")
            : removed.error;
        if (!state.journalRemovalRecoveryPath.isEmpty()) {
            appendError(
                error,
                QStringLiteral("recovery directory retained at %1")
                    .arg(state.journalRemovalRecoveryPath));
        }
        return false;
    }
    state.terminalCleanupCompleted = true;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-directory-removed");
#endif
    return true;
}

bool rollbackJournal(JournalState &state, QString &error)
{
    if (state.journalRemovalEffect
        != AnchoredFileSystem::MutationEffect::NoEffect) {
        if (!removeJournalDirectory(state, false, error)) return false;
        releaseTransactionResources(state);
        return true;
    }
    if (!journalDirectoryMatches(state, error)) return false;
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(state, journalFiles, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    if (committed) {
        error = QStringLiteral("Cannot roll back a committed linked save");
        return false;
    }
    if (!resolveIncompleteRetirements(state, error)) return false;
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !restoreOldGeneration(state, resolved, error)) {
        return false;
    }
    if (!completeRetirementIntents(state, error)) return false;
    if (!removeJournalDirectory(state, false, error)) return false;
    releaseTransactionResources(state);
    return true;
}

bool commitJournal(JournalState &state, QString &error)
{
    if (state.journalRemovalEffect
        != AnchoredFileSystem::MutationEffect::NoEffect) {
        if (!removeJournalDirectory(state, true, error)) return false;
        releaseTransactionResources(state);
        return true;
    }
    if (!journalDirectoryMatches(state, error)) return false;
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(state, journalFiles, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    if (!committed) {
        error = QStringLiteral("Cannot complete an uncommitted linked save");
        return false;
    }
    if (!resolveIncompleteRetirements(state, error)) return false;
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !ensureNewGeneration(state, resolved, error)
        || !verifyRetirementSourcesAbsent(state, resolved, error)) {
        return false;
    }
    if (!removeJournalDirectory(state, true, error)) return false;
    releaseTransactionResources(state);
    return true;
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
    const QString &namespacePath,
    const QString &journalPath,
    QString &error)
{
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)
        || !ensurePrivateDirectory(journalPath, error)) {
        return false;
    }

    AtomicFileLockSet locks;
    if (!locks.lock({journalPath}, error)) return false;

    AnchoredFileSystem::DirectoryAnchor namespaceDirectory;
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            namespacePath, namespaceDirectory, error)
        || !namespaceDirectory.openChild(
            id, journalDirectory, error)
        || !namespaceDirectory.pathMatches(error)
        || !journalDirectory.pathMatches(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot anchor an incomplete linked-save journal");
        }
        return false;
    }

    const QFileInfoList entries = QDir(journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    QList<AnchoredJournalFile> removable;
    for (const QFileInfo &entry : entries) {
        qint64 maximumSize = -1;
        if (entry.isSymLink() || !entry.isFile()
            || !maximumSizeForPreManifestEntry(
                entry.fileName(), maximumSize)) {
            error = QStringLiteral(
                "An incomplete linked-save transaction contains an unknown entry");
            return false;
        }
        ObservedFile observed;
        if (!inspectRegularFile(
                entry.absoluteFilePath(), observed, error, maximumSize)) {
            return false;
        }
        if (!pinAnchoredJournalFile(
                journalDirectory,
                entry.fileName(),
                maximumSize,
                removable,
                error)) {
            return false;
        }
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-pre-manifest-scanned");
#endif
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
            "Cannot remove an incomplete linked-save transaction")
        : removal.error;
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        appendError(
            error,
            QStringLiteral("recovery directory retained at %1")
                .arg(removal.verifiedRecoveryPath));
    }
    return false;
}

bool captureOptionalFile(
    const QString &path, ExpectedFile &expected, QString &error)
{
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    expected.exists = observed.exists;
    expected.contents = observed.contents;
    return true;
}

bool revalidateOriginalGeneration(
    const JournalState &state,
    const QList<ResolvedEntry> &resolved,
    QString &error)
{
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (!validateExpectedSnapshot(
                paths.source, entry.source.contents, error)) {
            return false;
        }
        if (atomicFilePathKey(paths.source)
                != atomicFilePathKey(paths.target)
            && pathEntryExists(paths.target)) {
            error = QStringLiteral(
                "Cannot save linked activities because a target already exists");
            return false;
        }
        ObservedFile backup;
        if (!inspectRegularFile(paths.backup, backup, error)) return false;
        if (entry.backup.exists != backup.exists
            || (entry.backup.exists
                && !snapshotMatches(backup, entry.backup.contents))) {
            error = QStringLiteral(
                "An activity backup changed while preparing the linked save");
            return false;
        }
    }
    return true;
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
    if (specification.entries.size() < 2
        || specification.entries.size() > Detail::MaximumEntries) {
        error = QStringLiteral(
            "A linked-save transaction requires a bounded activity set");
        return {};
    }

    QString root;
    if (!normalizeAthleteRoot(specification.athleteRoot, root, error)) {
        return {};
    }

    std::shared_ptr<JournalState> state(new JournalState);
    state->athleteRoot = root;
    state->transactionLease = std::make_unique<AtomicFileLockSet>();
    if (!state->transactionLease->lock(
            {transactionLeaseTarget(root)}, error)) {
        error = QStringLiteral(
            "Another linked activity transaction is already active: %1")
                    .arg(error);
        return {};
    }
    if (!ensureTransactionNamespace(root, state->namespacePath, error)
        || !transactionNamespacesAreReady(
            root, state->namespacePath, error)) {
        return {};
    }

    state->manifest.id = QUuid::createUuid()
                             .toString(QUuid::WithoutBraces)
                             .toLower();
    state->journalPath = QDir(state->namespacePath).filePath(
        state->manifest.id);
    state->manifestPath = QDir(state->journalPath).filePath(
        Detail::ManifestName);
    state->commitMarkerPath = QDir(state->journalPath).filePath(
        Detail::CommitMarkerName);

    for (int index = 0; index < specification.entries.size(); ++index) {
        const EntrySpecification &requested =
            specification.entries.at(index);
        Entry entry;
        QString sourceAbsolute;
        QString targetAbsolute;
        QString backupAbsolute;
        if (!makeRootRelativePath(
                root,
                requested.sourcePath,
                entry.source.relativePath,
                sourceAbsolute,
                error)
            || !makeRootRelativePath(
                root,
                requested.targetPath,
                entry.targetRelativePath,
                targetAbsolute,
                error)
            || !makeRootRelativePath(
                root,
                requested.backupPath,
                entry.backup.relativePath,
                backupAbsolute,
                error)) {
            return {};
        }
        entry.source.exists = true;
        if (!captureAtomicFileSnapshot(
                sourceAbsolute, entry.source.contents, error)) {
            return {};
        }
        if (!captureOptionalFile(
                backupAbsolute, entry.backup, error)) {
            return {};
        }
        entry.keepSourceBackup = requested.keepSourceBackup;
        if (atomicFilePathKey(sourceAbsolute)
                != atomicFilePathKey(targetAbsolute)
            && pathEntryExists(targetAbsolute)) {
            error = QStringLiteral(
                "Cannot save linked activities because a target already exists");
            return {};
        }
        state->manifest.entries.append(entry);
    }

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state, resolved, error)) return {};
    state->pathLocks = std::make_unique<AtomicFileLockSet>();
    if (!state->pathLocks->lock(productionLockPaths(resolved), error)) {
        error = QStringLiteral("A linked activity file is already being saved: %1")
                    .arg(error);
        return {};
    }
    if (!revalidateOriginalGeneration(*state, resolved, error)
        || !capturePreparedRetirementSources(*state, resolved, error)
        || !revalidateOriginalGeneration(*state, resolved, error)) {
        return {};
    }

    if (!QDir().mkdir(state->journalPath)
        || !makeDirectoryPrivate(state->journalPath, error)
        || !syncParentDirectory(state->journalPath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot create the linked-save transaction journal");
        }
        return {};
    }
    if (!anchorJournalDirectory(*state, error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1")
                .arg(state->journalPath));
        return {};
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-directory-created");
#endif

    for (int index = 0; index < state->manifest.entries.size(); ++index) {
        const Entry &entry = state->manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (!copyExpectedFileAtomically(
                paths.source,
                paths.sourceCopy,
                entry.source.contents,
                AtomicFileMode::CreateNew,
                error)) {
            appendError(
                error,
                QStringLiteral("recovery journal retained at %1")
                    .arg(state->journalPath));
            return {};
        }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        linkedActivitySaveTransitionReached("linked-save-source-copy-published");
#endif
        if (entry.backup.exists
            && !copyExpectedFileAtomically(
                paths.backup,
                paths.backupCopy,
                entry.backup.contents,
                AtomicFileMode::CreateNew,
                error)) {
            appendError(
                error,
                QStringLiteral("recovery journal retained at %1")
                    .arg(state->journalPath));
            return {};
        }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
        if (entry.backup.exists) {
            linkedActivitySaveTransitionReached("linked-save-backup-copy-published");
        }
#endif
    }
    if (!writeManifestFile(*state, false, error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1")
                .arg(state->journalPath));
        return {};
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-initial-manifest-published");
#endif
    return std::shared_ptr<Journal>(new Journal(state));
}

bool Journal::reconcileAll(const QString &athleteRoot, QString &error)
{
    error.clear();
    if (athleteRoot.isEmpty() || !QDir::isAbsolutePath(athleteRoot)) {
        error = QStringLiteral("The athlete root must be an absolute path");
        return false;
    }
    const QString requestedRoot = QDir::cleanPath(
        QFileInfo(athleteRoot).absoluteFilePath());
    const QFileInfo requestedRootInfo(requestedRoot);
    if (!requestedRootInfo.exists() || !requestedRootInfo.isDir()) {
        error = QStringLiteral("The athlete root is unavailable");
        return false;
    }

    AtomicFileLockSet transactionLease;
    if (!transactionLease.lock(
            {transactionLeaseTarget(requestedRoot)}, error)) {
        error = QStringLiteral(
            "A linked activity transaction is already active: %1")
                    .arg(error);
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
    const QString transactions = QDir(root).filePath(
        QStringLiteral(".gc-transactions"));
    const QFileInfo transactionsInfo(transactions);
    if (!transactionsInfo.exists() && !transactionsInfo.isSymLink()) {
        return true;
    }
    if (!ensurePrivateDirectory(transactions, error)) return false;

    const QString namespacePath = transactionNamespacePath(root);
    const QFileInfo namespaceInfo(namespacePath);
    if (!namespaceInfo.exists() && !namespaceInfo.isSymLink()) return true;
    if (!ensurePrivateDirectory(namespacePath, error)) return false;

    const QFileInfoList entries = QDir(namespacePath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    QStringList failures;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        QString lockedId;
        if (entry.isFile() && !entry.isSymLink()
            && atomicFileLockTargetName(name, lockedId)) {
            if (validTransactionId(lockedId)) continue;
        }
        if (entry.isSymLink() || !entry.isDir()
            || !validTransactionId(name)) {
            failures.append(QStringLiteral(
                "Unknown entry in the linked-save journal namespace: %1")
                                .arg(name));
            continue;
        }

        const QString manifestPath = QDir(entry.absoluteFilePath()).filePath(
            Detail::ManifestName);
        const QFileInfo manifestInfo(manifestPath);
        QString transactionError;
        if (!manifestInfo.exists() && !manifestInfo.isSymLink()) {
            if (!removePreManifestJournal(
                    namespacePath,
                    entry.absoluteFilePath(),
                    transactionError)) {
                failures.append(transactionError);
            }
            continue;
        }

        std::shared_ptr<JournalState> state;
        if (!loadManifestState(
                root,
                entry.absoluteFilePath(),
                state,
                transactionError)) {
            failures.append(transactionError);
            continue;
        }
        bool committed = false;
        if (!readCommitMarker(*state, committed, transactionError)
            || !(committed
                ? commitJournal(*state, transactionError)
                : rollbackJournal(*state, transactionError))) {
            failures.append(transactionError);
        }
    }

    if (!failures.isEmpty()) {
        error = failures.join(QStringLiteral("; "));
        return false;
    }
    return true;
}

int Journal::entryCount() const
{
    return state_ ? state_->manifest.entries.size() : 0;
}

QString Journal::stagingPath(int index) const
{
    if (!state_ || index < 0
        || index >= state_->manifest.entries.size()) {
        return {};
    }
    return QDir(state_->journalPath).filePath(stagingName(index));
}

QString Journal::directoryPath() const
{
    return state_ ? state_->journalPath : QString();
}

bool Journal::recordStaged(int index, QString &error)
{
    error.clear();
    if (!state_ || index < 0
        || index >= state_->manifest.entries.size()) {
        error = QStringLiteral("The linked-save journal entry is unavailable");
        return false;
    }
    if (!validateExpectedSnapshot(
            state_->manifestPath,
            state_->manifestSnapshot,
            error)) {
        return false;
    }
    const QString path = stagingPath(index);
    AtomicFileSnapshot staged;
    if (!captureAtomicFileSnapshot(path, staged, error)) return false;

    Entry &entry = state_->manifest.entries[index];
    if (entry.staged) {
        if (entry.stagedContents.size != staged.size
            || entry.stagedContents.digest != staged.digest) {
            error = QStringLiteral(
                "A staged linked activity changed unexpectedly");
            return false;
        }
        return true;
    }
    entry.staged = true;
    entry.stagedContents = staged;
    if (!writeManifestFile(*state_, true, error)) return false;
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-stage-recorded");
#endif
    return true;
}

bool Journal::publishAndCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-save journal is unavailable");
        return false;
    }
    if (!validateExpectedSnapshot(
            state_->manifestPath,
            state_->manifestSnapshot,
            error)) {
        return false;
    }
    QList<AnchoredJournalFile> journalFiles;
    if (!inspectJournalDirectory(*state_, journalFiles, error)) return false;
    const auto expectedJournalName = [&](const QString &name) {
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName) {
            return true;
        }
        for (int index = 0; index < state_->manifest.entries.size(); ++index) {
            const Entry &entry = state_->manifest.entries.at(index);
            if (name == sourceCopyName(index)
                || (entry.backup.exists
                    && name == backupCopyName(index))
                || (entry.staged && name == stagingName(index))) {
                return true;
            }
        }
        return false;
    };
    for (const AnchoredJournalFile &file :
         std::as_const(journalFiles)) {
        if (!expectedJournalName(file.name)
            && !removeAnchoredJournalFile(
                state_->journalDirectory, file, error)) {
            return false;
        }
    }
    bool committed = false;
    if (!readCommitMarker(*state_, committed, error)) return false;
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state_, resolved, error)) return false;
    if (committed) {
        if (!verifyNewGeneration(*state_, resolved, error)
            || !verifyRetirementSourcesAbsent(
                *state_, resolved, error)) {
            appendError(
                error,
                QStringLiteral(
                    "committed linked activity recovery is required before continuing"));
            return false;
        }
        return true;
    }

    if (!publishNewGeneration(*state_, resolved, error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached(
        "linked-save-before-final-retirement-check");
#endif
    if (!verifyRetirementSourcesAbsent(*state_, resolved, error)) {
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached(
        "linked-save-after-final-retirement-check");
#endif
    if (!verifyRetirementSourcesAbsent(*state_, resolved, error)) {
        return false;
    }

    const QByteArray contents = state_->manifest.id.toLatin1() + '\n';
    AtomicFileSnapshot marker;
    if (!publishAnchoredJournalFile(
            *state_,
            Detail::CommitMarkerName,
            contents,
            marker,
            error)) {
        appendError(
            error,
            QStringLiteral(
                "linked activity recovery is required before continuing"));
        return false;
    }
#ifdef GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS
    linkedActivitySaveTransitionReached("linked-save-commit-marker");
#endif
    if (!verifyRetirementSourcesAbsent(*state_, resolved, error)) {
        appendError(
            error,
            QStringLiteral(
                "committed linked activity recovery is required before continuing"));
        return false;
    }
    return true;
}

bool Journal::cleanupAfterRollback(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-save journal is unavailable");
        return false;
    }
    if (state_->terminalCleanupCompleted) return true;
    return rollbackJournal(*state_, error);
}

bool Journal::cleanupAfterCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-save journal is unavailable");
        return false;
    }
    if (state_->terminalCleanupCompleted) return true;
    return commitJournal(*state_, error);
}

bool Journal::hasCommitMarker() const
{
    return state_ && pathEntryExists(state_->commitMarkerPath);
}

} // namespace LinkedActivitySave
