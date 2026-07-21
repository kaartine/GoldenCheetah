/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _CpCsvImport_h
#define _CpCsvImport_h

#include <QString>
#include <QVector>

namespace CpCsvImport
{

// RideFileCache has the same two-day upper bound for a single activity.
constexpr int DefaultMaxPoints = 2 * 24 * 60 * 60;
constexpr int DefaultMaxRows = 2 * DefaultMaxPoints;

struct Limits
{
    int maxRows = DefaultMaxRows;
    int maxPoints = DefaultMaxPoints;
};

struct Segment
{
    int firstSecond;
    int lastSecond;
    double watts;
};

class Builder
{
public:
    explicit Builder(const Limits &limits = Limits());

    bool addRow(const QString &row, int lineNumber, QString &error);
    bool finalize(QString &error);

    const QVector<Segment> &segments() const { return segments_; }
    int pointCount() const { return pointCount_; }

private:
    struct Sample
    {
        int seconds;
        double averageWatts;
        int lineNumber;
    };

    bool fail(const QString &message, QString &error);

    Limits limits_;
    QVector<Sample> samples_;
    QVector<Segment> segments_;
    QString failure_;
    int pointCount_ = 0;
    bool failed_ = false;
    bool finalized_ = false;
};

} // namespace CpCsvImport

#endif // _CpCsvImport_h
