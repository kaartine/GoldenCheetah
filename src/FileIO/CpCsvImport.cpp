/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CpCsvImport.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace CpCsvImport
{

Builder::Builder(const Limits &limits)
    : limits_(limits)
{
}

bool Builder::fail(const QString &message, QString &error)
{
    failed_ = true;
    failure_ = message;
    segments_.clear();
    pointCount_ = 0;
    error = failure_;
    return false;
}

bool Builder::addRow(const QString &row, int lineNumber, QString &error)
{
    if (failed_) {
        error = failure_;
        return false;
    }
    if (finalized_)
        return fail(QStringLiteral(
            "CP Plot Export data was appended after finalization."), error);
    if (limits_.maxRows <= 0 || limits_.maxPoints <= 0)
        return fail(QStringLiteral("CP Plot Export limits are invalid."), error);
    if (samples_.size() >= limits_.maxRows) {
        return fail(QStringLiteral(
            "CP Plot Export row limit of %1 was exceeded at line %2.")
                        .arg(limits_.maxRows)
                        .arg(lineNumber),
                    error);
    }

    const QStringList columns = row.split(QLatin1Char(','),
                                          Qt::KeepEmptyParts);
    if (columns.size() < 2) {
        return fail(QStringLiteral(
            "Invalid CP Plot Export row at line %1: expected seconds and "
            "value columns.")
                        .arg(lineNumber),
                    error);
    }

    bool secondsOk = false;
    const double secondsValue = columns.at(0).trimmed().toDouble(&secondsOk);
    if (!secondsOk || !std::isfinite(secondsValue)
        || secondsValue < 1.0
        || secondsValue != std::floor(secondsValue)) {
        return fail(QStringLiteral(
            "Invalid CP Plot Export timestamp at line %1: expected a "
            "positive whole number of seconds.")
                        .arg(lineNumber),
                    error);
    }
    if (secondsValue > double(limits_.maxPoints)) {
        return fail(QStringLiteral(
            "CP Plot Export timestamp at line %1 would create %2 points; "
            "the limit is %3.")
                        .arg(lineNumber)
                        .arg(secondsValue, 0, 'f', 0)
                        .arg(limits_.maxPoints),
                    error);
    }

    bool powerOk = false;
    const double averageWatts = columns.at(1).trimmed().toDouble(&powerOk);
    if (!powerOk || !std::isfinite(averageWatts) || averageWatts < 0.0) {
        return fail(QStringLiteral(
            "Invalid CP Plot Export value at line %1: expected a finite "
            "non-negative number.")
                        .arg(lineNumber),
                    error);
    }

    samples_.append({int(secondsValue), averageWatts, lineNumber});
    error.clear();
    return true;
}

bool Builder::finalize(QString &error)
{
    if (failed_) {
        error = failure_;
        return false;
    }
    if (finalized_) {
        error.clear();
        return true;
    }
    if (samples_.isEmpty())
        return fail(QStringLiteral("CP Plot Export contains no data rows."),
                    error);

    QVector<Sample> sortedSamples = samples_;
    std::stable_sort(sortedSamples.begin(), sortedSamples.end(),
                     [](const Sample &left, const Sample &right) {
        return left.seconds < right.seconds;
    });

    QVector<Sample> uniqueSamples;
    uniqueSamples.reserve(sortedSamples.size());
    for (const Sample &sample : sortedSamples) {
        if (!uniqueSamples.isEmpty()
            && sample.seconds == uniqueSamples.constLast().seconds) {
            if (sample.averageWatts
                != uniqueSamples.constLast().averageWatts) {
                return fail(QStringLiteral(
                    "CP Plot Export contains conflicting values for second "
                    "%1 at lines %2 and %3.")
                                .arg(sample.seconds)
                                .arg(uniqueSamples.constLast().lineNumber)
                                .arg(sample.lineNumber),
                            error);
            }
            continue;
        }
        uniqueSamples.append(sample);
    }

    QVector<Segment> builtSegments;
    builtSegments.reserve(uniqueSamples.size());

    int previousSeconds = 0;
    long double previousAverage = 0.0L;
    long double maximumAverage = 0.0L;

    for (const Sample &sample : uniqueSamples) {
        const long double seconds = sample.seconds;
        const long double average = sample.averageWatts;
        const long double duration = sample.seconds - previousSeconds;
        if (average > maximumAverage)
            maximumAverage = average;

        const long double numerator =
            average * seconds
            - previousAverage * static_cast<long double>(previousSeconds);
        long double segmentWatts = numerator / duration;
        if (!std::isfinite(segmentWatts)) {
            return fail(QStringLiteral(
                "CP Plot Export value at line %1 overflows during "
                "reconstruction.")
                            .arg(sample.lineNumber),
                        error);
        }

        if (segmentWatts > maximumAverage)
            segmentWatts = maximumAverage;
        if (segmentWatts < 0.0L)
            segmentWatts = 0.0L;

        const double watts = static_cast<double>(segmentWatts);
        if (!std::isfinite(watts)) {
            return fail(QStringLiteral(
                "CP Plot Export value at line %1 is outside the supported "
                "range.")
                            .arg(sample.lineNumber),
                        error);
        }

        builtSegments.append(
            {previousSeconds + 1, sample.seconds, watts});

        previousAverage =
            (previousAverage * static_cast<long double>(previousSeconds)
             + segmentWatts * duration)
            / seconds;
        if (!std::isfinite(previousAverage)) {
            return fail(QStringLiteral(
                "CP Plot Export value at line %1 overflows during "
                "reconstruction.")
                            .arg(sample.lineNumber),
                        error);
        }
        previousSeconds = sample.seconds;
    }

    segments_ = builtSegments;
    pointCount_ = uniqueSamples.constLast().seconds;
    finalized_ = true;
    error.clear();
    return true;
}

} // namespace CpCsvImport
