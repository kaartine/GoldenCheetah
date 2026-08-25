/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"
#include "WorkoutGame3DWindow.h"
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
    course.sections = {section};
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
        const std::array<const char *, 10> requiredObjects = {{
            "ROOT_Tabletop",
            "GEO_Tabletop_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_LIP",
            "MARKER_APEX",
            "MARKER_LAND"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }

        QFile mesh(QStringLiteral(
                ":/qml/assets/meshes/geo_Tabletop_LOD0_mesh.mesh"));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.size(), qint64(7236));
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
        QTest::qWait(150);
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

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 260.0, 245.0, 95, 155, 10);

        QVERIFY(viewModel.riderY() - viewModel.groundY() > 1.0);
    }
};

QTEST_MAIN(TestWorkoutGame3DView)
#include "testWorkoutGame3DView.moc"
