/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHENVIRONMENT_H
#define GC_RIDEREFRESHENVIRONMENT_H

#include <QColor>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>
#include <memory>

class Context;
class RideMetricRegistrySnapshot;
class RideRefreshMeasures;
class RideRefreshZones;

class RideRefreshEnvironment final
{
public:
    struct Settings {
        QHash<QString, QVariant> global;
        QHash<QString, QVariant> athlete;
    };

    struct CalendarField {
        QString name;
        QString valueName;
        int type = -1;
        bool diary = false;
        QString metricSymbol;
        bool interval = false;
    };

    using TextLookup = std::function<QString(const QString &)>;
    using MetricLookup =
        std::function<QString(const QString &, bool useMetricUnits)>;
    using MetricRelevance = std::function<bool(const QString &)>;

    static std::shared_ptr<const RideRefreshEnvironment> create(
        quint64 generation,
        Settings settings,
        bool useMetricUnits,
        QString colorField,
        QMap<QString, QColor> colorRules,
        QVector<CalendarField> calendarFields,
        QStringList sports,
        std::shared_ptr<const RideMetricRegistrySnapshot> metricRegistry = {},
        std::shared_ptr<const RideRefreshZones> zones = {},
        std::shared_ptr<const RideRefreshMeasures> measures = {});

    quint64 generation() const { return generation_; }
    bool useMetricUnits() const { return useMetricUnits_; }
    const QString &colorField() const { return colorField_; }
    const QStringList &sports() const { return sports_; }
    const QVector<CalendarField> &metadataFields() const
    {
        return calendarFields_;
    }

    static QStringList globalSettingKeys();
    static QStringList athleteSettingKeys();

    QVariant globalSetting(
        const QString &key, const QVariant &defaultValue = {}) const;
    QVariant athleteSetting(
        const QString &key, const QVariant &defaultValue = {}) const;
    QColor colorFor(const QString &text) const;
    QString calendarText(
        const TextLookup &text,
        const MetricLookup &metric,
        const MetricRelevance &relevant) const;

    const RideMetricRegistrySnapshot *metricRegistry() const
    {
        return metricRegistry_.get();
    }
    const RideRefreshZones *zones() const { return zones_.get(); }
    const RideRefreshMeasures *measures() const { return measures_.get(); }

private:
    RideRefreshEnvironment(
        quint64 generation,
        Settings settings,
        bool useMetricUnits,
        QString colorField,
        QMap<QString, QColor> colorRules,
        QVector<CalendarField> calendarFields,
        QStringList sports,
        std::shared_ptr<const RideMetricRegistrySnapshot> metricRegistry,
        std::shared_ptr<const RideRefreshZones> zones,
        std::shared_ptr<const RideRefreshMeasures> measures);

    static QString formatCalendarField(
        const CalendarField &field, const QString &value);

    const quint64 generation_;
    const Settings settings_;
    const bool useMetricUnits_;
    const QString colorField_;
    const QMap<QString, QColor> colorRules_;
    const QVector<CalendarField> calendarFields_;
    const QStringList sports_;
    const std::shared_ptr<const RideMetricRegistrySnapshot> metricRegistry_;
    const std::shared_ptr<const RideRefreshZones> zones_;
    const std::shared_ptr<const RideRefreshMeasures> measures_;
};

std::shared_ptr<const RideRefreshEnvironment>
captureRideRefreshEnvironment(Context *context, quint64 generation);

#endif // GC_RIDEREFRESHENVIRONMENT_H
