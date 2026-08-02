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
#include <QString>
#include <QtGlobal>

#include <memory>
#include <utility>

namespace AnchoredFileSystem {

namespace Detail {
struct DirectoryState;
struct PinnedFileState;
}

class EntryRef;
class PinnedFile;
struct MutationResult;

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
    EntryRef entry(const QString &component, QString &error) const;
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
        const EntryRef &, class PinnedFile &, QString &);
    friend bool entryMatches(
        const EntryRef &, const class PinnedFile &, bool &, QString &);
    friend bool copyToNewFile(
        const class PinnedFile &, const EntryRef &,
        class PinnedFile &, QString &);
    friend MutationResult moveNoReplace(
        PinnedFile &, const EntryRef &);
    friend MutationResult remove(PinnedFile &);
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
        const EntryRef &, class PinnedFile &, QString &);
    friend bool entryMatches(
        const EntryRef &, const class PinnedFile &, bool &, QString &);
    friend bool copyToNewFile(
        const class PinnedFile &, const EntryRef &,
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

    friend bool pinRegularFile(
        const EntryRef &, PinnedFile &, QString &);
    friend bool entryMatches(
        const EntryRef &, const PinnedFile &, bool &, QString &);
    friend bool readAll(
        const PinnedFile &, qint64, QByteArray &, QString &);
    friend bool copyToNewFile(
        const PinnedFile &, const EntryRef &, PinnedFile &, QString &);
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

    bool applied() const
    {
        return effect == MutationEffect::AppliedDurable
            || effect == MutationEffect::AppliedNotDurable;
    }
};

bool pinRegularFile(
    const EntryRef &entry,
    PinnedFile &file,
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

bool copyToNewFile(
    const PinnedFile &source,
    const EntryRef &destination,
    PinnedFile &copy,
    QString &error);

MutationResult moveNoReplace(
    PinnedFile &source,
    const EntryRef &destination);

MutationResult remove(PinnedFile &file);

} // namespace AnchoredFileSystem

#endif
