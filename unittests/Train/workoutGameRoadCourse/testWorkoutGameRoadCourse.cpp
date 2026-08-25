/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameRoadProjection.h"
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameHorizon.h"
#include "Train/WorkoutGameTrailBranch.h"

#include <QTest>

#include <cmath>

namespace {

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 123u;
    course.durationMs = 30000;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 15000;
    climb.targetWatts = 220.0;
    climb.gradePercent = 8.0;
    climb.difficulty = 0.7;
    climb.challengeCount = 1;
    climb.visualVariant = 2u;
    WorkoutGameSection log = climb;
    log.feature = WorkoutGameFeature::SprintJump;
    log.terrain = WorkoutGameTerrainKind::LogOver;
    log.startMs = climb.durationMs;
    log.durationMs = 15000;
    log.targetWatts = 260.0;
    log.gradePercent = 0.0;
    log.difficulty = 0.5;
    log.visualVariant = 3u;
    course.sections = {climb, log};
    return course;
}

WorkoutGameCourse crestCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 912u;
    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 10000;
    climb.targetWatts = 220.0;
    climb.gradePercent = 15.0;
    WorkoutGameSection descent = climb;
    descent.startMs = 10000;
    descent.gradePercent = -15.0;
    WorkoutGameSection finish = climb;
    finish.startMs = 20000;
    finish.gradePercent = 0.0;
    course.durationMs = 30000;
    course.sections = {climb, descent, finish};
    return course;
}

bool near(double left, double right, double tolerance = 1e-7)
{
    return std::abs(left - right) <= tolerance;
}

}

class TestWorkoutGameRoadCourse : public QObject
{
    Q_OBJECT

private slots:
    void invalidInputsFailClosed()
    {
        QVERIFY(!WorkoutGameRoadCourseBuilder::build({}, 200.0).ready);
        QVERIFY(!WorkoutGameRoadCourseBuilder::build(
                sampleCourse(), 0.0).ready);
        QVERIFY(!WorkoutGameRoadProjection::project({}, 0.0).ready);
    }

    void piecesJoinWithoutPositionHeadingOrWidthGaps()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QVERIFY(road.ready);
        QVERIFY(road.pieces.size() >= 2u);
        for (std::size_t index = 1; index < road.pieces.size(); ++index) {
            const WorkoutGameRoadConnector &prior = road.pieces[index - 1].exit;
            const WorkoutGameRoadConnector &next = road.pieces[index].entry;
            QVERIFY(near(prior.xMeters, next.xMeters));
            QVERIFY(near(prior.zMeters, next.zMeters));
            QVERIFY(near(prior.elevationMeters, next.elevationMeters));
            QVERIFY(near(prior.headingRadians, next.headingRadians));
            QVERIFY(near(prior.halfWidthMeters, next.halfWidthMeters));
            QVERIFY(near(prior.gradePercent, next.gradePercent));
        }
    }

    void trailingCameraAnchorsTheTrackedRiderToTheChasePosition()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        WorkoutGameRoadProjectionConfig config;
        config.cameraTrailingDistanceMeters = 8.0;
        const double riderDistance = 30.0;
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(
                    road, riderDistance, config);
        QVERIFY(projection.ready);
        QVERIFY(projection.slices.back().worldDistanceMeters
                < riderDistance);
        const WorkoutGameRoadProjectedPoint rider =
                WorkoutGameRoadProjection::projectPoint(
                    projection, riderDistance, 0.0, 0.0);
        QVERIFY(rider.ready);
        QVERIFY(std::abs(rider.x - projection.riderScreenX) < 1e-6);
        QVERIFY(std::abs(rider.y - projection.riderScreenY) < 1e-6);
    }

    void logPieceOwnsTimedJumpGate()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece *jump = nullptr;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (piece.challenge.enabled
                    && piece.terrain == WorkoutGameTerrainKind::LogOver) {
                jump = &piece;
                break;
            }
        }
        QVERIFY(jump != nullptr);
        QCOMPARE(jump->animation, WorkoutGameRoadAnimation::Jump);
        QCOMPARE(jump->challenge.profile.cue, WorkoutGameChallengeCue::Jump);
        QVERIFY(jump->challenge.prepareDistanceMeters
                < jump->challenge.decisionDistanceMeters);
        QVERIFY(jump->challenge.decisionDistanceMeters
                - jump->challenge.prepareDistanceMeters <= 6.0 + 1e-9);
        QVERIFY(jump->challenge.decisionDistanceMeters
                < jump->challenge.obstacleDistanceMeters);
        QCOMPARE(jump->challenge.decisionDistanceMeters,
                 jump->challenge.bypassStartDistanceMeters);
        QVERIFY(jump->challenge.bypassStartDistanceMeters
                < jump->challenge.obstacleDistanceMeters);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                > jump->challenge.obstacleDistanceMeters);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                - jump->challenge.bypassStartDistanceMeters >= 18.0 - 1e-9);
        const double obstacleBypassLateral = std::abs(
                WorkoutGameTrailBranch::lateralAt(
                    jump->challenge.obstacleDistanceMeters,
                    jump->challenge.bypassStartDistanceMeters,
                    jump->challenge.bypassEndDistanceMeters,
                    jump->challenge.bypassLateralMeters));
        QVERIFY2(obstacleBypassLateral >= 1.67,
                 "bypass does not clear the log and rider at the obstacle");
        const WorkoutGameFeatureGeometryProfile featureGeometry =
                WorkoutGameFeatureGeometry::profile(
                    jump->terrain, jump->difficulty);
        QVERIFY(featureGeometry.ready);
        QVERIFY(jump->challenge.bypassEndDistanceMeters
                >= jump->challenge.obstacleDistanceMeters
                    + featureGeometry.endMeters + 10.0 - 1e-9);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                >= jump->startDistanceMeters);
        QVERIFY(jump->challenge.obstacleDistanceMeters
                <= jump->startDistanceMeters + jump->lengthMeters);
        const WorkoutGameRoadSample obstacle =
                WorkoutGameRoadCourseBuilder::sample(
                    road, jump->challenge.obstacleDistanceMeters);
        const WorkoutGameRoadSample approach =
                WorkoutGameRoadCourseBuilder::sample(
                    road, jump->challenge.obstacleDistanceMeters - 1.1);
        QVERIFY(obstacle.ready);
        QVERIFY(approach.ready);
        QVERIFY(obstacle.center.elevationMeters
                > approach.center.elevationMeters + 0.15);
        QVERIFY(std::isfinite(obstacle.center.gradePercent));
    }

    void bunnyHopUsesACompactPreloadWindowOnOrdinaryGround()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections.back().terrain = WorkoutGameTerrainKind::BunnyHop;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto bunny = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain
                                == WorkoutGameTerrainKind::BunnyHop;
                });
        QVERIFY(bunny != road.pieces.end());
        const double preloadMeters =
                bunny->challenge.decisionDistanceMeters
                - bunny->challenge.prepareDistanceMeters;
        QVERIFY(preloadMeters > 0.0);
        QVERIFY(preloadMeters <= 3.0 + 1e-9);
        QVERIFY(bunny->challenge.obstacleDistanceMeters
                - bunny->challenge.decisionDistanceMeters <= 4.0 + 1e-9);
        const WorkoutGameRoadSample obstacle =
                WorkoutGameRoadCourseBuilder::sample(
                    road, bunny->challenge.obstacleDistanceMeters);
        QVERIFY(obstacle.ready);
        QCOMPARE(obstacle.surfaceOffsetMeters, 0.0);
    }

    void dropRoadDefinesAnActualGapAndLowerLanding()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections.back().terrain = WorkoutGameTerrainKind::Drop;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto drop = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain == WorkoutGameTerrainKind::Drop;
                });
        QVERIFY(drop != road.pieces.end());
        const double lip = drop->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadSample approach =
                WorkoutGameRoadCourseBuilder::sample(road, lip - 0.1);
        const WorkoutGameRoadSample gap =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 0.5);
        const WorkoutGameRoadSample landing =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 1.25);
        const WorkoutGameRoadSample runout =
                WorkoutGameRoadCourseBuilder::sample(road, lip + 4.0);
        QVERIFY(approach.ready && gap.ready && landing.ready && runout.ready);
        QVERIFY(approach.rideableSurface);
        QVERIFY(!gap.rideableSurface);
        QVERIFY(landing.rideableSurface);
        QVERIFY(runout.rideableSurface);
        QCOMPARE(approach.surfaceOffsetMeters, 0.0);
        QVERIFY(landing.surfaceOffsetMeters < -0.35);
        QCOMPARE(runout.surfaceOffsetMeters, landing.surfaceOffsetMeters);
    }

    void generationAndSamplingAreDeterministic()
    {
        const WorkoutGameRoadCourse first =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadCourse second =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(first.pieces.size(), second.pieces.size());
        QCOMPARE(first.totalLengthMeters, second.totalLengthMeters);
        for (double distance = 0.0;
             distance <= first.totalLengthMeters; distance += 3.25) {
            const WorkoutGameRoadSample left =
                    WorkoutGameRoadCourseBuilder::sample(first, distance);
            const WorkoutGameRoadSample right =
                    WorkoutGameRoadCourseBuilder::sample(second, distance);
            QCOMPARE(left.center.xMeters, right.center.xMeters);
            QCOMPARE(left.center.zMeters, right.center.zMeters);
            QCOMPARE(left.center.elevationMeters,
                     right.center.elevationMeters);
        }
    }

    void workoutTimelineIsContinuousAndMonotonic()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        QCOMPARE(road.timeline.size(), std::size_t(2));
        QVERIFY(near(road.timeline[0].endDistanceMeters,
                     road.timeline[1].startDistanceMeters));

        double previousDistance = -1.0;
        for (std::int64_t timeMs = -1000; timeMs <= 31000; timeMs += 17) {
            const WorkoutGameRoadTimelineSample sample =
                    WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                            road, timeMs);
            QVERIFY(sample.ready);
            QVERIFY(sample.distanceMeters + 1e-9 >= previousDistance);
            previousDistance = sample.distanceMeters;
        }
        const WorkoutGameRoadTimelineSample boundary =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 15000);
        QCOMPARE(boundary.sourceSectionIndex, std::size_t(1));
        QCOMPARE(boundary.sectionProgress, 0.0);
        QVERIFY(near(boundary.distanceMeters,
                     road.timeline[1].startDistanceMeters));
        const WorkoutGameRoadTimelineSample end =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 40000);
        QCOMPARE(end.distanceMeters, road.totalLengthMeters);
        QCOMPARE(end.sectionProgress, 1.0);
    }

    void savedDistanceCourseLengthsAreNotReestimated()
    {
        WorkoutGameCourse course = sampleCourse();
        course.sections[0].lengthMeters = 114.86038315951464;
        course.sections[1].lengthMeters = 154.99442656317188;
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);

        QVERIFY(road.ready);
        QCOMPARE(road.timeline[0].startDistanceMeters, 0.0);
        QCOMPARE(road.timeline[0].endDistanceMeters,
                 course.sections[0].lengthMeters);
        QCOMPARE(road.timeline[1].startDistanceMeters,
                 course.sections[0].lengthMeters);
        QCOMPARE(road.totalLengthMeters,
                 course.sections[0].lengthMeters
                    + course.sections[1].lengthMeters);
    }

    void climbAndTurnAreVisibleInWorldGeometry()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadPiece &first = road.pieces.front();
        QVERIFY(first.exit.elevationMeters > first.entry.elevationMeters);
        QVERIFY(std::abs(first.exit.headingRadians
                - first.entry.headingRadians) > 0.01);
        QVERIFY(std::abs(first.exit.xMeters - first.entry.xMeters) > 0.01);
    }

    void trailUsesSingletrackWidthAndContinuousRelief()
    {
        WorkoutGameCourse course = sampleCourse();
        for (WorkoutGameSection &section : course.sections) {
            section.terrain = WorkoutGameTerrainKind::SmoothTrail;
            section.gradePercent = 0.0;
            section.challengeCount = 0;
        }
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        double minimumElevation = 1e9;
        double maximumElevation = -1e9;
        double maximumHalfWidth = 0.0;
        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 0.5) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            QVERIFY(sample.ready);
            minimumElevation = std::min(
                    minimumElevation, sample.center.elevationMeters);
            maximumElevation = std::max(
                    maximumElevation, sample.center.elevationMeters);
            maximumHalfWidth = std::max(
                    maximumHalfWidth, sample.center.halfWidthMeters);
        }
        QVERIFY2(maximumHalfWidth <= 0.75,
                 "the generated trail is wider than a singletrack");
        QVERIFY2(maximumElevation - minimumElevation >= 0.20,
                 "smooth trail relief is too flat to read while riding");
        QVERIFY(std::abs(road.pieces.front().turnRadians) > 0.02);
    }

    void jumpEffortWindowIsCloseToObstacle()
    {
        for (std::int64_t durationMs : {15000, 120000}) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.seed = 123u;
            course.durationMs = durationMs;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = WorkoutGameTerrainKind::LogOver;
            section.durationMs = durationMs;
            section.targetWatts = 260.0;
            section.difficulty = 0.5;
            section.challengeCount = 1;
            course.sections.push_back(section);
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, 200.0);
            const auto jump = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    });
            QVERIFY(jump != road.pieces.end());
            QVERIFY(jump->challenge.obstacleDistanceMeters
                    - jump->challenge.prepareDistanceMeters <= 10.0);
            QVERIFY(jump->challenge.obstacleDistanceMeters
                    - jump->challenge.decisionDistanceMeters <= 4.0 + 1e-9);
        }
    }

    void projectionUsesDepthInsteadOfSidewaysTravel()
    {
        WorkoutGameCourse straightCourse = sampleCourse();
        for (WorkoutGameSection &section : straightCourse.sections) {
            section.terrain = WorkoutGameTerrainKind::SmoothTrail;
            section.visualVariant = 0u;
            section.gradePercent = 0.0;
            section.challengeCount = 0;
        }
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(straightCourse, 200.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0);
        QVERIFY(frame.ready);
        QVERIFY(frame.slices.size() >= 90u);
        const double center = 1280.0 * 0.5;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            QVERIFY(std::abs(slice.centerX - center) < 1.0);
        }
        QVERIFY(frame.slices.front().centerY
                < frame.slices.back().centerY);
        QVERIFY(frame.slices.front().halfWidthPixels
                < frame.slices.back().halfWidthPixels);
        QCOMPARE(frame.riderScreenX, center);
    }

    void projectionReportsAndDrawsVisibleClimbRelief()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 19u;
        course.durationMs = 180000;
        WorkoutGameSection climb;
        climb.feature = WorkoutGameFeature::Climb;
        climb.terrain = WorkoutGameTerrainKind::Climb;
        climb.durationMs = course.durationMs;
        climb.targetWatts = 230.0;
        climb.gradePercent = 7.0;
        climb.difficulty = 0.7;
        course.sections = {climb};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 20.0);

        QVERIFY(frame.ready);
        QVERIFY(frame.renderedGradePercent > 5.0);
        QVERIFY(frame.visibleElevationChangeMeters > 4.0);
        QVERIFY(frame.slices.front().centerY
                < frame.slices.back().centerY - 20.0);
    }

    void projectionUsesCanonicalTechnicalSurfaceHeight()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 29u;
        course.durationMs = 60000;
        WorkoutGameSection roots;
        roots.feature = WorkoutGameFeature::Trail;
        roots.terrain = WorkoutGameTerrainKind::Roots;
        roots.durationMs = course.durationMs;
        roots.targetWatts = 170.0;
        roots.difficulty = 1.0;
        course.sections = {roots};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0);
        QVERIFY(frame.ready);

        bool sawTechnicalRelief = false;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, slice.worldDistanceMeters);
            QCOMPARE(slice.surfaceElevationMeters,
                     sample.center.elevationMeters);
            sawTechnicalRelief = sawTechnicalRelief
                    || sample.surfaceOffsetMeters > 0.005;
        }
        QVERIFY(sawTechnicalRelief);
    }

    void forestTrailHasVisibleRollingReliefBetweenConnectors()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 912u;
        course.durationMs = 90000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::Trail;
        section.terrain = WorkoutGameTerrainKind::SmoothTrail;
        section.durationMs = course.durationMs;
        section.targetWatts = 180.0;
        section.difficulty = 0.8;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 190.0);
        QVERIFY(road.ready);
        double maximumRelief = 0.0;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            const double start = piece.startDistanceMeters;
            const double end = start + piece.lengthMeters;
            const double startElevation =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, start).surfaceElevationMeters();
            const double endElevation =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, end).surfaceElevationMeters();
            for (int sampleIndex = 1; sampleIndex < 8; ++sampleIndex) {
                const double progress = double(sampleIndex) / 8.0;
                const double elevation =
                        WorkoutGameRoadCourseBuilder::sample(
                            road, start + progress * piece.lengthMeters)
                            .surfaceElevationMeters();
                const double baseline = startElevation
                        + (endElevation - startElevation) * progress;
                maximumRelief = std::max(
                        maximumRelief, std::abs(elevation - baseline));
            }
        }
        QVERIFY2(maximumRelief > 0.8,
                 "terrain relief is still visually too shallow");
    }

    void jumpSurfaceOffsetIsAppliedExactlyOnce()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 730u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = course.durationMs;
        section.targetWatts = 260.0;
        section.difficulty = 1.0;
        section.challengeCount = 1;
        course.sections = {section};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double obstacle = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, obstacle - 8.0);
        bool sawFeatureSurface = false;
        for (const WorkoutGameRoadProjectedSlice &slice : frame.slices) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, slice.worldDistanceMeters);
            QCOMPARE(slice.surfaceElevationMeters,
                     sample.surfaceElevationMeters());
            sawFeatureSurface = sawFeatureSurface
                    || sample.surfaceOffsetMeters
                        >= profile.heightMeters * 0.95;
        }
        QVERIFY(sawFeatureSurface);
        QVERIFY(frame.slices.size() >= 16u);
    }

    void horizonIsShapedDeterministicAndScrollsSmoothly()
    {
        const WorkoutGameHorizonSnapshot first =
                WorkoutGameHorizon::build(123u, 100.0, 32);
        const WorkoutGameHorizonSnapshot same =
                WorkoutGameHorizon::build(123u, 100.0, 32);
        const WorkoutGameHorizonSnapshot moved =
                WorkoutGameHorizon::build(123u, 100.1, 32);
        QVERIFY(first.ready);
        QCOMPARE(first.farRidgeY, same.farRidgeY);
        QCOMPARE(first.nearRidgeY, same.nearRidgeY);
        double minimum = 1.0;
        double maximum = 0.0;
        for (std::size_t index = 0; index < first.nearRidgeY.size(); ++index) {
            minimum = std::min(minimum, first.nearRidgeY[index]);
            maximum = std::max(maximum, first.nearRidgeY[index]);
            QVERIFY(std::abs(first.nearRidgeY[index]
                    - moved.nearRidgeY[index]) < 0.002);
        }
        QVERIFY(maximum - minimum > 0.06);
    }

    void tinyDistanceChangesProduceContinuousProjection()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        const WorkoutGameRoadProjectionFrame first =
                WorkoutGameRoadProjection::project(road, 10.0);
        const WorkoutGameRoadProjectionFrame next =
                WorkoutGameRoadProjection::project(road, 10.01);
        QVERIFY(first.ready);
        QVERIFY(next.ready);
        QCOMPARE(first.slices.size(), next.slices.size());
        for (std::size_t index = 0; index < first.slices.size(); ++index) {
            QVERIFY(std::abs(first.slices[index].centerX
                    - next.slices[index].centerX) < 2.0);
            QVERIFY(std::abs(first.slices[index].centerY
                    - next.slices[index].centerY) < 2.0);
        }
    }

    void crestOcclusionMarksRoadHiddenByNearerTerrain()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(crestCourse(), 200.0);
        WorkoutGameRoadProjectionConfig config;
        config.visibleDistanceMeters = 120.0;
        const WorkoutGameRoadProjectionFrame frame =
                WorkoutGameRoadProjection::project(road, 0.0, config);
        QVERIFY(frame.ready);

        bool hiddenSlice = false;
        double priorOcclusion = config.viewportHeight;
        for (auto slice = frame.slices.rbegin();
             slice != frame.slices.rend(); ++slice) {
            QVERIFY(slice->occlusionY <= priorOcclusion + 1e-9);
            priorOcclusion = slice->occlusionY;
            hiddenSlice = hiddenSlice
                    || slice->centerY > slice->occlusionY + 1e-6;
        }
        QVERIFY2(hiddenSlice, "the valley behind the crest was not occluded");
    }

    void explicitCameraElevationMovesProjectionSmoothly()
    {
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(sampleCourse(), 200.0);
        WorkoutGameRoadProjectionConfig lowConfig;
        lowConfig.cameraElevationMeters = 2.0;
        WorkoutGameRoadProjectionConfig highConfig = lowConfig;
        highConfig.cameraElevationMeters = 2.1;
        const WorkoutGameRoadProjectionFrame low =
                WorkoutGameRoadProjection::project(road, 10.0, lowConfig);
        const WorkoutGameRoadProjectionFrame high =
                WorkoutGameRoadProjection::project(road, 10.0, highConfig);
        QVERIFY(low.ready);
        QVERIFY(high.ready);
        QCOMPARE(low.slices.size(), high.slices.size());
        for (std::size_t index = 0; index < low.slices.size(); ++index) {
            QVERIFY(high.slices[index].centerY > low.slices[index].centerY);
        }
    }

    void projectionPreservesNarrowFeatureBreakpoints()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 551u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::LogOver;
        section.durationMs = source.durationMs;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        source.sections.push_back(section);
        const WorkoutGameRoadCourse course =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        QVERIFY(course.ready);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double obstacle = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadProjectionFrame projection =
                WorkoutGameRoadProjection::project(course, obstacle - 10.0);
        QVERIFY(projection.ready);
        const auto obstacleSlice = std::find_if(
                projection.slices.begin(), projection.slices.end(),
                [obstacle](const WorkoutGameRoadProjectedSlice &slice) {
                    return std::abs(slice.worldDistanceMeters - obstacle) < 1e-7;
                });
        QVERIFY(obstacleSlice != projection.slices.end());
        QVERIFY(obstacleSlice->surfaceOffsetMeters > 0.4);
    }

    void featureSurfaceContinuesAcrossPieceBoundary()
    {
        WorkoutGameCourse source;
        source.status = WorkoutGameCourseStatus::Ready;
        source.seed = 667u;
        source.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::Tabletop;
        section.durationMs = source.durationMs;
        section.targetWatts = 280.0;
        section.difficulty = 0.7;
        section.challengeCount = 1;
        source.sections.push_back(section);
        WorkoutGameRoadCourse selected =
                WorkoutGameRoadCourseBuilder::build(source, 200.0);
        QVERIFY(selected.pieces.size() >= 2u);
        const auto generatedChallenge = std::find_if(
                selected.pieces.begin(), selected.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled;
                });
        QVERIFY(generatedChallenge != selected.pieces.end());
        WorkoutGameRoadChallengeGate challenge = generatedChallenge->challenge;
        for (WorkoutGameRoadPiece &piece : selected.pieces) {
            piece.challenge.enabled = false;
        }
        WorkoutGameRoadPiece &challengePiece = selected.pieces.front();
        challengePiece.challenge = challenge;
        challengePiece.challenge.enabled = true;
        const double boundary = challengePiece.startDistanceMeters
                + challengePiece.lengthMeters;
        challengePiece.challenge.obstacleDistanceMeters = boundary;
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    challengePiece.terrain, challengePiece.difficulty);
        QVERIFY(profile.ready);
        const WorkoutGameRoadSample before =
                WorkoutGameRoadCourseBuilder::sample(selected, boundary - 0.01);
        const WorkoutGameRoadSample after =
                WorkoutGameRoadCourseBuilder::sample(selected, boundary + 0.01);
        QVERIFY(before.surfaceOffsetMeters > 0.05);
        QVERIFY(after.surfaceOffsetMeters > 0.05);
        QVERIFY(std::abs(before.surfaceOffsetMeters
                - after.surfaceOffsetMeters) < 0.05);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameRoadCourse)
#include "testWorkoutGameRoadCourse.moc"
