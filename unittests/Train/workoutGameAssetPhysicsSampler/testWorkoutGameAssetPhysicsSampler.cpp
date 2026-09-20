/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAssetPhysicsSampler.h"

#include <QTest>

#include <cmath>

namespace {

WorkoutGameCourseAssetPhysicsSnapshot triangleSnapshot()
{
    WorkoutGameCourseAssetPhysicsSnapshot snapshot;
    snapshot.catalogSchemaVersion = 1;
    WorkoutGameAssetPhysicsDefinition definition;
    definition.coulombFrictionMilli = 1100;
    definition.restitutionMilli = 25;
    definition.chains = {{
        {{-270, 0}, {0, 540}, {270, 0}}
    }};
    snapshot.physicsDefinitions.push_back(definition);

    WorkoutGameAssetPhysicsBinding asset;
    asset.assetId = QStringLiteral("FT-02-log-over-greybox");
    asset.definitionIndex = 0;
    asset.nativeForwardExtentMm = 540;
    asset.nativeUpExtentMm = 540;
    asset.resolvedExtentMm = 540;
    snapshot.bindings.push_back(asset);

    WorkoutGameAssetPhysicsPieceBinding piece;
    piece.definitionIndex = 0;
    piece.bindingIndex = 0;
    piece.obstacleAnchorMm = 10000;
    snapshot.pieceBindings.push_back(piece);
    return snapshot;
}

}

class TestWorkoutGameAssetPhysicsSampler : public QObject
{
    Q_OBJECT

private slots:
    void interpolatesProfileAndExposesMaterial()
    {
        const auto snapshot = triangleSnapshot();
        const auto approach = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 9.865);
        QVERIFY(approach.bound);
        QVERIFY(approach.surfacePresent);
        QVERIFY(std::abs(approach.offsetMeters - 0.27) < 1e-12);
        QCOMPARE(approach.coulombFriction, 1.1);
        QCOMPARE(approach.restitution, 0.025);

        const auto crest = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 10.0);
        QVERIFY(crest.surfacePresent);
        QCOMPARE(crest.offsetMeters, 0.54);

        const auto outside = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 9.0);
        QVERIFY(outside.bound);
        QVERIFY(!outside.surfacePresent);
        QCOMPARE(outside.offsetMeters, 0.0);
    }

    void returnsExactSortedBreakpoints()
    {
        const auto snapshot = triangleSnapshot();
        const auto points = WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                snapshot, 0);
        QCOMPARE(points.size(), std::size_t(3));
        QCOMPARE(points[0], 9.73);
        QCOMPARE(points[1], 10.0);
        QCOMPARE(points[2], 10.27);
    }

    void resolvesOneCompleteRenderTransform()
    {
        auto snapshot = triangleSnapshot();
        snapshot.bindings[0].nativeForwardOriginMm = -1020;
        snapshot.bindings[0].nativeUpExtentMm = 350;
        snapshot.bindings[0].resolvedExtentMm = 700;

        const WorkoutGameAssetRenderTransform transform =
                WorkoutGameAssetPhysicsSampler::renderTransform(snapshot, 0);
        QCOMPARE(transform.status, WorkoutGameAssetRenderFitStatus::Ready);
        QCOMPARE(transform.assetId,
                 QStringLiteral("FT-02-log-over-greybox"));
        QCOMPARE(transform.obstacleAnchorMeters, 10.0);
        QCOMPARE(transform.forwardScale, 700.0 / 540.0);
        QCOMPARE(transform.upScale, 2.0);
        QCOMPARE(transform.forwardExtentMeters, 0.7);
        QCOMPARE(transform.upExtentMeters, 0.7);
        QCOMPARE(transform.assetStartDistanceMeters,
                 10.0 - 1.02 * transform.forwardScale);

        snapshot.bindings[0].nativeUpExtentMm = 0;
        QCOMPARE(WorkoutGameAssetPhysicsSampler::renderTransform(snapshot, 0)
                    .status,
                 WorkoutGameAssetRenderFitStatus::Invalid);
    }

    void handlesUnboundLegacyAndInvalidIndicesWithoutDereferencing()
    {
        auto snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].definitionIndex =
                WorkoutGameCourseAssetPhysicsSnapshot::NoIndex;
        auto sample = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 10.0);
        QVERIFY(!sample.bound);
        QVERIFY(WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                    snapshot, 0).empty());

        snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].flags =
                WorkoutGameCourseAssetPhysicsSnapshot::LegacyProceduralV1;
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 0, 10.0);
        QVERIFY(!sample.bound);

        snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].definitionIndex = 99;
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 0, 10.0);
        QVERIFY(!sample.bound);
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 99, 10.0);
        QVERIFY(!sample.bound);
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameAssetPhysicsSampler)

#include "testWorkoutGameAssetPhysicsSampler.moc"
