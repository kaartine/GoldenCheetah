/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CloudDBChartImportPolicy.h"

#include "Gui/GcWindowTypes.h"

namespace {

bool isExecutableType(int type)
{
    switch (type) {
    case GcWindowTypes::RConsole:
    case GcWindowTypes::RConsoleSeason:
    case GcWindowTypes::Python:
    case GcWindowTypes::PythonSeason:
        return true;
    default:
        return false;
    }
}

bool hasExecutableProperty(
    const QMap<QString, QString> &properties)
{
    for (auto property = properties.constBegin();
         property != properties.constEnd(); ++property) {
        if (property.key().compare(
                QStringLiteral("script"),
                Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace CloudDBChartImportPolicy {

Decision evaluate(
    const QList<QMap<QString, QString>> &chartProperties)
{
    if (chartProperties.isEmpty()) {
        return Decision::RejectMalformed;
    }

    for (const QMap<QString, QString> &properties : chartProperties) {
        bool typeIsInteger = false;
        const int type = properties.value(
            QStringLiteral("TYPE")).toInt(&typeIsInteger);
        if (!typeIsInteger || type <= GcWindowTypes::None) {
            return Decision::RejectMalformed;
        }
        if (isExecutableType(type)) {
            return Decision::RejectExecutableType;
        }
        if (hasExecutableProperty(properties)) {
            return Decision::RejectExecutableProperty;
        }
    }

    return Decision::Allow;
}

} // namespace CloudDBChartImportPolicy
