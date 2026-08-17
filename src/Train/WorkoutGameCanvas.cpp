/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCanvas.h"

#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int TargetFrameMs = 33;

QColor skyColor() { return QColor(99, 190, 187); }
QColor distantColor() { return QColor(55, 102, 110); }
QColor forestColor() { return QColor(38, 85, 61); }
QColor dirtColor() { return QColor(145, 92, 52); }
QColor dirtHighlightColor() { return QColor(220, 177, 99); }
QColor inkColor() { return QColor(20, 27, 31); }

}

WorkoutGameCanvas::WorkoutGameCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setAutoFillBackground(false);
    animationTimer.setInterval(TargetFrameMs);
    connect(&animationTimer, &QTimer::timeout, this, [this]() {
        animationFrame = (animationFrame + 1) % 120;
        update();
    });
}

void WorkoutGameCanvas::setCourse(const WorkoutGameCourse &newCourse)
{
    course = newCourse;
    update();
}

void WorkoutGameCanvas::setSnapshot(
        const WorkoutGameSimulationSnapshot &snapshot,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    current = snapshot;
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    update();
}

void WorkoutGameCanvas::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    animationTimer.start();
}

void WorkoutGameCanvas::hideEvent(QHideEvent *event)
{
    animationTimer.stop();
    QWidget::hideEvent(event);
}

QString WorkoutGameCanvas::featureName(WorkoutGameFeature feature) const
{
    switch (feature) {
    case WorkoutGameFeature::WarmupTrail: return tr("WARM UP");
    case WorkoutGameFeature::Trail: return tr("TRAIL");
    case WorkoutGameFeature::FlowTrail: return tr("FLOW");
    case WorkoutGameFeature::Climb: return tr("CLIMB");
    case WorkoutGameFeature::SprintJump: return tr("JUMP");
    case WorkoutGameFeature::RecoveryDescent: return tr("DESCENT");
    case WorkoutGameFeature::CooldownDescent: return tr("COOL DOWN");
    }
    return QString();
}

double WorkoutGameCanvas::trailY(double x, const QRect &scene) const
{
    double grade = 0.0;
    std::uint32_t variant = 0;
    if (current.activeSection >= 0
            && current.activeSection < int(course.sections.size())) {
        const WorkoutGameSection &section = course.sections[current.activeSection];
        grade = section.gradePercent;
        variant = section.visualVariant;
    }

    const double normalizedX = (x - scene.left()) / std::max(1, scene.width());
    const double base = scene.top() + scene.height() * 0.67;
    const double gradeOffset = -grade * scene.height() * 0.018
            * (normalizedX - 0.28);
    const int steps = 8 + int(variant % 4u);
    const double pixelWave = ((int(normalizedX * steps) + int(variant)) % 3 - 1)
            * scene.height() * 0.012;
    return base + gradeOffset + pixelWave;
}

void WorkoutGameCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), skyColor());

    const int hudHeight = std::clamp(height() / 7, 42, 72);
    const QRect scene(0, hudHeight, width(), height() - hudHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(distantColor());
    QPolygon distant;
    distant << QPoint(scene.left(), scene.top() + scene.height() * 0.45)
            << QPoint(scene.left() + scene.width() * 0.16, scene.top() + scene.height() * 0.20)
            << QPoint(scene.left() + scene.width() * 0.34, scene.top() + scene.height() * 0.42)
            << QPoint(scene.left() + scene.width() * 0.56, scene.top() + scene.height() * 0.15)
            << QPoint(scene.left() + scene.width() * 0.78, scene.top() + scene.height() * 0.40)
            << QPoint(scene.right(), scene.top() + scene.height() * 0.24)
            << QPoint(scene.right(), scene.bottom())
            << QPoint(scene.left(), scene.bottom());
    painter.drawPolygon(distant);

    painter.setBrush(forestColor());
    const int treeWidth = std::clamp(width() / 24, 18, 46);
    for (int x = -treeWidth; x < width() + treeWidth; x += treeWidth) {
        const int offset = (x / treeWidth + animationFrame / 12) % 3;
        const int baseY = scene.top() + int(scene.height() * (0.48 + offset * 0.025));
        QPolygon tree;
        tree << QPoint(x, baseY)
             << QPoint(x + treeWidth / 2, baseY - treeWidth * 2)
             << QPoint(x + treeWidth, baseY)
             << QPoint(x + treeWidth * 3 / 4, baseY)
             << QPoint(x + treeWidth * 3 / 4, baseY + treeWidth)
             << QPoint(x + treeWidth / 4, baseY + treeWidth)
             << QPoint(x + treeWidth / 4, baseY);
        painter.drawPolygon(tree);
    }

    QPainterPath ground;
    ground.moveTo(scene.left(), trailY(scene.left(), scene));
    const int sampleWidth = std::max(4, width() / 100);
    for (int x = scene.left() + sampleWidth; x <= scene.right(); x += sampleWidth) {
        ground.lineTo(x, trailY(x, scene));
    }
    ground.lineTo(scene.right(), scene.bottom());
    ground.lineTo(scene.left(), scene.bottom());
    ground.closeSubpath();
    painter.setBrush(dirtColor());
    painter.drawPath(ground);

    painter.setPen(QPen(dirtHighlightColor(), std::clamp(height() / 90, 3, 8)));
    for (int x = scene.left(); x < scene.right(); x += sampleWidth) {
        painter.drawLine(
                QPointF(x, trailY(x, scene)),
                QPointF(x + sampleWidth, trailY(x + sampleWidth, scene)));
    }

    if (current.activeSection >= 0
            && current.activeSection < int(course.sections.size())
            && course.sections[current.activeSection].feature
                    == WorkoutGameFeature::SprintJump) {
        const int obstacleX = int(width() * 0.72);
        const int obstacleY = int(trailY(obstacleX, scene));
        painter.setPen(Qt::NoPen);
        painter.setBrush(current.route == WorkoutGameRoute::SafeBypass
                ? QColor(232, 197, 78)
                : QColor(190, 62, 48));
        QPolygon ramp;
        ramp << QPoint(obstacleX - 24, obstacleY)
             << QPoint(obstacleX + 18, obstacleY - 30)
             << QPoint(obstacleX + 38, obstacleY)
             << QPoint(obstacleX - 24, obstacleY);
        painter.drawPolygon(ramp);
        if (current.route == WorkoutGameRoute::SafeBypass) {
            painter.setPen(QPen(QColor(245, 230, 178), 4));
            painter.drawLine(obstacleX - 28, obstacleY + 16,
                             obstacleX + 54, obstacleY + 16);
        }
    }

    const int riderX = int(width() * 0.28);
    const int riderGround = int(trailY(riderX, scene));
    const int wheelRadius = std::clamp(height() / 30, 7, 18);
    painter.setPen(QPen(inkColor(), std::max(2, wheelRadius / 4)));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPoint(riderX - wheelRadius * 2, riderGround - wheelRadius),
                        wheelRadius, wheelRadius);
    painter.drawEllipse(QPoint(riderX + wheelRadius * 2, riderGround - wheelRadius),
                        wheelRadius, wheelRadius);
    painter.drawLine(riderX - wheelRadius * 2, riderGround - wheelRadius,
                     riderX, riderGround - wheelRadius * 2);
    painter.drawLine(riderX, riderGround - wheelRadius * 2,
                     riderX + wheelRadius * 2, riderGround - wheelRadius);
    painter.drawLine(riderX - wheelRadius * 2, riderGround - wheelRadius,
                     riderX + wheelRadius, riderGround - wheelRadius);
    painter.setBrush(QColor(235, 74, 66));
    painter.setPen(Qt::NoPen);
    painter.drawRect(riderX - wheelRadius / 2,
                     riderGround - wheelRadius * 4,
                     wheelRadius, wheelRadius * 2);
    painter.setBrush(QColor(242, 197, 132));
    painter.drawRect(riderX - wheelRadius / 2,
                     riderGround - wheelRadius * 5,
                     wheelRadius, wheelRadius);

    painter.fillRect(QRect(0, 0, width(), hudHeight), inkColor());
    painter.setPen(QColor(246, 239, 215));
    QFont hudFont = painter.font();
    hudFont.setPixelSize(14);
    hudFont.setBold(true);
    painter.setFont(hudFont);
    const QString stats = tr("%1 W   TARGET %2   %3 RPM   HR %4   GEAR %5")
            .arg(int(std::lround(watts)))
            .arg(int(std::lround(targetWatts)))
            .arg(cadenceRpm)
            .arg(heartRate)
            .arg(virtualGear);
    painter.drawText(QRect(14, 0, width() - 28, hudHeight / 2),
                     Qt::AlignLeft | Qt::AlignVCenter, stats);

    QString sectionText = tr("NO ERG WORKOUT");
    if (current.ready && current.activeSection >= 0
            && current.activeSection < int(course.sections.size())) {
        sectionText = featureName(course.sections[current.activeSection].feature);
    } else if (current.finished) {
        sectionText = tr("FINISH");
    }
    painter.setPen(QColor(232, 197, 78));
    painter.drawText(QRect(14, hudHeight / 2, width() / 2, hudHeight / 2),
                     Qt::AlignLeft | Qt::AlignVCenter, sectionText);
    painter.setPen(QColor(246, 239, 215));
    painter.drawText(QRect(width() / 2, hudHeight / 2, width() / 2 - 14, hudHeight / 2),
                     Qt::AlignRight | Qt::AlignVCenter,
                     tr("SCORE %1").arg(current.score));

    const int progressHeight = std::clamp(height() / 80, 4, 9);
    painter.fillRect(0, height() - progressHeight, width(), progressHeight,
                     QColor(29, 40, 43));
    painter.fillRect(0, height() - progressHeight,
                     int(width() * current.courseProgress), progressHeight,
                     QColor(232, 197, 78));
}
