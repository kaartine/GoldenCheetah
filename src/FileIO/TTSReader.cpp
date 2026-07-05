/*
* Copyright (c) 2020 Eric Christoffersen (impolexg@outlook.com)
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

// Data reader adapted to C++ from Wattzap Community Edition Java source code.

#include "TTSReader.h"
#include "LocationInterpolation.h"

#include <QtEndian>
#include <QIODevice>

#include <algorithm>
#include <map>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

// -------------------------------------------------------------
// LOG CONTROL
//
// Key is that it all goes away if disabled and optimized build.
// -------------------------------------------------------------

// Uncomment for debug logging
//#define DEBUG_PRINT

// Uncomment this too for more debug logging
//#define DEBUG_PRINT_VERBOSE

class log_disabled_output {};
static log_disabled_output log_disabled_output_instance;

template<typename T>
const log_disabled_output& operator << (const log_disabled_output& any, T const& thing) { Q_UNUSED(thing); return any; }

// std::endl simple, quick and dirty
const log_disabled_output& operator << (const log_disabled_output& any, const std::ostream&(*)(std::ostream&)) { return any; }

#ifdef DEBUG_PRINT
#include <QDebug>
#define DEBUG_LOG qDebug()
#else
#define DEBUG_LOG log_disabled_output_instance
#endif

#ifdef VERBOSE_DEBUG_PRINT
#define DEBUG_LOG_VERBOSE DEBUG_LOG
#else
#define DEBUG_LOG_VERBOSE log_disabled_output_instance
#endif

// -------------------------------------------------------------
// END LOG CONTROL
// -------------------------------------------------------------

using namespace NS_TTSReader;

namespace {

constexpr std::uint64_t kTtsHeaderBytes = 14;
constexpr int kTtsUtf16StringBlockType = 110;
constexpr std::uint64_t kMaxTtsUtf16PayloadBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxTtsBlockBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxTtsPayloadBytes = 512ULL * 1024ULL * 1024ULL;
constexpr size_t kMaxTtsBlocks = 65'536;

// Two million frame mappings cover more than 18 hours at 30 Hz.
constexpr std::uint64_t kMaxTtsDecodedRecords = 2'000'000;
// Includes payloads, decoded streams, final points, and the chart series.
constexpr std::uint64_t kMaxTtsWorkingSetBytes = 320ULL * 1024ULL * 1024ULL;

bool checkedPayloadSize(std::uint32_t elementSize,
                        std::uint32_t elementCount,
                        std::uint64_t &payloadSize)
{
    if (elementCount != 0
        && elementSize > std::numeric_limits<std::uint64_t>::max() / elementCount)
        return false;

    payloadSize = static_cast<std::uint64_t>(elementSize) * elementCount;
    return payloadSize <= kMaxTtsBlockBytes
        && payloadSize <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
}

bool checkedAdd(std::uint64_t amount, std::uint64_t &total)
{
    if (amount > std::numeric_limits<std::uint64_t>::max() - total)
        return false;

    total += amount;
    return true;
}

bool addEstimatedStorage(std::uint64_t count, std::uint64_t elementSize,
                         std::uint64_t &total)
{
    if (count != 0
        && elementSize > std::numeric_limits<std::uint64_t>::max() / count) {
        return false;
    }

    return checkedAdd(count * elementSize, total);
}

} // namespace

// This is the static const string table that tts files may refer to by string key.
static const std::map<int, const char*> tts_string_table = {
    {1001, "route name" },
    {1002, "route description"},
    {1041, "segment name"},
    {1042, "segment description"},
    {2001, "company"},
    {2004, "serial"},
    {2005, "time"},
    {2007, "link"},
    {5001, "product"},
    {5002, "video name"},
    {6001, "infobox #1"}
};

/*
 * Utility Routines For TTS Reader
 */

 // Handy utils
float AsFloat(std::uint32_t u) {
    float t;
    static_assert(sizeof(u) == sizeof(t), "Error: mismatched reinterpret sizes");
    memcpy(&t, &u, sizeof(u));
    return t;
}

int toUInt(Byte b) {
    return static_cast<UByte>(b);
}

int getUShort(const ByteArray &buffer, size_t offset) {
    if (offset > buffer.size() || buffer.size() - offset < sizeof(quint16))
        return 0;
    return qFromLittleEndian<quint16>(buffer.data() + offset);
}

std::uint32_t getUInt(const ByteArray &buffer, size_t offset) {
    if (offset > buffer.size() || buffer.size() - offset < sizeof(quint32))
        return 0;
    return qFromLittleEndian<quint32>(buffer.data() + offset);
}

bool isHeader(ByteArray &buffer) {
    if (buffer.size() < 2) {
        return false;
    }
    return getUShort(buffer, 0) <= 20;
}

void rehashKey(IntArray &A_0, int A_1, IntArray &outArray) {
    unsigned int i;
    CharArray chArray1;
    chArray1.resize(A_0.size() / 2);

    for (i = 0; i < chArray1.size(); i++) {
        chArray1[i] = (Char)(A_0[2 * i] + 256 * A_0[2 * i + 1]);
    }

    int num1 = 1000170181 + A_1;
    int num2 = 0;
    //int num3 = 1; ?
    while ((unsigned)num2 < chArray1.size()) {
        int index1 = num2;
        CharArray &chArray2 = chArray1;
        int index2 = index1;
        int num4 = (int)(short)chArray1[index1];
        int num5 = (int)255;
        int num6 = num4 & num5;
        int num7 = num1;
        int num8 = 1;
        int num9 = num7 + num8;
        Byte num10 = (Byte)(num6 ^ num7);
        int num11 = 8;
        int num12 = num4 >> num11;
        int num13 = num9;
        int num14 = 1;
        num1 = num13 + num14;
        int num15 = (int)(Byte)(num12 ^ num13);
        int num16 = (int)(toUInt(num10) << 8 | toUInt((Byte)num15)) & 0xffff;
        chArray2[index2] = (Char)num16;
        int num17 = 1;
        num2 += num17;
    }

    outArray.resize(chArray1.size());

    for (i = 0; i < outArray.size(); i++) {
        outArray[i] = (int)chArray1[i];
    }

    return;
}

bool encryptHeader(const IntArray &A_0, const IntArray &key2, IntArray &numArray) {

    const IntArray &bytes = key2;
    numArray.resize(bytes.size());

    unsigned int index1 = 0;
    unsigned int index2 = 0;
    unsigned int num = 6;

    while (true) {
        switch (num) {
        case 0:
            index1 = 0;
            num = 2;
            continue;
        case 1:
            if (index2 < bytes.size()) {
                numArray[index2] = (A_0[index1] ^ bytes[index2]);
                num = 5;
                continue;
            }
            else {
                num = 3;
                continue;
            }
        case 2:
            ++index2;
            num = 4;
            continue;
        case 3:
            return true;
        case 4:
        case 6:
            num = 1;
            continue;
        case 5:
            if (index1++ >= A_0.size() - 1) {
                num = 0;
                continue;
            }
            else {
                num = 2;
                continue;
            }
        default:
            return false;
        }
    }
}

bool decryptData(ByteArray &data, const IntArray &key)
{
    if (key.empty())
        return data.empty();

    for (size_t i = 0; i < data.size(); ++i) {
        const UByte encrypted = static_cast<UByte>(data[i]);
        const UByte keyByte = static_cast<UByte>(key[i % key.size()]);
        data[i] = static_cast<Byte>(encrypted ^ keyByte);
    }

    return true;
}

bool decodeUtf16String(const ByteArray &data, std::wstring &target)
{
    if (data.size() % 2 != 0
        || data.size() > kMaxTtsUtf16PayloadBytes) {
        return false;
    }

    const size_t characterCount = data.size() / 2;

    // Release an overwritten value before allocating its replacement.
    std::wstring().swap(target);
    target.reserve(characterCount);

    const size_t allowedCapacity = std::max<size_t>(32, 2 * characterCount);
    if (target.capacity() > allowedCapacity) {
        std::wstring().swap(target);
        return false;
    }

    target.resize(characterCount);

    for (size_t i = 0; i < characterCount; ++i) {
        target[i] =
            static_cast<wchar_t>(static_cast<UByte>(data[2 * i]))
            | static_cast<wchar_t>(
                static_cast<UByte>(data[2 * i + 1]) << 8);
    }

    return true;
}

void iarr(const ByteArray &a, IntArray &r) {
    r.resize(0);
    for (unsigned int i = 0; i < a.size(); i++) {
        r.push_back((int)a[i]);
    }
}

void videoInfo(int version, ByteArray & data) {
    // first three values could be shorts and then?
    Q_UNUSED(version);
    Q_UNUSED(data);
}

void TTSReader::segmentRange(int version, ByteArray &data) {
    if (!hasValidBlockSize(SEGMENT_RANGE, version, data.size())) {
        DEBUG_LOG << "Segment Range data wrong length " << data.size() << "\n";
        return;
    }

    const size_t rangeCount = data.size() / 10;
    if (segmentRanges.size() > kMaxTtsDecodedRecords
        || rangeCount > kMaxTtsDecodedRecords - segmentRanges.size()) {
        return;
    }

    segmentRanges.reserve(segmentRanges.size() + rangeCount);

    DEBUG_LOG << "[segment range token]";

    // In the files I've seen this segmentRange token is only set once and contains the route length.
    // Note this code seems to permit a segment to have multiple ranges. I have no example so not sure.
    for (size_t i = 0; i < rangeCount; i++) {
        const size_t offset = i * 10;
        const std::uint32_t startCM = getUInt(data, offset);
        const std::uint32_t endCM = getUInt(data, offset + 4);
        const std::uint16_t metadata = getUShort(data, offset + 8);

        const double startKM = startCM / 100000.0;
        const double endKM = endCM / 100000.0;

        segmentRanges.push_back({startKM, endKM, metadata});

        DEBUG_LOG << " [" << i << "=" << startKM << "-" << endKM << "\n";

        if (metadata != 0) {
            DEBUG_LOG << "/0x" << std::hex << metadata << std::dec << "\n";
        }

        DEBUG_LOG << "]\n";
    }
}

// segment range; 548300 is 5.483km. What is short value in "old" files?
bool TTSReader::segmentInfo(int version, ByteArray &data) {

    if (!hasValidBlockSize(SEGMENT_INFO, version, data.size()))
        return false;

    if ((version == 1104) && (data.size() == 8)) {
        unsigned startCM = getUInt(data, 0);
        unsigned endCM   = getUInt(data, 4);

        double startKM = (startCM / 100) / 1000.;
        double endKM   = (endCM   / 100) / 1000.;

        if (!std::isfinite(startKM) || !std::isfinite(endKM))
            return false;

        pendingSegment.startKM = startKM;
        pendingSegment.endKM   = endKM;

        DEBUG_LOG << "[segment range] " << startKM << "-" << endKM << "\n";
    }

    if ((version == 1000) && (data.size() == 10)) {
        unsigned startCM = getUInt(data, 2);
        unsigned endCM   = getUInt(data, 6);

        // NOTE: From the wattzapp debug print it looks like this 3rd value is a divisor.
        // In my test files its always 1. so no harm to divide - but beware I'm just guessing.
        const std::uint16_t divisor = getUShort(data, 0);
        if (divisor == 0)
            return false;

        double startKM = ((startCM / 100) / 1000.) / divisor;
        double endKM   = ((endCM   / 100) / 1000.) / divisor;

        if (!std::isfinite(startKM) || !std::isfinite(endKM))
            return false;

        pendingSegment.startKM = startKM;
        pendingSegment.endKM   = endKM;

        DEBUG_LOG << "[segment range] " << (getUInt(data, 2) / 100000.0) << "-" << (getUInt(data, 6) / 100000.0) << "/" << getUShort(data, 0) << "\n";
    }

    return true;
}

// 1 for "plain" RLV, 2 for ERGOs
void trainingType(int version, ByteArray &data) {

    if (version == 1004) {
        if (data.size() < 6)
            return;

        switch (data[5]) {
        case 1:
            DEBUG_LOG << "[video type] RLV\n";
            break; // ?
        case 2:
            DEBUG_LOG << "[video type] ERGO\n";
            break; // ?
        }
    }
}

void Point::print() const {
    DEBUG_LOG << "Point [latitude=" << latitude << ", longitude=" << longitude
              << ", elevation=" << elevation << ", distanceFromStart="
              << distanceFromStart << ", gradient=" << gradient << ", power="
              << power << ", speed=" << speed << ", time=" << time << "]\n";
}

//
// TTS Reader Method Impls
//

bool TTSReader::readData(QDataStream &is, ByteArray& buffer, bool copyPre) {
    size_t first = 0;
    if (copyPre) {
        if (buffer.size() < 2 || pre.size() < 2)
            return false;

        buffer[0] = pre[0];
        buffer[1] = pre[1];
        first = 2;
    }

    if (buffer.size() != first) {
        const size_t readSize = buffer.size() - first;
        if (readSize > static_cast<size_t>(std::numeric_limits<int>::max()))
            return false;

        const int bytesRead = is.readRawData(
            reinterpret_cast<char *>(&buffer[first]),
            static_cast<int>(readSize));
        if (bytesRead < 0 || static_cast<size_t>(bytesRead) != readSize)
            return false;
    }

    return true;
}

const XYSeries &TTSReader::getSeries() const {

    // TODO Auto-generated method stub
    return series;
}

const std::vector<Point> & TTSReader::getPoints() const {
    return points;
}

const std::vector<Segment>& TTSReader::getSegments() const {
    return segmentList;
}

const std::vector<SegmentRange>& TTSReader::getSegmentRanges() const {
    return segmentRanges;
}

const std::wstring& TTSReader::getRouteName()        const {
    return routeName; 
}

const std::wstring& TTSReader::getRouteDescription() const {
    return routeDescription;
}

double TTSReader::getDistanceMeters() const {
    return totalDistance;
}

int TTSReader::routeType() const {
    return 0;// SLOPE;
}

double TTSReader::getMaxSlope() const {
    return maxSlope;
}

double TTSReader::getMinSlope() const {
    return minSlope;
}

bool TTSReader::deriveMinMaxSlopes(double &minSlope, double &maxSlope, double &variance) const {

    minSlope = 0;
    maxSlope = 0;

    double varianceSum = 0;

    if (fHasAlt) {
        for (unsigned u = 3; u < points.size(); u++) {
            double rise = points[u].getElevation() - points[u - 1].getElevation();
            double run = points[u].getDistanceFromStart() - points[u - 1].getDistanceFromStart();
            double s = run ? rise / run : 0;
            minSlope = std::min(s, minSlope);
            maxSlope = std::max(s, maxSlope);

            double deltaFromRecorded = run * (s - points[u].getGradient());

            varianceSum += (deltaFromRecorded * deltaFromRecorded);
        }

        variance = varianceSum / ((points.size() - 1) * points[points.size() - 1].getDistanceFromStart());
    }

    return true;
}

void TTSReader::recomputeAltitudeFromGradient() {

    if (points.size() < 3)
        return;

    // Some tts files I see have an altitude crisis around the start: Fixup altitude
    // of index 0 and 1 by linear interpolating.
    double d2d1delta = points[2].getDistanceFromStart() - points[1].getDistanceFromStart();
    points[1].setElevation(points[2].getElevation() - (0.01 * points[1].getGradient()*d2d1delta));
    points[0].setElevation(points[1].getElevation() - (0.01 * points[0].getGradient()*d2d1delta));

    // Wish there was a fix... TTS Files with no altitude will start at elevation 0...
    double alt = points[0].getElevation();

    for (unsigned u = 1; u < points.size(); u++) {

        const Point &curr = points[u];
        const Point &prev = points[u - 1];

        double gradient = prev.getGradient() / 100.;

        double hyp = curr.getDistanceFromStart() - prev.getDistanceFromStart();
        double run = hyp / sqrt(gradient * gradient + 1); // hyp*cos(atan(gradient))

        double rise = gradient * run;

        alt += rise;

        points[u].setElevation(alt);
    }

    // Even if tts file started without altitude, now that it is generated
    // we will claim to have it.
    fHasAlt = true;
}

bool TTSReader::parseFile(QDataStream &file) {
    try {
        TTSReader parsed;
        if (!parsed.parseFileContents(file))
            return false;

        *this = std::move(parsed);
        return true;
    } catch (const std::bad_alloc &) {
        DEBUG_LOG << "Cannot allocate memory while parsing TTS data\n";
        return false;
    } catch (const std::length_error &) {
        DEBUG_LOG << "Invalid TTS allocation size\n";
        return false;
    }
}

bool TTSReader::parseFileContents(QDataStream &file) {
    pre.resize(2);
    std::uint64_t totalPayloadBytes = 0;
    size_t blockCount = 0;
    std::uint64_t frameRecordCount = 0;
    std::uint64_t gpsRecordCount = 0;
    std::uint64_t programRecordCount = 0;
    std::uint64_t segmentRangeCount = 0;
    std::uint64_t stringPayloadBytes = 0;

    const auto addRecords = [](std::uint64_t payloadBytes,
                               std::uint64_t recordSize,
                               std::uint64_t &totalRecords) {
        const std::uint64_t records = payloadBytes / recordSize;
        if (records > kMaxTtsDecodedRecords
            || totalRecords > kMaxTtsDecodedRecords - records) {
            return false;
        }

        totalRecords += records;
        return true;
    };

    const auto withinWorkingSetBudget = [&]() {
        std::uint64_t estimatedBytes = totalPayloadBytes;
        const std::uint64_t basisRecords =
            std::max({frameRecordCount, gpsRecordCount, programRecordCount});

        if (!addEstimatedStorage(frameRecordCount, sizeof(Point), estimatedBytes)
            || !addEstimatedStorage(gpsRecordCount, sizeof(GPSPoint), estimatedBytes)
            || !addEstimatedStorage(programRecordCount, sizeof(ProgramPoint), estimatedBytes)
            || !addEstimatedStorage(segmentRangeCount, sizeof(SegmentRange), estimatedBytes)
            || !addEstimatedStorage(basisRecords, sizeof(Point), estimatedBytes)
            || !addEstimatedStorage(basisRecords, sizeof(Pair), estimatedBytes)) {
            return false;
        }

        // Covers two simultaneously live strings at up to twice their size.
        const std::uint64_t stringCharacters = (stringPayloadBytes + 1) / 2;
        if (!addEstimatedStorage(stringCharacters, 4 * sizeof(wchar_t),
                                 estimatedBytes)) {
            return false;
        }

        return estimatedBytes <= kMaxTtsWorkingSetBytes;
    };

    for (;;) {
        const int prefixBytes = file.readRawData(pre.data(), 2);
        if (prefixBytes == 0)
            break;
        if (prefixBytes != 2) {
            DEBUG_LOG << "Truncated TTS header prefix\n";
            return false;
        }

        if (!isHeader(pre)) {
            DEBUG_LOG << "Data encountered where a TTS header was required\n";
            return false;
        }

        if (blockCount >= kMaxTtsBlocks) {
            DEBUG_LOG << "Too many TTS blocks\n";
            return false;
        }

        ByteArray header(static_cast<size_t>(kTtsHeaderBytes));
        if (!readData(file, header, true)) {
            DEBUG_LOG << "Cannot read complete TTS header\n";
            return false;
        }

        std::uint64_t payloadSize = 0;
        if (!checkedPayloadSize(getUInt(header, 6), getUInt(header, 10),
                                payloadSize)) {
            DEBUG_LOG << "Invalid TTS payload dimensions\n";
            return false;
        }

        const int blockType = getUShort(header, 2);
        const int version = getUShort(header, 4);
        if (!hasValidBlockSize(blockType, version,
                               static_cast<size_t>(payloadSize))) {
            DEBUG_LOG << "Invalid TTS record dimensions\n";
            return false;
        }

        if (!checkedAdd(payloadSize, totalPayloadBytes)
            || totalPayloadBytes > kMaxTtsPayloadBytes) {
            DEBUG_LOG << "TTS payload exceeds total size limit\n";
            return false;
        }

        bool decodedShapeAccepted = true;
        switch (blockType) {
        case DISTANCE_FRAME:
            decodedShapeAccepted =
                addRecords(payloadSize, 8, frameRecordCount);
            break;
        case GPS_DATA:
            decodedShapeAccepted =
                addRecords(payloadSize, 16, gpsRecordCount);
            break;
        case PROGRAM_DATA:
            decodedShapeAccepted =
                addRecords(payloadSize, 6, programRecordCount);
            if (decodedShapeAccepted) {
                ++programRecordCount;
                decodedShapeAccepted =
                    programRecordCount <= kMaxTtsDecodedRecords;
            }
            break;
        case SEGMENT_RANGE:
            decodedShapeAccepted =
                addRecords(payloadSize, 10, segmentRangeCount);
            break;
        case kTtsUtf16StringBlockType:
            decodedShapeAccepted =
                checkedAdd(payloadSize, stringPayloadBytes);
            break;
        default:
            break;
        }

        if (!decodedShapeAccepted || !withinWorkingSetBudget()) {
            DEBUG_LOG << "TTS decoded data exceeds memory limits\n";
            return false;
        }

        QIODevice *device = file.device();
        if (device && !device->isSequential()) {
            const qint64 remaining = device->size() - device->pos();
            if (remaining < 0
                || payloadSize > static_cast<std::uint64_t>(remaining)) {
                DEBUG_LOG << "TTS payload extends past end of input\n";
                return false;
            }
        }

        ByteArray data(static_cast<size_t>(payloadSize));
        if (!readData(file, data, false)) {
            DEBUG_LOG << "Cannot read complete TTS payload\n";
            return false;
        }

        content.push_back(std::move(header));
        content.push_back(std::move(data));
        ++blockCount;
    }

    if (!loadHeaders())
        return false;

    // Clean up passes.

    // Recompute all altitude data using the recorded gradient.
    // This is necessary because generally the altitude data in tts
    // files is far far too noisy to use to compute slope. For example
    // see IT_LOMBARDY08.TTS.
    // Benefit of this is that altitude data now reflects slope
    // recorded by Tacx, so you won't be climbing a huge pass
    // without putting in the right amount of effort.
    recomputeAltitudeFromGradient();

    return true;
}

bool TTSReader::loadHeaders() {

    static const unsigned char s_key[] = { 0xD6, 0x9C, 0xD8, 0xBC, 0xDA, 0xA9, 0xDC,
                                           0xB0, 0xDE, 0xB6, 0xE0, 0x95, 0xE2, 0xC3, 0xE4, 0x97, 0xE6, 0x92,
                                           0xE8, 0x85, 0xEA, 0x8E, 0xEC, 0x9E, 0xEE, 0x91, 0xF0, 0xB1, 0xF2,
                                           0xD2, 0xF4, 0xD4, 0xF6, 0xD7, 0xF8, 0xB1, 0xFA, 0x9E, 0xFC, 0xDD,
                                           0xFE, 0x96, 0x00, 0x72, 0x02, 0x23, 0x04, 0x71, 0x06, 0x6F, 0x08,
                                           0x6C, 0x0A, 0x2B, 0x0C, 0x7E, 0x0E, 0x4E, 0x10, 0x67, 0x12, 0x7A,
                                           0x14, 0x7A, 0x16, 0x65, 0x18, 0x39, 0x1A, 0x74, 0x1C, 0x7B, 0x1E,
                                           0x3F, 0x20, 0x44, 0x22, 0x75, 0x24, 0x40, 0x26, 0x55, 0x28, 0x50,
                                           0x2A, 0x0B, 0x2C, 0x18, 0x2E, 0x19, 0x30, 0x08, 0x32, 0x0B, 0x34,
                                           0x02, 0x36, 0x1F, 0x38, 0x10, 0x3A, 0x1F, 0x3C, 0x17, 0x3E, 0x17,
                                           0x40, 0x62, 0x42, 0x65, 0x44, 0x65, 0x46, 0x6E, 0x48, 0x69, 0x4A,
                                           0x25, 0x4C, 0x28, 0x4E, 0x2A, 0x50, 0x35, 0x52, 0x36, 0x54, 0x31,
                                           0x56, 0x77, 0x58, 0x09, 0x5A, 0x29, 0x5C, 0x6D, 0x5E, 0x2B, 0x60,
                                           0x52, 0x62, 0x00, 0x64, 0x31, 0x66, 0x2E, 0x68, 0x26, 0x6A, 0x25,
                                           0x6C, 0x4D, 0x6E, 0x3C, 0x70, 0x28, 0x72, 0x20, 0x74, 0x21, 0x76,
                                           0x32, 0x78, 0x34, 0x7A, 0x55, 0x7C, 0x37, 0x7E, 0x0A, 0x80, 0xF2,
                                           0x82, 0xF7, 0x84, 0xC7, 0x86, 0xC2, 0x88, 0xDA, 0x8A, 0xDE, 0x8C,
                                           0xDF, 0x8E, 0xCA, 0x90, 0xE5, 0x92, 0xFC, 0x94, 0xB0, 0x96, 0xC9,
                                           0x98, 0xF4, 0x9A, 0xAF, 0x9C, 0xF6, 0x9E, 0xFA, 0xA0, 0xD5, 0xA2,
                                           0xCB, 0xA4, 0xCC, 0xA6, 0xD4, 0xA8, 0x83, 0xAA, 0x8F, 0xAC, 0xD9,
                                           0xAE, 0xDD, 0xB0, 0xD8, 0xB2, 0xDD, 0xB4, 0xD2, 0xB6, 0xDB, 0xB8,
                                           0xE6, 0xBA, 0xE4, 0xBC, 0xE2, 0xBE, 0xE0, 0xC0, 0xAF, 0xC2, 0xA4,
                                           0xC4, 0xE2, 0xC6, 0xA9, 0xC8, 0xBC, 0xCA, 0xAD, 0xCC, 0xAB, 0xCE,
                                           0xEE };

    IntArray iakey;
    for (unsigned u = 0; u < (sizeof(s_key) / sizeof(s_key[0])); u++)
        iakey.push_back(s_key[u]);

    IntArray key2;
    rehashKey(iakey, 17, key2);

    IntArray keyH;

    int blockType = -1;
    int version   = -1;
    int stringId  = -1;

    StringType stringType = StringType::BLOCK;

    //int fingerprint = 0; // not used

    int bytes = 0;

    if (content.size() % 2 != 0)
        return false;

    for (size_t cu = 0; cu < content.size(); cu++)
    {
        ByteArray &data = content[cu];

        if (cu % 2 == 0) {
            if (data.size() != kTtsHeaderBytes || !isHeader(data))
                return false;

            DEBUG_LOG << bytes << " [" << std::hex << (bytes) << std::dec << "]: "
                << getUShort(data, 0) << "." << getUShort(data, 2) << " v"
                << getUShort(data, 4) << " " << getUInt(data, 6) << "x"
                << getUInt(data, 10) << "\n";

            //fingerprint = getUInt(data, 6);

            IntArray ia;
            iarr(data, ia);
            bool success = encryptHeader(ia, key2, keyH);
            if (!success)
                return false;

            stringType = StringType::NONPRINTABLE;

            switch (getUShort(data, 2)) {

            case 10: // crc of the data?

                // I don't know how to compute it.. and to which data it
                // belongs..
                // for sure I'm not going to check these, I assume file is
                // not broken
                // (why it can be?)
                stringType = StringType::CRC;
                break;

            case kTtsUtf16StringBlockType:

                stringType = StringType::STRING;
                stringId = getUShort(data, 0);
                break;

            case 120: // image fingerprint

                stringId = getUShort(data, 0) + 1000;
                break;

            case 121: // imageType? always 01
                break;

            case 122: // image bytes, name is present in previous string
                      // from the block
                stringType = StringType::IMAGE;
                break;

            default:

                stringType = StringType::BLOCK;
                blockType = getUShort(data, 2);
                version = getUShort(data, 4);

                DEBUG_LOG << "\nblock type " << blockType << " version "
                          << version << "\n";

                stringId = -1;
                break;
            }
        }
        else {
            if (!decryptData(data, keyH))
                return false;

            DEBUG_LOG << "::";

            switch (stringType) {

            case CRC:

                break;

            case IMAGE:

                //DEBUG_LOG << "[image " << blockType << "." << (stringId - 1000) + "]";

                /*
                 * try { result = currentFile + "." + (imageId++) + ".png";
                 * FileOutputStream file = new FileOutputStream(result);
                 * file.write(data); file.close(); } catch
                 * (IOException e) { result = "cannot create: " + e; }
                 */

                break;

            case STRING:

            {
                const int decodedStringId = blockType + stringId;
                if (tts_string_table.count(decodedStringId)) {
                    DEBUG_LOG << "[" << tts_string_table.at(decodedStringId) << "]";
                } else {
                    DEBUG_LOG << "[" << blockType << "." << stringId << "]";
                }

                std::wstring *target = nullptr;
                switch (decodedStringId) {
                case 5002: // Video Name
                    target = &ttsName;
                    break;
                case 1001: // Route Name
                    target = &routeName;
                    break;
                case 1002: // Route Description
                    target = &routeDescription;
                    break;
                case 1041: // Segment Name
                    // A new segment name closes the previous pending segment.
                    flushPendingSegment();
                    target = &pendingSegment.name;
                    break;
                case 1042: // Segment Description
                    target = &pendingSegment.description;
                    break;
                }

                if (target) {
                    if (!decodeUtf16String(data, *target))
                        return false;
                    DEBUG_LOG << "[" << *target << "]";
                }
            }
            break;

            case BLOCK:
            {
                if (!blockProcessing(blockType, version, data))
                    return false;
            }
            default:
            break;
            }
        }

        bytes += (int)data.size();
    }

    // At this... point...
    // - pointList holds framemapping info (if any)
    // - programList holds slope info
    // - gpsList holds gps info
    // - segmentList holds the segments and their names
    //
    // GPS Info is optional.
    // FrameMapping Info is optional.
    //
    // If this is a slope program then slope info is NOT optional.
    //
    // So we need to decide what is our base list.
    //
    // WattZap implementation based everything on framemapping info,
    // which doesn't work when framemapping isn't in the tts.
    //
    // One stream is chosen to be the basis for the interpolation of
    // the other streams. Choose whichever has the most data points,
    // then interpolate the other two onto it.

    // First thing, flush any pending segment.
    flushPendingSegment();

    size_t gpsCount = gpsPoints.size();
    size_t slopeCount = programList.size();
    size_t frameMapCount = pointList.size();

    enum Basis { NoBasis, FrameMap, GPS, Slope };

    Basis basis = NoBasis;

    if (frameMapCount == 0 && gpsCount == 0 && slopeCount == 0) {
        return false;
    }

    // Copy basis stream onto points[].
    if (frameMapCount > gpsCount && frameMapCount > slopeCount)
    {
        points = std::move(pointList);
        basis = FrameMap;
    } else if (gpsCount >= slopeCount) {
        // GpsList basis
        points.reserve(gpsCount);
        for (size_t i = 0; i < gpsCount; i++) {
            Point p;

            p.setDistanceFromStart(gpsPoints[i].distance);
            p.setLatitude(gpsPoints[i].lat);
            p.setLongitude(gpsPoints[i].lon);
            p.setElevation(gpsPoints[i].alt);

            points.push_back(p);
        }
        basis = GPS;
    } else {
        // slope basis
        points.reserve(slopeCount);
        for (size_t i = 0; i < slopeCount; i++) {
            Point p;

            p.setDistanceFromStart(programList[i].distance);
            p.setGradient(programList[i].slope);

            points.push_back(p);
        }
        fHasSlope = true;
        basis = Slope;
    }

    if (basis == NoBasis) {
        return false;
    }

    GeoPointInterpolator gpi; // geo interpolation

    // Interpolate non-geo data in xyz.
    DistancePointInterpolator<LinearTwoPointInterpolator> ti;
    DistancePointInterpolator<LinearTwoPointInterpolator> si;

    // Interpolate non-basis streams onto points[].
    size_t frameMapIdx = 0;
    size_t gpsIdx      = 0;
    size_t slopeIdx    = 0;

    for (unsigned long i = 0; i < points.size(); i++) {

        fHasKM = true;

        Point &p = points[i];

        // Interpolate framemap
        if (basis != FrameMap && frameMapCount) {

            while (ti.WantsInput(p.getDistanceFromStart())) {
                if (frameMapIdx >= frameMapCount) {
                    ti.NotifyInputComplete();
                    break;
                }
                ti.Push(pointList[frameMapIdx].getDistanceFromStart(),
                        xyz(pointList[frameMapIdx].getTime(), 0, 0));
                frameMapIdx++;
            }

            xyz interp = ti.Location(p.getDistanceFromStart());
            double time = interp.x();

            p.setTime(time);

            fHasFrameMapping = true;
        }

        // Interpolate gps location
        if (basis != GPS && gpsCount) {
            while (gpi.WantsInput(p.getDistanceFromStart())) {
                if (gpsIdx >= gpsCount) {
                    gpi.NotifyInputComplete();
                    break;
                }
                gpi.Push(gpsPoints[gpsIdx].distance,
                         geolocation(gpsPoints[gpsIdx].lat,
                                     gpsPoints[gpsIdx].lon,
                                     gpsPoints[gpsIdx].alt));
                gpsIdx++;
            }

            geolocation geoloc = gpi.Location(p.getDistanceFromStart());

            p.setLatitude (geoloc.Lat());
            p.setLongitude(geoloc.Long());
            p.setElevation(geoloc.Alt());

            fHasGPS = true;
            fHasAlt = true;
        }

        // Interpolate gradient
        if (basis != Slope && slopeCount) {

            while (si.WantsInput(p.getDistanceFromStart())) {
                if (slopeIdx >= slopeCount) {
                    si.NotifyInputComplete();
                    break;
                }
                si.Push(programList[slopeIdx].distance,
                        xyz(programList[slopeIdx].slope, 0, 0));
                slopeIdx++;
            }

            xyz interp = si.Location(p.getDistanceFromStart());
            double slope = interp.x();

            p.setGradient(slope);

            fHasSlope = true;
        }
    }

    // Convet units for distance and time. Compute speed.
    series.reserve(points.size());
    Point *pPrevPoint = &(points[0]);

    unsigned int pointCount = 0;
    while (pointCount < points.size()) {
        Point &p = points[pointCount];

        // turn into meters
        p.setDistanceFromStart(p.getDistanceFromStart() / 100);

        // convert to mS
        p.setTime(p.getTime() * 1000);

        if (!std::isfinite(p.getDistanceFromStart())
            || !std::isfinite(p.getTime())
            || !std::isfinite(p.getLatitude())
            || !std::isfinite(p.getLongitude())
            || !std::isfinite(p.getElevation())
            || !std::isfinite(p.getGradient())
            || !std::isfinite(p.getPower())) {
            return false;
        }

        // speed = d/t
        if (pointCount > 0) {
            const double distanceDelta = p.getDistanceFromStart()
                - pPrevPoint->getDistanceFromStart();
            const double timeDelta = p.getTime() - pPrevPoint->getTime();
            double speed = 0;

            if (std::isfinite(distanceDelta) && std::isfinite(timeDelta)
                && timeDelta > 0) {
                speed = 3600 * distanceDelta / timeDelta;
                if (!std::isfinite(speed))
                    speed = 0;
            }

            p.setSpeed(speed);
        }

        pPrevPoint = &(points[pointCount]);

        // meters / meters
        p.print();

        series.push_back({ p.getDistanceFromStart() / 1000, p.getElevation() });

        pointCount++;
    }// while

    // fill in speed for first point

    totalDistance = 0;
    if (points.size() > 2) {
        points[0].setSpeed(points[1].getSpeed());
        totalDistance = points[points.size() - 1].getDistanceFromStart();
    }

    return true;
}

bool TTSReader::hasValidBlockSize(int blockType, int version, size_t size) {
    const auto hasCompleteRecords = [size](size_t recordSize,
                                            bool allowEmpty = false) {
        return (allowEmpty || size != 0)
            && size % recordSize == 0
            && size / recordSize <= kMaxTtsDecodedRecords;
    };

    switch (blockType) {
    case kTtsUtf16StringBlockType:
        return size % 2 == 0 && size <= kMaxTtsUtf16PayloadBytes;
    case PROGRAM_DATA:
        return hasCompleteRecords(6);

    // GPS and frame mapping are optional streams. Segment ranges are optional
    // metadata. Empty blocks for these record types are valid no-ops.
    case DISTANCE_FRAME:
        return hasCompleteRecords(8, true);
    case GPS_DATA:
        return hasCompleteRecords(16, true);
    case GENERAL_INFO:
        return size >= 2;
    case TRAINING_TYPE:
        return version != 1004 || size >= 6;
    case SEGMENT_INFO:
        if (version == 1000)
            return size == 10;
        if (version == 1104)
            return size == 8;
        return true;
    case SEGMENT_RANGE:
        return hasCompleteRecords(10, true);
    default:
        return true;
    }
}

bool TTSReader::blockProcessing(int blockType, int version, ByteArray &data) {
    if (!hasValidBlockSize(blockType, version, data.size())) {
        DEBUG_LOG << "Invalid block size " << data.size() << " for block "
                  << blockType << " version " << version << "\n";
        return false;
    }

    switch (blockType) {
    case PROGRAM_DATA:
        programData(version, data);
        break;

    case DISTANCE_FRAME:
        return distanceToFrame(version, data);

    case GPS_DATA:
        GPSData(version, data);
        break;

    case GENERAL_INFO:
        generalInfo(version, data);
        break;

    case TRAINING_TYPE:
        trainingType(version, data);
        break;

    case SEGMENT_INFO:
        return segmentInfo(version, data);

    case SEGMENT_RANGE:
        segmentRange(version, data);
        break;

    case VIDEO_INFO:
        videoInfo(version, data);
        break;

    default:

        DEBUG_LOG << "Unidentified block type " << blockType << "\n";
        break;
    }

    return true;
}

/*

 * Block routines

 */

// it looks like part of GENERALINFO, definition of type is included:
// DWORD WattSlopePulse;//0 = Watt program, 1 = Slope program, 2 = Pulse
// (HR) program
// DWORD TimeDist; //0 = Time based program, 1 = distance based program
// I'm not sure about the order.. but only slope/distance (0/) and
// power/time (1/1) pairs are handled..
void TTSReader::generalInfo(int version, ByteArray &data) {
    if (!hasValidBlockSize(GENERAL_INFO, version, data.size()))
        return;

    const char* programType;

    switch (data[0]) {
    case 0:
        programType = "slope";
        break;
    case 1:
        programType = "watt";
        DEBUG_LOG << "Power files not currently supported\n";
        //throw new RuntimeException("Power files not currently supported");
        break;
    case 2:
        programType = "heartRate";
        break;

    default:
        programType = "unknown program";
        break;
    }

    const char* trainingType;

    switch (data[1]) {
    case 0:
        trainingType = "distance";
        break;
    case 1:
        trainingType = "time";
        break;
    default:
        trainingType = "unknown training";
        break;
    }

    DEBUG_LOG << "[program type] " << programType << " -> " << trainingType << "\n";

    return;
}

// It screams.. "I'm GPS position!". Distance followed by lat, lon,
// altitude
void TTSReader::GPSData(int version, ByteArray &data) {
    if (!hasValidBlockSize(GPS_DATA, version, data.size())) {
        DEBUG_LOG << "GPS Points Data wrong length " << data.size() << "\n";
        return;
    }

    DEBUG_LOG << "[" << (data.size() / 16) << " gps points]\n";

    const size_t gpsCount = data.size() / 16;
    if (gpsPoints.size() > kMaxTtsDecodedRecords
        || gpsCount > kMaxTtsDecodedRecords - gpsPoints.size()) {
        return;
    }

    gpsPoints.reserve(gpsPoints.size() + gpsCount);

    for (size_t i = 0; i < gpsCount; i++) {

        int distance = getUInt(data, i * 16);

        double lat = AsFloat(getUInt(data, i * 16 + 4));
        double lon = AsFloat(getUInt(data, i * 16 + 8));
        double altitude = AsFloat(getUInt(data, i * 16 + 12));

        if (lat != 0. || lon != 0.) {
            fHasGPS = true;
        }

        if (altitude != 0.) {
            fHasAlt = true;
        }

        bool fIsDuplicate = false;

        // Some tts files contain duplicate points. Remove these
        // since they only screw up interpolation during stream
        // merging.
        if (gpsPoints.size()) {
            GPSPoint &prevGPS = gpsPoints[gpsPoints.size() - 1];

            // Compare geo distance independent of altitude,
            // altitude noise is a separate problem.
            geolocation curGPS(lat, lon, 0);
            geolocation newGPS(prevGPS.lat, prevGPS.lon, 0);

            double distance = curGPS.DistanceFrom(newGPS);

            int distanceExp;
            std::frexp(distance, &distanceExp);

            // If outside of 2^-12 then they are not the same location.
            if (distanceExp < -12) {
                fIsDuplicate = true;
            }
        }

        if (!fIsDuplicate) {
            gpsPoints.push_back(GPSPoint(distance, lat, lon, altitude));
        }
    }

    return;
}

/*
 * Slope/Distance Program: Distance to Frame Mapping Format: 2102675
 * (cm)/77531 (frames)
 *
 * Watt/Time Program:
 */
bool TTSReader::distanceToFrame(int version, ByteArray &data) {
    if (!hasValidBlockSize(DISTANCE_FRAME, version, data.size())) {
        DEBUG_LOG << "Distance2Frame Data wrong length " << data.size() << "\n";
        return false;
    }

    if (data.empty())
        return true;

    /*
     * We can calculate frame rate by dividing number of frames by no. of
     * data points. If value is less than or equal to 30 this is the
     * framerate, if between 30 and 100 divided by 2 (one datapoint every 2
     * seconds of film - typical with tacx TTS files), if the value is more
     * than 100 divide by 10 (typical with RLV files converted to TTS).
     */
    DEBUG_LOG << "[" << (data.size() / 8) << " video points][last frame "
        << getUInt(data, data.size() - 4) << "]" << "\n";

    const size_t dataPointCount = data.size() / 8;
    const double frames = getUInt(data, data.size() - 4);
    double decodedFrameRate = frames / static_cast<double>(dataPointCount);

    if (decodedFrameRate > 30 && decodedFrameRate < 100) {
        decodedFrameRate /= 2.0;
    }
    else if (decodedFrameRate >= 100) {
        decodedFrameRate /= 10.0;
    }

    if (!std::isfinite(decodedFrameRate) || decodedFrameRate <= 0) {
        DEBUG_LOG << "Invalid frame rate " << decodedFrameRate << "\n";
        return false;
    }

    DEBUG_LOG << "Frame Rate " << decodedFrameRate << "\n";

    if (pointList.size() > kMaxTtsDecodedRecords
        || dataPointCount > kMaxTtsDecodedRecords - pointList.size()) {
        return false;
    }

    pointList.reserve(pointList.size() + dataPointCount);

    for (size_t i = 0; i < dataPointCount; i++) {
        Point p;
        p.setDistanceFromStart(getUInt(data, i * 8));
        const std::uint32_t frame = getUInt(data, i * 8 + 4);
        p.setTime(frame / decodedFrameRate);
        pointList.push_back(p);
    }

    frameRate = decodedFrameRate;

    // This reader was premised on all tts files having frame mapping, but
    // some don't...
    fHasFrameMapping = pointList.size() != 0;

    return true;
}

/**
 * PROGRAM data
 *
 *
 * Format: slope/distance information or power/time
 *
 * short slope // FLOAT RollingFriction; // Usually 4.0 // Now it is integer
 * (/100=>[m], /100=>[s]) and short (/100=>[%], [W], // probably HR as
 * well..) // Value selector is in 1031.
 */

void TTSReader::programData(int version, ByteArray &data) {
    if (!hasValidBlockSize(PROGRAM_DATA, version, data.size())) {
        DEBUG_LOG << "Program data wrong length" << data.size() << "\n";
        return;
    }

    DEBUG_LOG << "[" << data.size() / 6 << " program points]";

    double distance = 0;
    const size_t pointCount = data.size() / 6;
    const size_t maxAdditionalPoints = pointCount + 1;
    if (programList.size() > kMaxTtsDecodedRecords
        || maxAdditionalPoints > kMaxTtsDecodedRecords - programList.size()) {
        return;
    }

    programList.reserve(programList.size() + maxAdditionalPoints);

    for (size_t i = 0; i < pointCount; i++) {
        int slope = getUShort(data, i * 6);
        if ((slope & 0x8000) != 0) {
            slope -= 0x10000;
        }

        // slope %, distance meters
        distance += (getUInt(data, i * 6 + 2));

        // System.out.println("slope " + slope + " distance " + distance);
        ProgramPoint p;

        p.slope = (double)slope / 100;
        p.distance = distance;

        if (i == 0) {
            // first time thru'
            minSlope = p.slope;
            maxSlope = p.slope;
        }

        if (p.slope < minSlope) {
            minSlope = p.slope;
        }

        if (p.slope > maxSlope) {
            maxSlope = p.slope;
        }

        // If first point isn't zero distance then create fake
        // first point. This matters for a few early tts rides
        // including NO_OCEAN from lunicus and IT_Mortirolo from
        // tacx.
        if (i == 0 && distance != 0.) {
            programList.push_back({0, 0});
        }

        programList.push_back(p);
    }
}
