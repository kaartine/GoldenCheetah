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
    QList<QPair<QString, AtomicFileSnapshot>> inputSnapshots;
    std::unique_ptr<AtomicFileLockSet> transactionLease;
    std::unique_ptr<AtomicFileLockSet> pathLocks;
};

namespace {

using Detail::Entry;
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

bool makeDirectoryPrivate(const QString &path, QString &error)
{
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(path, privatePermissions)) {
        error = QStringLiteral(
            "Cannot make the plan transaction directory private");
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
            "The plan transaction directory is not private");
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
            "Cannot create a plan transaction under an unsafe directory");
        return false;
    }
    if (!QDir().mkdir(path)) {
        error = QStringLiteral("Cannot create the plan transaction directory");
        return false;
    }
    if (!makeDirectoryPrivate(path, error)) {
        QDir().rmdir(path);
        return false;
    }
    if (!syncParentDirectory(path, error)) return false;
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

bool readSmallRegularFile(
    const QString &path,
    qint64 maximumSize,
    QByteArray &contents,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    contents.clear();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        error = QStringLiteral(
            "A plan transaction control file is unavailable or unsafe");
        return false;
    }
    if (info.size() < 0 || info.size() > maximumSize) {
        error = QStringLiteral(
            "A plan transaction control file is unexpectedly large");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read a plan transaction control file: %1")
                    .arg(file.errorString());
        return false;
    }
    contents = file.read(maximumSize + 1);
    if (contents.size() > maximumSize
        || file.error() != QFileDevice::NoError) {
        error = QStringLiteral("Cannot read a bounded plan transaction file");
        return false;
    }
    snapshot.size = contents.size();
    snapshot.digest = QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256);
    return true;
}

bool writeBytesAtomically(
    const QString &path,
    const QByteArray &contents,
    AtomicFileMode mode,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    if (!writeFileAtomically(
            path, contents, qSaveFileWriterFactory(), error,
            mode == AtomicFileMode::ReplaceExisting)) {
        return false;
    }
    if (!syncParentDirectory(path, error)) return false;
    return captureAtomicFileSnapshot(path, snapshot, error);
}

bool copyExpectedFileAtomically(
    const QString &sourcePath,
    const QString &targetPath,
    const AtomicFileSnapshot &expected,
    AtomicFileMode mode,
    QString &error)
{
    if (!atomicFileMatchesSnapshot(sourcePath, expected, error)) return false;

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read a plan transaction source: %1")
                    .arg(source.errorString());
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

    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            error = QStringLiteral("Cannot read a plan transaction source: %1")
                        .arg(source.errorString());
            writer->cancelWriting();
            return false;
        }
        if (writer->write(chunk) != chunk.size()) {
            error = atomicFileError(
                QStringLiteral("Cannot copy a complete plan transaction file"),
                *writer);
            writer->cancelWriting();
            return false;
        }
        copied += chunk.size();
    }
    if (copied != expected.size || !writer->flush()) {
        error = writer->errorString().isEmpty()
            ? QStringLiteral("Cannot flush a complete plan transaction file")
            : writer->errorString();
        writer->cancelWriting();
        return false;
    }
    if (!atomicFileMatchesSnapshot(sourcePath, expected, error)) {
        writer->cancelWriting();
        return false;
    }
    if (!writer->commit()) {
        error = atomicFileError(
            QStringLiteral("Cannot publish a plan transaction file"),
            *writer);
        return false;
    }
    if (!syncParentDirectory(targetPath, error)) return false;
    return atomicFileMatchesSnapshot(targetPath, expected, error);
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
    std::shared_ptr<JournalState> &state,
    QString &error)
{
    if (!ensurePrivateDirectory(journalPath, error)) return false;
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)) {
        error = QStringLiteral("A plan transaction has an invalid identifier");
        return false;
    }

    const QString manifestPath = QDir(journalPath).filePath(
        Detail::ManifestName);
    QByteArray contents;
    AtomicFileSnapshot manifestSnapshot;
    if (!readSmallRegularFile(
            manifestPath,
            Detail::MaximumManifestSize,
            contents,
            manifestSnapshot,
            error)) {
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

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*loaded, resolved, error)) return false;
    loaded->pathLocks = std::make_unique<AtomicFileLockSet>();
    if (!loaded->pathLocks->lock(productionLockPaths(resolved), error)) {
        error = QStringLiteral("A plan activity file is already being changed: %1")
                    .arg(error);
        return false;
    }
    if (!validateExpectedSnapshot(
            loaded->manifestPath,
            true,
            loaded->manifestSnapshot,
            error)) {
        return false;
    }
    state = loaded;
    return true;
}

bool readCommitMarker(
    const JournalState &state,
    bool &committed,
    AtomicFileSnapshot *snapshot,
    QString &error)
{
    committed = false;
    const QFileInfo markerInfo(state.commitMarkerPath);
    if (!markerInfo.exists() && !markerInfo.isSymLink()) return true;

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
        error = QStringLiteral("The plan transaction commit marker is invalid");
        return false;
    }
    committed = true;
    if (snapshot) *snapshot = observed;
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
    QList<QPair<QString, ObservedFile>> &removable,
    QString &error)
{
    removable.clear();
    if (!validateExpectedSnapshot(
            state.manifestPath,
            true,
            state.manifestSnapshot,
            error)) {
        return false;
    }
    const QFileInfoList entries = QDir(state.journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &info : entries) {
        const QString name = info.fileName();
        if (info.isSymLink() || !info.isFile()) {
            error = QStringLiteral(
                "The plan transaction journal contains an unsafe entry");
            return false;
        }
        if (name == Detail::ManifestName
            || name == Detail::CommitMarkerName
            || expectedJournalDataName(state, name)) {
            continue;
        }
        if (!isKnownTemporaryName(name) && !isKnownLockName(name)) {
            error = QStringLiteral(
                "The plan transaction journal contains an unknown file");
            return false;
        }
        const qint64 maximumSize = isKnownLockName(name)
            ? Detail::MaximumLockFileSize
            : knownTemporaryMaximumSize(name);
        ObservedFile observed;
        if (!inspectRegularFile(
                info.absoluteFilePath(), observed, error, maximumSize)) {
            return false;
        }
        removable.append(qMakePair(info.absoluteFilePath(), observed));
    }

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)) return false;
    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        ObservedFile oldCopy;
        if (!inspectRegularFile(paths.oldCopy, oldCopy, error)) return false;
        if (entry.oldExists) {
            if (!snapshotMatches(oldCopy, entry.oldContents)) {
                error = QStringLiteral(
                    "A preserved plan activity does not match its journal");
                return false;
            }
        } else if (oldCopy.exists) {
            error = QStringLiteral(
                "An unexpected preserved plan activity exists");
            return false;
        }

        if (!entry.hasNew) continue;
        ObservedFile staged;
        if (!inspectRegularFile(paths.staging, staged, error)) return false;
        if (entry.staged) {
            if (!snapshotMatches(staged, entry.newContents)) {
                error = QStringLiteral(
                    "A staged plan activity does not match its journal");
                return false;
            }
        } else if (staged.exists) {
            removable.append(qMakePair(paths.staging, staged));
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
            if (!copyExpectedFileAtomically(
                    paths.oldCopy,
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
    const Entry &entry,
    const ResolvedEntry &paths,
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
    return copyExpectedFileAtomically(
        paths.staging,
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
    QString &error)
{
    if (!allTargetsAreStaged(state, error)) return false;
    int targetCount = 0;
    for (const Entry &entry : state.manifest.entries) {
        if (entry.hasNew) ++targetCount;
    }
    for (int stageIndex = 0; stageIndex < targetCount; ++stageIndex) {
        const int index = entryIndexForStage(state.manifest, stageIndex);
        if (index < 0
            || !installNewEntry(
                state.manifest.entries.at(index),
                resolved.at(index),
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
        if (index < 0
            || !installNewEntry(
                state.manifest.entries.at(index),
                resolved.at(index),
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
    QList<QPair<QString, ObservedFile>> removable;
    if (!inspectJournalDirectory(state, removable, error)) return false;

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
    for (const auto &file : std::as_const(removable)) {
        if (!removeObservedFile(file.first, file.second, error)) return false;
    }

    ObservedFile manifestFile;
    manifestFile.exists = true;
    manifestFile.contents = state.manifestSnapshot;
    if (!removeObservedFile(state.manifestPath, manifestFile, error)) {
        return false;
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-manifest-removed");
#endif

    for (int index = 0; index < state.manifest.entries.size(); ++index) {
        const Entry &entry = state.manifest.entries.at(index);
        const ResolvedEntry &paths = resolved.at(index);
        if (entry.oldExists) {
            ObservedFile oldCopy;
            oldCopy.exists = true;
            oldCopy.contents = entry.oldContents;
            if (!removeObservedFile(paths.oldCopy, oldCopy, error)) return false;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
            planReplacementTransitionReached("plan-replacement-cleanup-file");
#endif
        }
        if (entry.hasNew && entry.staged) {
            ObservedFile staged;
            staged.exists = true;
            staged.contents = entry.newContents;
            if (!removeObservedFile(paths.staging, staged, error)) return false;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
            planReplacementTransitionReached("plan-replacement-cleanup-file");
#endif
        }
    }
    if (markerExists) {
        ObservedFile marker;
        marker.exists = true;
        marker.contents = markerSnapshot;
        if (!removeObservedFile(state.commitMarkerPath, marker, error)) {
            return false;
        }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached(
            "plan-replacement-commit-marker-removed");
#endif
    }
    const QString id = state.manifest.id;
    if (!QDir(state.namespacePath).rmdir(id)) {
        error = QStringLiteral("Cannot remove the plan transaction directory");
        return false;
    }
    if (!syncParentDirectory(state.journalPath, error)) return false;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-directory-removed");
#endif
    return true;
}

bool rollbackJournal(JournalState &state, QString &error)
{
    QList<QPair<QString, ObservedFile>> removable;
    if (!inspectJournalDirectory(state, removable, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, nullptr, error)) return false;
    if (committed) {
        error = QStringLiteral("Cannot roll back a committed plan replacement");
        return false;
    }
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !restoreOldGeneration(state, resolved, error)) {
        return false;
    }
    return removeJournalDirectory(state, false, error);
}

bool commitJournal(JournalState &state, QString &error)
{
    QList<QPair<QString, ObservedFile>> removable;
    if (!inspectJournalDirectory(state, removable, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, nullptr, error)) return false;
    if (!committed) {
        error = QStringLiteral("Cannot complete an uncommitted plan replacement");
        return false;
    }
    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(state, resolved, error)
        || !ensureNewGeneration(state, resolved, error)) {
        return false;
    }
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
    const QString &namespacePath,
    const QString &journalPath,
    QString &error)
{
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)
        || !ensurePrivateDirectory(journalPath, error)) {
        return false;
    }
    AtomicFileLockSet lock;
    if (!lock.lock({journalPath}, error)) return false;

    const QFileInfoList entries = QDir(journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    QList<QPair<QString, ObservedFile>> removable;
    for (const QFileInfo &entry : entries) {
        qint64 maximumSize = -1;
        if (entry.isSymLink() || !entry.isFile()
            || !maximumSizeForPreManifestEntry(
                entry.fileName(), maximumSize)) {
            error = QStringLiteral(
                "An incomplete plan transaction contains an unknown entry");
            return false;
        }
        ObservedFile observed;
        if (!inspectRegularFile(
                entry.absoluteFilePath(), observed, error, maximumSize)) {
            return false;
        }
        removable.append(qMakePair(entry.absoluteFilePath(), observed));
    }
    for (const auto &file : std::as_const(removable)) {
        if (!removeObservedFile(file.first, file.second, error)) return false;
    }
    if (!QDir(namespacePath).rmdir(id)) {
        error = QStringLiteral(
            "Cannot remove an incomplete plan transaction");
        return false;
    }
    return syncParentDirectory(journalPath, error);
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
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    const AnchoredFileSystem::MutationResult creation =
        AnchoredFileSystem::createPrivateChildDirectory(
            namespaceDirectory,
            state->manifest.id,
            journalDirectory);
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
        } else if (journalDirectory.isValid()) {
            QString retainedError;
            if (journalDirectory.pathMatches(retainedError)) {
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

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state, resolved, error)) return {};
    for (int index = 0; index < state->manifest.entries.size(); ++index) {
        const Entry &entry = state->manifest.entries.at(index);
        if (!entry.oldExists) continue;
        if (!copyExpectedFileAtomically(
                resolved.at(index).path,
                resolved.at(index).oldCopy,
                entry.oldContents,
                AtomicFileMode::CreateNew,
                error)) {
            appendError(
                error,
                QStringLiteral("recovery journal retained at %1")
                    .arg(state->journalPath));
            return {};
        }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
        planReplacementTransitionReached("plan-replacement-old-copy-published");
#endif
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
    const QString namespacePath = transactionNamespacePath(root);
    const QString transactions = QDir(root).filePath(
        QStringLiteral(".gc-transactions"));
    if (!validateExistingDirectory(transactions, error)
        || !makeDirectoryPrivate(transactions, error)
        || !validateExistingDirectory(namespacePath, error)
        || !makeDirectoryPrivate(namespacePath, error)) {
        return {};
    }

    const QString journalPath =
        QDir(namespacePath).filePath(transactionId);
    const QFileInfo journalInfo(journalPath);
    if (journalInfo.isSymLink() || !journalInfo.isDir()
        || journalInfo.fileName() != transactionId
        || atomicFilePathKey(journalInfo.absolutePath())
            != atomicFilePathKey(namespacePath)) {
        error = QStringLiteral(
            "The prepared plan transaction is unavailable");
        return {};
    }

    std::shared_ptr<JournalState> state;
    if (!loadManifestState(root, journalPath, state, error))
        return {};
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

        // Control-file I/O and cleanup remain pathname based for the next
        // SEC-025 package. Release the Windows observation handle only after
        // binding this operation to the enumerated child generation.
        journalDirectory = {};
        if (!manifestExists) {
            if (!removePreManifestJournal(
                    namespacePath,
                    journalPath,
                    transactionError)) {
                failures.append(transactionError);
            }
            continue;
        }

        std::shared_ptr<JournalState> state;
        if (!loadManifestState(
                root,
                journalPath,
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
    if (!validateExpectedSnapshot(
            state_->manifestPath,
            true,
            state_->manifestSnapshot,
            error)) {
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

    ObservedFile staged;
    if (!inspectRegularFile(path, staged, error)
        || !staged.exists) {
        if (error.isEmpty()) {
            error = QStringLiteral("A staged plan activity is unavailable");
        }
        return false;
    }
    Entry &entry = state_->manifest.entries[entryIndex];
    if (entry.staged) {
        if (!snapshotMatches(staged, entry.newContents)) {
            error = QStringLiteral("A staged plan activity changed unexpectedly");
            return false;
        }
        return true;
    }
    entry.staged = true;
    entry.newContents = staged.contents;
    if (!writeManifestFile(*state_, true, error)) return false;
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-stage-recorded");
#endif
    return true;
}

bool Journal::publishAndCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    QList<QPair<QString, ObservedFile>> removable;
    if (!inspectJournalDirectory(*state_, removable, error)) return false;
    for (const auto &file : std::as_const(removable)) {
        if (!removeObservedFile(file.first, file.second, error)) return false;
    }
    bool committed = false;
    if (!readCommitMarker(*state_, committed, nullptr, error)) return false;
    if (committed) return true;

    QList<ResolvedEntry> resolved;
    if (!resolveManifestEntries(*state_, resolved, error)
        || !publishNewGeneration(*state_, resolved, error)) {
        return false;
    }
    AtomicFileSnapshot markerSnapshot;
    if (!writeBytesAtomically(
            state_->commitMarkerPath,
            state_->manifest.id.toLatin1() + '\n',
            AtomicFileMode::CreateNew,
            markerSnapshot,
            error)) {
        appendError(
            error,
            QStringLiteral(
                "plan replacement recovery is required before continuing"));
        return false;
    }
#ifdef GC_PLAN_REPLACEMENT_TEST_HOOKS
    planReplacementTransitionReached("plan-replacement-commit-marker");
#endif
    return true;
}

bool Journal::cleanupAfterRollback(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    const QFileInfo info(state_->journalPath);
    if (!info.exists() && !info.isSymLink()) return true;
    return rollbackJournal(*state_, error);
}

bool Journal::cleanupAfterCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The plan replacement journal is unavailable");
        return false;
    }
    const QFileInfo info(state_->journalPath);
    if (!info.exists() && !info.isSymLink()) return true;
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
