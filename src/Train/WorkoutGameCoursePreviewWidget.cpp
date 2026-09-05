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

namespace {

QString elapsedTimeText(std::int64_t durationMs)
{
    const std::int64_t totalSeconds = std::max<std::int64_t>(
            0, (durationMs + 500) / 1000);
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = totalSeconds / 60 % 60;
    const std::int64_t seconds = totalSeconds % 60;
    return hours > 0
            ? QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'))
            : QStringLiteral("%1:%2")
                .arg(minutes)
                .arg(seconds, 2, 10, QLatin1Char('0'));
}

}

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
    const std::vector<WorkoutGameInterval> &sourceIntervals =
            currentResult.document.sourceIntervals;
    const std::int64_t sourceDurationMs = sourceIntervals.empty()
            ? 0
            : sourceIntervals.back().startMs
                + sourceIntervals.back().durationMs;
    double minimumElevation = 0.0;
    double maximumElevation = 0.0;
    double maximumPower = 1.0;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        minimumElevation = std::min({minimumElevation,
                section.startElevationMeters, section.endElevationMeters});
        maximumElevation = std::max({maximumElevation,
                section.startElevationMeters, section.endElevationMeters});
    }
    for (const WorkoutGameInterval &interval : sourceIntervals) {
        maximumPower = std::max({maximumPower,
                interval.startWatts, interval.endWatts});
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

    const std::vector<WorkoutGameCoursePreviewPoint> powerProfile =
            WorkoutGameCoursePreviewMetrics::workoutPowerProfile(
                sourceIntervals);
    QPainterPath power;
    for (std::size_t index = 0; index < powerProfile.size(); ++index) {
        const QPointF point(
                powerChart.left() + powerChart.width()
                    * powerProfile[index].progress,
                yForPower(powerProfile[index].value));
        if (index == 0u) power.moveTo(point);
        else power.lineTo(point);
    }
    painter.setPen(QPen(QColor(218, 139, 42), 2.5));
    painter.drawPath(power);

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
            tr("Original workout power - time"));
    painter.drawText(
            QRectF(powerChart.left(), powerChart.bottom() + 4.0,
                   powerChart.width(), metrics.height()),
            Qt::AlignLeft | Qt::AlignTop,
            QStringLiteral("0:00"));
    painter.drawText(
            QRectF(powerChart.left(), powerChart.bottom() + 4.0,
                   powerChart.width(), metrics.height()),
            Qt::AlignRight | Qt::AlignTop,
            elapsedTimeText(sourceDurationMs));
    painter.drawText(
            QRectF(terrainChart.left(), terrainChart.top() - metrics.height() - 3.0,
                   terrainChart.width(), metrics.height()),
            Qt::AlignHCenter | Qt::AlignBottom,
            tr("Generated terrain - distance"));
}
