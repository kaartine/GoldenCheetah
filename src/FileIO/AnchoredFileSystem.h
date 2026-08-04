/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_AnchoredFileSystem_h
#define GC_AnchoredFileSystem_h

#include <QByteArray>
#include <QDebug>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <utility>

namespace AnchoredFileSystem {

namespace Detail {
struct DirectoryState;
struct PinnedFileState;
struct PrivateDirectoryOperations;
}

class EntryRef;
class PinnedFile;
struct MutationResult;
struct WriterPinHandoffState;

class NativeIdentity
{
public:
    NativeIdentity() = default;
    NativeIdentity(QByteArray key, quint64 linkCount)
        : key_(std::move(key)), linkCount_(linkCount)
    {
    }

    bool isValid() const { return !key_.isEmpty(); }
    quint64 linkCount() const { return linkCount_; }

    bool operator==(const NativeIdentity &other) const
    {
        return key_ == other.key_;
    }

    bool operator!=(const NativeIdentity &other) const
    {
        return !(*this == other);
    }

private:
    QByteArray key_;
    quint64 linkCount_ = 0;

    friend struct Detail::DirectoryState;
    friend struct Detail::PinnedFileState;
    friend QDebug operator<<(QDebug, const NativeIdentity &);
};

QDebug operator<<(QDebug debug, const NativeIdentity &identity);

enum class DirectoryEntryKind {
    RegularFile,
    Directory
};

struct DirectoryEntry
{
    QString name;
    DirectoryEntryKind kind = DirectoryEntryKind::RegularFile;
    NativeIdentity identity;
};

class DirectoryAnchor
{
public:
    DirectoryAnchor() = default;

    static bool open(
        const QString &absolutePath,
        DirectoryAnchor &directory,
        QString &error);

    bool isValid() const { return bool(state_); }
    NativeIdentity identity() const;
    bool openChild(
        const QString &component,
        DirectoryAnchor &directory,
        QString &error) const;
    bool openChildIfExists(
        const QString &component,
        DirectoryAnchor &directory,
        bool &exists,
        QString &error) const;
    // Enumerates the held directory generation, not its display pathname.
    // Unsafe types, observable two-pass changes, and budget exhaustion fail
    // closed. Exact identity/metadata reuse can remain unobservable, so a
    // caller opening an entry must compare the returned identity afterward.
    bool enumerateEntries(
        QList<DirectoryEntry> &entries,
        qsizetype maximumEntries,
        QString &error) const;
    EntryRef entry(const QString &component, QString &error) const;
    bool pathMatches(QString &error) const;
    bool sync(QString &error) const;

private:
    explicit DirectoryAnchor(
        std::shared_ptr<Detail::DirectoryState> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<Detail::DirectoryState> state_;

    friend class EntryRef;
    friend class PinnedFile;
    friend bool pinRegularFile(
        const EntryRef &, class PinnedFile &, QString &, qint64);
    friend bool entryExists(
        const EntryRef &, bool &, QString &);
    friend bool entryMatches(
        const EntryRef &, const class PinnedFile &, bool &, QString &);
    friend bool copyToNewFile(
        const class PinnedFile &, const EntryRef &,
        class PinnedFile &, QString &);
    friend bool writeNewFile(
        const QByteArray &, const EntryRef &,
        class PinnedFile &, QString &);
    friend MutationResult moveNoReplace(
        PinnedFile &, const EntryRef &);
    friend bool validateCurrentUserOwnedDirectory(
        const DirectoryAnchor &, QString &);
    friend bool validateCurrentUserControlledDirectory(
        const DirectoryAnchor &, QString &);
    friend bool hardenPrivateDirectory(
        DirectoryAnchor &, QString &);
    friend struct Detail::PrivateDirectoryOperations;
    friend MutationResult remove(PinnedFile &);
    friend MutationResult removeEmptyDirectory(DirectoryAnchor &);
    friend struct Detail::PinnedFileState;
};

class EntryRef
{
public:
    EntryRef() = default;

    bool isValid() const
    {
        return parent_.isValid() && !component_.isEmpty();
    }

    QString displayPath() const { return displayPath_; }

private:
    EntryRef(
        DirectoryAnchor parent,
        QString component,
        QString displayPath)
        : parent_(std::move(parent)),
          component_(std::move(component)),
          displayPath_(std::move(displayPath))
    {
    }

    DirectoryAnchor parent_;
    QString component_;
    QString displayPath_;

    friend class DirectoryAnchor;
    friend class PinnedFile;
    friend bool pinRegularFile(
        const EntryRef &, class PinnedFile &, QString &, qint64);
    friend bool entryExists(
        const EntryRef &, bool &, QString &);
    friend bool entryMatches(
        const EntryRef &, const class PinnedFile &, bool &, QString &);
    friend bool copyToNewFile(
        const class PinnedFile &, const EntryRef &,
        class PinnedFile &, QString &);
    friend bool writeNewFile(
        const QByteArray &, const EntryRef &,
        class PinnedFile &, QString &);
    friend struct Detail::PinnedFileState;
    friend MutationResult moveNoReplace(
        PinnedFile &, const EntryRef &);
    friend MutationResult remove(PinnedFile &);
};

class PinnedFile
{
public:
    PinnedFile();
    ~PinnedFile();

    PinnedFile(PinnedFile &&other) noexcept;
    PinnedFile &operator=(PinnedFile &&other) noexcept;

    PinnedFile(const PinnedFile &) = delete;
    PinnedFile &operator=(const PinnedFile &) = delete;

    bool isValid() const { return bool(state_); }
    NativeIdentity identity() const;
    qint64 size() const;
    QByteArray sha256() const;

private:
    std::unique_ptr<Detail::PinnedFileState> state_;
    QString verifiedPath(const EntryRef &entry) const;

    friend bool pinRegularFile(
        const EntryRef &, PinnedFile &, QString &, qint64);
    friend bool pinRegularFileAfterWriterRelease(
        const EntryRef &, qintptr, qint64, const QByteArray &,
        const std::function<void()> &, PinnedFile &,
        WriterPinHandoffState &, QString &);
    friend bool entryMatches(
        const EntryRef &, const PinnedFile &, bool &, QString &);
    friend bool readAll(
        const PinnedFile &, qint64, QByteArray &, QString &);
    friend QString verifiedRecoveryPath(
        const PinnedFile &, const EntryRef &);
    friend bool copyToNewFile(
        const PinnedFile &, const EntryRef &, PinnedFile &, QString &);
    friend bool writeNewFile(
        const QByteArray &, const EntryRef &, PinnedFile &, QString &);
    friend struct MutationResult moveNoReplace(
        PinnedFile &, const EntryRef &);
    friend struct MutationResult remove(PinnedFile &);
};

enum class MutationEffect {
    NoEffect,
    AppliedDurable,
    AppliedNotDurable,
    Conflict,
    Partial
};

struct MutationResult
{
    MutationEffect effect = MutationEffect::NoEffect;
    QString error;
    QString verifiedRecoveryPath;
    // The platform accepted deletion, even if final verification is pending.
    bool removalRequested = false;

    bool applied() const
    {
        return effect == MutationEffect::AppliedDurable
            || effect == MutationEffect::AppliedNotDurable;
    }
};

bool pinRegularFile(
    const EntryRef &entry,
    PinnedFile &file,
    QString &error,
    qint64 maximumSize = -1);

struct WriterPinHandoffState
{
    bool writerReleased = false;
};

// Keeps the writer identity anchored while its write handle is closed.
bool pinRegularFileAfterWriterRelease(
    const EntryRef &entry,
    qintptr writerDescriptor,
    qint64 expectedSize,
    const QByteArray &expectedSha256,
    const std::function<void()> &releaseWriter,
    PinnedFile &file,
    WriterPinHandoffState &handoff,
    QString &error);

bool entryExists(
    const EntryRef &entry,
    bool &exists,
    QString &error);

bool entryMatches(
    const EntryRef &entry,
    const PinnedFile &file,
    bool &matches,
    QString &error);

bool readAll(
    const PinnedFile &file,
    qint64 maximumSize,
    QByteArray &contents,
    QString &error);

QString verifiedRecoveryPath(
    const PinnedFile &file,
    const EntryRef &entry);

bool copyToNewFile(
    const PinnedFile &source,
    const EntryRef &destination,
    PinnedFile &copy,
    QString &error);

bool writeNewFile(
    const QByteArray &contents,
    const EntryRef &destination,
    PinnedFile &file,
    QString &error);

MutationResult moveNoReplace(
    PinnedFile &source,
    const EntryRef &destination);

// Private-directory operations exclude processes sharing the same OS identity
// and privileged administrators from their attacker model.
// These validators inspect the anchored object and revalidate its pathname.
bool validateCurrentUserOwnedDirectory(
    const DirectoryAnchor &directory,
    QString &error);

// Allows nonprivate read/traverse access but rejects untrusted principals that
// can mutate this directory, its child names, owner, or access controls.
bool validateCurrentUserControlledDirectory(
    const DirectoryAnchor &directory,
    QString &error);

MutationResult createPrivateChildDirectory(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory);

// Creates the requested name directly under an owner-controlled parent. The
// child is private from creation. After creation, failures retain the fixed
// name and, once established, its DirectoryAnchor for recovery or retry.
MutationResult createPrivateFixedChildDirectory(
    const DirectoryAnchor &parent,
    const QString &component,
    DirectoryAnchor &directory);

bool hardenPrivateDirectory(
    DirectoryAnchor &directory,
    QString &error);

MutationResult remove(PinnedFile &file);

MutationResult removeEmptyDirectory(DirectoryAnchor &directory);

} // namespace AnchoredFileSystem

#endif
