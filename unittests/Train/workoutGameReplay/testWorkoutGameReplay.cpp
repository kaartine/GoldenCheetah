/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameReplay.h"
#include "Train/WorkoutGameAssetCatalog.h"
#include "Train/WorkoutGameAssetPhysicsResolver.h"
#include "Train/WorkoutGameAssetPhysicsSampler.h"
#include "Train/WorkoutGameRoadPlan.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <limits>

namespace {

WorkoutGameReplay featureLabReplay(WorkoutGameFeatureLabScenario scenario)
{
    WorkoutGameReplay replay;
    replay.course = WorkoutGameFeatureLab::course(200.0);
    replay.ftpWatts = 200.0;
    replay.featureLabEnabled = true;
    for (std::int64_t timeMs = 0; timeMs < replay.course.durationMs;
         timeMs += 20) {
        WorkoutGameReplaySample sample;
        sample.input.simulation = WorkoutGameFeatureLab::input(
                replay.course, timeMs, scenario);
        sample.input.heartRate = 138 + int((timeMs / 1000) % 12);
        sample.presentationTimeMs = 250000 + timeMs;
        replay.samples.push_back(sample);
    }
    return replay;
}

}

class TestWorkoutGameReplay : public QObject
{
    Q_OBJECT

private slots:
    void resolvedReplayRetainsSnapshotAcrossCatalogChanges_data()
    {
        QTest::addColumn<bool>("replaceCatalog");
        QTest::addColumn<bool>("bypass");
        QTest::newRow("remove-pass") << false << false;
        QTest::newRow("remove-bypass") << false << true;
        QTest::newRow("replace-pass") << true << false;
        QTest::newRow("replace-bypass") << true << true;
    }

    void resolvedReplayRetainsSnapshotAcrossCatalogChanges()
    {
        QFETCH(bool, replaceCatalog);
        QFETCH(bool, bypass);
        QString error;
        auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));

        WorkoutGameReplay replay;
        replay.ftpWatts = 200.0;
        replay.featureLabEnabled = true;
        replay.course.status = WorkoutGameCourseStatus::Ready;
        replay.course.seed = 0x8f12u;
        replay.course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::LogOver;
        section.durationMs = replay.course.durationMs;
        section.lengthMeters = 150.0;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        replay.course.sections = {section};
        auto plan = std::make_shared<WorkoutGameRoadPlan>(
                WorkoutGameRoadCourseBuilder::generatePlan(replay.course, replay.ftpWatts));
        auto resolution = WorkoutGameAssetPhysicsResolver::resolve(*catalog, plan->pieces);
        QCOMPARE(resolution.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
        QVERIFY(resolution.snapshot);
        QVERIFY(!resolution.snapshot->physicsDefinitions.empty());
        plan->assetPhysicsSnapshot = resolution.snapshot;
        replay.course.roadPlan = plan;
        const auto challenge = std::find_if(plan->pieces.begin(), plan->pieces.end(),
            [](const WorkoutGameRoadPiece &piece) {
                return piece.terrain == WorkoutGameTerrainKind::LogOver && piece.challenge.enabled;
            });
        QVERIFY(challenge != plan->pieces.end());
        const auto pieceIndex = std::size_t(std::distance(plan->pieces.begin(), challenge));
        const auto originalFit = WorkoutGameAssetPhysicsSampler::renderFit(*resolution.snapshot, pieceIndex);
        QCOMPARE(originalFit.status, WorkoutGameAssetRenderFitStatus::Ready);
        QCOMPARE(originalFit.resolvedExtentMm, std::uint32_t(540));
        const double anchor = double(originalFit.obstacleAnchorMm) / 1000.0;
        const auto originalSample = WorkoutGameAssetPhysicsSampler::sample(*resolution.snapshot, pieceIndex, anchor);
        QVERIFY(originalSample.bound);
        QVERIFY(originalSample.surfacePresent);
        QCOMPARE(originalSample.offsetMeters, 0.54);
        std::weak_ptr<const WorkoutGameCourseAssetPhysicsSnapshot> retained = resolution.snapshot;

        for (std::int64_t timeMs = 0; timeMs < replay.course.durationMs; timeMs += 20) {
            WorkoutGameReplaySample sample;
            sample.input.simulation = WorkoutGameFeatureLab::input(replay.course, timeMs,
                    bypass ? WorkoutGameFeatureLabScenario::Bypass : WorkoutGameFeatureLabScenario::Pass);
            sample.input.heartRate = 140;
            sample.presentationTimeMs = 100000 + timeMs;
            replay.samples.push_back(sample);
        }
        const auto baseline = WorkoutGameReplayHarness::run(replay);
        QVERIFY(baseline.passed);
        QCOMPARE(baseline.frameStateHashes.size(), replay.samples.size());
        QVERIFY(baseline.finalFrame.visual.simulation.ready);
        QVERIFY(baseline.finalFrame.visual.world.ready);
        QVERIFY(baseline.finalFrame.visual.simulation.courseProgress > 0.9);
        QCOMPARE(baseline.finalFrame.visual.simulation.featureOutcome,
                 bypass ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed);

        if (replaceCatalog) {
            QFile source(QStringLiteral(":/json/workout-game-asset-catalog.json"));
            QVERIFY(source.open(QIODevice::ReadOnly));
            auto root = QJsonDocument::fromJson(source.readAll()).object();
            auto profiles = root.value(QStringLiteral("profiles")).toArray();
            bool replaced = false;
            for (qsizetype index = 0; index < profiles.size(); ++index) {
                auto profile = profiles[index].toObject();
                if (profile.value(QStringLiteral("profileId")).toString()
                        != QStringLiteral("FT-02-log-over-v1")) continue;
                auto scale = profile.value(QStringLiteral("difficultyScale")).toObject();
                scale.insert(QStringLiteral("baseExtentMm"), 540);
                profile.insert(QStringLiteral("difficultyScale"), scale);
                profiles[index] = profile;
                replaced = true;
            }
            QVERIFY(replaced);
            root.insert(QStringLiteral("profiles"), profiles);
            catalog = WorkoutGameAssetCatalog::fromJson(QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
            QVERIFY2(catalog, qPrintable(error));
            const auto next = WorkoutGameAssetPhysicsResolver::resolve(*catalog, plan->pieces);
            QCOMPARE(next.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
            QVERIFY(next.snapshot);
            const auto nextFit = WorkoutGameAssetPhysicsSampler::renderFit(*next.snapshot, pieceIndex);
            QCOMPARE(nextFit.status, WorkoutGameAssetRenderFitStatus::Ready);
            QCOMPARE(nextFit.resolvedExtentMm, std::uint32_t(640));
            const auto nextSample = WorkoutGameAssetPhysicsSampler::sample(*next.snapshot, pieceIndex, anchor);
            QVERIFY(nextSample.bound);
            QVERIFY(nextSample.surfacePresent);
            QCOMPARE(nextSample.offsetMeters, 0.64);
            QVERIFY(nextSample.offsetMeters != originalSample.offsetMeters);
            // The replacement changes newly resolved snapshot geometry, not the
            // course already retained by this replay. No global catalog API
            // exists: catalogs are immutable inputs to the resolver.
        } else {
            catalog.reset();
        }

        plan.reset();
        resolution.snapshot.reset();
        QVERIFY(!retained.expired());
        for (int repetition = 0; repetition < 3; ++repetition) {
            const auto repeated = WorkoutGameReplayHarness::run(replay);
            QVERIFY(repeated.passed);
            QCOMPARE(repeated.frameStateHashes, baseline.frameStateHashes);
            QCOMPARE(repeated.finalStateHash, baseline.finalStateHash);
            const auto frozen = retained.lock();
            QVERIFY(frozen);
            QCOMPARE(WorkoutGameAssetPhysicsSampler::renderFit(*frozen, pieceIndex).resolvedExtentMm,
                     std::uint32_t(540));
            QCOMPARE(WorkoutGameAssetPhysicsSampler::sample(*frozen, pieceIndex, anchor).offsetMeters, 0.54);
        }
        replay.course.roadPlan.reset();
        QVERIFY(retained.expired());
    }

    void completeReplaysAreBitExactWithinOneBuild()
    {
        const WorkoutGameReplay pass = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        const WorkoutGameReplay bypass = featureLabReplay(
                WorkoutGameFeatureLabScenario::Bypass);
        const WorkoutGameReplayResult passBaseline =
                WorkoutGameReplayHarness::run(pass);
        const WorkoutGameReplayResult bypassBaseline =
                WorkoutGameReplayHarness::run(bypass);
        QVERIFY(passBaseline.passed);
        QVERIFY(bypassBaseline.passed);
        QVERIFY(passBaseline.finalStateHash != 0);
        QVERIFY(bypassBaseline.finalStateHash != 0);
        QVERIFY(passBaseline.finalStateHash != bypassBaseline.finalStateHash);
        QCOMPARE(passBaseline.frameStateHashes.size(), pass.samples.size());

        for (int repetition = 0; repetition < 12; ++repetition) {
            const WorkoutGameReplayResult result =
                    WorkoutGameReplayHarness::run(
                        repetition & 1 ? bypass : pass);
            QVERIFY(result.passed);
            QCOMPARE(result.finalStateHash,
                     repetition & 1
                        ? bypassBaseline.finalStateHash
                        : passBaseline.finalStateHash);
            QCOMPARE(result.frameStateHashes,
                     repetition & 1
                        ? bypassBaseline.frameStateHashes
                        : passBaseline.frameStateHashes);
        }
    }

    void pauseAndResumeReplayPreservesProgressAndHash()
    {
        WorkoutGameReplay replay;
        replay.course = WorkoutGameFeatureLab::course(200.0);
        replay.ftpWatts = 200.0;
        replay.featureLabEnabled = true;
        std::int64_t presentationTimeMs = 10000;
        for (std::int64_t timeMs = 0; timeMs <= 3000; timeMs += 20) {
            WorkoutGameReplaySample sample;
            sample.input.simulation = WorkoutGameFeatureLab::input(
                    replay.course, timeMs,
                    WorkoutGameFeatureLabScenario::Pass);
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }
        for (int frame = 0; frame < 15; ++frame) {
            WorkoutGameReplaySample sample = replay.samples.back();
            sample.input.simulation.paused = true;
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }
        for (std::int64_t timeMs = 3020; timeMs <= 6000; timeMs += 20) {
            WorkoutGameReplaySample sample;
            sample.input.simulation = WorkoutGameFeatureLab::input(
                    replay.course, timeMs,
                    WorkoutGameFeatureLabScenario::Pass);
            sample.presentationTimeMs = presentationTimeMs;
            replay.samples.push_back(sample);
            presentationTimeMs += 20;
        }

        const WorkoutGameReplayResult first =
                WorkoutGameReplayHarness::run(replay);
        const WorkoutGameReplayResult second =
                WorkoutGameReplayHarness::run(replay);
        QVERIFY(first.passed);
        QVERIFY(second.passed);
        QCOMPARE(first.finalStateHash, second.finalStateHash);
        QVERIFY(first.finalFrame.visual.simulation.workoutTimeMs >= 6000);
        QVERIFY(first.finalFrame.visual.world.rider.distanceMeters > 0.0);
    }

    void oneInputMutationChangesTheReplayHash()
    {
        WorkoutGameReplay original = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        WorkoutGameReplay changed = original;
        changed.samples[changed.samples.size() / 2]
                .input.simulation.actualWatts += 1.0;
        const WorkoutGameReplayResult left =
                WorkoutGameReplayHarness::run(original);
        const WorkoutGameReplayResult right =
                WorkoutGameReplayHarness::run(changed);
        QVERIFY(left.passed);
        QVERIFY(right.passed);
        QVERIFY(left.finalStateHash != right.finalStateHash);
    }

    void rejectsUnsupportedOrInvalidStreams()
    {
        WorkoutGameReplay replay = featureLabReplay(
                WorkoutGameFeatureLabScenario::Pass);
        replay.formatVersion = WorkoutGameReplay::CurrentFormatVersion + 1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::UnsupportedFormat);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[1].presentationTimeMs =
                replay.samples[0].presentationTimeMs - 1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::PresentationTimeRegression);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[0].input.simulation.workoutTimeMs = -1;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::InvalidInput);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[2].input.simulation.workoutTimeMs = 10;
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::WorkoutTimeRegression);

        replay = featureLabReplay(WorkoutGameFeatureLabScenario::Pass);
        replay.samples[1].input.simulation.actualWatts =
                std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(WorkoutGameReplayHarness::run(replay).failure,
                 WorkoutGameReplayFailure::InvalidInput);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameReplay)
#include "testWorkoutGameReplay.moc"
