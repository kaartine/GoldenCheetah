/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <box2d/box2d.h>

#include <QTest>

#include <cmath>

class TestBox2DVendor : public QObject
{
    Q_OBJECT

private slots:
    void pinnedVersionIsLinked()
    {
        const b2Version version = b2GetVersion();
        QCOMPARE(version.major, 3);
        QCOMPARE(version.minor, 1);
        QCOMPARE(version.revision, 1);
    }

    void fixedStepWorldIsDeterministicAndReleasesMemory()
    {
        const auto run = []() {
            b2WorldDef worldDefinition = b2DefaultWorldDef();
            worldDefinition.gravity = b2Vec2{0.0f, -9.81f};
            const b2WorldId world = b2CreateWorld(&worldDefinition);

            b2BodyDef groundDefinition = b2DefaultBodyDef();
            const b2BodyId ground = b2CreateBody(world, &groundDefinition);
            b2ShapeDef groundShape = b2DefaultShapeDef();
            const b2Segment floor = {{-10.0f, 0.0f}, {10.0f, 0.0f}};
            b2CreateSegmentShape(ground, &groundShape, &floor);

            b2BodyDef bodyDefinition = b2DefaultBodyDef();
            bodyDefinition.type = b2_dynamicBody;
            bodyDefinition.position = b2Vec2{0.0f, 4.0f};
            const b2BodyId body = b2CreateBody(world, &bodyDefinition);
            b2ShapeDef bodyShape = b2DefaultShapeDef();
            bodyShape.density = 1.0f;
            const b2Circle circle = {{0.0f, 0.0f}, 0.5f};
            b2CreateCircleShape(body, &bodyShape, &circle);

            for (int step = 0; step < 240; ++step) {
                b2World_Step(world, 1.0f / 120.0f, 4);
            }
            const b2Vec2 position = b2Body_GetPosition(body);
            b2DestroyWorld(world);
            return position;
        };

        const int baselineBytes = b2GetByteCount();
        const b2Vec2 first = run();
        const b2Vec2 second = run();

        QCOMPARE(first.x, second.x);
        QCOMPARE(first.y, second.y);
        QVERIFY(std::isfinite(first.x));
        QVERIFY(std::isfinite(first.y));
        QVERIFY(first.y > 0.45f && first.y < 0.6f);
        QCOMPARE(b2GetByteCount(), baselineBytes);
    }
};

QTEST_GUILESS_MAIN(TestBox2DVendor)
#include "testBox2DVendor.moc"
