/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_RideFileCacheIntegrity_h
#define _GC_RideFileCacheIntegrity_h 1

#include "RideFileCRC.h"

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QVector>

#include <array>
#include <functional>

static const unsigned int RideFileCacheVersion = 26;

struct RideFileCacheHeader {
    unsigned int version;
    unsigned int crc;

    unsigned int wattsMeanMaxCount,
                 hrMeanMaxCount,
                 cadMeanMaxCount,
                 nmMeanMaxCount,
                 kphMeanMaxCount,
                 kphdMeanMaxCount,
                 wattsdMeanMaxCount,
                 caddMeanMaxCount,
                 nmdMeanMaxCount,
                 hrdMeanMaxCount,
                 xPowerMeanMaxCount,
                 npMeanMaxCount,
                 vamMeanMaxCount,
                 wattsKgMeanMaxCount,
                 aPowerMeanMaxCount,
                 aPowerKgMeanMaxCount,
                 wattsDistCount,
                 hrDistCount,
                 cadDistCount,
                 gearDistCount,
                 nmDistrCount,
                 kphDistCount,
                 xPowerDistCount,
                 npDistCount,
                 wattsKgDistCount,
                 aPowerDistCount,
                 smo2DistCount,
                 wbalDistCount;

    int LTHR;
    int CP;
    double CV;
    double WEIGHT;
    double WPRIME;
};

namespace RideFileCacheIntegrity {

inline constexpr qint64 CachePreambleBytes =
    static_cast<qint64>(
        sizeof(RideFileCacheHeader))
    + static_cast<qint64>(sizeof(qint64))
    + static_cast<qint64>(RideFileCRC::Sha256Size);
inline constexpr qint64 CacheFooterBytes =
    static_cast<qint64>(RideFileCRC::Sha256Size);

enum Block {
    WattsMeanMax,
    WattsKgMeanMax,
    HrMeanMax,
    CadMeanMax,
    NmMeanMax,
    KphMeanMax,
    KphdMeanMax,
    WattsdMeanMax,
    CaddMeanMax,
    NmdMeanMax,
    HrdMeanMax,
    XPowerMeanMax,
    NpMeanMax,
    VamMeanMax,
    APowerMeanMax,
    APowerKgMeanMax,
    WattsDistribution,
    HrDistribution,
    CadDistribution,
    GearDistribution,
    NmDistribution,
    KphDistribution,
    XPowerDistribution,
    NpDistribution,
    WattsKgDistribution,
    APowerDistribution,
    Smo2Distribution,
    WbalDistribution,
    BlockCount
};

enum ZoneBlock {
    WattsTimeInZone,
    WattsCPTimeInZone,
    HrTimeInZone,
    HrCPTimeInZone,
    PaceTimeInZone,
    PaceCPTimeInZone,
    WbalTimeInZone,
    ZoneBlockCount
};

struct CacheData {
    bool complete = false;
    RideFileCacheHeader header {};
    RideFileCRC::ContentFingerprint sourceFingerprint;
    std::array<QVector<float>, BlockCount> blocks;
    std::array<QVector<float>, ZoneBlockCount> zones;

    void clear();
    bool isEmpty() const;
};

bool validateCacheLayout(
    const RideFileCacheHeader &header,
    QString *error = nullptr);

// Construction validates the bounded preamble. Read requests are queued; their
// output objects must keep a stable address and outlive finish(), which
// authenticates one forward stream and publishes every requested value
// atomically. The one-shot helpers below do this automatically.
class PartialReader final
{
public:
    explicit PartialReader(
        QIODevice &input,
        QString *error = nullptr);

    PartialReader(const PartialReader &) = delete;
    PartialReader &operator=(const PartialReader &) = delete;

    bool isValid() const;
    const RideFileCacheHeader &header() const;
    const RideFileCRC::ContentFingerprint &
    sourceFingerprint() const;
    bool readBlock(
        Block block,
        QVector<float> &output,
        QString *error = nullptr);
    bool readZoneBlock(
        ZoneBlock block,
        QVector<float> &output,
        QString *error = nullptr);
    bool readBlockValue(
        Block block,
        qsizetype index,
        float &output,
        QString *error = nullptr);
    bool readZoneValue(
        ZoneBlock block,
        qsizetype index,
        float &output,
        QString *error = nullptr);
    bool finish(QString *error = nullptr);

private:
    struct PendingRead {
        qint64 firstFloat = 0;
        qint64 count = 0;
        QVector<float> *vectorOutput = nullptr;
        float *scalarOutput = nullptr;
        QVector<float> staged;
    };

    bool queueRead(
        qint64 firstFloat,
        qint64 count,
        QVector<float> *vectorOutput,
        float *scalarOutput,
        QString *error);
    void clearPendingOutputs();

    QIODevice *input_ = nullptr;
    QByteArray preamble_;
    RideFileCacheHeader header_ {};
    RideFileCRC::ContentFingerprint sourceFingerprint_;
    std::array<quint32, BlockCount> counts_ {};
    QVector<PendingRead> pendingReads_;
    qint64 expectedSize_ = 0;
    bool valid_ = false;
    bool finished_ = false;
};

bool inspectCache(QIODevice &input,
                  RideFileCacheHeader &header,
                  QString *error = nullptr);

bool inspectCache(
    QIODevice &input,
    RideFileCacheHeader &header,
    RideFileCRC::ContentFingerprint &sourceFingerprint,
    QString *error = nullptr);

bool readCache(QIODevice &input, CacheData &output, QString *error = nullptr);

bool readBlock(QIODevice &input,
               Block block,
               QVector<float> &output,
               QString *error = nullptr);

bool readBlockValue(QIODevice &input,
                    Block block,
                    qsizetype index,
                    float &output,
                    QString *error = nullptr);

bool readZoneValue(QIODevice &input,
                   ZoneBlock block,
                   qsizetype index,
                   float &output,
                   QString *error = nullptr);

using CacheWriteOperation =
    std::function<bool(QIODevice &output, QString *error)>;

// Runs after the temporary file is flushed and immediately before commit.
using CachePreCommitValidator =
    std::function<bool(QString *error)>;

bool writeCacheAtomically(const QString &path,
                          const CacheWriteOperation &write,
                          QString *error = nullptr);

bool writeCacheAtomically(
    const QString &path,
    const CacheWriteOperation &write,
    const CachePreCommitValidator &validateBeforeCommit,
    QString *error);

QString activitySourcePath(
    const QString &activityDirectory,
    const QString &fileName);

QString cachePathForActivity(const QString &cacheRoot,
                             const QString &completedRoot,
                             const QString &plannedRoot,
                             const QString &sourceActivityPath);

} // namespace RideFileCacheIntegrity

#endif
