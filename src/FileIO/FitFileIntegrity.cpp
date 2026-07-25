/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "FitFileIntegrity.h"

#include <QByteArray>
#include <QtEndian>

namespace {

constexpr qint64 CrcChunkSize = 64 * 1024;

class PositionRestorer
{
public:
    explicit PositionRestorer(QIODevice &device)
        : device(device), position(device.pos())
    {
    }

    ~PositionRestorer()
    {
        if (position >= 0) {
            device.seek(position);
        }
    }

private:
    QIODevice &device;
    qint64 position;
};

quint16 updateCrc(quint16 crc, const QByteArray &bytes)
{
    for (const char value : bytes) {
        crc = static_cast<quint16>(
            crc ^ static_cast<quint8>(value));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001)
                ? static_cast<quint16>((crc >> 1) ^ 0xA001)
                : static_cast<quint16>(crc >> 1);
        }
    }
    return crc;
}

quint16 littleEndian16(const char *bytes)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes));
}

quint32 littleEndian32(const char *bytes)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes));
}

FitFileIntegrity::ValidationResult failure(
    const QString &message, int segmentCount)
{
    FitFileIntegrity::ValidationResult result;
    result.error = message;
    result.segmentCount = segmentCount;
    return result;
}

QString segmentError(int segment, const QString &message)
{
    return QStringLiteral("FIT segment %1: %2")
        .arg(segment)
        .arg(message);
}

} // namespace

namespace FitFileIntegrity {

ValidationResult validate(QIODevice &device,
                          qint64 maximumFileSize)
{
    if (!device.isOpen() || !device.isReadable()) {
        return failure(
            QStringLiteral("FIT input is not open for reading."), 0);
    }
    if (device.isSequential()) {
        return failure(
            QStringLiteral("FIT integrity checking requires a seekable input."),
            0);
    }

    PositionRestorer restorePosition(device);
    const qint64 fileSize = device.size();
    if (fileSize <= 0) {
        return failure(QStringLiteral("FIT input is empty."), 0);
    }
    if (maximumFileSize < 0 || fileSize > maximumFileSize) {
        return failure(
            QStringLiteral("FIT input exceeds the supported size limit."), 0);
    }

    qint64 segmentOffset = 0;
    int segmentCount = 0;
    while (segmentOffset < fileSize) {
        const int segment = segmentCount + 1;
        if (!device.seek(segmentOffset)) {
            return failure(
                segmentError(segment,
                             QStringLiteral("cannot seek to the header.")),
                segmentCount);
        }

        const QByteArray sizeByte = device.read(1);
        if (sizeByte.size() != 1) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the header is truncated.")),
                segmentCount);
        }
        const quint8 headerSize =
            static_cast<quint8>(sizeByte.at(0));
        if (headerSize != 12 && headerSize != 14) {
            return failure(
                segmentError(
                    segment,
                    QStringLiteral("unsupported header size %1.")
                        .arg(headerSize)),
                segmentCount);
        }

        const qint64 bytesAvailable = fileSize - segmentOffset;
        if (bytesAvailable < qint64(headerSize) + 2) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the header or file CRC is truncated.")),
                segmentCount);
        }
        if (!device.seek(segmentOffset)) {
            return failure(
                segmentError(segment,
                             QStringLiteral("cannot reread the header.")),
                segmentCount);
        }
        const QByteArray header = device.read(headerSize);
        if (header.size() != headerSize) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the header is truncated.")),
                segmentCount);
        }
        if (header.mid(8, 4) != QByteArrayLiteral(".FIT")) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the .FIT signature is missing.")),
                segmentCount);
        }

        if (headerSize == 14) {
            const quint16 storedHeaderCrc =
                littleEndian16(header.constData() + 12);
            const quint16 computedHeaderCrc =
                updateCrc(0, header.left(12));
            if (storedHeaderCrc != 0
                && storedHeaderCrc != computedHeaderCrc) {
                return failure(
                    segmentError(segment,
                                 QStringLiteral("the header CRC is invalid.")),
                    segmentCount);
            }
        }

        const quint32 dataSize =
            littleEndian32(header.constData() + 4);
        const qint64 bytesAfterHeader =
            bytesAvailable - headerSize;
        if (qint64(dataSize) > bytesAfterHeader - 2) {
            return failure(
                segmentError(
                    segment,
                    QStringLiteral("the declared data length exceeds the input.")),
                segmentCount);
        }

        const qint64 crcOffset = segmentOffset
            + headerSize + qint64(dataSize);
        const qint64 segmentEnd = crcOffset + 2;
        if (!device.seek(segmentOffset)) {
            return failure(
                segmentError(segment,
                             QStringLiteral("cannot seek to the CRC range.")),
                segmentCount);
        }

        quint16 computedFileCrc = 0;
        qint64 remaining = qint64(headerSize) + dataSize;
        while (remaining > 0) {
            const qint64 requested = qMin(remaining, CrcChunkSize);
            const QByteArray chunk = device.read(requested);
            if (chunk.size() != requested) {
                return failure(
                    segmentError(segment,
                                 QStringLiteral("the data section is truncated.")),
                    segmentCount);
            }
            computedFileCrc = updateCrc(computedFileCrc, chunk);
            remaining -= requested;
        }

        const QByteArray storedCrcBytes = device.read(2);
        if (storedCrcBytes.size() != 2) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the file CRC is truncated.")),
                segmentCount);
        }
        const quint16 storedFileCrc =
            littleEndian16(storedCrcBytes.constData());
        if (storedFileCrc != computedFileCrc) {
            return failure(
                segmentError(segment,
                             QStringLiteral("the file CRC is invalid.")),
                segmentCount);
        }

        segmentOffset = segmentEnd;
        ++segmentCount;
    }

    ValidationResult result;
    result.valid = true;
    result.segmentCount = segmentCount;
    return result;
}

bool consumeRecordBytes(qint64 &remaining,
                        qint64 consumed)
{
    if (remaining < 0 || consumed <= 0
        || consumed > remaining) {
        return false;
    }
    remaining -= consumed;
    return true;
}

} // namespace FitFileIntegrity
