/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DGeometry.h"
#include "WorkoutGameGapJumpGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTrailBranch.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

WorkoutGameRoadCourse straightCourse(double length, double rise = 0.0)
{
    WorkoutGameRoadCourse course;
    course.ready = true;
    course.seed = 42;
    course.totalLengthMeters = length;
    WorkoutGameRoadPiece piece;
    piece.lengthMeters = length;
    piece.riseMeters = rise;
    piece.entry.halfWidthMeters = 0.68;
    piece.exit.halfWidthMeters = 0.68;
    piece.exit.zMeters = length;
    piece.exit.elevationMeters = rise;
    piece.exit.gradePercent = length > 0.0 ? rise / length * 100.0 : 0.0;
    course.pieces.push_back(piece);
    return course;
}

float vertexFloat(const QByteArray &data, int stride, int vertex, int offset)
{
    float value = 0.0f;
    std::memcpy(&value,
                data.constData() + vertex * stride + offset,
                sizeof(value));
    return value;
}

quint32 indexValue(const QByteArray &data, int index)
{
    quint32 value = 0;
    std::memcpy(&value, data.constData() + index * int(sizeof(value)),
                sizeof(value));
    return value;
}

WorkoutGameRoadCourse dropCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 611u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::RecoveryDescent;
    section.terrain = WorkoutGameTerrainKind::Drop;
    section.durationMs = source.durationMs;
    section.targetWatts = 150.0;
    section.gradePercent = -4.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse bermCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 407u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::Berm;
    section.durationMs = source.durationMs;
    section.lengthMeters = 70.0;
    section.targetWatts = 180.0;
    section.difficulty = 0.65;
    section.challengeCount = 0;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse rootsCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 713u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::Roots;
    section.durationMs = source.durationMs;
    section.lengthMeters = 70.0;
    section.targetWatts = 180.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse rockGardenCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 977u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::RockGarden;
    section.durationMs = source.durationMs;
    section.lengthMeters = 76.0;
    section.targetWatts = 185.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse rockSlabCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 1147u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::RockSlab;
    section.durationMs = source.durationMs;
    section.lengthMeters = 76.0;
    section.targetWatts = 210.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse skinnyCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 1201u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Trail;
    section.terrain = WorkoutGameTerrainKind::Skinny;
    section.durationMs = source.durationMs;
    section.lengthMeters = 76.0;
    section.targetWatts = 175.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse climbCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 1229u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::Climb;
    section.terrain = WorkoutGameTerrainKind::Climb;
    section.durationMs = source.durationMs;
    section.lengthMeters = 80.0;
    section.targetWatts = 230.0;
    section.gradePercent = 9.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse tabletopCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 1301u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::Tabletop;
    section.durationMs = source.durationMs;
    section.lengthMeters = 90.0;
    section.targetWatts = 240.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

WorkoutGameRoadCourse gapJumpCourse()
{
    WorkoutGameCourse source;
    source.status = WorkoutGameCourseStatus::Ready;
    source.seed = 1701u;
    source.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::GapJump;
    section.durationMs = source.durationMs;
    section.lengthMeters = 120.0;
    section.targetWatts = 260.0;
    section.difficulty = 0.5;
    section.challengeCount = 1;
    source.sections = {section};
    return WorkoutGameRoadCourseBuilder::build(source, 200.0);
}

}

class TestWorkoutGame3DGeometry : public QObject
{
    Q_OBJECT

private slots:
    void gapJumpBuildsThreeOpenLinesWithoutTrailBridges()
    {
        const WorkoutGameRoadCourse course = gapJumpCourse();
        QVERIFY(course.ready);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.gapJump.enabled;
                });
        QVERIFY(piece != course.pieces.end());

        WorkoutGame3DGeometry gapJump(
                WorkoutGame3DGeometry::Layer::GapJump);
        WorkoutGame3DGeometry trail(
                WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        gapJump.setCourse(course);
        trail.setCourse(course);
        floor.setCourse(course);
        QVERIFY(gapJump.ready());
        QVERIFY(floor.ready());
        QVERIFY(gapJump.sampleCount() >= 120);
        QVERIFY(gapJump.triangleCount() >= 180);

        const int vertexCount = gapJump.vertexData().size()
                / gapJump.stride();
        std::array<std::vector<int>, 3> lineRows;
        std::size_t parsedLine = 0u;
        double previousDistance = -1.0;
        for (int row = 0; row < vertexCount / 4; ++row) {
            const double distance = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), row * 4, 44)
                    / 0.22;
            if (row > 0 && distance < previousDistance - 0.1) {
                ++parsedLine;
            }
            QVERIFY(parsedLine < lineRows.size());
            lineRows[parsedLine].push_back(row);
            previousDistance = distance;
        }
        QCOMPARE(parsedLine, std::size_t(2));

        const auto vertexLateral = [&gapJump, &course](int vertex) {
            const double distance = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 44) / 0.22;
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        course, distance);
            const double x = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 0);
            const double z = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 8);
            const double rightX = std::cos(road.center.headingRadians);
            const double rightZ = -std::sin(road.center.headingRadians);
            return (x - road.center.xMeters) * rightX
                    + (z - road.center.zMeters) * rightZ;
        };
        const auto rowCenter = [&vertexLateral](int row) {
            const double left = vertexLateral(row * 4 + 1);
            const double right = vertexLateral(row * 4 + 2);
            return (left + right) * 0.5;
        };
        const auto rowHalfWidth = [&vertexLateral](int row) {
            const double left = vertexLateral(row * 4 + 1);
            const double right = vertexLateral(row * 4 + 2);
            return (right - left) * 0.5;
        };
        for (const WorkoutGameRoadGapJumpLine &line : piece->gapJump.lines) {
            const std::size_t lineIndex = std::size_t(
                    &line - piece->gapJump.lines.data());
            QVERIFY(!lineRows[lineIndex].empty());
            const int firstRow = lineRows[lineIndex].front();
            const int lastRow = lineRows[lineIndex].back();
            QVERIFY(std::abs(rowCenter(firstRow)) < 0.001);
            QVERIFY(std::abs(rowHalfWidth(firstRow) - 0.68) < 0.001);
            QVERIFY(std::abs(rowCenter(lastRow)) < 0.001);
            QVERIFY(std::abs(rowHalfWidth(lastRow) - 0.68) < 0.001);

            std::array<bool, 6> foundFanSamples{};
            bool foundLip = false;
            bool foundLanding = false;
            for (const int row : lineRows[lineIndex]) {
                const double distance = vertexFloat(
                        gapJump.vertexData(), gapJump.stride(), row * 4, 44)
                        / 0.22;
                for (std::size_t sample = 0;
                        sample < foundFanSamples.size(); ++sample) {
                    const double progress = double(sample)
                            / double(foundFanSamples.size() - 1u);
                    const double expectedDistance =
                            piece->gapJump.splitStartDistanceMeters
                            + (line.takeoffDistanceMeters
                                - piece->gapJump.splitStartDistanceMeters)
                                * progress;
                    if (std::abs(distance - expectedDistance) < 0.01) {
                        const double p3 = progress * progress * progress;
                        const double smoother = p3
                                * (progress * (progress * 6.0 - 15.0)
                                    + 10.0);
                        QVERIFY(std::abs(rowCenter(row)
                                - line.lateralMeters * smoother) < 0.01);
                        foundFanSamples[sample] = true;
                    }
                }
                if (std::abs(distance
                        - line.takeoffDistanceMeters) < 0.01) {
                    QVERIFY(std::abs(rowCenter(row)
                            - line.lateralMeters) < 0.01);
                    foundLip = true;
                }
                if (std::abs(distance
                        - line.landingDistanceMeters) < 0.01) {
                    QVERIFY(std::abs(rowCenter(row)
                            - line.lateralMeters) < 0.01);
                    foundLanding = true;
                }
            }
            for (const bool found : foundFanSamples) {
                QVERIFY2(found,
                         "gap line lacks a runtime-aligned fan-out sample");
            }
            QVERIFY2(foundLip, "gap line has no recognizable takeoff row");
            QVERIFY2(foundLanding, "gap line has no distinct landing row");

            const int indexCount = gapJump.indexData().size()
                    / int(sizeof(quint32));
            for (int index = 0; index + 2 < indexCount; index += 3) {
                double minimumDistance =
                        std::numeric_limits<double>::infinity();
                double maximumDistance =
                        -std::numeric_limits<double>::infinity();
                double meanLateral = 0.0;
                for (int corner = 0; corner < 3; ++corner) {
                    const int vertex = int(indexValue(
                            gapJump.indexData(), index + corner));
                    meanLateral += vertexLateral(vertex) / 3.0;
                    const double distance = vertexFloat(
                            gapJump.vertexData(), gapJump.stride(), vertex, 44)
                            / 0.22;
                    minimumDistance = std::min(minimumDistance, distance);
                    maximumDistance = std::max(maximumDistance, distance);
                }
                if (std::abs(meanLateral - line.lateralMeters) < 0.9) {
                    QVERIFY2(!(minimumDistance
                                <= line.takeoffDistanceMeters + 0.01
                               && maximumDistance
                                >= line.landingDistanceMeters - 0.01),
                             "gap-jump triangle bridges open tread");
                }
            }
        }

        const int trailIndexCount = trail.indexData().size()
                / int(sizeof(quint32));
        for (int index = 0; index + 2 < trailIndexCount; index += 3) {
            double minimumDistance =
                    std::numeric_limits<double>::infinity();
            double maximumDistance =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        trail.indexData(), index + corner));
                const double distance = vertexFloat(
                        trail.vertexData(), trail.stride(), vertex, 44) / 0.22;
                minimumDistance = std::min(minimumDistance, distance);
                maximumDistance = std::max(maximumDistance, distance);
            }
            QVERIFY2(maximumDistance
                        <= piece->gapJump.splitStartDistanceMeters + 0.01
                        || minimumDistance
                        >= piece->gapJump.mergeEndDistanceMeters - 0.01,
                     "ordinary trail intrudes into the gap-jump gate");
        }

        const int floorIndexCount = floor.indexData().size()
                / int(sizeof(quint32));
        int outerFloorTrianglesInsideGate = 0;
        for (int index = 0; index + 2 < floorIndexCount; index += 3) {
            double minimumDistance =
                    std::numeric_limits<double>::infinity();
            double maximumDistance =
                    -std::numeric_limits<double>::infinity();
            double minimumLateral =
                    std::numeric_limits<double>::infinity();
            double maximumLateral =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        floor.indexData(), index + corner));
                const double distance = vertexFloat(
                        floor.vertexData(), floor.stride(), vertex, 44) / 0.22;
                minimumDistance = std::min(minimumDistance, distance);
                maximumDistance = std::max(maximumDistance, distance);
                const WorkoutGameRoadSample road =
                        WorkoutGameRoadCourseBuilder::sampleVisual(
                            course, distance);
                QVERIFY(road.ready);
                const double x = vertexFloat(
                        floor.vertexData(), floor.stride(), vertex, 0);
                const double z = vertexFloat(
                        floor.vertexData(), floor.stride(), vertex, 8);
                const double lateral = (x - road.center.xMeters)
                            * std::cos(road.center.headingRadians)
                        + (z - road.center.zMeters)
                            * -std::sin(road.center.headingRadians);
                minimumLateral = std::min(minimumLateral, lateral);
                maximumLateral = std::max(maximumLateral, lateral);
            }
            if (minimumDistance
                        > piece->gapJump.splitStartDistanceMeters + 0.05
                    && maximumDistance
                        < piece->gapJump.mergeEndDistanceMeters - 0.05) {
                QVERIFY2(!(minimumLateral < -0.01
                            && maximumLateral > 0.01),
                         "procedural floor spans the authored gap tile");
                ++outerFloorTrianglesInsideGate;
            }
        }
        QVERIFY(outerFloorTrianglesInsideGate > 0);
    }

    void gapJumpPreservesCommonSocketAndSeparateGroundedBypass()
    {
        const WorkoutGameRoadCourse course = gapJumpCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.gapJump.enabled;
                });
        QVERIFY(piece != course.pieces.end());

        WorkoutGame3DGeometry gapJump(
                WorkoutGame3DGeometry::Layer::GapJump);
        WorkoutGame3DGeometry bypass(
                WorkoutGame3DGeometry::Layer::Bypass);
        gapJump.setCourse(course);
        bypass.setCourse(course);
        QVERIFY(gapJump.ready());
        QVERIFY(bypass.ready());

        float minimumEntryX = std::numeric_limits<float>::infinity();
        float maximumEntryX = -std::numeric_limits<float>::infinity();
        float minimumExitX = std::numeric_limits<float>::infinity();
        float maximumExitX = -std::numeric_limits<float>::infinity();
        const int vertexCount = gapJump.vertexData().size()
                / gapJump.stride();
        const auto localLateral = [&gapJump, &course](int vertex) {
            const double distance = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 44) / 0.22;
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        course, distance);
            const double x = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 0);
            const double z = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 8);
            return (x - road.center.xMeters)
                        * std::cos(road.center.headingRadians)
                    + (z - road.center.zMeters)
                        * -std::sin(road.center.headingRadians);
        };
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const float x = float(localLateral(vertex));
            const double distance = vertexFloat(
                    gapJump.vertexData(), gapJump.stride(), vertex, 44) / 0.22;
            if (std::abs(distance
                    - piece->gapJump.splitStartDistanceMeters) < 0.01) {
                minimumEntryX = std::min(minimumEntryX, x);
                maximumEntryX = std::max(maximumEntryX, x);
            }
            if (std::abs(distance
                    - piece->gapJump.mergeEndDistanceMeters) < 0.01) {
                minimumExitX = std::min(minimumExitX, x);
                maximumExitX = std::max(maximumExitX, x);
            }
        }
        QVERIFY(std::abs(minimumEntryX + 0.68f) < 0.01f);
        QVERIFY(std::abs(maximumEntryX - 0.68f) < 0.01f);
        QVERIFY(std::abs(minimumExitX + 0.68f) < 0.01f);
        QVERIFY(std::abs(maximumExitX - 0.68f) < 0.01f);

        double maximumBypassOffset = 0.0;
        for (int row = 0; row < bypass.sampleCount(); ++row) {
            const int base = row * 4;
            const double distance = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base, 44) / 0.22;
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                        course, distance);
            const auto bypassLateral = [&bypass, &road](int vertex) {
                const double x = vertexFloat(
                        bypass.vertexData(), bypass.stride(), vertex, 0);
                const double z = vertexFloat(
                        bypass.vertexData(), bypass.stride(), vertex, 8);
                return (x - road.center.xMeters)
                            * std::cos(road.center.headingRadians)
                        + (z - road.center.zMeters)
                            * -std::sin(road.center.headingRadians);
            };
            const double left = bypassLateral(base + 1);
            const double right = bypassLateral(base + 2);
            maximumBypassOffset = std::max(
                    maximumBypassOffset, std::abs((left + right) * 0.5));
        }
        QVERIFY(maximumBypassOffset > 4.4);
    }

    void climbBuildsMergedEmbeddedStepsWithinBudget()
    {
        const WorkoutGameRoadCourse course = climbCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameClimbGeometryProfile profile =
                WorkoutGameClimbGeometry::profile(piece->difficulty);
        WorkoutGame3DGeometry climb(WorkoutGame3DGeometry::Layer::Climb);
        climb.setCourse(course);
        QVERIFY(climb.ready());
        QCOMPARE(climb.sampleCount(), 680);
        QCOMPARE(climb.vertexData().size(),
                 climb.sampleCount() * climb.stride());
        const int triangleCount = climb.indexData().size()
                / int(3 * sizeof(quint32));
        QCOMPARE(triangleCount, 340);
        QVERIFY(climb.boundsMax().y() - climb.boundsMin().y() > 0.12f);

        constexpr int RampSegments = 4;
        constexpr int VerticesPerStep = 136;
        for (int stepIndex = 0;
             stepIndex < int(profile.steps.size()); ++stepIndex) {
            const WorkoutGameClimbStep &step =
                    profile.steps[std::size_t(stepIndex)];
            const WorkoutGameRoadSample center =
                    WorkoutGameRoadCourseBuilder::sample(
                        course,
                        piece->challenge.obstacleDistanceMeters
                            + step.forwardMeters);
            QVERIFY(center.ready);
            const double yaw = step.yawDegrees
                    * 3.14159265358979323846 / 180.0;
            const double cosine = std::cos(yaw);
            const double sine = std::sin(yaw);
            const std::array<std::array<double, 2>, 4> corners = {{
                {{-step.halfLengthMeters, -step.halfWidthMeters}},
                {{ step.halfLengthMeters, -step.halfWidthMeters}},
                {{ step.halfLengthMeters,  step.halfWidthMeters}},
                {{-step.halfLengthMeters,  step.halfWidthMeters}}
            }};
            const double datum = center.visualGroundElevationMeters()
                    - center.surfaceOffsetMeters;
            for (int corner = 0; corner < int(corners.size()); ++corner) {
                const double localForward = corners[std::size_t(corner)][0];
                const double localLateral = corners[std::size_t(corner)][1];
                const double forward = localForward * cosine
                        - localLateral * sine;
                const double lateral = step.lateralMeters
                        + localForward * sine + localLateral * cosine;
                const double contact = profile.surfaceOffsetMeters(
                        step.forwardMeters + forward, lateral);
                QCOMPARE(contact, step.heightMeters);
                const double expectedTop = datum
                        + forward * center.baseGradePercent / 100.0
                        + contact + 0.006;
                const float renderedTop = vertexFloat(
                        climb.vertexData(), climb.stride(),
                        stepIndex * VerticesPerStep + corner,
                        int(sizeof(float)));
                QVERIFY2(std::abs(double(renderedTop) - expectedTop) < 1e-5,
                         "climb top vertex does not match contact surface");
            }
            for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
                const int side = sideIndex == 0 ? -1 : 1;
                const double inner = side < 0
                        ? -step.halfLengthMeters : step.halfLengthMeters;
                const double outer = side < 0
                        ? inner - profile.contactRampMeters
                        : inner + profile.contactRampMeters;
                for (int segment = 0; segment < RampSegments; ++segment) {
                    const double p0 = double(segment) / RampSegments;
                    const double p1 = double(segment + 1) / RampSegments;
                    const double forward0 = side < 0
                            ? outer + (inner - outer) * p0
                            : inner + (outer - inner) * p0;
                    const double forward1 = side < 0
                            ? outer + (inner - outer) * p1
                            : inner + (outer - inner) * p1;
                    const std::array<std::array<double, 2>, 4> rampCorners = {{
                        {{forward0, -step.halfWidthMeters}},
                        {{forward1, -step.halfWidthMeters}},
                        {{forward1,  step.halfWidthMeters}},
                        {{forward0,  step.halfWidthMeters}}
                    }};
                    const int firstRampVertex = stepIndex * VerticesPerStep
                            + 24 + sideIndex * 56 + segment * 12;
                    for (int corner = 0;
                         corner < int(rampCorners.size()); ++corner) {
                        const double localForward =
                                rampCorners[std::size_t(corner)][0];
                        const double localLateral =
                                rampCorners[std::size_t(corner)][1];
                        const double forward = localForward * cosine
                                - localLateral * sine;
                        const double contact =
                                profile.stepSurfaceOffsetMeters(
                                    step, localForward, localLateral);
                        const double expectedTop = datum
                                + forward * center.baseGradePercent / 100.0
                                + contact + 0.006;
                        const float renderedTop = vertexFloat(
                                climb.vertexData(), climb.stride(),
                                firstRampVertex + corner,
                                int(sizeof(float)));
                        QVERIFY2(std::abs(double(renderedTop) - expectedTop)
                                    < 1e-5,
                                 "climb ramp vertex does not match contact");
                    }
                }
            }
        }
    }

    void climbRangeBuildUsesRotatedStepFootprint()
    {
        const WorkoutGameRoadCourse course = climbCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameClimbGeometryProfile profile =
                WorkoutGameClimbGeometry::profile(piece->difficulty);
        const WorkoutGameClimbStep &step = profile.steps.front();
        const double yaw = step.yawDegrees
                * 3.14159265358979323846 / 180.0;
        const double projectedExtent = std::abs(std::cos(yaw))
                    * (step.halfLengthMeters + profile.contactRampMeters)
                + std::abs(std::sin(yaw)) * step.halfWidthMeters;
        const double center = piece->challenge.obstacleDistanceMeters
                + step.forwardMeters;
        const double unrotatedExtent = step.halfLengthMeters
                + profile.contactRampMeters;
        const double overlapStart = center + unrotatedExtent
                + 0.25 * (projectedExtent - unrotatedExtent);

        WorkoutGame3DGeometry climb(WorkoutGame3DGeometry::Layer::Climb);
        climb.setCourseRange(course, overlapStart,
                center + projectedExtent - 1e-5);
        QVERIFY(climb.ready());
        QCOMPARE(climb.sampleCount(), 136);

        climb.setCourseRange(course,
                center + projectedExtent + 1e-4,
                center + projectedExtent + 0.02);
        QVERIFY(!climb.ready());
        QCOMPARE(climb.sampleCount(), 0);
    }

    void skinnyBuildsMergedBoardsBeamsAndGroundedSupportsWithinBudget()
    {
        const WorkoutGameRoadCourse course = skinnyCourse();
        WorkoutGame3DGeometry skinny(WorkoutGame3DGeometry::Layer::Skinny);
        skinny.setCourse(course);
        QVERIFY(skinny.ready());
        QCOMPARE(skinny.sampleCount(), 1584);
        QCOMPARE(skinny.vertexData().size(),
                 skinny.sampleCount() * skinny.stride());
        const int triangleCount = skinny.indexData().size()
                / int(3 * sizeof(quint32));
        QCOMPARE(triangleCount, 792);
        QVERIFY(skinny.boundsMax().x() - skinny.boundsMin().x() > 0.45f);
        QVERIFY(skinny.boundsMax().y() - skinny.boundsMin().y() > 0.30f);
        double maximumHorizontalSpan = 0.0;
        for (int first = 0; first < skinny.sampleCount(); ++first) {
            const double firstX = vertexFloat(
                    skinny.vertexData(), skinny.stride(), first, 0);
            const double firstZ = vertexFloat(
                    skinny.vertexData(), skinny.stride(), first,
                    2 * int(sizeof(float)));
            for (int second = first + 1;
                 second < skinny.sampleCount(); ++second) {
                const double secondX = vertexFloat(
                        skinny.vertexData(), skinny.stride(), second, 0);
                const double secondZ = vertexFloat(
                        skinny.vertexData(), skinny.stride(), second,
                        2 * int(sizeof(float)));
                maximumHorizontalSpan = std::max(
                        maximumHorizontalSpan,
                        std::hypot(secondX - firstX, secondZ - firstZ));
            }
        }
        QVERIFY2(maximumHorizontalSpan > 23.5,
                 "the merged skinny does not cover its authored length");
    }

    void skinnyRangeBuildExcludesDistantTiles()
    {
        WorkoutGame3DGeometry skinny(WorkoutGame3DGeometry::Layer::Skinny);
        const WorkoutGameRoadCourse course = skinnyCourse();
        skinny.setCourseRange(course, 0.0, 5.0);
        QVERIFY(!skinny.ready());
        QCOMPARE(skinny.sampleCount(), 0);
        skinny.setCourse(course);
        QVERIFY(skinny.ready());
    }

    void skinnyKeepsForestFloorUnderDeckWithoutRestoringDirtTrail()
    {
        const WorkoutGameRoadCourse course = skinnyCourse();
        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        trail.setCourse(course);
        floor.setCourse(course);

        QVERIFY(trail.ready());
        QVERIFY(floor.ready());
        QVERIFY(trail.indexData().size()
                < (trail.sampleCount() - 1) * 6 * int(sizeof(quint32)));
        QCOMPARE(floor.indexData().size(),
                 (floor.sampleCount() - 1) * 42 * int(sizeof(quint32)));
    }

    void rockSlabBuildsOneMergedAsymmetricMassWithinBudget()
    {
        const WorkoutGameRoadCourse course = rockSlabCourse();
        WorkoutGame3DGeometry slab(WorkoutGame3DGeometry::Layer::RockSlab);
        slab.setCourse(course);

        QVERIFY(slab.ready());
        QVERIFY(slab.sampleCount() >= 120);
        QVERIFY(slab.sampleCount() <= 160);
        QCOMPARE(slab.vertexData().size(),
                 slab.sampleCount() * slab.stride());
        const int triangleCount = slab.indexData().size()
                / int(3 * sizeof(quint32));
        QVERIFY(triangleCount >= 180);
        QVERIFY(triangleCount <= 228);
        const double horizontalX =
                slab.boundsMax().x() - slab.boundsMin().x();
        const double horizontalZ =
                slab.boundsMax().z() - slab.boundsMin().z();
        QVERIFY(std::min(horizontalX, horizontalZ) > 1.5);
        QVERIFY(slab.boundsMax().y() - slab.boundsMin().y() > 0.9f);
        QVERIFY(std::hypot(horizontalX, horizontalZ) > 7.0);
    }

    void rockSlabRangeBuildExcludesDistantTiles()
    {
        WorkoutGame3DGeometry slab(WorkoutGame3DGeometry::Layer::RockSlab);
        const WorkoutGameRoadCourse course = rockSlabCourse();
        slab.setCourseRange(course, 0.0, 5.0);
        QVERIFY(!slab.ready());
        QCOMPARE(slab.sampleCount(), 0);

        slab.setCourse(course);
        QVERIFY(slab.ready());
    }

    void rockGardenBuildsOneMergedBuriedStoneNetwork()
    {
        const WorkoutGameRoadCourse course = rockGardenCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameRockGardenGeometryProfile profile =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        WorkoutGame3DGeometry rocks(
                WorkoutGame3DGeometry::Layer::RockGarden);
        rocks.setCourse(course);

        constexpr int VerticesPerStone = 15;
        constexpr int IndicesPerStone = 63;
        QVERIFY(rocks.ready());
        QCOMPARE(rocks.sampleCount(),
                 int(profile.stones.size()) * VerticesPerStone);
        QCOMPARE(rocks.vertexData().size(),
                 rocks.sampleCount() * rocks.stride());
        QCOMPARE(rocks.indexData().size(),
                 int(profile.stones.size()) * IndicesPerStone
                    * int(sizeof(quint32)));
        QVERIFY(rocks.boundsMax().x() - rocks.boundsMin().x() > 1.6f);
        QVERIFY(rocks.boundsMax().y() > rocks.boundsMin().y() + 0.18f);
        QVERIFY(rocks.boundsMax().z() - rocks.boundsMin().z() > 5.0f);
    }

    void rockGardenRangeBuildExcludesDistantTiles()
    {
        WorkoutGame3DGeometry rocks(
                WorkoutGame3DGeometry::Layer::RockGarden);
        const WorkoutGameRoadCourse course = rockGardenCourse();
        rocks.setCourseRange(course, 0.0, 5.0);
        QVERIFY(!rocks.ready());
        QCOMPARE(rocks.sampleCount(), 0);

        rocks.setCourse(course);
        QVERIFY(rocks.ready());
    }

    void rootsBuildAProceduralBuriedTubeNetwork()
    {
        const WorkoutGameRoadCourse course = rootsCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const WorkoutGameRootGeometryProfile profile =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        WorkoutGame3DGeometry roots(WorkoutGame3DGeometry::Layer::Roots);
        roots.setCourse(course);

        QVERIFY(roots.ready());
        QCOMPARE(roots.sampleCount(),
                 int(profile.segments.size()) * 5);
        QCOMPARE(roots.vertexData().size(),
                 roots.sampleCount() * 8 * roots.stride());
        QCOMPARE(roots.indexData().size(),
                 int(profile.segments.size()) * 4 * 8 * 6
                    * int(sizeof(quint32)));
        QVERIFY(roots.boundsMax().x() - roots.boundsMin().x() > 1.4f);
        QVERIFY(roots.boundsMax().y() > roots.boundsMin().y() + 0.07f);
        QVERIFY(roots.boundsMax().z() > roots.boundsMin().z() + 3.0f);
    }

    void rootsRangeBuildExcludesDistantTiles()
    {
        WorkoutGame3DGeometry roots(WorkoutGame3DGeometry::Layer::Roots);
        const WorkoutGameRoadCourse course = rootsCourse();
        roots.setCourseRange(course, 0.0, 5.0);
        QVERIFY(!roots.ready());
        QCOMPARE(roots.sampleCount(), 0);

        roots.setCourse(course);
        QVERIFY(roots.ready());
    }

    void invalidCourseClearsGeometry()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(10.0));
        QVERIFY(geometry.ready());

        geometry.setCourse(WorkoutGameRoadCourse());
        QVERIFY(!geometry.ready());
        QVERIFY(geometry.vertexData().isEmpty());
        QVERIFY(geometry.indexData().isEmpty());
    }

    void trailHasStableIndexedVertexLayout()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(12.0));

        QVERIFY(geometry.ready());
        QCOMPARE(geometry.stride(), 48);
        QCOMPARE(geometry.attributeCount(), 5);
        QCOMPARE(geometry.primitiveType(),
                 QQuick3DGeometry::PrimitiveType::Triangles);
        QCOMPARE(geometry.vertexData().size(),
                 geometry.sampleCount() * 2 * geometry.stride());
        QCOMPARE(geometry.indexData().size(),
                 (geometry.sampleCount() - 1) * 6 * int(sizeof(quint32)));
        QCOMPARE(geometry.attribute(0).semantic,
                 QQuick3DGeometry::Attribute::PositionSemantic);
        QCOMPARE(geometry.attribute(4).semantic,
                 QQuick3DGeometry::Attribute::IndexSemantic);
    }

    void trailPreservesAcceptedRiderScale()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(12.0));

        QVERIFY(geometry.ready());
        const float leftX = vertexFloat(
                geometry.vertexData(), geometry.stride(), 0, 0);
        const float rightX = vertexFloat(
                geometry.vertexData(), geometry.stride(), 1, 0);
        QVERIFY(std::abs(leftX + 0.68f) < 0.001f);
        QVERIFY(std::abs(rightX - 0.68f) < 0.001f);
    }

    void floorIsWiderAndBelowTrail()
    {
        WorkoutGame3DGeometry trail(
                WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        const WorkoutGameRoadCourse course = straightCourse(8.0);
        trail.setCourse(course);
        floor.setCourse(course);

        const float trailLeftX = vertexFloat(
                trail.vertexData(), trail.stride(), 0, 0);
        const float floorLeftX = vertexFloat(
                floor.vertexData(), floor.stride(), 0, 0);
        const float trailY = vertexFloat(
                trail.vertexData(), trail.stride(), 0, sizeof(float));
        const float floorY = vertexFloat(
                floor.vertexData(), floor.stride(), 3, sizeof(float));
        QVERIFY(std::abs(floorLeftX) > std::abs(trailLeftX) + 12.0f);
        QVERIFY(floorY < trailY - 0.02f);
        QCOMPARE(floor.vertexData().size(),
                 floor.sampleCount() * 8 * floor.stride());
        QCOMPARE(floor.indexData().size(),
                 (floor.sampleCount() - 1) * 42 * int(sizeof(quint32)));
    }

    void forestFloorShouldersJoinBothTrailEdges()
    {
        WorkoutGame3DGeometry trail(
                WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        const WorkoutGameRoadCourse course = straightCourse(12.0);
        trail.setCourse(course);
        floor.setCourse(course);

        const float trailLeftX = vertexFloat(
                trail.vertexData(), trail.stride(), 0, 0);
        const float trailRightX = vertexFloat(
                trail.vertexData(), trail.stride(), 1, 0);
        const float trailY = vertexFloat(
                trail.vertexData(), trail.stride(), 0, sizeof(float));
        const float floorLeftX = vertexFloat(
                floor.vertexData(), floor.stride(), 3, 0);
        const float floorRightX = vertexFloat(
                floor.vertexData(), floor.stride(), 4, 0);
        const float floorLeftY = vertexFloat(
                floor.vertexData(), floor.stride(), 3, sizeof(float));
        const float floorRightY = vertexFloat(
                floor.vertexData(), floor.stride(), 4, sizeof(float));

        QVERIFY(std::abs(trailLeftX - floorLeftX) < 0.001f);
        QVERIFY(std::abs(trailRightX - floorRightX) < 0.001f);
        QVERIFY(std::abs(trailY - floorLeftY) < 0.04f);
        QVERIFY(std::abs(trailY - floorRightY) < 0.04f);
    }

    void bermMeshAndForestUseTheSameBankedEdgeSockets()
    {
        const WorkoutGameRoadCourse course = bermCourse();
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(piece != course.pieces.end());
        QVERIFY(!piece->challenge.enabled);
        const double center = piece->geometryAnchorDistanceMeters;
        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        WorkoutGame3DGeometry berm(WorkoutGame3DGeometry::Layer::Berm);
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        trail.setCourse(course);
        berm.setCourse(course);
        floor.setCourse(course);
        QVERIFY(trail.ready() && berm.ready() && floor.ready());

        int bermRow = -1;
        for (int row = 0; row < berm.sampleCount(); ++row) {
            const double distance = vertexFloat(
                    berm.vertexData(), berm.stride(), row * 7, 44) / 0.22;
            if (std::abs(distance - center) < 0.001) bermRow = row;
        }
        int floorRow = -1;
        for (int row = 0; row < floor.sampleCount(); ++row) {
            const double distance = vertexFloat(
                    floor.vertexData(), floor.stride(), row * 8, 44) / 0.22;
            if (std::abs(distance - center) < 0.001) floorRow = row;
        }
        int trailRow = -1;
        for (int row = 0; row < trail.sampleCount(); ++row) {
            const double distance = vertexFloat(
                    trail.vertexData(), trail.stride(), row * 2, 44) / 0.22;
            if (std::abs(distance - center) < 0.001) trailRow = row;
        }
        QVERIFY(bermRow >= 0 && floorRow >= 0 && trailRow >= 0);
        const float trailLeftY = vertexFloat(
                berm.vertexData(), berm.stride(), bermRow * 7 + 1, 4);
        const float trailRightY = vertexFloat(
                berm.vertexData(), berm.stride(), bermRow * 7 + 5, 4);
        const float floorLeftY = vertexFloat(
                floor.vertexData(), floor.stride(), floorRow * 8 + 3, 4);
        const float floorRightY = vertexFloat(
                floor.vertexData(), floor.stride(), floorRow * 8 + 4, 4);
        const float backingY = vertexFloat(
                trail.vertexData(), trail.stride(), trailRow * 2, 4);
        QVERIFY(std::abs(trailLeftY - trailRightY) > 0.60f);
        QVERIFY2(backingY < std::min(trailLeftY, trailRightY) - 0.01f,
                 "berm backing can occlude or z-fight the authored bank");
        QVERIFY(std::abs(trailLeftY - floorLeftY - 0.035f) < 0.002f);
        QVERIFY(std::abs(trailRightY - floorRightY - 0.035f) < 0.002f);

        const WorkoutGameRoadSample road =
                WorkoutGameRoadCourseBuilder::sample(course, center);
        QVERIFY(road.ready);
        QVERIFY(std::abs(road.bermBankRadians) > 0.30);
        QVERIFY((trailLeftY - trailRightY) * piece->turnRadians > 0.0);
        QCOMPARE(berm.vertexData().size(),
                 berm.sampleCount() * 7 * berm.stride());
        QVERIFY(berm.sampleCount() >= 52);
        QVERIFY(berm.indexData().size()
                / int(3 * sizeof(quint32)) <= 1024);
        QCOMPARE(trail.indexData().size(),
                 (trail.sampleCount() - 1) * 6 * int(sizeof(quint32)));
        QVERIFY(floor.indexData().size()
                < (floor.sampleCount() - 1) * 42 * int(sizeof(quint32)));
    }

    void bermDoesNotCreateASeparateBypassRibbon()
    {
        WorkoutGame3DGeometry bypass(
                WorkoutGame3DGeometry::Layer::Bypass);
        bypass.setCourse(bermCourse());
        QVERIFY(!bypass.ready());
        QCOMPARE(bypass.sampleCount(), 0);
        QVERIFY(bypass.vertexData().isEmpty());
        QVERIFY(bypass.indexData().isEmpty());
    }

    void forestFloorHasReliefMaterialsAndUnitNormals()
    {
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        floor.setCourse(straightCourse(24.0));
        QVERIFY(floor.ready());

        const int base = (floor.sampleCount() / 2) * 8;
        float minimumY = 1000.0f;
        float maximumY = -1000.0f;
        for (int vertex = 0; vertex < 8; ++vertex) {
            const float y = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex,
                    sizeof(float));
            const float nx = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 12);
            const float ny = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 16);
            const float nz = vertexFloat(
                    floor.vertexData(), floor.stride(), base + vertex, 20);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            QVERIFY(ny > 0.0f);
            QVERIFY(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f)
                    < 0.001f);
        }
        QVERIFY(maximumY - minimumY > 0.30f);
        float colorDistance = 0.0f;
        for (int component = 0; component < 3; ++component) {
            colorDistance += std::abs(vertexFloat(
                    floor.vertexData(), floor.stride(), base,
                    24 + component * int(sizeof(float))) - vertexFloat(
                    floor.vertexData(), floor.stride(), base + 3,
                    24 + component * int(sizeof(float))));
        }
        QVERIFY(colorDistance > 0.15f);
    }

    void forestDressingBuildsDenseDeterministicBatchedTreeLine()
    {
        const WorkoutGameRoadCourse course = straightCourse(160.0, 5.0);
        const WorkoutGame3DMeshData first =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, 145.0);
        const WorkoutGame3DMeshData second =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, 145.0);

        QVERIFY(first.ready);
        QVERIFY(first.sampleCount >= 80);
        QVERIFY(first.triangleCount() >= first.sampleCount * 4);
        QVERIFY(first.triangleCount() <= 5200);
        QVERIFY(first.boundsMin.x() < -6.0f);
        QVERIFY(first.boundsMax.x() > 6.0f);
        QVERIFY(first.boundsMax.y() - first.boundsMin.y() > 4.0f);
        QVERIFY(first.boundsMin.z() >= -0.1f);
        QVERIFY(first.boundsMax.z() <= 145.1f);
        QCOMPARE(first.sampleCount, second.sampleCount);
        QCOMPARE(first.vertexData, second.vertexData);
        QCOMPARE(first.indexData, second.indexData);
    }

    void forestDressingIncludesRocksStumpsAndShrubsInTheSameBatch()
    {
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(160.0, 5.0), 0.0, 145.0);
        QVERIFY(dressing.ready);

        const auto hasColor = [&dressing](float red, float green, float blue) {
            const int vertexCount = dressing.vertexData.size()
                    / int(12 * sizeof(float));
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                if (std::abs(vertexFloat(
                            dressing.vertexData, 12 * int(sizeof(float)),
                            vertex, 24) - red) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, 12 * int(sizeof(float)),
                            vertex, 28) - green) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, 12 * int(sizeof(float)),
                            vertex, 32) - blue) < 0.002f) {
                    return true;
                }
            }
            return false;
        };

        QVERIFY2(hasColor(0.34f, 0.35f, 0.31f),
                 "forest batch has no decorative rocks");
        QVERIFY2(hasColor(0.29f, 0.17f, 0.08f),
                 "forest batch has no cut or broken stumps");
        QVERIFY2(hasColor(0.09f, 0.29f, 0.12f),
                 "forest batch has no shrub clusters");
        QVERIFY2(dressing.triangleCount() <= 5200,
                 "forest variety exceeds the bounded batch budget");
    }

    void forestDressingIncludesRecognizableBirchVariation()
    {
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(160.0), 0.0, 145.0);
        QVERIFY(dressing.ready);

        constexpr float BirchTrunk[] = {0.82f, 0.83f, 0.75f};
        constexpr float BirchBark[] = {0.18f, 0.17f, 0.14f};
        constexpr float BirchCrown[] = {0.31f, 0.49f, 0.18f};
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int trunkVertices = 0;
        int barkVertices = 0;
        int crownVertices = 0;
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const auto matches = [&](const float color[3]) {
                return std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 24)
                        - color[0]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 28)
                        - color[1]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 32)
                        - color[2]) < 0.002f;
            };
            if (!matches(BirchTrunk) && !matches(BirchBark)
                    && !matches(BirchCrown)) {
                continue;
            }
            QCOMPARE(vertexFloat(
                    dressing.vertexData, stride, vertex, 36), 1.0f);
            const float x = vertexFloat(
                    dressing.vertexData, stride, vertex, 0);
            const float y = vertexFloat(
                    dressing.vertexData, stride, vertex, 4);
            QVERIFY2(std::abs(x) >= 9.0f,
                     "birch entered the protected camera corridor");
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            trunkVertices += matches(BirchTrunk) ? 1 : 0;
            barkVertices += matches(BirchBark) ? 1 : 0;
            crownVertices += matches(BirchCrown) ? 1 : 0;
        }
        QVERIFY2(trunkVertices >= 16, "forest batch has no birch trunks");
        QVERIFY2(barkVertices >= 8, "birch trunks have no dark bark marks");
        QVERIFY2(crownVertices >= 16, "forest batch has no birch crowns");
        QVERIFY2(maximumY - minimumY >= 3.0f,
                 "birch silhouette is not recognizably tree-sized");
        QVERIFY(dressing.triangleCount() <= 5200);
    }

    void forestDressingIncludesGroundedSaplings()
    {
        const WorkoutGameRoadCourse course = straightCourse(160.0);
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, 145.0);
        QVERIFY(dressing.ready);

        constexpr float SaplingTrunk[] = {0.34f, 0.23f, 0.11f};
        constexpr float SaplingLeaves[] = {0.18f, 0.43f, 0.16f};
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int trunkVertices = 0;
        int leafVertices = 0;
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const auto matches = [&](const float color[3]) {
                return std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 24)
                        - color[0]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 28)
                        - color[1]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 32)
                        - color[2]) < 0.002f;
            };
            if (!matches(SaplingTrunk) && !matches(SaplingLeaves)) continue;
            QCOMPARE(vertexFloat(
                    dressing.vertexData, stride, vertex, 36), 1.0f);
            const float x = vertexFloat(
                    dressing.vertexData, stride, vertex, 0);
            const float y = vertexFloat(
                    dressing.vertexData, stride, vertex, 4);
            QVERIFY2(std::abs(x) >= 9.0f,
                     "sapling entered the protected camera corridor");
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            trunkVertices += matches(SaplingTrunk) ? 1 : 0;
            leafVertices += matches(SaplingLeaves) ? 1 : 0;
        }
        QVERIFY2(trunkVertices >= 8, "forest batch has no sapling stems");
        QVERIFY2(leafVertices >= 12, "forest batch has no sapling foliage");
        QVERIFY2(maximumY - minimumY >= 1.0f,
                 "saplings have no measurable upright silhouette");
        QVERIFY2(maximumY - minimumY <= 3.0f,
                 "saplings are indistinguishable from mature trees");
        QVERIFY(dressing.triangleCount() <= 5200);
    }

    void forestDressingIncludesFallenDeadTimber()
    {
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(160.0), 0.0, 145.0);
        QVERIFY(dressing.ready);

        constexpr float DeadWood[] = {0.31f, 0.20f, 0.11f};
        constexpr float CutWood[] = {0.52f, 0.37f, 0.19f};
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int woodVertices = 0;
        int cutVertices = 0;
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = std::numeric_limits<float>::lowest();
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const auto matches = [&](const float color[3]) {
                return std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 24)
                        - color[0]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 28)
                        - color[1]) < 0.002f
                        && std::abs(vertexFloat(
                            dressing.vertexData, stride, vertex, 32)
                        - color[2]) < 0.002f;
            };
            if (!matches(DeadWood) && !matches(CutWood)) continue;
            QCOMPARE(vertexFloat(
                    dressing.vertexData, stride, vertex, 36), 1.0f);
            const float y = vertexFloat(
                    dressing.vertexData, stride, vertex, 4);
            const float z = vertexFloat(
                    dressing.vertexData, stride, vertex, 8);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            minimumZ = std::min(minimumZ, z);
            maximumZ = std::max(maximumZ, z);
            woodVertices += matches(DeadWood) ? 1 : 0;
            cutVertices += matches(CutWood) ? 1 : 0;
        }
        QVERIFY2(woodVertices >= 12, "forest batch has no fallen timber");
        QVERIFY2(cutVertices >= 4,
                 "fallen timber has no recognizable cut ends");
        QVERIFY2(maximumZ - minimumZ >= 2.0f,
                 "fallen timber has no measurable horizontal extent");
        QVERIFY2(maximumY - minimumY <= 1.5f,
                 "fallen timber is not a low ground prop");
        QVERIFY(dressing.triangleCount() <= 5200);
    }

    void forestDressingIncludesSparseGroundVegetation()
    {
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(160.0), 0.0, 145.0);
        QVERIFY(dressing.ready);

        constexpr float GroundLeaves[] = {0.16f, 0.38f, 0.10f};
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int vegetationVertices = 0;
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const bool vegetation =
                    std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 24)
                            - GroundLeaves[0]) < 0.002f
                    && std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 28)
                            - GroundLeaves[1]) < 0.002f
                    && std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 32)
                            - GroundLeaves[2]) < 0.002f;
            if (!vegetation) continue;
            QCOMPARE(vertexFloat(
                    dressing.vertexData, stride, vertex, 36), 1.0f);
            const float y = vertexFloat(
                    dressing.vertexData, stride, vertex, 4);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            ++vegetationVertices;
        }
        QVERIFY2(vegetationVertices >= 12,
                 "forest batch has no sparse ground vegetation");
        QVERIFY2(vegetationVertices < vertexCount / 8,
                 "ground vegetation is no longer sparse");
        QVERIFY2(maximumY - minimumY <= 1.0f,
                 "ground vegetation is too tall for forest-floor dressing");
        QVERIFY(dressing.triangleCount() <= 5200);
    }

    void forestDressingKeepsStreamingOverlapByteStable()
    {
        const WorkoutGameRoadCourse course = straightCourse(170.0);
        const WorkoutGame3DMeshData first =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, 110.0);
        const WorkoutGame3DMeshData second =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 40.0, 145.0);
        QVERIFY(first.ready);
        QVERIFY(second.ready);

        constexpr float InteriorStartMeters = 55.0f;
        constexpr float InteriorEndMeters = 95.0f;
        const int stride = 12 * int(sizeof(float));
        const auto overlapVertices = [&](const WorkoutGame3DMeshData &mesh) {
            QByteArray overlap;
            const int vertexCount = mesh.vertexData.size() / stride;
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                const float z = vertexFloat(
                        mesh.vertexData, stride, vertex, 8);
                if (z < InteriorStartMeters || z > InteriorEndMeters) {
                    continue;
                }
                overlap.append(mesh.vertexData.constData() + vertex * stride,
                               stride);
            }
            return overlap;
        };
        const QByteArray firstOverlap = overlapVertices(first);
        const QByteArray secondOverlap = overlapVertices(second);
        QVERIFY(!firstOverlap.isEmpty());
        QCOMPARE(firstOverlap, secondOverlap);
        QVERIFY(first.triangleCount() <= 5200);
        QVERIFY(second.triangleCount() <= 5200);
    }

    void forestDressingBasesFollowTheGroundSurface()
    {
        const WorkoutGameRoadCourse course = straightCourse(160.0, 9.0);
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, 145.0);
        QVERIFY(dressing.ready);

        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        const auto hasColor = [&](int vertex, float red, float green,
                                  float blue) {
            return std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 24) - red)
                            < 0.002f
                    && std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 28) - green)
                            < 0.002f
                    && std::abs(vertexFloat(
                        dressing.vertexData, stride, vertex, 32) - blue)
                            < 0.002f;
        };
        const auto groundAt = [&course](float x, float z) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(course, z);
            if (!sample.ready) return std::numeric_limits<double>::quiet_NaN();
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                        sample, z, course.seed);
            return WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, x);
        };
        const auto verifyBase = [&](int vertex, double maximumEmbed) {
            const float x = vertexFloat(
                    dressing.vertexData, stride, vertex, 0);
            const float y = vertexFloat(
                    dressing.vertexData, stride, vertex, 4);
            const float z = vertexFloat(
                    dressing.vertexData, stride, vertex, 8);
            const double ground = groundAt(x, z);
            QVERIFY(std::isfinite(ground));
            QVERIFY2(y <= ground + 0.012,
                     "forest prop base floats above the terrain");
            QVERIFY2(y >= ground - maximumEmbed,
                     "forest prop base is visibly buried in the terrain");
        };

        int groundedBaseVertices = 0;
        for (int vertex = 0; vertex < vertexCount;) {
            if (hasColor(vertex, 0.22f, 0.13f, 0.07f)) {
                QVERIFY(vertex + 3 < vertexCount);
                verifyBase(vertex, 0.04);
                verifyBase(vertex + 1, 0.04);
                groundedBaseVertices += 2;
                vertex += 4;
            } else if (hasColor(vertex, 0.82f, 0.83f, 0.75f)) {
                QVERIFY(vertex + 3 < vertexCount);
                verifyBase(vertex, 0.04);
                verifyBase(vertex + 1, 0.04);
                groundedBaseVertices += 2;
                vertex += 4;
            } else if (hasColor(vertex, 0.34f, 0.35f, 0.31f)) {
                QVERIFY(vertex + 4 < vertexCount);
                for (int corner = 0; corner < 4; ++corner) {
                    verifyBase(vertex + corner, 0.07);
                    ++groundedBaseVertices;
                }
                vertex += 5;
            } else if (hasColor(vertex, 0.29f, 0.17f, 0.08f)) {
                QVERIFY(vertex + 12 < vertexCount);
                for (int side = 0; side < 6; ++side) {
                    verifyBase(vertex + side, 0.035);
                    ++groundedBaseVertices;
                }
                vertex += 13;
            } else if (hasColor(vertex, 0.09f, 0.29f, 0.12f)) {
                QVERIFY(vertex + 9 < vertexCount);
                for (const int baseVertex : {0, 1, 5, 6}) {
                    verifyBase(vertex + baseVertex, 0.035);
                    ++groundedBaseVertices;
                }
                vertex += 10;
            } else if (hasColor(vertex, 0.34f, 0.23f, 0.11f)) {
                QVERIFY(vertex + 3 < vertexCount);
                verifyBase(vertex, 0.03);
                verifyBase(vertex + 1, 0.03);
                groundedBaseVertices += 2;
                vertex += 4;
            } else if (hasColor(vertex, 0.31f, 0.20f, 0.11f)) {
                QVERIFY(vertex + 7 < vertexCount);
                for (int corner = 0; corner < 4; ++corner) {
                    verifyBase(vertex + corner, 0.03);
                    ++groundedBaseVertices;
                }
                vertex += 8;
            } else if (hasColor(vertex, 0.16f, 0.38f, 0.10f)) {
                QVERIFY(vertex + 2 < vertexCount);
                verifyBase(vertex, 0.02);
                verifyBase(vertex + 1, 0.02);
                groundedBaseVertices += 2;
                vertex += 3;
            } else {
                ++vertex;
            }
        }
        QVERIFY(groundedBaseVertices > 100);
    }

    void forestDressingKeepsEntireTreesOutsideTheCameraCorridor()
    {
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(160.0), 0.0, 145.0);
        QVERIFY(dressing.ready);

        constexpr float CameraCorridorHalfWidthMeters = 9.0f;
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int trunkVertices = 0;
        int crownVertices = 0;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const float red = vertexFloat(
                    dressing.vertexData, stride, vertex, 24);
            const float green = vertexFloat(
                    dressing.vertexData, stride, vertex, 28);
            const float blue = vertexFloat(
                    dressing.vertexData, stride, vertex, 32);
            const bool crown = red >= 0.05f && red <= 0.08f
                    && green >= 0.18f && green <= 0.26f
                    && blue >= 0.09f && blue <= 0.14f;
            const bool trunk = std::abs(red - 0.22f) < 0.002f
                    && std::abs(green - 0.13f) < 0.002f
                    && std::abs(blue - 0.07f) < 0.002f;
            if (!crown && !trunk) continue;
            if (crown) ++crownVertices;
            if (trunk) ++trunkVertices;
            const float x = vertexFloat(
                    dressing.vertexData, stride, vertex, 0);
            QVERIFY2(std::abs(x) >= CameraCorridorHalfWidthMeters,
                     "foreground tree obstructs the rider view");
        }
        QVERIFY(trunkVertices > 100);
        QVERIFY(crownVertices > 100);
    }

    void forestDressingDoesNotClampPropsToStreamingRangeEdges()
    {
        constexpr double StartDistanceMeters = 10.0;
        constexpr double EndDistanceMeters = 155.0;
        constexpr float EdgeInsetMeters = 1.5f;
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    straightCourse(180.0),
                    StartDistanceMeters, EndDistanceMeters);
        QVERIFY(dressing.ready);

        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        int verticesPinnedToRangeEdge = 0;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const float z = vertexFloat(
                    dressing.vertexData, stride, vertex, 8);
            if (std::abs(z - float(StartDistanceMeters) - EdgeInsetMeters)
                        < 0.0001f
                    || std::abs(z - float(EndDistanceMeters)
                                + EdgeInsetMeters) < 0.0001f) {
                ++verticesPinnedToRangeEdge;
            }
        }
        QCOMPARE(verticesPinnedToRangeEdge, 0);
    }

    void forestPropsStayOutsideFeatureAndBypassCorridor()
    {
        const WorkoutGameRoadCourse course = tabletopCourse();
        const auto challenge = std::find_if(
                course.pieces.cbegin(), course.pieces.cend(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled;
                });
        QVERIFY(challenge != course.pieces.cend());
        const WorkoutGame3DMeshData dressing =
                WorkoutGame3DGeometry::buildMeshData(
                    WorkoutGame3DGeometry::Layer::ForestDressing,
                    course, 0.0, course.totalLengthMeters);
        QVERIFY(dressing.ready);

        const double protectedStart = std::min(
                challenge->challenge.prepareDistanceMeters,
                challenge->challenge.bypassStartDistanceMeters);
        const double protectedEnd = std::max(
                challenge->challenge.bypassEndDistanceMeters,
                challenge->challenge.obstacleDistanceMeters);
        int propVertices = 0;
        const int stride = 12 * int(sizeof(float));
        const int vertexCount = dressing.vertexData.size() / stride;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            const float red = vertexFloat(
                    dressing.vertexData, stride, vertex, 24);
            const float green = vertexFloat(
                    dressing.vertexData, stride, vertex, 28);
            const float blue = vertexFloat(
                    dressing.vertexData, stride, vertex, 32);
            const bool prop =
                    (std::abs(red - 0.34f) < 0.002f
                     && std::abs(green - 0.35f) < 0.002f
                     && std::abs(blue - 0.31f) < 0.002f)
                    || (std::abs(red - 0.29f) < 0.002f
                        && std::abs(green - 0.17f) < 0.002f
                        && std::abs(blue - 0.08f) < 0.002f)
                    || (std::abs(red - 0.09f) < 0.002f
                        && std::abs(green - 0.29f) < 0.002f
                        && std::abs(blue - 0.12f) < 0.002f)
                    || (std::abs(red - 0.34f) < 0.002f
                        && std::abs(green - 0.23f) < 0.002f
                        && std::abs(blue - 0.11f) < 0.002f)
                    || (std::abs(red - 0.18f) < 0.002f
                        && std::abs(green - 0.43f) < 0.002f
                        && std::abs(blue - 0.16f) < 0.002f)
                    || (std::abs(red - 0.31f) < 0.002f
                        && std::abs(green - 0.20f) < 0.002f
                        && std::abs(blue - 0.11f) < 0.002f)
                    || (std::abs(red - 0.52f) < 0.002f
                        && std::abs(green - 0.37f) < 0.002f
                        && std::abs(blue - 0.19f) < 0.002f)
                    || (std::abs(red - 0.16f) < 0.002f
                        && std::abs(green - 0.38f) < 0.002f
                        && std::abs(blue - 0.10f) < 0.002f);
            if (!prop) continue;
            ++propVertices;
            const double distance = vertexFloat(
                    dressing.vertexData, stride, vertex, 8);
            QVERIFY2(distance < protectedStart || distance > protectedEnd,
                     "forest prop entered the feature or bypass corridor");
        }
        QVERIFY(propVertices > 0);
    }

    void forestPropsStayOutsideRollableFeatureGeometry()
    {
        const auto featureSpan = [](WorkoutGameTerrainKind terrain,
                                    double difficulty) {
            switch (terrain) {
            case WorkoutGameTerrainKind::Roots: {
                const auto profile = WorkoutGameRootGeometry::profile(
                        difficulty);
                return std::pair<double, double>{
                    profile.startMeters, profile.endMeters};
            }
            case WorkoutGameTerrainKind::RockGarden: {
                const auto profile = WorkoutGameRockGardenGeometry::profile(
                        difficulty);
                return std::pair<double, double>{
                    profile.startMeters, profile.endMeters};
            }
            case WorkoutGameTerrainKind::RockSlab: {
                const auto profile = WorkoutGameRockSlabGeometry::profile(
                        difficulty);
                return std::pair<double, double>{
                    profile.startMeters, profile.endMeters};
            }
            case WorkoutGameTerrainKind::Skinny: {
                const auto profile = WorkoutGameSkinnyGeometry::profile(
                        difficulty);
                return std::pair<double, double>{
                    profile.startMeters, profile.endMeters};
            }
            default:
                return std::pair<double, double>{0.0, 0.0};
            }
        };
        const auto isPropColor = [](float red, float green, float blue) {
            return (std::abs(red - 0.34f) < 0.002f
                    && std::abs(green - 0.35f) < 0.002f
                    && std::abs(blue - 0.31f) < 0.002f)
                    || (std::abs(red - 0.29f) < 0.002f
                        && std::abs(green - 0.17f) < 0.002f
                        && std::abs(blue - 0.08f) < 0.002f)
                    || (std::abs(red - 0.09f) < 0.002f
                        && std::abs(green - 0.29f) < 0.002f
                        && std::abs(blue - 0.12f) < 0.002f)
                    || (std::abs(red - 0.34f) < 0.002f
                        && std::abs(green - 0.23f) < 0.002f
                        && std::abs(blue - 0.11f) < 0.002f)
                    || (std::abs(red - 0.18f) < 0.002f
                        && std::abs(green - 0.43f) < 0.002f
                        && std::abs(blue - 0.16f) < 0.002f)
                    || (std::abs(red - 0.31f) < 0.002f
                        && std::abs(green - 0.20f) < 0.002f
                        && std::abs(blue - 0.11f) < 0.002f)
                    || (std::abs(red - 0.52f) < 0.002f
                        && std::abs(green - 0.37f) < 0.002f
                        && std::abs(blue - 0.19f) < 0.002f)
                    || (std::abs(red - 0.16f) < 0.002f
                        && std::abs(green - 0.38f) < 0.002f
                        && std::abs(blue - 0.10f) < 0.002f);
        };

        for (const WorkoutGameTerrainKind terrain : {
                 WorkoutGameTerrainKind::Roots,
                 WorkoutGameTerrainKind::RockGarden,
                 WorkoutGameTerrainKind::RockSlab,
                 WorkoutGameTerrainKind::Skinny}) {
            WorkoutGameRoadCourse course = straightCourse(120.0);
            WorkoutGameRoadPiece &piece = course.pieces.front();
            piece.terrain = terrain;
            piece.difficulty = 0.65;
            piece.geometryAnchorDistanceMeters = 50.0;
            piece.challenge.enabled = true;
            piece.challenge.prepareDistanceMeters = 40.0;
            piece.challenge.bypassStartDistanceMeters = 45.0;
            piece.challenge.bypassEndDistanceMeters = 45.0;
            piece.challenge.obstacleDistanceMeters = 50.0;
            const auto [featureStart, featureEnd] = featureSpan(
                    terrain, piece.difficulty);
            const double protectedStart = std::min(
                    piece.challenge.prepareDistanceMeters,
                    piece.challenge.obstacleDistanceMeters + featureStart)
                    - 2.0;
            const double protectedEnd = std::max(
                    piece.challenge.bypassEndDistanceMeters,
                    piece.challenge.obstacleDistanceMeters + featureEnd)
                    + 2.0;

            const WorkoutGame3DMeshData dressing =
                    WorkoutGame3DGeometry::buildMeshData(
                        WorkoutGame3DGeometry::Layer::ForestDressing,
                        course, 0.0, course.totalLengthMeters);
            QVERIFY(dressing.ready);
            int propVertices = 0;
            const int stride = 12 * int(sizeof(float));
            const int vertexCount = dressing.vertexData.size() / stride;
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                const float red = vertexFloat(
                        dressing.vertexData, stride, vertex, 24);
                const float green = vertexFloat(
                        dressing.vertexData, stride, vertex, 28);
                const float blue = vertexFloat(
                        dressing.vertexData, stride, vertex, 32);
                if (!isPropColor(red, green, blue)) continue;
                ++propVertices;
                const double distance = vertexFloat(
                        dressing.vertexData, stride, vertex, 8);
                QVERIFY2(distance < protectedStart || distance > protectedEnd,
                         "forest prop entered rollable feature geometry");
            }
            QVERIFY(propVertices > 0);
        }
    }

    void risingCourseExpandsVerticalBounds()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(50.0, 6.0));

        QVERIFY(geometry.ready());
        QVERIFY(geometry.boundsMax().y() > geometry.boundsMin().y() + 5.5f);
        QVERIFY(geometry.boundsMax().z() > 49.0f);
    }

    void longCourseHasBoundedSampleCount()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(straightCourse(20000.0));

        QVERIFY(geometry.ready());
        QCOMPARE(geometry.sampleCount(), 16000);
        QVERIFY(geometry.vertexData().size() < 2 * 1024 * 1024);
        QVERIFY(geometry.indexData().size() < 512 * 1024);
    }

    void denseFeatureCourseKeepsTrailSamplesContinuous()
    {
        constexpr int PieceCount = 4000;
        constexpr double PieceLengthMeters = 5.0;
        constexpr int GapPieceIndex = 3000;
        constexpr double GapStartMeters =
                GapPieceIndex * PieceLengthMeters + 1.0;
        constexpr double GapEndMeters =
                GapPieceIndex * PieceLengthMeters + 4.0;
        WorkoutGameRoadCourse course;
        course.ready = true;
        course.seed = 0x51a17u;
        course.totalLengthMeters = PieceCount * PieceLengthMeters;
        course.visualLengthMeters = course.totalLengthMeters;
        course.pieces.reserve(PieceCount);
        for (int index = 0; index < PieceCount; ++index) {
            const double start = double(index) * PieceLengthMeters;
            WorkoutGameRoadPiece piece;
            piece.terrain = WorkoutGameTerrainKind::LogOver;
            piece.startDistanceMeters = start;
            piece.lengthMeters = PieceLengthMeters;
            piece.difficulty = 1.0;
            piece.geometryAnchorDistanceMeters = start
                    + PieceLengthMeters * 0.5;
            piece.entry.zMeters = start;
            piece.exit.zMeters = start + PieceLengthMeters;
            piece.challenge.enabled = true;
            piece.challenge.obstacleDistanceMeters =
                    piece.geometryAnchorDistanceMeters;
            if (index == GapPieceIndex) {
                piece.gapJump.enabled = true;
                piece.gapJump.splitStartDistanceMeters = GapStartMeters;
                piece.gapJump.mergeEndDistanceMeters = GapEndMeters;
                piece.gapJump.lines.front().takeoffDistanceMeters =
                        GapStartMeters + 0.5;
                for (WorkoutGameRoadGapJumpLine &line
                        : piece.gapJump.lines) {
                    line.landingDistanceMeters = GapEndMeters - 0.5;
                }
            }
            course.pieces.push_back(piece);
        }

        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(course);

        QVERIFY(geometry.ready());
        QVERIFY(geometry.sampleCount() <= 16000);
        QVERIFY(geometry.sampleCount() >= 15000);
        double maximumGapMeters = 0.0;
        bool sampledGapStart = false;
        bool sampledGapEnd = false;
        double previousDistanceMeters = vertexFloat(
                geometry.vertexData(), geometry.stride(), 0, 8);
        for (int sample = 1; sample < geometry.sampleCount(); ++sample) {
            const double distanceMeters = vertexFloat(
                    geometry.vertexData(), geometry.stride(), sample * 2, 8);
            maximumGapMeters = std::max(
                    maximumGapMeters,
                    distanceMeters - previousDistanceMeters);
            sampledGapStart = sampledGapStart
                    || std::abs(distanceMeters - GapStartMeters) < 0.001;
            sampledGapEnd = sampledGapEnd
                    || std::abs(distanceMeters - GapEndMeters) < 0.001;
            previousDistanceMeters = distanceMeters;
        }
        QVERIFY2(maximumGapMeters <= 3.0,
                 qPrintable(QStringLiteral(
                     "dense feature sampling left a %1 m trail gap")
                     .arg(maximumGapMeters)));
        QVERIFY(sampledGapStart);
        QVERIFY(sampledGapEnd);
        const int indexCount = geometry.indexData().size()
                / int(sizeof(quint32));
        for (int index = 0; index < indexCount; index += 3) {
            double minimumDistanceMeters =
                    std::numeric_limits<double>::infinity();
            double maximumDistanceMeters =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        geometry.indexData(), index + corner));
                const double distanceMeters = vertexFloat(
                        geometry.vertexData(), geometry.stride(), vertex, 8);
                minimumDistanceMeters = std::min(
                        minimumDistanceMeters, distanceMeters);
                maximumDistanceMeters = std::max(
                        maximumDistanceMeters, distanceMeters);
            }
            QVERIFY2(!(minimumDistanceMeters < GapStartMeters
                       && maximumDistanceMeters >= GapEndMeters),
                     "trail triangles bridged the gap-jump opening");
        }
    }

    void maximumPieceBermCourseStillBuildsTrailGeometry()
    {
        constexpr int PieceCount = 4096;
        constexpr double PieceLengthMeters = 5.0;
        WorkoutGameRoadCourse course;
        course.ready = true;
        course.seed = 0xb3a5u;
        course.totalLengthMeters = PieceCount * PieceLengthMeters;
        course.visualLengthMeters = course.totalLengthMeters;
        course.pieces.reserve(PieceCount);
        for (int index = 0; index < PieceCount; ++index) {
            const double start = double(index) * PieceLengthMeters;
            WorkoutGameRoadPiece piece;
            piece.terrain = WorkoutGameTerrainKind::Berm;
            piece.startDistanceMeters = start;
            piece.lengthMeters = PieceLengthMeters;
            piece.geometryAnchorDistanceMeters =
                    start + PieceLengthMeters * 0.5;
            piece.entry.zMeters = start;
            piece.exit.zMeters = start + PieceLengthMeters;
            piece.turnRadians = index % 2 == 0 ? 0.55 : -0.55;
            piece.bank.enabled = true;
            piece.bank.startDistanceMeters = start + 0.20;
            piece.bank.curveStartDistanceMeters = start + 0.75;
            piece.bank.curveEndDistanceMeters =
                    start + PieceLengthMeters - 0.75;
            piece.bank.endDistanceMeters = start + PieceLengthMeters - 0.20;
            piece.bank.maximumBankRadians = 0.30;
            course.pieces.push_back(piece);
        }

        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        trail.setCourse(course);

        QVERIFY2(trail.ready(),
                 "maximum valid berm plan produced no trail geometry");
        QVERIFY(trail.sampleCount() <= 16000);
        QVERIFY(trail.boundsMax().z() > course.totalLengthMeters - 1.0);
    }

    void maximumPieceDropCourseDoesNotBridgeAirGaps()
    {
        constexpr int PieceCount = 4096;
        constexpr double PieceLengthMeters = 30.0;
        WorkoutGameRoadCourse course;
        course.ready = true;
        course.seed = 0xd20fu;
        course.totalLengthMeters = PieceCount * PieceLengthMeters;
        course.visualLengthMeters = course.totalLengthMeters;
        course.pieces.reserve(PieceCount);
        for (int index = 0; index < PieceCount; ++index) {
            const double start = double(index) * PieceLengthMeters;
            WorkoutGameRoadPiece piece;
            piece.terrain = WorkoutGameTerrainKind::Drop;
            piece.startDistanceMeters = start;
            piece.lengthMeters = PieceLengthMeters;
            piece.difficulty = 0.65;
            piece.geometryAnchorDistanceMeters =
                    start + PieceLengthMeters * 0.5;
            piece.entry.zMeters = start;
            piece.exit.zMeters = start + PieceLengthMeters;
            piece.challenge.enabled = true;
            piece.challenge.obstacleDistanceMeters =
                    piece.geometryAnchorDistanceMeters;
            course.pieces.push_back(piece);
        }

        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        trail.setCourse(course);

        QVERIFY2(trail.ready(),
                 "maximum valid drop plan produced no trail geometry");
        QVERIFY(trail.sampleCount() <= 16000);
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Drop, 0.65);
        QVERIFY(profile.ready);
        int bridgedOpenings = 0;
        const int indexCount = trail.indexData().size()
                / int(sizeof(quint32));
        for (int index = 0; index + 2 < indexCount; index += 3) {
            double minimumDistance = std::numeric_limits<double>::infinity();
            double maximumDistance =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        trail.indexData(), index + corner));
                const double distance = vertexFloat(
                        trail.vertexData(), trail.stride(), vertex, 44) / 0.22;
                minimumDistance = std::min(minimumDistance, distance);
                maximumDistance = std::max(maximumDistance, distance);
            }
            const int pieceIndex = std::clamp(
                    int(minimumDistance / PieceLengthMeters),
                    0, PieceCount - 1);
            const double center =
                    (double(pieceIndex) + 0.5) * PieceLengthMeters;
            const double openingStart = center + profile.plateauStartMeters;
            const double openingEnd = center + profile.landingStartMeters;
            bridgedOpenings += minimumDistance <= openingStart + 1e-4
                    && maximumDistance >= openingEnd - 1e-4;
        }
        QCOMPARE(bridgedOpenings, 0);
    }

    void authoredObstacleDoesNotRaiseTheRenderedTrailBase()
    {
        WorkoutGameRoadCourse course = straightCourse(20000.0);
        WorkoutGameRoadPiece &piece = course.pieces.front();
        piece.terrain = WorkoutGameTerrainKind::LogOver;
        piece.difficulty = 1.0;
        piece.challenge.enabled = true;
        piece.challenge.obstacleDistanceMeters = 10000.37;

        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::Trail);
        geometry.setCourse(course);

        QVERIFY(geometry.ready());
        QVERIFY(geometry.sampleCount() <= 16000);
        bool sampledObstacle = false;
        for (int sample = 0; sample < geometry.sampleCount(); ++sample) {
            const float y = vertexFloat(
                    geometry.vertexData(), geometry.stride(),
                    sample * 2, sizeof(float));
            const float z = vertexFloat(
                    geometry.vertexData(), geometry.stride(),
                    sample * 2, 2 * int(sizeof(float)));
            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sample(course, z);
            QVERIFY(road.ready);
            QVERIFY(std::abs(y
                    - float(road.visualGroundElevationMeters() + 0.015))
                    < 0.001f);
            if (std::abs(z - piece.challenge.obstacleDistanceMeters)
                    < 0.001f) {
                sampledObstacle = true;
                QVERIFY(road.nonPhysicalFeatureOffsetMeters > 0.60);
                QVERIFY(y < road.center.elevationMeters - 0.50);
            }
        }
        QVERIFY(sampledObstacle);
    }

    void rangeBuildContainsOnlyRequestedCourseChunk()
    {
        WorkoutGame3DGeometry geometry(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        geometry.setCourseRange(straightCourse(1000.0), 300.0, 520.0);

        QVERIFY(geometry.ready());
        QVERIFY(geometry.boundsMin().z() >= 299.9f);
        QVERIFY(geometry.boundsMax().z() <= 520.1f);
        QVERIFY(geometry.sampleCount() < 400);

        geometry.setCourseRange(straightCourse(1000.0), 520.0, 300.0);
        QVERIFY(!geometry.ready());
        QVERIFY(geometry.vertexData().isEmpty());
    }

    void forestFloorDoesNotCopyTrailReliefAcrossItsWidth()
    {
        WorkoutGame3DGeometry floor(
                WorkoutGame3DGeometry::Layer::ForestFloor);
        floor.setCourse(straightCourse(24.0));

        QVERIFY(floor.ready());
        const int middleVertex = (floor.sampleCount() / 2) * 8;
        const float outerY = vertexFloat(
                floor.vertexData(), floor.stride(), middleVertex,
                sizeof(float));
        const float trailEdgeY = vertexFloat(
                floor.vertexData(), floor.stride(), middleVertex + 3,
                sizeof(float));
        QVERIFY(std::abs(outerY - trailEdgeY) > 0.10f);
    }

    void dropTrailIndicesDoNotBridgeTheAirGap()
    {
        const WorkoutGameRoadCourse course = dropCourse();
        QVERIFY(course.ready);
        const auto piece = std::find_if(
                course.pieces.begin(), course.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != course.pieces.end());
        const double lip = piece->challenge.obstacleDistanceMeters;
        WorkoutGame3DGeometry trail(WorkoutGame3DGeometry::Layer::Trail);
        trail.setCourse(course);
        QVERIFY(trail.ready());

        const int indexCount = trail.indexData().size()
                / int(sizeof(quint32));
        QVERIFY(indexCount
                < (trail.sampleCount() - 1) * 6);
        for (int index = 0; index + 2 < indexCount; index += 3) {
            double minimumDistance = std::numeric_limits<double>::infinity();
            double maximumDistance =
                    -std::numeric_limits<double>::infinity();
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = int(indexValue(
                        trail.indexData(), index + corner));
                const double distance = vertexFloat(
                        trail.vertexData(), trail.stride(), vertex, 44) / 0.22;
                minimumDistance = std::min(minimumDistance, distance);
                maximumDistance = std::max(maximumDistance, distance);
            }
            QVERIFY2(!(minimumDistance <= lip + 1e-4
                       && maximumDistance >= lip + 2.45 - 1e-4),
                     "trail triangle bridges the drop air gap");
        }
    }

    void bypassUsesTheRuntimeBranchCurveAndTerrainSurface()
    {
        WorkoutGameRoadCourse course = straightCourse(40.0);
        WorkoutGameRoadPiece &piece = course.pieces.front();
        piece.challenge.enabled = true;
        piece.challenge.obstacleDistanceMeters = 20.0;
        piece.challenge.bypassStartDistanceMeters = 10.0;
        piece.challenge.bypassEndDistanceMeters = 30.0;
        piece.challenge.bypassLateralMeters = 2.2;

        WorkoutGame3DGeometry bypass(
                WorkoutGame3DGeometry::Layer::Bypass);
        bypass.setCourse(course);

        QVERIFY(bypass.ready());
        QVERIFY(bypass.sampleCount() >= 20);
        QCOMPARE(bypass.vertexData().size(),
                 bypass.sampleCount() * 4 * bypass.stride());
        QCOMPARE(bypass.indexData().size(),
                 (bypass.sampleCount() - 1) * 18
                    * int(sizeof(quint32)));
        double maximumCenter = 0.0;
        for (int row = 0; row < bypass.sampleCount(); ++row) {
            const int base = row * 4;
            const double distance = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base, 8);
            const double left = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base + 1, 0);
            const double right = vertexFloat(
                    bypass.vertexData(), bypass.stride(), base + 2, 0);
            const double center = (left + right) * 0.5;
            const double expectedCenter = WorkoutGameTrailBranch::lateralAt(
                    distance,
                    piece.challenge.bypassStartDistanceMeters,
                    piece.challenge.bypassEndDistanceMeters,
                    piece.challenge.bypassLateralMeters);
            QVERIFY(std::abs(center - expectedCenter) < 0.001);
            maximumCenter = std::max(maximumCenter, std::abs(center));

            const WorkoutGameRoadSample road =
                    WorkoutGameRoadCourseBuilder::sample(course, distance);
            const WorkoutGame3DTerrainProfileSnapshot profile =
                    WorkoutGame3DTerrainProfile::build(
                        road, distance, course.seed);
            QVERIFY(road.ready && profile.ready);
            const double branchBlend = WorkoutGameTrailBranch::blend(
                    (distance
                     - piece.challenge.bypassStartDistanceMeters)
                    / (piece.challenge.bypassEndDistanceMeters
                       - piece.challenge.bypassStartDistanceMeters));
            for (int vertex = 0; vertex < 4; ++vertex) {
                const double lateral = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 0);
                const double y = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 4);
                const double expected =
                        WorkoutGame3DTerrainProfile::elevationAtLateral(
                            profile, lateral)
                        + (vertex == 1 || vertex == 2
                            ? WorkoutGameTrailBranch::treadLiftMeters(
                                branchBlend)
                            : WorkoutGameTrailBranch::edgeLiftMeters(
                                branchBlend));
                QVERIFY(std::abs(y - expected) < 0.001);
                const double nx = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 12);
                const double ny = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 16);
                const double nz = vertexFloat(
                        bypass.vertexData(), bypass.stride(), base + vertex, 20);
                QVERIFY(ny > 0.0);
                QVERIFY(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz)
                                 - 1.0) < 0.001);
            }
        }
        QVERIFY(maximumCenter > 2.1);
        const double firstOuter = vertexFloat(
                bypass.vertexData(), bypass.stride(), 0, 0);
        const double lastOuter = vertexFloat(
                bypass.vertexData(), bypass.stride(),
                (bypass.sampleCount() - 1) * 4 + 3, 0);
        QVERIFY(std::abs(firstOuter + 0.68) < 0.001);
        QVERIFY(std::abs(lastOuter - 0.68) < 0.001);
    }

    void ordinaryTurnBankBuildsASeparateBoundedSurface()
    {
        WorkoutGameRoadCourse course = straightCourse(40.0);
        WorkoutGameRoadPiece &piece = course.pieces.front();
        piece.terrain = WorkoutGameTerrainKind::SmoothTrail;
        piece.turnRadians = 0.72;
        piece.geometryAnchorDistanceMeters = 20.0;
        piece.bank.enabled = true;
        piece.bank.startDistanceMeters = 0.0;
        piece.bank.curveStartDistanceMeters = 3.0;
        piece.bank.curveEndDistanceMeters = 37.0;
        piece.bank.endDistanceMeters = 40.0;
        piece.bank.socketHalfWidthMeters = 0.68;
        piece.bank.activeHalfWidthMeters = 0.96;
        piece.bank.maximumBankRadians = 0.34;
        piece.bank.maximumLineOffsetMeters = 0.42;
        piece.bank.designSpeedMetersPerSecond = 7.0;

        const WorkoutGameRoadSample middle =
                WorkoutGameRoadCourseBuilder::sample(course, 20.0);
        QVERIFY(middle.ready);
        QVERIFY(std::abs(middle.bermBankRadians) > 0.30);
        QVERIFY(!middle.renderableTrailSurface);
        QCOMPARE(middle.terrain, WorkoutGameTerrainKind::SmoothTrail);

        WorkoutGame3DGeometry bank(WorkoutGame3DGeometry::Layer::Berm);
        bank.setCourse(course);
        QVERIFY(bank.ready());
        QVERIFY(bank.sampleCount() > 200);
        QVERIFY(bank.sampleCount() <= 16000);
        const int triangleCount = bank.indexData().size()
                / int(3 * sizeof(quint32));
        QVERIFY(triangleCount <= 192000);
        QCOMPARE(bank.vertexData().size(),
                 bank.sampleCount() * 7 * bank.stride());
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGame3DGeometry)
#include "testWorkoutGame3DGeometry.moc"
