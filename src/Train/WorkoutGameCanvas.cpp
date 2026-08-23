/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCanvas.h"
#include "WorkoutGameClock.h"
#include "WorkoutGamePowerProfile.h"
#include "WorkoutGameTrailScene.h"

#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QPixmap>
#include <QShowEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

QColor skyColor() { return QColor(99, 190, 187); }
QColor distantColor() { return QColor(55, 102, 110); }
QColor forestColor() { return QColor(38, 85, 61); }
QColor dirtColor() { return QColor(145, 92, 52); }
QColor dirtHighlightColor() { return QColor(220, 177, 99); }
QColor inkColor() { return QColor(20, 27, 31); }
QColor riderKeylineColor() { return QColor(246, 239, 215); }

struct TerrainPalette
{
    QColor ground;
    QColor highlight;
    QColor accent;
};

TerrainPalette terrainPalette(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::Roots:
        return {{113, 70, 43}, {201, 146, 76}, {67, 44, 32}};
    case WorkoutGameTerrainKind::Rollers:
        return {{151, 94, 48}, {236, 189, 91}, {91, 65, 39}};
    case WorkoutGameTerrainKind::Climb:
        return {{130, 76, 47}, {224, 159, 84}, {77, 63, 48}};
    case WorkoutGameTerrainKind::RockGarden:
        return {{105, 91, 77}, {184, 169, 132}, {57, 64, 63}};
    case WorkoutGameTerrainKind::BunnyHop:
        return {{139, 86, 48}, {232, 183, 82}, {190, 62, 48}};
    case WorkoutGameTerrainKind::Drop:
        return {{121, 77, 51}, {214, 157, 88}, {232, 197, 78}};
    case WorkoutGameTerrainKind::Skinny:
        return {{92, 78, 55}, {191, 154, 83}, {53, 47, 38}};
    case WorkoutGameTerrainKind::Berm:
        return {{151, 83, 50}, {228, 165, 84}, {72, 91, 76}};
    case WorkoutGameTerrainKind::LogOver:
        return {{123, 76, 45}, {217, 153, 75}, {73, 45, 29}};
    case WorkoutGameTerrainKind::Tabletop:
        return {{148, 91, 47}, {235, 185, 88}, {83, 70, 44}};
    case WorkoutGameTerrainKind::RockSlab:
        return {{101, 94, 82}, {188, 176, 145}, {55, 60, 58}};
    case WorkoutGameTerrainKind::SmoothTrail:
        return {dirtColor(), dirtHighlightColor(), QColor(91, 65, 39)};
    }
    return {dirtColor(), dirtHighlightColor(), QColor(91, 65, 39)};
}

QColor blendColor(const QColor &from, const QColor &to, double amount)
{
    const double blend = std::clamp(amount, 0.0, 1.0);
    return QColor(
            int(std::lround(from.red() + (to.red() - from.red()) * blend)),
            int(std::lround(from.green()
                    + (to.green() - from.green()) * blend)),
            int(std::lround(from.blue()
                    + (to.blue() - from.blue()) * blend)));
}

constexpr int RiderKeylineRadius = 3;

const QPixmap &riderSourcePixmap()
{
    static const QPixmap sprite(
            QStringLiteral(":/images/workout-game-rider-oblique.png"));
    return sprite;
}

const QPixmap &outlinedRiderPixmap()
{
    static const QPixmap sprite = QPixmap::fromImage(
            WorkoutGameCanvas::addRiderContrastKeyline(
                riderSourcePixmap().toImage()));
    return sprite;
}

const QPixmap &trailPropAtlas()
{
    static const QPixmap atlas(
            QStringLiteral(":/images/workout-game-trail-props.png"));
    return atlas;
}

const QPixmap &trailSurfaceTexture()
{
    static const QPixmap texture(
            QStringLiteral(":/images/workout-game-trail-surface.png"));
    return texture;
}

int trailPropAtlasIndex(WorkoutGameTrailPropKind kind)
{
    switch (kind) {
    case WorkoutGameTrailPropKind::Root: return 0;
    case WorkoutGameTrailPropKind::Rock: return 1;
    case WorkoutGameTrailPropKind::Pebble: return 2;
    case WorkoutGameTrailPropKind::Log: return 3;
    case WorkoutGameTrailPropKind::Plank: return 4;
    case WorkoutGameTrailPropKind::BermMarker: return 5;
    case WorkoutGameTrailPropKind::RollerMarker: return 6;
    case WorkoutGameTrailPropKind::DropMarker: return 7;
    case WorkoutGameTrailPropKind::ClimbMarker: return 8;
    case WorkoutGameTrailPropKind::TabletopMarker:
    case WorkoutGameTrailPropKind::Slab:
        return -1;
    }
    return 2;
}

const QPixmap &scaledBackground(const QSize &size)
{
    static QSize cachedSize;
    static QPixmap cached;
    static const QPixmap source(
            QStringLiteral(":/images/workout-game-background-oblique.png"));
    if (!source.isNull() && (cached.isNull() || cachedSize != size)) {
        cachedSize = size;
        cached = source.scaled(
                size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    return cached;
}

}

WorkoutGameCanvas::WorkoutGameCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setAutoFillBackground(false);
    visualClock.start();
    animationTimer.setTimerType(Qt::PreciseTimer);
    animationTimer.setInterval(targetFrameIntervalMs(60.0));
    connect(&animationTimer, &QTimer::timeout, this, [this]() {
        animationFrame = (animationFrame + 1) % 120;
        update();
    });
}

int WorkoutGameCanvas::targetFrameIntervalMs(double refreshRateHz)
{
    if (!std::isfinite(refreshRateHz) || refreshRateHz <= 0.0) {
        return 16;
    }
    const double boundedRate = std::clamp(refreshRateHz, 30.0, 120.0);
    return std::max(1, int(std::floor(1000.0 / boundedRate)));
}

void WorkoutGameCanvas::setCourse(const WorkoutGameCourse &newCourse)
{
    course = newCourse;
    current = WorkoutGameSimulationSnapshot();
    world = WorkoutGameWorldSnapshot();
    camera = WorkoutGameCameraSnapshot();
    visualSmoother.reset();
    update();
}

void WorkoutGameCanvas::setCompetition(
        const WorkoutGameCompetitionSnapshot &newCompetition)
{
    competition = newCompetition;
    visualSmoother.setTarget(
            {current, competition, world, camera, {}, {}},
            WorkoutGameClock::monotonicMilliseconds());
    update();
}

void WorkoutGameCanvas::setWorld(
        const WorkoutGameWorldSnapshot &newWorld,
        const WorkoutGameCameraSnapshot &newCamera)
{
    world = newWorld;
    camera = newCamera;
    visualSmoother.setTarget(
            {current, competition, world, camera, {}, {}},
            WorkoutGameClock::monotonicMilliseconds());
    update();
}

void WorkoutGameCanvas::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    current = frame.simulation;
    competition = frame.competition;
    world = frame.world;
    camera = frame.camera;
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    visualSmoother.setTarget(
            frame, WorkoutGameClock::monotonicMilliseconds());
    if (!animationTimer.isActive()) update();
}

void WorkoutGameCanvas::setTelemetry(
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    if (!animationTimer.isActive()) update();
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
    visualSmoother.setTarget(
            {current, competition, world, camera, {}, {}},
            WorkoutGameClock::monotonicMilliseconds());
    update();
}

void WorkoutGameCanvas::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QScreen *display = windowHandle() ? windowHandle()->screen() : nullptr;
    if (!display) display = QGuiApplication::primaryScreen();
    animationTimer.setInterval(targetFrameIntervalMs(
            display ? display->refreshRate() : 60.0));
    animationTimer.start();
}

void WorkoutGameCanvas::hideEvent(QHideEvent *event)
{
    animationTimer.stop();
    QWidget::hideEvent(event);
}

QString WorkoutGameCanvas::featureName(WorkoutGameFeature feature)
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

QString WorkoutGameCanvas::terrainName(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::SmoothTrail: return tr("TRAIL");
    case WorkoutGameTerrainKind::Roots: return tr("ROOTS");
    case WorkoutGameTerrainKind::Rollers: return tr("ROLLERS");
    case WorkoutGameTerrainKind::Climb: return tr("CLIMB");
    case WorkoutGameTerrainKind::RockGarden: return tr("ROCK GARDEN");
    case WorkoutGameTerrainKind::BunnyHop: return tr("BUNNY HOP");
    case WorkoutGameTerrainKind::Drop: return tr("DROP");
    case WorkoutGameTerrainKind::Skinny: return tr("SKINNY");
    case WorkoutGameTerrainKind::Berm: return tr("BERM");
    case WorkoutGameTerrainKind::LogOver: return tr("LOG OVER");
    case WorkoutGameTerrainKind::Tabletop: return tr("TABLETOP");
    case WorkoutGameTerrainKind::RockSlab: return tr("ROCK SLAB");
    }
    return QString();
}

QString WorkoutGameCanvas::challengeCueName(WorkoutGameChallengeCue cue)
{
    switch (cue) {
    case WorkoutGameChallengeCue::CarrySpeed: return tr("CARRY SPEED");
    case WorkoutGameChallengeCue::Jump: return tr("BUILD FOR JUMP");
    case WorkoutGameChallengeCue::HoldLine: return tr("HOLD LINE");
    case WorkoutGameChallengeCue::Climb: return tr("KEEP PRESSURE");
    case WorkoutGameChallengeCue::None: break;
    }
    return QString();
}

double WorkoutGameCanvas::trailY(
        double x,
        const QRect &scene,
        const WorkoutGameCourse &course,
        const WorkoutGameSimulationSnapshot &snapshot)
{
    double grade = 0.0;
    std::uint32_t variant = 0;
    if (snapshot.activeSection >= 0
            && snapshot.activeSection < int(course.sections.size())) {
        const WorkoutGameSection &section = course.sections[snapshot.activeSection];
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

double WorkoutGameCanvas::physicsTrailY(
        double x,
        const QRect &scene,
        const WorkoutGameWorldSnapshot &world,
        const WorkoutGameCameraSnapshot &camera)
{
    WorkoutGameTerrainProfile profile;
    profile.terrain = world.terrain;
    profile.seed = world.seed;
    profile.gradePercent = world.gradePercent;
    profile.difficulty = world.difficulty;
    profile.terrainOffsetMeters = world.terrainOffsetMeters;
    return terrainProfileY(x, scene, world, camera, profile);
}

double WorkoutGameCanvas::terrainProfileY(
        double x,
        const QRect &scene,
        const WorkoutGameWorldSnapshot &world,
        const WorkoutGameCameraSnapshot &camera,
        const WorkoutGameTerrainProfile &profile)
{
    const double width = std::max(1, scene.width());
    const double pixelsPerMeter = width / 42.0;
    const double riderX = scene.left() + width * 0.28;
    const double distance = world.rider.distanceMeters
            + profile.terrainOffsetMeters + (x - riderX) / pixelsPerMeter;
    const double riderTerrainDistance = world.rider.distanceMeters
            + profile.terrainOffsetMeters;
    const double height = WorkoutGamePhysics::terrainHeight(
            profile.terrain, distance, profile.gradePercent,
            profile.difficulty, profile.seed);
    const double riderHeight = WorkoutGamePhysics::terrainHeight(
            profile.terrain, riderTerrainDistance, profile.gradePercent,
            profile.difficulty, profile.seed);
    const double yaw = camera.ready ? camera.yawDegrees : 90.0;
    const double verticalScale = 0.72
            + 0.28 * std::sin(std::clamp(yaw, 0.0, 90.0) * Pi / 180.0);
    const double pitchShift = camera.ready
            ? camera.pitchDegrees * scene.height() / 180.0
            : 0.0;
    return scene.top() + scene.height() * 0.67
            - (height - riderHeight) * pixelsPerMeter * verticalScale
            + pitchShift;
}

void WorkoutGameCanvas::paintEvent(QPaintEvent *)
{
    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    const WorkoutGameVisualSnapshot visual = visualSmoother.sample(nowMs);
    const double fps = frameRateCounter.frameRendered(nowMs);
    QPainter painter(this);
    paintScene(
            painter, rect(), course, visual.simulation, visual.competition,
            visual.world, visual.camera,
            visual.terrainTransition,
            watts, targetWatts, cadenceRpm, heartRate, virtualGear,
            animationFrame, fps, QStringLiteral("CPU"));
}

QImage WorkoutGameCanvas::addRiderContrastKeyline(const QImage &sprite)
{
    if (sprite.isNull()) return QImage();

    const QImage source =
            sprite.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int padding = RiderKeylineRadius;
    QImage result(
            source.width() + padding * 2,
            source.height() + padding * 2,
            QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    const auto paintDilatedMask = [&](int radius, const QColor &color) {
        const QRgb pixel = color.rgba();
        for (int y = 0; y < source.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(
                    source.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                if (qAlpha(line[x]) < 32) continue;
                for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
                    QRgb *target = reinterpret_cast<QRgb *>(
                            result.scanLine(y + padding + offsetY));
                    for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                        target[x + padding + offsetX] = pixel;
                    }
                }
            }
        }
    };

    paintDilatedMask(RiderKeylineRadius, riderKeylineColor());
    paintDilatedMask(1, inkColor());

    QPainter painter(&result);
    painter.drawImage(padding, padding, source);
    return result;
}

QString WorkoutGameCanvas::elapsedTimeText(std::int64_t workoutTimeMs)
{
    const std::int64_t totalSeconds =
            std::max<std::int64_t>(0, workoutTimeMs) / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
}

void WorkoutGameCanvas::paintScene(
        QPainter &painter,
        const QRect &viewport,
        const WorkoutGameCourse &course,
        const WorkoutGameSimulationSnapshot &current,
        const WorkoutGameCompetitionSnapshot &competition,
        const WorkoutGameWorldSnapshot &world,
        const WorkoutGameCameraSnapshot &camera,
        const WorkoutGameTerrainTransitionSnapshot &terrainTransition,
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear,
        int animationFrame,
        double framesPerSecond,
        const QString &rendererLabel)
{
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(viewport, skyColor());

    const int hudHeight = std::clamp(viewport.height() / 7, 42, 72);
    const QRect scene(
            viewport.left(), viewport.top() + hudHeight,
            viewport.width(), viewport.height() - hudHeight);
    const double cameraScale = camera.ready
            ? std::clamp(camera.zoom, 0.8, 1.2)
            : 1.0;

    static const QPixmap background(
            QStringLiteral(":/images/workout-game-background-oblique.png"));
    if (!background.isNull()) {
        painter.drawPixmap(scene.topLeft(), scaledBackground(scene.size()));
    } else {
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
        const int treeWidth = std::clamp(viewport.width() / 24, 18, 46);
        for (int x = -treeWidth;
             x < viewport.width() + treeWidth; x += treeWidth) {
            const int offset = (x / treeWidth + animationFrame / 12) % 3;
            const int baseY = scene.top()
                    + int(scene.height() * (0.48 + offset * 0.025));
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
    }

    const TerrainPalette currentPalette = terrainPalette(world.terrain);
    const TerrainPalette previousPalette = terrainTransition.active
            ? terrainPalette(terrainTransition.from.terrain)
            : currentPalette;
    const double terrainBlend = terrainTransition.active
            ? terrainTransition.progress : 1.0;
    const TerrainPalette palette = {
        blendColor(previousPalette.ground, currentPalette.ground, terrainBlend),
        blendColor(previousPalette.highlight,
                   currentPalette.highlight, terrainBlend),
        blendColor(previousPalette.accent, currentPalette.accent, terrainBlend)
    };

    const WorkoutGameTrailSceneSnapshot currentTrail =
            WorkoutGameTrailScene::build(world);
    WorkoutGameTrailSceneSnapshot previousTrail;
    if (terrainTransition.active && world.ready) {
        WorkoutGameWorldSnapshot previousWorld = world;
        previousWorld.terrain = terrainTransition.from.terrain;
        previousWorld.seed = terrainTransition.from.seed;
        previousWorld.gradePercent = terrainTransition.from.gradePercent;
        previousWorld.difficulty = terrainTransition.from.difficulty;
        previousWorld.terrainOffsetMeters =
                terrainTransition.from.terrainOffsetMeters;
        previousTrail = WorkoutGameTrailScene::build(previousWorld);
    }

    WorkoutGameTrailSceneSnapshot displayTrail = currentTrail;
    if (terrainTransition.active && previousTrail.ready && currentTrail.ready
            && previousTrail.points.size() == currentTrail.points.size()) {
        displayTrail.riderYNormalized = previousTrail.riderYNormalized
                + (currentTrail.riderYNormalized
                    - previousTrail.riderYNormalized) * terrainBlend;
        for (std::size_t index = 0; index < displayTrail.points.size(); ++index) {
            WorkoutGameTrailPoint &point = displayTrail.points[index];
            const WorkoutGameTrailPoint &from = previousTrail.points[index];
            point.centerYNormalized = from.centerYNormalized
                    + (point.centerYNormalized - from.centerYNormalized)
                        * terrainBlend;
            point.farEdgeYNormalized = from.farEdgeYNormalized
                    + (point.farEdgeYNormalized - from.farEdgeYNormalized)
                        * terrainBlend;
            point.nearEdgeYNormalized = from.nearEdgeYNormalized
                    + (point.nearEdgeYNormalized - from.nearEdgeYNormalized)
                        * terrainBlend;
        }
    }

    const auto trailPoint = [&](const WorkoutGameTrailPoint &point,
                                double yNormalized) {
        return QPointF(
                scene.left() + point.xNormalized * scene.width(),
                scene.top() + yNormalized * scene.height());
    };
    const auto trailYAt = [&](double x) {
        if (!displayTrail.ready || displayTrail.points.empty()) {
            return trailY(x, scene, course, current);
        }
        const double xNormalized = std::clamp(
                (x - scene.left()) / std::max(1.0, double(scene.width())),
                0.0,
                1.0);
        const double position = xNormalized
                * double(displayTrail.points.size() - 1);
        const std::size_t first = std::min(
                std::size_t(position), displayTrail.points.size() - 1);
        const std::size_t second = std::min(
                first + 1, displayTrail.points.size() - 1);
        const double amount = position - double(first);
        const double center = displayTrail.points[first].centerYNormalized
                + (displayTrail.points[second].centerYNormalized
                    - displayTrail.points[first].centerYNormalized) * amount;
        return scene.top() + center * scene.height();
    };

    if (displayTrail.ready) {
        QPainterPath trail;
        const WorkoutGameTrailPoint &first = displayTrail.points.front();
        trail.moveTo(trailPoint(first, first.farEdgeYNormalized));
        for (const WorkoutGameTrailPoint &point : displayTrail.points) {
            trail.lineTo(trailPoint(point, point.farEdgeYNormalized));
        }
        for (auto point = displayTrail.points.rbegin();
                point != displayTrail.points.rend(); ++point) {
            trail.lineTo(trailPoint(*point, point->nearEdgeYNormalized));
        }
        trail.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette.ground);
        painter.drawPath(trail);
        const QPixmap &surface = trailSurfaceTexture();
        if (!surface.isNull()) {
            QBrush surfaceBrush(surface);
            QTransform textureTransform;
            const double textureOffset = std::fmod(
                    std::max(0.0, world.rider.distanceMeters)
                        * scene.width() / WorkoutGameTrailScene::VisibleMeters,
                    double(surface.width()));
            textureTransform.translate(-textureOffset, 0.0);
            surfaceBrush.setTransform(textureTransform);
            painter.fillPath(trail, surfaceBrush);
            QColor tint = palette.ground;
            tint.setAlpha(72);
            painter.fillPath(trail, tint);
        }

        QPainterPath farEdge;
        QPainterPath nearEdge;
        farEdge.moveTo(trailPoint(first, first.farEdgeYNormalized));
        nearEdge.moveTo(trailPoint(first, first.nearEdgeYNormalized));
        for (const WorkoutGameTrailPoint &point : displayTrail.points) {
            farEdge.lineTo(trailPoint(point, point.farEdgeYNormalized));
            nearEdge.lineTo(trailPoint(point, point.nearEdgeYNormalized));
        }
        painter.setPen(QPen(
                palette.highlight,
                std::clamp(viewport.height() / 135, 2, 5)));
        painter.drawPath(farEdge);
        painter.setPen(QPen(
                palette.highlight.darker(125),
                std::clamp(viewport.height() / 180, 2, 4)));
        painter.drawPath(nearEdge);
    } else {
        QPainterPath ground;
        ground.moveTo(scene.left(), trailYAt(scene.left()));
        const int sampleWidth = std::max(4, viewport.width() / 100);
        for (int x = scene.left() + sampleWidth;
                x <= scene.right(); x += sampleWidth) {
            ground.lineTo(x, trailYAt(x));
        }
        ground.lineTo(scene.right(), scene.bottom());
        ground.lineTo(scene.left(), scene.bottom());
        ground.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(dirtColor());
        painter.drawPath(ground);
    }

    const auto drawProps = [&](const WorkoutGameTrailSceneSnapshot &trail,
                               double opacity) {
        if (!trail.ready || opacity <= 0.0) return;
        painter.save();
        painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
        painter.setPen(QPen(
                palette.accent,
                std::clamp(viewport.height() / 210, 2, 4)));
        painter.setBrush(palette.accent);
        const QPixmap &atlas = trailPropAtlas();
        const int cellWidth = atlas.isNull() ? 0 : atlas.width() / 3;
        const int cellHeight = atlas.isNull() ? 0 : atlas.height() / 3;
        for (const WorkoutGameTrailProp &prop : trail.props) {
            const int x = int(std::lround(
                    scene.left() + prop.xNormalized * scene.width()));
            const int y = int(std::lround(
                    scene.top() + prop.yNormalized * scene.height()));
            const int size = std::clamp(
                    int(std::lround(
                        scene.height() / 42.0 * prop.scale * cameraScale)),
                    6,
                    20);
            const int atlasIndex = trailPropAtlasIndex(prop.kind);
            if (!atlas.isNull() && cellWidth > 0 && cellHeight > 0
                    && atlasIndex >= 0) {
                const QRect source(
                        (atlasIndex % 3) * cellWidth,
                        (atlasIndex / 3) * cellHeight,
                        cellWidth,
                        cellHeight);
                double kindScale = 1.0;
                if (prop.kind == WorkoutGameTrailPropKind::BermMarker
                        || prop.kind == WorkoutGameTrailPropKind::Plank) {
                    kindScale = 1.25;
                }
                const int extent = std::clamp(
                        int(std::lround(size * 6.5 * kindScale)), 34, 130);
                const QRect target(
                        x - extent / 2,
                        y - extent * 3 / 5,
                        extent,
                        extent);
                painter.drawPixmap(target, atlas, source);
                continue;
            }
            switch (prop.kind) {
            case WorkoutGameTrailPropKind::Root:
                painter.drawArc(
                        QRect(x - size * 2, y - size / 2,
                              size * 4, size), 0, 180 * 16);
                painter.drawLine(
                        x - size * 2, y,
                        x + size * 2, y - size / 3);
                break;
            case WorkoutGameTrailPropKind::Rock: {
                QPolygon rock;
                rock << QPoint(x - size, y)
                     << QPoint(x - size / 2, y - size)
                     << QPoint(x + size / 3, y - size * 4 / 3)
                     << QPoint(x + size, y);
                painter.drawPolygon(rock);
                break;
            }
            case WorkoutGameTrailPropKind::Log:
                painter.drawRoundedRect(
                        QRect(x - size * 2, y - size / 2,
                              size * 4, size), size / 3, size / 3);
                break;
            case WorkoutGameTrailPropKind::Plank:
                painter.drawRect(
                        QRect(x - size * 2, y - size / 3,
                              size * 4, std::max(3, size * 2 / 3)));
                break;
            case WorkoutGameTrailPropKind::BermMarker:
                painter.drawArc(
                        QRect(x - size * 2, y - size,
                              size * 4, size * 2), 0, 180 * 16);
                break;
            case WorkoutGameTrailPropKind::DropMarker:
                painter.drawLine(x - size, y - size, x + size, y - size);
                painter.drawLine(x, y - size, x, y);
                break;
            case WorkoutGameTrailPropKind::ClimbMarker:
                painter.drawLine(x - size, y, x, y - size);
                painter.drawLine(x, y - size, x + size, y);
                break;
            case WorkoutGameTrailPropKind::RollerMarker:
                painter.drawEllipse(QPoint(x, y - size / 3), size / 3, size / 3);
                break;
            case WorkoutGameTrailPropKind::Pebble:
                painter.drawEllipse(QPoint(x, y), size / 4, size / 5);
                break;
            case WorkoutGameTrailPropKind::TabletopMarker: {
                QPolygon tabletop;
                tabletop << QPoint(x - size * 3, y)
                         << QPoint(x - size, y - size)
                         << QPoint(x + size, y - size)
                         << QPoint(x + size * 3, y);
                painter.drawPolygon(tabletop);
                break;
            }
            case WorkoutGameTrailPropKind::Slab: {
                QPolygon slab;
                slab << QPoint(x - size * 2, y)
                     << QPoint(x - size, y - size)
                     << QPoint(x + size * 2, y - size * 2 / 3)
                     << QPoint(x + size * 3, y)
                     << QPoint(x, y + size / 3);
                painter.drawPolygon(slab);
                break;
            }
            }
        }
        painter.restore();
    };
    if (terrainTransition.active) {
        drawProps(previousTrail, 1.0 - terrainBlend);
    }
    drawProps(currentTrail, terrainBlend);

    const int riderX = scene.left()
            + int(scene.width() * WorkoutGameTrailScene::RiderXNormalized);
    const int wheelRadius = std::clamp(viewport.height() / 30, 7, 18);
    if (competition.ready && current.ready) {
        for (const WorkoutGameCompetitorSnapshot &rider : competition.competitors) {
            const double progressGap = std::clamp(
                    rider.relativeProgress, -0.15, 0.15);
            const int x = std::clamp(
                    riderX + int(progressGap * scene.width() * 4.5),
                    scene.left() + wheelRadius * 3,
                    scene.right() - wheelRadius * 3);
            const int ground = int(trailYAt(x));
            const QPixmap &competitorSprite = outlinedRiderPixmap();
            if (!competitorSprite.isNull()) {
                const int competitorWidth = std::clamp(
                        int(viewport.width() / 6 * cameraScale), 90, 210);
                const int competitorHeight = competitorWidth
                        * competitorSprite.height() / competitorSprite.width();
                const QRect competitorRect(
                        x - competitorWidth / 2,
                        ground - competitorHeight + competitorHeight / 20,
                        competitorWidth,
                        competitorHeight);
                painter.save();
                painter.setOpacity(
                        rider.kind == WorkoutGameCompetitorKind::Ghost
                                ? 0.58 : 0.9);
                painter.drawPixmap(
                        competitorRect, competitorSprite,
                        competitorSprite.rect());
                painter.restore();
            }
        }
    }

    if (current.activeSection >= 0
            && current.activeSection < int(course.sections.size())
            && !world.ready
            && course.sections[current.activeSection].feature
                    == WorkoutGameFeature::SprintJump) {
        const int obstacleX = int(viewport.width() * 0.72);
        const int obstacleY = int(trailYAt(obstacleX));
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

    const int riderBob = world.ready
            ? (world.rider.walking ? (animationFrame / 5) % 3 : 0)
            : (current.speedKph > 1.0 ? (animationFrame / 6) % 2 : 0);
    const double pixelsPerMeter = std::max(1, scene.width()) / 42.0;
    const double airLift = world.ready
            ? world.rider.airHeightMeters() * pixelsPerMeter
            : 0.0;
    const double suspensionLift = world.ready
            ? ((world.rider.frontSuspension + world.rider.rearSuspension)
                    * 0.5 - 0.5) * wheelRadius * 0.5
            : 0.0;
    const int riderGround = int(trailYAt(riderX)
            - airLift - suspensionLift) - riderBob;
    const QPixmap &riderSource = riderSourcePixmap();
    const QPixmap &rider = outlinedRiderPixmap();
    if (!riderSource.isNull() && !rider.isNull()) {
        const int riderWidth = std::clamp(viewport.width() / 5, 110, 260);
        const int scaledRiderWidth = std::clamp(
                int(riderWidth * cameraScale), 110, 280);
        const int riderHeight =
                scaledRiderWidth * riderSource.height() / riderSource.width();
        const int horizontalPadding = std::max(
                1, int(std::ceil(
                    scaledRiderWidth * RiderKeylineRadius
                    / double(riderSource.width()))));
        const int verticalPadding = std::max(
                1, int(std::ceil(
                    riderHeight * RiderKeylineRadius
                    / double(riderSource.height()))));
        const QRect riderRect(
                riderX - scaledRiderWidth / 2,
                riderGround - riderHeight + riderHeight / 20,
                scaledRiderWidth,
                riderHeight);
        const QRect drawRect = riderRect.adjusted(
                    -horizontalPadding, -verticalPadding,
                    horizontalPadding, verticalPadding);
        painter.save();
        painter.translate(drawRect.center());
        if (world.ready) {
            painter.rotate(-std::clamp(
                    world.rider.pitchDegrees, -35.0, 35.0));
        }
        painter.translate(-drawRect.center());
        painter.drawPixmap(drawRect, rider, rider.rect());
        painter.restore();
    } else {
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
    }

    if (current.speedKph > 10.0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(220, 177, 99, 190));
        for (int dust = 0; dust < 3; ++dust) {
            const int offset = (animationFrame * 2 + dust * 13) % 38;
            painter.drawRect(
                    riderX - wheelRadius * 2 - offset,
                    riderGround - 3 - dust * 3,
                    3 + dust,
                    3 + dust);
        }
    }

    if (current.challenge.enabled
            && current.featureOutcome != WorkoutGameFeatureOutcome::None) {
        const WorkoutGamePowerProfileSnapshot powerProfile =
                WorkoutGamePowerProfile::build(
                    course, current, watts, cadenceRpm);
        const WorkoutGameTerrainKind challengeTerrain = current.activeSection >= 0
                && current.activeSection < int(course.sections.size())
                ? course.sections[std::size_t(current.activeSection)].terrain
                : world.terrain;
        const int challengeWidth = std::clamp(
                viewport.width() * 2 / 5, 230, 460);
        const int challengeHeight = std::clamp(
                viewport.height() / 11, 54, 68);
        const QRect challengeRect(
                viewport.center().x() - challengeWidth / 2,
                scene.top() + 10,
                challengeWidth,
                challengeHeight);
        const bool completed = current.featureOutcome
                == WorkoutGameFeatureOutcome::Completed;
        const bool bypassed = current.featureOutcome
                == WorkoutGameFeatureOutcome::Bypassed;
        const QColor accent = completed
                ? QColor(88, 188, 105)
                : bypassed ? QColor(232, 197, 78) : QColor(246, 239, 215);
        painter.fillRect(challengeRect, QColor(20, 27, 31, 225));
        painter.setPen(accent);
        QFont challengeFont = painter.font();
        challengeFont.setPixelSize(std::clamp(challengeHeight / 3, 11, 15));
        challengeFont.setBold(true);
        painter.setFont(challengeFont);
        QString challengeText;
        if (completed) {
            challengeText = tr("%1 CLEARED  +%2")
                    .arg(terrainName(challengeTerrain))
                    .arg(current.challenge.bonusPoints);
        } else if (bypassed) {
            challengeText = tr("%1  SAFE LINE")
                    .arg(terrainName(challengeTerrain));
        } else {
            challengeText = tr("%1  %2  %3% READY")
                    .arg(terrainName(challengeTerrain))
                    .arg(challengeCueName(current.challenge.cue))
                    .arg(int(std::floor(
                        std::clamp(current.challengeReadiness, 0.0, 1.0)
                            * 100.0 + 1e-9)));
        }
        painter.drawText(
                challengeRect.adjusted(8, 0, -8, -challengeHeight / 2),
                Qt::AlignCenter,
                challengeText);
        if (!completed && !bypassed) {
            const QColor readyColor(88, 188, 105);
            const QColor missingColor(238, 101, 82);
            const QColor unusedColor(125, 143, 135);
            const auto requirementColor = [&](bool required,
                                               double readiness) {
                if (!required) return unusedColor;
                return readiness >= 1.0 - 1e-9
                        ? readyColor : missingColor;
            };
            QString speedRequirement = QStringLiteral("KM/H --");
            if (powerProfile.cue.speedRequired) {
                const int actualSpeed = int(std::lround(
                        powerProfile.cue.actualSpeedKph));
                if (powerProfile.cue.requiredSpeedKph > 0.0
                        && powerProfile.cue.maximumSpeedKph > 0.0) {
                    speedRequirement = QStringLiteral("KM/H %1/%2-%3")
                            .arg(actualSpeed)
                            .arg(int(std::lround(
                                powerProfile.cue.requiredSpeedKph)))
                            .arg(int(std::lround(
                                powerProfile.cue.maximumSpeedKph)));
                } else if (powerProfile.cue.maximumSpeedKph > 0.0) {
                    speedRequirement = QStringLiteral("KM/H %1/<%2")
                            .arg(actualSpeed)
                            .arg(int(std::lround(
                                powerProfile.cue.maximumSpeedKph)));
                } else {
                    speedRequirement = QStringLiteral("KM/H %1/%2")
                            .arg(actualSpeed)
                            .arg(int(std::lround(
                                powerProfile.cue.requiredSpeedKph)));
                }
            }
            const std::array<QString, 3> labels = {
                powerProfile.cue.powerRequired
                    ? QStringLiteral("W %1/%2")
                        .arg(int(std::lround(
                            powerProfile.cue.actualWatts)))
                        .arg(int(std::lround(
                            powerProfile.cue.requiredWatts)))
                    : QStringLiteral("W --"),
                powerProfile.cue.cadenceRequired
                    ? QStringLiteral("RPM %1/%2")
                        .arg(int(std::lround(
                            powerProfile.cue.actualCadenceRpm)))
                        .arg(int(std::lround(
                            powerProfile.cue.requiredCadenceRpm)))
                    : QStringLiteral("RPM --"),
                speedRequirement
            };
            const std::array<QColor, 3> colors = {
                requirementColor(powerProfile.cue.powerRequired,
                                 powerProfile.cue.powerReadiness),
                requirementColor(powerProfile.cue.cadenceRequired,
                                 powerProfile.cue.cadenceReadiness),
                requirementColor(powerProfile.cue.speedRequired,
                                 powerProfile.cue.speedReadiness)
            };
            QFont requirementFont = challengeFont;
            requirementFont.setPixelSize(10);
            painter.setFont(requirementFont);
            const int requirementWidth = challengeRect.width() / 3;
            for (int index = 0; index < 3; ++index) {
                painter.setPen(colors[std::size_t(index)]);
                painter.drawText(
                        QRect(challengeRect.left()
                                + index * requirementWidth,
                              challengeRect.center().y() - 2,
                              requirementWidth, 22),
                        Qt::AlignCenter, labels[std::size_t(index)]);
            }
            const int meterHeight = std::clamp(challengeHeight / 9, 3, 5);
            const QRect meter(
                    challengeRect.left() + 8,
                    challengeRect.bottom() - meterHeight - 4,
                    challengeRect.width() - 16,
                    meterHeight);
            painter.fillRect(meter, QColor(55, 65, 65));
            painter.fillRect(
                    meter.left(), meter.top(),
                    int(meter.width() * std::clamp(
                        current.challengeReadiness, 0.0, 1.0)),
                    meter.height(),
                    current.challengeReadiness >= 1.0
                            ? QColor(88, 188, 105)
                            : QColor(232, 197, 78));
        }
    }

    painter.fillRect(QRect(viewport.left(), viewport.top(), viewport.width(), hudHeight), inkColor());
    painter.setPen(QColor(246, 239, 215));
    QFont hudFont = painter.font();
    hudFont.setPixelSize(14);
    hudFont.setBold(true);
    painter.setFont(hudFont);
    const int roundedFps = int(std::lround(framesPerSecond));
    const QString fpsText = roundedFps > 0
            ? QString::number(roundedFps)
            : QStringLiteral("--");
    const QString elapsed = elapsedTimeText(current.workoutTimeMs);
    const QString stats = viewport.width() < 700
            ? tr("%1W T%2 C%3 H%4 G%5 %6K %7 %8 %9F")
                .arg(int(std::lround(watts)))
                .arg(int(std::lround(targetWatts)))
                .arg(cadenceRpm)
                .arg(heartRate)
                .arg(virtualGear)
                .arg(int(std::lround(current.speedKph)))
                .arg(elapsed)
                .arg(rendererLabel)
                .arg(fpsText)
            : tr("TIME %1   %2 W   TARGET %3 W   %4 RPM   HR %5   GEAR %6   %7 KM/H   %8 %9 FPS")
                .arg(elapsed)
                .arg(int(std::lround(watts)))
                .arg(int(std::lround(targetWatts)))
                .arg(cadenceRpm)
                .arg(heartRate)
                .arg(virtualGear)
                .arg(current.speedKph, 0, 'f', 1)
                .arg(rendererLabel)
                .arg(fpsText);
    painter.drawText(QRect(14, viewport.top(), viewport.width() - 28, hudHeight / 2),
                     Qt::AlignLeft | Qt::AlignVCenter, stats);

    QString sectionText = tr("NO ERG WORKOUT");
    if (current.ready && current.activeSection >= 0
            && current.activeSection < int(course.sections.size())) {
        sectionText = world.ready
                ? (world.rider.walking
                    ? tr("HIKE")
                    : terrainName(world.terrain))
                : featureName(course.sections[current.activeSection].feature);
    } else if (current.finished) {
        sectionText = tr("FINISH");
    }
    painter.setPen(QColor(232, 197, 78));
    painter.drawText(QRect(14, viewport.top() + hudHeight / 2,
                           viewport.width() / 2, hudHeight / 2),
                     Qt::AlignLeft | Qt::AlignVCenter, sectionText);
    painter.setPen(QColor(246, 239, 215));
    painter.drawText(QRect(viewport.width() / 2, viewport.top() + hudHeight / 2,
                           viewport.width() / 2 - 14, hudHeight / 2),
                     Qt::AlignRight | Qt::AlignVCenter,
                     competition.ready && competition.totalRiders > 0
                        ? (viewport.width() < 700
                            ? tr("#%1/%2  %3")
                                .arg(competition.playerRank)
                                .arg(competition.totalRiders)
                                .arg(current.score)
                            : tr("RANK %1/%2   SCORE %3")
                            .arg(competition.playerRank)
                            .arg(competition.totalRiders)
                            .arg(current.score))
                        : tr("SCORE %1").arg(current.score));

    const int progressHeight = std::clamp(viewport.height() / 80, 4, 9);
    painter.fillRect(viewport.left(), viewport.bottom() - progressHeight + 1,
                     viewport.width(), progressHeight,
                     QColor(29, 40, 43));
    painter.fillRect(viewport.left(), viewport.bottom() - progressHeight + 1,
                     int(viewport.width() * current.courseProgress), progressHeight,
                     QColor(232, 197, 78));
}
