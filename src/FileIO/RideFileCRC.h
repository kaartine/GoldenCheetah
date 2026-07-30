/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RideFileCRC_h
#define GC_RideFileCRC_h

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <functional>
#include <memory>

class QFile;
class QIODevice;
class QString;
class QTemporaryDir;

namespace RideFileCRC {

inline constexpr qint64 ReadChunkSize = 64 * 1024;
inline constexpr qint64 MaximumSourceSize =
    512LL * 1024 * 1024;
inline constexpr qsizetype Sha256Size = 32;

using SnapshotValidator = std::function<bool()>;
using ChunkObserver = std::function<void(qint64)>;

// The SHA-256 value contains the 32 raw digest bytes, not hexadecimal text.
struct ContentFingerprint
{
    qint64 byteSize = -1;
    QByteArray sha256;
    quint16 legacyCrc16 = 0;

    bool isValid() const;
    bool operator==(const ContentFingerprint &other) const;
    bool operator!=(const ContentFingerprint &other) const
    {
        return !(*this == other);
    }
};

class StagedSource;

// Boolean APIs in this namespace leave output arguments unchanged on failure.
// captureFile also verifies that the opened source and its path still identify
// the same regular file before and after staging.
bool captureFile(
    const QString &filename,
    StagedSource &captured);

#ifdef GC_RIDE_FILE_CRC_TEST_HOOKS
struct FileTestHooks;

bool captureFileForTest(
    const QString &filename,
    StagedSource &captured,
    const FileTestHooks &hooks);
#endif

// Owns a private staged copy until destruction. On successful capture the
// QFile is closed, positioned independently of the source, and intended to
// be opened read-only by a RideFileReader.
class StagedSource
{
public:
    StagedSource();
    ~StagedSource();

    StagedSource(StagedSource &&other) noexcept;
    StagedSource &operator=(StagedSource &&other) noexcept;

    StagedSource(const StagedSource &) = delete;
    StagedSource &operator=(const StagedSource &) = delete;

    bool isValid() const;
    QFile *fileForReading();
    const QFile *fileForReading() const;
    const ContentFingerprint &fingerprint() const;

private:
    friend bool captureFile(
        const QString &filename,
        StagedSource &captured);
#ifdef GC_RIDE_FILE_CRC_TEST_HOOKS
    friend bool captureFileForTest(
        const QString &filename,
        StagedSource &captured,
        const FileTestHooks &hooks);
#endif

    std::unique_ptr<QTemporaryDir> stagedDirectory_;
    std::unique_ptr<QFile> stagedFile_;
    ContentFingerprint fingerprint_;
};

bool compute(
    QIODevice &input,
    quint16 &checksum,
    const SnapshotValidator &snapshotIsCurrent = {});

bool computeExact(
    QIODevice &input,
    qint64 expectedSize,
    quint16 &checksum,
    const SnapshotValidator &snapshotIsCurrent = {},
    const ChunkObserver &afterChunk = {});

bool computeFile(
    const QString &filename,
    quint16 &checksum);

bool computeFingerprint(
    QIODevice &input,
    ContentFingerprint &fingerprint,
    const SnapshotValidator &snapshotIsCurrent = {});

bool computeFileFingerprint(
    const QString &filename,
    ContentFingerprint &fingerprint);

#ifdef GC_RIDE_FILE_CRC_TEST_HOOKS
struct WindowsIdentityForTest
{
    bool extendedAvailable = false;
    quint64 extendedVolume = 0;
    std::array<unsigned char, 16> extendedId {};
    quint64 legacyVolume = 0;
    quint64 legacyIndex = 0;
};

bool windowsIdentityUsableForTest(
    const WindowsIdentityForTest &identity);

bool windowsIdentitiesMatchForTest(
    const WindowsIdentityForTest &left,
    const WindowsIdentityForTest &right);

struct FileTestHooks
{
    std::function<void()> afterInitialSnapshot;
    ChunkObserver afterChunk;
    std::function<void()> afterSourceCopied;
};

bool computeFileForTest(
    const QString &filename,
    quint16 &checksum,
    const FileTestHooks &hooks);
#endif

} // namespace RideFileCRC

#endif
