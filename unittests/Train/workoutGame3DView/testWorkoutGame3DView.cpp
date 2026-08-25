/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGame3DWindow.h"
#include "WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameFeatureRuntime.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSGRendererInterface>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {

constexpr double FtpWatts = 200.0;

class ScopedEnvironmentVariable
{
public:
    explicit ScopedEnvironmentVariable(const char *variable) :
        name(variable),
        value(qgetenv(variable)),
        present(qEnvironmentVariableIsSet(variable))
    {
    }

    ~ScopedEnvironmentVariable()
    {
        if (present) {
            qputenv(name.constData(), value);
        } else {
            qunsetenv(name.constData());
        }
    }

private:
    QByteArray name;
    QByteArray value;
    bool present;
};

struct FeatureCatalogEntry
{
    WorkoutGameTerrainKind terrain;
    const char *name;
};

constexpr std::array<FeatureCatalogEntry, 11> FeatureCatalog = {{
    {WorkoutGameTerrainKind::Roots, "roots"},
    {WorkoutGameTerrainKind::Rollers, "rollers"},
    {WorkoutGameTerrainKind::Climb, "climb"},
    {WorkoutGameTerrainKind::RockGarden, "rock-garden"},
    {WorkoutGameTerrainKind::BunnyHop, "bunny-hop"},
    {WorkoutGameTerrainKind::Drop, "drop"},
    {WorkoutGameTerrainKind::Skinny, "skinny"},
    {WorkoutGameTerrainKind::Berm, "berm"},
    {WorkoutGameTerrainKind::LogOver, "log-over"},
    {WorkoutGameTerrainKind::Tabletop, "tabletop"},
    {WorkoutGameTerrainKind::RockSlab, "rock-slab"}
}};

WorkoutGameCourse catalogCourse(WorkoutGameTerrainKind terrain)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 9173u + std::uint32_t(terrain);
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = terrain == WorkoutGameTerrainKind::Climb
            ? WorkoutGameFeature::Climb
            : terrain == WorkoutGameTerrainKind::BunnyHop
                    || terrain == WorkoutGameTerrainKind::LogOver
                    || terrain == WorkoutGameTerrainKind::Tabletop
            ? WorkoutGameFeature::SprintJump
            : terrain == WorkoutGameTerrainKind::Drop
            ? WorkoutGameFeature::RecoveryDescent
            : WorkoutGameFeature::Trail;
    section.terrain = terrain;
    section.durationMs = course.durationMs;
    section.targetWatts = 220.0;
    section.gradePercent = terrain == WorkoutGameTerrainKind::Climb
            ? 8.0 : terrain == WorkoutGameTerrainKind::Drop ? -6.0 : 0.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x334456u;
    const WorkoutGameTerrainKind terrains[] = {
        WorkoutGameTerrainKind::Climb,
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Tabletop,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::Drop
    };
    std::int64_t startMs = 0;
    for (int index = 0; index < 5; ++index) {
        WorkoutGameSection section;
        section.feature = index == 2
                ? WorkoutGameFeature::SprintJump
                : WorkoutGameFeature::Trail;
        section.terrain = terrains[index];
        section.startMs = startMs;
        section.durationMs = 20000;
        section.targetWatts = 175.0 + index * 22.0;
        section.gradePercent = index == 0 ? 8.0 : (index == 4 ? -7.0 : 2.0);
        section.difficulty = 0.35 + index * 0.1;
        section.challengeCount = 1;
        section.visualVariant = std::uint32_t(index + 1);
        section.gravityAssisted = index == 4;
        course.sections.push_back(section);
        startMs += section.durationMs;
    }
    course.durationMs = startMs;
    return course;
}

WorkoutGameCourse cameraMotionCourse()
{
    WorkoutGameCourse course = sampleCourse();
    const std::array<double, 5> lengths = {{18.0, 16.0, 28.0, 16.0, 24.0}};
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        course.sections[index].lengthMeters = lengths[index];
    }
    return course;
}

WorkoutGameVisualSnapshot frameAt(
        const WorkoutGameRoadCourse &road,
        double distanceMeters)
{
    const WorkoutGameRoadSample roadSample =
            WorkoutGameRoadCourseBuilder::sample(road, distanceMeters);
    WorkoutGameVisualSnapshot frame;
    frame.world.ready = true;
    frame.world.terrain = roadSample.terrain;
    frame.world.rider.distanceMeters = distanceMeters;
    frame.world.rider.elevationMeters = roadSample.center.elevationMeters;
    frame.world.rider.pitchDegrees = roadSample.center.gradePercent * 0.45;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = 75400;
    frame.simulation.speedKph = 22.5;
    frame.riderPedalCycles = distanceMeters * 0.35;
    return frame;
}

int sampledColorCount(const QImage &image)
{
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 8) {
        for (int x = 0; x < image.width(); x += 8) {
            colors.insert(image.pixel(x, y));
        }
    }
    return colors.size();
}

int changedPixels(const QImage &first, const QImage &second)
{
    int changed = 0;
    for (int y = 0; y < first.height(); y += 3) {
        for (int x = 0; x < first.width(); x += 3) {
            const QColor before(first.pixel(x, y));
            const QColor after(second.pixel(x, y));
            if (std::abs(before.red() - after.red())
                    + std::abs(before.green() - after.green())
                    + std::abs(before.blue() - after.blue()) > 35) {
                ++changed;
            }
        }

    }
    return changed;
}

double horizontalDistanceToSegment(
        double pointX,
        double pointZ,
        double startX,
        double startZ,
        double endX,
        double endZ)
{
    const double segmentX = endX - startX;
    const double segmentZ = endZ - startZ;
    const double lengthSquared = segmentX * segmentX + segmentZ * segmentZ;
    const double projection = lengthSquared > 1.0e-9
            ? std::clamp(((pointX - startX) * segmentX
                    + (pointZ - startZ) * segmentZ) / lengthSquared,
                    0.0, 1.0)
            : 0.0;
    const double offsetX = pointX - (startX + projection * segmentX);
    const double offsetZ = pointZ - (startZ + projection * segmentZ);
    return std::hypot(offsetX, offsetZ);
}

double normalizedRadians(double angle)
{
    constexpr double pi = 3.14159265358979323846;
    while (angle > pi) angle -= 2.0 * pi;
    while (angle < -pi) angle += 2.0 * pi;
    return angle;
}

QQuickItem *findVisualItem(QQuickItem *parent, const QString &objectName)
{
    if (!parent) return nullptr;
    if (parent->objectName() == objectName) return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (QQuickItem *match = findVisualItem(child, objectName)) return match;
    }
    return nullptr;
}

double tabletopApproachDistance(const WorkoutGameRoadCourse &road)
{
    for (const WorkoutGameRoadPiece &piece : road.pieces) {
        if (piece.terrain == WorkoutGameTerrainKind::Tabletop
                && piece.challenge.enabled) {
            return std::max(0.0,
                    piece.challenge.obstacleDistanceMeters - 5.0);
        }
    }
    return 64.0;
}

bool hasInteractiveGraphicsPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen")
            && platform != QStringLiteral("minimal");
}

}

class TestWorkoutGame3DView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    void cameraCompositionDefaultsToCentreAndSupportsAuditVariants()
    {
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");

        qunsetenv("GC_WORKOUT_GAME_3D_CAMERA");
        {
            WorkoutGame3DViewModel medium;
            QCOMPARE(medium.cameraComposition(),
                     QStringLiteral("medium-centre"));
            QCOMPARE(medium.cameraSideMeters(), 0.0);
            QCOMPARE(medium.cameraBackMeters(), 8.2);
            QCOMPARE(medium.cameraHeightMeters(), 3.2);
            QCOMPARE(medium.cameraLookAheadMeters(), 12.0);
        }

        qputenv("GC_WORKOUT_GAME_3D_CAMERA", "low-centre");
        {
            WorkoutGame3DViewModel low;
            QCOMPARE(low.cameraComposition(), QStringLiteral("low-centre"));
            QCOMPARE(low.cameraSideMeters(), 0.0);
            QCOMPARE(low.cameraBackMeters(), 7.4);
            QCOMPARE(low.cameraHeightMeters(), 2.55);
            QCOMPARE(low.cameraTargetHeightMeters(), 0.75);
        }

        qputenv("GC_WORKOUT_GAME_3D_CAMERA", "shoulder");
        {
            WorkoutGame3DViewModel shoulder;
            QCOMPARE(shoulder.cameraComposition(), QStringLiteral("shoulder"));
            QVERIFY(shoulder.cameraSideMeters() > 0.0);
            QVERIFY(shoulder.cameraSideMeters() < 0.68);
        }

    }

    void cameraFollowsRoadAndMaintainsTerrainClearance()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 1.0) {
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            QVERIFY(std::isfinite(viewModel.cameraX()));
            QVERIFY(std::isfinite(viewModel.cameraY()));
            QVERIFY(std::isfinite(viewModel.cameraZ()));
            QVERIFY(std::isfinite(viewModel.cameraTargetX()));
            QVERIFY(std::isfinite(viewModel.cameraTargetY()));
            QVERIFY(std::isfinite(viewModel.cameraTargetZ()));

            const double cameraDistance = std::max(
                    0.0, distance - viewModel.cameraBackMeters());
            const WorkoutGameRoadSample cameraSample =
                    WorkoutGameRoadCourseBuilder::sample(
                            road, cameraDistance);
            QVERIFY(cameraSample.ready);
            const double cameraSurface =
                    cameraSample.center.elevationMeters
                    - cameraSample.nonPhysicalFeatureOffsetMeters;
            QVERIFY2(viewModel.cameraY() - cameraSurface >= 2.54,
                     "camera entered the terrain exclusion height");

            const double cameraToTarget = std::hypot(
                    viewModel.cameraTargetX() - viewModel.cameraX(),
                    viewModel.cameraTargetZ() - viewModel.cameraZ());
            QVERIFY2(cameraToTarget >= 10.0,
                     "camera target collapsed into the camera position");
            QVERIFY2(cameraToTarget <= 24.0,
                     "camera target escaped the bounded chase composition");
        }
    }

    void treesStayOutsideCameraAndCueCorridor()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        int inspectedTrees = 0;

        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 0.5) {
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            for (const QVariant &entry : viewModel.trees()) {
                const QVariantMap tree = entry.toMap();
                const double clearance = horizontalDistanceToSegment(
                        tree.value(QStringLiteral("x")).toDouble(),
                        tree.value(QStringLiteral("z")).toDouble(),
                        viewModel.cameraX(), viewModel.cameraZ(),
                        viewModel.cameraTargetX(), viewModel.cameraTargetZ());
                const double required = tree.value(
                        QStringLiteral("crownRadius")).toDouble() + 0.85;
                QVERIFY2(clearance + 1.0e-6 >= required,
                         qPrintable(QStringLiteral(
                             "tree clearance %1 is below required %2")
                             .arg(clearance).arg(required)));
                ++inspectedTrees;
            }
        }
        QVERIFY2(inspectedTrees >= 1000,
                 "camera exclusion removed the forest instead of relocating it");
    }

    void treesAreAnchoredToGeneratedTerrain()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 30.0), 220.0, 220.0, 88, 150, 7);
        QVERIFY(!viewModel.trees().isEmpty());

        int reliefAnchors = 0;
        for (const QVariant &entry : viewModel.trees()) {
            const QVariantMap tree = entry.toMap();
            const double distance = tree.value(
                    QStringLiteral("distance")).toDouble();
            const double lateral = tree.value(
                    QStringLiteral("lateral")).toDouble();
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                        sample, distance, road.seed);
            QVERIFY(terrain.ready);
            const double expected =
                    WorkoutGame3DTerrainProfile::elevationAtLateral(
                        terrain, lateral);
            QVERIFY(std::abs(tree.value(QStringLiteral("y")).toDouble()
                             - expected) < 1.0e-9);
            if (std::abs(expected - sample.baseElevationMeters) > 0.20) {
                ++reliefAnchors;
            }
        }
        QVERIFY2(reliefAnchors > 0,
                 "trees still appear to use the former flat floor elevation");
    }

    void cameraMotionIsContinuousAndBounded()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        constexpr int frameCount = 360;
        double priorX = 0.0;
        double priorY = 0.0;
        double priorZ = 0.0;
        double priorYaw = 0.0;
        double priorYawStep = 0.0;
        double maximumStep = 0.0;
        double maximumYawStep = 0.0;
        double maximumYawAcceleration = 0.0;

        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            const double progress = double(frameIndex)
                    / double(frameCount - 1);
            const double distance = 2.0
                    + (road.totalLengthMeters - 4.0) * progress;
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            const double yaw = std::atan2(
                    viewModel.cameraTargetX() - viewModel.cameraX(),
                    viewModel.cameraTargetZ() - viewModel.cameraZ());
            if (frameIndex > 0) {
                maximumStep = std::max(maximumStep, std::sqrt(
                        std::pow(viewModel.cameraX() - priorX, 2.0)
                        + std::pow(viewModel.cameraY() - priorY, 2.0)
                        + std::pow(viewModel.cameraZ() - priorZ, 2.0)));
                const double yawStep = normalizedRadians(yaw - priorYaw);
                maximumYawStep = std::max(
                        maximumYawStep, std::abs(yawStep));
                if (frameIndex > 1) {
                    maximumYawAcceleration = std::max(
                            maximumYawAcceleration,
                            std::abs(normalizedRadians(
                                    yawStep - priorYawStep)));
                }
                priorYawStep = yawStep;
            }
            priorX = viewModel.cameraX();
            priorY = viewModel.cameraY();
            priorZ = viewModel.cameraZ();
            priorYaw = yaw;
        }

        QVERIFY2(maximumStep <= 0.55,
                 qPrintable(QStringLiteral("camera step reached %1 m")
                         .arg(maximumStep)));
        QVERIFY2(maximumYawStep <= 0.10,
                 qPrintable(QStringLiteral("camera yaw step reached %1 rad")
                         .arg(maximumYawStep)));
        QVERIFY2(maximumYawAcceleration <= 0.05,
                 qPrintable(QStringLiteral(
                         "camera yaw acceleration reached %1 rad/frame^2")
                         .arg(maximumYawAcceleration)));
    }

    void featureHudSeparatesPowerCadenceDistanceAndState()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.visualDistanceMeters = 12.0;
        frame.feature.decisionDistanceMeters = 17.5;
        frame.feature.actionStartDistanceMeters = 21.0;
        frame.simulation.challenge.enabled = true;
        frame.simulation.challenge.minimumEffortRatio = 1.0;
        frame.simulation.challenge.minimumCadenceRpm = 0.0;
        frame.simulation.challengeAssessment.effortReadiness = 0.75;
        frame.simulation.challengeAssessment.cadenceReadiness = 1.0;

        viewModel.setFrame(frame, 165.0, 220.0, 72, 148, 6);

        QVERIFY(viewModel.featureHudVisible());
        QCOMPARE(viewModel.featureName(), QStringLiteral("Tabletop"));
        QCOMPARE(viewModel.featureState(),
                 int(WorkoutGameFeatureHudState::Measure));
        QCOMPARE(viewModel.featureDistanceKind(),
                 int(WorkoutGameFeatureHudDistanceKind::Decision));
        QCOMPARE(viewModel.featureDistanceMeters(), 5.5);
        QCOMPARE(viewModel.requiredPowerWatts(), 220.0);
        QCOMPARE(viewModel.powerReadinessPercent(), 75);
        QVERIFY(!viewModel.cadenceRequired());
        QCOMPARE(viewModel.requiredCadenceRpm(), 0.0);
        QCOMPARE(viewModel.cadenceReadinessPercent(), 100);
    }

    void settingCourseClearsFeatureHudState()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.featureHudVisible());

        viewModel.setCourse(sampleCourse(), FtpWatts);

        QVERIFY(!viewModel.featureHudVisible());
        QVERIFY(viewModel.featureName().isEmpty());
        QVERIFY(viewModel.featureActionText().isEmpty());
        QVERIFY(viewModel.featureStatus().isEmpty());
        QCOMPARE(viewModel.readinessPercent(), 0);
    }

    void featureHudFitsSupportedWidths_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("expectedHeight");
        QTest::newRow("narrow") << QSize(360, 640) << 166.0;
        QTest::newRow("desktop") << QSize(1280, 720) << 112.0;
        QTest::newRow("full-hd") << QSize(1920, 1080) << 112.0;
    }

    void featureHudFitsSupportedWidths()
    {
        QFETCH(QSize, size);
        QFETCH(double, expectedHeight);
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.visualDistanceMeters = 12.0;
        frame.feature.decisionDistanceMeters = 17.5;
        frame.simulation.challenge.enabled = true;
        frame.simulation.challenge.minimumEffortRatio = 1.0;
        frame.simulation.challengeAssessment.effortReadiness = 0.75;
        frame.simulation.challengeAssessment.cadenceReadiness = 1.0;
        viewModel.setFrame(frame, 165.0, 220.0, 72, 148, 6);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(size);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        auto *hud = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureHud"));
        QVERIFY(hud);
        QVERIFY(hud->isVisible());
        QCOMPARE(hud->height(), expectedHeight);
        QVERIFY(hud->x() >= 0.0);
        QVERIFY(hud->y() >= 0.0);
        QVERIFY(hud->x() + hud->width() <= rootItem->width() + 1e-9);
        QVERIFY(hud->y() + hud->height() <= rootItem->height() + 1e-9);

        auto *stateLabel = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureStateLabel"));
        auto *powerBar = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featurePowerBar"));
        auto *cadenceBar = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureCadenceBar"));
        QVERIFY(stateLabel && powerBar && cadenceBar);
        QVERIFY(stateLabel->width() > 0.0);
        QVERIFY(stateLabel->x() >= 0.0);
        QVERIFY(stateLabel->x() + stateLabel->width()
                <= stateLabel->parentItem()->width() + 1e-9);
        QVERIFY(powerBar->width() >= 0.0);
        QVERIFY(powerBar->width() <= powerBar->parentItem()->width() + 1e-9);
        QVERIFY(cadenceBar->width() >= 0.0);
        QVERIFY(cadenceBar->width()
                <= cadenceBar->parentItem()->width() + 1e-9);
    }

    void trainingHudPublishesProfileCursorGradeAndTelemetry()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        const QVariantList profile = viewModel.powerProfileSegments();
        QCOMPARE(profile.size(), int(course.sections.size()));
        QCOMPARE(profile.front().toMap().value(QStringLiteral("start")).toDouble(),
                 0.0);
        QCOMPARE(profile.back().toMap().value(QStringLiteral("end")).toDouble(),
                 1.0);
        QVERIFY(viewModel.powerProfileMaximumWatts() > 240.0);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.simulation.workoutTimeMs = 25000;
        frame.world.gradePercent = 6.25;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);

        QCOMPARE(viewModel.workoutProgress(), 0.25);
        QCOMPARE(viewModel.gradePercent(), 6.25);
        QCOMPARE(viewModel.watts(), 213.0);
        QCOMPARE(viewModel.targetWatts(), 219.0);
        QCOMPARE(viewModel.cadenceRpm(), 87);
        QCOMPARE(viewModel.heartRate(), 151);
        QCOMPARE(viewModel.speedKph(), 22.5);
        QCOMPARE(viewModel.virtualGear(), 8);
        QCOMPARE(viewModel.workoutTimeSeconds(), 25);

        frame.simulation.workoutTimeMs = course.durationMs * 2;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);
        QCOMPARE(viewModel.workoutProgress(), 1.0);
        frame.simulation.workoutTimeMs = -1000;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);
        QCOMPARE(viewModel.workoutProgress(), 0.0);
    }

    void trainingHudFitsAndReportsMissingSensors_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("mobile-aspect") << QSize(360, 640);
        QTest::newRow("laptop") << QSize(1024, 600);
        QTest::newRow("full-hd") << QSize(1920, 1080);
    }

    void trainingHudFitsAndReportsMissingSensors()
    {
        QFETCH(QSize, size);
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.simulation.workoutTimeMs = 25000;
        frame.world.gradePercent = 6.25;
        viewModel.setFrame(frame, 213.0, 219.0, 0, 0, 8);
        viewModel.setFps(58.75);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(size);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        auto *hud = rootItem->findChild<QQuickItem *>(
                QStringLiteral("trainingHud"));
        auto *profile = rootItem->findChild<QQuickItem *>(
                QStringLiteral("powerProfile"));
        auto *cursor = rootItem->findChild<QQuickItem *>(
                QStringLiteral("powerProfileCursor"));
        QVERIFY(hud && profile && cursor);
        QVERIFY(hud->x() >= 0.0 && hud->y() >= 0.0);
        QVERIFY(hud->x() + hud->width() <= rootItem->width() + 1e-9);
        QVERIFY(hud->y() + hud->height() <= rootItem->height() + 1e-9);
        QVERIFY(cursor->x() >= 0.0);
        QVERIFY(cursor->x() + cursor->width() <= profile->width() + 1e-9);

        auto *cadence = findVisualItem(
                rootItem, QStringLiteral("cadenceValue"));
        auto *heartRate = findVisualItem(
                rootItem, QStringLiteral("heartRateValue"));
        auto *grade = findVisualItem(rootItem, QStringLiteral("gradeValue"));
        auto *fps = findVisualItem(rootItem, QStringLiteral("fpsValue"));
        QVERIFY(cadence && heartRate && grade && fps);
        QCOMPARE(cadence->property("text").toString(), QStringLiteral("-- RPM"));
        QCOMPARE(heartRate->property("text").toString(), QStringLiteral("-- BPM"));
        QCOMPARE(grade->property("text").toString(), QStringLiteral("6.3%"));
        QCOMPARE(fps->property("text").toString(), QStringLiteral("58.8 FPS"));

        if (hasInteractiveGraphicsPlatform()) {
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QTest::qWait(120);
            const QImage image = window.grabWindow();
            QVERIFY(!image.isNull());
            QCOMPARE(image.size(), size);
            QVERIFY2(sampledColorCount(image) > 40,
                     "training HUD layout appears blank");
            const QString outputDirectory = qEnvironmentVariable(
                    "GC_WORKOUT_GAME_3D_HUD_SCREENSHOT_DIR");
            if (!outputDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(outputDirectory));
                const QString filename = QDir(outputDirectory).filePath(
                        QStringLiteral("training-hud-%1x%2.png")
                            .arg(size.width()).arg(size.height()));
                QVERIFY2(image.save(filename), qPrintable(filename));
            }
        }

        QQuickItem *cadenceDelegate = cadence;
        viewModel.setTelemetry(225.0, 230.0, 91, 155, 9);
        QCoreApplication::processEvents();
        cadence = findVisualItem(rootItem, QStringLiteral("cadenceValue"));
        QCOMPARE(cadence, cadenceDelegate);
        QCOMPARE(cadence->property("text").toString(), QStringLiteral("91 RPM"));
    }

    void tabletopAssetUsesAuthoritativeRoadAnchorAndProfile()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain
                            == WorkoutGameTerrainKind::Tabletop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                        piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(
                frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::Tabletop);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        const double expectedScaleZ =
                (profile.endMeters - profile.startMeters) / 4.84;
        const double expectedStartDistance = std::clamp(
                piece->challenge.obstacleDistanceMeters
                    + profile.startMeters - 0.75 * expectedScaleZ,
                0.0, road.totalLengthMeters);
        const WorkoutGameRoadSample expected =
                WorkoutGameRoadCourseBuilder::sample(
                        road, expectedStartDistance);
        QVERIFY(expected.ready);
        QCOMPARE(values.value(QStringLiteral("assetX")).toDouble(),
                 expected.center.xMeters);
        QCOMPARE(values.value(QStringLiteral("assetY")).toDouble(),
                 expected.center.elevationMeters
                    - expected.nonPhysicalFeatureOffsetMeters);
        QCOMPARE(values.value(QStringLiteral("assetZ")).toDouble(),
                 expected.center.zMeters);
        QCOMPARE(values.value(QStringLiteral("assetScaleY")).toDouble(),
                 profile.heightMeters / 0.446);
        QCOMPARE(values.value(QStringLiteral("assetScaleZ")).toDouble(),
                 expectedScaleZ);
        QVERIFY(std::isfinite(
                values.value(QStringLiteral("assetPitch")).toDouble()));
        QVERIFY(std::isfinite(
                values.value(QStringLiteral("assetYaw")).toDouble()));
    }

    void packagedTabletopAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_Tabletop_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 11> requiredObjects = {{
            "ROOT_Tabletop",
            "GEO_Tabletop_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_LIP",
            "MARKER_APEX",
            "MARKER_LAND",
            "MAT_TabletopBypass_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }

        QFile mesh(QStringLiteral(
                ":/qml/assets/meshes/geo_Tabletop_LOD0_mesh.mesh"));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.size(), qint64(6976));
    }

    void logOverAssetUsesAuthoritativeRoadAnchorAndProfile()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::LogOver
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::LogOver);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        const double expectedScale = profile.heightMeters / 0.54;
        QCOMPARE(values.value(QStringLiteral("assetScaleY")).toDouble(),
                 expectedScale);
        QCOMPARE(values.value(QStringLiteral("assetScaleZ")).toDouble(),
                 expectedScale);
        QVERIFY(values.contains(QStringLiteral("assetX")));
        QVERIFY(values.contains(QStringLiteral("assetY")));
        QVERIFY(values.contains(QStringLiteral("assetZ")));
    }

    void packagedLogOverAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_LogOver_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 12> requiredObjects = {{
            "ROOT_LogOver",
            "GEO_LogOverTile_LOD0",
            "GEO_LogOverObstacle_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_APEX",
            "MARKER_LAND",
            "MAT_LogOverBark_Grey",
            "MAT_LogOverBypass_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }
        QFile obstacle(QStringLiteral(
                ":/qml/assets/meshes/geo_LogOverObstacle_LOD0_mesh.mesh"));
        QFile tile(QStringLiteral(
                ":/qml/assets/meshes/geo_LogOverTile_LOD0_mesh.mesh"));
        QVERIFY(obstacle.open(QIODevice::ReadOnly));
        QVERIFY(tile.open(QIODevice::ReadOnly));
        QCOMPARE(obstacle.size(), qint64(3508));
        QCOMPARE(tile.size(), qint64(1056));
    }

    void rendersPackagedLogOverAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/LogOverAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 12,
                 "packaged log-over mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_LOG_OVER_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void rendersPackagedTabletopAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/TabletopAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 12,
                 "packaged tabletop mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_TABLETOP_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void loadsRendersAndMovesScene()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        QVERIFY(viewModel.ready());
        QVERIFY(viewModel.trees().size() <= 19);
        QVERIFY(viewModel.features().size() <= 32);
        viewModel.setFrame(
                frameAt(road, 12.0), 215.0, 220.0, 87, 148, 7);
        QCOMPARE(viewModel.workoutTimeSeconds(), 75);
        viewModel.setFps(59.7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(900);

        const QImage first = window.grabWindow();
        QVERIFY(!first.isNull());
        QCOMPARE(first.size(), QSize(1280, 720));
        QVERIFY2(sampledColorCount(first) > 45,
                 "3D scene appears blank or nearly monochrome");

        QObject *firstFloorGeometry = viewModel.floorGeometry();
        viewModel.setFrame(
                frameAt(road, tabletopApproachDistance(road)),
                248.0, 242.0, 93, 154, 9);
        QVERIFY(viewModel.floorGeometry() != firstFloorGeometry);
        auto *floorGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.floorGeometry());
        QVERIFY(floorGeometry);
        QVERIFY(floorGeometry->ready());
        QTest::qWait(350);
        const QImage second = window.grabWindow();
        QVERIFY(!second.isNull());
        const int changed = changedPixels(first, second);
        QVERIFY2(changed > 900,
                 qPrintable(QStringLiteral("only %1 sampled pixels changed")
                         .arg(changed)));

        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_SCREENSHOT",
                QDir(QDir::tempPath()).filePath(
                        QStringLiteral("workout-game-3d-test.png")));
        QVERIFY2(second.save(screenshot), qPrintable(screenshot));
    }

    void productionWindowLoadsAndTearsDown()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        auto window = std::make_unique<WorkoutGame3DWindow>(true);
        QVERIFY(window->rendererAvailable());
        window->setCourse(course, FtpWatts);
        window->setFrame(
                frameAt(road, 18.0), 225.0, 220.0, 88, 149, 8);
        window->resize(960, 540);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 5000);
        QTest::qWait(300);
        const QImage image = window->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(sampledColorCount(image) > 40);
        window->setSessionRunning(true);
        window->setFrame(
                frameAt(road, 24.0), 235.0, 230.0, 90, 151, 9);
        auto *rootItem = qobject_cast<QQuickItem *>(window->rootObject());
        QVERIFY(rootItem);
        QTRY_VERIFY_WITH_TIMEOUT(
                findVisualItem(rootItem, QStringLiteral("fpsValue"))
                    && findVisualItem(rootItem, QStringLiteral("fpsValue"))
                        ->property("text").toString()
                            != QStringLiteral("0.0 FPS"),
                2000);
        window->setSessionRunning(false);
        window.reset();
    }

    void exportsEveryFeatureAtTheLegacyViewpoint()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_FEATURE_CATALOG_DIR");
        const QString outputDirectory = requestedOutput.isEmpty()
                ? QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-3d-feature-catalog"))
                : QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

        for (const FeatureCatalogEntry &entry : FeatureCatalog) {
            const WorkoutGameCourse course = catalogCourse(entry.terrain);
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
            QVERIFY(road.ready);
            const auto challenge = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    });
            QVERIFY(challenge != road.pieces.end());
            const WorkoutGameRoadTimelineSection &timeline =
                    road.timeline.front();
            const double distance = std::max(
                    timeline.startDistanceMeters,
                    challenge->challenge.obstacleDistanceMeters - 10.0);
            const double progress = std::clamp(
                    (distance - timeline.startDistanceMeters)
                        / (timeline.endDistanceMeters
                           - timeline.startDistanceMeters),
                    0.0, 1.0);
            WorkoutGameSimulationSnapshot simulation;
            simulation.ready = true;
            simulation.activeSection = 0;
            simulation.sectionProgress = progress;
            simulation.workoutTimeMs = std::int64_t(std::llround(
                    course.durationMs * progress));
            simulation.courseProgress = progress;
            simulation.speedKph = 20.0;
            simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
            simulation.route = WorkoutGameRoute::MainLine;
            simulation.challengeReadiness = 1.0;
            simulation.challenge = challenge->challenge.profile;
            WorkoutGameFeatureChallengeMetrics metrics;
            metrics.averageActualWatts = 220.0;
            metrics.averageTargetWatts = 220.0;
            metrics.averageEffortRatio = 1.0;
            metrics.averageCadenceRpm = 88.0;
            metrics.averageSpeedKph = 20.0;
            metrics.averageAdherence = 1.0;
            simulation.challengeAssessment =
                    WorkoutGameFeatureChallenge::assess(
                        simulation.challenge, metrics);
            WorkoutGameFeatureRuntime runtime;
            QVERIFY(runtime.configure(road));
            WorkoutGameVisualSnapshot frame;
            frame.simulation = simulation;
            frame.feature = runtime.update(simulation);
            frame.world.ready = true;
            frame.world.generation = 1;
            frame.world.terrain = entry.terrain;
            frame.world.gradePercent = course.sections.front().gradePercent;
            frame.world.rider.distanceMeters = distance;
            frame.world.rider.clearanceMeters = 0.82;
            frame.world.speedMetersPerSecond = 20.0 / 3.6;

            window.setCourse(course, FtpWatts);
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QTest::qWait(350);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QCOMPARE(rendered.size(), QSize(1280, 720));
            QVERIFY2(sampledColorCount(rendered) > 35,
                     "3D feature scene appears blank or nearly monochrome");
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("feature-%1.png")
                        .arg(QString::fromLatin1(entry.name)));
            QVERIFY2(rendered.save(output), qPrintable(output));
        }
    }

    void exportsCameraCompositionCatalog()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_CAMERA_CATALOG_DIR");
        const QString outputDirectory = requestedOutput.isEmpty()
                ? QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-3d-camera-catalog"))
                : QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const WorkoutGameVisualSnapshot frame = frameAt(
                road, tabletopApproachDistance(road));
        const std::array<QByteArray, 3> compositions = {{
            QByteArrayLiteral("low-centre"),
            QByteArrayLiteral("medium-centre"),
            QByteArrayLiteral("shoulder")
        }};
        QImage prior;

        for (const QByteArray &composition : compositions) {
            qputenv("GC_WORKOUT_GAME_3D_CAMERA", composition);
            WorkoutGame3DWindow window(true);
            QVERIFY(window.rendererAvailable());
            window.setCourse(course, FtpWatts);
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            window.resize(1280, 720);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QTest::qWait(350);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QCOMPARE(rendered.size(), QSize(1280, 720));
            QVERIFY(sampledColorCount(rendered) > 35);
            if (!prior.isNull()) {
                QVERIFY2(changedPixels(prior, rendered) > 500,
                         qPrintable(QStringLiteral(
                             "camera composition %1 is not visually distinct")
                             .arg(QString::fromLatin1(composition))));
            }
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("camera-%1.png")
                        .arg(QString::fromLatin1(composition)));
            QVERIFY2(rendered.save(output), qPrintable(output));
            prior = rendered;
        }

    }

    void exportsCameraCompositionMotionFrames()
    {
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_CAMERA_VIDEO_DIR");
        if (requestedOutput.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_3D_CAMERA_VIDEO_DIR to export frames");
        }
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }

        constexpr int frameRate = 30;
        constexpr int frameCount = 360;
        constexpr int width = 960;
        constexpr int height = 540;
        const QString outputDirectory =
                QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        QCOMPARE(road.totalLengthMeters, 102.0);
        const double startDistance = 2.0;
        const double endDistance = road.totalLengthMeters - 2.0;
        const double speedKph = (endDistance - startDistance)
                / (double(frameCount - 1) / double(frameRate)) * 3.6;
        const std::array<QByteArray, 3> compositions = {{
            QByteArrayLiteral("low-centre"),
            QByteArrayLiteral("medium-centre"),
            QByteArrayLiteral("shoulder")
        }};

        for (const QByteArray &composition : compositions) {
            qputenv("GC_WORKOUT_GAME_3D_CAMERA", composition);
            const QString compositionDirectory = QDir(outputDirectory).filePath(
                    QString::fromLatin1(composition));
            QVERIFY(QDir().mkpath(compositionDirectory));
            QDir frames(compositionDirectory);
            for (const QString &stale : frames.entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files)) {
                QVERIFY(frames.remove(stale));
            }

            WorkoutGame3DWindow window(true);
            QVERIFY(window.rendererAvailable());
            window.setCourse(course, FtpWatts);
            window.resize(width, height);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QImage prior;
            int visiblyChangedFrames = 0;

            for (int frameIndex = 0;
                 frameIndex < frameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(frameCount - 1);
                const double distance = startDistance
                        + (endDistance - startDistance) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        1000.0 * double(frameIndex) / double(frameRate)));
                frame.simulation.speedKph = speedKph;
                window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
                QTest::qWait(4);

                const QImage rendered = window.grabWindow();
                QVERIFY(!rendered.isNull());
                QCOMPARE(rendered.size(), QSize(width, height));
                QVERIFY2(sampledColorCount(rendered) > 35,
                         "camera motion frame is blank or nearly monochrome");
                if (!prior.isNull()
                        && changedPixels(prior, rendered) > 40) {
                    ++visiblyChangedFrames;
                }
                prior = rendered;

                const QString output = frames.filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(output, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(rendered),
                         qPrintable(writer.errorString()));
            }

            QCOMPARE(frames.entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files).size(),
                     frameCount);
            QVERIFY2(visiblyChangedFrames > frameCount * 9 / 10,
                     qPrintable(QStringLiteral(
                         "%1 camera moved in only %2 of %3 transitions")
                         .arg(QString::fromLatin1(composition))
                         .arg(visiblyChangedFrames)
                         .arg(frameCount - 1)));
        }
    }

    void jumpLiftRemainsVisibleAgainstGroundCamera()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const double distance = tabletopApproachDistance(road) + 5.0;
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = 1.15;
        frame.world.rider.clearanceMeters = 0.82 + 1.15;
        frame.world.rider.airborne = true;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 260.0, 245.0, 95, 155, 10);

        QVERIFY(viewModel.riderY() - viewModel.groundY() > 1.0);
    }

    void physicsOwnsAirHeightWhenAWorldSnapshotIsAvailable()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGameVisualSnapshot frame = frameAt(road, 20.0);
        frame.world.rider.clearanceMeters = 0.82 + 0.38;
        frame.world.rider.airborne = true;
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = 1.35;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 260.0, 245.0, 95, 155, 10);

        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(road, 20.0);
        QVERIFY(sample.ready);
        const double visualGround = sample.center.elevationMeters;
        QVERIFY(std::abs(viewModel.riderY() - visualGround - 0.38) < 1e-9);
    }

    void completedDropKeepsTheNegativeRoadSurfaceOffset()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Drop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double beforeDistance =
                piece->challenge.obstacleDistanceMeters - 1.0;
        const double droppedDistance =
                piece->challenge.obstacleDistanceMeters + 4.0;
        const WorkoutGameRoadSample beforeSample =
                WorkoutGameRoadCourseBuilder::sample(road, beforeDistance);
        const WorkoutGameRoadSample droppedSample =
                WorkoutGameRoadCourseBuilder::sample(road, droppedDistance);
        QVERIFY(beforeSample.ready);
        QVERIFY(droppedSample.ready);
        QVERIFY(droppedSample.center.elevationMeters
                < beforeSample.center.elevationMeters - 0.5);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, droppedDistance);
        frame.world.rider.clearanceMeters = 0.82;
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Drop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Drop;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = -0.45;
        viewModel.setFrame(frame, 150.0, 150.0, 80, 145, 4);

        QVERIFY(std::abs(viewModel.riderY()
                - droppedSample.center.elevationMeters) < 1e-9);
    }
};

QTEST_MAIN(TestWorkoutGame3DView)
#include "testWorkoutGame3DView.moc"
