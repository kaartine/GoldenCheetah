/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LinkedActivityRemovalJournal.h"

#include "AnchoredFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
void rideCacheRemovalTransitionReached(const char *transition);
#endif

namespace LinkedActivityRemoval {

namespace Detail {

constexpr int ManifestVersion = 2;
constexpr qint64 MaximumManifestSize = 4 * 1024 * 1024;
constexpr qint64 MaximumCommitMarkerSize = 128;
constexpr qint64 MaximumLockFileSize = 64 * 1024;
constexpr int MaximumDerivedPaths = 4096;

const QString ManifestName = QStringLiteral("manifest.json");
const QString PeerOldName = QStringLiteral("peer.old");
const QString CommitMarkerName = QStringLiteral("COMMITTED");

struct ExpectedFile
{
    QString relativePath;
    bool exists = false;
    AtomicFileSnapshot contents;
};

struct Manifest
{
    int version = ManifestVersion;
    QString id;
    bool hasPeer = false;
    ExpectedFile source;
    ExpectedFile previousBackup;
    ExpectedFile peerOld;
    ExpectedFile peerNew;
    QList<ExpectedFile> derived;
};

struct ResolvedPaths
{
    QString source;
    QString backup;
    QString peer;
    QStringList derived;
    QString backupStaging;
    QString sourceTombstone;
    QString previousBackup;
    QString peerStaging;
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
    QString namespacePath;
    QString journalPath;
    QString manifestPath;
    QString peerOldPath;
    QString commitMarkerPath;
    Detail::Manifest manifest;
    AtomicFileSnapshot manifestSnapshot;
    AnchoredFileSystem::DirectoryAnchor journalDirectory;
    AnchoredFileSystem::EntryRef manifestEntry;
    std::shared_ptr<AnchoredFileSystem::PinnedFile> manifestFile;
    std::unique_ptr<AtomicFileLockSet> transactionLease;
    bool cleanupComplete = false;
};

namespace {

using Detail::ExpectedFile;
using Detail::Manifest;
using Detail::ObservedFile;
using Detail::ResolvedPaths;

void appendError(QString &error, const QString &detail)
{
    if (detail.isEmpty()) return;
    if (!error.isEmpty()) error += QStringLiteral("; ");
    error += detail;
}

bool journalDirectoryMatches(
    const JournalState &state, QString &error)
{
    if (!state.journalDirectory.isValid()) {
        error = QStringLiteral(
            "The active transaction journal directory is not anchored");
        return false;
    }

    QString detail;
    if (state.journalDirectory.pathMatches(detail)) return true;
    error = detail.isEmpty()
        ? QStringLiteral(
              "The active transaction journal directory was replaced")
        : QStringLiteral(
              "Cannot revalidate the active transaction journal directory: %1")
              .arg(detail);
    return false;
}

bool pathEntryExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

void removeCreatedFileBestEffort(const QString &path)
{
    QString ignored;
    if (pathEntryExists(path))
        removeFileDurably(path, ignored);
}

bool validTransactionId(const QString &id)
{
    if (id.size() != 36 || id != id.toLower()) return false;
    const QUuid uuid(id);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == id;
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
    const QString &root,
    const QString &relativePath,
    QString &error,
    bool allowMissingParents = false)
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
            if (allowMissingParents && !info.exists())
                return true;
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
    QString &error,
    bool allowMissingParents = false)
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
    if (!validatePathComponents(
            root, relativePath, error, allowMissingParents)) {
        return false;
    }
    return true;
}

bool resolveRootRelativePath(
    const QString &root,
    const QString &relativePath,
    QString &absolutePath,
    QString &error,
    bool allowMissingParents = false)
{
    if (!validateRelativePath(relativePath, error)) return false;
    absolutePath = QDir::cleanPath(QDir(root).filePath(relativePath));
    const QString roundTrip = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(absolutePath));
    if (roundTrip != QDir::fromNativeSeparators(relativePath)
        || !validatePathComponents(
            root, relativePath, error, allowMissingParents)) {
        if (error.isEmpty()) {
            error = QStringLiteral("A journal path escapes the athlete root");
        }
        return false;
    }
    return true;
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
    if ((permissions & nonOwnerPermissions)
            != QFileDevice::Permissions()
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
            && makeDirectoryPrivate(path, error)
            && syncParentDirectory(path, error);
    }

    const QFileInfo parent(existing.absolutePath());
    if (!parent.exists() || !parent.isDir() || parent.isSymLink()) {
        error = QStringLiteral(
            "Cannot create an activity transaction under an unsafe directory");
        return false;
    }
    if (!createDirectoryDurably(path, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot create the activity transaction directory");
        }
        return false;
    }

    if (!makeDirectoryPrivate(path, error)) {
        QString cleanupError;
        if (!removeDirectoryDurably(path, cleanupError)) {
            cleanupError = QStringLiteral(
                "cannot remove the insufficiently private directory");
        }
        appendError(error, cleanupError);
        return false;
    }

    QString syncError;
    if (!syncParentDirectory(path, syncError)) {
        error = syncError;
        return false;
    }
    return true;
}

QString transactionNamespacePath(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral(".gc-transactions/linked-removal"));
}

QString transactionLeaseTarget(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral("linked-removal-transaction"));
}

bool ensureTransactionNamespace(
    const QString &root, QString &namespacePath, QString &error)
{
    const QString transactions = QDir(root).filePath(
        QStringLiteral(".gc-transactions"));
    if (!ensurePrivateDirectory(transactions, error)) return false;

    namespacePath = QDir(transactions).filePath(
        QStringLiteral("linked-removal"));
    return ensurePrivateDirectory(namespacePath, error);
}

bool transactionNamespaceIsReady(
    const QString &root,
    const QString &namespacePath,
    QString &error)
{
    const QStringList namespaces = {
        namespacePath,
        QDir(root).filePath(
            QStringLiteral(".gc-transactions/linked-save")),
        QDir(root).filePath(
            QStringLiteral(".gc-transactions/plan-replacement"))};
    for (const QString &candidate : namespaces) {
        const QFileInfo candidateInfo(candidate);
        if (!candidateInfo.exists() && !candidateInfo.isSymLink()) continue;
        if (!ensurePrivateDirectory(candidate, error)) return false;
        const QFileInfoList entries = QDir(candidate).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot
                | QDir::Hidden | QDir::System,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString name = entry.fileName();
            if (entry.isFile() && !entry.isSymLink()
                && name.startsWith(QLatin1Char('.'))
                && name.endsWith(QStringLiteral(".lock"))) {
                const QString lockedId = name.mid(1, name.size() - 6);
                if (validTransactionId(lockedId)) continue;
            }

            error = QStringLiteral(
                "Pending linked activity recovery must be completed before starting another transaction");
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

bool expectedMatches(
    const ObservedFile &observed, const ExpectedFile &expected)
{
    if (!expected.exists) return !observed.exists;
    return snapshotMatches(observed, expected.contents);
}

bool inspectExpectedFile(
    const QString &path,
    const QString &relativePath,
    bool required,
    ExpectedFile &expected,
    QString &error)
{
    expected = {};
    expected.relativePath = relativePath;
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    if (required && !observed.exists) {
        error = QStringLiteral("A required activity transaction file is missing");
        return false;
    }
    expected.exists = observed.exists;
    if (observed.exists) expected.contents = observed.contents;
    return true;
}

bool validateExpectedFile(
    const QString &path, const ExpectedFile &expected, QString &error)
{
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    if (!expectedMatches(observed, expected)) {
        error = QStringLiteral(
            "An activity transaction file changed unexpectedly: %1")
                    .arg(path);
        return false;
    }
    return true;
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
            "An activity transaction file does not match its journal: %1")
                    .arg(path);
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

QByteArray serializeManifest(const Manifest &manifest)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), manifest.version);
    root.insert(QStringLiteral("id"), manifest.id);
    if (manifest.version >= 2)
        root.insert(QStringLiteral("hasPeer"), manifest.hasPeer);
    root.insert(
        QStringLiteral("source"), expectedFileToJson(manifest.source));
    root.insert(
        QStringLiteral("previousBackup"),
        expectedFileToJson(manifest.previousBackup));
    root.insert(
        QStringLiteral("peerOld"), expectedFileToJson(manifest.peerOld));
    root.insert(
        QStringLiteral("peerNew"), expectedFileToJson(manifest.peerNew));

    QJsonArray derived;
    for (const ExpectedFile &entry : manifest.derived) {
        derived.append(expectedFileToJson(entry));
    }
    root.insert(QStringLiteral("derived"), derived);

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

bool parseExpectedFile(
    const QJsonValue &value,
    ExpectedFile &expected,
    QString &error,
    bool pathRequired = true)
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
    expected.exists = object.value(QStringLiteral("exists")).toBool();
    if (expected.relativePath.isEmpty()) {
        if (pathRequired || expected.exists) {
            error = QStringLiteral("A journal file path is missing");
            return false;
        }
    } else if (!validateRelativePath(expected.relativePath, error)) {
        return false;
    }

    const QString sizeText = object.value(QStringLiteral("size")).toString();
    bool sizeValid = false;
    const qint64 size = sizeText.toLongLong(&sizeValid, 10);
    if (!sizeValid || size < 0 || QString::number(size) != sizeText) {
        error = QStringLiteral("A journal file size is invalid");
        return false;
    }

    const QString digestText =
        object.value(QStringLiteral("sha256")).toString();
    if (!expected.exists) {
        if (size != 0 || !digestText.isEmpty()) {
            error = QStringLiteral(
                "An absent journal file has unexpected contents");
            return false;
        }
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

    expected.contents.size = size;
    expected.contents.digest = digest;
    return true;
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
        error = QStringLiteral("The linked-removal journal is not valid JSON");
        return false;
    }

    const QJsonObject root = document.object();
    const QSet<QString> versionOneKeys = {
        QStringLiteral("version"),
        QStringLiteral("id"),
        QStringLiteral("source"),
        QStringLiteral("previousBackup"),
        QStringLiteral("peerOld"),
        QStringLiteral("peerNew"),
        QStringLiteral("derived")};
    QSet<QString> versionTwoKeys = versionOneKeys;
    versionTwoKeys.insert(QStringLiteral("hasPeer"));
    if (!root.value(QStringLiteral("version")).isDouble()) {
        error = QStringLiteral("The linked-removal journal schema is invalid");
        return false;
    }
    const double serializedVersion =
        root.value(QStringLiteral("version")).toDouble();
    const bool versionOne = serializedVersion == 1.0;
    const bool versionTwo =
        serializedVersion == Detail::ManifestVersion;
    if ((!versionOne && !versionTwo)
        || !hasExactKeys(
            root, versionOne ? versionOneKeys : versionTwoKeys)
        || !root.value(QStringLiteral("id")).isString()
        || !root.value(QStringLiteral("derived")).isArray()
        || (versionTwo
            && !root.value(QStringLiteral("hasPeer")).isBool())) {
        error = QStringLiteral("The linked-removal journal schema is invalid");
        return false;
    }

    manifest = {};
    manifest.version = versionOne ? 1 : Detail::ManifestVersion;
    manifest.id = root.value(QStringLiteral("id")).toString();
    manifest.hasPeer = versionOne
        || root.value(QStringLiteral("hasPeer")).toBool();
    if (!validTransactionId(manifest.id)
        || manifest.id != expectedId) {
        error = QStringLiteral("The linked-removal journal id is invalid");
        return false;
    }

    if (!parseExpectedFile(
            root.value(QStringLiteral("source")), manifest.source, error)
        || !parseExpectedFile(
            root.value(QStringLiteral("previousBackup")),
            manifest.previousBackup,
            error)
        || !parseExpectedFile(
            root.value(QStringLiteral("peerOld")),
            manifest.peerOld,
            error,
            manifest.hasPeer)
        || !parseExpectedFile(
            root.value(QStringLiteral("peerNew")),
            manifest.peerNew,
            error,
            manifest.hasPeer)) {
        return false;
    }

    if (!manifest.source.exists
        || (manifest.hasPeer
            && (!manifest.peerOld.exists
                || manifest.peerOld.relativePath
                    != manifest.peerNew.relativePath))
        || (!manifest.hasPeer
            && (manifest.peerOld.exists
                || manifest.peerNew.exists
                || !manifest.peerOld.relativePath.isEmpty()
                || !manifest.peerNew.relativePath.isEmpty()))) {
        error = QStringLiteral(
            "The linked-removal journal has invalid required files");
        return false;
    }

    const QJsonArray derived = root.value(QStringLiteral("derived")).toArray();
    if (derived.size() > Detail::MaximumDerivedPaths) {
        error = QStringLiteral("The linked-removal journal is too large");
        return false;
    }
    for (const QJsonValue &value : derived) {
        ExpectedFile entry;
        if (!parseExpectedFile(value, entry, error)) return false;
        manifest.derived.append(entry);
    }
    return true;
}

bool manifestFileMatches(
    const JournalState &state, QString &error)
{
    if (!state.manifestEntry.isValid()
        || !state.manifestFile
        || !state.manifestFile->isValid()) {
        error = QStringLiteral(
            "The transaction journal manifest is not anchored");
        return false;
    }
    if (state.manifestFile->size() != state.manifestSnapshot.size
        || state.manifestFile->sha256()
            != state.manifestSnapshot.digest) {
        error = QStringLiteral(
            "The anchored transaction journal manifest changed");
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    rideCacheRemovalTransitionReached(
        "journal-manifest-inspected");
#endif
    bool matches = false;
    if (!AnchoredFileSystem::entryMatches(
            state.manifestEntry,
            *state.manifestFile,
            matches,
            error)) {
        return false;
    }
    if (!matches) {
        error = QStringLiteral(
            "The transaction journal manifest was replaced");
        return false;
    }
    return true;
}

bool pinManifestFile(
    JournalState &state,
    const AtomicFileSnapshot *expected,
    QString &error)
{
    const AtomicFileSnapshot expectedSnapshot =
        expected ? *expected : AtomicFileSnapshot();
    AnchoredFileSystem::EntryRef entry = state.journalDirectory.entry(
        Detail::ManifestName, error);
    if (!entry.isValid()) return false;

    auto file = std::make_shared<AnchoredFileSystem::PinnedFile>();
    if (!AnchoredFileSystem::pinRegularFile(
            entry,
            *file,
            error,
            Detail::MaximumManifestSize)) {
        error = QStringLiteral(
            "Cannot pin the transaction journal manifest: %1")
                    .arg(error);
        return false;
    }
    if (file->size() > Detail::MaximumManifestSize) {
        error = QStringLiteral(
            "The transaction journal manifest is unexpectedly large");
        return false;
    }

    AtomicFileSnapshot snapshot;
    snapshot.size = file->size();
    snapshot.digest = file->sha256();
    if (expected
        && (snapshot.size != expectedSnapshot.size
            || snapshot.digest != expectedSnapshot.digest)) {
        error = QStringLiteral("A transaction file changed while being read");
        return false;
    }
    state.manifestEntry = std::move(entry);
    state.manifestSnapshot = snapshot;
    state.manifestFile = std::move(file);
    return true;
}

bool readManifestFile(
    JournalState &state, QByteArray &contents, QString &error)
{
    if (!pinManifestFile(state, nullptr, error)) return false;
    if (!AnchoredFileSystem::readAll(
            *state.manifestFile,
            Detail::MaximumManifestSize,
            contents,
            error)) {
        return false;
    }
    return manifestFileMatches(state, error);
}

bool writeManifestFile(
    const QString &path,
    const Manifest &manifest,
    bool replace,
    bool targetLockHeld,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    const QByteArray contents = serializeManifest(manifest);
    if (contents.size() > Detail::MaximumManifestSize) {
        error = QStringLiteral("The linked-removal journal is too large");
        return false;
    }
    if (!writeFileAtomically(
            path,
            contents,
            qSaveFileWriterFactory(),
            error,
            replace,
            targetLockHeld)) {
        return false;
    }
    return captureAtomicFileSnapshot(path, snapshot, error);
}

bool copyExpectedFileCreateNew(
    const QString &sourcePath,
    const QString &targetPath,
    const AtomicFileSnapshot &expected,
    QString &error)
{
    if (!validateExpectedSnapshot(sourcePath, expected, error)) return false;
    if (pathEntryExists(targetPath)) {
        error = QStringLiteral("A transaction staging file already exists");
        return false;
    }

    QFile source(sourcePath);
    NewAtomicFileWriter target(targetPath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read an activity transaction file: %1")
                    .arg(source.errorString());
        return false;
    }
    if (!target.open()) {
        error = QStringLiteral("Cannot create an activity transaction file: %1")
                    .arg(target.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            error = QStringLiteral("Cannot read an activity transaction file: %1")
                        .arg(source.errorString());
            target.cancelWriting();
            return false;
        }
        if (target.write(chunk) != chunk.size()) {
            error = QStringLiteral("Cannot write an activity transaction file: %1")
                        .arg(target.errorString());
            target.cancelWriting();
            return false;
        }
        copied += chunk.size();
        hash.addData(chunk);
    }

    if (!target.flush()) {
        if (error.isEmpty()) {
            error = QStringLiteral("Cannot flush an activity transaction file: %1")
                        .arg(target.errorString());
        }
        target.cancelWriting();
        return false;
    }

    if (copied != expected.size || hash.result() != expected.digest
        || !validateExpectedSnapshot(sourcePath, expected, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A staged transaction file does not match its source");
        }
        target.cancelWriting();
        return false;
    }
    if (!target.commit()) {
        error = QStringLiteral("Cannot publish an activity transaction file: %1")
                    .arg(target.errorString());
        return false;
    }

    QString syncError;
    if (!syncParentDirectory(targetPath, syncError)) {
        error = syncError;
        return false;
    }
    if (!validateExpectedSnapshot(targetPath, expected, error)) {
        removeCreatedFileBestEffort(targetPath);
        return false;
    }
    return true;
}

bool writeBytesCreateNew(
    const QString &path,
    const QByteArray &contents,
    bool targetLockHeld,
    AtomicFileSnapshot &snapshot,
    QString &error)
{
    if (!writeFileAtomically(
            path,
            contents,
            qSaveFileWriterFactory(),
            error,
            false,
            targetLockHeld)) {
        return false;
    }
    if (!captureAtomicFileSnapshot(path, snapshot, error)) return false;

    const QByteArray digest = QCryptographicHash::hash(
        contents, QCryptographicHash::Sha256);
    if (snapshot.size != contents.size() || snapshot.digest != digest) {
        error = QStringLiteral(
            "A staged peer file does not match the requested contents");
        return false;
    }
    return true;
}

bool copyExpectedFileAtomically(
    const QString &sourcePath,
    const QString &targetPath,
    const AtomicFileSnapshot &expected,
    QString &error)
{
    if (!validateExpectedSnapshot(sourcePath, expected, error)) return false;

    const QFileInfo targetInfo(targetPath);
    if (targetInfo.isSymLink()
        || (targetInfo.exists() && !targetInfo.isFile())) {
        error = QStringLiteral(
            "Cannot replace an unsafe transaction target: %1").arg(targetPath);
        return false;
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read a recovery source: %1")
                    .arg(source.errorString());
        return false;
    }

    const AtomicFileMode mode = targetInfo.exists()
        ? AtomicFileMode::ReplaceExisting
        : AtomicFileMode::CreateNew;
    std::unique_ptr<AtomicFileWriter> writer =
        qSaveFileWriterFactory()(targetPath, mode);
    if (!writer || !writer->open()) {
        error = writer
            ? atomicFileError(
                  QStringLiteral("Cannot open a recovery target"), *writer)
            : QStringLiteral("Cannot create a recovery writer");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            error = QStringLiteral("Cannot read a recovery source: %1")
                        .arg(source.errorString());
            writer->cancelWriting();
            return false;
        }
        if (writer->write(chunk) != chunk.size()) {
            error = atomicFileError(
                QStringLiteral("Cannot write a complete recovery target"),
                *writer);
            writer->cancelWriting();
            return false;
        }
        copied += chunk.size();
        hash.addData(chunk);
    }

    if (copied != expected.size || hash.result() != expected.digest
        || !validateExpectedSnapshot(sourcePath, expected, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("A recovery source changed while being copied");
        }
        writer->cancelWriting();
        return false;
    }
    if (!writer->flush()) {
        error = atomicFileError(
            QStringLiteral("Cannot flush a recovery target"), *writer);
        writer->cancelWriting();
        return false;
    }
    if (!writer->commit()) {
        error = atomicFileError(
            QStringLiteral("Cannot commit a recovery target"), *writer);
        return false;
    }

    QString syncError;
    if (!syncParentDirectory(targetPath, syncError)) {
        error = syncError;
        return false;
    }
    return validateExpectedSnapshot(targetPath, expected, error);
}

bool removeObservedFile(
    const QString &path, const ObservedFile &observed, QString &error)
{
    if (!observed.exists) return true;
    if (!validateExpectedSnapshot(path, observed.contents, error)) return false;
    if (!removeFileDurably(path, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot remove a transaction file: %1").arg(path);
        }
        return false;
    }
    return true;
}

bool removeExpectedFile(
    const QString &path,
    const AtomicFileSnapshot &expected,
    QString &error)
{
    ObservedFile observed;
    if (!inspectRegularFile(path, observed, error)) return false;
    if (!observed.exists) return true;
    if (!snapshotMatches(observed, expected)) {
        error = QStringLiteral(
            "A cleanup file changed and was retained: %1").arg(path);
        return false;
    }
    return removeObservedFile(path, observed, error);
}

bool removeManifestFile(
    const JournalState &state, QString &error)
{
    if (!manifestFileMatches(state, error)) return false;
    const AnchoredFileSystem::MutationResult removal =
        AnchoredFileSystem::remove(*state.manifestFile);
    if (removal.effect
        == AnchoredFileSystem::MutationEffect::AppliedDurable) {
        return true;
    }
    error = removal.error.isEmpty()
        ? QStringLiteral(
              "Cannot remove the anchored transaction journal manifest")
        : removal.error;
    if (!removal.verifiedRecoveryPath.isEmpty()) {
        error += QStringLiteral("; recovery file retained at %1")
                     .arg(removal.verifiedRecoveryPath);
    }
    return false;
}

bool resolveManifestPaths(
    const JournalState &state, ResolvedPaths &paths, QString &error)
{
    paths = {};
    if (!resolveRootRelativePath(
            state.athleteRoot,
            state.manifest.source.relativePath,
            paths.source,
            error)
        || !resolveRootRelativePath(
            state.athleteRoot,
            state.manifest.previousBackup.relativePath,
            paths.backup,
            error)) {
        return false;
    }
    if (state.manifest.hasPeer
        && !resolveRootRelativePath(
            state.athleteRoot,
            state.manifest.peerOld.relativePath,
            paths.peer,
            error)) {
        return false;
    }

    for (const ExpectedFile &entry : state.manifest.derived) {
        QString path;
        if (!resolveRootRelativePath(
                state.athleteRoot,
                entry.relativePath,
                path,
                error,
                !entry.exists)) {
            return false;
        }
        paths.derived.append(path);
    }

    paths.backupStaging = paths.backup
        + QStringLiteral(".gc-copy-") + state.manifest.id;
    paths.sourceTombstone = paths.source
        + QStringLiteral(".gc-remove-") + state.manifest.id;
    paths.previousBackup = paths.backup
        + QStringLiteral(".gc-previous-") + state.manifest.id;
    if (state.manifest.hasPeer) {
        paths.peerStaging = paths.peer
            + QStringLiteral(".gc-linked-new-") + state.manifest.id;
    }

    QStringList sidecars = {
        paths.backupStaging,
        paths.sourceTombstone,
        paths.previousBackup};
    if (state.manifest.hasPeer)
        sidecars.append(paths.peerStaging);
    for (const QString &sidecar : sidecars) {
        QString relative;
        QString absolute;
        if (!makeRootRelativePath(
                state.athleteRoot, sidecar,
                relative, absolute, error)
            || atomicFilePathKey(sidecar) != atomicFilePathKey(absolute)) {
            if (error.isEmpty()) {
                error = QStringLiteral("A recovery sidecar path is unsafe");
            }
            return false;
        }
    }

    QSet<QString> uniquePaths;
    const auto addUnique = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (uniquePaths.contains(key)) return false;
        uniquePaths.insert(key);
        return true;
    };
    if (!addUnique(paths.source) || !addUnique(paths.backup)
        || (state.manifest.hasPeer && !addUnique(paths.peer))) {
        error = QStringLiteral("Activity transaction paths overlap");
        return false;
    }
    for (const QString &derived : std::as_const(paths.derived)) {
        if (!addUnique(derived)) {
            error = QStringLiteral("Activity transaction paths overlap");
            return false;
        }
    }
    for (const QString &sidecar : sidecars) {
        if (!addUnique(sidecar)) {
            error = QStringLiteral("Activity transaction sidecars overlap");
            return false;
        }
    }
    return true;
}

qint64 knownTemporaryMaximumSize(const QString &name)
{
    const auto atomicTemporary = [&](const QString &base) {
        return name.startsWith(QLatin1Char('.') + base + QLatin1Char('.'))
            && name.endsWith(QStringLiteral(".tmp"));
    };
    if (atomicTemporary(Detail::ManifestName))
        return Detail::MaximumManifestSize;
    if (atomicTemporary(Detail::CommitMarkerName))
        return Detail::MaximumCommitMarkerSize;
    if (name == QStringLiteral(".manifest.json.lock")
        || name == QStringLiteral(".COMMITTED.lock")) {
        return Detail::MaximumLockFileSize;
    }
    return -1;
}

bool isKnownTemporaryName(const QString &name)
{
    return knownTemporaryMaximumSize(name) >= 0;
}

bool inspectJournalDirectory(
    const JournalState &state,
    QList<QPair<QString, ObservedFile>> &temporaryFiles,
    QString &error)
{
    temporaryFiles.clear();
    if (!journalDirectoryMatches(state, error)) return false;
    if (!ensurePrivateDirectory(state.journalPath, error)) return false;

    const QFileInfoList entries = QDir(state.journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || !entry.isFile()) {
            error = QStringLiteral(
                "The transaction journal contains an unsafe entry");
            return false;
        }
        const QString name = entry.fileName();
        if (name == Detail::ManifestName || name == Detail::PeerOldName
            || name == Detail::CommitMarkerName) {
            continue;
        }
        if (!isKnownTemporaryName(name)) {
            error = QStringLiteral(
                "The transaction journal contains an unknown file");
            return false;
        }
        ObservedFile observed;
        if (!inspectRegularFile(
                entry.absoluteFilePath(),
                observed,
                error,
                knownTemporaryMaximumSize(name))) {
            return false;
        }
        temporaryFiles.append({entry.absoluteFilePath(), observed});
    }
    return true;
}

bool readCommitMarker(
    const JournalState &state,
    bool &exists,
    QString &error)
{
    exists = false;
    ObservedFile observed;
    if (!inspectRegularFile(
            state.commitMarkerPath,
            observed,
            error,
            Detail::MaximumCommitMarkerSize)) {
        return false;
    }
    if (!observed.exists) return true;

    QFile marker(state.commitMarkerPath);
    if (!marker.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read the transaction commit marker");
        return false;
    }
    if (marker.size() > Detail::MaximumCommitMarkerSize) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(state.commitMarkerPath);
        return false;
    }
    const QByteArray contents = marker.read(
        Detail::MaximumCommitMarkerSize + 1);
    if (contents.size() > Detail::MaximumCommitMarkerSize
        || !marker.atEnd()) {
        error = QStringLiteral("A transaction file is unexpectedly large: %1")
                    .arg(state.commitMarkerPath);
        return false;
    }
    if (marker.error() != QFileDevice::NoError
        || contents != state.manifest.id.toLatin1() + '\n') {
        error = QStringLiteral("The transaction commit marker is invalid");
        return false;
    }
    if (!validateExpectedSnapshot(
            state.commitMarkerPath, observed.contents, error)) {
        return false;
    }
    exists = true;
    return true;
}

bool loadJournalState(
    const QString &root,
    const QString &journalPath,
    std::shared_ptr<JournalState> &state,
    QString &error)
{
    if (!ensurePrivateDirectory(journalPath, error)) return false;
    const QString id = QFileInfo(journalPath).fileName();
    if (!validTransactionId(id)) {
        error = QStringLiteral("The transaction directory name is invalid");
        return false;
    }

    const QString expectedNamespace = transactionNamespacePath(root);
    if (atomicFilePathKey(QFileInfo(journalPath).absolutePath())
        != atomicFilePathKey(expectedNamespace)) {
        error = QStringLiteral("The transaction directory is outside its namespace");
        return false;
    }

    std::shared_ptr<JournalState> loaded(new JournalState);
    loaded->athleteRoot = root;
    loaded->namespacePath = expectedNamespace;
    loaded->journalPath = QFileInfo(journalPath).absoluteFilePath();
    loaded->manifestPath = QDir(loaded->journalPath).filePath(
        Detail::ManifestName);
    loaded->peerOldPath = QDir(loaded->journalPath).filePath(
        Detail::PeerOldName);
    loaded->commitMarkerPath = QDir(loaded->journalPath).filePath(
        Detail::CommitMarkerName);
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            loaded->journalPath,
            loaded->journalDirectory,
            error)) {
        error = QStringLiteral(
            "Cannot anchor the transaction journal directory: %1")
                    .arg(error);
        return false;
    }

    QByteArray contents;
    if (!readManifestFile(*loaded, contents, error)
        || !parseManifest(contents, id, loaded->manifest, error)) {
        return false;
    }

    ResolvedPaths paths;
    if (!resolveManifestPaths(*loaded, paths, error)) return false;
    state = loaded;
    return true;
}

QStringList lockPathsFor(
    const JournalState &state, const ResolvedPaths &paths)
{
    QStringList lockPaths = {
        state.journalPath,
        paths.source,
        paths.backup,
        paths.backupStaging,
        paths.sourceTombstone,
        paths.previousBackup};
    if (state.manifest.hasPeer) {
        lockPaths.append(paths.peer);
        lockPaths.append(paths.peerStaging);
    }
    for (const QString &derived : paths.derived) {
        if (QFileInfo(derived).absoluteDir().exists())
            lockPaths.append(derived);
    }
    return lockPaths;
}

bool loadAndLockJournal(
    const QString &root,
    const QString &journalPath,
    std::shared_ptr<JournalState> &state,
    std::unique_ptr<AtomicFileLockSet> &locks,
    QString &error,
    const std::shared_ptr<JournalState> &expected = {})
{
    std::shared_ptr<JournalState> current = expected;
    if (current) {
        if (atomicFilePathKey(current->athleteRoot)
                != atomicFilePathKey(root)
            || atomicFilePathKey(current->journalPath)
                != atomicFilePathKey(journalPath)) {
            error = QStringLiteral(
                "The active transaction journal path changed");
            return false;
        }
    } else if (!loadJournalState(
                   root, journalPath, current, error)) {
        return false;
    }
    ResolvedPaths paths;
    if (!journalDirectoryMatches(*current, error)
        || !manifestFileMatches(*current, error)
        || !resolveManifestPaths(*current, paths, error)) {
        return false;
    }

    locks.reset(new AtomicFileLockSet);
    if (!locks->lock(lockPathsFor(*current, paths), error)) return false;

    if (!journalDirectoryMatches(*current, error)
        || !manifestFileMatches(*current, error)) {
        error = QStringLiteral(
            "The linked-removal journal changed while it was being locked");
        return false;
    }
    state = current;
    return true;
}

bool loadAndLockRuntimeCommit(
    const QString &root,
    const QString &journalPath,
    std::shared_ptr<JournalState> &state,
    std::unique_ptr<AtomicFileLockSet> &locks,
    QString &error,
    const std::shared_ptr<JournalState> &expected = {})
{
    std::shared_ptr<JournalState> current = expected;
    if (current) {
        if (atomicFilePathKey(current->athleteRoot)
                != atomicFilePathKey(root)
            || atomicFilePathKey(current->journalPath)
                != atomicFilePathKey(journalPath)) {
            error = QStringLiteral(
                "The active transaction journal path changed");
            return false;
        }
    } else if (!loadJournalState(
                   root, journalPath, current, error)) {
        return false;
    }
    ResolvedPaths paths;
    if (!journalDirectoryMatches(*current, error)
        || !manifestFileMatches(*current, error)
        || !resolveManifestPaths(*current, paths, error)) {
        return false;
    }

    // The surrounding storage transaction owns source, backup, tombstone,
    // previous-backup, and derived locks. Taking those locks recursively would
    // make markCommitted() fail. This lock set covers the journal-owned files
    // and the peer side of the transaction; startup recovery uses the full set.
    locks.reset(new AtomicFileLockSet);
    QStringList lockPaths = {
        current->journalPath,
        paths.backupStaging};
    if (current->manifest.hasPeer) {
        lockPaths.append(paths.peer);
        lockPaths.append(paths.peerStaging);
    }
    if (!locks->lock(lockPaths, error)) {
        return false;
    }

    if (!journalDirectoryMatches(*current, error)
        || !manifestFileMatches(*current, error)) {
        error = QStringLiteral(
            "The linked-removal journal changed while it was being locked");
        return false;
    }
    state = current;
    return true;
}

bool validatePeerOldCopy(const JournalState &state, QString &error)
{
    if (!state.manifest.hasPeer) {
        ObservedFile observed;
        if (!inspectRegularFile(state.peerOldPath, observed, error))
            return false;
        if (observed.exists) {
            error = QStringLiteral(
                "An unlinked removal journal contains unexpected peer data");
            return false;
        }
        return true;
    }
    return validateExpectedSnapshot(
        state.peerOldPath, state.manifest.peerOld.contents, error);
}

bool validateDerivedForRollback(
    const JournalState &state,
    const ResolvedPaths &paths,
    QString &error)
{
    for (int index = 0; index < state.manifest.derived.size(); ++index) {
        if (!validateExpectedFile(
                paths.derived.at(index),
                state.manifest.derived.at(index),
                error)) {
            return false;
        }
    }
    return true;
}

bool validateOriginalStorageState(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    QList<QPair<QString, ObservedFile>> temporaryFiles;
    if (!inspectJournalDirectory(state, temporaryFiles, error)
        || !validatePeerOldCopy(state, error)) {
        return false;
    }

    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    if (committed) {
        error = QStringLiteral("The linked-removal transaction is committed");
        return false;
    }

    if (!validateExpectedFile(
            paths.source, state.manifest.source, error)
        || !validateExpectedFile(
            paths.backup, state.manifest.previousBackup, error)
        || !validateDerivedForRollback(state, paths, error)) {
        return false;
    }

    const QStringList storageSidecars = {
        paths.backupStaging,
        paths.sourceTombstone,
        paths.previousBackup};
    for (const QString &path : storageSidecars) {
        ObservedFile observed;
        if (!inspectRegularFile(path, observed, error)) return false;
        if (observed.exists) {
            error = QStringLiteral(
                "An activity storage sidecar already exists: %1").arg(path);
            return false;
        }
    }

    if (!state.manifest.hasPeer) {
        if (state.manifest.peerNew.exists) {
            error = QStringLiteral(
                "An unlinked removal journal contains linked-peer state");
            return false;
        }
    } else if (state.manifest.peerNew.exists) {
        if (!validateExpectedSnapshot(
                paths.peer,
                state.manifest.peerNew.contents,
                error)
            || !validateExpectedSnapshot(
                paths.peerStaging,
                state.manifest.peerNew.contents,
                error)) {
            return false;
        }
    } else {
        if (!validateExpectedSnapshot(
                paths.peer,
                state.manifest.peerOld.contents,
                error)) {
            return false;
        }
        ObservedFile staging;
        if (!inspectRegularFile(paths.peerStaging, staging, error)) return false;
        if (staging.exists) {
            error = QStringLiteral(
                "An unrecorded linked-peer staging file exists");
            return false;
        }
    }
    return true;
}

bool markerCanBeCreated(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    if (state.manifest.hasPeer) {
        if (!state.manifest.peerNew.exists) {
            error = QStringLiteral(
                "The linked peer has not been staged for deletion");
            return false;
        }
        if (!validatePeerOldCopy(state, error)
            || !validateExpectedSnapshot(
                paths.peer, state.manifest.peerNew.contents, error)
            || !validateExpectedSnapshot(
                paths.peerStaging, state.manifest.peerNew.contents, error)) {
            return false;
        }
    } else if (!validatePeerOldCopy(state, error)
               || state.manifest.peerNew.exists) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "An unlinked removal journal contains linked-peer state");
        }
        return false;
    }
    if (!validateExpectedSnapshot(
            paths.backup, state.manifest.source.contents, error)) {
        return false;
    }

    ObservedFile source;
    ObservedFile tombstone;
    if (!inspectRegularFile(paths.source, source, error)
        || !inspectRegularFile(paths.sourceTombstone, tombstone, error)) {
        return false;
    }
    if (source.exists
        || !snapshotMatches(tombstone, state.manifest.source.contents)) {
        error = QStringLiteral(
            "The activity source has not been durably tombstoned");
        return false;
    }

    ObservedFile previous;
    if (!inspectRegularFile(paths.previousBackup, previous, error)) return false;
    if (state.manifest.previousBackup.exists) {
        if (!snapshotMatches(
                previous, state.manifest.previousBackup.contents)) {
            error = QStringLiteral(
                "The previous activity backup has not been preserved");
            return false;
        }
    } else if (previous.exists) {
        error = QStringLiteral("An unexpected previous backup exists");
        return false;
    }

    ObservedFile backupStaging;
    if (!inspectRegularFile(paths.backupStaging, backupStaging, error)) {
        return false;
    }
    if (backupStaging.exists
        && !snapshotMatches(
            backupStaging, state.manifest.source.contents)) {
        error = QStringLiteral("The activity backup staging file changed");
        return false;
    }
    return validateDerivedForRollback(state, paths, error);
}

bool restoreSourceForRollback(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    ObservedFile source;
    ObservedFile tombstone;
    if (!inspectRegularFile(paths.source, source, error)
        || !inspectRegularFile(paths.sourceTombstone, tombstone, error)) {
        return false;
    }
    const bool sourceMatches = snapshotMatches(
        source, state.manifest.source.contents);
    const bool tombstoneMatches = snapshotMatches(
        tombstone, state.manifest.source.contents);
    if (source.exists && !sourceMatches) {
        error = QStringLiteral("The activity source changed during recovery");
        return false;
    }
    if (tombstone.exists && !tombstoneMatches) {
        error = QStringLiteral("The activity source tombstone changed");
        return false;
    }
    if (!sourceMatches && !tombstoneMatches) {
        error = QStringLiteral(
            "The activity source and its tombstone are both missing");
        return false;
    }

    if (!sourceMatches
        && !copyExpectedFileAtomically(
            paths.sourceTombstone,
            paths.source,
            state.manifest.source.contents,
            error)) {
        return false;
    }
    return validateExpectedSnapshot(
        paths.source, state.manifest.source.contents, error);
}

bool restoreBackupForRollback(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    ObservedFile backup;
    ObservedFile previous;
    if (!inspectRegularFile(paths.backup, backup, error)
        || !inspectRegularFile(paths.previousBackup, previous, error)) {
        return false;
    }

    const bool backupIsOld = state.manifest.previousBackup.exists
        && snapshotMatches(backup, state.manifest.previousBackup.contents);
    const bool backupIsNew = snapshotMatches(
        backup, state.manifest.source.contents);
    const bool previousIsOld = state.manifest.previousBackup.exists
        && snapshotMatches(previous, state.manifest.previousBackup.contents);

    if (backup.exists && !backupIsOld && !backupIsNew) {
        error = QStringLiteral("The activity backup changed during recovery");
        return false;
    }
    if (previous.exists && !previousIsOld) {
        error = QStringLiteral("The previous activity backup changed");
        return false;
    }

    if (state.manifest.previousBackup.exists) {
        if (!backupIsOld && !previousIsOld) {
            error = QStringLiteral("The previous activity backup is unavailable");
            return false;
        }
        if (!backupIsOld
            && !copyExpectedFileAtomically(
                paths.previousBackup,
                paths.backup,
                state.manifest.previousBackup.contents,
                error)) {
            return false;
        }
        return validateExpectedSnapshot(
            paths.backup, state.manifest.previousBackup.contents, error);
    }

    if (previous.exists) {
        error = QStringLiteral("An unexpected previous activity backup exists");
        return false;
    }
    if (backupIsNew
        && !removeExpectedFile(
            paths.backup, state.manifest.source.contents, error)) {
        return false;
    }
    return !pathEntryExists(paths.backup);
}

bool restorePeerForRollback(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    if (!state.manifest.hasPeer)
        return validatePeerOldCopy(state, error);

    ObservedFile peer;
    if (!inspectRegularFile(paths.peer, peer, error)) return false;
    const bool peerIsOld = snapshotMatches(
        peer, state.manifest.peerOld.contents);
    const bool peerIsNew = state.manifest.peerNew.exists
        && snapshotMatches(peer, state.manifest.peerNew.contents);
    if (peer.exists && !peerIsOld && !peerIsNew) {
        error = QStringLiteral("The linked peer changed during recovery");
        return false;
    }

    if (!peerIsOld
        && !copyExpectedFileAtomically(
            state.peerOldPath,
            paths.peer,
            state.manifest.peerOld.contents,
            error)) {
        return false;
    }
    return validateExpectedSnapshot(
        paths.peer, state.manifest.peerOld.contents, error);
}

bool validateRollbackSidecars(
    const JournalState &state,
    const ResolvedPaths &paths,
    ObservedFile &peerStaging,
    ObservedFile &backupStaging,
    ObservedFile &tombstone,
    ObservedFile &previous,
    QString &error)
{
    if ((state.manifest.hasPeer
         && !inspectRegularFile(paths.peerStaging, peerStaging, error))
        || !inspectRegularFile(paths.backupStaging, backupStaging, error)
        || !inspectRegularFile(paths.sourceTombstone, tombstone, error)
        || !inspectRegularFile(paths.previousBackup, previous, error)) {
        return false;
    }

    if (state.manifest.peerNew.exists && peerStaging.exists
        && !snapshotMatches(peerStaging, state.manifest.peerNew.contents)) {
        error = QStringLiteral("The linked-peer staging file changed");
        return false;
    }
    if (backupStaging.exists
        && !snapshotMatches(backupStaging, state.manifest.source.contents)) {
        error = QStringLiteral("The activity backup staging file changed");
        return false;
    }
    if (tombstone.exists
        && !snapshotMatches(tombstone, state.manifest.source.contents)) {
        error = QStringLiteral("The activity source tombstone changed");
        return false;
    }
    if (previous.exists
        && (!state.manifest.previousBackup.exists
            || !snapshotMatches(
                previous, state.manifest.previousBackup.contents))) {
        error = QStringLiteral("The previous activity backup changed");
        return false;
    }
    return true;
}

bool ensurePeerForCommit(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    if (!state.manifest.hasPeer)
        return validatePeerOldCopy(state, error);

    if (!state.manifest.peerNew.exists) {
        error = QStringLiteral("The committed journal has no linked-peer data");
        return false;
    }

    ObservedFile peer;
    ObservedFile staging;
    if (!inspectRegularFile(paths.peer, peer, error)
        || !inspectRegularFile(paths.peerStaging, staging, error)) {
        return false;
    }
    const bool peerIsOld = snapshotMatches(
        peer, state.manifest.peerOld.contents);
    const bool peerIsNew = snapshotMatches(
        peer, state.manifest.peerNew.contents);
    const bool stagingIsNew = snapshotMatches(
        staging, state.manifest.peerNew.contents);
    if (peer.exists && !peerIsOld && !peerIsNew) {
        error = QStringLiteral("The linked peer changed during recovery");
        return false;
    }
    if (staging.exists && !stagingIsNew) {
        error = QStringLiteral("The linked-peer staging file changed");
        return false;
    }
    if (!peerIsNew) {
        if (!stagingIsNew) {
            error = QStringLiteral("The staged linked-peer update is unavailable");
            return false;
        }
        if (!copyExpectedFileAtomically(
                paths.peerStaging,
                paths.peer,
                state.manifest.peerNew.contents,
                error)) {
            return false;
        }
    }
    return validateExpectedSnapshot(
        paths.peer, state.manifest.peerNew.contents, error);
}

bool ensureBackupStagingForCommit(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    ObservedFile staging;
    if (!inspectRegularFile(paths.backupStaging, staging, error)) return false;
    if (staging.exists) {
        if (!snapshotMatches(staging, state.manifest.source.contents)) {
            error = QStringLiteral("The activity backup staging file changed");
            return false;
        }
        return true;
    }

    ObservedFile source;
    ObservedFile tombstone;
    if (!inspectRegularFile(paths.source, source, error)
        || !inspectRegularFile(paths.sourceTombstone, tombstone, error)) {
        return false;
    }
    const QString copySource = snapshotMatches(
        source, state.manifest.source.contents)
        ? paths.source
        : snapshotMatches(tombstone, state.manifest.source.contents)
            ? paths.sourceTombstone
            : QString();
    if (copySource.isEmpty()) {
        error = QStringLiteral("The activity source is unavailable for archival");
        return false;
    }
    return copyExpectedFileCreateNew(
        copySource,
        paths.backupStaging,
        state.manifest.source.contents,
        error);
}

bool ensureBackupForCommit(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    ObservedFile backup;
    ObservedFile previous;
    if (!inspectRegularFile(paths.backup, backup, error)
        || !inspectRegularFile(paths.previousBackup, previous, error)) {
        return false;
    }
    const bool backupIsOld = state.manifest.previousBackup.exists
        && snapshotMatches(backup, state.manifest.previousBackup.contents);
    const bool backupIsNew = snapshotMatches(
        backup, state.manifest.source.contents);
    const bool previousIsOld = state.manifest.previousBackup.exists
        && snapshotMatches(previous, state.manifest.previousBackup.contents);
    if (backup.exists && !backupIsOld && !backupIsNew) {
        error = QStringLiteral("The activity backup changed during recovery");
        return false;
    }
    if (previous.exists && !previousIsOld) {
        error = QStringLiteral("The previous activity backup changed");
        return false;
    }

    if (backupIsNew) return true;

    if (state.manifest.previousBackup.exists && backupIsOld) {
        if (!previousIsOld
            && !copyExpectedFileAtomically(
                paths.backup,
                paths.previousBackup,
                state.manifest.previousBackup.contents,
                error)) {
            return false;
        }
        if (!removeExpectedFile(
                paths.backup,
                state.manifest.previousBackup.contents,
                error)) {
            return false;
        }
    } else if (backup.exists) {
        error = QStringLiteral("The activity backup is in an unknown state");
        return false;
    }

    if (!ensureBackupStagingForCommit(state, paths, error)
        || !copyExpectedFileAtomically(
            paths.backupStaging,
            paths.backup,
            state.manifest.source.contents,
            error)) {
        return false;
    }
    return true;
}

bool ensureSourceRemovedForCommit(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    ObservedFile source;
    ObservedFile tombstone;
    if (!inspectRegularFile(paths.source, source, error)
        || !inspectRegularFile(paths.sourceTombstone, tombstone, error)) {
        return false;
    }
    const bool sourceMatches = snapshotMatches(
        source, state.manifest.source.contents);
    const bool tombstoneMatches = snapshotMatches(
        tombstone, state.manifest.source.contents);
    if (source.exists && !sourceMatches) {
        error = QStringLiteral("The activity source changed during recovery");
        return false;
    }
    if (tombstone.exists && !tombstoneMatches) {
        error = QStringLiteral("The activity source tombstone changed");
        return false;
    }

    if (sourceMatches && !tombstoneMatches
        && !copyExpectedFileAtomically(
            paths.source,
            paths.sourceTombstone,
            state.manifest.source.contents,
            error)) {
        return false;
    }
    if (sourceMatches
        && !removeExpectedFile(
            paths.source, state.manifest.source.contents, error)) {
        return false;
    }

    if (pathEntryExists(paths.source)) {
        error = QStringLiteral("The committed activity source still exists");
        return false;
    }
    return validateExpectedSnapshot(
        paths.backup, state.manifest.source.contents, error);
}

bool removeDerivedForCommit(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    for (int index = 0; index < state.manifest.derived.size(); ++index) {
        const ExpectedFile &expected = state.manifest.derived.at(index);
        ObservedFile observed;
        if (!inspectRegularFile(paths.derived.at(index), observed, error)) {
            return false;
        }
        if (!expected.exists) {
            if (observed.exists) {
                error = QStringLiteral(
                    "A derived activity file was created during recovery");
                return false;
            }
            continue;
        }
        if (!observed.exists) continue;
        if (!snapshotMatches(observed, expected.contents)) {
            error = QStringLiteral("A derived activity file changed");
            return false;
        }
        if (!removeObservedFile(paths.derived.at(index), observed, error)) {
            return false;
        }
    }
    return true;
}

bool removeJournalDirectory(
    const JournalState &state,
    const QList<QPair<QString, ObservedFile>> &temporaryFiles,
    bool committed,
    QString &error)
{
    for (const auto &temporary : temporaryFiles) {
        if (!removeObservedFile(temporary.first, temporary.second, error)) {
            return false;
        }
    }

    AtomicFileSnapshot markerSnapshot;
    if (committed) {
        const QByteArray markerContents = state.manifest.id.toLatin1() + '\n';
        markerSnapshot.size = markerContents.size();
        markerSnapshot.digest = QCryptographicHash::hash(
            markerContents, QCryptographicHash::Sha256);
        if (!validateExpectedSnapshot(
                state.commitMarkerPath, markerSnapshot, error)) {
            return false;
        }
    }

    if (!removeManifestFile(state, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    rideCacheRemovalTransitionReached(
        "journal-manifest-removed");
#endif
    if (state.manifest.hasPeer
        && !removeExpectedFile(
            state.peerOldPath, state.manifest.peerOld.contents, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (state.manifest.hasPeer) {
        rideCacheRemovalTransitionReached(
            "journal-peer-old-removed");
    }
#endif
    if (committed
        && !removeExpectedFile(
            state.commitMarkerPath, markerSnapshot, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (committed) {
        rideCacheRemovalTransitionReached(
            "journal-commit-marker-removed");
    }
#endif

    const QFileInfoList remaining = QDir(state.journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System);
    if (!remaining.isEmpty()) {
        error = QStringLiteral(
            "The transaction journal contains files that cannot be removed");
        return false;
    }
    if (!removeDirectoryDurably(state.journalPath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot remove the completed transaction journal");
        }
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    rideCacheRemovalTransitionReached(
        "journal-directory-removed");
#endif
    return true;
}

bool rollbackJournal(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    QList<QPair<QString, ObservedFile>> temporaryFiles;
    if (!journalDirectoryMatches(state, error)
        || !manifestFileMatches(state, error)
        || !inspectJournalDirectory(state, temporaryFiles, error)
        || !validatePeerOldCopy(state, error)
        || !validateDerivedForRollback(state, paths, error)) {
        return false;
    }

    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    if (committed) {
        error = QStringLiteral("Cannot roll back a committed transaction");
        return false;
    }

    ObservedFile peerStaging;
    ObservedFile backupStaging;
    ObservedFile tombstone;
    ObservedFile previous;
    if (!validateRollbackSidecars(
            state,
            paths,
            peerStaging,
            backupStaging,
            tombstone,
            previous,
            error)) {
        return false;
    }

    // Restore the only durable source before discarding any archived copies.
    if (!restoreSourceForRollback(state, paths, error)
        || !restoreBackupForRollback(state, paths, error)
        || !restorePeerForRollback(state, paths, error)) {
        return false;
    }

    if (!validateDerivedForRollback(state, paths, error)) return false;

    if (state.manifest.hasPeer
        && peerStaging.exists
        && !removeObservedFile(paths.peerStaging, peerStaging, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (state.manifest.hasPeer && peerStaging.exists) {
        rideCacheRemovalTransitionReached(
            "rollback-peer-staging-removed");
    }
#endif
    if (backupStaging.exists
        && !removeExpectedFile(
            paths.backupStaging, state.manifest.source.contents, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (backupStaging.exists) {
        rideCacheRemovalTransitionReached(
            "rollback-backup-staging-removed");
    }
#endif
    if (pathEntryExists(paths.sourceTombstone)
        && !removeExpectedFile(
            paths.sourceTombstone, state.manifest.source.contents, error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (tombstone.exists) {
        rideCacheRemovalTransitionReached(
            "rollback-source-tombstone-removed");
    }
#endif
    if (state.manifest.previousBackup.exists
        && pathEntryExists(paths.previousBackup)
        && !removeExpectedFile(
            paths.previousBackup,
            state.manifest.previousBackup.contents,
            error)) {
        return false;
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (previous.exists) {
        rideCacheRemovalTransitionReached(
            "rollback-previous-backup-removed");
    }
#endif

    return removeJournalDirectory(
        state, temporaryFiles, false, error);
}

bool commitJournal(
    const JournalState &state, const ResolvedPaths &paths, QString &error)
{
    QList<QPair<QString, ObservedFile>> temporaryFiles;
    if (!journalDirectoryMatches(state, error)
        || !manifestFileMatches(state, error)
        || !inspectJournalDirectory(state, temporaryFiles, error)
        || !validatePeerOldCopy(state, error)) {
        return false;
    }

    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    if (!committed) {
        error = QStringLiteral("Cannot complete an uncommitted transaction");
        return false;
    }

    // The marker is the decision point: every subsequent restart rolls forward.
    if (!ensurePeerForCommit(state, paths, error)
        || !ensureBackupForCommit(state, paths, error)
        || !ensureSourceRemovedForCommit(state, paths, error)
        || !removeDerivedForCommit(state, paths, error)) {
        return false;
    }

    if (state.manifest.previousBackup.exists
        && pathEntryExists(paths.previousBackup)
        && !removeExpectedFile(
            paths.previousBackup,
            state.manifest.previousBackup.contents,
            error)) {
        return false;
    }
    if (pathEntryExists(paths.sourceTombstone)
        && !removeExpectedFile(
            paths.sourceTombstone, state.manifest.source.contents, error)) {
        return false;
    }
    if (pathEntryExists(paths.backupStaging)
        && !removeExpectedFile(
            paths.backupStaging, state.manifest.source.contents, error)) {
        return false;
    }
    if (state.manifest.hasPeer
        && pathEntryExists(paths.peerStaging)) {
        if (!removeExpectedFile(
                paths.peerStaging,
                state.manifest.peerNew.contents,
                error)) {
            return false;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "journal-peer-staging-removed");
#endif
    }

    return removeJournalDirectory(
        state, temporaryFiles, true, error);
}

bool reconcileLockedJournal(
    const JournalState &state, QString &error)
{
    ResolvedPaths paths;
    if (!resolveManifestPaths(state, paths, error)) return false;
    bool committed = false;
    if (!readCommitMarker(state, committed, error)) return false;
    return committed
        ? commitJournal(state, paths, error)
        : rollbackJournal(state, paths, error);
}

bool removePreManifestJournal(
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

    const QFileInfoList entries = QDir(journalPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot
            | QDir::Hidden | QDir::System,
        QDir::Name);
    QList<QPair<QString, ObservedFile>> removable;
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || !entry.isFile()
            || (entry.fileName() != Detail::PeerOldName
                && entry.fileName() != Detail::CommitMarkerName
                && !isKnownTemporaryName(entry.fileName()))) {
            error = QStringLiteral(
                "An incomplete transaction contains an unknown entry");
            return false;
        }
        const qint64 maximumSize =
            entry.fileName() == Detail::CommitMarkerName
            ? Detail::MaximumCommitMarkerSize
            : knownTemporaryMaximumSize(entry.fileName());
        ObservedFile observed;
        if (!inspectRegularFile(
                entry.absoluteFilePath(), observed, error, maximumSize)) {
            return false;
        }
        removable.append({entry.absoluteFilePath(), observed});
    }

    for (const auto &entry : removable) {
        if (!removeObservedFile(entry.first, entry.second, error)) return false;
    }
    if (!removeDirectoryDurably(journalPath, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Cannot remove an incomplete transaction journal");
        }
        return false;
    }
    return true;
}

class JournalPeerWriter final : public AtomicFileWriter
{
public:
    JournalPeerWriter(
        std::shared_ptr<JournalState> state,
        AtomicFileWriterFactory delegateFactory,
        QString path,
        AtomicFileMode mode)
        : state_(std::move(state)),
          delegateFactory_(std::move(delegateFactory)),
          path_(std::move(path)),
          mode_(mode)
    {
    }

    bool open() override
    {
        if (opened_) {
            error_ = QStringLiteral("The linked-peer writer is already open");
            return false;
        }
        if (!state_ || !state_->manifest.hasPeer || !delegateFactory_) {
            error_ = QStringLiteral("The linked-peer writer is unavailable");
            return false;
        }

        ResolvedPaths paths;
        if (!journalDirectoryMatches(*state_, error_)
            || !resolveManifestPaths(*state_, paths, error_)
            || atomicFilePathKey(path_) != atomicFilePathKey(paths.peer)
            || mode_ != AtomicFileMode::ReplaceExisting) {
            if (error_.isEmpty()) {
                error_ = QStringLiteral(
                    "The linked-peer writer cannot change the peer path");
            }
            return false;
        }
        if (state_->manifest.peerNew.exists) {
            error_ = QStringLiteral("The linked peer has already been staged");
            return false;
        }

        AtomicFileLockSet locks;
        if (!locks.lock({state_->journalPath, paths.peerStaging}, error_)
            || !validatePeerOldCopy(*state_, error_)
            || !validateExpectedSnapshot(
                paths.peer, state_->manifest.peerOld.contents, error_)) {
            return false;
        }
        ObservedFile staging;
        if (!inspectRegularFile(paths.peerStaging, staging, error_)) return false;
        if (staging.exists) {
            error_ = QStringLiteral(
                "A linked-peer staging file already exists");
            return false;
        }

        delegate_ = delegateFactory_(path_, mode_);
        if (!delegate_) {
            error_ = QStringLiteral("Cannot create the linked-peer writer");
            return false;
        }
        if (!delegate_->open()) {
            error_ = atomicFileError(
                QStringLiteral("Cannot open the linked-peer writer"),
                *delegate_);
            return false;
        }
        opened_ = true;
        return true;
    }

    qint64 write(const QByteArray &data) override
    {
        if (!opened_ || !delegate_ || committed_) {
            error_ = QStringLiteral("The linked-peer writer is not writable");
            return -1;
        }
        const qint64 written = delegate_->write(data);
        if (written > 0) {
            accumulated_.append(data.constData(), static_cast<int>(written));
        }
        return written;
    }

    bool flush() override
    {
        if (!opened_ || !delegate_ || committed_) {
            error_ = QStringLiteral("The linked-peer writer is not open");
            return false;
        }
        if (!delegate_->flush()) {
            error_ = atomicFileError(
                QStringLiteral("Cannot flush the linked peer"), *delegate_);
            return false;
        }
        flushed_ = true;
        return true;
    }

    bool commit() override
    {
        if (!opened_ || !delegate_ || committed_ || !flushed_) {
            error_ = QStringLiteral(
                "The linked-peer writer is not ready to commit");
            return false;
        }

        ResolvedPaths paths;
        if (!journalDirectoryMatches(*state_, error_)
            || !resolveManifestPaths(*state_, paths, error_)) {
            return false;
        }
        AtomicFileLockSet locks;
        if (!locks.lock(
                {state_->journalPath, paths.peerStaging}, error_)
            || !manifestFileMatches(*state_, error_)
            || !validateExpectedSnapshot(
                paths.peer,
                state_->manifest.peerOld.contents,
                error_)) {
            return false;
        }

        AtomicFileSnapshot stagedSnapshot;
        if (!writeBytesCreateNew(
                paths.peerStaging,
                accumulated_,
                true,
                stagedSnapshot,
                error_)) {
            return false;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "peer-staging-published");
#endif

        Manifest updated = state_->manifest;
        updated.peerNew.exists = true;
        updated.peerNew.contents = stagedSnapshot;
        AtomicFileSnapshot updatedManifestSnapshot;
        if (!writeManifestFile(
                state_->manifestPath,
                updated,
                true,
                true,
                updatedManifestSnapshot,
                error_)) {
            return false;
        }
        if (!pinManifestFile(
                *state_, &updatedManifestSnapshot, error_)) {
            return false;
        }

        state_->manifest = updated;
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "peer-manifest-published");
#endif
        if (!delegate_->commit()) {
            error_ = atomicFileError(
                QStringLiteral("Cannot commit the linked peer"), *delegate_);
            return false;
        }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
        rideCacheRemovalTransitionReached(
            "peer-file-committed");
#endif
        committed_ = true;
        return true;
    }

    void cancelWriting() override
    {
        if (delegate_) delegate_->cancelWriting();
        opened_ = false;
    }

    QString errorString() const override
    {
        if (!error_.isEmpty()) return error_;
        return delegate_ ? delegate_->errorString() : QString();
    }

private:
    std::shared_ptr<JournalState> state_;
    AtomicFileWriterFactory delegateFactory_;
    QString path_;
    AtomicFileMode mode_ = AtomicFileMode::ReplaceExisting;
    std::unique_ptr<AtomicFileWriter> delegate_;
    QByteArray accumulated_;
    QString error_;
    bool opened_ = false;
    bool flushed_ = false;
    bool committed_ = false;
};

} // namespace

Journal::Journal(std::shared_ptr<JournalState> state)
    : state_(std::move(state))
{
}

std::shared_ptr<Journal> Journal::prepare(
    const Specification &specification, QString &error)
{
    error.clear();
    QString root;
    if (!normalizeAthleteRoot(specification.athleteRoot, root, error)) {
        return {};
    }

    QString sourceRelative;
    QString sourcePath;
    QString backupRelative;
    QString backupPath;
    QString peerRelative;
    QString peerPath;
    const bool hasPeer = !specification.peerPath.isEmpty();
    if (!makeRootRelativePath(
            root,
            specification.sourcePath,
            sourceRelative,
            sourcePath,
            error)
        || !makeRootRelativePath(
            root,
            specification.backupPath,
            backupRelative,
            backupPath,
            error)) {
        return {};
    }
    if (hasPeer
        && !makeRootRelativePath(
            root,
            specification.peerPath,
            peerRelative,
            peerPath,
            error)) {
        return {};
    }

    QStringList derivedRelative;
    QStringList derivedPaths;
    if (specification.derivedPaths.size() > Detail::MaximumDerivedPaths) {
        error = QStringLiteral("Too many derived activity paths");
        return {};
    }
    for (const QString &candidate : specification.derivedPaths) {
        QString relative;
        QString absolute;
        if (!makeRootRelativePath(
                root,
                candidate,
                relative,
                absolute,
                error,
                true)) {
            return {};
        }
        derivedRelative.append(relative);
        derivedPaths.append(absolute);
    }

    const QString id = QUuid::createUuid()
                           .toString(QUuid::WithoutBraces)
                           .toLower();
    std::shared_ptr<JournalState> state(new JournalState);
    state->transactionLease.reset(new AtomicFileLockSet);
    if (!state->transactionLease->lock(
            {transactionLeaseTarget(root)}, error)) {
        return {};
    }
    QString namespacePath;
    if (!ensureTransactionNamespace(root, namespacePath, error)) return {};
    if (!transactionNamespaceIsReady(
            root, namespacePath, error)) return {};
    const QString journalPath = QDir(namespacePath).filePath(id);

    state->athleteRoot = root;
    state->namespacePath = namespacePath;
    state->journalPath = journalPath;
    state->manifestPath = QDir(journalPath).filePath(Detail::ManifestName);
    state->peerOldPath = QDir(journalPath).filePath(Detail::PeerOldName);
    state->commitMarkerPath = QDir(journalPath).filePath(
        Detail::CommitMarkerName);
    state->manifest.id = id;
    state->manifest.hasPeer = hasPeer;

    ResolvedPaths fixedPaths;
    fixedPaths.source = sourcePath;
    fixedPaths.backup = backupPath;
    if (hasPeer)
        fixedPaths.peer = peerPath;
    fixedPaths.derived = derivedPaths;
    fixedPaths.backupStaging = backupPath
        + QStringLiteral(".gc-copy-") + id;
    fixedPaths.sourceTombstone = sourcePath
        + QStringLiteral(".gc-remove-") + id;
    fixedPaths.previousBackup = backupPath
        + QStringLiteral(".gc-previous-") + id;
    if (hasPeer) {
        fixedPaths.peerStaging = peerPath
            + QStringLiteral(".gc-linked-new-") + id;
    }

    QStringList pathsToLock = {
        journalPath,
        fixedPaths.source,
        fixedPaths.backup,
        fixedPaths.backupStaging,
        fixedPaths.sourceTombstone,
        fixedPaths.previousBackup};
    if (hasPeer) {
        pathsToLock.append(fixedPaths.peer);
        pathsToLock.append(fixedPaths.peerStaging);
    }
    for (const QString &derived : fixedPaths.derived) {
        if (QFileInfo(derived).absoluteDir().exists())
            pathsToLock.append(derived);
    }
    AtomicFileLockSet locks;
    if (!locks.lock(pathsToLock, error)) return {};

    QSet<QString> uniquePaths;
    const auto addUnique = [&](const QString &path) {
        const QString key = atomicFilePathKey(path);
        if (uniquePaths.contains(key)) return false;
        uniquePaths.insert(key);
        return true;
    };
    QStringList uniqueCandidates = {
        fixedPaths.source,
        fixedPaths.backup,
        fixedPaths.backupStaging,
        fixedPaths.sourceTombstone,
        fixedPaths.previousBackup};
    if (hasPeer) {
        uniqueCandidates.append(fixedPaths.peer);
        uniqueCandidates.append(fixedPaths.peerStaging);
    }
    uniqueCandidates.append(fixedPaths.derived);
    for (const QString &path : uniqueCandidates) {
        if (!addUnique(path)) {
            error = QStringLiteral("Activity transaction paths overlap");
            return {};
        }
    }

    QStringList sidecars = {
        fixedPaths.backupStaging,
        fixedPaths.sourceTombstone,
        fixedPaths.previousBackup};
    if (hasPeer)
        sidecars.append(fixedPaths.peerStaging);
    for (const QString &sidecar : sidecars) {
        QString relative;
        QString absolute;
        if (!makeRootRelativePath(
                root, sidecar, relative, absolute, error)) {
            return {};
        }
        ObservedFile observed;
        if (!inspectRegularFile(sidecar, observed, error)) return {};
        if (observed.exists) {
            error = QStringLiteral(
                "An activity transaction sidecar already exists");
            return {};
        }
    }

    if (!inspectExpectedFile(
            sourcePath,
            sourceRelative,
            true,
            state->manifest.source,
            error)
        || !inspectExpectedFile(
            backupPath,
            backupRelative,
            false,
            state->manifest.previousBackup,
            error)) {
        return {};
    }
    if (hasPeer) {
        if (!inspectExpectedFile(
                peerPath,
                peerRelative,
                true,
                state->manifest.peerOld,
                error)) {
            return {};
        }
        state->manifest.peerNew.relativePath = peerRelative;
    }

    for (int index = 0; index < derivedPaths.size(); ++index) {
        ExpectedFile derived;
        if (!inspectExpectedFile(
                derivedPaths.at(index),
                derivedRelative.at(index),
                false,
                derived,
                error)) {
            return {};
        }
        state->manifest.derived.append(derived);
    }

    if (!ensurePrivateDirectory(journalPath, error)) return {};
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            journalPath, state->journalDirectory, error)) {
        error = QStringLiteral(
            "Cannot anchor the new transaction journal directory: %1")
                    .arg(error);
        return {};
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    rideCacheRemovalTransitionReached(
        "journal-directory-created");
#endif
    if (!journalDirectoryMatches(*state, error)) return {};

    if (hasPeer
        && !copyExpectedFileCreateNew(
            peerPath,
            state->peerOldPath,
            state->manifest.peerOld.contents,
            error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1").arg(journalPath));
        return {};
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    if (hasPeer) {
        rideCacheRemovalTransitionReached(
            "journal-peer-old-published");
    }
#endif
    if (!journalDirectoryMatches(*state, error)) return {};
    if (!writeManifestFile(
            state->manifestPath,
            state->manifest,
            false,
            false,
            state->manifestSnapshot,
            error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1").arg(journalPath));
        return {};
    }
    if (!pinManifestFile(
            *state, &state->manifestSnapshot, error)) {
        appendError(
            error,
            QStringLiteral("recovery journal retained at %1").arg(journalPath));
        return {};
    }
#ifdef GC_RIDE_CACHE_REMOVAL_TEST_HOOKS
    rideCacheRemovalTransitionReached(
        "journal-initial-manifest-published");
#endif
    if (!journalDirectoryMatches(*state, error)) return {};

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
            "A linked activity removal transaction is already active: %1")
                    .arg(error);
        return false;
    }
    const QString requestedNamespace =
        transactionNamespacePath(requestedRoot);
    const QFileInfo requestedNamespaceInfo(
        requestedNamespace);
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
        if (entry.isFile() && !entry.isSymLink()
            && name.startsWith(QLatin1Char('.'))
            && name.endsWith(QStringLiteral(".lock"))) {
            const QString lockedId = name.mid(1, name.size() - 6);
            if (validTransactionId(lockedId)) continue;
        }
        if (entry.isSymLink() || !entry.isDir()
            || !validTransactionId(name)) {
            failures.append(QStringLiteral(
                "Unknown entry in the linked-removal journal namespace: %1")
                                .arg(name));
            continue;
        }

        const QString manifestPath = QDir(entry.absoluteFilePath()).filePath(
            Detail::ManifestName);
        const QFileInfo manifestInfo(manifestPath);
        QString transactionError;
        if (!manifestInfo.exists() && !manifestInfo.isSymLink()) {
            if (!removePreManifestJournal(
                    entry.absoluteFilePath(),
                    transactionError)) {
                failures.append(transactionError);
            }
            continue;
        }

        std::shared_ptr<JournalState> state;
        std::unique_ptr<AtomicFileLockSet> locks;
        if (!loadAndLockJournal(
                root,
                entry.absoluteFilePath(),
                state,
                locks,
                transactionError)
            || !reconcileLockedJournal(*state, transactionError)) {
            failures.append(transactionError);
        }
    }

    if (!failures.isEmpty()) {
        error = failures.join(QStringLiteral("; "));
        return false;
    }
    return true;
}

QString Journal::transactionId() const
{
    return state_ ? state_->manifest.id : QString();
}

QString Journal::backupStagingPath() const
{
    if (!state_) return {};
    ResolvedPaths paths;
    QString error;
    return resolveManifestPaths(*state_, paths, error)
        ? paths.backupStaging : QString();
}

QString Journal::sourceTombstonePath() const
{
    if (!state_) return {};
    ResolvedPaths paths;
    QString error;
    return resolveManifestPaths(*state_, paths, error)
        ? paths.sourceTombstone : QString();
}

QString Journal::previousBackupPath() const
{
    if (!state_) return {};
    ResolvedPaths paths;
    QString error;
    return resolveManifestPaths(*state_, paths, error)
        ? paths.previousBackup : QString();
}

QString Journal::peerStagingPath() const
{
    if (!state_) return {};
    ResolvedPaths paths;
    QString error;
    return resolveManifestPaths(*state_, paths, error)
        ? paths.peerStaging : QString();
}

QString Journal::directoryPath() const
{
    return state_ ? state_->journalPath : QString();
}

AtomicFileWriterFactory Journal::peerWriterFactory(
    const AtomicFileWriterFactory &delegate)
{
    const std::shared_ptr<Journal> owner = shared_from_this();
    const std::shared_ptr<JournalState> state = state_;
    return [owner, state, delegate](const QString &path, AtomicFileMode mode) {
        (void)owner;
        return std::unique_ptr<AtomicFileWriter>(
            new JournalPeerWriter(state, delegate, path, mode));
    };
}

bool Journal::validateOriginalStorage(QString &error) const
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-removal journal is unavailable");
        return false;
    }

    std::shared_ptr<JournalState> current;
    std::unique_ptr<AtomicFileLockSet> locks;
    if (!loadAndLockJournal(
            state_->athleteRoot,
            state_->journalPath,
            current,
            locks,
            error,
            state_)) {
        return false;
    }
    ResolvedPaths paths;
    return resolveManifestPaths(*current, paths, error)
        && validateOriginalStorageState(*current, paths, error);
}

bool Journal::markCommitted(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-removal journal is unavailable");
        return false;
    }

    std::shared_ptr<JournalState> current;
    std::unique_ptr<AtomicFileLockSet> locks;
    if (!loadAndLockRuntimeCommit(
            state_->athleteRoot,
            state_->journalPath,
            current,
            locks,
            error,
            state_)) {
        return false;
    }

    bool committed = false;
    if (!readCommitMarker(*current, committed, error)) return false;
    if (committed) return true;

    ResolvedPaths paths;
    if (!resolveManifestPaths(*current, paths, error)
        || !markerCanBeCreated(*current, paths, error)) {
        return false;
    }

    AtomicFileSnapshot markerSnapshot;
    const QByteArray contents = current->manifest.id.toLatin1() + '\n';
    if (!writeBytesCreateNew(
            current->commitMarkerPath,
            contents,
            true,
            markerSnapshot,
            error)) {
        return false;
    }
    bool markerExists = false;
    return readCommitMarker(*current, markerExists, error) && markerExists;
}

bool Journal::hasCommitMarker() const
{
    QString ignored;
    return state_
        && journalDirectoryMatches(*state_, ignored)
        && pathEntryExists(state_->commitMarkerPath);
}

bool Journal::cleanupAfterRollback(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-removal journal is unavailable");
        return false;
    }
    if (state_->cleanupComplete) return true;
    if (!journalDirectoryMatches(*state_, error)) return false;
    const QFileInfo journalInfo(state_->journalPath);
    if (!journalInfo.exists() && !journalInfo.isSymLink()) return true;

    std::shared_ptr<JournalState> current;
    std::unique_ptr<AtomicFileLockSet> locks;
    if (!loadAndLockJournal(
            state_->athleteRoot,
            state_->journalPath,
            current,
            locks,
            error,
            state_)) {
        return false;
    }
    ResolvedPaths paths;
    const bool cleaned = resolveManifestPaths(*current, paths, error)
        && rollbackJournal(*current, paths, error);
    if (cleaned) state_->cleanupComplete = true;
    return cleaned;
}

bool Journal::cleanupAfterCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral("The linked-removal journal is unavailable");
        return false;
    }
    if (state_->cleanupComplete) return true;
    if (!journalDirectoryMatches(*state_, error)) return false;
    const QFileInfo journalInfo(state_->journalPath);
    if (!journalInfo.exists() && !journalInfo.isSymLink()) return true;

    std::shared_ptr<JournalState> current;
    std::unique_ptr<AtomicFileLockSet> locks;
    if (!loadAndLockJournal(
            state_->athleteRoot,
            state_->journalPath,
            current,
            locks,
            error,
            state_)) {
        return false;
    }
    ResolvedPaths paths;
    const bool cleaned = resolveManifestPaths(*current, paths, error)
        && commitJournal(*current, paths, error);
    if (cleaned) state_->cleanupComplete = true;
    return cleaned;
}

QStringList Journal::recoveryPaths() const
{
    QStringList paths;
    if (!state_) return paths;
    QString generationError;
    if (!journalDirectoryMatches(*state_, generationError)) return paths;

    const auto addMatchingFile = [&paths](
        const QString &path,
        const AtomicFileSnapshot &expected) {
        ObservedFile observed;
        QString ignored;
        if (inspectRegularFile(
                path, observed, ignored, expected.size)
            && snapshotMatches(observed, expected)) {
            paths.append(path);
        }
    };
    const auto addMatchingJournal = [&] {
        QString ignored;
        if (manifestFileMatches(*state_, ignored)) {
            paths.append(state_->journalPath);
        }
    };

    ResolvedPaths resolved;
    QString error;
    if (!resolveManifestPaths(*state_, resolved, error)) {
        addMatchingJournal();
        return paths;
    }

    addMatchingJournal();
    addMatchingFile(
        resolved.backupStaging,
        state_->manifest.source.contents);
    addMatchingFile(
        resolved.sourceTombstone,
        state_->manifest.source.contents);
    if (state_->manifest.previousBackup.exists) {
        addMatchingFile(
            resolved.previousBackup,
            state_->manifest.previousBackup.contents);
    }
    if (state_->manifest.hasPeer
        && state_->manifest.peerNew.exists) {
        addMatchingFile(
            resolved.peerStaging,
            state_->manifest.peerNew.contents);
    }
    return paths;
}

} // namespace LinkedActivityRemoval
