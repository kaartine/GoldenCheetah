/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#if defined(_WIN32)
#if __has_include(<qt_windows.h>)
#include <qt_windows.h>
#else
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <io.h>
#endif

#include "RideFileCRC.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <algorithm>
#include <cstring>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class PositionRestorer
{
public:
    explicit PositionRestorer(QIODevice &input)
        : input_(input),
          position_(input.pos())
    {
    }

    ~PositionRestorer()
    {
        if (!restored_ && position_ >= 0)
            input_.seek(position_);
    }

    bool isValid() const
    {
        return position_ >= 0;
    }

    bool restore()
    {
        if (restored_)
            return true;
        restored_ = input_.seek(position_);
        return restored_;
    }

private:
    QIODevice &input_;
    qint64 position_;
    bool restored_ = false;
};

class Iso3309Checksum
{
public:
    void update(QByteArrayView bytes)
    {
        static constexpr quint16 table[16] = {
            0x0000, 0x1081, 0x2102, 0x3183,
            0x4204, 0x5285, 0x6306, 0x7387,
            0x8408, 0x9489, 0xa50a, 0xb58b,
            0xc60c, 0xd68d, 0xe70e, 0xf78f
        };

        const auto *current =
            reinterpret_cast<const uchar *>(bytes.data());
        for (qsizetype remaining = bytes.size();
             remaining > 0;
             --remaining) {
            uchar byte = *current++;
            state_ = static_cast<quint16>(
                ((state_ >> 4) & 0x0fff)
                ^ table[(state_ ^ byte) & 0x0f]);
            byte >>= 4;
            state_ = static_cast<quint16>(
                ((state_ >> 4) & 0x0fff)
                ^ table[(state_ ^ byte) & 0x0f]);
        }
    }

    quint16 result() const
    {
        return static_cast<quint16>(~state_);
    }

private:
    quint16 state_ = 0xffff;
};

struct WindowsFileIdentity
{
    bool extendedAvailable = false;
    quint64 extendedVolume = 0;
    std::array<unsigned char, 16> extendedId {};
    quint64 legacyVolume = 0;
    quint64 legacyIndex = 0;

    bool hasExtendedIdentity() const
    {
        return extendedAvailable
            && std::any_of(
                extendedId.cbegin(),
                extendedId.cend(),
                [](unsigned char byte) {
                    return byte != 0;
                });
    }

    bool usable() const
    {
        return hasExtendedIdentity()
            || legacyIndex != 0;
    }

    bool matches(
        const WindowsFileIdentity &other) const
    {
        if (hasExtendedIdentity()
            && other.hasExtendedIdentity()) {
            return extendedVolume
                    == other.extendedVolume
                && extendedId == other.extendedId;
        }

        return legacyIndex != 0
            && other.legacyIndex != 0
            && legacyVolume == other.legacyVolume
            && legacyIndex == other.legacyIndex;
    }
};

#if defined(Q_OS_WIN)

struct NativeFileSnapshot
{
    WindowsFileIdentity identity;
    qint64 size = -1;
    qint64 modified = 0;
    qint64 metadataChanged = 0;

    bool operator==(const NativeFileSnapshot &other) const
    {
        return identity.matches(other.identity)
            && size == other.size
            && modified == other.modified
            && metadataChanged == other.metadataChanged;
    }
};

bool captureNativeSnapshot(
    QFile &file,
    NativeFileSnapshot &snapshot)
{
    const int descriptor =
        static_cast<int>(file.handle());
    if (descriptor < 0)
        return false;

    const intptr_t nativeValue =
        _get_osfhandle(descriptor);
    if (nativeValue == -1)
        return false;
    const HANDLE handle =
        reinterpret_cast<HANDLE>(nativeValue);
    if (GetFileType(handle) != FILE_TYPE_DISK)
        return false;

    BY_HANDLE_FILE_INFORMATION legacy {};
#if _WIN32_WINNT >= 0x0602
    FILE_ID_INFO identity {};
#endif
    FILE_STANDARD_INFO standard {};
    FILE_BASIC_INFO basic {};
    if (!GetFileInformationByHandle(
            handle, &legacy)
        || !GetFileInformationByHandleEx(
            handle,
            FileStandardInfo,
            &standard,
            sizeof(standard))
        || !GetFileInformationByHandleEx(
            handle,
            FileBasicInfo,
            &basic,
            sizeof(basic))
        || standard.Directory
        || standard.EndOfFile.QuadPart < 0) {
        return false;
    }

    snapshot.identity.legacyVolume =
        legacy.dwVolumeSerialNumber;
    snapshot.identity.legacyIndex =
        (static_cast<quint64>(
             legacy.nFileIndexHigh)
         << 32)
        | legacy.nFileIndexLow;
#if _WIN32_WINNT >= 0x0602
    snapshot.identity.extendedAvailable =
        GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            &identity,
            sizeof(identity));
    snapshot.identity.extendedVolume =
        identity.VolumeSerialNumber;
    std::memcpy(
        snapshot.identity.extendedId.data(),
        identity.FileId.Identifier,
        snapshot.identity.extendedId.size());
#endif
    if (!snapshot.identity.usable())
        return false;

    snapshot.size =
        standard.EndOfFile.QuadPart;
    snapshot.modified =
        basic.LastWriteTime.QuadPart;
    snapshot.metadataChanged =
        basic.ChangeTime.QuadPart;
    return true;
}

#elif defined(Q_OS_UNIX)

struct NativeTimestamp
{
    qint64 seconds = 0;
    qint64 nanoseconds = 0;

    bool operator==(const NativeTimestamp &other) const
    {
        return seconds == other.seconds
            && nanoseconds == other.nanoseconds;
    }
};

struct NativeFileSnapshot
{
    quint64 device = 0;
    quint64 inode = 0;
    qint64 size = -1;
    NativeTimestamp modified;
    NativeTimestamp metadataChanged;

    bool operator==(const NativeFileSnapshot &other) const
    {
        return device == other.device
            && inode == other.inode
            && size == other.size
            && modified == other.modified
            && metadataChanged == other.metadataChanged;
    }
};

bool captureNativeSnapshot(
    QFile &file,
    NativeFileSnapshot &snapshot)
{
    const int descriptor =
        static_cast<int>(file.handle());
    if (descriptor < 0)
        return false;

    struct stat status {};
    if (::fstat(descriptor, &status) != 0
        || !S_ISREG(status.st_mode)
        || status.st_size < 0) {
        return false;
    }

    snapshot.device =
        static_cast<quint64>(status.st_dev);
    snapshot.inode =
        static_cast<quint64>(status.st_ino);
    snapshot.size =
        static_cast<qint64>(status.st_size);
#if defined(Q_OS_MACOS)
    snapshot.modified = {
        static_cast<qint64>(
            status.st_mtimespec.tv_sec),
        static_cast<qint64>(
            status.st_mtimespec.tv_nsec)
    };
    snapshot.metadataChanged = {
        static_cast<qint64>(
            status.st_ctimespec.tv_sec),
        static_cast<qint64>(
            status.st_ctimespec.tv_nsec)
    };
#else
    snapshot.modified = {
        static_cast<qint64>(status.st_mtim.tv_sec),
        static_cast<qint64>(status.st_mtim.tv_nsec)
    };
    snapshot.metadataChanged = {
        static_cast<qint64>(status.st_ctim.tv_sec),
        static_cast<qint64>(status.st_ctim.tv_nsec)
    };
#endif
    return true;
}

#else

struct NativeFileSnapshot
{
    qint64 size = -1;

    bool operator==(const NativeFileSnapshot &other) const
    {
        return size == other.size;
    }
};

bool captureNativeSnapshot(
    QFile &,
    NativeFileSnapshot &)
{
    return false;
}

#endif

bool capturePathSnapshot(
    const QString &path,
    NativeFileSnapshot &snapshot)
{
    QFile current(path);
    return current.open(QIODevice::ReadOnly)
        && captureNativeSnapshot(current, snapshot);
}

bool computeFileImpl(
    const QString &filename,
    quint16 &checksum,
    const std::function<void()> &afterInitialSnapshot,
    const RideFileCRC::ChunkObserver &afterChunk)
{
    const QString absolutePath =
        QFileInfo(filename).absoluteFilePath();
    QFile input(absolutePath);
    if (!input.open(QIODevice::ReadOnly))
        return false;

    NativeFileSnapshot initial;
    if (!captureNativeSnapshot(input, initial))
        return false;

    if (afterInitialSnapshot)
        afterInitialSnapshot();

    NativeFileSnapshot initialPath;
    if (!capturePathSnapshot(
            absolutePath, initialPath)
        || !(initial == initialPath)) {
        return false;
    }

    quint16 computed = 0;
    if (!RideFileCRC::computeExact(
            input,
            initial.size,
            computed,
            {},
            afterChunk)) {
        return false;
    }

    NativeFileSnapshot finalHandle;
    NativeFileSnapshot finalPath;
    if (!captureNativeSnapshot(
            input, finalHandle)
        || !(initial == finalHandle)
        || !capturePathSnapshot(
            absolutePath, finalPath)
        || !(initial == finalPath)) {
        return false;
    }

    checksum = computed;
    return true;
}

#ifdef GC_RIDE_FILE_CRC_TEST_HOOKS
WindowsFileIdentity windowsIdentityForTest(
    const RideFileCRC::WindowsIdentityForTest &identity)
{
    WindowsFileIdentity converted;
    converted.extendedAvailable =
        identity.extendedAvailable;
    converted.extendedVolume =
        identity.extendedVolume;
    converted.extendedId =
        identity.extendedId;
    converted.legacyVolume =
        identity.legacyVolume;
    converted.legacyIndex =
        identity.legacyIndex;
    return converted;
}
#endif

} // namespace

namespace RideFileCRC {

bool compute(
    QIODevice &input,
    quint16 &checksum,
    const SnapshotValidator &snapshotIsCurrent)
{
    return computeExact(
        input,
        input.size(),
        checksum,
        snapshotIsCurrent);
}

bool computeExact(
    QIODevice &input,
    qint64 expectedSize,
    quint16 &checksum,
    const SnapshotValidator &snapshotIsCurrent,
    const ChunkObserver &afterChunk)
{
    if (!input.isOpen()
        || !input.isReadable()
        || input.isSequential()
        || expectedSize < 0
        || expectedSize > MaximumSourceSize) {
        return false;
    }

    PositionRestorer restorePosition(input);
    if (!restorePosition.isValid()
        || !input.seek(0)) {
        return false;
    }

    if (snapshotIsCurrent && !snapshotIsCurrent()) {
        return false;
    }

    QByteArray buffer(
        static_cast<int>(ReadChunkSize),
        Qt::Uninitialized);
    Iso3309Checksum accumulator;
    qint64 remaining = expectedSize;
    while (remaining > 0) {
        const qint64 requested =
            std::min(remaining, ReadChunkSize);
        qint64 received = 0;
        while (received < requested) {
            const qint64 count = input.read(
                buffer.data() + received,
                requested - received);
            if (count <= 0
                || count > requested - received) {
                return false;
            }
            accumulator.update(QByteArrayView(
                buffer.constData() + received,
                count));
            received += count;
        }
        remaining -= received;
        if (afterChunk)
            afterChunk(expectedSize - remaining);
    }

    char trailingByte = 0;
    const qint64 trailingRead =
        input.read(&trailingByte, 1);
    if (trailingRead != 0
        || !input.atEnd()
        || input.size() != expectedSize
        || (snapshotIsCurrent && !snapshotIsCurrent())
        || !restorePosition.restore()) {
        return false;
    }

    checksum = accumulator.result();
    return true;
}

bool computeFile(
    const QString &filename,
    quint16 &checksum)
{
    return computeFileImpl(
        filename, checksum, {}, {});
}

#ifdef GC_RIDE_FILE_CRC_TEST_HOOKS
bool windowsIdentityUsableForTest(
    const WindowsIdentityForTest &identity)
{
    return windowsIdentityForTest(identity).usable();
}

bool windowsIdentitiesMatchForTest(
    const WindowsIdentityForTest &left,
    const WindowsIdentityForTest &right)
{
    return windowsIdentityForTest(left).matches(
        windowsIdentityForTest(right));
}

bool computeFileForTest(
    const QString &filename,
    quint16 &checksum,
    const FileTestHooks &hooks)
{
    return computeFileImpl(
        filename,
        checksum,
        hooks.afterInitialSnapshot,
        hooks.afterChunk);
}
#endif

} // namespace RideFileCRC
