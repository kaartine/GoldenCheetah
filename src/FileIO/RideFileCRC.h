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

#include <QtGlobal>

#include <array>
#include <functional>

class QIODevice;
class QString;

namespace RideFileCRC {

inline constexpr qint64 ReadChunkSize = 64 * 1024;
inline constexpr qint64 MaximumSourceSize =
    512LL * 1024 * 1024;

using SnapshotValidator = std::function<bool()>;
using ChunkObserver = std::function<void(qint64)>;

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
};

bool computeFileForTest(
    const QString &filename,
    quint16 &checksum,
    const FileTestHooks &hooks);
#endif

} // namespace RideFileCRC

#endif
