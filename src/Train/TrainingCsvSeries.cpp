/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainingCsvSeries.h"

TrainingCsvSeriesLayout TrainingCsvSeriesLayout::fromColumns(
        const QStringList &columns)
{
    TrainingCsvSeriesLayout layout;
    layout.hasTarget_ = columns.contains(QStringLiteral("target"));
    layout.hasVirtualGear_ = columns.contains(QStringLiteral("virtualgear"));
    return layout;
}

bool TrainingCsvSeriesLayout::isEmpty() const
{
    return !hasTarget_ && !hasVirtualGear_;
}

bool TrainingCsvSeriesLayout::shouldAppend(
        double target,
        double virtualGear) const
{
    return (hasTarget_ && target > 0.0)
            || (hasVirtualGear_ && virtualGear > 0.0);
}

QStringList TrainingCsvSeriesLayout::valueNames() const
{
    QStringList names;
    if (hasTarget_) names.append(QStringLiteral("TARGET"));
    if (hasVirtualGear_) names.append(QStringLiteral("VIRTUAL_GEAR"));
    return names;
}

QStringList TrainingCsvSeriesLayout::unitNames() const
{
    QStringList names;
    if (hasTarget_) names.append(QStringLiteral("Watts"));
    if (hasVirtualGear_) names.append(QStringLiteral("Gear"));
    return names;
}

QVector<double> TrainingCsvSeriesLayout::values(
        double target,
        double virtualGear) const
{
    QVector<double> result;
    if (hasTarget_) result.append(target);
    if (hasVirtualGear_) result.append(virtualGear);
    return result;
}
