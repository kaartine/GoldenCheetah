/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingCsvSeries_h
#define _GC_TrainingCsvSeries_h

#include <QStringList>
#include <QVector>

class TrainingCsvSeriesLayout
{
public:
    static TrainingCsvSeriesLayout fromColumns(const QStringList &columns);

    bool isEmpty() const;
    bool shouldAppend(double target, double virtualGear) const;
    QStringList valueNames() const;
    QStringList unitNames() const;
    QVector<double> values(double target, double virtualGear) const;

private:
    bool hasTarget_ = false;
    bool hasVirtualGear_ = false;
};

#endif
