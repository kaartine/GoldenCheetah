/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshEnvironment.h"

#include "RideRefreshMeasures.h"
#include "RideRefreshCacheInputs.h"
#include "RideRefreshRoutes.h"
#include "RideRefreshZones.h"

#include <QDate>
#include <QDir>
#include <QTime>

#include <cmath>
#include <limits>
#include <utility>

bool RideRefreshEnvironment::StoragePaths::isComplete() const
{
    return QDir::isAbsolutePath(cache)
        && QDir::isAbsolutePath(activities)
        && QDir::isAbsolutePath(planned);
}

QStringList RideRefreshEnvironment::globalSettingKeys()
{
    return {
        QStringLiteral("<global-general>unit"),
        QStringLiteral("<global-general>pace"),
        QStringLiteral("<global-general>swimpace"),
        QStringLiteral("<global-general>elevationHysteresis"),
        QStringLiteral("<global-general>wbal/formula"),
        QStringLiteral("<global-general>garminSmartRecord"),
        QStringLiteral("<global-general>garminHWMark"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_max"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_min"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_filt"),
        QStringLiteral("<global-general>dataprocess/filterhrv/rr_window")
    };
}

QStringList RideRefreshEnvironment::athleteSettingKeys()
{
    return {
        QStringLiteral("<athlete-preferences>weight"),
        QStringLiteral("<athlete-preferences>height"),
        QStringLiteral("<athlete-preferences>crankLength"),
        QStringLiteral("<athlete-preferences>intervals/discovery"),
        QStringLiteral("<athlete-preferences>dob"),
        QStringLiteral("<athlete-preferences>sex"),
        QStringLiteral("<athlete-preferences>wbaltau"),
        QStringLiteral("<athlete-preferences>wheelsize")
    };
}

std::shared_ptr<const RideRefreshEnvironment>
RideRefreshEnvironment::create(
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
    StoragePaths storagePaths)
{
    return std::shared_ptr<const RideRefreshEnvironment>(
        new RideRefreshEnvironment(
            generation,
            std::move(settings),
            useMetricUnits,
            std::move(colorField),
            std::move(colorRules),
            std::move(calendarFields),
            std::move(sports),
            std::move(metricRegistry),
            std::move(zones),
            std::move(measures),
            std::move(routes),
            std::move(storagePaths)));
}

RideRefreshEnvironment::RideRefreshEnvironment(
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
    StoragePaths storagePaths)
    : generation_(generation)
    , settings_(std::move(settings))
    , useMetricUnits_(useMetricUnits)
    , colorField_(std::move(colorField))
    , colorRules_(std::move(colorRules))
    , calendarFields_(std::move(calendarFields))
    , sports_(std::move(sports))
    , metricRegistry_(std::move(metricRegistry))
    , zones_(std::move(zones))
    , measures_(std::move(measures))
    , routes_(std::move(routes))
    , storagePaths_(std::move(storagePaths))
{
}

QVariant RideRefreshEnvironment::globalSetting(
    const QString &key, const QVariant &defaultValue) const
{
    const auto found = settings_.global.constFind(key);
    return found == settings_.global.cend() || !found->isValid()
        ? defaultValue : *found;
}

QVariant RideRefreshEnvironment::athleteSetting(
    const QString &key, const QVariant &defaultValue) const
{
    const auto found = settings_.athlete.constFind(key);
    return found == settings_.athlete.cend() || !found->isValid()
        ? defaultValue : *found;
}

QColor RideRefreshEnvironment::colorFor(const QString &text) const
{
    QColor color(1, 1, 1, 1);
    for (auto rule = colorRules_.cbegin();
         rule != colorRules_.cend(); ++rule) {
        if (text.contains(rule.key(), Qt::CaseInsensitive))
            color = rule.value();
    }
    return color;
}

QString RideRefreshEnvironment::formatCalendarField(
    const CalendarField &field, const QString &value)
{
    if (value.isEmpty() || !field.diary) return {};

    // Numeric values match GcFieldType's FIELD_INTEGER through
    // FIELD_CHECKBOX range. Keeping this value object independent of
    // RideMetadata prevents QWidget code from entering refresh workers.
    switch (field.type) {
    case 6: // FIELD_TIME
        if (field.name == QStringLiteral("Start Time")) {
            return QStringLiteral("%1: %2\n").arg(field.name).arg(
                QTime(0, 0, 0).addSecs(value.toInt())
                    .toString(QStringLiteral("hh:mm:ss.zzz")));
        }
        Q_FALLTHROUGH();
    case 5: // FIELD_DATE
        if (field.name == QStringLiteral("Start Date")) {
            return QStringLiteral("%1: %2\n").arg(field.name).arg(
                QDate(1900, 1, 1).addDays(value.toInt())
                    .toString(QStringLiteral("dd/MM/yyyy")));
        }
        Q_FALLTHROUGH();
    case 3: // FIELD_INTEGER
    case 4: // FIELD_DOUBLE
    case 7: // FIELD_CHECKBOX
        return QStringLiteral("%1: %2\n").arg(field.name, value);
    default:
        return QStringLiteral("%1\n").arg(value);
    }
}

QString RideRefreshEnvironment::calendarText(
    const TextLookup &text,
    const MetricLookup &metric,
    const MetricRelevance &relevant) const
{
    QString result;
    for (const CalendarField &field : calendarFields_) {
        QString value;
        if (!field.metricSymbol.isEmpty()) {
            if (!relevant || relevant(field.metricSymbol))
                value = metric(field.metricSymbol, useMetricUnits_);
        } else {
            value = text(field.valueName);
        }
        result += formatCalendarField(field, value);
    }
    return result;
}

std::optional<unsigned long> RideRefreshEnvironment::rideItemFingerprint(
    const QDate &date, const QString &sport, bool isSwim) const
{
    if (!zones_ || !measures_ || !routes_) return std::nullopt;
    const auto *power = zones_->power(sport);
    const auto *heartRate = zones_->heartRate(sport);
    const auto *pace = zones_->pace(isSwim);
    const auto hrv = measures_->fingerprint(
        QStringLiteral("Hrv"), date);
    if (!power || !heartRate || !pace || !hrv) return std::nullopt;

    unsigned long fingerprint =
        static_cast<unsigned long>(power->fingerprint(date));
    fingerprint += power->rawCpForFtpSetting ? 1UL : 0UL;
    fingerprint += static_cast<unsigned long>(pace->fingerprint(date));
    fingerprint += static_cast<unsigned long>(heartRate->fingerprint(date));
    fingerprint += static_cast<unsigned long>(routes_->fingerprint());
    fingerprint += static_cast<unsigned long>(*hrv);
    fingerprint += athleteSetting(
        QStringLiteral("<athlete-preferences>intervals/discovery"), 57)
                       .toInt();
    return fingerprint;
}

std::optional<double> RideRefreshEnvironment::rideItemWeight(
    const QDate &date, const QString &metadataWeight) const
{
    if (!measures_) return std::nullopt;

    const auto valid = [](double value) {
        return value > 0.0
            && RideRefreshEnvironment::rideItemWeightMilligrams(value);
    };
    double resolved = 0.0;
    if (measures_->group(QStringLiteral("Body"))) {
        const auto *observation = measures_->observationForDate(
            QStringLiteral("Body"), date);
        if (observation) resolved = observation->values[0];
    }
    if (!valid(resolved)) resolved = metadataWeight.toDouble();
    if (!valid(resolved)) {
        resolved = athleteSetting(
            QStringLiteral("<athlete-preferences>weight"),
            QStringLiteral("75.0")).toString().toDouble();
    }
    if (!valid(resolved)) resolved = 80.0;
    return resolved;
}

std::optional<unsigned long> RideRefreshEnvironment::rideItemWeightMilligrams(
    double kilograms)
{
    const double milligrams = 1000.0f * kilograms;
    if (!std::isfinite(milligrams) || milligrams < 0.0
        || static_cast<long double>(milligrams)
            > static_cast<long double>(
                std::numeric_limits<unsigned long>::max())) {
        return std::nullopt;
    }
    return static_cast<unsigned long>(milligrams);
}

QByteArray RideRefreshEnvironment::rideFileCacheAnalysisFingerprint(
    const QDate &date,
    const QString &sport,
    bool isSwim,
    double weight) const
{
    RideRefreshCacheSettings settings;
    settings.wbalFormula = globalSetting(
        QStringLiteral("<global-general>wbal/formula"),
        QStringLiteral("int")).toString();
    settings.wbalTau = athleteSetting(
        QStringLiteral("<athlete-preferences>wbaltau"), 300).toInt();
    settings.wheelSize = athleteSetting(
        QStringLiteral("<athlete-preferences>wheelsize"), 2100).toInt();
    return rideRefreshCacheAnalysisFingerprint(
        zones_.get(), settings, date, sport, isSwim, weight);
}
