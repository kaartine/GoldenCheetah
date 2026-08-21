/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameSceneGraphWindow.h"
#include "WorkoutGameClock.h"

#include "WorkoutGameRoadProjection.h"
#include "WorkoutGameMesh.h"
#include "WorkoutGamePowerCueGeometry.h"

#include <QColor>
#include <QDir>
#include <QDebug>
#include <QFont>
#include <QMatrix4x4>
#include <QPainter>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QScreen>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QSGRendererInterface>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

using Vertex = QSGGeometry::ColoredPoint2D;

struct SceneTriangle
{
    Vertex vertices[3];
    double depthMeters = 0.0;
};

bool environmentEnabled(const char *name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return !value.isEmpty() && value != "0" && value != "false"
            && value != "off" && value != "no";
}

struct WorkoutGameSceneRoot : public QSGNode
{
    QSGSimpleTextureNode *background = nullptr;
    QSGGeometryNode *terrain = nullptr;
    QSGTransformNode *riderTransform = nullptr;
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
    root->terrain = createGeometryNode(root);
    root->riderTransform = new QSGTransformNode;
    root->appendChildNode(root->riderTransform);
    root->rider = new QSGSimpleTextureNode;
    root->rider->setOwnsTexture(true);
    root->riderTransform->appendChildNode(root->rider);
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
        std::vector<SceneTriangle> &triangles,
        float ax, float ay,
        float bx, float by,
        float cx, float cy,
        const QColor &color,
        double depthMeters)
{
    SceneTriangle triangle;
    int corner = 0;
    const auto add = [&triangle, &corner, &color](float x, float y) {
        Vertex vertex;
        vertex.set(x, y,
                   uchar(color.red()), uchar(color.green()),
                   uchar(color.blue()), uchar(color.alpha()));
        triangle.vertices[corner++] = vertex;
    };
    add(ax, ay);
    add(bx, by);
    add(cx, cy);
    triangle.depthMeters = depthMeters;
    triangles.push_back(triangle);
}

void appendQuad(
        std::vector<SceneTriangle> &triangles,
        float farLeft, float farY,
        float farRight,
        float nearLeft, float nearY,
        float nearRight,
        const QColor &color,
        double farDepthMeters,
        double nearDepthMeters)
{
    appendTriangle(triangles,
                   farLeft, farY, nearLeft, nearY, nearRight, nearY,
                   color, (farDepthMeters + nearDepthMeters * 2.0) / 3.0);
    appendTriangle(triangles,
                   farLeft, farY, nearRight, nearY, farRight, farY,
                   color, (farDepthMeters * 2.0 + nearDepthMeters) / 3.0);
}

WorkoutGameRoadProjectedSlice interpolateSlice(
        const WorkoutGameRoadProjectedSlice &from,
        const WorkoutGameRoadProjectedSlice &to,
        double amount)
{
    const auto value = [amount](double first, double second) {
        return first + (second - first) * amount;
    };
    WorkoutGameRoadProjectedSlice result = to;
    result.worldDistanceMeters = value(
            from.worldDistanceMeters, to.worldDistanceMeters);
    result.depthMeters = value(from.depthMeters, to.depthMeters);
    result.centerX = value(from.centerX, to.centerX);
    result.centerY = value(from.centerY, to.centerY);
    result.halfWidthPixels = value(
            from.halfWidthPixels, to.halfWidthPixels);
    result.halfWidthMeters = value(
            from.halfWidthMeters, to.halfWidthMeters);
    result.pixelsPerMeter = value(
            from.pixelsPerMeter, to.pixelsPerMeter);
    result.surfaceOffsetMeters = value(
            from.surfaceOffsetMeters, to.surfaceOffsetMeters);
    result.surfaceElevationMeters = value(
            from.surfaceElevationMeters, to.surfaceElevationMeters);
    result.gradePercent = value(from.gradePercent, to.gradePercent);
    result.occlusionY = value(from.occlusionY, to.occlusionY);
    return result;
}

bool clipProjectedPair(
        WorkoutGameRoadProjectedSlice &far,
        WorkoutGameRoadProjectedSlice &near)
{
    const double farVisibility = far.occlusionY - far.centerY;
    const double nearVisibility = near.occlusionY - near.centerY;
    if (farVisibility < -1e-6 && nearVisibility < -1e-6) return false;
    if (farVisibility < -1e-6 || nearVisibility < -1e-6) {
        const double span = farVisibility - nearVisibility;
        const double amount = std::abs(span) > 1e-12
                ? std::clamp(farVisibility / span, 0.0, 1.0)
                : 0.0;
        const WorkoutGameRoadProjectedSlice crossing =
                interpolateSlice(far, near, amount);
        if (farVisibility < -1e-6) far = crossing;
        else near = crossing;
    }
    return true;
}

std::vector<Vertex> sortedVertices(std::vector<SceneTriangle> triangles)
{
    std::stable_sort(triangles.begin(), triangles.end(),
            [](const SceneTriangle &left, const SceneTriangle &right) {
                return left.depthMeters > right.depthMeters;
            });
    std::vector<Vertex> vertices;
    vertices.reserve(triangles.size() * 3u);
    for (const SceneTriangle &triangle : triangles) {
        vertices.insert(vertices.end(), std::begin(triangle.vertices),
                        std::end(triangle.vertices));
    }
    return vertices;
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

QColor blendColor(const QColor &from, const QColor &to, double amount)
{
    const double blend = std::clamp(amount, 0.0, 1.0);
    return QColor(
            int(std::lround(from.red() + (to.red() - from.red()) * blend)),
            int(std::lround(from.green()
                + (to.green() - from.green()) * blend)),
            int(std::lround(from.blue()
                + (to.blue() - from.blue()) * blend)),
            int(std::lround(from.alpha()
                + (to.alpha() - from.alpha()) * blend)));
}

QColor gradeTint(QColor color, double gradePercent)
{
    const double strength = std::clamp(
            std::abs(gradePercent) / 12.0, 0.0, 1.0);
    const QColor accent = gradePercent >= 0.0
            ? QColor(190, 139, 72) : QColor(70, 124, 102);
    return blendColor(color, accent, strength * 0.22);
}

QColor meshColor(WorkoutGameMeshMaterial material, bool selectedBypass = false)
{
    switch (material) {
    case WorkoutGameMeshMaterial::Dirt: return QColor(145, 105, 62);
    case WorkoutGameMeshMaterial::DirtEdge: return QColor(79, 57, 39);
    case WorkoutGameMeshMaterial::Bypass:
        return selectedBypass ? QColor(218, 167, 76) : QColor(91, 76, 52);
    case WorkoutGameMeshMaterial::WoodSide: return QColor(78, 47, 28);
    case WorkoutGameMeshMaterial::WoodTop: return QColor(166, 96, 43);
    case WorkoutGameMeshMaterial::RockSide: return QColor(67, 73, 68);
    case WorkoutGameMeshMaterial::RockTop: return QColor(145, 147, 130);
    case WorkoutGameMeshMaterial::DropFace: return QColor(45, 47, 43);
    }
    return QColor(180, 140, 72);
}

void buildRoadGeometry(
        const WorkoutGameRoadProjectionFrame &projection,
        double viewportWidth,
        std::vector<SceneTriangle> &geometry)
{
    if (projection.slices.size() < 2) return;
    geometry.reserve(geometry.size()
            + (projection.slices.size() - 1u) * 10u);
    for (std::size_t index = 1; index < projection.slices.size(); ++index) {
        WorkoutGameRoadProjectedSlice far = projection.slices[index - 1];
        WorkoutGameRoadProjectedSlice near = projection.slices[index];
        if (!clipProjectedPair(far, near)) continue;
        const bool alternate = (int(std::floor(
                near.worldDistanceMeters / 3.5)) & 1) != 0;
        const float farLeft = float(far.centerX - far.halfWidthPixels);
        const float farRight = float(far.centerX + far.halfWidthPixels);
        const float nearLeft = float(near.centerX - near.halfWidthPixels);
        const float nearRight = float(near.centerX + near.halfWidthPixels);
        const float farY = float(far.centerY);
        const float nearY = float(near.centerY);
        const QColor grass = gradeTint(blendColor(
                groundColor(far.terrain, alternate),
                groundColor(near.terrain, alternate), 0.65),
                near.gradePercent);
        appendQuad(geometry,
                   0.0f, farY, farLeft,
                   0.0f, nearY, nearLeft, grass,
                   far.depthMeters, near.depthMeters);
        appendQuad(geometry,
                   farRight, farY, float(viewportWidth),
                   nearRight, nearY, float(viewportWidth), grass,
                   far.depthMeters, near.depthMeters);

        const float farShoulder = std::max(1.0f,
                float(far.halfWidthPixels * 0.12));
        const float nearShoulder = std::max(1.0f,
                float(near.halfWidthPixels * 0.12));
        const QColor shoulder(66, 72, 57);
        appendQuad(geometry,
                   farLeft - farShoulder, farY, farLeft,
                   nearLeft - nearShoulder, nearY, nearLeft, shoulder,
                   far.depthMeters, near.depthMeters);
        appendQuad(geometry,
                   farRight, farY, farRight + farShoulder,
                   nearRight, nearY, nearRight + nearShoulder, shoulder,
                   far.depthMeters, near.depthMeters);
        appendQuad(geometry,
                   farLeft, farY, farRight,
                   nearLeft, nearY, nearRight,
                   gradeTint(blendColor(
                       roadColor(far.terrain, alternate),
                       roadColor(near.terrain, alternate), 0.65),
                       near.gradePercent),
                   far.depthMeters, near.depthMeters);
    }
}

bool projectedSliceAt(
        const WorkoutGameRoadProjectionFrame &projection,
        double worldDistanceMeters,
        WorkoutGameRoadProjectedSlice &result)
{
    if (projection.slices.size() < 2
            || worldDistanceMeters > projection.slices.front().worldDistanceMeters
            || worldDistanceMeters < projection.slices.back().worldDistanceMeters) {
        return false;
    }
    for (std::size_t index = 1; index < projection.slices.size(); ++index) {
        const WorkoutGameRoadProjectedSlice &far = projection.slices[index - 1];
        const WorkoutGameRoadProjectedSlice &near = projection.slices[index];
        if (worldDistanceMeters > far.worldDistanceMeters
                || worldDistanceMeters < near.worldDistanceMeters) {
            continue;
        }
        const double span = far.worldDistanceMeters - near.worldDistanceMeters;
        const double amount = span > 1e-9
                ? (far.worldDistanceMeters - worldDistanceMeters) / span
                : 0.0;
        result = near;
        result.worldDistanceMeters = worldDistanceMeters;
        result.depthMeters = far.depthMeters
                + (near.depthMeters - far.depthMeters) * amount;
        result.centerX = far.centerX + (near.centerX - far.centerX) * amount;
        result.centerY = far.centerY + (near.centerY - far.centerY) * amount;
        result.halfWidthPixels = far.halfWidthPixels
                + (near.halfWidthPixels - far.halfWidthPixels) * amount;
        result.halfWidthMeters = far.halfWidthMeters
                + (near.halfWidthMeters - far.halfWidthMeters) * amount;
        result.pixelsPerMeter = far.pixelsPerMeter
                + (near.pixelsPerMeter - far.pixelsPerMeter) * amount;
        result.surfaceOffsetMeters = far.surfaceOffsetMeters
                + (near.surfaceOffsetMeters - far.surfaceOffsetMeters) * amount;
        result.surfaceElevationMeters = far.surfaceElevationMeters
                + (near.surfaceElevationMeters
                   - far.surfaceElevationMeters) * amount;
        result.gradePercent = far.gradePercent
                + (near.gradePercent - far.gradePercent) * amount;
        result.occlusionY = far.occlusionY
                + (near.occlusionY - far.occlusionY) * amount;
        return true;
    }
    return false;
}

void buildMotionCueGeometry(
        const WorkoutGameRoadProjectionFrame &projection,
        std::vector<SceneTriangle> &geometry)
{
    if (projection.slices.size() < 2) return;
    constexpr double SpacingMeters = 4.0;
    constexpr double LengthMeters = 0.55;
    const double nearDistance = projection.slices.back().worldDistanceMeters;
    const double farDistance = projection.slices.front().worldDistanceMeters;
    const std::int64_t firstMarker = std::int64_t(
            std::ceil(nearDistance / SpacingMeters));
    for (std::int64_t markerIndex = firstMarker;; ++markerIndex) {
        const double markerDistance = double(markerIndex) * SpacingMeters;
        if (markerDistance + LengthMeters > farDistance) break;
        WorkoutGameRoadProjectedSlice near;
        WorkoutGameRoadProjectedSlice far;
        if (!projectedSliceAt(projection, markerDistance, near)
                || !projectedSliceAt(
                    projection, markerDistance + LengthMeters, far)) {
            continue;
        }
        const double lateral = markerIndex % 3 == 0
                ? -0.32 : markerIndex % 3 == 1 ? 0.08 : 0.35;
        const float nearCenter = float(near.centerX
                + lateral * near.halfWidthPixels);
        const float farCenter = float(far.centerX
                + lateral * far.halfWidthPixels);
        const float nearHalfWidth = std::max(
                1.2f, float(near.halfWidthPixels * 0.07));
        const float farHalfWidth = std::max(
                0.7f, float(far.halfWidthPixels * 0.07));
        if (!clipProjectedPair(far, near)) continue;
        appendQuad(geometry,
                   farCenter - farHalfWidth, float(far.centerY),
                   farCenter + farHalfWidth,
                   nearCenter - nearHalfWidth, float(near.centerY),
                   nearCenter + nearHalfWidth,
                   markerIndex & 1 ? QColor(91, 69, 46, 210)
                                   : QColor(155, 122, 77, 190),
                   far.depthMeters, near.depthMeters);
    }
}

void appendCourseBand(
        const WorkoutGameRoadProjectionFrame &projection,
        double startDistanceMeters,
        double endDistanceMeters,
        double halfWidthRatio,
        const QColor &color,
        std::vector<SceneTriangle> &geometry)
{
    WorkoutGameRoadProjectedSlice far;
    WorkoutGameRoadProjectedSlice near;
    if (!projectedSliceAt(projection, endDistanceMeters, far)
            || !projectedSliceAt(projection, startDistanceMeters, near)) return;
    constexpr double ElevationMeters = 0.035;
    far.centerY -= ElevationMeters * far.pixelsPerMeter;
    near.centerY -= ElevationMeters * near.pixelsPerMeter;
    if (!clipProjectedPair(far, near)) return;
    appendQuad(geometry,
               float(far.centerX
                     - far.halfWidthPixels * halfWidthRatio),
               float(far.centerY),
               float(far.centerX
                     + far.halfWidthPixels * halfWidthRatio),
               float(near.centerX
                     - near.halfWidthPixels * halfWidthRatio),
               float(near.centerY),
               float(near.centerX
                     + near.halfWidthPixels * halfWidthRatio),
               color, far.depthMeters, near.depthMeters);
}

void buildPowerCueGeometry(
        const WorkoutGameRoadProjectionFrame &projection,
        const WorkoutGameFeatureRuntimeSnapshot &active,
        WorkoutGameChallengeCue cue,
        std::vector<SceneTriangle> &geometry)
{
    const std::vector<WorkoutGamePowerCueBand> bands =
            WorkoutGamePowerCueGeometry::build(active, cue);
    for (const WorkoutGamePowerCueBand &band : bands) {
        appendCourseBand(
                projection,
                band.startDistanceMeters,
                band.endDistanceMeters,
                band.halfWidthRatio,
                band.decision
                    ? QColor(245, 231, 98, 235)
                    : QColor(241, 184, 58, 185),
                geometry);
    }
}

QString graphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software: return QStringLiteral("QSG SW");
    case QSGRendererInterface::OpenGL: return QStringLiteral("QSG GL");
    case QSGRendererInterface::Direct3D11: return QStringLiteral("QSG D3D11");
    case QSGRendererInterface::Vulkan: return QStringLiteral("QSG Vulkan");
    case QSGRendererInterface::Metal: return QStringLiteral("QSG Metal");
    case QSGRendererInterface::Null: return QStringLiteral("QSG Null");
    default: return QStringLiteral("QSG");
    }
}

void appendProjectedMesh(
        const WorkoutGameMeshInstance &instance,
        const WorkoutGameRoadProjectionFrame &projection,
        bool selectedBypass,
        std::vector<SceneTriangle> &geometry)
{
    const std::vector<WorkoutGameProjectedMeshTriangle> triangles =
            WorkoutGameMeshProjector::project(instance, projection);
    geometry.reserve(geometry.size() + triangles.size());
    for (const WorkoutGameProjectedMeshTriangle &triangle : triangles) {
        appendTriangle(
                geometry,
                float(triangle.vertices[0].x), float(triangle.vertices[0].y),
                float(triangle.vertices[1].x), float(triangle.vertices[1].y),
                float(triangle.vertices[2].x), float(triangle.vertices[2].y),
                meshColor(triangle.material, selectedBypass),
                triangle.depthMeters);
    }
}

void buildFeatureGeometry(
        const WorkoutGameCourse &workout,
        const WorkoutGameRoadCourse &course,
        const WorkoutGameRoadProjectionFrame &projection,
        const WorkoutGameFeatureRuntimeSnapshot &active,
        std::vector<SceneTriangle> &geometry)
{
    if (projection.slices.empty()) return;
    for (const WorkoutGameRoadPiece &piece : course.pieces) {
        if (!piece.challenge.enabled) continue;
        const double obstacle = piece.challenge.obstacleDistanceMeters;
        const double branchStart = piece.challenge.decisionDistanceMeters;
        double sectionEnd = piece.startDistanceMeters + piece.lengthMeters;
        double sectionStart = piece.startDistanceMeters;
        if (piece.sourceSectionIndex < course.timeline.size()) {
            sectionStart = course.timeline[piece.sourceSectionIndex]
                    .startDistanceMeters;
            sectionEnd = course.timeline[piece.sourceSectionIndex]
                    .endDistanceMeters;
        }
        const double branchEnd = std::min(
                sectionEnd,
                std::max(branchStart + 1.0,
                         sectionStart + (sectionEnd - sectionStart) * 0.96));
        const double branchLength = std::max(1.0, branchEnd - branchStart);
        const bool selectedBypass = active.ready
                && active.sourceSectionIndex == int(piece.sourceSectionIndex)
                && active.route == WorkoutGameRoute::SafeBypass;
        const double direction = (piece.sourceSectionIndex & 1u) == 0u
                ? -1.0 : 1.0;
        WorkoutGameMeshInstance bypass;
        bypass.mesh = WorkoutGameMeshLibrary::bypassRibbon(
                branchLength, direction * 2.2, 0.50);
        bypass.anchorDistanceMeters = branchStart;
        bypass.anchorToBaseSurface = true;
        appendProjectedMesh(bypass, projection, selectedBypass, geometry);

        const double difficulty = piece.sourceSectionIndex
                < workout.sections.size()
                ? workout.sections[piece.sourceSectionIndex].difficulty
                : 0.5;
        WorkoutGameMeshInstance obstacleMesh;
        obstacleMesh.mesh = WorkoutGameMeshLibrary::feature(
                piece.terrain, difficulty);
        obstacleMesh.anchorDistanceMeters = obstacle;
        obstacleMesh.anchorToBaseSurface = true;
        appendProjectedMesh(obstacleMesh, projection, false, geometry);
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
    riderImage(QStringLiteral(":/images/workout-game-rider-chase-sheet.png"))
{
    setFlag(ItemHasContents, true);
    diagnosticsEnabled = environmentEnabled("GC_WORKOUT_GAME_DIAGNOSTICS");
    traceEnabled = environmentEnabled("GC_WORKOUT_GAME_TRACE");
    visualClock.start();
    rebuildHud();
}

void WorkoutGameSceneGraphItem::setCourse(
        const WorkoutGameCourse &course,
        double ftpWatts)
{
    currentCourse = course;
    roadCourse = WorkoutGameRoadCourseBuilder::build(course, ftpWatts);
    currentFrame = {};
    visualSmoother.reset();
    frameRateResetRequested.store(true, std::memory_order_release);
    diagnostics.reset();
    publishedDiagnostics = {};
    frameNumber = 0;
    lastDiagnosticsPublishMs = -1;
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
    visualSmoother.setTarget(
            frame, WorkoutGameClock::monotonicMilliseconds());
    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    if (lastHudRebuildMs < 0 || nowMs - lastHudRebuildMs >= 100) {
        rebuildHud();
    }
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
    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    if (lastHudRebuildMs < 0 || nowMs - lastHudRebuildMs >= 100) {
        rebuildHud();
    }
    update();
}

void WorkoutGameSceneGraphItem::setRendererLabel(const QString &label)
{
    const QString normalized = label.trimmed();
    rendererLabel = normalized.isEmpty()
            ? QStringLiteral("QSG") : normalized;
    rebuildHud();
    update();
}

void WorkoutGameSceneGraphItem::setSessionRunning(bool running)
{
    if (running != sessionRunning) {
        diagnostics.reset();
        publishedDiagnostics = {};
        lastDiagnosticsPublishMs = -1;
    }
    if (running && !sessionRunning) {
        frameRateResetRequested.store(true, std::memory_order_release);
    }
    sessionRunning = running;
    update();
}

void WorkoutGameSceneGraphItem::framePresented()
{
    if (frameRateResetRequested.exchange(false, std::memory_order_acq_rel)) {
        frameRateCounter.reset();
    }
    frameRateCounter.frameRenderedNanoseconds(visualClock.nsecsElapsed());
}

void WorkoutGameSceneGraphItem::rebuildHud()
{
    lastHudRebuildMs = WorkoutGameClock::monotonicMilliseconds();
    constexpr int StatsHeight = 74;
    constexpr int ProfileHeight = 62;
    constexpr int BaseHudHeight = StatsHeight + ProfileHeight + 4;
    const int hudHeight = diagnosticsEnabled ? BaseHudHeight + 78 : BaseHudHeight;
    hudImage = QImage(1240, hudHeight,
                      QImage::Format_RGBA8888_Premultiplied);
    hudImage.fill(Qt::transparent);
    QPainter painter(&hudImage);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(hudImage.rect(), QColor(16, 24, 25, 224));
    painter.fillRect(0, hudHeight - 4, hudImage.width(), 4,
                     QColor(217, 176, 65));
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
        {tr("GRADE"), QStringLiteral("%1%").arg(
                currentFrame.world.gradePercent, 0, 'f', 1)},
        {tr("GEAR"), QString::number(virtualGear)},
        {tr("TIME"), elapsedText(currentFrame.simulation.workoutTimeMs)},
        {rendererLabel + QStringLiteral(" FPS"),
         QString::number(displayedFps, 'f', 1)}
    };
    const int columnWidth = hudImage.width() / int(stats.size());
    for (std::size_t index = 0; index < stats.size(); ++index) {
        const QRect column(int(index) * columnWidth, 0, columnWidth, StatsHeight);
        painter.setFont(labelFont);
        painter.setPen(QColor(154, 181, 167));
        painter.drawText(column.adjusted(8, 6, -4, 0),
                         Qt::AlignLeft | Qt::AlignTop, stats[index].label);
        painter.setFont(valueFont);
        painter.setPen(Qt::white);
        painter.drawText(column.adjusted(8, 28, -4, -4),
                         Qt::AlignLeft | Qt::AlignTop, stats[index].value);
    }
    const WorkoutGamePowerProfileSnapshot profile =
            WorkoutGamePowerProfile::build(
                currentCourse, currentFrame.simulation, watts);
    const QRect profileArea(8, StatsHeight + 3, 930, ProfileHeight - 9);
    painter.fillRect(profileArea, QColor(7, 13, 14, 238));
    if (profile.ready) {
        const double graphBottom = profileArea.bottom() - 3.0;
        const double graphHeight = profileArea.height() - 8.0;
        for (const WorkoutGamePowerProfileSegment &segment : profile.segments) {
            const double left = profileArea.left()
                    + segment.start * profileArea.width();
            const double right = profileArea.left()
                    + segment.end * profileArea.width();
            const double height = graphHeight * segment.targetWatts
                    / profile.maximumWatts;
            painter.fillRect(QRectF(left, graphBottom - height,
                                    std::max(1.0, right - left), height),
                             QColor(78, 151, 139));
            if (segment.challenge) {
                const double challengeLeft = profileArea.left()
                        + segment.challengeStart * profileArea.width();
                const double challengeRight = profileArea.left()
                        + segment.challengeEnd * profileArea.width();
                painter.fillRect(QRectF(challengeLeft, profileArea.top(),
                                        std::max(1.0,
                                            challengeRight - challengeLeft),
                                        profileArea.height()),
                                 QColor(239, 174, 54, 92));
            }
        }
        const double cursorX = profileArea.left()
                + profile.cursor * profileArea.width();
        painter.fillRect(QRectF(cursorX - 1.0, profileArea.top(),
                                3.0, profileArea.height()),
                         QColor(250, 231, 91));
        const double actualY = graphBottom - graphHeight
                * std::min(profile.actualWatts, profile.maximumWatts)
                / profile.maximumWatts;
        painter.setBrush(QColor(255, 255, 255));
        painter.setPen(QColor(12, 18, 18));
        painter.drawEllipse(QPointF(cursorX, actualY), 4.0, 4.0);
    }

    const QRect cueArea(952, StatsHeight + 3, 280, ProfileHeight - 9);
    painter.fillRect(cueArea, QColor(7, 13, 14, 238));
    QString cueText = tr("RIDE STEADY");
    QColor cueColor(151, 181, 169);
    if (profile.ready) {
        switch (profile.cue.state) {
        case WorkoutGamePowerCueState::Prepare:
            cueText = tr("PUSH IN %1 s").arg(
                    int(std::ceil(profile.cue.secondsUntilWindow)));
            cueColor = QColor(242, 190, 67);
            break;
        case WorkoutGamePowerCueState::PushNow:
            cueText = tr("PUSH NOW >= %1 W").arg(
                    int(std::lround(profile.cue.requiredWatts)));
            cueColor = QColor(250, 231, 91);
            break;
        case WorkoutGamePowerCueState::Committed:
            cueText = tr("FEATURE COMMITTED");
            cueColor = QColor(99, 207, 136);
            break;
        case WorkoutGamePowerCueState::Bypassed:
            cueText = tr("SAFE LINE");
            cueColor = QColor(181, 139, 72);
            break;
        case WorkoutGamePowerCueState::None:
            break;
        }
    }
    QFont cueFont = labelFont;
    cueFont.setPixelSize(17);
    painter.setFont(cueFont);
    painter.setPen(cueColor);
    painter.drawText(cueArea.adjusted(8, 4, -8, -19),
                     Qt::AlignCenter, cueText);
    const int readinessWidth = int((cueArea.width() - 16)
            * std::clamp(profile.cue.readiness, 0.0, 1.0));
    painter.fillRect(cueArea.left() + 8, cueArea.bottom() - 10,
                     cueArea.width() - 16, 6, QColor(42, 61, 57));
    painter.fillRect(cueArea.left() + 8, cueArea.bottom() - 10,
                     readinessWidth, 6, cueColor);

    if (diagnosticsEnabled) {
        const WorkoutGameDiagnosticsSnapshot &snapshot = publishedDiagnostics;
        painter.fillRect(0, BaseHudHeight, hudImage.width(), 78,
                         QColor(8, 13, 14, 230));
        QFont diagnosticFont = labelFont;
        diagnosticFont.setPixelSize(12);
        painter.setFont(diagnosticFont);
        painter.setPen(snapshot.backwardFrameCount > 0
                ? QColor(255, 116, 96) : QColor(186, 211, 198));
        const auto &input = snapshot.input;
        painter.drawText(
                QRect(8, BaseHudHeight + 2, hudImage.width() - 16, 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral(
                    "ROAD %1 m  SRC %2 m  d %3 m  SEC %4 %5%  "
                    "TIME %6/%7 ms")
                    .arg(input.renderedRoadDistanceMeters, 0, 'f', 2)
                    .arg(input.sourceRoadDistanceMeters, 0, 'f', 2)
                    .arg(snapshot.frameDistanceDeltaMeters, 0, 'f', 3)
                    .arg(input.renderedSection)
                    .arg(int(std::lround(
                            input.renderedSectionProgress * 100.0)))
                    .arg(input.renderedWorkoutTimeMs)
                    .arg(input.sourceWorkoutTimeMs));
        painter.drawText(
                QRect(8, BaseHudHeight + 26, hudImage.width() - 16, 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral(
                    "DT %1 ms  P95 %2 ms  MAX %3 ms  LATE %4  BACK %5  "
                    "STILL %6  SKIP %7  FRAME %8")
                    .arg(snapshot.frameIntervalMs)
                    .arg(input.p95FrameIntervalMs, 0, 'f', 1)
                    .arg(snapshot.largestFrameIntervalMs)
                    .arg(snapshot.lateFrameCount)
                    .arg(snapshot.backwardFrameCount)
                    .arg(snapshot.stationaryFrameCount)
                    .arg(input.skippedSimulationTicks)
                    .arg(input.frameNumber));
        painter.drawText(
                QRect(8, BaseHudHeight + 50, hudImage.width() - 16, 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral(
                    "GRADE %1%  VIEW DZ %2 m  SURFACE %3 m  RIDER %4 m  "
                    "CLEAR %5 m  AIR %6 m  UNEXPECTED %7")
                    .arg(input.renderedGradePercent, 0, 'f', 1)
                    .arg(input.visibleElevationChangeMeters, 0, 'f', 2)
                    .arg(input.surfaceElevationMeters, 0, 'f', 2)
                    .arg(input.riderElevationMeters, 0, 'f', 2)
                    .arg(input.riderClearanceMeters, 0, 'f', 2)
                    .arg(input.airHeightMeters, 0, 'f', 2)
                    .arg(snapshot.unexpectedAirborneFrameCount));
    }
    ++hudRevision;
}

void WorkoutGameSceneGraphItem::publishFps(double fps)
{
    if (std::abs(displayedFps - fps) < 0.5) return;
    displayedFps = fps;
    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    if (lastHudRebuildMs < 0 || nowMs - lastHudRebuildMs >= 100) {
        rebuildHud();
    }
    update();
}

void WorkoutGameSceneGraphItem::publishDiagnostics(
        const WorkoutGameDiagnosticsSnapshot &snapshot)
{
    publishedDiagnostics = snapshot;
    if (traceEnabled && snapshot.ready) {
        const auto &input = snapshot.input;
        qInfo().nospace()
                << "workout-game-trace frame=" << input.frameNumber
                << " mono_ms=" << input.monotonicTimeMs
                << " source_ms=" << input.sourceWorkoutTimeMs
                << " render_ms=" << input.renderedWorkoutTimeMs
                << " source_section=" << input.sourceSection
                << " render_section=" << input.renderedSection
                << " source_progress=" << input.sourceSectionProgress
                << " render_progress=" << input.renderedSectionProgress
                << " source_road_m=" << input.sourceRoadDistanceMeters
                << " render_road_m=" << input.renderedRoadDistanceMeters
                << " delta_m=" << snapshot.frameDistanceDeltaMeters
                << " frame_ms=" << snapshot.frameIntervalMs
                << " max_frame_ms=" << snapshot.largestFrameIntervalMs
                << " late_frames=" << snapshot.lateFrameCount
                << " backwards=" << snapshot.backwardFrameCount
                << " stationary=" << snapshot.stationaryFrameCount
                << " fps=" << input.framesPerSecond
                << " p95_frame_ms=" << input.p95FrameIntervalMs
                << " skipped_ticks=" << input.skippedSimulationTicks
                << " grade=" << input.renderedGradePercent
                << " view_dz_m=" << input.visibleElevationChangeMeters
                << " surface_m=" << input.surfaceElevationMeters
                << " rider_elevation_m=" << input.riderElevationMeters
                << " clearance_m=" << input.riderClearanceMeters
                << " air_height_m=" << input.airHeightMeters
                << " airborne=" << input.riderAirborne
                << " airborne_expected=" << input.airborneExpected
                << " unexpected_airborne_frames="
                    << snapshot.unexpectedAirborneFrameCount
                << " watts=" << watts
                << " target_watts=" << targetWatts
                << " cadence=" << cadenceRpm
                << " hr=" << heartRate
                << " gear=" << virtualGear;
    }
    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    if (diagnosticsEnabled
            && (lastHudRebuildMs < 0 || nowMs - lastHudRebuildMs >= 100)) {
        rebuildHud();
    }
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

    const std::int64_t nowMs = WorkoutGameClock::monotonicMilliseconds();
    const WorkoutGameVisualSnapshot visual = visualSmoother.sample(nowMs);
    const WorkoutGameRoadTimelineSample sourceTimeline =
            WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                    roadCourse, currentFrame.simulation.workoutTimeMs);
    const WorkoutGameRoadTimelineSample renderedTimeline =
            WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                    roadCourse, visual.simulation.workoutTimeMs);
    const WorkoutGameFeatureRuntimeSnapshot &feature = visual.feature;
    WorkoutGameRoadProjectionConfig config;
    config.viewportWidth = viewportWidth;
    config.viewportHeight = viewportHeight;
    if (visual.camera.ready) {
        config.cameraElevationMeters = visual.camera.centerElevationMeters;
    }
    const double riderDistance = visual.world.ready
            ? visual.world.rider.distanceMeters
            : renderedTimeline.ready
            ? renderedTimeline.distanceMeters
            : roadCourse.totalLengthMeters
                * visual.simulation.courseProgress;
    const WorkoutGameRoadProjectionFrame projection =
            WorkoutGameRoadProjection::project(
                    roadCourse, riderDistance, config);

    std::vector<SceneTriangle> terrain;
    if (projection.ready) {
        WorkoutGameChallengeCue cue = WorkoutGameChallengeCue::None;
        if (feature.sourceSectionIndex >= 0
                && feature.sourceSectionIndex
                    < int(currentCourse.sections.size())) {
            cue = WorkoutGameFeatureChallenge::profile(
                    currentCourse.sections[std::size_t(
                        feature.sourceSectionIndex)]).cue;
        }
        buildRoadGeometry(projection, viewportWidth, terrain);
        buildMotionCueGeometry(projection, terrain);
        buildPowerCueGeometry(projection, feature, cue, terrain);
        buildFeatureGeometry(
                currentCourse, roadCourse, projection, feature, terrain);
    }
    updateGeometry(root->terrain, sortedVertices(std::move(terrain)));

    constexpr int RiderColumns = 4;
    constexpr int RiderRows = 2;
    constexpr int RiderFrameCount = RiderColumns * RiderRows;
    const int riderFrameWidth = riderImage.width() / RiderColumns;
    const int riderFrameHeight = riderImage.height() / RiderRows;
    const int riderFrame = cadenceRpm > 0
            ? int(std::floor(visual.riderPedalCycles * RiderFrameCount))
                    % RiderFrameCount
            : 0;
    root->rider->setSourceRect(QRectF(
            (riderFrame % RiderColumns) * riderFrameWidth,
            (riderFrame / RiderColumns) * riderFrameHeight,
            riderFrameWidth, riderFrameHeight));
    const double riderWidth = std::clamp(
            viewportWidth * 0.11, 90.0, 165.0);
    const double riderHeight = riderWidth * riderFrameHeight
            / double(std::max(1, riderFrameWidth));
    const double featureShake = feature.vibration > 0.0
            ? feature.vibration
                * std::sin(feature.visualDistanceMeters * 18.0) * 12.0
            : 0.0;
    const double physicsLift = visual.world.ready
            ? visual.world.rider.airHeightMeters() * 18.0
            : 0.0;
    const double landingCompression = std::max(
            visual.world.landingImpact, feature.landingImpact) * 12.0;
    const double bob = visual.world.ready
            ? physicsLift
                - (visual.world.rider.rearSuspension
                    + visual.world.rider.frontSuspension) * 3.0
                + featureShake
                - landingCompression
            : 0.0;
    const double riderX = (projection.ready
            ? projection.riderScreenX : viewportWidth * 0.5)
            + feature.lateralOffset * viewportWidth * 0.12;
    const double riderY = projection.ready
            ? projection.riderScreenY : viewportHeight * 0.82;
    root->rider->setRect(
            riderX - riderWidth * 0.5,
            riderY - riderHeight * 0.78 - bob,
            riderWidth, riderHeight);
    QMatrix4x4 riderTransform;
    const double riderCenterY = riderY - riderHeight * 0.35 - bob;
    riderTransform.translate(float(riderX), float(riderCenterY));
    riderTransform.rotate(
            float(-visual.world.rider.pitchDegrees
                  - feature.pitchDegrees * 0.35),
            0.0f, 0.0f, 1.0f);
    riderTransform.translate(float(-riderX), float(-riderCenterY));
    root->riderTransform->setMatrix(riderTransform);

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

    const double fps = frameRateCounter.framesPerSecond();
    WorkoutGameDiagnosticsInput diagnosticsInput;
    diagnosticsInput.ready = sourceTimeline.ready && renderedTimeline.ready;
    diagnosticsInput.sessionRunning = sessionRunning;
    diagnosticsInput.movingForward = sessionRunning
            && visual.simulation.ready
            && !visual.simulation.finished;
    diagnosticsInput.frameNumber = ++frameNumber;
    diagnosticsInput.monotonicTimeMs = nowMs;
    diagnosticsInput.sourceWorkoutTimeMs =
            currentFrame.simulation.workoutTimeMs;
    diagnosticsInput.renderedWorkoutTimeMs = visual.simulation.workoutTimeMs;
    diagnosticsInput.sourceSection = sourceTimeline.ready
            ? int(sourceTimeline.sourceSectionIndex) : -1;
    diagnosticsInput.renderedSection = renderedTimeline.ready
            ? int(renderedTimeline.sourceSectionIndex) : -1;
    diagnosticsInput.sourceSectionProgress = sourceTimeline.sectionProgress;
    diagnosticsInput.renderedSectionProgress = renderedTimeline.sectionProgress;
    diagnosticsInput.sourceRoadDistanceMeters = sourceTimeline.distanceMeters;
    diagnosticsInput.renderedRoadDistanceMeters = renderedTimeline.distanceMeters;
    diagnosticsInput.framesPerSecond = fps;
    diagnosticsInput.p95FrameIntervalMs =
            frameRateCounter.p95FrameIntervalMilliseconds();
    diagnosticsInput.skippedSimulationTicks =
            currentFrame.skippedSimulationTicks;
    diagnosticsInput.worldReady = visual.world.ready;
    diagnosticsInput.riderAirborne = visual.world.rider.airborne;
    diagnosticsInput.airborneExpected =
            (feature.motion == WorkoutGameFeatureMotion::Jump
             || feature.motion == WorkoutGameFeatureMotion::Drop)
            && (feature.phase == WorkoutGameFeaturePhase::Action
                || feature.phase == WorkoutGameFeaturePhase::Recovery);
    diagnosticsInput.riderElevationMeters =
            visual.world.rider.elevationMeters;
    diagnosticsInput.surfaceElevationMeters =
            visual.world.surfaceElevationMeters;
    diagnosticsInput.riderClearanceMeters =
            visual.world.rider.clearanceMeters;
    diagnosticsInput.airHeightMeters =
            visual.world.rider.airHeightMeters();
    diagnosticsInput.visibleElevationChangeMeters =
            projection.visibleElevationChangeMeters;
    diagnosticsInput.renderedGradePercent =
            projection.renderedGradePercent;
    const WorkoutGameDiagnosticsSnapshot diagnosticSnapshot =
            diagnostics.update(diagnosticsInput);
    if ((diagnosticsEnabled || traceEnabled)
            && (lastDiagnosticsPublishMs < 0
                || nowMs - lastDiagnosticsPublishMs >= 250)) {
        lastDiagnosticsPublishMs = nowMs;
        QMetaObject::invokeMethod(
                this, [this, diagnosticSnapshot]() {
                    publishDiagnostics(diagnosticSnapshot);
                }, Qt::QueuedConnection);
    }
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
    renderClock.start();
    connect(this, &QQuickWindow::frameSwapped,
            sceneItem, [item = sceneItem]() {
                // Direct delivery keeps the timestamp at actual presentation
                // and leaves the counter on Qt's render-loop owner.
                item->framePresented();
            }, Qt::DirectConnection);
    connect(this, &QQuickWindow::sceneGraphInitialized,
            this, [this]() {
                const QSGRendererInterface::GraphicsApi api =
                        rendererInterface()->graphicsApi();
                activeRendererLabel = graphicsApiName(api);
                sceneItem->setRendererLabel(activeRendererLabel);
                const double refreshRate = screen()
                        ? screen()->refreshRate() : 0.0;
                qInfo().noquote()
                        << "Workout Game scene graph initialized:"
                        << activeRendererLabel
                        << "api=" << int(api)
                        << "refresh=" << refreshRate << "Hz";
            });
    connect(this, &QQuickWindow::frameSwapped,
            this, [this]() {
                // Let Qt's presentation loop pace interpolation. A queued call
                // keeps QQuickItem::update() on the GUI thread and avoids an
                // independent 16 ms timer fighting display vsync.
                if (isVisible()
                        && (sessionRunning
                            || renderClock.elapsed() <= renderUntilMs)) {
                    sceneItem->update();
                }
            }, Qt::QueuedConnection);
    connect(this, &QQuickWindow::sceneGraphError,
            this, [this](QQuickWindow::SceneGraphError, const QString &message) {
                if (failureReported) return;
                failureReported = true;
                qWarning().noquote() << "Workout Game scene graph error:"
                                     << message;
                emit rendererFailed();
            });
    captureDirectory = QString::fromLocal8Bit(
            qgetenv("GC_WORKOUT_GAME_CAPTURE_DIR")).trimmed();
    if (!captureDirectory.isEmpty()) {
        QDir().mkpath(captureDirectory);
        bool intervalOk = false;
        int intervalMs = QString::fromLocal8Bit(
                qgetenv("GC_WORKOUT_GAME_CAPTURE_MS"))
                .toInt(&intervalOk);
        intervalMs = intervalOk ? std::clamp(intervalMs, 50, 5000) : 100;
        bool frameLimitOk = false;
        int frameLimit = QString::fromLocal8Bit(
                qgetenv("GC_WORKOUT_GAME_CAPTURE_FRAMES"))
                .toInt(&frameLimitOk);
        frameLimit = frameLimitOk ? std::clamp(frameLimit, 1, 10000) : 300;
        auto *captureTimer = new QTimer(this);
        captureTimer->setInterval(intervalMs);
        connect(captureTimer, &QTimer::timeout, this,
                [this, captureTimer, frameLimit]() {
            if (captureFrameNumber >= std::uint64_t(frameLimit)) {
                captureTimer->stop();
                return;
            }
            if (!isVisible() || !sessionRunning) return;
            const QImage frame = grabWindow();
            if (frame.isNull()) return;
            const QString fileName = QStringLiteral(
                    "frame-%1.png").arg(
                        ++captureFrameNumber, 6, 10, QLatin1Char('0'));
            if (!frame.save(QDir(captureDirectory).filePath(fileName))) {
                qWarning() << "Workout Game capture failed:" << fileName;
            }
        });
        captureTimer->start();
    }
}

WorkoutGameSceneGraphWindow::~WorkoutGameSceneGraphWindow()
{
    // frameSwapped can be emitted while QQuickWindow tears down its scene graph.
    // Disconnect while the derived members captured by the callbacks still live.
    disconnect(this, nullptr, nullptr, nullptr);
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
    renderUntilMs = renderClock.elapsed() + 1700;
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

void WorkoutGameSceneGraphWindow::setSessionRunning(bool running)
{
    sessionRunning = running;
    sceneItem->setSessionRunning(running);
    if (!running) {
        renderUntilMs = renderClock.elapsed() + 250;
    }
}

void WorkoutGameSceneGraphWindow::resizeEvent(QResizeEvent *event)
{
    QQuickWindow::resizeEvent(event);
    sceneItem->setSize(event->size());
}
