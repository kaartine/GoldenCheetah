/*
 * Copyright (c) 2010 Ilja Booij (ibooij@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "RideFile.h"
#include <QtEndian>
#include <QtGlobal>
#include <QDateTime>
#include <QDebug>
#include <QFile>

#include <cstring>
#include <limits>
#include <memory>

struct TacxCafFileReader : public RideFileReader {
    virtual RideFile *openRideFile(QFile &file, QStringList &errors, QList<RideFile*>* = 0) const;
    virtual bool hasWrite() const { return false; }
};

static int tacxCafFileReaderRegistered =
    RideFileFactory::instance().registerReader(
        "caf", "Tacx Fortius", new TacxCafFileReader());

struct file_header_t {
    qint16 fingerprint;
    qint16 version;
    qint32 blockCount;
};

/** a simple struct which describes the header of a block */
struct block_header_t {
    /** fingerprint of the block, see the list of block fingerprints */
    qint16 fingerprint;
    /** version of the block */
    qint16 version;
    /** nr of records in the block */
    qint32 recordCount;
    /** size of each record */
    qint32 recordSize;
};

/**
  Read header block, containing general information about the file.
  */
static bool readHeaderBlock(const QByteArray& bytes, file_header_t& header,
                            QStringList& errors);

/**
  Read all data blocks and return a RideFile.
  */
static RideFile* readBlocks(const QByteArray& bytes,
                            const file_header_t& fileHeader,
                            QStringList& errors);

/**
  read the header of one block
  */
static bool readBlockHeader(const QByteArray& bytes, qsizetype offset,
                            block_header_t& header);

/**
  Read general information about the ride
  */
static bool readRideInformationBlock(RideFile* rideFile, const QByteArray& bytes,
                                     qsizetype offset, qsizetype size);

/**
  Read the actual data points
  */
static bool readRideData(RideFile *rideFile, const QByteArray& bytes,
                         qsizetype offset, qsizetype size,
                         qint32 nrOfRecords, qint32 recordSize);

static qint16 readShort(const char *bytes);
static quint16 readUnsignedShort(const char *bytes);
static qint32 readInt(const char *bytes);
static float readFloat(const char *bytes);

static const qint16 TACX_CAF_FILE_FINGERPRINT = 3000;
static const qsizetype TACX_HEADER_BLOCK_SIZE = 8;
static const qsizetype TACX_BLOCK_INFO_HEADER_SIZE = 12;
static const qsizetype TACX_RIDE_INFORMATION_PREFIX_SIZE = 20;
static const qint32 TACX_RIDE_INFORMATION_V100_RECORD_SIZE = 202;
static const qint32 TACX_RIDE_INFORMATION_V110_RECORD_SIZE = 236;
static const qint32 TACX_RIDE_DATA_V100_RECORD_SIZE = 10;
static const qint32 TACX_RIDE_DATA_V110_RECORD_SIZE = 18;

// Block fingerprints
static const qint16 TACX_RIDE_INFORMATION_BLOCK = 3010;
static const qint16 TACX_RIDE_DATA_BLOCK = 3020;

static const QString TACX_FORTIUS_DEVICE_TYPE = "Tacx Fortius";

static bool isSupportedVersion(qint16 version)
{
    return version == 100 || version == 110;
}

static bool isSupportedBlock(qint16 fingerprint, qint16 version)
{
    switch (fingerprint) {
    case 120:  // Notes
    case 130:  // Unknown RLV metadata
    case 210:  // Rider information
    case 1010: // General information
    case 1020: // Program
    case 2010: // RLV video information
    case 2040: // Course information
    case 6010: // RLV multi-course information
    case 6020: // RLV ItemMultiSect
        return version == 100;
    case TACX_RIDE_INFORMATION_BLOCK:
    case TACX_RIDE_DATA_BLOCK:
        return isSupportedVersion(version);
    default:
        return false;
    }
}

static qint32 rideInformationRecordSize(qint16 version)
{
    return version == 100 ? TACX_RIDE_INFORMATION_V100_RECORD_SIZE
                          : TACX_RIDE_INFORMATION_V110_RECORD_SIZE;
}

static qint32 rideDataRecordSize(qint16 version)
{
    return version == 100 ? TACX_RIDE_DATA_V100_RECORD_SIZE
                          : TACX_RIDE_DATA_V110_RECORD_SIZE;
}

RideFile *TacxCafFileReader::openRideFile(QFile &file, QStringList &errors, QList<RideFile*>*) const {
    if (!file.open(QFile::ReadOnly)) {
       errors << ("Could not open ride file: \""
                   + file.fileName() + "\"");
        return NULL;
    }

    QByteArray bytes = file.readAll();
    file.close();

    file_header_t fileHeader;
    if (!readHeaderBlock(bytes, fileHeader, errors))
        return NULL;

    return readBlocks(bytes, fileHeader, errors);
}

bool readHeaderBlock(const QByteArray& bytes, file_header_t& header,
                     QStringList& errors)
{
    if (bytes.size() < TACX_HEADER_BLOCK_SIZE) {
        errors << "Truncated Tacx CAF file header";
        return false;
    }

    const char *data = bytes.constData();
    header.fingerprint = readShort(data);
    header.version = readShort(data + 2);
    header.blockCount = readInt(data + 4);

    if (header.fingerprint != TACX_CAF_FILE_FINGERPRINT) {
        errors << "This is not a Tacx run file";
        return false;
    }
    if (!isSupportedVersion(header.version)) {
        errors << QString("Unsupported Tacx CAF file version: %1")
                      .arg(header.version);
        return false;
    }
    if (header.blockCount < 0) {
        errors << "Invalid Tacx CAF block count";
        return false;
    }

    const qsizetype bytesAfterHeader = bytes.size() - TACX_HEADER_BLOCK_SIZE;
    const qsizetype maximumBlockCount =
        bytesAfterHeader / TACX_BLOCK_INFO_HEADER_SIZE;
    if (static_cast<qint64>(header.blockCount)
        > static_cast<qint64>(maximumBlockCount)) {
        errors << "Tacx CAF block count exceeds the available file data";
        return false;
    }

    return true;
}

RideFile* readBlocks(const QByteArray& bytes, const file_header_t& fileHeader,
                     QStringList& errors)
{
    std::unique_ptr<RideFile> rideFile(new RideFile());

    rideFile->setDeviceType(TACX_FORTIUS_DEVICE_TYPE);
    rideFile->setFileFormat("Tacx Fortius (caf)");

    qsizetype position = TACX_HEADER_BLOCK_SIZE;
    bool foundRideInformation = false;
    bool foundRideData = false;

    for (qint32 blockIndex = 0; blockIndex < fileHeader.blockCount;
         ++blockIndex) {
        block_header_t blockHeader;
        if (!readBlockHeader(bytes, position, blockHeader)) {
            errors << "Truncated Tacx CAF block header";
            return NULL;
        }

        if (!isSupportedBlock(blockHeader.fingerprint,
                              blockHeader.version)) {
            errors << QString("Unsupported Tacx CAF block fingerprint/version: %1/%2")
                          .arg(blockHeader.fingerprint)
                          .arg(blockHeader.version);
            return NULL;
        }
        if (blockHeader.recordCount < 0) {
            errors << "Invalid Tacx CAF block record count";
            return NULL;
        }
        if (blockHeader.recordSize < 0) {
            errors << "Invalid Tacx CAF block record size";
            return NULL;
        }

        const bool emptyBlock = blockHeader.recordCount == 0
                             && blockHeader.recordSize == 0;
        if ((blockHeader.recordCount == 0) != (blockHeader.recordSize == 0)) {
            errors << "Invalid Tacx CAF empty block dimensions";
            return NULL;
        }
        if (emptyBlock
            && (blockHeader.fingerprint == TACX_RIDE_INFORMATION_BLOCK
                || blockHeader.fingerprint == TACX_RIDE_DATA_BLOCK)) {
            errors << "Required Tacx CAF block is empty";
            return NULL;
        }

        const qint64 payloadSize64 =
            static_cast<qint64>(blockHeader.recordCount)
            * static_cast<qint64>(blockHeader.recordSize);
        if (payloadSize64 > (std::numeric_limits<qsizetype>::max)()) {
            errors << "Tacx CAF block payload is too large";
            return NULL;
        }
        const qsizetype payloadSize = static_cast<qsizetype>(payloadSize64);
        const qsizetype payloadOffset = position + TACX_BLOCK_INFO_HEADER_SIZE;
        if (payloadSize > bytes.size() - payloadOffset) {
            errors << "Tacx CAF block payload exceeds remaining file size";
            return NULL;
        }

        switch (blockHeader.fingerprint) {
        case TACX_RIDE_INFORMATION_BLOCK: {
            if (foundRideInformation) {
                errors << "Duplicate Tacx CAF ride information block";
                return NULL;
            }
            if (blockHeader.recordCount != 1) {
                errors << "Invalid Tacx CAF ride information record count";
                return NULL;
            }
            if (blockHeader.recordSize
                != rideInformationRecordSize(blockHeader.version)) {
                errors << "Invalid Tacx CAF ride information record size";
                return NULL;
            }
            if (!readRideInformationBlock(rideFile.get(), bytes, payloadOffset,
                                          payloadSize)) {
                errors << "Invalid Tacx CAF ride information block";
                return NULL;
            }
            foundRideInformation = true;
            break;
        }
        case TACX_RIDE_DATA_BLOCK: {
            if (foundRideData) {
                errors << "Duplicate Tacx CAF ride data block";
                return NULL;
            }
            if (!foundRideInformation) {
                errors << "Tacx CAF ride data precedes ride information";
                return NULL;
            }
            if (blockHeader.recordCount == 0) {
                errors << "Tacx CAF ride data block has no records";
                return NULL;
            }
            if (blockHeader.recordSize != rideDataRecordSize(blockHeader.version)) {
                errors << "Invalid Tacx CAF ride data record size";
                return NULL;
            }
            if (!readRideData(rideFile.get(), bytes, payloadOffset, payloadSize,
                              blockHeader.recordCount, blockHeader.recordSize)) {
                errors << "Invalid Tacx CAF ride data block";
                return NULL;
            }
            foundRideData = true;
            break;
        }
        default:
            break;
        }

        position = payloadOffset + payloadSize;
    }

    if (position != bytes.size()) {
        errors << "Tacx CAF file contains data beyond its declared blocks";
        return NULL;
    }
    if (!foundRideInformation) {
        errors << "Tacx CAF file has no ride information block";
        return NULL;
    }
    if (!foundRideData) {
        errors << "Tacx CAF file has no ride data block";
        return NULL;
    }

    return rideFile.release();
}

bool readBlockHeader(const QByteArray& bytes, const qsizetype offset,
                     block_header_t& header)
{
    if (offset < 0 || offset > bytes.size()
        || bytes.size() - offset < TACX_BLOCK_INFO_HEADER_SIZE) {
        return false;
    }

    const char *data = bytes.constData() + offset;
    header.fingerprint = readShort(data);
    header.version = readShort(data + 2);
    header.recordCount = readInt(data + 4);
    header.recordSize = readInt(data + 8);
    return true;
}

bool readRideInformationBlock(RideFile* rideFile, const QByteArray& bytes,
                              const qsizetype offset, const qsizetype size)
{
    if (!rideFile || size < TACX_RIDE_INFORMATION_PREFIX_SIZE
        || offset < 0 || offset > bytes.size() || size > bytes.size() - offset) {
        return false;
    }

    const char *data = bytes.constData() + offset;
    const qint16 year = readShort(data);
    const qint16 month = readShort(data + 2);
    const qint16 day = readShort(data + 6);
    const qint16 hour = readShort(data + 8);
    const qint16 minute = readShort(data + 10);
    const qint16 second = readShort(data + 12);
    const float recordingInterval = readFloat(data + 16);

    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid()
        || !qIsFinite(recordingInterval) || recordingInterval <= 0.0f) {
        return false;
    }

    rideFile->setStartTime(QDateTime(date, time));
    rideFile->setRecIntSecs(recordingInterval);

    return true;
}

struct RideFilePoint readSinglePoint(const QByteArray& bytes,
                                     const qsizetype recordOffset,
                                     const double timeInSeconds,
                                     const double startDistance)
{
    const char *record = bytes.constData() + recordOffset;
    const double distance = readFloat(record);
    const double relativeDistance = distance - startDistance;

    const quint8 heartRate = static_cast<quint8>(record[4]);
    const quint8 cadence = static_cast<quint8>(record[5]);
    const quint16 powerX10 = readUnsignedShort(record + 6);
    const double power = powerX10 / 10.0;

    const quint16 speedX10 = readUnsignedShort(record + 8);
    const double speed = speedX10 / 10.0;

    struct RideFilePoint point(timeInSeconds, cadence, heartRate, relativeDistance / 1000.0, speed, 0.0, power,
                               0.0, 0.0, 0.0, 0.0, 0.0,
                               RideFile::NA, RideFile::NA,
                               0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0,
                               0);

    return point;
}

bool readRideData(RideFile *rideFile, const QByteArray& bytes,
                  const qsizetype offset, const qsizetype size,
                  const qint32 nrOfRecords, const qint32 recordSize)
{
    const qint64 requiredSize64 =
        static_cast<qint64>(nrOfRecords) * static_cast<qint64>(recordSize);
    if (!rideFile || nrOfRecords <= 0 || recordSize < 10
        || requiredSize64 != static_cast<qint64>(size)
        || offset < 0 || offset > bytes.size() || size > bytes.size() - offset) {
        return false;
    }

    const double recordingInterval = rideFile->recIntSecs();
    if (!qIsFinite(recordingInterval) || recordingInterval <= 0.0)
        return false;

    const double startDistance = readFloat(bytes.constData() + offset);
    if (!qIsFinite(startDistance))
        return false;

    for (qint32 i = 0; i < nrOfRecords; ++i) {
        const qint64 recordOffset64 =
            static_cast<qint64>(i) * static_cast<qint64>(recordSize);
        const qsizetype recordOffset =
            offset + static_cast<qsizetype>(recordOffset64);
        const double distance = readFloat(bytes.constData() + recordOffset);
        const double seconds = recordingInterval * (static_cast<double>(i) + 1.0);
        if (!qIsFinite(distance) || !qIsFinite(seconds))
            return false;

        struct RideFilePoint nextDataPoint = readSinglePoint(
            bytes, recordOffset, seconds, startDistance);

        rideFile->appendPoint(nextDataPoint.secs, nextDataPoint.cad,
                              nextDataPoint.hr, nextDataPoint.km,
                              nextDataPoint.kph, nextDataPoint.nm,
                              nextDataPoint.watts, nextDataPoint.alt,
                              nextDataPoint.lon, nextDataPoint.lat,
                              nextDataPoint.headwind, 0.0, RideFile::NA, 0.0,
                              0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                              0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0, nextDataPoint.interval);
    }
    return true;
}

qint16 readShort(const char *bytes)
{
    return qFromLittleEndian<qint16>(
        reinterpret_cast<const uchar *>(bytes));
}

quint16 readUnsignedShort(const char *bytes)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes));
}

qint32 readInt(const char *bytes)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(bytes));
}

float readFloat(const char *bytes)
{
    const quint32 bits = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes));
    float value;
    static_assert(sizeof(value) == sizeof(bits), "Unexpected float size");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
