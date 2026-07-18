/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHESTARTUP_H
#define GC_RIDECACHESTARTUP_H

#include "ConfigFlags.h"
#include <QtGlobal>

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <utility>

namespace RideCacheStartup {

inline constexpr qsizetype BatchSize = 512;
inline constexpr int MaximumPendingSnapshotBatches = 4;

struct IndexedFile
{
    QString path;
    QString fileName;
    QDateTime dateTime;
    bool planned = false;
};

template<typename DateParser>
QVector<IndexedFile> buildIndex(
    const QStringList &activityNames,
    const QString &activityPath,
    const QStringList &plannedNames,
    const QString &plannedPath,
    DateParser &&parseDate)
{
    QVector<IndexedFile> files;
    files.reserve(activityNames.size() + plannedNames.size());

    const auto append = [&](const QStringList &names,
                            const QString &path,
                            bool planned) {
        for (const QString &name : names) {
            QDateTime dateTime;
            if (parseDate(name, &dateTime)) {
                files.append({path, name, dateTime, planned});
            }
        }
    };
    append(activityNames, activityPath, false);
    append(plannedNames, plannedPath, true);

    std::sort(
        files.begin(), files.end(),
        [](const IndexedFile &left, const IndexedFile &right) {
            if (left.dateTime != right.dateTime) {
                return left.dateTime < right.dateTime;
            }
            if (left.planned != right.planned) {
                return !left.planned;
            }
            return left.fileName < right.fileName;
        });
    return files;
}

struct BatchRange
{
    qsizetype first = 0;
    qsizetype count = 0;
};

inline QVector<BatchRange> batchRanges(
    qsizetype count, qsizetype batchSize)
{
    QVector<BatchRange> ranges;
    if (count <= 0 || batchSize <= 0) return ranges;

    ranges.reserve((count + batchSize - 1) / batchSize);
    for (qsizetype first = 0; first < count; first += batchSize) {
        ranges.append({first, std::min(batchSize, count - first)});
    }
    return ranges;
}

struct SnapshotTargetState
{
    QString fileName;
    QDateTime dateTime;
    bool stale = true;
    bool dirty = false;
    bool editing = false;
    bool open = false;
    bool hasIntervals = false;
};

inline bool canApplySnapshot(
    const QString &snapshotFileName,
    const QDateTime &snapshotDateTime,
    const SnapshotTargetState &target)
{
    return snapshotFileName == target.fileName
        && snapshotDateTime == target.dateTime
        && target.stale
        && !target.dirty
        && !target.editing
        && !target.open
        && !target.hasIntervals;
}

struct InvalidationPlan
{
    bool invalidateWbal = false;
    bool rebuildCalendarText = false;
    bool recolor = false;
    bool refreshMetrics = false;
};

inline InvalidationPlan planInvalidation(qint32 flags)
{
    constexpr qint32 MetricDependencies =
        CONFIG_ATHLETE | CONFIG_ZONES | CONFIG_GENERAL
        | CONFIG_USERMETRICS | CONFIG_DISCOVERY;

    return {
        bool(flags & CONFIG_WBAL),
        bool(flags & CONFIG_FIELDS),
        bool(flags & CONFIG_NOTECOLOR),
        bool(flags & MetricDependencies)
    };
}

template<typename Function>
void forEachMetricCandidate(
    const InvalidationPlan &plan,
    qsizetype count,
    Function &&function)
{
    if (!plan.refreshMetrics) return;
    for (qsizetype index = 0; index < count; ++index) {
        std::forward<Function>(function)(index);
    }
}

class RefreshGeneration
{
public:
    quint64 request()
    {
        return ++requested_;
    }

    quint64 beginLatest()
    {
        Q_ASSERT(active_ == 0);
        Q_ASSERT(hasPending());
        active_ = requested_;
        return active_;
    }

    bool accepts(quint64 generation) const
    {
        return active_ == generation && requested_ == generation;
    }

    bool finish(quint64 generation)
    {
        if (active_ == generation) active_ = 0;
        completed_ = std::max(completed_, generation);
        return hasPending();
    }

    void cancel()
    {
        ++requested_;
        active_ = 0;
        completed_ = requested_;
    }

    bool hasActive() const
    {
        return active_ != 0;
    }

    bool hasPending() const
    {
        return requested_ > completed_;
    }

    quint64 requested() const
    {
        return requested_;
    }

private:
    quint64 requested_ = 0;
    quint64 active_ = 0;
    quint64 completed_ = 0;
};

} // namespace RideCacheStartup

#endif // GC_RIDECACHESTARTUP_H
