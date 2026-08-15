/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Bindings.h"

#include <algorithm>
#include <utility>

PythonDataSeries::PythonDataSeries(
    QString name,
    Py_ssize_t count,
    bool readOnly,
    RideFile::SeriesType seriesType,
    RideFile *rideFile)
    : name(std::move(name))
    , count(count)
    , data(nullptr)
    , readOnly(readOnly)
    , seriesType(seriesType)
    , rideFile(rideFile)
{
    if (count > 0) {
        data = new double[count];
    }
}

PythonDataSeries::PythonDataSeries(QString name, Py_ssize_t count)
    : name(std::move(name))
    , count(count)
    , data(nullptr)
    , readOnly(true)
    , seriesType(RideFile::none)
    , rideFile(nullptr)
{
    if (count > 0) {
        data = new double[count];
    }
}

PythonDataSeries::PythonDataSeries()
    : name()
    , count(0)
    , data(nullptr)
    , readOnly(true)
    , seriesType(RideFile::none)
    , rideFile(nullptr)
{
}

PythonDataSeries::PythonDataSeries(const PythonDataSeries &other)
    : name(other.name)
    , count(other.count)
    , data(nullptr)
    , readOnly(other.readOnly)
    , seriesType(other.seriesType)
    , rideFile(other.rideFile)
{
    if (count > 0) {
        data = new double[count];
        std::copy_n(other.data, count, data);
    }
}

PythonDataSeries::PythonDataSeries(PythonDataSeries &&other) noexcept
    : PythonDataSeries()
{
    swap(other);
}

PythonDataSeries::PythonDataSeries(PythonDataSeries *ownedSeries)
    : PythonDataSeries()
{
    if (ownedSeries) {
        swap(*ownedSeries);
        delete ownedSeries;
    }
}

PythonDataSeries::~PythonDataSeries()
{
    delete[] data;
}

PythonDataSeries &
PythonDataSeries::operator=(const PythonDataSeries &other)
{
    if (this != &other) {
        PythonDataSeries copy(other);
        swap(copy);
    }
    return *this;
}

PythonDataSeries &
PythonDataSeries::operator=(PythonDataSeries &&other) noexcept
{
    if (this != &other) {
        PythonDataSeries moved(std::move(other));
        swap(moved);
    }
    return *this;
}

void
PythonDataSeries::swap(PythonDataSeries &other) noexcept
{
    name.swap(other.name);
    std::swap(count, other.count);
    std::swap(data, other.data);
    std::swap(readOnly, other.readOnly);
    std::swap(seriesType, other.seriesType);
    std::swap(rideFile, other.rideFile);
}
