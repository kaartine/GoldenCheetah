/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_MAP_ROUTE_POINT_INDEX_H
#define GC_MAP_ROUTE_POINT_INDEX_H

#include <QVector>

class MapRoutePointIndex
{
public:
    void clear();
    void reserve(qsizetype count);
    bool append(double latitude,
                double longitude,
                qsizetype sourceIndex);
    void finalize();

    qsizetype nearest(double latitude,
                      double longitude,
                      qsizetype *examined = nullptr) const;
    qsizetype size() const { return entries_.size(); }

private:
    struct Entry {
        double latitude = 0.0;
        double longitude = 0.0;
        qsizetype sourceIndex = -1;
    };

    QVector<Entry> entries_;
    bool finalized_ = true;
};

#endif
