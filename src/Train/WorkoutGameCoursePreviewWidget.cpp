/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCoursePreviewWidget.h"
#include "WorkoutGameCoursePreviewMetrics.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

WorkoutGameCoursePreviewWidget::WorkoutGameCoursePreviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("coursePreview"));
    setAccessibleName(tr("MTB course preview"));
    setMinimumHeight(330);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void WorkoutGameCoursePreviewWidget::setResult(
        const WorkoutGameCourseSourceResult &result)
{
    currentResult = result;
    update();
}

QSize WorkoutGameCoursePreviewWidget::minimumSizeHint() const
{
    return {620, 330};
}

void WorkoutGameCoursePreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const QRectF content = QRectF(rect()).adjusted(54.0, 22.0, -58.0, -30.0);
    if (content.width() <= 1.0 || content.height() <= 1.0) return;
    const double gap = 52.0;
    const double chartHeight = (content.height() - gap) * 0.5;
    const QRectF powerChart(content.left(), content.top(),
                            content.width(), chartHeight);
    const QRectF terrainChart(content.left(), powerChart.bottom() + gap,
                              content.width(), chartHeight);

    const QColor grid = palette().color(QPalette::Mid).lighter(125);
    painter.setPen(QPen(grid, 1.0));
    for (int line = 0; line <= 4; ++line) {
        const double powerY = powerChart.top()
                + powerChart.height() * line / 4.0;
        const double terrainY = terrainChart.top()
                + terrainChart.height() * line / 4.0;
        painter.drawLine(QPointF(powerChart.left(), powerY),
                         QPointF(powerChart.right(), powerY));
        painter.drawLine(QPointF(terrainChart.left(), terrainY),
                         QPointF(terrainChart.right(), terrainY));
    }

    if (currentResult.status != WorkoutGameCourseSourceStatus::Ready
            || currentResult.document.course.sections.empty()) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(content, Qt::AlignCenter, tr("Preview unavailable"));
        return;
    }

    const WorkoutGameDistanceCourse &course = currentResult.document.course;
    double minimumElevation = 0.0;
    double maximumElevation = 0.0;
    double maximumPower = 1.0;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        minimumElevation = std::min({minimumElevation,
                section.startElevationMeters, section.endElevationMeters});
        maximumElevation = std::max({maximumElevation,
                section.startElevationMeters, section.endElevationMeters});
    }
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        maximumPower = std::max({maximumPower,
                section.targetStartWatts, section.targetEndWatts,
                section.referenceEffortStartWatts,
                section.referenceEffortEndWatts});
    }
    const double elevationRange = std::max(
            1.0, maximumElevation - minimumElevation);
    const auto xForDistance = [&](double meters) {
        return terrainChart.left() + terrainChart.width()
                * meters / course.totalDistanceMeters;
    };
    const auto yForElevation = [&](double meters) {
        return terrainChart.bottom() - terrainChart.height()
                * (meters - minimumElevation) / elevationRange;
    };
    const auto yForPower = [&](double watts) {
        return powerChart.bottom() - powerChart.height() * watts / maximumPower;
    };

    QPainterPath elevation;
    elevation.moveTo(terrainChart.left(), terrainChart.bottom());
    elevation.lineTo(terrainChart.left(), yForElevation(
            course.sections.front().startElevationMeters));
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        elevation.lineTo(
                xForDistance(section.startDistanceMeters + section.lengthMeters),
                yForElevation(section.endElevationMeters));
    }
    elevation.lineTo(terrainChart.right(), terrainChart.bottom());
    elevation.closeSubpath();
    painter.fillPath(elevation, QColor(58, 124, 86, 90));
    painter.setPen(QPen(QColor(42, 112, 73), 2.5));
    painter.drawPath(elevation);

    QPainterPath sourcePower;
    QPainterPath terrainEffort;
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section = course.sections[index];
        const double referenceStart = section.referenceEffortStartWatts >= 0.0
                ? section.referenceEffortStartWatts : section.targetStartWatts;
        const double referenceEnd = section.referenceEffortEndWatts >= 0.0
                ? section.referenceEffortEndWatts : section.targetEndWatts;
        const QPointF sourceStart(
                xForDistance(section.startDistanceMeters),
                yForPower(section.targetStartWatts));
        const QPointF sourceEnd(
                xForDistance(section.startDistanceMeters + section.lengthMeters),
                yForPower(section.targetEndWatts));
        const QPointF effortStart(
                xForDistance(section.startDistanceMeters),
                yForPower(referenceStart));
        const QPointF effortEnd(
                xForDistance(section.startDistanceMeters + section.lengthMeters),
                yForPower(referenceEnd));
        if (index == 0u) {
            sourcePower.moveTo(sourceStart);
            terrainEffort.moveTo(effortStart);
        } else {
            sourcePower.lineTo(sourceStart);
            terrainEffort.lineTo(effortStart);
        }
        sourcePower.lineTo(sourceEnd);
        terrainEffort.lineTo(effortEnd);
    }
    painter.setPen(QPen(QColor(218, 139, 42), 2.0));
    painter.drawPath(sourcePower);
    painter.setPen(QPen(QColor(55, 120, 190), 2.5));
    painter.drawPath(terrainEffort);

    painter.setPen(QPen(QColor(120, 90, 150, 150), 1.0));
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        if (section.challengeCount <= 0) continue;
        const double x = xForDistance(
                section.startDistanceMeters + section.lengthMeters * 0.5);
        painter.drawLine(QPointF(x, terrainChart.top()),
                         QPointF(x, terrainChart.top() + 8.0));
    }

    painter.setPen(palette().color(QPalette::Text));
    const QFontMetrics metrics(painter.font());
    painter.drawText(
            QRectF(0.0, terrainChart.top(), 48.0, metrics.height()),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 m").arg(std::lround(maximumElevation)));
    painter.drawText(
            QRectF(powerChart.right() + 6.0, powerChart.top(),
                   52.0, metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("%1 W").arg(std::lround(maximumPower)));
    painter.drawText(
            QRectF(terrainChart.left(), terrainChart.bottom() + 6.0,
                   terrainChart.width(), metrics.height()),
            Qt::AlignHCenter | Qt::AlignTop,
            QStringLiteral("%1 km").arg(
                course.totalDistanceMeters / 1000.0, 0, 'f', 1));
    painter.drawText(
            QRectF(powerChart.left(), powerChart.top() - metrics.height() - 3.0,
                   powerChart.width(), metrics.height()),
            Qt::AlignHCenter | Qt::AlignBottom,
            tr("Workout baseline and generated terrain effort - distance"));
    painter.drawText(
            QRectF(powerChart.left(), powerChart.bottom() + 4.0,
                   powerChart.width(), metrics.height()),
            Qt::AlignLeft | Qt::AlignTop,
            QStringLiteral("0 km"));
    painter.drawText(
            QRectF(powerChart.left(), powerChart.bottom() + 4.0,
                   powerChart.width(), metrics.height()),
            Qt::AlignRight | Qt::AlignTop,
            QStringLiteral("%1 km").arg(
                course.totalDistanceMeters / 1000.0, 0, 'f', 1));
    painter.drawText(
            QRectF(terrainChart.left(), terrainChart.top() - metrics.height() - 3.0,
                   terrainChart.width(), metrics.height()),
            Qt::AlignHCenter | Qt::AlignBottom,
            tr("Generated terrain - distance"));
}
