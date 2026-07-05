/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef CAF_TEST_STUBS_H
#define CAF_TEST_STUBS_H

// TacxCafRideFile only needs this small part of RideFile's interface. Keeping
// the test double local lets the focused test compile the real importer with
// release assertions disabled and sanitizers enabled. The test project and
// test source both enforce that release configuration.
#define _RideFile_h

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class Context;

struct RideFilePoint
{
    template<typename... Rest>
    RideFilePoint(double secsValue, double cadenceValue, double heartRateValue,
                  double distanceValue, double speedValue, double torqueValue,
                  double powerValue, double altitudeValue, double longitudeValue,
                  double latitudeValue, double headwindValue, Rest...)
        : secs(secsValue), cad(cadenceValue), hr(heartRateValue),
          km(distanceValue), kph(speedValue), nm(torqueValue), watts(powerValue),
          alt(altitudeValue), lon(longitudeValue), lat(latitudeValue),
          headwind(headwindValue), interval(0)
    {
    }

    double secs;
    double cad;
    double hr;
    double km;
    double kph;
    double nm;
    double watts;
    double alt;
    double lon;
    double lat;
    double headwind;
    int interval;
};

class RideFile
{
public:
    enum SpecialValue { NA = -255 };

    struct AppendedPoint {
        double secs;
        double cadence;
        double heartRate;
        double distance;
        double speed;
        double torque;
        double power;
    };

    RideFile() : recIntSecs_(0.0) {}

    void setDeviceType(const QString &value) { deviceType_ = value; }
    void setFileFormat(const QString &value) { fileFormat_ = value; }
    void setStartTime(const QDateTime &value) { startTime_ = value; }
    void setRecIntSecs(double value) { recIntSecs_ = value; }
    double recIntSecs() const { return recIntSecs_; }

    template<typename... Rest>
    void appendPoint(double secs, double cadence, double heartRate,
                     double distance, double speed, double torque,
                     double power, Rest...)
    {
        points_.append({ secs, cadence, heartRate, distance, speed,
                         torque, power });
    }

    int pointCount() const { return points_.size(); }
    const QList<AppendedPoint> &points() const { return points_; }
    const QDateTime &startTime() const { return startTime_; }

private:
    double recIntSecs_;
    QString deviceType_;
    QString fileFormat_;
    QDateTime startTime_;
    QList<AppendedPoint> points_;
};

struct RideFileReader
{
    virtual ~RideFileReader() {}
    virtual RideFile *openRideFile(QFile &, QStringList &,
                                   QList<RideFile *> * = 0) const = 0;
    virtual bool hasWrite() const { return false; }
};

class RideFileFactory
{
public:
    static RideFileFactory &instance()
    {
        static RideFileFactory factory;
        return factory;
    }

    ~RideFileFactory()
    {
        qDeleteAll(readers_);
    }

    int registerReader(const QString &suffix, const QString &,
                       RideFileReader *reader)
    {
        delete readers_.take(suffix);
        readers_.insert(suffix, reader);
        return 0;
    }

    RideFileReader *readerForSuffix(const QString &suffix) const
    {
        return readers_.value(suffix, 0);
    }

private:
    QMap<QString, RideFileReader *> readers_;
};

#endif // CAF_TEST_STUBS_H
