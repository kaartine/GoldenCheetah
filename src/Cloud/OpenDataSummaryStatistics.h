/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_SUMMARY_STATISTICS_H
#define GC_OPEN_DATA_SUMMARY_STATISTICS_H

#include <QList>
#include <QString>
#include <QVector>

class QTextStream;

namespace OpenDataSummaryStatistics {

struct Distribution
{
    QString type;
    QVector<double> values;
    int binSize = 10;
    int divisor = 1;
};

struct Statistics
{
    bool complete = false;
    QList<Distribution> distributions;
    QVector<double> powerMeanMax;
    QVector<int> meanMaxDurations;
};

bool append(
    const Statistics &statistics,
    QTextStream &destination,
    QString &error);

} // namespace OpenDataSummaryStatistics

#endif
