/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameTrailScene.h"

#include <QTest>

#include <algorithm>
#include <cmath>

namespace {

WorkoutGameWorldSnapshot worldAt(
        double distanceMeters,
        WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::Roots)
{
    WorkoutGameWorldSnapshot world;
    world.ready = true;
    world.terrain = terrain;
    world.seed = 0x1234u;
    world.gradePercent = 4.0;
    world.difficulty = 0.65;
    world.rider.distanceMeters = distanceMeters;
    return world;
}

const WorkoutGameTrailProp *propAtDistance(
        const WorkoutGameTrailSceneSnapshot &scene,
        double distanceMeters)
{
    const auto found = std::find_if(
            scene.props.begin(), scene.props.end(),
            [&](const WorkoutGameTrailProp &prop) {
                return std::abs(prop.worldDistanceMeters - distanceMeters) < 1e-9;
            });
    return found == scene.props.end() ? nullptr : &*found;
}

}

class TestWorkoutGameTrailScene : public QObject
{
    Q_OBJECT

private slots:
    void unavailableWorldProducesNoGeometry()
    {
        const WorkoutGameTrailSceneSnapshot scene =
                WorkoutGameTrailScene::build({});

        QVERIFY(!scene.ready);
        QVERIFY(scene.points.empty());
        QVERIFY(scene.props.empty());
    }

    void obliqueTrailHasTwoVisibleEdges()
    {
        const WorkoutGameTrailSceneSnapshot scene =
                WorkoutGameTrailScene::build(worldAt(100.0));

        QVERIFY(scene.ready);
        QVERIFY(scene.points.size() >= 48u);
        for (const WorkoutGameTrailPoint &point : scene.points) {
            QVERIFY(point.xNormalized >= 0.0);
            QVERIFY(point.xNormalized <= 1.0);
            QVERIFY(point.farEdgeYNormalized < point.centerYNormalized);
            QVERIFY(point.centerYNormalized < point.nearEdgeYNormalized);
            QVERIFY(point.nearEdgeYNormalized - point.farEdgeYNormalized > 0.1);
        }
    }

    void propsAreAnchoredToWorldDistance()
    {
        const WorkoutGameTrailSceneSnapshot first =
                WorkoutGameTrailScene::build(worldAt(100.0));
        const WorkoutGameTrailSceneSnapshot advanced =
                WorkoutGameTrailScene::build(worldAt(101.0));
        QVERIFY(!first.props.empty());

        const WorkoutGameTrailProp &before = first.props[first.props.size() / 2];
        const WorkoutGameTrailProp *after = propAtDistance(
                advanced, before.worldDistanceMeters);
        QVERIFY(after != nullptr);
        QCOMPARE(after->worldDistanceMeters, before.worldDistanceMeters);
        QVERIFY(after->xNormalized < before.xNormalized);
        QVERIFY(std::abs(
                (before.xNormalized - after->xNormalized)
                    - 1.0 / WorkoutGameTrailScene::VisibleMeters) < 1e-9);
    }

    void propsStayInsideTrailAndHaveStableDepthOrder()
    {
        const WorkoutGameTrailSceneSnapshot scene =
                WorkoutGameTrailScene::build(worldAt(100.0));

        QVERIFY(scene.props.size() >= 4u);
        double priorDepth = -1.0;
        for (const WorkoutGameTrailProp &prop : scene.props) {
            QVERIFY(prop.yNormalized >= prop.farEdgeYNormalized);
            QVERIFY(prop.yNormalized <= prop.nearEdgeYNormalized);
            QVERIFY(prop.depthKey >= priorDepth);
            priorDepth = prop.depthKey;
        }
    }

    void sameWorldProducesIdenticalScene()
    {
        const WorkoutGameTrailSceneSnapshot first =
                WorkoutGameTrailScene::build(worldAt(42.5));
        const WorkoutGameTrailSceneSnapshot second =
                WorkoutGameTrailScene::build(worldAt(42.5));

        QCOMPARE(first.points.size(), second.points.size());
        QCOMPARE(first.props.size(), second.props.size());
        for (std::size_t index = 0; index < first.props.size(); ++index) {
            QCOMPARE(first.props[index].kind, second.props[index].kind);
            QCOMPARE(first.props[index].worldDistanceMeters,
                     second.props[index].worldDistanceMeters);
            QCOMPARE(first.props[index].xNormalized,
                     second.props[index].xNormalized);
            QCOMPARE(first.props[index].yNormalized,
                     second.props[index].yNormalized);
        }
    }

    void terrainSelectsMatchingPropFamily()
    {
        const struct {
            WorkoutGameTerrainKind terrain;
            WorkoutGameTrailPropKind prop;
        } cases[] = {
            {WorkoutGameTerrainKind::Roots, WorkoutGameTrailPropKind::Root},
            {WorkoutGameTerrainKind::RockGarden, WorkoutGameTrailPropKind::Rock},
            {WorkoutGameTerrainKind::BunnyHop, WorkoutGameTrailPropKind::Log},
            {WorkoutGameTerrainKind::Skinny, WorkoutGameTrailPropKind::Plank},
            {WorkoutGameTerrainKind::Berm, WorkoutGameTrailPropKind::BermMarker}
        };

        for (const auto &entry : cases) {
            const WorkoutGameTrailSceneSnapshot scene =
                    WorkoutGameTrailScene::build(worldAt(50.0, entry.terrain));
            QVERIFY(!scene.props.empty());
            for (const WorkoutGameTrailProp &prop : scene.props) {
                QCOMPARE(prop.kind, entry.prop);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameTrailScene)
#include "testWorkoutGameTrailScene.moc"
