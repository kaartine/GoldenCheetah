/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameWorld.h"
#include "WorkoutGameFeatureCatalog.h"
#include "WorkoutGameRoadCourse.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double TerrainPeriodMeters = 80.0;
constexpr double TerrainStartMeters = -12.0;
constexpr double TerrainEndMeters = 220.0;
constexpr double TerrainSampleMeters = 0.25;
constexpr double RiderStartMeters = 4.0;
constexpr double RebaseAtMeters = 180.0;
constexpr double WheelRadiusMeters = 0.36;
constexpr std::int64_t PhysicsStepMicroseconds = 8333;
constexpr std::int64_t MaximumCatchupMicroseconds = 1000000;
constexpr std::int64_t WalkDecisionMicroseconds = 1500000;
constexpr float TechnicalFeatureLaunchSpeedMetersPerSecond = 4.8f;
constexpr float TabletopLaunchSpeedMetersPerSecond = 6.6f;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double approach(double current, double target, double blend)
{
    return current + (target - current) * blend;
}

double targetYaw(WorkoutGameCameraMode mode)
{
    switch (mode) {
    case WorkoutGameCameraMode::Side: return 90.0;
    case WorkoutGameCameraMode::ThreeQuarter: return 42.0;
    case WorkoutGameCameraMode::Chase: return 8.0;
    }
    return 90.0;
}

double targetZoom(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::BunnyHop:
    case WorkoutGameTerrainKind::LogOver:
    case WorkoutGameTerrainKind::Tabletop:
    case WorkoutGameTerrainKind::Drop:
        return 0.9;
    case WorkoutGameTerrainKind::Skinny:
    case WorkoutGameTerrainKind::Berm:
        return 1.08;
    default:
        return 1.0;
    }
}

double positivePhase(double distance, std::uint32_t seed)
{
    const double seedOffset = double(seed % 997u) * TerrainPeriodMeters / 997.0;
    double phase = std::fmod(distance + seedOffset, TerrainPeriodMeters);
    if (phase < 0.0) phase += TerrainPeriodMeters;
    return phase;
}

double smoothStep(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

double radiansToDegrees(double value)
{
    return value * 180.0 / Pi;
}

bool retainsOrdinaryGroundContact(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::SmoothTrail:
    case WorkoutGameTerrainKind::Roots:
    case WorkoutGameTerrainKind::RockGarden:
    case WorkoutGameTerrainKind::Climb:
    case WorkoutGameTerrainKind::Skinny:
    case WorkoutGameTerrainKind::Berm:
    case WorkoutGameTerrainKind::RockSlab:
        return true;
    default:
        return false;
    }
}

double physicalSurfaceElevation(const WorkoutGameRoadSample &sample)
{
    return sample.surfaceElevationMeters()
            - sample.nonPhysicalFeatureOffsetMeters;
}

}

struct WorkoutGamePhysics::Impl
{
    std::uint32_t seed = 0;
    bool configured = false;
    bool initialized = false;
    std::int64_t lastWorkoutTimeMs = 0;
    std::int64_t remainderMicroseconds = 0;
    std::int64_t weakClimbMicroseconds = 0;
    std::uint64_t generation = 0;
    WorkoutGameTerrainKind terrain = WorkoutGameTerrainKind::SmoothTrail;
    double gradePercent = 0.0;
    double difficulty = 0.0;
    double distanceBase = 0.0;
    double authoritativeDistanceMeters = -1.0;
    double publishedDistanceMeters = 0.0;
    double elevationBase = 0.0;
    double originBodyY = 0.0;
    double landingImpact = 0.0;
    bool wasGrounded = true;
    int lastJumpTile = -1;
    std::uint64_t lastFeatureActionId = 0;
    WorkoutGameWorldSnapshot latest;
    WorkoutGameRoadCourse roadCourse;

    b2WorldId world = b2_nullWorldId;
    b2BodyId chassis = b2_nullBodyId;
    b2BodyId rearWheel = b2_nullBodyId;
    b2BodyId frontWheel = b2_nullBodyId;
    b2ShapeId rearWheelShape = b2_nullShapeId;
    b2ShapeId frontWheelShape = b2_nullShapeId;
    b2JointId rearJoint = b2_nullJointId;
    b2JointId frontJoint = b2_nullJointId;

    ~Impl()
    {
        destroyWorld();
    }

    void destroyWorld()
    {
        if (B2_IS_NON_NULL(world)) b2DestroyWorld(world);
        world = b2_nullWorldId;
        chassis = b2_nullBodyId;
        rearWheel = b2_nullBodyId;
        frontWheel = b2_nullBodyId;
        rearWheelShape = b2_nullShapeId;
        frontWheelShape = b2_nullShapeId;
        rearJoint = b2_nullJointId;
        frontJoint = b2_nullJointId;
    }

    b2BodyId createWheel(b2Vec2 position, b2ShapeId &shape)
    {
        b2BodyDef bodyDefinition = b2DefaultBodyDef();
        bodyDefinition.type = b2_dynamicBody;
        bodyDefinition.position = position;
        bodyDefinition.allowFastRotation = true;
        const b2BodyId body = b2CreateBody(world, &bodyDefinition);

        b2ShapeDef shapeDefinition = b2DefaultShapeDef();
        shapeDefinition.density = 8.0f;
        shapeDefinition.material.friction = 1.4f;
        shapeDefinition.material.rollingResistance = 0.025f;
        const b2Circle circle = {{0.0f, 0.0f}, float(WheelRadiusMeters)};
        shape = b2CreateCircleShape(body, &shapeDefinition, &circle);
        return body;
    }

    b2JointId attachWheel(b2BodyId wheel, bool driven)
    {
        const b2Vec2 pivot = b2Body_GetPosition(wheel);
        b2WheelJointDef definition = b2DefaultWheelJointDef();
        definition.bodyIdA = chassis;
        definition.bodyIdB = wheel;
        definition.localAxisA = b2Body_GetLocalVector(
                chassis, b2Vec2{0.0f, 1.0f});
        definition.localAnchorA = b2Body_GetLocalPoint(chassis, pivot);
        definition.localAnchorB = b2Body_GetLocalPoint(wheel, pivot);
        definition.enableMotor = driven;
        definition.maxMotorTorque = driven ? 120.0f : 0.0f;
        definition.hertz = 6.0f;
        definition.dampingRatio = 0.8f;
        definition.lowerTranslation = -0.2f;
        definition.upperTranslation = 0.2f;
        definition.enableLimit = true;
        return b2CreateWheelJoint(world, &definition);
    }

    double surfaceHeight(double localX) const
    {
        if (roadCourse.ready) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        roadCourse,
                        distanceBase + localX - RiderStartMeters);
            const WorkoutGameRoadSample origin =
                    WorkoutGameRoadCourseBuilder::sample(
                        roadCourse, distanceBase);
            if (sample.ready && origin.ready) {
                return physicalSurfaceElevation(sample)
                        - physicalSurfaceElevation(origin);
            }
        }
        return WorkoutGamePhysics::terrainHeight(
                terrain, localX, gradePercent, difficulty, seed);
    }

    void createWorld()
    {
        destroyWorld();

        b2WorldDef worldDefinition = b2DefaultWorldDef();
        worldDefinition.gravity = b2Vec2{0.0f, -9.81f};
        worldDefinition.enableSleep = false;
        world = b2CreateWorld(&worldDefinition);

        b2BodyDef groundDefinition = b2DefaultBodyDef();
        const b2BodyId ground = b2CreateBody(world, &groundDefinition);
        b2ShapeDef terrainShape = b2DefaultShapeDef();
        terrainShape.material.friction = 1.1f;
        double priorX = TerrainStartMeters;
        double priorY = surfaceHeight(priorX);
        for (double x = TerrainStartMeters + TerrainSampleMeters;
                x <= TerrainEndMeters + 0.001;
                x += TerrainSampleMeters) {
            const double y = surfaceHeight(x);
            const b2Segment segment = {
                {float(priorX), float(priorY)},
                {float(x), float(y)}
            };
            b2CreateSegmentShape(ground, &terrainShape, &segment);
            priorX = x;
            priorY = y;
        }

        const double groundY = surfaceHeight(RiderStartMeters);
        b2BodyDef chassisDefinition = b2DefaultBodyDef();
        chassisDefinition.type = b2_dynamicBody;
        chassisDefinition.position = {
            float(RiderStartMeters), float(groundY + 0.92)
        };
        chassis = b2CreateBody(world, &chassisDefinition);
        b2ShapeDef chassisShape = b2DefaultShapeDef();
        chassisShape.density = 22.0f;
        chassisShape.material.friction = 0.15f;
        const b2Polygon frame = b2MakeRoundedBox(0.72f, 0.18f, 0.1f);
        b2CreatePolygonShape(chassis, &chassisShape, &frame);

        rearWheel = createWheel(
                {float(RiderStartMeters - 0.62), float(groundY + 0.43)},
                rearWheelShape);
        frontWheel = createWheel(
                {float(RiderStartMeters + 0.62), float(groundY + 0.43)},
                frontWheelShape);
        rearJoint = attachWheel(rearWheel, true);
        frontJoint = attachWheel(frontWheel, false);
        originBodyY = b2Body_GetPosition(chassis).y;

        for (int settle = 0; settle < 60; ++settle) {
            b2World_Step(world, 1.0f / 120.0f, 4);
        }
        wasGrounded = grounded();
    }

    bool shapeGrounded(b2ShapeId shape) const
    {
        const int capacity = b2Shape_GetContactCapacity(shape);
        if (capacity <= 0) return false;
        std::vector<b2ContactData> contacts;
        contacts.resize(std::size_t(capacity));
        const int count = b2Shape_GetContactData(
                shape, contacts.data(), capacity);
        for (int index = 0; index < count; ++index) {
            if (contacts[std::size_t(index)].manifold.pointCount > 0) return true;
        }
        return false;
    }

    bool grounded() const
    {
        return shapeGrounded(rearWheelShape)
                || shapeGrounded(frontWheelShape);
    }

    double suspensionCompression(b2BodyId wheel) const
    {
        const b2Vec2 wheelInChassis = b2Body_GetLocalPoint(
                chassis, b2Body_GetPosition(wheel));
        return std::clamp((double(wheelInChassis.y) + 0.69) / 0.4, 0.0, 1.0);
    }

    void setDriveSpeed(double metersPerSecond)
    {
        const float wheelSpeed = float(-metersPerSecond / WheelRadiusMeters);
        b2WheelJoint_SetMotorSpeed(rearJoint, wheelSpeed);

        const b2Vec2 velocity = b2Body_GetLinearVelocity(chassis);
        const float mass = b2Body_GetMass(chassis);
        const float force = std::clamp(
                mass * float(metersPerSecond - velocity.x) * 3.0f,
                -450.0f, 450.0f);
        b2Body_ApplyForceToCenter(chassis, {force, 0.0f}, true);
    }

    void synchronizeDistance(double distanceMeters, bool forceGroundFollowing)
    {
        if (!std::isfinite(distanceMeters) || distanceMeters < 0.0
                || B2_IS_NULL(world)) {
            return;
        }
        authoritativeDistanceMeters = distanceMeters;
        const double targetLocalX = RiderStartMeters
                + authoritativeDistanceMeters - distanceBase;
        if (targetLocalX < 0.0 || targetLocalX > RebaseAtMeters) {
            const WorkoutGameWorldSnapshot before = captureSnapshot();
            distanceBase = authoritativeDistanceMeters;
            elevationBase = before.ready
                    ? before.rider.elevationMeters : elevationBase;
            createWorld();
            return;
        }

        const b2Vec2 chassisPosition = b2Body_GetPosition(chassis);
        const double delta = targetLocalX - chassisPosition.x;
        if (std::abs(delta) <= 1e-6) return;
        double verticalDelta = 0.0;
        // Distance playback translates the vehicle horizontally. Follow the
        // same ground delta so synchronization cannot manufacture air time.
        if (forceGroundFollowing || retainsOrdinaryGroundContact(terrain)) {
            verticalDelta = surfaceHeight(targetLocalX)
                    - surfaceHeight(chassisPosition.x);
        }
        for (b2BodyId body : {chassis, rearWheel, frontWheel}) {
            b2Body_SetTransform(
                    body,
                    b2Body_GetPosition(body) + b2Vec2{
                        float(delta), float(verticalDelta)},
                    b2Body_GetRotation(body));
        }
    }

    WorkoutGameWorldSnapshot snapshot() const
    {
        WorkoutGameWorldSnapshot result;
        if (B2_IS_NULL(world)) return result;

        const b2Vec2 position = b2Body_GetPosition(chassis);
        const b2Vec2 velocity = b2Body_GetLinearVelocity(chassis);
        result.ready = true;
        result.generation = generation;
        result.terrain = terrain;
        result.seed = seed;
        result.gradePercent = gradePercent;
        result.difficulty = difficulty;
        result.rider.distanceMeters = authoritativeDistanceMeters >= 0.0
                ? authoritativeDistanceMeters
                : distanceBase + double(position.x) - RiderStartMeters;
        result.terrainOffsetMeters = double(position.x)
                - result.rider.distanceMeters;
        const WorkoutGameRoadSample groundOrigin = roadCourse.ready
                ? WorkoutGameRoadCourseBuilder::sample(
                    roadCourse, distanceBase)
                : WorkoutGameRoadSample();
        const double originSurfaceElevation = groundOrigin.ready
                ? physicalSurfaceElevation(groundOrigin)
                : elevationBase;
        result.rider.elevationMeters = originSurfaceElevation
                + double(position.y) - originBodyY;
        result.rider.pitchDegrees = radiansToDegrees(
                b2Rot_GetAngle(b2Body_GetRotation(chassis)));
        result.rider.rearSuspension = suspensionCompression(rearWheel);
        result.rider.frontSuspension = suspensionCompression(frontWheel);
        result.rider.rearWheelRadians = b2Rot_GetAngle(
                b2Body_GetRotation(rearWheel));
        result.rider.frontWheelRadians = b2Rot_GetAngle(
                b2Body_GetRotation(frontWheel));
        const WorkoutGameRoadSample ground = roadCourse.ready
                ? WorkoutGameRoadCourseBuilder::sample(
                    roadCourse, distanceBase + double(position.x)
                        - RiderStartMeters)
                : WorkoutGameRoadSample();
        const double groundY = ground.ready && groundOrigin.ready
                ? physicalSurfaceElevation(ground)
                    - physicalSurfaceElevation(groundOrigin)
                : WorkoutGamePhysics::terrainHeight(
                    terrain, position.x, gradePercent, difficulty, seed);
        result.surfaceElevationMeters = ground.ready
                ? physicalSurfaceElevation(ground)
                : originSurfaceElevation + groundY;
        result.rider.clearanceMeters = double(position.y) - groundY;
        result.rider.airborne = !grounded();
        result.rider.walking = weakClimbMicroseconds
                >= WalkDecisionMicroseconds;
        result.speedMetersPerSecond = std::max(0.0, double(velocity.x));
        result.landingImpact = landingImpact;
        return result;
    }

    WorkoutGameWorldSnapshot captureSnapshot()
    {
        WorkoutGameWorldSnapshot result = snapshot();
        if (!result.ready) return result;

        // Box2D may push the chassis a few centimetres backwards at an
        // obstacle contact. Keep that local response for suspension and pose,
        // but never publish backwards course progress within one generation.
        result.rider.distanceMeters = std::max(
                publishedDistanceMeters, result.rider.distanceMeters);
        publishedDistanceMeters = result.rider.distanceMeters;
        result.terrainOffsetMeters = double(b2Body_GetPosition(chassis).x)
                - result.rider.distanceMeters;
        return result;
    }

    void rebaseIfNeeded()
    {
        if (b2Body_GetPosition(chassis).x < RebaseAtMeters) return;
        const WorkoutGameWorldSnapshot before = captureSnapshot();
        distanceBase = before.rider.distanceMeters;
        elevationBase = before.rider.elevationMeters;
        createWorld();
        latest = captureSnapshot();
    }

    void step(const WorkoutGamePhysicsInput &input)
    {
        const bool walking = weakClimbMicroseconds >= WalkDecisionMicroseconds;
        const double requestedSpeed = walking
                ? 1.3
                : std::clamp(
                    finiteOr(input.desiredSpeedMetersPerSecond, 0.0),
                    0.0, 16.0);
        setDriveSpeed(requestedSpeed);

        if (input.jumpRequested
                && WorkoutGameFeatureCatalog::definition(terrain).jumpable
                && grounded()) {
            if (input.featureActionId != 0
                    && input.featureActionId != lastFeatureActionId) {
                const float launchSpeed = terrain
                        == WorkoutGameTerrainKind::Tabletop
                        ? TabletopLaunchSpeedMetersPerSecond
                        : TechnicalFeatureLaunchSpeedMetersPerSecond;
                const float impulse = b2Body_GetMass(chassis) * launchSpeed;
                b2Body_ApplyLinearImpulseToCenter(
                        chassis, {0.0f, impulse}, true);
                lastFeatureActionId = input.featureActionId;
            } else if (input.featureActionId == 0) {
                const double distance = distanceBase
                        + double(b2Body_GetPosition(chassis).x)
                            - RiderStartMeters;
                const int tile = int(std::floor(
                        distance / TerrainPeriodMeters));
                const double phase = positivePhase(
                        b2Body_GetPosition(chassis).x, seed);
                if (phase >= 23.0 && phase <= 29.0
                        && tile != lastJumpTile) {
                    const float impulse = b2Body_GetMass(chassis) * 3.8f;
                    b2Body_ApplyLinearImpulseToCenter(
                            chassis, {0.0f, impulse}, true);
                    lastJumpTile = tile;
                }
            }
        }

        const b2Vec2 priorVelocity = b2Body_GetLinearVelocity(chassis);
        b2World_Step(world, float(PhysicsStepMicroseconds) / 1000000.0f, 4);
        const bool isGrounded = grounded();
        if (!wasGrounded && isGrounded && priorVelocity.y < -0.5f) {
            landingImpact = std::clamp(
                    double(-priorVelocity.y) / 8.0, 0.0, 1.0);
        } else {
            landingImpact *= 0.94;
        }
        wasGrounded = isGrounded;
    }
};

WorkoutGamePhysics::WorkoutGamePhysics() : impl(new Impl)
{
}

WorkoutGamePhysics::~WorkoutGamePhysics() = default;

bool WorkoutGamePhysics::configure(std::uint32_t seed)
{
    impl->roadCourse = WorkoutGameRoadCourse();
    impl->seed = seed == 0 ? 2166136261u : seed;
    impl->configured = true;
    impl->generation = 0;
    reset();
    return true;
}

bool WorkoutGamePhysics::configure(const WorkoutGameRoadCourse &course)
{
    if (!course.ready || course.pieces.empty()) return false;
    impl->roadCourse = course;
    impl->seed = course.seed == 0 ? 2166136261u : course.seed;
    impl->configured = true;
    impl->generation = 0;
    reset();
    return true;
}

void WorkoutGamePhysics::reset()
{
    impl->destroyWorld();
    impl->initialized = false;
    impl->lastWorkoutTimeMs = 0;
    impl->remainderMicroseconds = 0;
    impl->weakClimbMicroseconds = 0;
    impl->distanceBase = 0.0;
    impl->authoritativeDistanceMeters = -1.0;
    impl->publishedDistanceMeters = 0.0;
    impl->elevationBase = 0.0;
    impl->landingImpact = 0.0;
    impl->lastJumpTile = -1;
    impl->lastFeatureActionId = 0;
    impl->latest = WorkoutGameWorldSnapshot();
    ++impl->generation;
}

double WorkoutGamePhysics::terrainHeight(
        WorkoutGameTerrainKind terrain,
        double distanceMeters,
        double gradePercent,
        double difficulty,
        std::uint32_t seed)
{
    const double distance = finiteOr(distanceMeters, 0.0);
    const double grade = std::clamp(finiteOr(gradePercent, 0.0), -20.0, 20.0);
    const double challenge = std::clamp(finiteOr(difficulty, 0.0), 0.0, 1.0);
    const double phase = positivePhase(distance, seed);
    const double slope = distance * grade / 100.0;

    switch (terrain) {
    case WorkoutGameTerrainKind::Roots: {
        const double root = std::pow(
                std::max(0.0, std::sin(2.0 * Pi * phase / 1.6)), 6.0);
        const double crossRoot = std::pow(
                std::max(0.0, std::sin(2.0 * Pi * phase / 2.5 + 0.8)), 8.0);
        return slope + (0.06 + 0.09 * challenge) * root
                + (0.02 + 0.05 * challenge) * crossRoot;
    }
    case WorkoutGameTerrainKind::Rollers:
        return slope + (0.25 + 0.4 * challenge)
                * (1.0 - std::cos(2.0 * Pi * phase / 8.0)) * 0.5;
    case WorkoutGameTerrainKind::RockGarden: {
        const double rock = std::pow(
                std::max(0.0, std::sin(2.0 * Pi * phase / 3.2)), 4.0);
        const double offsetRock = std::pow(
                std::max(0.0, std::sin(2.0 * Pi * phase / 5.0 + 1.1)), 6.0);
        return slope + (0.14 + 0.3 * challenge) * rock
                + (0.08 + 0.2 * challenge) * offsetRock;
    }
    case WorkoutGameTerrainKind::BunnyHop: {
        const double approach = std::abs(phase - 30.0);
        const double obstacle = approach < 0.7
                ? 0.3 * (1.0 - smoothStep(approach / 0.7))
                : 0.0;
        return slope + obstacle;
    }
    case WorkoutGameTerrainKind::LogOver: {
        const double approach = std::abs(phase - 30.0);
        const double obstacle = approach < 0.85
                ? (0.22 + 0.12 * challenge)
                    * (1.0 - smoothStep(approach / 0.85))
                : 0.0;
        return slope + obstacle;
    }
    case WorkoutGameTerrainKind::Tabletop: {
        const double height = 0.45 + 0.35 * challenge;
        if (phase < 26.0 || phase >= 38.0) return slope;
        if (phase < 30.0) {
            return slope + height * smoothStep((phase - 26.0) / 4.0);
        }
        if (phase < 34.0) return slope + height;
        return slope + height * (1.0 - smoothStep((phase - 34.0) / 4.0));
    }
    case WorkoutGameTerrainKind::RockSlab: {
        const double height = 0.2 + 0.24 * challenge;
        if (phase < 27.0 || phase >= 36.0) return slope;
        if (phase < 29.0) {
            return slope + height * smoothStep((phase - 27.0) / 2.0);
        }
        if (phase < 34.0) return slope + height;
        return slope + height * (1.0 - smoothStep((phase - 34.0) / 2.0));
    }
    case WorkoutGameTerrainKind::Drop:
        if (phase < 30.0) return slope;
        if (phase < 31.0) return slope - smoothStep(phase - 30.0)
                * (0.7 + 0.5 * challenge);
        if (phase < 68.0) return slope - (0.7 + 0.5 * challenge);
        if (phase < 76.0) return slope - (0.7 + 0.5 * challenge)
                * (1.0 - smoothStep((phase - 68.0) / 8.0));
        return slope;
    case WorkoutGameTerrainKind::Climb:
    case WorkoutGameTerrainKind::SmoothTrail:
    case WorkoutGameTerrainKind::Skinny:
    case WorkoutGameTerrainKind::Berm:
        return slope;
    }
    return slope;
}

WorkoutGameWorldSnapshot WorkoutGamePhysics::update(
        const WorkoutGamePhysicsInput &rawInput)
{
    if (!impl->configured) return WorkoutGameWorldSnapshot();

    WorkoutGamePhysicsInput input = rawInput;
    input.workoutTimeMs = std::max<std::int64_t>(0, input.workoutTimeMs);
    input.gradePercent = std::clamp(
            finiteOr(input.gradePercent, 0.0), -20.0, 20.0);
    input.difficulty = std::clamp(
            finiteOr(input.difficulty, 0.0), 0.0, 1.0);
    input.effortRatio = std::clamp(
            finiteOr(input.effortRatio, 0.0), 0.0, 3.0);
    input.courseDistanceMeters = std::isfinite(input.courseDistanceMeters)
            && input.courseDistanceMeters >= 0.0
            ? input.courseDistanceMeters : -1.0;
    impl->authoritativeDistanceMeters = input.courseDistanceMeters;
    if (!impl->initialized && input.courseDistanceMeters >= 0.0) {
        impl->distanceBase = input.courseDistanceMeters;
        impl->publishedDistanceMeters = input.courseDistanceMeters;
    }

    if (impl->initialized && input.workoutTimeMs < impl->lastWorkoutTimeMs) {
        reset();
    }

    const bool terrainChanged = !impl->initialized
            || input.terrain != impl->terrain
            || (!impl->roadCourse.ready
                && (input.gradePercent != impl->gradePercent
                    || input.difficulty != impl->difficulty));
    if (terrainChanged) {
        if (impl->initialized) {
            impl->latest = impl->captureSnapshot();
            impl->distanceBase = impl->latest.rider.distanceMeters;
            impl->elevationBase = impl->latest.rider.elevationMeters;
            ++impl->generation;
        }
        impl->terrain = input.terrain;
        impl->gradePercent = input.gradePercent;
        impl->difficulty = input.difficulty;
        impl->createWorld();
    } else {
        impl->gradePercent = input.gradePercent;
        impl->difficulty = input.difficulty;
    }

    if (!impl->initialized) {
        impl->initialized = true;
        impl->lastWorkoutTimeMs = input.workoutTimeMs;
        impl->latest = impl->captureSnapshot();
        return impl->latest;
    }

    impl->synchronizeDistance(
            input.courseDistanceMeters, input.forceGroundFollowing);

    const std::int64_t elapsedMs = input.workoutTimeMs - impl->lastWorkoutTimeMs;
    impl->lastWorkoutTimeMs = input.workoutTimeMs;
    if (input.paused || elapsedMs <= 0) {
        impl->latest = impl->captureSnapshot();
        return impl->latest;
    }

    const std::int64_t elapsedMicroseconds = std::min<std::int64_t>(
            elapsedMs, MaximumCatchupMicroseconds / 1000) * 1000;
    if (input.terrain == WorkoutGameTerrainKind::Climb
            && input.effortRatio < 0.55) {
        impl->weakClimbMicroseconds = std::min(
                WalkDecisionMicroseconds,
                impl->weakClimbMicroseconds + elapsedMicroseconds);
    } else {
        impl->weakClimbMicroseconds = 0;
    }

    std::int64_t available = elapsedMicroseconds + impl->remainderMicroseconds;
    while (available >= PhysicsStepMicroseconds) {
        impl->step(input);
        available -= PhysicsStepMicroseconds;
    }
    impl->remainderMicroseconds = available;
    impl->latest = impl->captureSnapshot();
    impl->rebaseIfNeeded();
    return impl->latest;
}

WorkoutGameCameraMode WorkoutGameCamera::preferredMode(
        WorkoutGameTerrainKind)
{
    return WorkoutGameCameraMode::ThreeQuarter;
}

void WorkoutGameCamera::reset()
{
    initialized = false;
    currentGeneration = 0;
    current = WorkoutGameCameraSnapshot();
}

WorkoutGameCameraSnapshot WorkoutGameCamera::update(
        const WorkoutGameWorldSnapshot &world,
        double elapsedSeconds)
{
    if (!world.ready) {
        reset();
        return current;
    }

    const WorkoutGameCameraMode mode = preferredMode(world.terrain);
    const double distance = finiteOr(world.rider.distanceMeters, 0.0);
    const double elevation = finiteOr(world.rider.elevationMeters, 0.0)
            - world.rider.airHeightMeters();
    const double speed = std::clamp(
            finiteOr(world.speedMetersPerSecond, 0.0), 0.0, 30.0);
    const double lookAhead = std::clamp(3.0 + speed * 0.55, 3.0, 15.0);
    const double pitch = std::clamp(
            finiteOr(world.rider.pitchDegrees, 0.0) * 0.3,
            -12.0, 12.0);

    const bool worldReset = initialized
            && world.generation != currentGeneration
            && distance + 2.0 < current.centerDistanceMeters;
    if (!initialized || worldReset) {
        current.ready = true;
        current.mode = mode;
        current.centerDistanceMeters = distance;
        current.centerElevationMeters = elevation + 1.2;
        current.lookAheadMeters = lookAhead;
        current.zoom = targetZoom(world.terrain);
        current.yawDegrees = targetYaw(mode);
        current.pitchDegrees = pitch;
        currentGeneration = world.generation;
        initialized = true;
        return current;
    }

    const double dt = std::clamp(
            finiteOr(elapsedSeconds, 0.0), 0.0, 0.1);
    const double followBlend = 1.0 - std::exp(-dt / 0.18);
    const double angleBlend = 1.0 - std::exp(-dt / 0.45);
    current.ready = true;
    current.mode = mode;
    current.centerDistanceMeters = approach(
            current.centerDistanceMeters, distance, followBlend);
    current.centerElevationMeters = approach(
            current.centerElevationMeters, elevation + 1.2, followBlend);
    current.lookAheadMeters = approach(
            current.lookAheadMeters, lookAhead, followBlend);
    current.zoom = approach(
            current.zoom, targetZoom(world.terrain), angleBlend);
    current.yawDegrees = approach(
            current.yawDegrees, targetYaw(mode), angleBlend);
    current.pitchDegrees = approach(
            current.pitchDegrees, pitch, angleBlend);
    return current;
}
