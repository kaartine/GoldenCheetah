/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCoursePreviewWidget.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

WorkoutGameCoursePreviewWidget::WorkoutGameCoursePreviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("coursePreview"));
    setAccessibleName(tr("MTB course preview"));
    setMinimumHeight(250);
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
    return {620, 250};
}

void WorkoutGameCoursePreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const QRectF chart = QRectF(rect()).adjusted(54.0, 22.0, -58.0, -36.0);
    if (chart.width() <= 1.0 || chart.height() <= 1.0) return;

    const QColor grid = palette().color(QPalette::Mid).lighter(125);
    painter.setPen(QPen(grid, 1.0));
    for (int line = 0; line <= 4; ++line) {
        const double y = chart.top() + chart.height() * line / 4.0;
        painter.drawLine(QPointF(chart.left(), y), QPointF(chart.right(), y));
    }

    if (currentResult.status != WorkoutGameCourseSourceStatus::Ready
            || currentResult.document.course.sections.empty()) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(chart, Qt::AlignCenter, tr("Preview unavailable"));
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
        maximumPower = std::max({maximumPower,
                section.targetStartWatts, section.targetEndWatts});
    }
    const double elevationRange = std::max(
            1.0, maximumElevation - minimumElevation);
    const auto xForDistance = [&](double meters) {
        return chart.left() + chart.width()
                * meters / course.totalDistanceMeters;
    };
    const auto yForElevation = [&](double meters) {
        return chart.bottom() - chart.height()
                * (meters - minimumElevation) / elevationRange;
    };
    const auto yForPower = [&](double watts) {
        return chart.bottom() - chart.height() * watts / maximumPower;
    };

    QPainterPath elevation;
    elevation.moveTo(chart.left(), chart.bottom());
    elevation.lineTo(chart.left(), yForElevation(
            course.sections.front().startElevationMeters));
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        elevation.lineTo(
                xForDistance(section.startDistanceMeters + section.lengthMeters),
                yForElevation(section.endElevationMeters));
    }
    elevation.lineTo(chart.right(), chart.bottom());
    elevation.closeSubpath();
    painter.fillPath(elevation, QColor(58, 124, 86, 90));
    painter.setPen(QPen(QColor(42, 112, 73), 2.5));
    painter.drawPath(elevation);

    QPainterPath power;
    bool firstPoint = true;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        const QPointF start(
                xForDistance(section.startDistanceMeters),
                yForPower(section.targetStartWatts));
        const QPointF end(
                xForDistance(section.startDistanceMeters + section.lengthMeters),
                yForPower(section.targetEndWatts));
        if (firstPoint) {
            power.moveTo(start);
            firstPoint = false;
        } else {
            power.lineTo(start);
        }
        power.lineTo(end);
    }
    painter.setPen(QPen(QColor(218, 139, 42), 2.5));
    painter.drawPath(power);

    painter.setPen(palette().color(QPalette::Text));
    const QFontMetrics metrics(painter.font());
    painter.drawText(
            QRectF(0.0, chart.top(), 48.0, metrics.height()),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 m").arg(std::lround(maximumElevation)));
    painter.drawText(
            QRectF(chart.right() + 6.0, chart.top(), 52.0, metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("%1 W").arg(std::lround(maximumPower)));
    painter.drawText(
            QRectF(chart.left(), chart.bottom() + 8.0,
                   chart.width(), metrics.height()),
            Qt::AlignHCenter | Qt::AlignTop,
            QStringLiteral("%1 km").arg(
                course.totalDistanceMeters / 1000.0, 0, 'f', 1));
}
