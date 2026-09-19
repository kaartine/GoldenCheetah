/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHMEASURES_H
#define GC_RIDEREFRESHMEASURES_H

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <memory>
#include <optional>

class Athlete;
class MeasuresGroup;

class RideRefreshMeasures final
{
public:
    static constexpr int MaximumFields = 16;

    struct Field {
        QString symbol;
        QString name;
        QString metricUnits;
        QString imperialUnits;
        double unitsFactor = 1.0;
        QStringList headers;
    };

    struct Observation {
        QDateTime when;
        QString comment;
        int source = 0;
        QString originalSource;
        std::array<double, MaximumFields> values{};
        quint16 legacyFingerprint = 0;

        quint16 fingerprint() const { return legacyFingerprint; }
    };

    struct Group {
        QString symbol;
        QString name;
        QVector<Field> fields;
        QVector<Observation> observations;
    };

    static std::shared_ptr<const RideRefreshMeasures> create(
        QVector<Group> groups,
        quint16 missingObservationFingerprint);

    const QVector<Group> &groups() const { return groups_; }
    QStringList groupSymbols() const;
    QStringList groupNames() const;
    const Group *group(const QString &symbol) const;
    const Group *group(int index) const;
    const Field *field(const QString &groupSymbol, int index) const;
    const Field *field(
        const QString &groupSymbol, const QString &fieldSymbol) const;
    const Observation *observationForDate(
        const QString &groupSymbol, const QDate &date) const;
    const Observation *observationForDate(
        int group, const QDate &date) const;

    std::optional<QDate> startDate(const QString &groupSymbol) const;
    std::optional<QDate> startDate(int group) const;
    std::optional<QDate> endDate(const QString &groupSymbol) const;
    std::optional<QDate> endDate(int group) const;
    std::optional<QStringList> fieldSymbols(
        const QString &groupSymbol) const;
    std::optional<QStringList> fieldSymbols(int group) const;
    std::optional<QString> fieldUnits(
        const QString &groupSymbol,
        int field,
        bool useMetricUnits = true) const;
    std::optional<QString> fieldUnits(
        const QString &groupSymbol,
        const QString &fieldSymbol,
        bool useMetricUnits = true) const;

    // Unknown groups and fields fail closed with nullopt. A valid lookup with
    // no observation for the requested date preserves the legacy zero value.
    std::optional<double> fieldValue(
        const QString &groupSymbol,
        const QDate &date,
        int field,
        bool useMetricUnits = true) const;
    std::optional<double> fieldValue(
        int group,
        const QDate &date,
        int field,
        bool useMetricUnits = true) const;
    std::optional<double> fieldValue(
        const QString &groupSymbol,
        const QDate &date,
        const QString &fieldSymbol,
        bool useMetricUnits = true) const;

    // A known group with no matching observation has the same fingerprint as
    // a default-constructed legacy Measure. Unknown groups return nullopt.
    std::optional<quint16> fingerprint(
        const QString &groupSymbol, const QDate &date) const;
    std::optional<quint16> fingerprint(
        int group, const QDate &date) const;

private:
    RideRefreshMeasures(
        QVector<Group> groups,
        QHash<QString, int> groupIndexes,
        quint16 missingObservationFingerprint);

    static const Observation *observationForDate(
        const Group &group, const QDate &date);

    const QVector<Group> groups_;
    const QHash<QString, int> groupIndexes_;
    const quint16 missingObservationFingerprint_;
};

RideRefreshMeasures::Group captureRideRefreshMeasuresGroup(
    MeasuresGroup *source);

// Must be called on athlete's QObject owner thread. Returns null otherwise.
std::shared_ptr<const RideRefreshMeasures>
captureRideRefreshMeasures(const Athlete *athlete);

#endif // GC_RIDEREFRESHMEASURES_H
