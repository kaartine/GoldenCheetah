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

#include <QIODevice>
#include <QString>
#include <QVector>

#include <array>
#include <functional>

static const unsigned int RideFileCacheVersion = 25;

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
    std::array<QVector<float>, BlockCount> blocks;
    std::array<QVector<float>, ZoneBlockCount> zones;

    void clear();
    bool isEmpty() const;
};

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
    bool readBlock(
        Block block,
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

private:
    QIODevice *input_ = nullptr;
    RideFileCacheHeader header_ {};
    std::array<quint32, BlockCount> counts_ {};
    qint64 expectedSize_ = 0;
    bool valid_ = false;
};

bool inspectCache(QIODevice &input,
                  RideFileCacheHeader &header,
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

bool writeCacheAtomically(const QString &path,
                          const CacheWriteOperation &write,
                          QString *error = nullptr);

QString activitySourcePath(
    const QString &activityDirectory,
    const QString &fileName);

QString cachePathForActivity(const QString &cacheRoot,
                             const QString &completedRoot,
                             const QString &plannedRoot,
                             const QString &sourceActivityPath);

} // namespace RideFileCacheIntegrity

#endif
