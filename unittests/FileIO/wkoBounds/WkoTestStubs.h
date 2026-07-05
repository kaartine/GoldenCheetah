/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef WKO_TEST_STUBS_H
#define WKO_TEST_STUBS_H

// WkoRideFile only needs this small part of RideFile's interface. Keeping the
// test double here lets this focused test compile the real importer with ASan.
#define _GC_GoldenCheetah_h
#define _RideFile_h

#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class Context;

struct RideFilePoint
{
    RideFilePoint() : secs(0.0), watts(0.0) {}
    explicit RideFilePoint(double secs) : secs(secs), watts(0.0) {}

    double secs;
    double watts;
};

class RideFileInterval
{
public:
    enum IntervalType { DEVICE, USER };

    RideFileInterval()
        : type(USER), start(0.0), stop(0.0), test(false), color(Qt::black) {}

    IntervalType type;
    double start;
    double stop;
    QString name;
    bool test;
    QColor color;
};

class RideFile
{
public:
    enum SpecialValue { NA = -255, NIL = 0 };

    RideFile() : recIntSecs_(0.0) {}
    ~RideFile() { qDeleteAll(dataPoints_); }

    void setFileFormat(const QString &value) { fileFormat_ = value; }
    void setStartTime(const QDateTime &value) { startTime_ = value; }
    void setRecIntSecs(double value) { recIntSecs_ = value; }
    double recIntSecs() const { return recIntSecs_; }
    const QVector<RideFilePoint *> &dataPoints() const { return dataPoints_; }

    void setTag(const QString &name, const QString &value)
    {
        tags_.insert(name, value);
    }

    QString getTag(const QString &name, const QString &fallback) const
    {
        return tags_.value(name, fallback);
    }

    void setDeviceType(const QString &value) { setTag("Device", value); }

    void appendReference(const RideFilePoint &) {}

    void addInterval(RideFileInterval::IntervalType, double, double,
                     const QString &, QColor = Qt::black, bool = false) {}

    void appendPoint(double secs, double, double, double, double, double,
                     double, double, double, double, double, double, double,
                     double, double, double, double, double, double, double,
                     double, double, double, double, double, double, double,
                     double, double, double, double, double, double, double,
                     int)
    {
        dataPoints_.append(new RideFilePoint(secs));
    }

private:
    double recIntSecs_;
    QDateTime startTime_;
    QString fileFormat_;
    QMap<QString, QString> tags_;
    QVector<RideFilePoint *> dataPoints_;
};

struct RideFileReader
{
    virtual ~RideFileReader() {}
    virtual RideFile *openRideFile(QFile &, QStringList &,
                                   QList<RideFile *> * = 0) const = 0;
    virtual bool hasWrite() const { return false; }
    virtual bool writeRideFile(Context *, const RideFile *, QFile &) const
    {
        return false;
    }
};

class RideFileFactory
{
public:
    static RideFileFactory &instance()
    {
        static RideFileFactory factory;
        return factory;
    }

    int registerReader(const QString &, const QString &, RideFileReader *reader)
    {
        delete reader;
        return 0;
    }
};

#include "FileIO/WkoRideFile.h"

#endif // WKO_TEST_STUBS_H
