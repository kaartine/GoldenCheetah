/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainPerspectiveState.h"

#include "RealtimeData.h"

#include <QDomDocument>

namespace {

// These identifiers are part of the persisted train perspective XML format.
constexpr int TelemetryChartId = 22;
constexpr int ErgTrainSwitch = 1;
constexpr int SlopeTrainSwitch = 2;
constexpr int MapTrainSwitch = 4;

QDomElement chartProperty(const QDomElement &chart, const QString &name)
{
    for (QDomElement property = chart.firstChildElement("property");
         !property.isNull();
         property = property.nextSiblingElement("property")) {
        if (property.attribute("name") == name) return property;
    }
    return QDomElement();
}

int chartDataSeries(const QDomElement &chart)
{
    const QDomElement property = chartProperty(chart, "dataSeries");
    if (property.isNull()) return -1;

    bool ok = false;
    const int value = property.attribute("value").toInt(&ok);
    return ok ? value : -1;
}

QList<QDomElement> leadingTelemetryCharts(const QDomElement &layout)
{
    QList<QDomElement> charts;
    for (QDomElement chart = layout.firstChildElement("chart");
         !chart.isNull();
         chart = chart.nextSiblingElement("chart")) {
        if (chart.attribute("id").toInt() != TelemetryChartId) break;
        charts.append(chart);
    }
    return charts;
}

bool containsDataSeries(const QDomElement &layout, int seriesIndex)
{
    for (QDomElement chart = layout.firstChildElement("chart");
         !chart.isNull();
         chart = chart.nextSiblingElement("chart")) {
        if (chartDataSeries(chart) == seriesIndex) return true;
    }
    return false;
}

QDomElement layoutForTrainSwitch(const QDomDocument &document, const QString &trainSwitch)
{
    const QDomElement root = document.documentElement();
    for (QDomElement layout = root.firstChildElement("layout");
         !layout.isNull();
         layout = layout.nextSiblingElement("layout")) {
        if (layout.attribute("trainswitch") == trainSwitch) return layout;
    }
    return QDomElement();
}

int selectorIndex(RealtimeData::DataSeries series)
{
    return RealtimeData::listDataSeries().indexOf(series);
}

QList<int> expectedTelemetrySeries(const QString &trainSwitch)
{
    if (trainSwitch == QString::number(ErgTrainSwitch)) {
        return QList<int>() << selectorIndex(RealtimeData::Watts)
                            << selectorIndex(RealtimeData::Load)
                            << selectorIndex(RealtimeData::BikeStress)
                            << selectorIndex(RealtimeData::Wbal)
                            << selectorIndex(RealtimeData::Speed)
                            << selectorIndex(RealtimeData::Cadence);
    }

    if (trainSwitch == QString::number(SlopeTrainSwitch) ||
        trainSwitch == QString::number(MapTrainSwitch)) {
        return QList<int>() << selectorIndex(RealtimeData::Watts)
                            << selectorIndex(RealtimeData::Slope)
                            << selectorIndex(RealtimeData::BikeStress)
                            << selectorIndex(RealtimeData::Wbal)
                            << selectorIndex(RealtimeData::Speed)
                            << selectorIndex(RealtimeData::Cadence);
    }

    return QList<int>();
}

}

bool
TrainPerspectiveState::migrate(QString &content, const QString &defaultsContent)
{
    QDomDocument document;
    if (!document.setContent(content)) return false;

    QDomElement root = document.documentElement();
    if (root.tagName() != "layouts") return false;

    bool versionOk = false;
    const int version = root.attribute("version").toInt(&versionOk);
    if (versionOk && version >= CurrentVersion) return false;

    QDomDocument defaults;
    if (!defaults.setContent(defaultsContent) ||
        defaults.documentElement().tagName() != "layouts") return false;

    const int heartRateIndex = selectorIndex(RealtimeData::HeartRate);
    for (QDomElement layout = root.firstChildElement("layout");
         !layout.isNull();
         layout = layout.nextSiblingElement("layout")) {
        const QString trainSwitch = layout.attribute("trainswitch");
        const QList<int> expectedSeries = expectedTelemetrySeries(trainSwitch);
        if (expectedSeries.isEmpty() || containsDataSeries(layout, heartRateIndex)) continue;

        const QList<QDomElement> telemetryCharts = leadingTelemetryCharts(layout);
        QList<int> actualSeries;
        foreach (const QDomElement &chart, telemetryCharts) {
            actualSeries.append(chartDataSeries(chart));
        }
        if (actualSeries != expectedSeries) continue;

        const QDomElement defaultLayout = layoutForTrainSwitch(defaults, trainSwitch);
        QDomElement defaultHeartRate;
        for (QDomElement chart = defaultLayout.firstChildElement("chart");
             !chart.isNull();
             chart = chart.nextSiblingElement("chart")) {
            if (chartDataSeries(chart) == heartRateIndex) {
                defaultHeartRate = chart;
                break;
            }
        }
        if (defaultHeartRate.isNull()) continue;

        const QString width = chartProperty(defaultHeartRate, "widthFactor").attribute("value");
        if (!width.isEmpty()) {
            foreach (const QDomElement &chart, telemetryCharts) {
                QDomElement widthProperty = chartProperty(chart, "widthFactor");
                if (!widthProperty.isNull()) widthProperty.setAttribute("value", width);
            }
        }

        const QDomNode importedHeartRate = document.importNode(defaultHeartRate, true);
        const QDomElement insertionPoint = telemetryCharts.last().nextSiblingElement("chart");
        if (insertionPoint.isNull()) layout.appendChild(importedHeartRate);
        else layout.insertBefore(importedHeartRate, insertionPoint);
    }

    root.setAttribute("version", CurrentVersion);
    content = document.toString(1);
    return true;
}
