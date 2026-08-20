/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameSceneGraphWindow.h"

#include "WorkoutGameRoadProjection.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using Vertex = QSGGeometry::ColoredPoint2D;

struct WorkoutGameSceneRoot : public QSGNode
{
    QSGSimpleTextureNode *background = nullptr;
    QSGGeometryNode *ground = nullptr;
    QSGGeometryNode *shoulders = nullptr;
    QSGGeometryNode *road = nullptr;
    QSGGeometryNode *features = nullptr;
    QSGSimpleTextureNode *rider = nullptr;
    QSGSimpleTextureNode *hud = nullptr;
    std::uint64_t hudRevision = 0;
};

QSGGeometryNode *createGeometryNode(QSGNode *parent)
{
    auto *geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    parent->appendChildNode(node);
    return node;
}

WorkoutGameSceneRoot *createSceneRoot()
{
    auto *root = new WorkoutGameSceneRoot;
    root->background = new QSGSimpleTextureNode;
    root->background->setOwnsTexture(true);
    root->appendChildNode(root->background);
    root->ground = createGeometryNode(root);
    root->shoulders = createGeometryNode(root);
    root->road = createGeometryNode(root);
    root->features = createGeometryNode(root);
    root->rider = new QSGSimpleTextureNode;
    root->rider->setOwnsTexture(true);
    root->appendChildNode(root->rider);
    root->hud = new QSGSimpleTextureNode;
    root->hud->setOwnsTexture(true);
    root->appendChildNode(root->hud);
    return root;
}

void setTexture(
        QSGSimpleTextureNode *node,
        QQuickWindow *window,
        const QImage &image)
{
    if (!node || !window || image.isNull()) return;
    node->setTexture(window->createTextureFromImage(image));
    node->setFiltering(QSGTexture::Nearest);
}

void appendTriangle(
        std::vector<Vertex> &vertices,
        float ax, float ay,
        float bx, float by,
        float cx, float cy,
        const QColor &color)
{
    const auto add = [&vertices, &color](float x, float y) {
        Vertex vertex;
        vertex.set(x, y,
                   uchar(color.red()), uchar(color.green()),
                   uchar(color.blue()), uchar(color.alpha()));
        vertices.push_back(vertex);
    };
    add(ax, ay);
    add(bx, by);
    add(cx, cy);
}

void appendQuad(
        std::vector<Vertex> &vertices,
        float farLeft, float farY,
        float farRight,
        float nearLeft, float nearY,
        float nearRight,
        const QColor &color)
{
    appendTriangle(vertices,
                   farLeft, farY, nearLeft, nearY, nearRight, nearY,
                   color);
    appendTriangle(vertices,
                   farLeft, farY, nearRight, nearY, farRight, farY,
                   color);
}

void updateGeometry(
        QSGGeometryNode *node,
        const std::vector<Vertex> &vertices)
{
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(int(vertices.size()));
    if (!vertices.empty()) {
        std::copy(vertices.begin(), vertices.end(),
                  geometry->vertexDataAsColoredPoint2D());
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

QColor roadColor(WorkoutGameTerrainKind terrain, bool alternate)
{
    QColor base;
    switch (terrain) {
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::RockSlab: base = QColor(98, 104, 93); break;
    case WorkoutGameTerrainKind::Skinny: base = QColor(129, 91, 49); break;
    case WorkoutGameTerrainKind::Roots: base = QColor(104, 79, 50); break;
    case WorkoutGameTerrainKind::Climb: base = QColor(114, 88, 58); break;
    default: base = QColor(126, 99, 65); break;
    }
    return alternate ? base.lighter(108) : base.darker(106);
}

QColor groundColor(WorkoutGameTerrainKind terrain, bool alternate)
{
    QColor base = terrain == WorkoutGameTerrainKind::RockGarden
            || terrain == WorkoutGameTerrainKind::RockSlab
            ? QColor(75, 104, 78)
            : QColor(50, 111, 66);
    return alternate ? base.lighter(108) : base.darker(106);
}

void buildRoadGeometry(
        const WorkoutGameRoadProjectionFrame &projection,
        double viewportWidth,
        std::vector<Vertex> &ground,
        std::vector<Vertex> &shoulders,
        std::vector<Vertex> &road)
{
    if (projection.slices.size() < 2) return;
    ground.reserve((projection.slices.size() - 1u) * 12u);
    shoulders.reserve((projection.slices.size() - 1u) * 12u);
    road.reserve((projection.slices.size() - 1u) * 6u);
    for (std::size_t index = 1; index < projection.slices.size(); ++index) {
        const WorkoutGameRoadProjectedSlice &far = projection.slices[index - 1];
        const WorkoutGameRoadProjectedSlice &near = projection.slices[index];
        const bool alternate = (int(std::floor(
                near.worldDistanceMeters / 3.5)) & 1) != 0;
        const float farLeft = float(far.centerX - far.halfWidthPixels);
        const float farRight = float(far.centerX + far.halfWidthPixels);
        const float nearLeft = float(near.centerX - near.halfWidthPixels);
        const float nearRight = float(near.centerX + near.halfWidthPixels);
        const float farY = float(far.centerY);
        const float nearY = float(near.centerY);
        const QColor grass = groundColor(near.terrain, alternate);
        appendQuad(ground,
                   0.0f, farY, farLeft,
                   0.0f, nearY, nearLeft, grass);
        appendQuad(ground,
                   farRight, farY, float(viewportWidth),
                   nearRight, nearY, float(viewportWidth), grass);

        const float farShoulder = std::max(1.0f,
                float(far.halfWidthPixels * 0.12));
        const float nearShoulder = std::max(1.0f,
                float(near.halfWidthPixels * 0.12));
        const QColor shoulder(66, 72, 57);
        appendQuad(shoulders,
                   farLeft - farShoulder, farY, farLeft,
                   nearLeft - nearShoulder, nearY, nearLeft, shoulder);
        appendQuad(shoulders,
                   farRight, farY, farRight + farShoulder,
                   nearRight, nearY, nearRight + nearShoulder, shoulder);
        appendQuad(road,
                   farLeft, farY, farRight,
                   nearLeft, nearY, nearRight,
                   roadColor(near.terrain, alternate));
    }
}

void buildFeatureGeometry(
        const WorkoutGameRoadCourse &course,
        const WorkoutGameRoadProjectionFrame &projection,
        std::vector<Vertex> &features)
{
    if (projection.slices.empty()) return;
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled) continue;
        const double obstacle = piece.challenge.obstacleDistanceMeters;
        const WorkoutGameRoadProjectedSlice *nearest = nullptr;
        double nearestDistance = std::numeric_limits<double>::max();
        for (const WorkoutGameRoadProjectedSlice &slice : projection.slices) {
            const double distance = std::abs(
                    slice.worldDistanceMeters - obstacle);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = &slice;
            }
        }
        if (!nearest || nearestDistance > 2.5) continue;
        const float left = float(nearest->centerX
                - nearest->halfWidthPixels * 0.85);
        const float right = float(nearest->centerX
                + nearest->halfWidthPixels * 0.85);
        const float thickness = std::clamp(
                float(nearest->halfWidthPixels * 0.12), 2.0f, 18.0f);
        const float y = float(nearest->centerY);
        const QColor color = piece.animation == WorkoutGameRoadAnimation::Jump
                ? QColor(75, 45, 26)
                : QColor(189, 157, 75);
        appendQuad(features,
                   left, y - thickness, right,
                   left, y + thickness, right, color);
    }
}

QString elapsedText(std::int64_t milliseconds)
{
    const int seconds = int(std::max<std::int64_t>(0, milliseconds) / 1000);
    return QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}

WorkoutGameSceneGraphItem::WorkoutGameSceneGraphItem(QQuickItem *parent) :
    QQuickItem(parent),
    backgroundImage(QStringLiteral(
            ":/images/workout-game-background-oblique.png")),
    riderImage(QStringLiteral(":/images/workout-game-rider-oblique.png"))
{
    setFlag(ItemHasContents, true);
    visualClock.start();
    rebuildHud();
}

void WorkoutGameSceneGraphItem::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    currentFrame = {};
    visualSmoother.reset();
    frameRateCounter.reset();
    rebuildHud();
    update();
}

void WorkoutGameSceneGraphItem::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double newWatts,
        double newTargetWatts,
        int newCadenceRpm,
        int newHeartRate,
        int newVirtualGear)
{
    currentFrame = frame;
    watts = std::max(0.0, newWatts);
    targetWatts = std::max(0.0, newTargetWatts);
    cadenceRpm = std::max(0, newCadenceRpm);
    heartRate = std::max(0, newHeartRate);
    virtualGear = std::max(1, newVirtualGear);
    visualSmoother.setTarget(frame, visualClock.elapsed());
    rebuildHud();
    update();
}

void WorkoutGameSceneGraphItem::setTelemetry(
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
    rebuildHud();
    update();
}

void WorkoutGameSceneGraphItem::rebuildHud()
{
    hudImage = QImage(1240, 78, QImage::Format_RGBA8888_Premultiplied);
    hudImage.fill(Qt::transparent);
    QPainter painter(&hudImage);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(hudImage.rect(), QColor(16, 24, 25, 224));
    painter.fillRect(0, 74, hudImage.width(), 4, QColor(217, 176, 65));
    QFont labelFont = painter.font();
    labelFont.setPixelSize(13);
    labelFont.setBold(true);
    labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    QFont valueFont = labelFont;
    valueFont.setPixelSize(22);

    struct Stat { QString label; QString value; };
    const std::vector<Stat> stats = {
        {tr("POWER"), QStringLiteral("%1 W").arg(int(std::lround(watts)))},
        {tr("TARGET"), QStringLiteral("%1 W").arg(int(std::lround(targetWatts)))},
        {tr("HEART"), QStringLiteral("%1 bpm").arg(heartRate)},
        {tr("CADENCE"), QStringLiteral("%1 rpm").arg(cadenceRpm)},
        {tr("SPEED"), QStringLiteral("%1 km/h").arg(
                currentFrame.simulation.speedKph, 0, 'f', 1)},
        {tr("GEAR"), QString::number(virtualGear)},
        {tr("TIME"), elapsedText(currentFrame.simulation.workoutTimeMs)},
        {QStringLiteral("FPS"), QString::number(displayedFps, 'f', 0)}
    };
    const int columnWidth = hudImage.width() / int(stats.size());
    for (std::size_t index = 0; index < stats.size(); ++index) {
        const QRect column(int(index) * columnWidth, 0, columnWidth, 74);
        painter.setFont(labelFont);
        painter.setPen(QColor(154, 181, 167));
        painter.drawText(column.adjusted(8, 6, -4, 0),
                         Qt::AlignLeft | Qt::AlignTop, stats[index].label);
        painter.setFont(valueFont);
        painter.setPen(Qt::white);
        painter.drawText(column.adjusted(8, 28, -4, -4),
                         Qt::AlignLeft | Qt::AlignTop, stats[index].value);
    }
    ++hudRevision;
}

void WorkoutGameSceneGraphItem::publishFps(double fps)
{
    if (std::abs(displayedFps - fps) < 0.5) return;
    displayedFps = fps;
    rebuildHud();
    update();
}

QSGNode *WorkoutGameSceneGraphItem::updatePaintNode(
        QSGNode *oldNode,
        UpdatePaintNodeData *)
{
    auto *root = static_cast<WorkoutGameSceneRoot *>(oldNode);
    if (!root) {
        root = createSceneRoot();
        setTexture(root->background, window(), backgroundImage);
        setTexture(root->rider, window(), riderImage);
        setTexture(root->hud, window(), hudImage);
        root->hudRevision = hudRevision;
    }
    const double viewportWidth = width();
    const double viewportHeight = height();
    if (viewportWidth <= 0.0 || viewportHeight <= 0.0) return root;

    root->background->setRect(0.0, 0.0, viewportWidth, viewportHeight);
    const double imageRatio = backgroundImage.width()
            / double(std::max(1, backgroundImage.height()));
    const double viewportRatio = viewportWidth / viewportHeight;
    if (viewportRatio > imageRatio) {
        const double sourceHeight = backgroundImage.width() / viewportRatio;
        root->background->setSourceRect(
                0.0, (backgroundImage.height() - sourceHeight) * 0.35,
                backgroundImage.width(), sourceHeight);
    } else {
        const double sourceWidth = backgroundImage.height() * viewportRatio;
        root->background->setSourceRect(
                (backgroundImage.width() - sourceWidth) * 0.5, 0.0,
                sourceWidth, backgroundImage.height());
    }

    const std::int64_t nowMs = visualClock.elapsed();
    const WorkoutGameVisualSnapshot visual = visualSmoother.sample(nowMs);
    WorkoutGameRoadProjectionConfig config;
    config.viewportWidth = viewportWidth;
    config.viewportHeight = viewportHeight;
    const double riderDistance = visual.world.ready
            ? visual.world.rider.distanceMeters
            : roadCourse.totalLengthMeters
                * visual.simulation.courseProgress;
    const WorkoutGameRoadProjectionFrame projection =
            WorkoutGameRoadProjection::project(
                    roadCourse, riderDistance, config);

    std::vector<Vertex> ground;
    std::vector<Vertex> shoulders;
    std::vector<Vertex> road;
    std::vector<Vertex> features;
    if (projection.ready) {
        buildRoadGeometry(
                projection, viewportWidth, ground, shoulders, road);
        buildFeatureGeometry(roadCourse, projection, features);
    }
    updateGeometry(root->ground, ground);
    updateGeometry(root->shoulders, shoulders);
    updateGeometry(root->road, road);
    updateGeometry(root->features, features);

    const double riderWidth = std::clamp(
            viewportWidth * 0.16, 105.0, 210.0);
    const double riderHeight = riderWidth * riderImage.height()
            / double(std::max(1, riderImage.width()));
    const double bob = visual.world.ready
            ? visual.world.rider.clearanceMeters * 18.0
                - (visual.world.rider.rearSuspension
                    + visual.world.rider.frontSuspension) * 3.0
            : 0.0;
    const double riderX = projection.ready
            ? projection.riderScreenX : viewportWidth * 0.5;
    const double riderY = projection.ready
            ? projection.riderScreenY : viewportHeight * 0.82;
    root->rider->setRect(
            riderX - riderWidth * 0.5,
            riderY - riderHeight * 0.78 - bob,
            riderWidth, riderHeight);

    if (root->hudRevision != hudRevision) {
        setTexture(root->hud, window(), hudImage);
        root->hudRevision = hudRevision;
    }
    const double hudWidth = std::min(
            viewportWidth - 24.0, double(hudImage.width()));
    const double hudHeight = hudWidth * hudImage.height()
            / double(hudImage.width());
    root->hud->setRect(12.0, 12.0,
                       std::max(1.0, hudWidth), std::max(1.0, hudHeight));

    const double fps = frameRateCounter.frameRendered(nowMs);
    if (fps > 0.0 && std::abs(publishedFps - fps) >= 0.5) {
        publishedFps = fps;
        QMetaObject::invokeMethod(
                this, [this, fps]() { publishFps(fps); },
                Qt::QueuedConnection);
    }
    return root;
}

WorkoutGameSceneGraphWindow::WorkoutGameSceneGraphWindow(QWindow *parent) :
    QQuickWindow(parent),
    sceneItem(new WorkoutGameSceneGraphItem(contentItem()))
{
    setColor(QColor(99, 190, 187));
    sceneItem->setSize(size());
    connect(this, &QQuickWindow::frameSwapped,
            sceneItem, [this]() {
                if (isVisible()) sceneItem->update();
            }, Qt::QueuedConnection);
    connect(this, &QQuickWindow::sceneGraphError,
            this, [this](QQuickWindow::SceneGraphError, const QString &message) {
                if (failureReported) return;
                failureReported = true;
                qWarning().noquote() << "Workout Game scene graph error:"
                                     << message;
                emit rendererFailed();
            });
}

void WorkoutGameSceneGraphWindow::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    sceneItem->setCourse(course, ftpWatts);
}

void WorkoutGameSceneGraphWindow::setFrame(
        const WorkoutGameVisualSnapshot &frame,
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear)
{
    sceneItem->setFrame(frame, watts, targetWatts,
                        cadenceRpm, heartRate, virtualGear);
}

void WorkoutGameSceneGraphWindow::setTelemetry(
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRate,
        int virtualGear)
{
    sceneItem->setTelemetry(watts, targetWatts,
                            cadenceRpm, heartRate, virtualGear);
}

void WorkoutGameSceneGraphWindow::resizeEvent(QResizeEvent *event)
{
    QQuickWindow::resizeEvent(event);
    sceneItem->setSize(event->size());
}
