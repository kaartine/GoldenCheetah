/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameEngine.h"
#include "Train/WorkoutGameAssetCatalog.h"
#include "Train/WorkoutGameAssetPhysicsResolver.h"
#include "Train/WorkoutGameAssetPhysicsSampler.h"
#include "Train/WorkoutGameCourseDocument.h"
#include "Train/WorkoutGameCourseRuntime.h"
#include "Train/WorkoutGameCourseSourceAdapter.h"
#include "Train/WorkoutGameDistancePlayback.h"
#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameLegacyFt02V1.h"
#include "Train/WorkoutGameRoadPlan.h"
#include "Train/WorkoutGameRiderVisual.h"
#include "Train/WorkoutGameTabletopGeometry.h"
#include "Train/TrainingDataGenerator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>

namespace {

void mixValue(std::uint64_t &hash, std::uint64_t value)
{
    constexpr std::uint64_t FnvPrime = 1099511628211ULL;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffULL;
        hash *= FnvPrime;
    }
}

void mixReal(std::uint64_t &hash, double value)
{
    mixValue(hash, std::uint64_t(std::int64_t(std::llround(value * 1000000.0))));
}

std::uint64_t replayDigest(
        const WorkoutGameCourse &course,
        WorkoutGameFeatureLabScenario scenario)
{
    constexpr double FtpWatts = 200.0;
    constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
    WorkoutGameEngine engine;
    if (!engine.configure(course, FtpWatts, true)) return 0;

    std::uint64_t digest = FnvOffset;
    for (std::int64_t timeMs = 0; timeMs < course.durationMs; timeMs += 20) {
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, timeMs, scenario);
        input.heartRate = scenario == WorkoutGameFeatureLabScenario::Pass
                ? 148 : 122;
        const WorkoutGameEngineFrame frame = engine.update(
                input, 100000 + timeMs);
        const auto &simulation = frame.visual.simulation;
        const auto &world = frame.visual.world;
        const auto &camera = frame.visual.camera;
        mixValue(digest, std::uint64_t(simulation.workoutTimeMs));
        mixValue(digest, std::uint64_t(simulation.activeSection));
        mixValue(digest, simulation.score);
        mixValue(digest, std::uint64_t(simulation.featureOutcome));
        mixValue(digest, std::uint64_t(simulation.route));
        mixReal(digest, simulation.courseProgress);
        mixReal(digest, simulation.sectionProgress);
        mixReal(digest, simulation.speedKph);
        mixReal(digest, simulation.challengeReadiness);
        mixValue(digest, world.generation);
        mixValue(digest, std::uint64_t(world.terrain));
        mixReal(digest, world.rider.distanceMeters);
        mixReal(digest, world.rider.elevationMeters);
        mixReal(digest, world.rider.pitchDegrees);
        mixReal(digest, world.rider.rearSuspension);
        mixReal(digest, world.rider.frontSuspension);
        mixReal(digest, world.rider.clearanceMeters);
        mixValue(digest, world.rider.airborne ? 1 : 0);
        mixValue(digest, world.rider.walking ? 1 : 0);
        mixReal(digest, world.speedMetersPerSecond);
        mixReal(digest, world.landingImpact);
        mixReal(digest, camera.centerDistanceMeters);
        mixReal(digest, camera.centerElevationMeters);
        mixReal(digest, camera.lookAheadMeters);
        mixReal(digest, camera.zoom);
        mixReal(digest, frame.visual.riderPedalCycles);
        mixValue(digest, frame.sequence);
        mixValue(digest, std::uint64_t(frame.heartRate));
    }
    return digest;
}

int sectionForTerrain(
        const WorkoutGameCourse &course,
        WorkoutGameTerrainKind terrain)
{
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        if (course.sections[index].terrain == terrain) return int(index);
    }
    return -1;
}

const WorkoutGameRoadPiece *challengePieceFor(
        const WorkoutGameRoadCourse &road,
        int sourceSectionIndex)
{
    for (const WorkoutGameRoadPiece &piece : road.pieces) {
        if (piece.challenge.enabled
                && int(piece.sourceSectionIndex) == sourceSectionIndex) {
            return &piece;
        }
    }
    return nullptr;
}

WorkoutGameCourse gapJumpCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 1701u;
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::GapJump;
    section.durationMs = course.durationMs;
    section.lengthMeters = 120.0;
    section.targetWatts = 200.0;
    section.difficulty = 0.5;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

auto worldValues(const WorkoutGameWorldSnapshot &world)
{
    const auto &rider = world.rider;
    return std::make_tuple(
            world.ready, world.generation, world.terrain, world.seed,
            world.gradePercent, world.difficulty, world.terrainOffsetMeters,
            world.surfaceElevationMeters, world.speedMetersPerSecond,
            world.landingImpact, rider.distanceMeters, rider.elevationMeters,
            rider.pitchDegrees, rider.rollDegrees, rider.rearSuspension,
            rider.frontSuspension, rider.rearWheelRadians,
            rider.frontWheelRadians, rider.clearanceMeters,
            rider.rearWheelGrounded, rider.frontWheelGrounded,
            rider.airborne, rider.walking);
}

auto featureValues(const WorkoutGameFeatureRuntimeSnapshot &f)
{
    return std::make_tuple(
            f.ready, f.sourceSectionIndex, f.terrain, f.phase, f.motion,
            f.outcome, f.route, f.visualDistanceMeters, f.prepareDistanceMeters,
            f.launchWindowStartDistanceMeters, f.decisionDistanceMeters,
            f.obstacleDistanceMeters, f.physicalTakeoffDistanceMeters,
            f.actionStartDistanceMeters, f.actionEndDistanceMeters,
            f.distanceToObstacleMeters, f.readiness, f.bermLineBias,
            f.lateralOffsetMeters, f.verticalOffsetMeters, f.flightDurationSeconds,
            f.pitchDegrees, f.vibration, f.landingImpact, f.provisionalGapLine,
            f.lockedGapLine, f.steeringGapLine, f.predictedApproachSpeedMetersPerSecond,
            f.launchRollingSpeedMetersPerSecond, f.launchBestSpeedMetersPerSecond,
            f.launchPowerHoldMilliseconds, f.selectedGapLengthMeters,
            f.launchWindowActive, f.launchSpeedReady, f.launchPowerReady,
            f.gapLineReachable, f.gapLineLocked, f.actionId, f.triggerJump);
}

WorkoutGameCourseDocument legacyLogDocument(double difficulty, double speed,
                                             bool rebase)
{
    WorkoutGameCourseDocument document;
    document.schemaVersion = 6;
    document.title = QStringLiteral("FT02 Engine codec trace");
    document.sourceFileName = QStringLiteral("ft02-trace.erg");
    document.ftpWatts = 200.0;
    document.course.status = WorkoutGameDistanceCourseStatus::Ready;
    document.course.seed = 0x8f12u;
    document.course.nominalDurationMs = rebase ? 90000 : 30000;
    document.course.totalDistanceMeters = speed
            * double(document.course.nominalDurationMs) / 1000.0;
    WorkoutGameDistanceCourseSection section;
    section.feature = WorkoutGameFeature::SprintJump;
    section.terrain = WorkoutGameTerrainKind::LogOver;
    section.nominalDurationMs = document.course.nominalDurationMs;
    section.minimumDurationMs = section.nominalDurationMs;
    section.maximumDurationMs = section.nominalDurationMs;
    section.lengthMeters = document.course.totalDistanceMeters;
    section.targetStartWatts = section.targetEndWatts = 260.0;
    section.referenceEffortStartWatts = section.referenceEffortEndWatts = 260.0;
    section.difficulty = difficulty;
    section.challengeCount = 1;
    document.course.sections.push_back(section);
    auto plan = std::make_shared<WorkoutGameRoadPlan>(
            WorkoutGameRoadCourseBuilder::generatePlan(
                WorkoutGameDistancePlayback::visualCourse(document.course),
                document.ftpWatts));
    // The oracle really takes the old procedural path, not the frozen adapter.
    plan->generationVersion =
            WorkoutGameRoadPlan::BankAndReliefGenerationVersion;
    plan->assetPhysicsSnapshot.reset();
    document.course.roadPlan = plan;
    return document;
}

WorkoutGameCourseSourceRequest convertedFt02Request()
{
    WorkoutGameCourseSourceRequest request;
    request.sourceContents = QByteArrayLiteral("converted FT02 fixture");
    request.sourceFileName = QStringLiteral("converted-ft02.erg");
    request.ftpWatts = 190.0;
    request.preset = WorkoutGameCoursePreset::RideFirst;
    request.seed = 1701u;
    double timeMs = 0.0;
    for (int interval = 0; interval < 12; ++interval) {
        const double watts = interval % 2 == 0 ? 150.0 : 153.0;
        request.points.push_back({timeMs, watts});
        timeMs += 5.0 * 60000.0;
        request.points.push_back({timeMs, watts});
    }
    return request;
}

}

class TestWorkoutGameEngine : public QObject
{
    Q_OBJECT

private slots:
    void convertedCoursePersistsFrozenLogPhysicsIntoRuntimeEngine()
    {
        const WorkoutGameCourseSourceResult converted =
                WorkoutGameCourseSourceAdapter::convert(
                    convertedFt02Request());
        QCOMPARE(converted.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(converted.document.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QVERIFY(converted.document.course.roadPlan);
        const auto &generatedPlan = *converted.document.course.roadPlan;
        QVERIFY(generatedPlan.assetPhysicsSnapshot);
        const auto &generatedSnapshot = *generatedPlan.assetPhysicsSnapshot;
        QCOMPARE(generatedSnapshot.pieceBindings.size(),
                 generatedPlan.pieces.size());

        const auto generatedPiece = std::find_if(
                generatedPlan.pieces.begin(), generatedPlan.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.challenge.enabled
                            && piece.terrain == WorkoutGameTerrainKind::LogOver;
                });
        QVERIFY(generatedPiece != generatedPlan.pieces.end());
        const std::size_t pieceIndex = std::size_t(
                generatedPiece - generatedPlan.pieces.begin());
        const auto &generatedBinding =
                generatedSnapshot.pieceBindings.at(pieceIndex);
        QVERIFY((generatedBinding.flags
                 & WorkoutGameCourseAssetPhysicsSnapshot::LegacyProceduralV1)
                != 0u);
        QVERIFY(generatedBinding.legacyFt02RecordIndex
                < generatedSnapshot.legacyFt02Records.size());
        const auto &generatedRecord = generatedSnapshot.legacyFt02Records.at(
                generatedBinding.legacyFt02RecordIndex);
        const double generatedRadius =
                WorkoutGameLegacyFt02V1::radiusMeters(
                    generatedPiece->difficulty);
        QVERIFY(generatedRecord.enabled);
        QCOMPARE(WorkoutGameLegacyBinary64::encode(generatedRecord.startMeters),
                 WorkoutGameLegacyBinary64::encode(-generatedRadius));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(generatedRecord.endMeters),
                 WorkoutGameLegacyBinary64::encode(generatedRadius));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(generatedRecord.heightMeters),
                 WorkoutGameLegacyBinary64::encode(2.0 * generatedRadius));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(
                    generatedRecord.obstacleAnchorMeters),
                 WorkoutGameLegacyBinary64::encode(
                    generatedPiece->challenge.obstacleDistanceMeters));

        WorkoutGameCourseDocument persisted = converted.document;
        auto persistedPlan = std::make_shared<WorkoutGameRoadPlan>(
                generatedPlan);
        auto persistedSnapshot =
                std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>(
                    generatedSnapshot);
        auto &persistedRecord = persistedSnapshot->legacyFt02Records.at(
                generatedBinding.legacyFt02RecordIndex);
        persistedRecord.startMeters = -0.731;
        persistedRecord.endMeters = 1.337;
        persistedRecord.heightMeters = 0.913;
        persistedPlan->assetPhysicsSnapshot = persistedSnapshot;
        persisted.course.roadPlan = persistedPlan;
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(persisted));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath(
                QStringLiteral("converted-ft02.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, persisted, error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseRuntime runtime;
        QCOMPARE(runtime.configure(coursePath),
                 WorkoutGameCourseRuntimeStatus::Ready);
        QVERIFY(runtime.visualCourse().roadPlan);
        const auto &runtimeSnapshot =
                *runtime.visualCourse().roadPlan->assetPhysicsSnapshot;
        const auto &runtimeRecord = runtimeSnapshot.legacyFt02Records.at(
                generatedBinding.legacyFt02RecordIndex);
        QCOMPARE(WorkoutGameLegacyBinary64::encode(runtimeRecord.startMeters),
                 WorkoutGameLegacyBinary64::encode(-0.731));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(runtimeRecord.endMeters),
                 WorkoutGameLegacyBinary64::encode(1.337));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(runtimeRecord.heightMeters),
                 WorkoutGameLegacyBinary64::encode(0.913));

        WorkoutGameDistancePlayback playback;
        QVERIFY(playback.configure(persisted.course));
        const WorkoutGameDistancePlaybackSnapshot atObstacle =
                playback.atDistance(runtimeRecord.obstacleAnchorMeters);
        QVERIFY(atObstacle.ready && !atObstacle.finished);

        WorkoutGameEngine engine;
        QVERIFY(engine.configure(runtime.visualCourse(), runtime.ftpWatts(),
                                 false));
        WorkoutGameEngineInput input;
        input.simulation.workoutTimeMs = atObstacle.nominalTimeMs;
        input.simulation.actualWatts = 220.0;
        input.simulation.targetWatts = atObstacle.targetWatts;
        input.simulation.cadenceRpm = 90.0;
        input.simulation.authoritativeSpeedKph = 28.0;
        const WorkoutGameEngineFrame frame = engine.update(input, 100000);
        QVERIFY(frame.visual.feature.ready);
        QCOMPARE(frame.visual.feature.terrain,
                 WorkoutGameTerrainKind::LogOver);
        QCOMPARE(WorkoutGameLegacyBinary64::encode(
                    frame.visual.feature.obstacleDistanceMeters),
                 WorkoutGameLegacyBinary64::encode(
                    runtimeRecord.obstacleAnchorMeters));
        QCOMPARE(WorkoutGameLegacyBinary64::encode(
                    frame.visual.feature.physicalTakeoffDistanceMeters),
                 WorkoutGameLegacyBinary64::encode(
                    runtimeRecord.obstacleAnchorMeters
                        + runtimeRecord.startMeters));
    }

    void frozenLogEngineTraceSurvivesCodec_data()
    {
        QTest::addColumn<double>("difficulty");
        QTest::addColumn<double>("speed");
        QTest::addColumn<bool>("bypass");
        QTest::addColumn<bool>("rebase");
        QTest::addColumn<bool>("customBounds");
        for (double difficulty : {0.0, 0.50049, 1.0}) {
            for (double speed : {3.33, 5.0, 7.0}) {
                for (bool bypass : {false, true}) {
                    const auto name = QStringLiteral("d%1-v%2-bypass%3")
                            .arg(difficulty).arg(speed).arg(bypass).toLatin1();
                    QTest::newRow(name.constData())
                            << difficulty << speed << bypass << false << false;
                }
            }
        }
        for (bool bypass : {false, true}) {
            const auto name = QStringLiteral("rebase-bypass%1").arg(bypass).toLatin1();
            QTest::newRow(name.constData()) << 0.50049 << 7.0 << bypass << true << false;
        }
        QTest::newRow("stored-bounds-authority") << 0.50049 << 5.0 << false << false << true;
    }

    void frozenLogEngineTraceSurvivesCodec()
    {
        QFETCH(double, difficulty);
        QFETCH(double, speed);
        QFETCH(bool, bypass);
        QFETCH(bool, rebase);
        QFETCH(bool, customBounds);
        const auto legacy = legacyLogDocument(difficulty, speed, rebase);
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(legacy));
        const auto legacyBytes = WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(!legacyBytes.isEmpty());
        WorkoutGameCourseDocument legacyDecoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(legacyBytes, legacyDecoded),
                 WorkoutGameCourseDocumentStatus::Ready);

        auto frozen = legacy;
        frozen.schemaVersion = WorkoutGameCourseDocumentCodec::AssetPhysicsSchemaVersion;
        auto plan = std::make_shared<WorkoutGameRoadPlan>(*legacy.course.roadPlan);
        plan->generationVersion = WorkoutGameRoadPlan::CurrentGenerationVersion;
        auto snapshot = WorkoutGameAssetPhysicsSnapshotBuilder::frozenLegacyFt02For(*plan);
        QVERIFY(snapshot);
        const auto challenge = std::find_if(plan->pieces.begin(), plan->pieces.end(),
                [](const auto &p) { return p.challenge.enabled; });
        QVERIFY(challenge != plan->pieces.end());
        const auto pieceIndex = std::size_t(challenge - plan->pieces.begin());
        const auto recordIndex = snapshot->pieceBindings.at(pieceIndex).legacyFt02RecordIndex;
        QVERIFY(recordIndex < snapshot->legacyFt02Records.size());
        if (customBounds) {
            auto modified = std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>(*snapshot);
            auto &record = modified->legacyFt02Records.at(recordIndex);
            record.startMeters = -0.73;
            record.endMeters = 1.31;
            record.heightMeters = 0.89;
            snapshot = modified;
        }
        const auto record = snapshot->legacyFt02Records.at(recordIndex);
        QVERIFY(record.enabled);
        plan->assetPhysicsSnapshot = snapshot;
        frozen.course.roadPlan = plan;
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(frozen));
        const auto encoded = WorkoutGameCourseDocumentCodec::encode(frozen);
        QVERIFY(!encoded.isEmpty());
        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);
        QVERIFY(decoded.course.roadPlan->assetPhysicsSnapshot);
        const auto &decodedSnapshot = *decoded.course.roadPlan->assetPhysicsSnapshot;
        const auto &decodedBinding = decodedSnapshot.pieceBindings.at(pieceIndex);
        QCOMPARE(decodedBinding.legacyFt02RecordIndex, recordIndex);
        const auto &decodedRecord = decodedSnapshot.legacyFt02Records.at(recordIndex);
        for (const auto &values : {std::make_pair(record.startMeters, decodedRecord.startMeters),
                 std::make_pair(record.endMeters, decodedRecord.endMeters),
                 std::make_pair(record.heightMeters, decodedRecord.heightMeters),
                 std::make_pair(record.obstacleAnchorMeters, decodedRecord.obstacleAnchorMeters)}) {
            QCOMPARE(WorkoutGameLegacyBinary64::encode(values.first),
                     WorkoutGameLegacyBinary64::encode(values.second));
        }
        const auto sample = WorkoutGameAssetPhysicsSampler::sample(
                decodedSnapshot, pieceIndex, record.obstacleAnchorMeters);
        QVERIFY(sample.bound && sample.surfacePresent);
        QVERIFY(!sample.obstacleContact && !sample.materialDefined);

        WorkoutGameEngine legacyEngine, frozenEngine, decodedEngine;
        QVERIFY(legacyEngine.configure(WorkoutGameDistancePlayback::visualCourse(
                    legacyDecoded.course), legacy.ftpWatts, false));
        QVERIFY(frozenEngine.configure(WorkoutGameDistancePlayback::visualCourse(
                    frozen.course), frozen.ftpWatts, false));
        QVERIFY(decodedEngine.configure(WorkoutGameDistancePlayback::visualCourse(
                    decoded.course), decoded.ftpWatts, false));
        bool sawCompleted = false, sawBypass = false, sawJump = false;
        bool sawAirborne = false, sawLanding = false, priorAirborne = false;
        bool crossedAnchor = false, crossedRebase = false, sawDistinctTakeoff = false;
        double previousDistance = 0.0;
        std::uint64_t committedActionId = 0;
        for (std::int64_t timeMs = 0; timeMs < legacy.course.nominalDurationMs; timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation.workoutTimeMs = timeMs;
            input.simulation.actualWatts = 260.0 * (bypass ? 0.2 : 1.2);
            input.simulation.targetWatts = 260.0;
            input.simulation.cadenceRpm = 85.0;
            input.simulation.authoritativeSpeedKph = speed * 3.6;
            const auto old = legacyEngine.update(input, 100000 + timeMs);
            const auto current = frozenEngine.update(input, 100000 + timeMs);
            const auto loaded = decodedEngine.update(input, 100000 + timeMs);
            const auto context = QStringLiteral("tick %1, distance %2")
                    .arg(timeMs).arg(current.visual.world.rider.distanceMeters, 0, 'g', 17);
            const auto &f = current.visual.feature;
            QVERIFY(current.visual.world.ready && loaded.visual.world.ready);
            QVERIFY2(featureValues(f) == featureValues(loaded.visual.feature), qPrintable(context));
            QVERIFY2(worldValues(current.visual.world) == worldValues(loaded.visual.world), qPrintable(context));
            QCOMPARE(current.visual.simulation.score, loaded.visual.simulation.score);
            QCOMPARE(current.visual.simulation.route, loaded.visual.simulation.route);
            QCOMPARE(current.visual.simulation.featureOutcome, loaded.visual.simulation.featureOutcome);
            if (!customBounds) {
                QVERIFY2(featureValues(old.visual.feature) == featureValues(f), qPrintable(context));
                QVERIFY2(worldValues(old.visual.world) == worldValues(current.visual.world), qPrintable(context));
                QCOMPARE(old.visual.simulation.score, current.visual.simulation.score);
                QCOMPARE(old.visual.simulation.featureOutcome, current.visual.simulation.featureOutcome);
                QCOMPARE(old.visual.simulation.route, current.visual.simulation.route);
            }
            if (f.ready) {
                QCOMPARE(f.sourceSectionIndex, int(challenge->sourceSectionIndex));
                QCOMPARE(f.obstacleDistanceMeters, record.obstacleAnchorMeters);
                QCOMPARE(f.physicalTakeoffDistanceMeters,
                         record.obstacleAnchorMeters + record.startMeters);
                sawDistinctTakeoff |= f.physicalTakeoffDistanceMeters
                        != old.visual.feature.physicalTakeoffDistanceMeters;
                sawCompleted |= f.outcome == WorkoutGameFeatureOutcome::Completed;
                sawBypass |= f.outcome == WorkoutGameFeatureOutcome::Bypassed;
                if (f.outcome == WorkoutGameFeatureOutcome::Completed) {
                    QCOMPARE(f.route, WorkoutGameRoute::MainLine);
                    QVERIFY(f.actionId != 0);
                    if (committedActionId == 0) committedActionId = f.actionId;
                    QCOMPARE(f.actionId, committedActionId);
                }
                if (f.outcome == WorkoutGameFeatureOutcome::Bypassed)
                    QCOMPARE(f.route, WorkoutGameRoute::SafeBypass);
                sawJump |= f.triggerJump;
            }
            const auto &world = current.visual.world;
            if (sawJump && !priorAirborne && world.rider.airborne) sawAirborne = true;
            if (sawAirborne && priorAirborne && !world.rider.airborne) sawLanding = true;
            if (bypass) QVERIFY(!world.rider.airborne && !f.triggerJump);
            priorAirborne = world.rider.airborne;
            crossedAnchor |= previousDistance < record.obstacleAnchorMeters
                    && world.rider.distanceMeters >= record.obstacleAnchorMeters;
            crossedRebase |= previousDistance < 176.0 && world.rider.distanceMeters >= 176.0;
            previousDistance = world.rider.distanceMeters;
        }
        QVERIFY(crossedAnchor);
        QCOMPARE(sawCompleted, !bypass);
        QCOMPARE(sawBypass, bypass);
        QCOMPARE(sawJump, !bypass);
        if (!bypass) QVERIFY(sawAirborne && sawLanding);
        if (rebase) {
            QVERIFY(record.obstacleAnchorMeters > 220.0);
            QVERIFY(crossedRebase);
        }
        QCOMPARE(sawDistinctTakeoff, customBounds);
    }

    void configuredEngineOwnsSnapshotAfterCatalogReplacement()
    {
        constexpr std::int64_t DurationMs = 30000;
        constexpr std::size_t InitialFrames = 25;
        WorkoutGameCourse course = gapJumpCourse();
        course.sections[0].terrain = WorkoutGameTerrainKind::LogOver;
        auto plan = std::make_shared<WorkoutGameRoadPlan>(
                WorkoutGameRoadCourseBuilder::generatePlan(course, 200.0));
        QString error;
        auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));
        auto resolution = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, plan->pieces);
        QCOMPARE(resolution.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
        QVERIFY(resolution.snapshot);
        QCOMPARE(resolution.snapshot->bindings.size(), std::size_t(1));
        QCOMPARE(resolution.snapshot->bindings[0].assetId,
                 QStringLiteral("FT-02-log-over-greybox"));
        QCOMPARE(resolution.snapshot->bindings[0].resolvedExtentMm,
                 std::uint32_t(540));
        plan->assetPhysicsSnapshot = resolution.snapshot;
        course.roadPlan = plan;
        auto road = WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);
        QCOMPARE(road.assetPhysicsSnapshot, resolution.snapshot);
        const auto *challenge = challengePieceFor(road, 0);
        QVERIFY(challenge);
        const double obstacle = challenge->challenge.obstacleDistanceMeters;
        const auto oldSample = WorkoutGameRoadCourseBuilder::sample(road, obstacle);
        QVERIFY(oldSample.ready);
        QVERIFY(std::abs(oldSample.surfaceOffsetMeters - 0.54) < 1e-12);
        std::weak_ptr<const WorkoutGameCourseAssetPhysicsSnapshot> snapshotOwner =
                resolution.snapshot;
        std::weak_ptr<const WorkoutGameRoadPlan> planOwner = plan;

        std::vector<WorkoutGameEngineInput> inputs;
        std::vector<WorkoutGameEngineFrame> expected;
        {
            WorkoutGameEngine baseline;
            QVERIFY(baseline.configure(course, 200.0, true));
            for (std::int64_t timeMs = 0; timeMs < DurationMs; timeMs += 20) {
                WorkoutGameEngineInput input;
                input.simulation = WorkoutGameFeatureLab::input(
                        course, timeMs, WorkoutGameFeatureLabScenario::Pass);
                inputs.push_back(input);
                expected.push_back(baseline.update(input, 100000 + timeMs));
            }
        }
        auto engine = std::make_unique<WorkoutGameEngine>();
        QVERIFY(engine->configure(course, 200.0, true));
        for (std::size_t index = 0; index < InitialFrames; ++index) {
            const auto frame = engine->update(inputs[index], 100000 + index * 20);
            QVERIFY(frame.visual.world.ready);
            QVERIFY(worldValues(frame.visual.world)
                    == worldValues(expected[index].visual.world));
        }

        // The process owns a catalog candidate; only future resolutions see it.
        QFile file(QStringLiteral(":/json/workout-game-asset-catalog.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto document = QJsonDocument::fromJson(file.readAll());
        QVERIFY(document.isObject());
        auto root = document.object();
        auto profiles = root.value(QStringLiteral("profiles")).toArray();
        bool changedProfile = false;
        for (qsizetype index = 0; index < profiles.size(); ++index) {
            auto profile = profiles[index].toObject();
            if (profile.value(QStringLiteral("profileId")).toString()
                    != QStringLiteral("FT-02-log-over-v1")) continue;
            auto scale = profile.value(QStringLiteral("difficultyScale")).toObject();
            QCOMPARE(scale.value(QStringLiteral("baseExtentMm")).toInt(), 440);
            scale.insert(QStringLiteral("baseExtentMm"), 540);
            profile.insert(QStringLiteral("difficultyScale"), scale);
            profiles[index] = profile;
            changedProfile = true;
        }
        QVERIFY(changedProfile);
        root.insert(QStringLiteral("profiles"), profiles);
        catalog.reset();
        catalog = WorkoutGameAssetCatalog::fromJson(
                QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
        QVERIFY2(catalog, qPrintable(error));
        {
            const auto replacement = WorkoutGameAssetPhysicsResolver::resolve(
                    *catalog, plan->pieces);
            QCOMPARE(replacement.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
            QVERIFY(replacement.snapshot);
            QCOMPARE(replacement.snapshot->bindings.size(), std::size_t(1));
            QCOMPARE(replacement.snapshot->bindings[0].resolvedExtentMm,
                     std::uint32_t(640));
            auto newPlan = std::make_shared<WorkoutGameRoadPlan>(*plan);
            newPlan->assetPhysicsSnapshot = replacement.snapshot;
            auto newCourse = course;
            newCourse.roadPlan = newPlan;
            const auto newRoad = WorkoutGameRoadCourseBuilder::build(newCourse, 200.0);
            QVERIFY(newRoad.ready);
            const auto newSample = WorkoutGameRoadCourseBuilder::sample(newRoad, obstacle);
            QVERIFY(newSample.ready);
            QVERIFY(std::abs(newSample.surfaceOffsetMeters
                             - oldSample.surfaceOffsetMeters - 0.1) < 1e-12);
            WorkoutGameEngine newEngine;
            QVERIFY(newEngine.configure(newCourse, 200.0, true));
            bool changedWorld = false;
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                const auto frame = newEngine.update(inputs[index], 100000 + index * 20);
                QVERIFY(frame.visual.world.ready);
                changedWorld |= frame.visual.world.surfaceElevationMeters
                        != expected[index].visual.world.surfaceElevationMeters;
            }
            QVERIFY(changedWorld);
        }
        catalog.reset();
        road = {};
        course = {};
        plan.reset();
        resolution.snapshot.reset();
        QVERIFY(!snapshotOwner.expired());
        QVERIFY(!planOwner.expired());

        bool traversedObstacle = false;
        for (std::size_t index = InitialFrames; index < inputs.size(); ++index) {
            const auto frame = engine->update(inputs[index], 100000 + index * 20);
            const auto &reference = expected[index];
            QVERIFY(frame.visual.simulation.ready);
            QVERIFY(frame.visual.world.ready);
            QVERIFY2(worldValues(frame.visual.world) == worldValues(reference.visual.world),
                     qPrintable(QStringLiteral("world differs at frame %1").arg(index)));
            QCOMPARE(frame.visual.world.generation, expected[0].visual.world.generation);
            QCOMPARE(frame.sequence, reference.sequence);
            QCOMPARE(frame.visual.simulation.workoutTimeMs,
                     reference.visual.simulation.workoutTimeMs);
            QCOMPARE(frame.visual.simulation.score, reference.visual.simulation.score);
            QCOMPARE(frame.visual.simulation.featureOutcome,
                     reference.visual.simulation.featureOutcome);
            QCOMPARE(frame.visual.feature.phase, reference.visual.feature.phase);
            QCOMPARE(frame.visual.camera.centerElevationMeters,
                     reference.visual.camera.centerElevationMeters);
            traversedObstacle |= frame.visual.world.rider.distanceMeters > obstacle + 1.0;
        }
        QVERIFY(expected[InitialFrames - 1].visual.world.rider.distanceMeters < obstacle);
        QVERIFY(traversedObstacle);
        QVERIFY(!snapshotOwner.expired());
        engine.reset();
        QVERIFY(snapshotOwner.expired());
        QVERIFY(planOwner.expired());
    }

    void featureLabGapScenariosExerciseEveryLineAndPowerGate()
    {
        constexpr double FtpWatts = 200.0;
        constexpr double MaximumLateralStepMeters = 0.07;
        constexpr double MainTrailMergeToleranceMeters = 0.08;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const int gapSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::GapJump);
        QVERIFY(gapSection >= 0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const WorkoutGameRoadPiece *gapPiece = challengePieceFor(
                road, gapSection);
        QVERIFY(gapPiece != nullptr);
        QVERIFY(gapPiece->gapJump.enabled);
        struct Case {
            WorkoutGameFeatureLabGapScenario scenario;
            WorkoutGameGapJumpLine expectedLine;
            WorkoutGameFeatureOutcome expectedOutcome;
        };
        const Case cases[] = {
            {WorkoutGameFeatureLabGapScenario::Short,
             WorkoutGameGapJumpLine::Short,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Medium,
             WorkoutGameGapJumpLine::Medium,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Long,
             WorkoutGameGapJumpLine::Long,
             WorkoutGameFeatureOutcome::Completed},
            {WorkoutGameFeatureLabGapScenario::Safe,
             WorkoutGameGapJumpLine::None,
             WorkoutGameFeatureOutcome::Bypassed}
        };

        for (const Case &testCase : cases) {
            enum class JumpStage {
                Approach,
                Takeoff,
                Airborne,
                Landed,
                GroundedRecovery
            };
            WorkoutGameEngine engine;
            QVERIFY(engine.configure(course, FtpWatts, true));
            bool observedLock = false;
            bool leftGapSection = false;
            bool observedSafeRecovery = false;
            bool hasPriorGapPosition = false;
            bool movedBackTowardMainTrail = false;
            double priorForwardDistanceMeters = 0.0;
            double priorLateralMeters = 0.0;
            double maximumAbsoluteLateralMeters = 0.0;
            double finalGapLateralMeters = 0.0;
            WorkoutGameGapJumpLine lockedLine =
                    WorkoutGameGapJumpLine::None;
            JumpStage jumpStage = JumpStage::Approach;
            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                input.simulation = WorkoutGameFeatureLab::input(
                        course, timeMs, WorkoutGameFeatureLabScenario::Pass);
                WorkoutGameFeatureLab::applyGapScenario(
                        course, timeMs, testCase.scenario, input.simulation);
                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (observedLock && frame.visual.simulation.activeSection
                            != frame.visual.feature.sourceSectionIndex) {
                    leftGapSection = true;
                }
                if (frame.visual.feature.terrain
                            != WorkoutGameTerrainKind::GapJump) {
                    continue;
                }

                const double forwardDistanceMeters =
                        frame.visual.world.rider.distanceMeters;
                const double lateralMeters =
                        frame.visual.feature.lateralOffsetMeters;
                QVERIFY(std::isfinite(forwardDistanceMeters));
                QVERIFY(std::isfinite(lateralMeters));
                if (hasPriorGapPosition) {
                    QVERIFY2(forwardDistanceMeters + 1.0e-7
                                    >= priorForwardDistanceMeters,
                             "gap scenario moved backward");
                    QVERIFY2(std::abs(lateralMeters - priorLateralMeters)
                                    <= MaximumLateralStepMeters + 1.0e-9,
                             qPrintable(QStringLiteral(
                                 "gap scenario moved laterally %1 m in one "
                                 "20 ms engine step")
                                 .arg(std::abs(
                                     lateralMeters - priorLateralMeters),
                                      0, 'f', 6)));
                    if (std::abs(lateralMeters)
                            < std::abs(priorLateralMeters) - 1.0e-6) {
                        movedBackTowardMainTrail = true;
                    }
                }
                hasPriorGapPosition = true;
                priorForwardDistanceMeters = forwardDistanceMeters;
                priorLateralMeters = lateralMeters;
                finalGapLateralMeters = lateralMeters;
                maximumAbsoluteLateralMeters = std::max(
                        maximumAbsoluteLateralMeters,
                        std::abs(lateralMeters));

                if (frame.visual.feature.gapLineLocked) {
                    if (!observedLock) {
                        observedLock = true;
                        lockedLine = frame.visual.feature.lockedGapLine;
                    }
                    QCOMPARE(frame.visual.feature.lockedGapLine, lockedLine);
                    QCOMPARE(frame.visual.feature.lockedGapLine,
                             testCase.expectedLine);
                    QCOMPARE(frame.visual.feature.outcome,
                             testCase.expectedOutcome);
                    QCOMPARE(frame.visual.simulation.featureOutcome,
                             testCase.expectedOutcome);
                    const WorkoutGameRoute expectedRoute =
                            testCase.expectedOutcome
                                == WorkoutGameFeatureOutcome::Completed
                            ? WorkoutGameRoute::MainLine
                            : WorkoutGameRoute::SafeBypass;
                    QCOMPARE(frame.visual.feature.route, expectedRoute);
                    QCOMPARE(frame.visual.simulation.route, expectedRoute);
                    QCOMPARE(frame.visual.feature.launchSpeedReady, true);
                    QCOMPARE(frame.visual.feature.launchPowerReady,
                             testCase.expectedOutcome
                                == WorkoutGameFeatureOutcome::Completed);
                } else if (observedLock) {
                    QFAIL("gap line lock was lost before leaving the section");
                }

                if (testCase.expectedOutcome
                        == WorkoutGameFeatureOutcome::Completed) {
                    if (frame.visual.feature.triggerJump
                            && jumpStage == JumpStage::Approach) {
                        jumpStage = JumpStage::Takeoff;
                    }
                    if (frame.visual.world.rider.airborne) {
                        QVERIFY(jumpStage == JumpStage::Takeoff
                                || jumpStage == JumpStage::Airborne);
                        QVERIFY2(
                            WorkoutGameFeatureRuntime::airborneExpected(
                                frame.visual.feature),
                            qPrintable(QStringLiteral(
                                "gap flight escaped its runtime action phase "
                                "at %1 ms, phase %2")
                                .arg(timeMs)
                                .arg(int(frame.visual.feature.phase))));
                        jumpStage = JumpStage::Airborne;
                    } else if (jumpStage == JumpStage::Airborne) {
                        jumpStage = JumpStage::Landed;
                    }
                    if (frame.visual.feature.phase
                            == WorkoutGameFeaturePhase::Recovery
                            && !frame.visual.world.rider.airborne
                            && jumpStage == JumpStage::Landed) {
                        jumpStage = JumpStage::GroundedRecovery;
                    }
                } else {
                    QVERIFY(!frame.visual.feature.triggerJump);
                    QVERIFY2(!frame.visual.world.rider.airborne,
                             qPrintable(QStringLiteral(
                                 "safe gap became airborne at %1 ms, phase "
                                 "%2, feature distance %3 m")
                                 .arg(timeMs)
                                 .arg(int(frame.visual.feature.phase))
                                 .arg(frame.visual.feature.visualDistanceMeters,
                                      0, 'f', 3)));
                    if (frame.visual.feature.phase
                            == WorkoutGameFeaturePhase::Recovery) {
                        observedSafeRecovery = true;
                    }
                }
            }
            QVERIFY2(observedLock, "gap scenario never reached line lock");
            QVERIFY(leftGapSection);
            QVERIFY(hasPriorGapPosition);
            QVERIFY(std::abs(finalGapLateralMeters)
                    <= MainTrailMergeToleranceMeters);
            if (testCase.expectedOutcome
                    == WorkoutGameFeatureOutcome::Completed) {
                QCOMPARE(jumpStage, JumpStage::GroundedRecovery);
            } else {
                QVERIFY(observedSafeRecovery);
            }

            double authoredLateralMeters =
                    gapPiece->challenge.bypassLateralMeters;
            if (testCase.expectedLine != WorkoutGameGapJumpLine::None) {
                const auto authoredLine = std::find_if(
                        gapPiece->gapJump.lines.begin(),
                        gapPiece->gapJump.lines.end(),
                        [&testCase](const WorkoutGameRoadGapJumpLine &line) {
                            return line.id == testCase.expectedLine;
                        });
                QVERIFY(authoredLine != gapPiece->gapJump.lines.end());
                authoredLateralMeters = authoredLine->lateralMeters;
            }
            if (std::abs(authoredLateralMeters) > 0.1) {
                QVERIFY(maximumAbsoluteLateralMeters
                        >= 0.75 * std::abs(authoredLateralMeters));
                QVERIFY(movedBackTowardMainTrail);
            } else {
                QVERIFY(maximumAbsoluteLateralMeters
                        <= MainTrailMergeToleranceMeters);
            }
        }
    }

    void gapJumpUsesSectionTargetAndPublishesOneCommittedOutcome()
    {
        const WorkoutGameCourse course = gapJumpCourse();
        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, 200.0, false));

        bool observedLock = false;
        std::uint64_t committedScore = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation.workoutTimeMs = timeMs;
            input.simulation.actualWatts = 200.0;
            input.simulation.targetWatts = 0.0;
            input.simulation.cadenceRpm = 85.0;
            input.simulation.authoritativeSpeedKph = 25.2;
            const WorkoutGameEngineFrame frame =
                    engine.update(input, timeMs);
            if (!frame.visual.feature.gapLineLocked) continue;

            observedLock = true;
            QCOMPARE(frame.visual.feature.lockedGapLine,
                     WorkoutGameGapJumpLine::Long);
            QCOMPARE(frame.visual.feature.outcome,
                     WorkoutGameFeatureOutcome::Completed);
            QCOMPARE(frame.visual.simulation.featureOutcome,
                     WorkoutGameFeatureOutcome::Completed);
            QCOMPARE(frame.visual.simulation.route,
                     WorkoutGameRoute::MainLine);
            QVERIFY(frame.visual.simulation.score >= committedScore);
            committedScore = frame.visual.simulation.score;
        }

        QVERIFY(observedLock);
        QVERIFY(committedScore > 0);
    }

    void completeFeatureReplayIsDeterministicFiniteAndForwardOnly()
    {
        constexpr double FtpWatts = 200.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const int expectedFeatureCount = int(std::count_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameSection &section) {
                    return section.challengeCount > 0;
                }));
        WorkoutGameEngine first;
        WorkoutGameEngine second;
        QVERIFY(first.configure(course, FtpWatts, true));
        QVERIFY(second.configure(course, FtpWatts, true));

        double priorDistance = 0.0;
        WorkoutGameAudioCueTracker audioTracker;
        int featureCueCount = 0;
        int landingCueCount = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            input.heartRate = 148;
            const WorkoutGameEngineFrame left = first.update(
                    input, 100000 + timeMs);
            const WorkoutGameEngineFrame right = second.update(
                    input, 100000 + timeMs);

            QVERIFY(left.visual.simulation.ready);
            QVERIFY(left.visual.world.ready);
            QVERIFY(std::isfinite(left.visual.world.rider.distanceMeters));
            QVERIFY(std::isfinite(left.visual.world.rider.elevationMeters));
            QVERIFY(std::isfinite(left.visual.world.speedMetersPerSecond));
            QVERIFY(std::isfinite(left.visual.riderPedalCycles));
            QVERIFY2(left.visual.world.rider.distanceMeters + 1e-7
                            >= priorDistance,
                     qPrintable(QStringLiteral(
                         "the canonical rider moved backward at %1 ms: %2 -> %3")
                         .arg(timeMs)
                         .arg(priorDistance, 0, 'f', 9)
                         .arg(left.visual.world.rider.distanceMeters,
                              0, 'f', 9)));
            QCOMPARE(left.visual.simulation.workoutTimeMs,
                     right.visual.simulation.workoutTimeMs);
            QCOMPARE(left.visual.world.rider.distanceMeters,
                     right.visual.world.rider.distanceMeters);
            QCOMPARE(left.visual.world.rider.elevationMeters,
                     right.visual.world.rider.elevationMeters);
            QCOMPARE(left.visual.world.rider.pitchDegrees,
                     right.visual.world.rider.pitchDegrees);
            QCOMPARE(left.visual.riderPedalCycles,
                     right.visual.riderPedalCycles);
            QCOMPARE(left.audioEvents.count, right.audioEvents.count);
            QCOMPARE(left.audioEvents.epoch, right.audioEvents.epoch);
            const WorkoutGameAudioEvents audio = audioTracker.update(
                    left.audioEvents);
            if (audio.feature) ++featureCueCount;
            if (audio.landing) ++landingCueCount;
            priorDistance = left.visual.world.rider.distanceMeters;
        }
        QVERIFY(priorDistance > 100.0);
        QCOMPARE(featureCueCount, expectedFeatureCount);
        QVERIFY(landingCueCount > 0);
    }

    void completeReplayDigestIsStableAcrossTwentyRuns()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const std::uint64_t expected = replayDigest(
                course, WorkoutGameFeatureLabScenario::Pass);
        QVERIFY(expected != 0);
        for (int run = 1; run < 20; ++run) {
            QCOMPARE(replayDigest(
                    course, WorkoutGameFeatureLabScenario::Pass), expected);
        }
        QVERIFY(replayDigest(course, WorkoutGameFeatureLabScenario::Bypass)
                != expected);
    }

    void dataGeneratorDeterministicallyCompletesAndBypassesEveryFeature()
    {
        constexpr double FtpWatts = 190.0;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);

        const auto runScenario = [&](TrainingDataGeneratorMode mode,
                                     WorkoutGameFeatureOutcome expected) {
            TrainingDataGenerator generator;
            generator.setMode(mode);
            WorkoutGameEngine engine;
            QVERIFY(engine.configure(course, FtpWatts, true));
            std::vector<bool> observed(course.sections.size(), false);
            double maximumMainLineLateralMeters = 0.0;
            double maximumGapLineLateralMeters = 0.0;
            double maximumTraceLateralStepMeters = 0.0;
            double priorTraceLateralMeters = 0.0;
            bool hasPriorTraceLateral = false;
            std::int64_t nextTraceTimeMs = 0;

            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                const WorkoutGameFeatureLabScenario labScenario =
                        expected == WorkoutGameFeatureOutcome::Completed
                        ? WorkoutGameFeatureLabScenario::Pass
                        : WorkoutGameFeatureLabScenario::Bypass;
                input.simulation = WorkoutGameFeatureLab::input(
                        course, timeMs, labScenario);
                generator.setTargetWatts(input.simulation.targetWatts);
                const TrainingDataGeneratorSample sample =
                        generator.nextSample();
                // RealtimeData stores watts as an integer in the live path.
                input.simulation.actualWatts = std::floor(sample.watts);
                input.simulation.cadenceRpm = sample.cadence;
                input.simulation.authoritativeSpeedKph = -1.0;
                input.heartRate = int(std::lround(sample.heartRate));

                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (frame.visual.feature.terrain
                            == WorkoutGameTerrainKind::GapJump
                        && frame.visual.feature.gapLineLocked) {
                    QCOMPARE(frame.visual.simulation.featureOutcome,
                             frame.visual.feature.outcome);
                    QCOMPARE(frame.visual.simulation.route,
                             frame.visual.feature.route);
                    QCOMPARE(frame.visual.simulation.challengeReadiness,
                             frame.visual.feature.readiness);
                }
                const int section = frame.visual.simulation.activeSection;
                if (section >= 0
                        && section < int(course.sections.size())
                        && course.sections[std::size_t(section)].challengeCount > 0
                        && frame.visual.simulation.featureOutcome == expected) {
                    observed[std::size_t(section)] = true;
                }
                if (expected == WorkoutGameFeatureOutcome::Completed) {
                    const double lateral = std::abs(
                            frame.visual.feature.lateralOffsetMeters);
                    if (frame.visual.feature.terrain
                            == WorkoutGameTerrainKind::GapJump) {
                        maximumGapLineLateralMeters = std::max(
                                maximumGapLineLateralMeters, lateral);
                    } else {
                        maximumMainLineLateralMeters = std::max(
                                maximumMainLineLateralMeters, lateral);
                    }
                    const WorkoutGameRiderVisualPose pose =
                            WorkoutGameRiderVisual::pose(
                                frame.visual.world,
                                frame.visual.feature,
                                frame.watts);
                    if (pose.airborne && pose.airHeightMeters >= 0.08) {
                        QVERIFY2(
                            WorkoutGameFeatureRuntime::airborneExpected(
                                frame.visual.feature),
                            qPrintable(QStringLiteral(
                                "unexpected air at %1 ms in section %2")
                                .arg(timeMs).arg(section)));
                    }
                }
                if (timeMs >= nextTraceTimeMs) {
                    const double lateral =
                            frame.visual.feature.lateralOffsetMeters;
                    if (hasPriorTraceLateral) {
                        maximumTraceLateralStepMeters = std::max(
                                maximumTraceLateralStepMeters,
                                std::abs(lateral
                                    - priorTraceLateralMeters));
                    }
                    priorTraceLateralMeters = lateral;
                    hasPriorTraceLateral = true;
                    nextTraceTimeMs += 250;
                }
            }

            int observedFeatures = 0;
            for (std::size_t section = 0; section < course.sections.size();
                 ++section) {
                if (course.sections[section].challengeCount <= 0) continue;
                QVERIFY2(observed[section], qPrintable(QStringLiteral(
                    "generator mode %1 did not produce outcome %2 for section %3")
                    .arg(int(mode)).arg(int(expected)).arg(section)));
                ++observedFeatures;
            }
            QCOMPARE(observedFeatures, int(std::count_if(
                    course.sections.begin(), course.sections.end(),
                    [](const WorkoutGameSection &section) {
                        return section.challengeCount > 0;
                    })));
            if (expected == WorkoutGameFeatureOutcome::Completed) {
                QVERIFY(maximumMainLineLateralMeters < 0.01);
                QVERIFY(maximumGapLineLateralMeters <= 2.5);
            } else {
                QVERIFY2(maximumTraceLateralStepMeters < 1.0,
                         qPrintable(QStringLiteral(
                             "safe line moved %1 m in one trace interval")
                             .arg(maximumTraceLateralStepMeters)));
            }
        };

        runScenario(TrainingDataGeneratorMode::OnTarget,
                    WorkoutGameFeatureOutcome::Completed);
        runScenario(TrainingDataGeneratorMode::UnderTarget,
                    WorkoutGameFeatureOutcome::Bypassed);
    }

    void enginePreservesTheAuthoritativeInputTargetInFeatureLab()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, 200.0, true));

        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        input.simulation.actualWatts = 137.0;
        input.simulation.targetWatts = 137.0;
        const WorkoutGameEngineFrame frame = engine.update(input, 100000);

        QCOMPARE(frame.targetWatts, 137.0);
    }

    void skippedTimeResynchronizesWithoutCatchupSimulation()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        const WorkoutGameEngineFrame before = engine.update(input, 1000);

        input.simulation = WorkoutGameFeatureLab::input(
                course, 5000, WorkoutGameFeatureLabScenario::Pass);
        engine.resynchronize(input, 6000, 250);
        input.simulation.workoutTimeMs = 5020;
        const WorkoutGameEngineFrame after = engine.update(input, 6020, 250);
        const WorkoutGameRoadTimelineSample road =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(
                    WorkoutGameRoadCourseBuilder::build(course, 200.0), 5020);

        QCOMPARE(after.visual.simulation.droppedCatchupMs, std::int64_t(0));
        QCOMPARE(after.skippedTicks, std::size_t(250));
        QVERIFY(road.ready);
        QCOMPARE(after.visual.world.rider.distanceMeters,
                 road.distanceMeters);
        const double expectedCycles =
                input.simulation.cadenceRpm * 20.0 / 60000.0;
        QVERIFY(std::abs((after.visual.riderPedalCycles
                          - before.visual.riderPedalCycles)
                         - expectedCycles) < 1.0e-9);
    }

    void riderPedallingUsesPresentationTimeInsteadOfWorkoutProgressRate()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        input.simulation.cadenceRpm = 60.0;

        const WorkoutGameEngineFrame before = engine.update(input, 1000);
        input.simulation.workoutTimeMs = 250;
        const WorkoutGameEngineFrame after = engine.update(input, 2000);

        // One real second at 60 rpm is one full pedal cycle even when the
        // distance-course workout timeline advances at only quarter speed.
        QVERIFY(std::abs((after.visual.riderPedalCycles
                          - before.visual.riderPedalCycles) - 1.0) < 1.0e-9);
    }

    void riderPedallingDoesNotCatchUpPausedPresentationTime()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 0, WorkoutGameFeatureLabScenario::Pass);
        input.simulation.cadenceRpm = 60.0;

        const WorkoutGameEngineFrame before = engine.update(input, 1000);
        input.simulation.paused = true;
        input.simulation.workoutTimeMs = 250;
        const WorkoutGameEngineFrame paused = engine.update(input, 5000);
        QCOMPARE(paused.visual.riderPedalCycles,
                 before.visual.riderPedalCycles);

        input.simulation.paused = false;
        input.simulation.workoutTimeMs = 255;
        const WorkoutGameEngineFrame resumed = engine.update(input, 5020);
        QVERIFY(std::abs((resumed.visual.riderPedalCycles
                          - paused.visual.riderPedalCycles) - 0.02) < 1.0e-9);
    }

    void nonFiniteTelemetryCannotPoisonPublishedFrame()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.workoutTimeMs = 0;
        input.simulation.actualWatts =
                std::numeric_limits<double>::quiet_NaN();
        input.simulation.targetWatts =
                std::numeric_limits<double>::infinity();
        input.simulation.cadenceRpm =
                std::numeric_limits<double>::infinity();
        input.simulation.authoritativeSpeedKph =
                std::numeric_limits<double>::quiet_NaN();
        input.simulation.drivetrainSpeedLimitKph =
                std::numeric_limits<double>::infinity();

        const WorkoutGameEngineFrame frame = engine.update(input, 1000);
        QVERIFY(frame.visual.simulation.ready);
        QVERIFY(std::isfinite(frame.visual.riderPedalCycles));
        QVERIFY(std::isfinite(frame.watts));
        QVERIFY(std::isfinite(frame.targetWatts));
        QCOMPARE(frame.cadenceRpm, 0);
    }

    void extremeFiniteTelemetryIsBoundedBeforeSimulation()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        QVERIFY(engine.configure(course, 200.0, false));
        WorkoutGameEngineInput input;
        input.simulation.workoutTimeMs = 0;
        input.simulation.actualWatts = std::numeric_limits<double>::max();
        input.simulation.targetWatts = std::numeric_limits<double>::max();
        input.simulation.cadenceRpm = std::numeric_limits<double>::max();
        input.simulation.authoritativeSpeedKph =
                std::numeric_limits<double>::max();
        input.simulation.drivetrainSpeedLimitKph =
                std::numeric_limits<double>::max();
        input.heartRate = std::numeric_limits<int>::max();

        const WorkoutGameEngineFrame frame = engine.update(input, 1000);
        QVERIFY(frame.visual.simulation.ready);
        QVERIFY(std::isfinite(frame.visual.riderPedalCycles));
        QCOMPARE(frame.watts, 10000.0);
        QCOMPARE(frame.targetWatts, 10000.0);
        QCOMPARE(frame.cadenceRpm, 300);
        QCOMPARE(frame.heartRate, 300);
    }

    void framePublishesOneAuthoritativeRoadAndFeaturePosition()
    {
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(engine.configure(course, 200.0, true));
        WorkoutGameEngineInput input;
        input.simulation = WorkoutGameFeatureLab::input(
                course, 5000, WorkoutGameFeatureLabScenario::Pass);

        const WorkoutGameEngineFrame frame = engine.update(input, 6000);
        const WorkoutGameRoadTimelineSample expected =
                WorkoutGameRoadCourseBuilder::sampleAtWorkoutTime(road, 5000);
        QVERIFY(expected.ready);
        QVERIFY(frame.visual.feature.ready);
        QCOMPARE(frame.visual.world.rider.distanceMeters,
                 expected.distanceMeters);
        QCOMPARE(frame.visual.feature.visualDistanceMeters,
                 expected.distanceMeters);
        QCOMPARE(frame.visual.feature.sourceSectionIndex,
                 int(expected.sourceSectionIndex));
    }

    void completedLogFeatureProducesAVisibleAirborneArc()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        QVERIFY(engine.configure(course, FtpWatts, true));

        double maximumAirHeightMeters = 0.0;
        double maximumLiftPixels = 0.0;
        int airborneFrames = 0;
        for (std::int64_t timeMs = 0;
             timeMs < course.sections.front().durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            // Keep drivetrain speed far below the 7 m/s course timeline so
            // landing still proves that the timeline owns flight calibration.
            input.simulation.authoritativeSpeedKph = 12.0;
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters,
                    frame.visual.world.rider.airHeightMeters());
            airborneFrames += frame.visual.world.rider.airborne ? 1 : 0;
            const WorkoutGameRiderVisualPose pose =
                    WorkoutGameRiderVisual::pose(
                        frame.visual.world, frame.visual.feature, 188.0);
            maximumLiftPixels = std::max(
                    maximumLiftPixels, pose.liftPixels);
        }

        QVERIFY2(maximumAirHeightMeters >= 0.20,
                 qPrintable(QStringLiteral(
                     "feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
        QVERIFY(airborneFrames >= 8);
        QVERIFY2(maximumLiftPixels >= 65.0,
                 qPrintable(QStringLiteral(
                     "feature produced only %1 px of visible lift")
                     .arg(maximumLiftPixels)));
    }

    void completedTabletopProducesReadableLiftAndDepthCue()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const int tabletopSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *tabletop = challengePieceFor(
                road, tabletopSection);
        QVERIFY(tabletopSection >= 0);
        QVERIFY(tabletop != nullptr);
        QVERIFY(engine.configure(course, FtpWatts, true));
        const WorkoutGameTabletopGeometryProfile profile =
                WorkoutGameTabletopGeometry::profile(tabletop->difficulty);

        double maximumLiftPixels = 0.0;
        double maximumAirHeightMeters = 0.0;
        double maximumAirHeightStepMeters = 0.0;
        double airHeightBeforeMaximumStepMeters = 0.0;
        double airHeightAfterMaximumStepMeters = 0.0;
        std::int64_t maximumAirHeightStepTimeMs = 0;
        double minimumAirborneShadowScale = 1.0;
        int readableAirborneFrames = 0;
        int consecutiveAirborneFrames = 0;
        int maximumConsecutiveAirborneFrames = 0;
        double priorAirHeightMeters = 0.0;
        bool hasPriorAirHeight = false;
        bool priorAirborne = false;
        bool observedLanding = false;
        double landingLocalDistanceMeters = 0.0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Pass);
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection != tabletopSection) {
                continue;
            }
            const WorkoutGameRiderVisualPose pose =
                    WorkoutGameRiderVisual::pose(
                        frame.visual.world, frame.visual.feature, 188.0);
            maximumAirHeightMeters = std::max(
                    maximumAirHeightMeters, pose.airHeightMeters);
            if (hasPriorAirHeight) {
                const double step = std::abs(
                        pose.airHeightMeters - priorAirHeightMeters);
                if (step > maximumAirHeightStepMeters) {
                    maximumAirHeightStepMeters = step;
                    airHeightBeforeMaximumStepMeters = priorAirHeightMeters;
                    airHeightAfterMaximumStepMeters = pose.airHeightMeters;
                    maximumAirHeightStepTimeMs = timeMs;
                }
            }
            priorAirHeightMeters = pose.airHeightMeters;
            hasPriorAirHeight = true;
            consecutiveAirborneFrames = pose.airborne
                    ? consecutiveAirborneFrames + 1 : 0;
            maximumConsecutiveAirborneFrames = std::max(
                    maximumConsecutiveAirborneFrames,
                    consecutiveAirborneFrames);
            maximumLiftPixels = std::max(maximumLiftPixels, pose.liftPixels);
            if (pose.airborne && pose.liftPixels >= 25.0) {
                ++readableAirborneFrames;
                minimumAirborneShadowScale = std::min(
                        minimumAirborneShadowScale, pose.shadowScale);
            }
            if (priorAirborne && !frame.visual.world.rider.airborne) {
                const double physicalDistanceMeters =
                        frame.visual.world.rider.distanceMeters
                        + frame.visual.world.terrainOffsetMeters;
                const double candidateLandingLocalMeters = physicalDistanceMeters
                        - tabletop->challenge.obstacleDistanceMeters;
                if (candidateLandingLocalMeters
                            >= profile.deckEndMeters - 0.15
                        && candidateLandingLocalMeters
                            <= profile.endMeters + 0.25) {
                    landingLocalDistanceMeters = candidateLandingLocalMeters;
                    observedLanding = true;
                }
            }
            priorAirborne = frame.visual.world.rider.airborne;
        }

        QVERIFY2(maximumLiftPixels >= 28.0,
                 qPrintable(QStringLiteral(
                     "tabletop produced only %1 px of visible lift")
                     .arg(maximumLiftPixels)));
        QVERIFY2(readableAirborneFrames >= 8,
                 qPrintable(QStringLiteral(
                     "tabletop produced only %1 readable airborne frames")
                     .arg(readableAirborneFrames)));
        QVERIFY(maximumAirHeightMeters <= 1.8);
        QVERIFY2(maximumConsecutiveAirborneFrames <= 100,
                 qPrintable(QStringLiteral(
                     "tabletop remained airborne for %1 ms")
                     .arg(maximumConsecutiveAirborneFrames * 20)));
        QVERIFY2(maximumAirHeightStepMeters <= 0.25,
                 qPrintable(QStringLiteral(
                     "tabletop air height stepped by %1 m at %2 ms (%3 -> %4)")
                     .arg(maximumAirHeightStepMeters)
                     .arg(maximumAirHeightStepTimeMs)
                     .arg(airHeightBeforeMaximumStepMeters)
                     .arg(airHeightAfterMaximumStepMeters)));
        QVERIFY(minimumAirborneShadowScale <= 0.84);
        QVERIFY(observedLanding);
        QVERIFY(landingLocalDistanceMeters >= profile.deckEndMeters - 0.15);
        QVERIFY2(landingLocalDistanceMeters <= profile.endMeters + 0.25,
                 qPrintable(QStringLiteral(
                     "tabletop landed at local %1 m after profile end %2 m")
                     .arg(landingLocalDistanceMeters)
                     .arg(profile.endMeters)));
    }

    void tabletopEngineGatesLaunchOutsideTheCalibratedSpeedRange()
    {
        constexpr double FtpWatts = 200.0;
        const auto verifyNoLaunch = [FtpWatts](
                double timelineSpeedMetersPerSecond) {
            WorkoutGameCourse course;
            course.status = WorkoutGameCourseStatus::Ready;
            course.seed = 1771u;
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = WorkoutGameTerrainKind::Tabletop;
            section.lengthMeters = 60.0;
            section.durationMs = std::int64_t(std::llround(
                    section.lengthMeters / timelineSpeedMetersPerSecond
                        * 1000.0));
            section.targetWatts = 230.0;
            section.difficulty = 0.68;
            section.challengeCount = 1;
            course.durationMs = section.durationMs;
            course.sections = {section};

            WorkoutGameEngine engine;
            if (!engine.configure(course, FtpWatts, false)) return false;
            bool observedCompletedAction = false;
            for (std::int64_t timeMs = 0; timeMs < course.durationMs;
                 timeMs += 20) {
                WorkoutGameEngineInput input;
                input.simulation.workoutTimeMs = timeMs;
                input.simulation.actualWatts = section.targetWatts;
                input.simulation.targetWatts = section.targetWatts;
                input.simulation.cadenceRpm = 90.0;
                input.simulation.authoritativeSpeedKph = 20.0;
                const WorkoutGameEngineFrame frame = engine.update(
                        input, 100000 + timeMs);
                if (frame.visual.feature.phase
                            == WorkoutGameFeaturePhase::Action
                        && frame.visual.feature.outcome
                            == WorkoutGameFeatureOutcome::Completed) {
                    observedCompletedAction = true;
                    if (frame.visual.feature.triggerJump) return false;
                }
            }
            return observedCompletedAction;
        };

        QVERIFY(verifyNoLaunch(2.5));
        QVERIFY(verifyNoLaunch(10.0));
    }

    void tabletopBypassEntersAtTheBranchWithoutLateralTeleport()
    {
        constexpr double FtpWatts = 200.0;
        WorkoutGameEngine engine;
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const int tabletopSection = sectionForTerrain(
                course, WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadPiece *tabletop = challengePieceFor(
                road, tabletopSection);
        QVERIFY(tabletopSection >= 0);
        QVERIFY(tabletop != nullptr);
        QVERIFY(engine.configure(course, FtpWatts, true));

        bool enteredBypass = false;
        double offsetAtEntry = 0.0;
        double maximumOffsetStep = 0.0;
        double priorOffset = 0.0;
        bool hasPriorOffset = false;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            WorkoutGameEngineInput input;
            input.simulation = WorkoutGameFeatureLab::input(
                    course, timeMs, WorkoutGameFeatureLabScenario::Bypass);
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection != tabletopSection) {
                continue;
            }
            const double offset = frame.visual.feature.lateralOffsetMeters;
            if (hasPriorOffset) {
                maximumOffsetStep = std::max(
                        maximumOffsetStep, std::abs(offset - priorOffset));
            }
            if (!enteredBypass
                    && frame.visual.feature.route
                        == WorkoutGameRoute::SafeBypass) {
                enteredBypass = true;
                offsetAtEntry = offset;
                const double physicalDistanceMeters =
                        frame.visual.world.rider.distanceMeters
                        + frame.visual.world.terrainOffsetMeters;
                QVERIFY(physicalDistanceMeters
                        >= tabletop->challenge.bypassStartDistanceMeters);
                QVERIFY(physicalDistanceMeters
                        < tabletop->challenge.bypassStartDistanceMeters
                            + 0.25);
            }
            priorOffset = offset;
            hasPriorOffset = true;
        }

        QVERIFY(enteredBypass);
        QVERIFY(std::abs(offsetAtEntry) < 0.01);
        QVERIFY2(maximumOffsetStep < 0.08,
                 qPrintable(QStringLiteral(
                     "tabletop bypass moved laterally by %1 m in one frame")
                     .arg(maximumOffsetStep)));
    }

    void bypassCannotActivateTheScriptedAirbornePose()
    {
        WorkoutGameWorldSnapshot world;
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        feature.route = WorkoutGameRoute::SafeBypass;
        feature.verticalOffsetMeters = 1.35;

        const WorkoutGameRiderVisualPose pose =
                WorkoutGameRiderVisual::pose(world, feature, 188.0);
        QVERIFY(!pose.airborne);
        QCOMPARE(pose.airHeightMeters, 0.0);
        QCOMPARE(pose.liftPixels, 0.0);
        QCOMPARE(pose.shadowScale, 1.0);
    }

    void physicsSnapshotOwnsAirHeightOverScriptedFeatureArc()
    {
        WorkoutGameWorldSnapshot world;
        world.ready = true;
        world.rider.airborne = true;
        world.rider.clearanceMeters = 0.82 + 0.37;
        WorkoutGameFeatureRuntimeSnapshot feature;
        feature.ready = true;
        feature.outcome = WorkoutGameFeatureOutcome::Completed;
        feature.route = WorkoutGameRoute::MainLine;
        feature.verticalOffsetMeters = 1.35;

        const WorkoutGameRiderVisualPose pose =
                WorkoutGameRiderVisual::pose(world, feature, 188.0);
        QVERIFY(pose.airborne);
        QVERIFY(std::abs(pose.airHeightMeters - 0.37) < 1e-9);
    }

    void generatedThirtySecondEffortProducesAVisibleJump()
    {
        constexpr double FtpWatts = 190.0;
        const WorkoutGameCourse course = WorkoutGameCourseBuilder::build({
            {0, 60000, 140.0, 140.0},
            {60000, 30000, 250.0, 250.0},
            {90000, 60000, 120.0, 120.0}
        }, FtpWatts, 1234u);
        QCOMPARE(course.sections[1].feature,
                 WorkoutGameFeature::SprintJump);

        WorkoutGameEngine engine;
        QVERIFY(engine.configure(course, FtpWatts, false));
        double maximumAirHeightMeters = 0.0;
        int airborneFrames = 0;
        for (std::int64_t timeMs = 0; timeMs < course.durationMs;
             timeMs += 20) {
            const WorkoutGameSection &section = timeMs < 60000
                    ? course.sections[0]
                    : timeMs < 90000
                    ? course.sections[1] : course.sections[2];
            WorkoutGameEngineInput input;
            input.simulation.workoutTimeMs = timeMs;
            input.simulation.actualWatts = section.targetWatts * 1.2;
            input.simulation.targetWatts = section.targetWatts;
            input.simulation.cadenceRpm = 90.0;
            input.simulation.authoritativeSpeedKph = 20.0;
            const WorkoutGameEngineFrame frame = engine.update(
                    input, 100000 + timeMs);
            if (frame.visual.simulation.activeSection == 1) {
                maximumAirHeightMeters = std::max(
                        maximumAirHeightMeters,
                        frame.visual.world.rider.airHeightMeters());
                airborneFrames += frame.visual.world.rider.airborne ? 1 : 0;
            }
        }

        QVERIFY2(maximumAirHeightMeters >= 0.20,
                 qPrintable(QStringLiteral(
                     "generated feature reached only %1 m air height")
                     .arg(maximumAirHeightMeters)));
        QVERIFY(airborneFrames >= 8);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameEngine)
#include "testWorkoutGameEngine.moc"
