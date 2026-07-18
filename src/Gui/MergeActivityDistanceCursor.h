/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#ifndef GC_MERGEACTIVITYDISTANCECURSOR_H
#define GC_MERGEACTIVITYDISTANCECURSOR_H

#include <QVector>

struct RideFilePoint;

namespace MergeActivityDistance {

class SourceCursor final
{
public:
    explicit SourceCursor(const QVector<RideFilePoint *> &points);

    const RideFilePoint *atOrAfter(double distance);

private:
    const QVector<RideFilePoint *> &points_;
    int index_ = 0;
};

} // namespace MergeActivityDistance

#endif // GC_MERGEACTIVITYDISTANCECURSOR_H
