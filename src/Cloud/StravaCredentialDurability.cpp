/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaCredentialDurability.h"

#include "FileIO/AnchoredFileSystem.h"
#include "FileIO/AtomicFileWriter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef GC_STRAVA_CREDENTIAL_TEST_HOOKS
void stravaCredentialDurabilityTransitionReached(
    const char *transition);
#endif

namespace StravaCredentialDurability {
namespace {

void reportCredentialTransition(const char *transition)
{
#ifdef GC_STRAVA_CREDENTIAL_TEST_HOOKS
    stravaCredentialDurabilityTransitionReached(transition);
#else
    Q_UNUSED(transition)
#endif
}

constexpr int LegacyJournalVersion = 1;
constexpr int JournalVersion = 2;
constexpr qint64 MaximumJournalBytes = 16 * 1024;
constexpr qint64 MaximumLockBytes = 4 * 1024;
constexpr qsizetype MaximumPendingBytes = 64 * 1024;
constexpr qsizetype MaximumTokenCharacters = 16 * 1024;
constexpr qsizetype MaximumTimestampCharacters = 256;
const QString NamespaceName =
    QStringLiteral(".strava-credential-durability");
const QString StateFileName = QStringLiteral("state.json");
const QString LockFileName = QStringLiteral("mutation.lock");

enum class Phase
{
    Idle,
    RemotePending,
    CommitUnknown,
    LocalCommitPending,
    PublicationPending
};

struct JournalState
{
    QString accountDigest;
    quint64 generation = 0;
    Phase phase = Phase::Idle;
    QString transactionId;
    MutationKind kind = MutationKind::Refresh;
    QString previousAuthorizationState;
    bool previousRemoteGrantUncertain = false;
    bool previousAuthorizationStateKnown = true;
};

QString accountDigest(const QString &accountKey)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            accountKey.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

bool validTransactionId(const QString &transactionId)
{
    static const QRegularExpression expression(
        QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
            "[0-9a-f]{4}-[0-9a-f]{12}$"));
    return expression.match(transactionId).hasMatch();
}

bool validAuthorizationState(const QString &state)
{
    return state == QStringLiteral("active")
        || state == QStringLiteral("revoked")
        || state == QStringLiteral("authorization_pending")
        || state == QStringLiteral("revocation_pending");
}

QString phaseName(Phase phase)
{
    switch (phase) {
    case Phase::Idle:
        return QStringLiteral("idle");
    case Phase::RemotePending:
        return QStringLiteral("remote_pending");
    case Phase::CommitUnknown:
        return QStringLiteral("commit_unknown");
    case Phase::LocalCommitPending:
        return QStringLiteral("local_commit_pending");
    case Phase::PublicationPending:
        return QStringLiteral("publication_pending");
    }
    return {};
}

bool parsePhase(const QString &stored, Phase &phase)
{
    if (stored == QStringLiteral("idle")) {
        phase = Phase::Idle;
        return true;
    }
    if (stored == QStringLiteral("remote_pending")) {
        phase = Phase::RemotePending;
        return true;
    }
    if (stored == QStringLiteral("commit_unknown")) {
        phase = Phase::CommitUnknown;
        return true;
    }
    if (stored == QStringLiteral("local_commit_pending")) {
        phase = Phase::LocalCommitPending;
        return true;
    }
    if (stored == QStringLiteral("publication_pending")) {
        phase = Phase::PublicationPending;
        return true;
    }
    return false;
}

QString kindName(MutationKind kind)
{
    switch (kind) {
    case MutationKind::Refresh:
        return QStringLiteral("refresh");
    case MutationKind::Authorization:
        return QStringLiteral("authorization");
    case MutationKind::Revocation:
        return QStringLiteral("revocation");
    }
    return {};
}

bool parseKind(const QString &stored, MutationKind &kind)
{
    if (stored == QStringLiteral("refresh")) {
        kind = MutationKind::Refresh;
        return true;
    }
    if (stored == QStringLiteral("authorization")) {
        kind = MutationKind::Authorization;
        return true;
    }
    if (stored == QStringLiteral("revocation")) {
        kind = MutationKind::Revocation;
        return true;
    }
    return false;
}

QByteArray serializeJournal(const JournalState &state)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), JournalVersion);
    object.insert(
        QStringLiteral("account_sha256"), state.accountDigest);
    object.insert(
        QStringLiteral("generation"),
        QString::number(state.generation));
    object.insert(QStringLiteral("phase"), phaseName(state.phase));
    object.insert(
        QStringLiteral("transaction_id"), state.transactionId);
    object.insert(
        QStringLiteral("kind"),
        state.phase == Phase::Idle ? QString() : kindName(state.kind));
    object.insert(
        QStringLiteral("previous_authorization_state"),
        state.phase == Phase::Idle
            ? QString() : state.previousAuthorizationState);
    object.insert(
        QStringLiteral("previous_remote_grant_uncertain"),
        state.phase == Phase::Idle
            ? false : state.previousRemoteGrantUncertain);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool exactKeys(
    const QJsonObject &object,
    const QStringList &expected)
{
    QStringList actual = object.keys();
    QStringList sortedExpected = expected;
    std::sort(actual.begin(), actual.end());
    std::sort(sortedExpected.begin(), sortedExpected.end());
    return actual == sortedExpected;
}

bool parseGeneration(const QJsonValue &value, quint64 &generation)
{
    if (!value.isString()) return false;
    bool ok = false;
    generation = value.toString().toULongLong(&ok, 10);
    return ok;
}

bool parseJournal(
    const QByteArray &data,
    const QString &expectedAccountDigest,
    JournalState &state,
    QString &error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "The Strava credential journal is malformed");
        return false;
    }
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("version")).isDouble()) {
        error = QStringLiteral(
            "The Strava credential journal schema is invalid");
        return false;
    }
    const int version = object.value(QStringLiteral("version")).toInt(-1);
    const QStringList commonKeys = {
            QStringLiteral("version"),
            QStringLiteral("account_sha256"),
            QStringLiteral("generation"),
            QStringLiteral("phase"),
            QStringLiteral("transaction_id"),
            QStringLiteral("kind")
        };
    QStringList expectedKeys = commonKeys;
    if (version == JournalVersion) {
        expectedKeys.append(QStringLiteral("previous_authorization_state"));
        expectedKeys.append(
            QStringLiteral("previous_remote_grant_uncertain"));
    }
    if ((version != LegacyJournalVersion && version != JournalVersion)
        || !exactKeys(object, expectedKeys)
        || !object.value(QStringLiteral("account_sha256")).isString()
        || !object.value(QStringLiteral("phase")).isString()
        || !object.value(QStringLiteral("transaction_id")).isString()
        || !object.value(QStringLiteral("kind")).isString()
        || (version == JournalVersion
            && (!object.value(
                    QStringLiteral("previous_authorization_state")).isString()
                || !object.value(
                    QStringLiteral(
                        "previous_remote_grant_uncertain")).isBool()))) {
        error = QStringLiteral(
            "The Strava credential journal schema is invalid");
        return false;
    }

    state.accountDigest =
        object.value(QStringLiteral("account_sha256")).toString();
    state.transactionId =
        object.value(QStringLiteral("transaction_id")).toString();
    state.previousAuthorizationStateKnown = version == JournalVersion;
    if (state.previousAuthorizationStateKnown) {
        state.previousAuthorizationState = object.value(
            QStringLiteral("previous_authorization_state")).toString();
        state.previousRemoteGrantUncertain = object.value(
            QStringLiteral("previous_remote_grant_uncertain")).toBool();
    } else {
        state.previousAuthorizationState.clear();
        state.previousRemoteGrantUncertain = true;
    }
    if (state.accountDigest != expectedAccountDigest
        || !parseGeneration(
            object.value(QStringLiteral("generation")),
            state.generation)
        || !parsePhase(
            object.value(QStringLiteral("phase")).toString(),
            state.phase)) {
        error = QStringLiteral(
            "The Strava credential journal identity is invalid");
        return false;
    }
    if (!state.previousAuthorizationStateKnown
        && state.phase != Phase::Idle) {
        state.previousAuthorizationState =
            QStringLiteral("authorization_pending");
    }

    const QString storedKind =
        object.value(QStringLiteral("kind")).toString();
    if (state.phase == Phase::Idle) {
        if (!state.transactionId.isEmpty() || !storedKind.isEmpty()
            || (state.previousAuthorizationStateKnown
                && (!state.previousAuthorizationState.isEmpty()
                    || state.previousRemoteGrantUncertain))) {
            error = QStringLiteral(
                "The idle Strava credential journal is invalid");
            return false;
        }
        state.kind = MutationKind::Refresh;
        return true;
    }
    if (!validTransactionId(state.transactionId)
        || (state.previousAuthorizationStateKnown
            && !validAuthorizationState(
                state.previousAuthorizationState))
        || !parseKind(storedKind, state.kind)) {
        error = QStringLiteral(
            "The pending Strava credential journal is invalid");
        return false;
    }
    return true;
}

QString modeName(StravaTokenPublication::PublicationMode mode)
{
    return mode
            == StravaTokenPublication::PublicationMode::Authoritative
        ? QStringLiteral("authoritative")
        : QStringLiteral("compare_and_swap");
}

bool parseMode(
    const QString &stored,
    StravaTokenPublication::PublicationMode &mode)
{
    if (stored == QStringLiteral("authoritative")) {
        mode = StravaTokenPublication::PublicationMode::Authoritative;
        return true;
    }
    if (stored == QStringLiteral("compare_and_swap")) {
        mode = StravaTokenPublication::PublicationMode::CompareAndSwap;
        return true;
    }
    return false;
}

struct RemovalIntent
{
    QString expectedRefreshToken;
    StravaTokenPublication::PublicationMode mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;

    bool isValid() const
    {
        const bool supportedMode = mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || mode
                == StravaTokenPublication::PublicationMode::CompareAndSwap;
        return supportedMode
            && expectedRefreshToken.size() <= MaximumTokenCharacters
            && (mode
                    == StravaTokenPublication::PublicationMode::Authoritative
                || !expectedRefreshToken.isEmpty());
    }
};

QString serializePendingRemoval(
    const JournalState &journal,
    const RemovalIntent &removal)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), JournalVersion);
    object.insert(
        QStringLiteral("account_sha256"), journal.accountDigest);
    object.insert(
        QStringLiteral("generation"),
        QString::number(journal.generation));
    object.insert(
        QStringLiteral("transaction_id"), journal.transactionId);
    object.insert(
        QStringLiteral("operation"), QStringLiteral("removal"));
    object.insert(
        QStringLiteral("expected_refresh_token"),
        removal.expectedRefreshToken);
    object.insert(QStringLiteral("mode"), modeName(removal.mode));
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool parsePendingRemoval(
    const QString &stored,
    const JournalState &journal,
    RemovalIntent &removal,
    QString &error)
{
    if (stored.isEmpty()) {
        error = QStringLiteral(
            "The secure Strava removal package is not available");
        return false;
    }
    const QByteArray data = stored.toUtf8();
    if (data.size() > MaximumPendingBytes) {
        error = QStringLiteral(
            "The secure Strava removal package is too large");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "The secure Strava removal package is malformed");
        return false;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {
            QStringLiteral("version"),
            QStringLiteral("account_sha256"),
            QStringLiteral("generation"),
            QStringLiteral("transaction_id"),
            QStringLiteral("operation"),
            QStringLiteral("expected_refresh_token"),
            QStringLiteral("mode")
        })
        || !object.value(QStringLiteral("version")).isDouble()
        || object.value(QStringLiteral("version")).toInt(-1)
            != JournalVersion
        || !object.value(QStringLiteral("account_sha256")).isString()
        || !object.value(QStringLiteral("generation")).isString()
        || !object.value(QStringLiteral("transaction_id")).isString()
        || object.value(QStringLiteral("operation")).toString()
            != QStringLiteral("removal")
        || !object.value(
                QStringLiteral("expected_refresh_token")).isString()
        || !object.value(QStringLiteral("mode")).isString()) {
        error = QStringLiteral(
            "The secure Strava removal package schema is invalid");
        return false;
    }

    quint64 generation = 0;
    if (object.value(QStringLiteral("account_sha256")).toString()
            != journal.accountDigest
        || object.value(QStringLiteral("transaction_id")).toString()
            != journal.transactionId
        || !parseGeneration(
            object.value(QStringLiteral("generation")), generation)
        || generation != journal.generation) {
        error = QStringLiteral(
            "The secure Strava removal package is stale");
        return false;
    }

    removal.expectedRefreshToken = object.value(
        QStringLiteral("expected_refresh_token")).toString();
    if (!parseMode(
            object.value(QStringLiteral("mode")).toString(),
            removal.mode)
        || !removal.isValid()) {
        error = QStringLiteral(
            "The secure Strava removal package is invalid");
        return false;
    }
    return true;
}

QString serializePending(
    const JournalState &journal,
    const Publication &publication)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), JournalVersion);
    object.insert(
        QStringLiteral("account_sha256"), journal.accountDigest);
    object.insert(
        QStringLiteral("generation"),
        QString::number(journal.generation));
    object.insert(
        QStringLiteral("transaction_id"), journal.transactionId);
    object.insert(
        QStringLiteral("expected_refresh_token"),
        publication.expectedRefreshToken);
    object.insert(
        QStringLiteral("access_token"),
        publication.replacement.accessToken);
    object.insert(
        QStringLiteral("refresh_token"),
        publication.replacement.refreshToken);
    object.insert(
        QStringLiteral("refreshed_at"), publication.refreshedAt);
    object.insert(
        QStringLiteral("mode"), modeName(publication.mode));
    object.insert(
        QStringLiteral("activates_authorization"),
        publication.activatesAuthorization);
    object.insert(
        QStringLiteral("clears_remote_grant_uncertainty"),
        publication.clearsRemoteGrantUncertainty);
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool parsePending(
    const QString &stored,
    const JournalState &journal,
    Publication &publication,
    QString &error)
{
    if (stored.isEmpty()) {
        error = QStringLiteral(
            "The secure Strava publication package is not available");
        return false;
    }
    const QByteArray data = stored.toUtf8();
    if (data.size() > MaximumPendingBytes) {
        error = QStringLiteral(
            "The secure Strava publication package is too large");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        error = QStringLiteral(
            "The secure Strava publication package is malformed");
        return false;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {
            QStringLiteral("version"),
            QStringLiteral("account_sha256"),
            QStringLiteral("generation"),
            QStringLiteral("transaction_id"),
            QStringLiteral("expected_refresh_token"),
            QStringLiteral("access_token"),
            QStringLiteral("refresh_token"),
            QStringLiteral("refreshed_at"),
            QStringLiteral("mode"),
            QStringLiteral("activates_authorization"),
            QStringLiteral("clears_remote_grant_uncertainty")
        })
        || !object.value(QStringLiteral("version")).isDouble()
        || (object.value(QStringLiteral("version")).toInt(-1)
                != LegacyJournalVersion
            && object.value(QStringLiteral("version")).toInt(-1)
                != JournalVersion)
        || !object.value(QStringLiteral("account_sha256")).isString()
        || !object.value(QStringLiteral("generation")).isString()
        || !object.value(QStringLiteral("transaction_id")).isString()
        || !object.value(QStringLiteral("expected_refresh_token")).isString()
        || !object.value(QStringLiteral("access_token")).isString()
        || !object.value(QStringLiteral("refresh_token")).isString()
        || !object.value(QStringLiteral("refreshed_at")).isString()
        || !object.value(QStringLiteral("mode")).isString()
        || !object.value(QStringLiteral("activates_authorization")).isBool()
        || !object.value(QStringLiteral("clears_remote_grant_uncertainty")).isBool()) {
        error = QStringLiteral(
            "The secure Strava publication package schema is invalid");
        return false;
    }

    quint64 generation = 0;
    const QString packageAccount =
        object.value(QStringLiteral("account_sha256")).toString();
    const QString transactionId =
        object.value(QStringLiteral("transaction_id")).toString();
    if (packageAccount != journal.accountDigest
        || transactionId != journal.transactionId
        || !parseGeneration(
            object.value(QStringLiteral("generation")), generation)
        || generation != journal.generation) {
        error = QStringLiteral(
            "The secure Strava publication package is stale");
        return false;
    }

    publication.expectedRefreshToken = object.value(
        QStringLiteral("expected_refresh_token")).toString();
    publication.replacement.accessToken = object.value(
        QStringLiteral("access_token")).toString();
    publication.replacement.refreshToken = object.value(
        QStringLiteral("refresh_token")).toString();
    publication.refreshedAt = object.value(
        QStringLiteral("refreshed_at")).toString();
    publication.activatesAuthorization = object.value(
        QStringLiteral("activates_authorization")).toBool();
    publication.clearsRemoteGrantUncertainty = object.value(
        QStringLiteral("clears_remote_grant_uncertainty")).toBool();
    if (!parseMode(
            object.value(QStringLiteral("mode")).toString(),
            publication.mode)
        || !publication.isValid()) {
        error = QStringLiteral(
            "The secure Strava publication package is invalid");
        return false;
    }
    return true;
}

StravaTokenPublication::PublicationResult pendingPublication(
    const QString &error = QString())
{
    return {
        StravaTokenPublication::PublicationStatus::Pending,
        error.isEmpty()
            ? QStringLiteral(
                  "Strava credential publication is pending recovery.")
            : error
    };
}

StravaTokenPublication::PublicationResult storageFailure(
    const QString &error)
{
    return {
        StravaTokenPublication::PublicationStatus::StorageFailure,
        error.isEmpty()
            ? QStringLiteral(
                  "Strava credentials could not be stored securely.")
            : error
    };
}

RecoveryResult pendingRecovery(
    const JournalState &journal,
    const QString &error)
{
    return {
        RecoveryStatus::Pending,
        error.isEmpty()
            ? QStringLiteral(
                  "A Strava credential transaction is pending recovery.")
            : error,
        journal.generation
    };
}

} // namespace

namespace Detail {

struct CoordinatorState
{
    QString accountKey;
    QString transactionParent;
    QString accountDigest;
    StorageCallbacks storage;
};

struct MutationState
{
    std::shared_ptr<CoordinatorState> coordinator;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> lock;
    JournalState journal;
    Publication armedPublication;
    std::mutex operationMutex;
    std::mutex mutex;
    std::atomic<bool> commitStarted{false};
    std::atomic<bool> finished{false};
    bool armed = false;
};

} // namespace Detail

namespace {

bool openNamespace(
    const Detail::CoordinatorState &state,
    AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error)
{
    const QFileInfo parentInfo(state.transactionParent);
    const QString parentCanonical =
        QDir::cleanPath(parentInfo.canonicalFilePath());
    const QString requestedParent = QDir::cleanPath(
        parentInfo.absoluteFilePath());
    if (state.transactionParent.isEmpty()
        || !parentInfo.exists() || !parentInfo.isDir()
        || parentInfo.isSymLink() || parentCanonical.isEmpty()
        || atomicFilePathKey(parentCanonical)
            != atomicFilePathKey(requestedParent)) {
        error = QStringLiteral(
            "The Strava credential transaction parent is unsafe");
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor parent;
    if (!AnchoredFileSystem::DirectoryAnchor::open(
            parentCanonical, parent, error)
        || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
            parent, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The Strava credential transaction parent is not controlled by the current user");
        }
        return false;
    }

    bool exists = false;
    if (!parent.openChildIfExists(
            NamespaceName, directory, exists, error)) {
        return false;
    }
    if (!exists) {
        const AnchoredFileSystem::MutationResult created =
            AnchoredFileSystem::createPrivateFixedChildDirectory(
                parent, NamespaceName, directory);
        if (created.effect
                != AnchoredFileSystem::MutationEffect::AppliedDurable
            && created.effect
                != AnchoredFileSystem::MutationEffect::Conflict) {
            error = created.error.isEmpty()
                ? QStringLiteral(
                      "Cannot create the Strava credential transaction directory")
                : created.error;
            return false;
        }
        if (!directory.isValid()
            && !parent.openChild(
                NamespaceName, directory, error)) {
            return false;
        }
    }
    return AnchoredFileSystem::hardenPrivateDirectory(
            directory, error)
        && directory.pathMatches(error);
}

bool acquireAccountLock(
    const AnchoredFileSystem::DirectoryAnchor &directory,
    int timeoutMs,
    std::unique_ptr<AnchoredFileSystem::PinnedFile> &lock,
    QString &error)
{
    lock.reset();
    reportCredentialTransition("lock-before-acquire");
    if (!directory.pathMatches(error)) return false;

    const AnchoredFileSystem::EntryRef entry =
        directory.entry(LockFileName, error);
    bool exists = false;
    if (!entry.isValid()
        || !AnchoredFileSystem::entryExists(entry, exists, error)) {
        return false;
    }

    AnchoredFileSystem::PinnedFile pinned;
    if (exists) {
        if (!AnchoredFileSystem::pinRegularFile(
                entry, pinned, error, MaximumLockBytes)) {
            return false;
        }
    } else if (!AnchoredFileSystem::writeNewFile(
                   QByteArray(), entry, pinned, error)) {
        // A cooperating process may have won the fixed-name creation race.
        error.clear();
        if (!AnchoredFileSystem::pinRegularFile(
                entry, pinned, error, MaximumLockBytes)) {
            return false;
        }
    }
    if (!AnchoredFileSystem::tryLockExclusive(
            pinned, timeoutMs, error)) {
        return false;
    }
    lock = std::make_unique<AnchoredFileSystem::PinnedFile>(
        std::move(pinned));
    return true;
}

bool loadJournal(
    const Detail::CoordinatorState &state,
    const AnchoredFileSystem::DirectoryAnchor &directory,
    JournalState &journal,
    QString &error)
{
    const AnchoredFileSystem::EntryRef entry =
        directory.entry(StateFileName, error);
    if (!entry.isValid()) return false;
    bool exists = false;
    if (!AnchoredFileSystem::entryExists(entry, exists, error))
        return false;
    if (!exists) {
        journal = {};
        journal.accountDigest = state.accountDigest;
        return true;
    }

    AnchoredFileSystem::PinnedFile file;
    QByteArray contents;
    return AnchoredFileSystem::pinRegularFile(
            entry, file, error, MaximumJournalBytes)
        && AnchoredFileSystem::readAll(
            file, MaximumJournalBytes, contents, error)
        && parseJournal(
            contents, state.accountDigest, journal, error);
}

bool saveJournal(
    const Detail::CoordinatorState &state,
    const AnchoredFileSystem::DirectoryAnchor &directory,
    const JournalState &journal,
    QString &error)
{
    reportCredentialTransition("journal-before-write");
    if (!directory.pathMatches(error)) return false;
    const QByteArray contents = serializeJournal(journal);
    if (contents.isEmpty() || contents.size() > MaximumJournalBytes) {
        error = QStringLiteral(
            "The Strava credential journal is invalid");
        return false;
    }

    const AnchoredFileSystem::EntryRef entry =
        directory.entry(StateFileName, error);
    bool exists = false;
    if (!entry.isValid()
        || !AnchoredFileSystem::entryExists(entry, exists, error)) {
        return false;
    }
    if (!exists) {
        AnchoredFileSystem::PinnedFile journal;
        if (!AnchoredFileSystem::writeNewFile(
                contents, entry, journal, error)) {
            return false;
        }
    } else {
        AnchoredFileSystem::PinnedFile expected;
        if (!AnchoredFileSystem::pinRegularFile(
                entry, expected, error, MaximumJournalBytes)) {
            return false;
        }
        const QString stagingName = QStringLiteral(".state-%1.tmp")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const AnchoredFileSystem::EntryRef stagingEntry =
            directory.entry(stagingName, error);
        AnchoredFileSystem::PinnedFile staging;
        if (!stagingEntry.isValid()
            || !AnchoredFileSystem::writeNewFile(
                contents, stagingEntry, staging, error)) {
            return false;
        }
        const AnchoredFileSystem::MutationResult replaced =
            AnchoredFileSystem::replaceExisting(staging, expected);
        if (replaced.effect
                != AnchoredFileSystem::MutationEffect::AppliedDurable) {
            error = replaced.error.isEmpty()
                ? QStringLiteral(
                      "Cannot durably replace the Strava credential journal")
                : replaced.error;
            if (staging.isValid()
                && (replaced.effect
                        == AnchoredFileSystem::MutationEffect::NoEffect
                    || replaced.effect
                        == AnchoredFileSystem::MutationEffect::Conflict)) {
                AnchoredFileSystem::remove(staging);
            }
            return false;
        }
        const AnchoredFileSystem::MutationResult removed =
            AnchoredFileSystem::remove(expected);
        if (removed.effect
                != AnchoredFileSystem::MutationEffect::AppliedDurable) {
            error = removed.error.isEmpty()
                ? QStringLiteral(
                      "Cannot durably retire the previous Strava credential journal")
                : removed.error;
            return false;
        }
    }

    JournalState verified;
    if (!loadJournal(state, directory, verified, error)
        || verified.accountDigest != journal.accountDigest
        || verified.generation != journal.generation
        || verified.phase != journal.phase
        || verified.transactionId != journal.transactionId
        || verified.previousAuthorizationState
            != journal.previousAuthorizationState
        || verified.previousRemoteGrantUncertain
            != journal.previousRemoteGrantUncertain
        || (verified.phase != Phase::Idle
            && verified.kind != journal.kind)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The durable Strava credential journal verification failed");
        }
        return false;
    }
    return true;
}

bool markAuthorizationPending(
    const StorageCallbacks &storage,
    MutationKind kind,
    QString &error)
{
    const QString state = kind == MutationKind::Revocation
        ? QStringLiteral("revocation_pending")
        : QStringLiteral("authorization_pending");
    bool saved = false;
    try {
        saved = storage.writeAuthorizationState(state)
            && storage.writeRemoteGrantUncertain(true)
            && storage.touchAuthorizationRevision()
            && storage.sync();
    } catch (...) {
        saved = false;
    }
    if (!saved) {
        error = QStringLiteral(
            "The pending Strava authorization state could not be stored");
    }
    return saved;
}

bool restoreAuthorizationState(
    const StorageCallbacks &storage,
    const JournalState &journal,
    QString &error)
{
    bool saved = false;
    try {
        saved = !journal.previousAuthorizationState.isEmpty()
            && storage.writeAuthorizationState(
                journal.previousAuthorizationState)
            && storage.writeRemoteGrantUncertain(
                journal.previousRemoteGrantUncertain)
            && storage.touchAuthorizationRevision()
            && storage.sync();
    } catch (...) {
        saved = false;
    }
    if (!saved) {
        error = QStringLiteral(
            "The previous Strava authorization state could not be restored");
    }
    return saved;
}

bool refreshStorageMetadata(
    const StorageCallbacks &storage,
    QString &error)
{
    bool refreshed = false;
    try {
        refreshed = storage.refresh();
    } catch (...) {
        refreshed = false;
    }
    if (!refreshed) {
        error = QStringLiteral(
            "The Strava credential metadata could not be refreshed");
    }
    return refreshed;
}

bool readCoherentStoredState(
    const StorageCallbacks &storage,
    StoredState &stored,
    QString &error)
{
    stored = {};
    try {
        const QString revisionBefore =
            storage.readAuthorizationRevision();
        const TokenPairReadResult current = storage.readCurrent();
        if (!current.readable) {
            error = QStringLiteral(
                "The Strava credential state could not be read securely");
            return false;
        }
        stored.credentials = current.value;
        stored.refreshedAt = storage.readTimestamp();
        stored.authorizationState = storage.readAuthorizationState();
        stored.remoteGrantUncertain = storage.readRemoteGrantUncertain();
        stored.authorizationRevision =
            storage.readAuthorizationRevision();
        if (revisionBefore != stored.authorizationRevision) {
            stored = {};
            error = QStringLiteral(
                "The Strava credential state changed while it was read");
            return false;
        }
        stored.readable = true;
        return true;
    } catch (...) {
        stored = {};
        error = QStringLiteral(
            "The Strava credential state could not be read securely");
        return false;
    }
}

StravaTokenPublication::PublicationResult commitPublication(
    const std::shared_ptr<Detail::CoordinatorState> &coordinator,
    JournalState &journal,
    const Publication &publication,
    const AnchoredFileSystem::DirectoryAnchor &directory,
    QString &error,
    bool recovery = false)
{
    if (!publication.isValid()) {
        return {
            StravaTokenPublication::PublicationStatus::InvalidInput,
            QStringLiteral(
                "Strava credential publication inputs are invalid.")
        };
    }

    journal.phase = Phase::PublicationPending;
    if (!saveJournal(
            *coordinator, directory, journal, error)) {
        return pendingPublication(error);
    }
    if (!markAuthorizationPending(
            coordinator->storage, journal.kind, error)) {
        return pendingPublication(error);
    }

    TokenPairReadResult current;
    try {
        current = coordinator->storage.readCurrent();
    } catch (...) {
        current = {};
    }
    if (!current.readable) {
        return storageFailure(
            recovery
                ? QStringLiteral(
                    "The Strava credentials could not be read during recovery")
                : QStringLiteral(
                    "The Strava credentials could not be read securely"));
    }
    StravaTokenPublication::PublicationCallbacks callbacks;
    callbacks.readCurrent = [current] { return current.value; };
    callbacks.writeRefreshToken =
        coordinator->storage.writeRefreshToken;
    callbacks.writeAccessToken =
        coordinator->storage.writeAccessToken;
    callbacks.writeTimestamp =
        coordinator->storage.writeTimestamp;
    StravaTokenPublication::PublicationResult result =
        StravaTokenPublication::publish(
            publication.expectedRefreshToken,
            publication.replacement,
            publication.refreshedAt,
            publication.mode,
            callbacks);
    if (!result.isSuccess()) {
        if (result.status
            == StravaTokenPublication::PublicationStatus::Conflict) {
            return result;
        }
        return pendingPublication(result.error);
    }

    bool synchronized = false;
    try {
        const bool uncertaintySaved =
            !publication.clearsRemoteGrantUncertainty
            || coordinator->storage
                .writeRemoteGrantUncertain(false);
        const bool stateSaved =
            !publication.activatesAuthorization
            || coordinator->storage.writeAuthorizationState(
                QStringLiteral("active"));
        synchronized = uncertaintySaved
            && stateSaved
            && coordinator->storage.touchAuthorizationRevision()
            && coordinator->storage.sync();
    } catch (...) {
        synchronized = false;
    }
    if (!synchronized) {
        QString pendingError;
        markAuthorizationPending(
            coordinator->storage, journal.kind, pendingError);
        return pendingPublication(QStringLiteral(
            "The complete Strava authorization state could not be synchronized"));
    }

    JournalState idle = journal;
    idle.phase = Phase::Idle;
    idle.transactionId.clear();
    idle.previousAuthorizationState.clear();
    idle.previousRemoteGrantUncertain = false;
    if (!saveJournal(
            *coordinator, directory, idle, error)) {
        QString pendingError;
        markAuthorizationPending(
            coordinator->storage, journal.kind, pendingError);
        return pendingPublication(error);
    }
    journal = idle;

    // The publication decision is already idle and durable. A stale secure
    // package is harmless and will be overwritten by the next fenced mutation.
    try {
        if (coordinator->storage.clearPendingTransaction())
            coordinator->storage.sync();
    } catch (...) {
    }
    return result;
}

RecoveryResult recoverLocked(
    const std::shared_ptr<Detail::CoordinatorState> &coordinator,
    JournalState &journal,
    const AnchoredFileSystem::DirectoryAnchor &directory)
{
    if (journal.phase == Phase::Idle) {
        return {
            RecoveryStatus::NoWork,
            QString(),
            journal.generation
        };
    }

    QString error;
    if (journal.phase == Phase::RemotePending) {
        // No provider call can start until the journal advances to
        // CommitUnknown, so the exact pre-mutation state is authoritative.
        const bool restoredExactly =
            journal.previousAuthorizationStateKnown;
        const bool stateRestored = restoredExactly
            ? restoreAuthorizationState(
                coordinator->storage, journal, error)
            : markAuthorizationPending(
                coordinator->storage, journal.kind, error);
        if (!stateRestored) {
            return pendingRecovery(journal, error);
        }
        JournalState idle = journal;
        idle.phase = Phase::Idle;
        idle.transactionId.clear();
        idle.previousAuthorizationState.clear();
        idle.previousRemoteGrantUncertain = false;
        if (!saveJournal(*coordinator, directory, idle, error)) {
            return pendingRecovery(journal, error);
        }
        try {
            if (coordinator->storage.clearPendingTransaction())
                coordinator->storage.sync();
        } catch (...) {
        }
        journal = idle;
        return restoredExactly
            ? RecoveryResult{
                RecoveryStatus::Recovered,
                QString(),
                idle.generation
            }
            : RecoveryResult{
                RecoveryStatus::Pending,
                QStringLiteral(
                    "A legacy Strava credential transaction was retired fail-closed"),
                idle.generation
            };
    }

    if (!markAuthorizationPending(
            coordinator->storage, journal.kind, error)) {
        return {
            RecoveryStatus::StorageFailure,
            error,
            journal.generation
        };
    }
    const auto retireJournal =
        [&](RecoveryStatus status,
            QString message) -> RecoveryResult {
            JournalState idle = journal;
            idle.phase = Phase::Idle;
            idle.transactionId.clear();
            idle.previousAuthorizationState.clear();
            idle.previousRemoteGrantUncertain = false;
            error.clear();
            if (!saveJournal(
                    *coordinator, directory, idle, error)) {
                return pendingRecovery(journal, error);
            }
            try {
                if (coordinator->storage.clearPendingTransaction())
                    coordinator->storage.sync();
            } catch (...) {
            }
            journal = idle;
            return {status, message, idle.generation};
    };
    if (journal.kind == MutationKind::Revocation) {
        TokenPairReadResult current;
        if (journal.phase == Phase::LocalCommitPending) {
            PendingTransactionReadResult storedRemoval;
            try {
                storedRemoval = coordinator->storage
                    .readPendingTransactionForRecovery();
            } catch (...) {
                storedRemoval = {};
            }
            if (!storedRemoval.readable) {
                return {
                    RecoveryStatus::StorageFailure,
                    QStringLiteral(
                        "The secure Strava removal package could not be read"),
                    journal.generation
                };
            }
            RemovalIntent removal;
            if (!parsePendingRemoval(
                    storedRemoval.value,
                    journal,
                    removal,
                    error)) {
                return retireJournal(
                    RecoveryStatus::Pending, error);
            }
            StravaTokenPublication::PublicationCallbacks callbacks;
            callbacks.readCurrent = [coordinator] {
                const TokenPairReadResult observed =
                    coordinator->storage.readCurrent();
                if (!observed.readable)
                    throw std::runtime_error("credential read unavailable");
                return observed.value;
            };
            callbacks.writeRefreshToken =
                coordinator->storage.writeRefreshToken;
            callbacks.writeAccessToken =
                coordinator->storage.writeAccessToken;
            callbacks.writeTimestamp =
                coordinator->storage.writeTimestamp;
            const StravaTokenPublication::RemovalResult removed =
                StravaTokenPublication::remove(
                    removal.expectedRefreshToken,
                    removal.mode,
                    callbacks);
            if (removed.status
                == StravaTokenPublication::RemovalStatus::Conflict) {
                return retireJournal(
                    RecoveryStatus::Recovered, QString());
            }
            if (!removed.isSuccess()) {
                return {
                    RecoveryStatus::StorageFailure,
                    removed.error,
                    journal.generation
                };
            }
            current = {true, {}};
        } else {
            try {
                current = coordinator->storage.readCurrent();
            } catch (...) {
                current = {};
            }
        }
        if (!current.readable) {
            return {
                RecoveryStatus::StorageFailure,
                QStringLiteral(
                    "The Strava credentials could not be read during removal recovery"),
                journal.generation
            };
        }
        if (current.value.accessToken.isEmpty()
            && current.value.refreshToken.isEmpty()) {
            bool cleanupComplete = false;
            try {
                cleanupComplete = coordinator->storage
                    .completeRevocationCleanup();
            } catch (...) {
                cleanupComplete = false;
            }
            if (!cleanupComplete) {
                return pendingRecovery(
                    journal,
                    QStringLiteral(
                        "Strava credentials were removed, but local account cleanup is still pending"));
            }
            return retireJournal(
                RecoveryStatus::Recovered, QString());
        }
        // The pending authorization state is now the durable fence. Retire
        // this unknown generation so an explicit cleanup can supersede it.
        return retireJournal(
            RecoveryStatus::Pending,
            QStringLiteral(
                "A prior Strava revocation has an unknown outcome"));
    }

    PendingTransactionReadResult stored;
    try {
        stored = coordinator->storage
            .readPendingTransactionForRecovery();
    } catch (...) {
        stored = {};
    }
    if (!stored.readable) {
        return {
            RecoveryStatus::StorageFailure,
            QStringLiteral(
                "The secure Strava publication package could not be read"),
            journal.generation
        };
    }
    Publication publication;
    if (!parsePending(stored.value, journal, publication, error)) {
        return retireJournal(
            RecoveryStatus::Pending, error);
    }

    const StravaTokenPublication::PublicationResult published =
        commitPublication(
            coordinator, journal, publication,
            directory, error, true);
    if (published.isSuccess()) {
        return {
            RecoveryStatus::Recovered,
            QString(),
            journal.generation
        };
    }
    if (published.status
        == StravaTokenPublication::PublicationStatus::Conflict) {
        return retireJournal(
            RecoveryStatus::Recovered, QString());
    }
    if (published.status
        == StravaTokenPublication::PublicationStatus::StorageFailure) {
        return {
            RecoveryStatus::StorageFailure,
            published.error,
            journal.generation
        };
    }
    return pendingRecovery(journal, published.error);
}

bool journalMatchesMutation(
    const Detail::MutationState &mutation,
    const JournalState &journal,
    QString &error)
{
    if (mutation.finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is already complete");
        return false;
    }
    if (journal.accountDigest
            != mutation.journal.accountDigest
        || journal.generation
            != mutation.journal.generation
        || journal.transactionId
            != mutation.journal.transactionId
        || journal.kind != mutation.journal.kind
        || journal.phase == Phase::Idle) {
        error = QStringLiteral(
            "The Strava credential mutation was superseded");
        return false;
    }
    return true;
}

void releaseMutation(Detail::MutationState &mutation)
{
    std::lock_guard<std::mutex> lock(mutation.mutex);
    mutation.finished.store(true);
    mutation.lock.reset();
}

} // namespace

bool Publication::isValid() const
{
    const bool supportedMode = mode
            == StravaTokenPublication::PublicationMode::Authoritative
        || mode
            == StravaTokenPublication::PublicationMode::CompareAndSwap;
    return replacement.isValid()
        && replacement.accessToken.size()
            <= MaximumTokenCharacters
        && replacement.refreshToken.size()
            <= MaximumTokenCharacters
        && expectedRefreshToken.size()
            <= MaximumTokenCharacters
        && !refreshedAt.isEmpty()
        && refreshedAt.size() <= MaximumTimestampCharacters
        && supportedMode
        && (mode
                == StravaTokenPublication::PublicationMode::Authoritative
            || !expectedRefreshToken.isEmpty());
}

bool StorageCallbacks::isValid() const
{
    return readCurrent
        && readTimestamp
        && readAuthorizationState
        && readRemoteGrantUncertain
        && readPendingTransaction
        && writePendingTransaction
        && clearPendingTransaction
        && writeRefreshToken
        && writeAccessToken
        && writeTimestamp
        && writeAuthorizationState
        && writeRemoteGrantUncertain
        && sync
        && refresh
        && completeRevocationCleanup
        && readAuthorizationRevision
        && readPendingTransactionForRecovery;
}

Mutation::Mutation(
    std::shared_ptr<Detail::MutationState> state)
    : state_(std::move(state))
{
}

Mutation::~Mutation() = default;

quint64 Mutation::generation() const
{
    if (!state_) return 0;
    const std::lock_guard<std::mutex> lock(
        state_->operationMutex);
    return state_->journal.generation;
}

QString Mutation::transactionId() const
{
    if (!state_) return {};
    const std::lock_guard<std::mutex> lock(
        state_->operationMutex);
    return state_->journal.transactionId;
}

bool Mutation::isFinished() const
{
    return !state_ || state_->finished.load();
}

bool Mutation::isCurrent(QString &error) const
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (!state_->coordinator
        || !state_->lock || state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    return openNamespace(*state_->coordinator, directory, error)
        && loadJournal(
            *state_->coordinator, directory, current, error)
        && journalMatchesMutation(*state_, current, error);
}

bool Mutation::readStoredState(
    StoredState &stored,
    QString &error) const
{
    stored = {};
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (!state_->coordinator || !state_->lock
        || state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    return readCoherentStoredState(
        state_->coordinator->storage, stored, error);
}

bool Mutation::markPendingState(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (!state_->coordinator || !state_->lock
        || state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    return markAuthorizationPending(
        state_->coordinator->storage, current.kind, error);
}

bool Mutation::armPublication(
    const Publication &publication,
    QString &error)
{
    error.clear();
    if (!state_ || !publication.isValid()) {
        error = QStringLiteral(
            "The Strava credential publication is invalid");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->armed || state_->finished.load()) {
            error = QStringLiteral(
                "The Strava credential publication is already armed");
            return false;
        }
    }

    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }

    current.phase = Phase::CommitUnknown;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;

    bool pendingSaved = false;
    try {
        pendingSaved = state_->coordinator->storage
            .writePendingTransaction(
                serializePending(current, publication))
            && state_->coordinator->storage.sync();
    } catch (...) {
        pendingSaved = false;
    }
    if (!pendingSaved) {
        QString pendingError;
        markAuthorizationPending(
            state_->coordinator->storage,
            current.kind,
            pendingError);
        error = QStringLiteral(
            "The secure Strava publication package could not be stored");
        return false;
    }

    current.phase = Phase::PublicationPending;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        QString pendingError;
        markAuthorizationPending(
            state_->coordinator->storage,
            current.kind,
            pendingError);
        return false;
    }
    state_->journal = current;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->armedPublication = publication;
        state_->armed = true;
    }
    return true;
}

StravaTokenPublication::PublicationResult
Mutation::commitArmedPublication()
{
    if (!state_) {
        return storageFailure(QStringLiteral(
            "The Strava credential mutation is unavailable"));
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (state_->finished.load()) {
        return storageFailure(QStringLiteral(
            "The Strava credential mutation is unavailable"));
    }
    if (state_->commitStarted.exchange(true)) {
        return pendingPublication(QStringLiteral(
            "The Strava credential publication is already running"));
    }

    Publication publication;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->armed)
            publication = state_->armedPublication;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    QString error;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return pendingPublication(error);
    }
    if (!publication.isValid()) {
        QString stored;
        try {
            stored = state_->coordinator->storage
                .readPendingTransaction();
        } catch (...) {
        }
        if (!parsePending(stored, current, publication, error))
            return pendingPublication(error);
    }

    const StravaTokenPublication::PublicationResult result =
        commitPublication(
            state_->coordinator, current, publication,
            directory, error);
    state_->journal = current;
    if (result.isSuccess())
        releaseMutation(*state_);
    return result;
}

StravaTokenPublication::PublicationResult Mutation::publish(
    const Publication &publication)
{
    QString error;
    if (!armPublication(publication, error))
        return pendingPublication(error);
    return commitArmedPublication();
}

bool Mutation::markCommitUnknown(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    current.phase = Phase::CommitUnknown;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    return true;
}

bool Mutation::markLocalCommitStarted(
    const QString &expectedRefreshToken,
    StravaTokenPublication::PublicationMode mode,
    QString &error)
{
    error.clear();
    const RemovalIntent removal{
        mode == StravaTokenPublication::PublicationMode::Authoritative
            ? QString() : expectedRefreshToken,
        mode
    };
    if (!state_ || !removal.isValid()) {
        error = QStringLiteral(
            "The Strava credential removal intent is invalid");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (!state_->coordinator || !state_->lock
        || state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    if (current.kind != MutationKind::Revocation
        || (current.phase != Phase::RemotePending
            && current.phase != Phase::CommitUnknown
            && current.phase != Phase::LocalCommitPending)) {
        error = QStringLiteral(
            "The Strava credential mutation cannot start local removal");
        return false;
    }
    if (current.phase == Phase::LocalCommitPending) {
        QString stored;
        try {
            stored = state_->coordinator->storage
                .readPendingTransaction();
        } catch (...) {
        }
        RemovalIntent existing;
        if (!parsePendingRemoval(
                stored, current, existing, error)) {
            return false;
        }
        if (existing.expectedRefreshToken
                != removal.expectedRefreshToken
            || existing.mode != removal.mode) {
            error = QStringLiteral(
                "The Strava credential removal intent changed");
            return false;
        }
        return true;
    }

    bool pendingSaved = false;
    try {
        pendingSaved = state_->coordinator->storage
            .writePendingTransaction(
                serializePendingRemoval(current, removal))
            && state_->coordinator->storage.sync();
    } catch (...) {
        pendingSaved = false;
    }
    if (!pendingSaved) {
        error = QStringLiteral(
            "The secure Strava removal package could not be stored");
        return false;
    }

    current.phase = Phase::LocalCommitPending;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    return true;
}

bool Mutation::abortBeforeRemoteDispatch(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (!state_->coordinator || !state_->lock
        || state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->armed || state_->commitStarted.load()) {
            error = QStringLiteral(
                "A started Strava credential commit cannot be discarded");
            return false;
        }
    }

    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    if (current.phase != Phase::CommitUnknown) {
        error = QStringLiteral(
            "The Strava credential commit is not awaiting dispatch");
        return false;
    }

    // Once this durable transition succeeds, restart recovery again knows
    // that no provider request was dispatched and can restore exactly.
    current.phase = Phase::RemotePending;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    if (!restoreAuthorizationState(
            state_->coordinator->storage, current, error)) {
        return false;
    }

    current.phase = Phase::Idle;
    current.transactionId.clear();
    current.previousAuthorizationState.clear();
    current.previousRemoteGrantUncertain = false;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    try {
        if (state_->coordinator->storage.clearPendingTransaction())
            state_->coordinator->storage.sync();
    } catch (...) {
    }
    releaseMutation(*state_);
    return true;
}

bool Mutation::finishNoChange(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    if (current.phase != Phase::RemotePending) {
        error = QStringLiteral(
            "A started Strava credential commit cannot be discarded");
        return false;
    }
    if (!restoreAuthorizationState(
            state_->coordinator->storage, current, error)) {
        return false;
    }
    current.phase = Phase::Idle;
    current.transactionId.clear();
    current.previousAuthorizationState.clear();
    current.previousRemoteGrantUncertain = false;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    try {
        if (state_->coordinator->storage.clearPendingTransaction())
            state_->coordinator->storage.sync();
    } catch (...) {
    }
    releaseMutation(*state_);
    return true;
}

bool Mutation::finishCommit(QString &error)
{
    error.clear();
    if (!state_) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    const std::lock_guard<std::mutex> operationLock(
        state_->operationMutex);
    if (state_->finished.load()) {
        error = QStringLiteral(
            "The Strava credential mutation is unavailable");
        return false;
    }
    AnchoredFileSystem::DirectoryAnchor directory;
    JournalState current;
    if (!openNamespace(*state_->coordinator, directory, error)
        || !loadJournal(
            *state_->coordinator, directory, current, error)
        || !journalMatchesMutation(*state_, current, error)) {
        return false;
    }
    if (current.phase != Phase::CommitUnknown
        && current.phase != Phase::LocalCommitPending) {
        error = QStringLiteral(
            "The Strava credential commit is not awaiting completion");
        return false;
    }
    current.phase = Phase::Idle;
    current.transactionId.clear();
    current.previousAuthorizationState.clear();
    current.previousRemoteGrantUncertain = false;
    if (!saveJournal(
            *state_->coordinator, directory, current, error)) {
        return false;
    }
    state_->journal = current;
    try {
        if (state_->coordinator->storage.clearPendingTransaction())
            state_->coordinator->storage.sync();
    } catch (...) {
    }
    releaseMutation(*state_);
    return true;
}

Coordinator::Coordinator(
    QString accountKey,
    QString transactionParent,
    StorageCallbacks storage)
    : state_(std::make_shared<Detail::CoordinatorState>())
{
    state_->accountKey = std::move(accountKey);
    state_->transactionParent = std::move(transactionParent);
    state_->accountDigest = accountDigest(state_->accountKey);
    state_->storage = std::move(storage);
}

std::shared_ptr<Mutation> Coordinator::begin(
    MutationKind kind,
    int timeoutMs,
    QString &error,
    bool recoverPending)
{
    error.clear();
    if (!state_ || state_->accountKey.trimmed().isEmpty()
        || state_->accountKey.size() > 512
        || !state_->storage.isValid() || timeoutMs <= 0) {
        error = QStringLiteral(
            "The Strava credential mutation inputs are invalid");
        return {};
    }

    AnchoredFileSystem::DirectoryAnchor directory;
    if (!openNamespace(*state_, directory, error)) return {};
    std::unique_ptr<AnchoredFileSystem::PinnedFile> lock;
    if (!acquireAccountLock(
            directory, timeoutMs, lock, error)) {
        error = QStringLiteral(
            "Another process is changing this Strava authorization");
        return {};
    }
    if (!refreshStorageMetadata(state_->storage, error))
        return {};

    JournalState journal;
    if (!loadJournal(*state_, directory, journal, error))
        return {};
    if (journal.phase != Phase::Idle) {
        if (!recoverPending) {
            error = QStringLiteral(
                "A prior Strava credential transaction requires recovery");
            return {};
        }
        const RecoveryResult recovered = recoverLocked(
            state_, journal, directory);
        if (!recovered.isSuccess()) {
            error = recovered.error;
            return {};
        }
    }
    if (journal.generation
        == std::numeric_limits<quint64>::max()) {
        error = QStringLiteral(
            "The Strava credential generation is exhausted");
        return {};
    }

    try {
        journal.previousAuthorizationState =
            state_->storage.readAuthorizationState();
        journal.previousRemoteGrantUncertain =
            state_->storage.readRemoteGrantUncertain();
    } catch (...) {
        error = QStringLiteral(
            "The current Strava authorization state could not be read");
        return {};
    }
    if (!validAuthorizationState(
            journal.previousAuthorizationState)) {
        error = QStringLiteral(
            "The current Strava authorization state is invalid");
        return {};
    }

    ++journal.generation;
    journal.phase = Phase::RemotePending;
    journal.transactionId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    journal.kind = kind;
    if (!saveJournal(*state_, directory, journal, error))
        return {};

    auto mutationState =
        std::make_shared<Detail::MutationState>();
    mutationState->coordinator = state_;
    mutationState->lock = std::move(lock);
    mutationState->journal = journal;
    return std::shared_ptr<Mutation>(
        new Mutation(std::move(mutationState)));
}

RecoveryResult Coordinator::recover(int timeoutMs)
{
    if (!state_ || state_->accountKey.trimmed().isEmpty()
        || state_->accountKey.size() > 512
        || !state_->storage.isValid() || timeoutMs <= 0) {
        return {
            RecoveryStatus::Invalid,
            QStringLiteral(
                "The Strava credential recovery inputs are invalid"),
            0
        };
    }
    QString error;
    AnchoredFileSystem::DirectoryAnchor directory;
    if (!openNamespace(*state_, directory, error)) {
        return {RecoveryStatus::StorageFailure, error, 0};
    }
    std::unique_ptr<AnchoredFileSystem::PinnedFile> lock;
    if (!acquireAccountLock(
            directory, timeoutMs, lock, error)) {
        return {
            RecoveryStatus::Pending,
            QStringLiteral(
                "Another process is completing the Strava credential transaction"),
            0
        };
    }
    if (!refreshStorageMetadata(state_->storage, error)) {
        return {RecoveryStatus::StorageFailure, error, 0};
    }
    JournalState journal;
    if (!loadJournal(*state_, directory, journal, error)) {
        return {RecoveryStatus::StorageFailure, error, 0};
    }
    return recoverLocked(state_, journal, directory);
}

bool Coordinator::readStoredState(
    StoredState &stored,
    int timeoutMs,
    QString &error)
{
    stored = {};
    error.clear();
    if (!state_ || state_->accountKey.trimmed().isEmpty()
        || state_->accountKey.size() > 512
        || !state_->storage.isValid() || timeoutMs <= 0) {
        error = QStringLiteral(
            "The Strava credential snapshot inputs are invalid");
        return false;
    }

    AnchoredFileSystem::DirectoryAnchor directory;
    if (!openNamespace(*state_, directory, error)) return false;
    std::unique_ptr<AnchoredFileSystem::PinnedFile> lock;
    if (!acquireAccountLock(
            directory, timeoutMs, lock, error)) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "Another process is changing this Strava authorization");
        }
        return false;
    }
    if (!refreshStorageMetadata(state_->storage, error))
        return false;

    JournalState journal;
    if (!loadJournal(*state_, directory, journal, error))
        return false;
    const RecoveryResult recovered = recoverLocked(
        state_, journal, directory);
    if (!recovered.isSuccess()) {
        error = recovered.error.isEmpty()
            ? QStringLiteral(
                  "The Strava credential snapshot requires recovery")
            : recovered.error;
        return false;
    }
    return readCoherentStoredState(state_->storage, stored, error);
}

} // namespace StravaCredentialDurability
