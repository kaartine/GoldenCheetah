/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenDataSummaryStatistics.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

namespace {

QString jsonStringLiteral(const QString &value)
{
    const QByteArray array =
        QJsonDocument(QJsonArray{value}).toJson(
            QJsonDocument::Compact);
    return QString::fromUtf8(
        array.mid(1, array.size() - 2));
}

} // namespace

namespace OpenDataSummaryStatistics {

bool append(
    const Statistics &statistics,
    QTextStream &destination,
    QString &error)
{
    error.clear();
    if (!statistics.complete) {
        error = QStringLiteral(
            "OpenData activity statistics are incomplete");
        return false;
    }

    QString fragment;
    QTextStream stream(&fragment);
    for (const Distribution &distribution :
         statistics.distributions) {
        if (distribution.type.isEmpty()
            || distribution.binSize <= 0
            || distribution.divisor <= 0) {
            error = QStringLiteral(
                "Invalid OpenData activity statistics");
            return false;
        }

        QVector<int> bins;
        QVector<int> totals;
        int count = 0;
        double total = 0;
        for (int index = 0;
             index < distribution.values.size();
             ++index) {
            if (count == distribution.binSize) {
                if (total > 0) {
                    bins.append(
                        index - distribution.binSize);
                    totals.append(total);
                }
                count = 0;
                total = 0;
            }
            total += distribution.values.at(index);
            ++count;
        }
        if (total > 0 && count > 0) {
            bins.append(
                distribution.values.size() - count);
            totals.append(total);
        }
        if (bins.isEmpty()) continue;

        const QString totalsKey =
            jsonStringLiteral(
                distribution.type
                + QStringLiteral("_dist"));
        stream << ",\n\t\t\t"
               << totalsKey << ":[";
        for (int index = 0;
             index < totals.size();
             ++index) {
            stream << totals.at(index);
            if (index + 1 != totals.size())
                stream << ", ";
        }
        stream << " ]";

        const QString binsKey =
            jsonStringLiteral(
                distribution.type
                + QStringLiteral("_dist_bins"));
        stream << ",\n\t\t\t"
               << binsKey << ":[";
        for (int index = 0;
             index < bins.size();
             ++index) {
            stream << (bins.at(index) > 0
                    ? bins.at(index)
                        / distribution.divisor
                    : 0);
            if (index + 1 != bins.size())
                stream << ", ";
        }
        stream << " ]";
    }

    if (!statistics.powerMeanMax.isEmpty()) {
        QVector<int> durations;
        QVector<double> values;
        for (const int duration :
             statistics.meanMaxDurations) {
            if (duration <= 0
                || duration
                    >= statistics.powerMeanMax.size()) {
                continue;
            }
            durations.append(duration);
            values.append(
                statistics.powerMeanMax.at(duration));
        }
        if (!durations.isEmpty()) {
            stream << ",\n\t\t\t\"power_mmp\":[";
            for (int index = 0;
                 index < values.size();
                 ++index) {
                if (index > 0) stream << ", ";
                stream << QStringLiteral("%1").arg(
                    values.at(index), 0, 'f', 0);
            }
            stream << " ]";

            stream << ",\n\t\t\t\"power_mmp_secs\":[";
            for (int index = 0;
                 index < durations.size();
                 ++index) {
                if (index > 0) stream << ", ";
                stream << durations.at(index);
            }
            stream << " ]";
        }
    }
    stream.flush();
    destination << fragment;
    if (destination.status() != QTextStream::Ok) {
        error = QStringLiteral(
            "Cannot write OpenData activity statistics");
        return false;
    }
    return true;
}

} // namespace OpenDataSummaryStatistics
