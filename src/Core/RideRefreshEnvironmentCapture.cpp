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
#include "RideRefreshRoutes.h"
#include "RideRefreshZones.h"

#include "Athlete.h"
#include "Context.h"
#include "RideMetadata.h"
#include "RideMetric.h"
#include "Settings.h"
#include "SpecialFields.h"
#include "Zones.h"

#include <QCoreApplication>
#include <QThread>

#include <utility>

namespace {

void captureGlobalSetting(
    RideRefreshEnvironment::Settings &settings, const QString &key)
{
    settings.global.insert(
        key, appsettings->value(nullptr, key, QVariant()));
}

void captureAthleteSetting(
    RideRefreshEnvironment::Settings &settings,
    const QString &athlete,
    const QString &key)
{
    settings.athlete.insert(
        key, appsettings->cvalue(athlete, key, QVariant()));
}

} // namespace

std::shared_ptr<const RideRefreshEnvironment>
captureRideRefreshEnvironment(Context *context, quint64 generation)
{
    Q_ASSERT(context);
    Q_ASSERT(context->athlete);
    Q_ASSERT(QThread::currentThread() == context->thread());
    Q_ASSERT(RideRefreshEnvironment::globalSettingKeys() == QStringList({
        QString(GC_UNIT), QString(GC_PACE), QString(GC_SWIMPACE),
        QString(GC_ELEVATION_HYSTERESIS), QString(GC_WBALFORM),
        QString(GC_GARMIN_SMARTRECORD), QString(GC_GARMIN_HWMARK),
        QString(GC_RR_MAX), QString(GC_RR_MIN), QString(GC_RR_FILT),
        QString(GC_RR_WINDOW)}));
    Q_ASSERT(RideRefreshEnvironment::athleteSettingKeys() == QStringList({
        QString(GC_WEIGHT), QString(GC_HEIGHT), QString(GC_CRANKLENGTH),
        QString(GC_DISCOVERY), QString(GC_DOB), QString(GC_SEX),
        QString(GC_WBALTAU), QString(GC_WHEELSIZE)}));

    RideRefreshEnvironment::Settings settings;
    for (const QString &key : RideRefreshEnvironment::globalSettingKeys()) {
        captureGlobalSetting(settings, key);
    }

    const QString athlete = context->athlete->cyclist;
    for (const QString &key : RideRefreshEnvironment::athleteSettingKeys()) {
        captureAthleteSetting(settings, athlete, key);
    }

    RideMetadata *metadata = GlobalContext::context()->rideMetadata;
    const QList<FieldDefinition> definitions = metadata->getFields();
    QVector<RideRefreshEnvironment::CalendarField> calendarFields;
    calendarFields.reserve(definitions.size());
    QStringList sports;
    for (const FieldDefinition &definition : definitions) {
        QString fieldName = definition.name == QStringLiteral("Weight")
            ? QStringLiteral("Athlete Weight") : definition.name;
        QString metricSymbol;
        if (SpecialFields::getInstance().isMetric(fieldName)) {
            const RideMetric *metric =
                SpecialFields::getInstance().rideMetric(fieldName);
            if (metric) metricSymbol = metric->symbol();
        }
        calendarFields.append({
            definition.name,
            fieldName,
            int(definition.type),
            definition.diary,
            metricSymbol,
            definition.interval
        });
        if (definition.name == QStringLiteral("Sport"))
            sports = definition.values;
    }
    bool hasBike = false;
    for (const QString &sport : sports) {
        if (RideFile::sportTag(sport) == QStringLiteral("Bike")) {
            hasBike = true;
            break;
        }
    }
    if (!hasBike) {
        sports.prepend(QCoreApplication::translate(
            "RideMetadata", "Bike"));
    }

    QMap<QString, QColor> colorRules;
    for (const KeywordDefinition &keyword : metadata->getKeywords()) {
        if (keyword.name == QStringLiteral("Default")
            || keyword.name == QStringLiteral("Reverse")) {
            continue;
        }
        colorRules.insert(keyword.name, keyword.color);
        for (const QString &token : keyword.tokens)
            colorRules.insert(token, keyword.color);
    }

    // Dynamic CP/FTP setting names are part of the environment even before
    // the value-only zone snapshot in F2b starts consuming them.
    for (const Zones *zones : context->athlete->zones_) {
        if (zones) {
            captureAthleteSetting(
                settings, athlete, zones->useCPforFTPSetting());
        }
    }

    const bool useMetricUnits = GlobalContext::context()->useMetricUnits;
    const auto zones = captureRideRefreshZones(
        context->athlete,
        settings.global,
        settings.athlete,
        useMetricUnits);
    Q_ASSERT(zones);
    const auto measures = captureRideRefreshMeasures(context->athlete);
    Q_ASSERT(measures);
    const auto routes = captureRideRefreshRoutes(context->athlete->routes);
    Q_ASSERT(routes);
    Q_ASSERT(context->athlete->home);

    return RideRefreshEnvironment::create(
        generation,
        std::move(settings),
        useMetricUnits,
        metadata->getColorField(),
        std::move(colorRules),
        std::move(calendarFields),
        std::move(sports),
        std::make_shared<const RideMetricRegistrySnapshot>(
            RideMetricFactory::instance().snapshot()),
        zones,
        measures,
        routes,
        {
            context->athlete->home->cache().absolutePath(),
            context->athlete->home->activities().absolutePath(),
            context->athlete->home->planned().absolutePath()
        });
}
