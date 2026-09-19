/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHZONES_H
#define GC_RIDEREFRESHZONES_H

#include <QDate>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVector>

#include <memory>

class Athlete;
class HrZones;
class PaceZones;
class Zones;

class RideRefreshZones final
{
public:
    template<typename T> struct Band {
        QString name;
        QString description;
        T low{};
        T high{};
        double trimp = 0.0;
    };

    struct PowerRange {
        QDate begin;
        QDate end;
        int cp = 0;
        int rawAeT = 0;
        int ftp = 0;
        int wprime = 0;
        int pmax = 0;
        QVector<Band<int>> bands;

        int resolvedAeT() const;
        int whichZone(double value) const;
        quint16 fingerprint() const;
    };

    struct HeartRateRange {
        QDate begin;
        QDate end;
        int lt = 0;
        int rawAeT = 0;
        int restHr = 0;
        int maxHr = 0;
        QVector<Band<int>> bands;

        int resolvedAeT() const;
        int whichZone(double value) const;
        quint16 fingerprint() const;
    };

    struct PaceRange {
        QDate begin;
        QDate end;
        double cv = 0.0;
        double rawAeT = 0.0;
        QVector<Band<double>> bands;

        double resolvedAeT(bool swim) const;
        int whichZone(double value) const;
        quint16 fingerprint() const;
    };

    struct PowerHistory {
        bool present = false;
        QString sport;
        QString cpForFtpSettingKey;
        int rawCpForFtpSetting = 0;
        QVector<PowerRange> ranges;

        int whichRange(const QDate &date) const;
        const PowerRange *rangeForDate(const QDate &date) const;
        quint16 fingerprint(const QDate &date) const;
        bool cpOverridesFtp() const { return rawCpForFtpSetting == 0; }
    };

    struct HeartRateHistory {
        bool present = false;
        QString sport;
        QVector<HeartRateRange> ranges;

        int whichRange(const QDate &date) const;
        const HeartRateRange *rangeForDate(const QDate &date) const;
        quint16 fingerprint(const QDate &date) const;
    };

    struct PaceHistory {
        bool present = false;
        bool swim = false;
        bool metricPace = true;
        QString metricUnits;
        QString imperialUnits;
        QVector<PaceRange> ranges;

        int whichRange(const QDate &date) const;
        const PaceRange *rangeForDate(const QDate &date) const;
        quint16 fingerprint(const QDate &date) const;
        QString formatPace(double kph) const;
        QString paceUnits() const;
    };

    static std::shared_ptr<const RideRefreshZones> create(
        QHash<QString, PowerHistory> power,
        QHash<QString, HeartRateHistory> heartRate,
        PaceHistory run,
        PaceHistory swim);

    const PowerHistory *power(const QString &sport) const;
    const HeartRateHistory *heartRate(const QString &sport) const;
    const PaceHistory *pace(bool swim) const
    {
        const PaceHistory &result = swim ? swim_ : run_;
        return result.present ? &result : nullptr;
    }

private:
    RideRefreshZones(
        QHash<QString, PowerHistory> power,
        QHash<QString, HeartRateHistory> heartRate,
        PaceHistory run,
        PaceHistory swim);

    const QHash<QString, PowerHistory> power_;
    const QHash<QString, HeartRateHistory> heartRate_;
    const PaceHistory run_;
    const PaceHistory swim_;
};

RideRefreshZones::PowerHistory captureRideRefreshPowerZones(
    const Zones *source, int rawCpForFtpSetting);
RideRefreshZones::HeartRateHistory captureRideRefreshHeartRateZones(
    const HrZones *source);
RideRefreshZones::PaceHistory captureRideRefreshPaceZones(
    const PaceZones *source, bool swim, bool metricPace);

// Must be called on athlete's QObject owner thread. Returns null otherwise.
std::shared_ptr<const RideRefreshZones>
captureRideRefreshZones(
    const Athlete *athlete,
    const QHash<QString, QVariant> &globalSettings,
    const QHash<QString, QVariant> &athleteSettings,
    bool useMetricUnits);

#endif // GC_RIDEREFRESHZONES_H
