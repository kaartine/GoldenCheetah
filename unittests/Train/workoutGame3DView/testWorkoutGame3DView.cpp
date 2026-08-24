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

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickWindow>
#include <QSet>
#include <QSGRendererInterface>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

constexpr double FtpWatts = 200.0;

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
