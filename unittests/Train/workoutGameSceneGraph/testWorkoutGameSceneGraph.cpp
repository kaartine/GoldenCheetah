/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameSceneGraphWindow.h"
#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameFeatureRuntime.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSet>
#include <QTest>

#include <algorithm>

namespace {

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 441u;
    course.durationMs = 90000;
    WorkoutGameSection trail;
    trail.feature = WorkoutGameFeature::Trail;
    trail.terrain = WorkoutGameTerrainKind::SmoothTrail;
    trail.durationMs = 30000;
    trail.targetWatts = 170.0;
    trail.difficulty = 0.3;
    WorkoutGameSection climb = trail;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.startMs = 30000;
    climb.targetWatts = 230.0;
    climb.gradePercent = 7.0;
    climb.difficulty = 0.7;
    climb.visualVariant = 2u;
    WorkoutGameSection log = trail;
    log.feature = WorkoutGameFeature::SprintJump;
    log.terrain = WorkoutGameTerrainKind::LogOver;
    log.startMs = 60000;
    log.targetWatts = 270.0;
    log.challengeCount = 1;
    log.difficulty = 0.6;
    log.visualVariant = 3u;
    course.sections = {trail, climb, log};
    return course;
}

WorkoutGameVisualSnapshot frameAt(double distanceMeters)
{
    WorkoutGameVisualSnapshot frame;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = std::int64_t(distanceMeters * 500.0);
    frame.simulation.courseProgress = distanceMeters / 500.0;
    frame.simulation.sectionProgress = std::clamp(
            (distanceMeters - 190.0) / 100.0, 0.0, 1.0);
    frame.simulation.speedKph = 18.4;
    frame.simulation.activeSection = 1;
    frame.world.ready = true;
    frame.world.generation = 1;
    frame.world.terrain = WorkoutGameTerrainKind::Climb;
    frame.world.rider.distanceMeters = distanceMeters;
    frame.world.rider.rearSuspension = 0.15;
    frame.world.rider.frontSuspension = 0.2;
    frame.world.speedMetersPerSecond = 5.1;
    return frame;
}

WorkoutGameVisualSnapshot featureFrame(
        WorkoutGameFeatureOutcome outcome,
        WorkoutGameRoute route)
{
    WorkoutGameVisualSnapshot frame;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = 69400;
    frame.simulation.courseProgress = 0.94;
    frame.simulation.sectionProgress = 0.94;
    frame.simulation.speedKph = 20.0;
    frame.simulation.activeSection = 2;
    frame.simulation.featureOutcome = outcome;
    frame.simulation.route = route;
    frame.simulation.challengeReadiness = outcome
            == WorkoutGameFeatureOutcome::Completed ? 1.0 : 0.4;
    frame.world.ready = true;
    frame.world.generation = 1;
    frame.world.terrain = WorkoutGameTerrainKind::LogOver;
    frame.world.rider.distanceMeters = 1.0;
    frame.world.speedMetersPerSecond = 5.5;
    return frame;
}

int changedPixels(const QImage &left, const QImage &right, int top)
{
    int changed = 0;
    for (int y = top; y < left.height(); y += 2) {
        for (int x = 0; x < left.width(); x += 2) {
            if (left.pixel(x, y) != right.pixel(x, y)) ++changed;
        }
    }
    return changed;
}

}

class TestWorkoutGameSceneGraph : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    void rendersAndMovesPseudoThreeDimensionalRoad()
    {
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(sampleCourse(), 200.0);
        window.setFrame(frameAt(200.0), 215.0, 230.0, 86, 151, 5);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
        QTest::qWait(1200);

        const QImage first = window.grabWindow();
        QVERIFY(!first.isNull());
        QCOMPARE(first.size(), QSize(1280, 720));
        QSet<QRgb> colors;
        for (int y = 0; y < first.height(); y += 12) {
            for (int x = 0; x < first.width(); x += 12) {
                colors.insert(first.pixel(x, y));
            }
        }
        QVERIFY(colors.size() > 120);
        QVERIFY(QColor(first.pixel(640, 690))
                != QColor(first.pixel(60, 690)));

        window.setFrame(frameAt(205.0), 245.0, 230.0, 91, 154, 6);
        QTest::qWait(300);
        const QImage second = window.grabWindow();
        QVERIFY(!second.isNull());
        QVERIFY(changedPixels(first, second, 250) > 1200);

        const QString screenshotPath = QDir(QDir::tempPath()).filePath(
                QStringLiteral("workout-game-scenegraph-test.png"));
        QVERIFY(second.save(screenshotPath));
    }

    void completedAndBypassedFeaturesRenderDifferentLines()
    {
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(sampleCourse(), 200.0);
        window.setFrame(
                featureFrame(WorkoutGameFeatureOutcome::Completed,
                             WorkoutGameRoute::MainLine),
                270.0, 270.0, 88, 155, 7);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);
        QTest::qWait(400);
        const QImage completed = window.grabWindow();
        QVERIFY(!completed.isNull());

        window.setFrame(
                featureFrame(WorkoutGameFeatureOutcome::Bypassed,
                             WorkoutGameRoute::SafeBypass),
                110.0, 270.0, 45, 130, 2);
        QTest::qWait(400);
        const QImage bypassed = window.grabWindow();
        QVERIFY(!bypassed.isNull());

        QVERIFY(changedPixels(completed, bypassed, 180) > 1800);
    }

    void featureLabRendersFiveDistinctObstacles()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSceneGraphWindow window;
        window.resize(1280, 720);
        window.setCourse(course, 200.0);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 3000);

        QImage prior;
        const int featureSections[] = {0, 2, 4, 6, 8};
        for (int section : featureSections) {
            WorkoutGameVisualSnapshot frame;
            frame.simulation.ready = true;
            frame.simulation.workoutTimeMs =
                    course.sections[std::size_t(section)].startMs + 9800;
            frame.simulation.activeSection = section;
            frame.simulation.sectionProgress = 0.82;
            frame.simulation.courseProgress = double(frame.simulation.workoutTimeMs)
                    / double(course.durationMs);
            frame.simulation.speedKph = 20.0;
            frame.simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
            frame.simulation.challengeReadiness = 1.0;
            frame.world.ready = true;
            frame.world.generation = std::uint64_t(section + 1);
            frame.world.terrain = course.sections[std::size_t(section)].terrain;
            frame.world.speedMetersPerSecond = 5.5;
            window.setFrame(frame, 220.0,
                    course.sections[std::size_t(section)].targetWatts,
                    88, 150, 7);
            QTest::qWait(300);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            if (!prior.isNull()) {
                QVERIFY(changedPixels(prior, rendered, 180) > 900);
            }
            const QString output = QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-feature-%1.png")
                        .arg(section));
            QVERIFY(rendered.save(output));
            prior = rendered;
        }
    }
};

QTEST_MAIN(TestWorkoutGameSceneGraph)
#include "testWorkoutGameSceneGraph.moc"
