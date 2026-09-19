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

#include <QByteArray>
#include <QColor>
#include <QDate>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class Context;
class RideMetricRegistrySnapshot;
class RideRefreshMeasures;
class RideRefreshRoutes;
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

    struct StoragePaths {
        QString cache;
        QString activities;
        QString planned;

        bool isComplete() const;
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
        std::shared_ptr<const RideRefreshMeasures> measures = {},
        std::shared_ptr<const RideRefreshRoutes> routes = {},
        StoragePaths storagePaths = {});

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
    std::optional<unsigned long> rideItemFingerprint(
        const QDate &date, const QString &sport, bool isSwim) const;
    std::optional<double> rideItemWeight(
        const QDate &date, const QString &metadataWeight) const;
    static std::optional<unsigned long> rideItemWeightMilligrams(
        double kilograms);
    QByteArray rideFileCacheAnalysisFingerprint(
        const QDate &date,
        const QString &sport,
        bool isSwim,
        double weight) const;
    const StoragePaths &storagePaths() const { return storagePaths_; }

    const RideMetricRegistrySnapshot *metricRegistry() const
    {
        return metricRegistry_.get();
    }
    const RideRefreshZones *zones() const { return zones_.get(); }
    const RideRefreshMeasures *measures() const { return measures_.get(); }
    const RideRefreshRoutes *routes() const { return routes_.get(); }

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
        std::shared_ptr<const RideRefreshMeasures> measures,
        std::shared_ptr<const RideRefreshRoutes> routes,
        StoragePaths storagePaths);

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
    const std::shared_ptr<const RideRefreshRoutes> routes_;
    const StoragePaths storagePaths_;
};

template<typename LegacyColor>
QColor rideRefreshBuildColor(
    const RideRefreshEnvironment *environment,
    const QString &colorText,
    LegacyColor &&legacyColor)
{
    if (environment) return environment->colorFor(colorText);
    return legacyColor();
}

std::shared_ptr<const RideRefreshEnvironment>
captureRideRefreshEnvironment(Context *context, quint64 generation);

#endif // GC_RIDEREFRESHENVIRONMENT_H
