/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHESNAPSHOT_H
#define GC_RIDECACHESNAPSHOT_H

#include <QColor>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

class IntervalItem;
class RideItem;

class RideCacheItemSnapshot
{
public:
    RideCacheItemSnapshot();
    ~RideCacheItemSnapshot();
    RideCacheItemSnapshot(RideCacheItemSnapshot &&) noexcept;
    RideCacheItemSnapshot &operator=(
        RideCacheItemSnapshot &&) noexcept;

    RideCacheItemSnapshot(const RideCacheItemSnapshot &) = delete;
    RideCacheItemSnapshot &operator=(
        const RideCacheItemSnapshot &) = delete;

    static RideCacheItemSnapshot takeFrom(RideItem &source);

    const QString &fileName() const { return fileName_; }
    const QDateTime &dateTime() const { return dateTime_; }
    bool applyTo(RideItem &target);

private:
    QVector<double> metrics_;
    QVector<double> counts_;
    QMap<int, double> stdmeans_;
    QMap<int, double> stdvariances_;
    QMap<QString, QString> metadata_;
    QMap<QString, QStringList> xdata_;
    QStringList errors_;
    std::vector<std::unique_ptr<IntervalItem>> intervals_;

    bool dirty_ = false;
    bool stale_ = true;
    bool edit_ = false;
    bool skipSave_ = false;
    QString fileName_;
    QDateTime dateTime_;
    int zoneRange_ = -1;
    int hrZoneRange_ = -1;
    int paceZoneRange_ = -1;
    unsigned long fingerprint_ = 0;
    unsigned long metadataCrc_ = 0;
    unsigned long crc_ = 0;
    unsigned long timestamp_ = 0;
    int databaseVersion_ = 0;
    int userDatabaseVersion_ = 0;
    QColor color_;
    QString present_;
    QString sport_;
    bool bike_ = false;
    bool run_ = false;
    bool swim_ = false;
    bool crossTraining_ = false;
    bool aero_ = false;
    double weight_ = 0;
    QStringList overrides_;
    bool samples_ = false;
};

class RideCacheSnapshotBatch
{
public:
    void append(RideCacheItemSnapshot &&snapshot);
    bool isEmpty() const { return snapshots_.empty(); }
    qsizetype size() const {
        return static_cast<qsizetype>(snapshots_.size());
    }

    std::vector<RideCacheItemSnapshot> &snapshots() {
        return snapshots_;
    }

private:
    std::vector<RideCacheItemSnapshot> snapshots_;
};

#endif // GC_RIDECACHESNAPSHOT_H
