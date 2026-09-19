/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshEnvironment.h"

#include <QDate>
#include <QTime>

#include <utility>

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
    std::shared_ptr<const RideRefreshMeasures> measures)
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
            std::move(measures)));
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
    std::shared_ptr<const RideRefreshMeasures> measures)
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
