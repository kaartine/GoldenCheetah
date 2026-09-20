/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAssetCatalog.h"
#include "Train/WorkoutGameAssetPhysicsResolver.h"
#include "Train/WorkoutGameAssetPhysicsSampler.h"
#include "Train/WorkoutGameRoadPlan.h"

#include <QTest>

#include <algorithm>
#include <cmath>

namespace {

WorkoutGameCourse logOverCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x8f12u;
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::LogOver;
    section.durationMs = course.durationMs;
    section.targetWatts = 260.0;
    section.difficulty = 0.5;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

std::size_t challengeIndex(const WorkoutGameRoadPlan &plan)
{
    const auto found = std::find_if(
            plan.pieces.begin(), plan.pieces.end(),
            [](const WorkoutGameRoadPiece &piece) {
                return piece.terrain == WorkoutGameTerrainKind::LogOver
                        && piece.challenge.enabled;
            });
    return found == plan.pieces.end()
            ? plan.pieces.size()
            : std::size_t(std::distance(plan.pieces.begin(), found));
}

}

class TestWorkoutGameAssetPhysicsRoadCourse : public QObject
{
    Q_OBJECT

private slots:
    void resolvedProfileReplacesLegacyExactlyOnce()
    {
        const WorkoutGameCourse course = logOverCourse();
        WorkoutGameRoadPlan legacyPlan =
                WorkoutGameRoadCourseBuilder::generatePlan(course, 200.0);
        const std::size_t pieceIndex = challengeIndex(legacyPlan);
        QVERIFY(pieceIndex < legacyPlan.pieces.size());
        legacyPlan.assetPhysicsSnapshot =
                WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(legacyPlan);
        QVERIFY(legacyPlan.assetPhysicsSnapshot);

        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));
        const auto resolution = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, legacyPlan.pieces);
        QCOMPARE(resolution.status,
                 WorkoutGameAssetPhysicsResolveStatus::Ready);
        QVERIFY(resolution.snapshot);

        WorkoutGameRoadPlan resolvedPlan = legacyPlan;
        auto raised = std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>(
                *resolution.snapshot);
        QVERIFY(!raised->physicsDefinitions.empty());
        auto &points = raised->physicsDefinitions[0].chains[0].points;
        const auto crest = std::max_element(
                points.begin(), points.end(),
                [](const auto &left, const auto &right) {
                    return left.heightMm < right.heightMm;
                });
        QVERIFY(crest != points.end());
        crest->heightMm += 100;
        resolvedPlan.assetPhysicsSnapshot = raised;

        const WorkoutGameRoadCourse legacy =
                WorkoutGameRoadCourseBuilder::materialize(course, legacyPlan);
        const WorkoutGameRoadCourse resolved =
                WorkoutGameRoadCourseBuilder::materialize(course, resolvedPlan);
        QVERIFY(legacy.ready);
        QVERIFY(resolved.ready);
        QCOMPARE(resolved.assetPhysicsSnapshot,
                 resolvedPlan.assetPhysicsSnapshot);

        const double obstacle = double(
                raised->pieceBindings[pieceIndex].obstacleAnchorMm) / 1000.0;
        const auto legacySample = WorkoutGameRoadCourseBuilder::sample(
                legacy, obstacle);
        const auto resolvedSample = WorkoutGameRoadCourseBuilder::sample(
                resolved, obstacle);
        QVERIFY(legacySample.ready);
        QVERIFY(resolvedSample.ready);
        const double raisedDifference = resolvedSample.surfaceOffsetMeters
                - legacySample.surfaceOffsetMeters;
        QVERIFY2(std::abs(resolvedSample.surfaceOffsetMeters - 0.64) < 1e-12
                    && std::abs(raisedDifference - 0.1) <= 0.001,
                 qPrintable(QStringLiteral("legacy=%1 resolved=%2 delta=%3")
                     .arg(legacySample.surfaceOffsetMeters, 0, 'f', 9)
                     .arg(resolvedSample.surfaceOffsetMeters, 0, 'f', 9)
                     .arg(raisedDifference, 0, 'f', 9)));
    }

    void catalogProfileMatchesLegacyAtFacetEdges()
    {
        const WorkoutGameCourse course = logOverCourse();
        WorkoutGameRoadPlan plan =
                WorkoutGameRoadCourseBuilder::generatePlan(course, 200.0);
        const std::size_t pieceIndex = challengeIndex(plan);
        QVERIFY(pieceIndex < plan.pieces.size());

        WorkoutGameRoadPlan legacyPlan = plan;
        legacyPlan.assetPhysicsSnapshot =
                WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(legacyPlan);
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));
        const auto resolution = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, plan.pieces);
        QCOMPARE(resolution.status,
                 WorkoutGameAssetPhysicsResolveStatus::Ready);
        plan.assetPhysicsSnapshot = resolution.snapshot;

        const auto legacy = WorkoutGameRoadCourseBuilder::materialize(
                course, legacyPlan);
        const auto resolved = WorkoutGameRoadCourseBuilder::materialize(
                course, plan);
        QVERIFY(legacy.ready);
        QVERIFY(resolved.ready);
        const auto breakpoints =
                WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                    *resolution.snapshot, pieceIndex);
        QCOMPARE(breakpoints.size(), std::size_t(9));
        for (double breakpoint : breakpoints) {
            for (double delta : {-0.001, 0.0, 0.001}) {
                const auto oldSample = WorkoutGameRoadCourseBuilder::sample(
                        legacy, breakpoint + delta);
                const auto newSample = WorkoutGameRoadCourseBuilder::sample(
                        resolved, breakpoint + delta);
                QVERIFY(oldSample.ready);
                QVERIFY(newSample.ready);
                const double difference = std::abs(
                        oldSample.surfaceOffsetMeters
                            - newSample.surfaceOffsetMeters);
                const double tolerance = delta == 0.0 ? 0.002 : 0.005;
                QVERIFY2(difference <= tolerance,
                         qPrintable(QStringLiteral(
                             "distance=%1 legacy=%2 resolved=%3 delta=%4")
                             .arg(breakpoint + delta, 0, 'f', 6)
                             .arg(oldSample.surfaceOffsetMeters, 0, 'f', 9)
                             .arg(newSample.surfaceOffsetMeters, 0, 'f', 9)
                             .arg(difference, 0, 'f', 9)));
            }
        }
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameAssetPhysicsRoadCourse)

#include "testWorkoutGameAssetPhysicsRoadCourse.moc"
