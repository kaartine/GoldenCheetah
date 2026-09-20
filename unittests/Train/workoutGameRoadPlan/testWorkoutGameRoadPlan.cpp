/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameRoadPlan.h"
#include "Train/WorkoutGameRoadQuality.h"

#include <QTest>

#include <cmath>
#include <limits>

namespace {

constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;

WorkoutGameRoadPlan planWithTurns(
        const std::vector<double> &turnDegrees,
        double pieceLengthMeters = 20.0)
{
    WorkoutGameRoadPlan plan;
    plan.generationVersion = WorkoutGameRoadPlan::CurrentGenerationVersion;
    WorkoutGameRoadConnector connector;
    for (std::size_t index = 0; index < turnDegrees.size(); ++index) {
        WorkoutGameRoadPiece piece;
        piece.sourceSectionIndex = 0;
        piece.startDistanceMeters = double(index) * pieceLengthMeters;
        piece.lengthMeters = pieceLengthMeters;
        piece.turnRadians = turnDegrees[index] * DegreesToRadians;
        piece.relief.enabled = true;
        piece.relief.phaseRadians = 0.25;
        piece.relief.constantCoefficientMeters = 0.4;
        piece.relief.cosineCoefficientMeters = 0.1;
        piece.relief.sineCoefficientMeters = 0.1;
        piece.geometryAnchorDistanceMeters = piece.startDistanceMeters
                + piece.lengthMeters * 0.5;
        piece.entry = connector;
        piece.exit = connector;
        piece.exit.headingRadians += piece.turnRadians;
        piece.exit.zMeters += piece.lengthMeters;
        connector = piece.exit;
        plan.pieces.push_back(piece);
    }
    return plan;
}

WorkoutGameAssetPhysicsDefinition triangularDefinition(
        std::int32_t heightMm = 540)
{
    WorkoutGameAssetPhysicsDefinition definition;
    definition.operation = WorkoutGameAssetPhysicsOperation::AddObstacle;
    definition.coulombFrictionMilli = 1100;
    definition.restitutionMilli = 0;
    definition.chains = {{
        {{-270, 0}, {0, heightMm}, {270, 0}}
    }};
    return definition;
}

}

class TestWorkoutGameRoadPlan : public QObject
{
    Q_OBJECT

private slots:
    void deterministicPlanMeetsRollingQualityContract()
    {
        std::vector<double> turns;
        for (int index = 0; index < 50; ++index) {
            double magnitude = 15.0;
            if (index == 8 || index == 24 || index == 40) magnitude = 80.0;
            turns.push_back((index & 1) == 0 ? magnitude : -magnitude);
        }
        const WorkoutGameRoadPlan plan = planWithTurns(turns);

        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
        const WorkoutGameRoadQualityReport first =
                WorkoutGameRoadQuality::audit(plan);
        const WorkoutGameRoadQualityReport repeated =
                WorkoutGameRoadQuality::audit(plan);
        QVERIFY(first.accepted());
        QCOMPARE(first.violations, repeated.violations);
    }

    void qualityRejectsLongNearStraightAndWeakRollingWindow()
    {
        const WorkoutGameRoadPlan plan = planWithTurns({0.0, 0.0, 0.0,
                10.0, -10.0, 10.0}, 20.0);
        const WorkoutGameRoadQualityReport report =
                WorkoutGameRoadQuality::audit(plan);

        QVERIFY(!report.accepted());
        QVERIFY(report.contains(
                WorkoutGameRoadQualityViolation::NearStraightTooLong));
        QVERIFY(report.contains(
                WorkoutGameRoadQualityViolation::RollingWindowTooFewBends));
        QVERIFY(report.contains(
                WorkoutGameRoadQualityViolation::RollingWindowTooLittleTurn));
    }

    void exactFeatureSafetyZoneIsTheOnlyExemption()
    {
        WorkoutGameRoadPlan plan = planWithTurns({15.0, -15.0, 0.0, 15.0,
                -15.0, 15.0}, 20.0);
        WorkoutGameRoadPiece &feature = plan.pieces[2];
        feature.challenge.enabled = true;
        feature.challenge.prepareDistanceMeters = 40.0;
        feature.challenge.decisionDistanceMeters = 45.0;
        feature.challenge.obstacleDistanceMeters = 50.0;
        feature.challenge.bypassStartDistanceMeters = 40.0;
        feature.challenge.bypassEndDistanceMeters = 60.0;
        feature.challenge.profile.enabled = true;
        feature.qualityExempt = true;
        feature.qualityExemptionStartDistanceMeters = 40.0;
        feature.qualityExemptionEndDistanceMeters = 60.0;

        QVERIFY(WorkoutGameRoadQuality::audit(plan).accepted());

        feature.challenge.enabled = false;
        const WorkoutGameRoadQualityReport withoutExactExemption =
                WorkoutGameRoadQuality::audit(plan);
        QVERIFY(!withoutExactExemption.accepted());
        QVERIFY(withoutExactExemption.contains(
                WorkoutGameRoadQualityViolation::NearStraightTooLong));
    }

    void rollableFeatureDecisionMayFollowItsGeometryAnchor()
    {
        WorkoutGameRoadPlan plan = planWithTurns({15.0, -15.0, 0.0}, 20.0);
        WorkoutGameRoadPiece &feature = plan.pieces[2];
        feature.terrain = WorkoutGameTerrainKind::Rollers;
        feature.challenge.enabled = true;
        feature.challenge.prepareDistanceMeters = 42.0;
        feature.challenge.decisionDistanceMeters = 56.0;
        feature.challenge.obstacleDistanceMeters = 52.0;
        feature.challenge.bypassStartDistanceMeters = 56.0;
        feature.challenge.bypassEndDistanceMeters = 56.0;
        feature.challenge.profile.enabled = true;
        feature.qualityExempt = true;
        feature.qualityExemptionStartDistanceMeters = 40.0;
        feature.qualityExemptionEndDistanceMeters = 60.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);

        feature.challenge.decisionDistanceMeters = 61.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
        feature.challenge.decisionDistanceMeters = 56.0;
        feature.challenge.prepareDistanceMeters = 57.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void jumpDecisionMustNotFollowTakeoff_data()
    {
        QTest::addColumn<int>("terrain");
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop);
        QTest::newRow("drop") << int(WorkoutGameTerrainKind::Drop);
        QTest::newRow("log-over")
                << int(WorkoutGameTerrainKind::LogOver);
        QTest::newRow("tabletop")
                << int(WorkoutGameTerrainKind::Tabletop);
        QTest::newRow("gap-jump")
                << int(WorkoutGameTerrainKind::GapJump);
    }

    void jumpDecisionMustNotFollowTakeoff()
    {
        QFETCH(int, terrain);
        WorkoutGameRoadPlan plan = planWithTurns({15.0, -15.0, 0.0}, 20.0);
        WorkoutGameRoadPiece &feature = plan.pieces[2];
        feature.terrain = WorkoutGameTerrainKind(terrain);
        feature.challenge.enabled = true;
        feature.challenge.prepareDistanceMeters = 42.0;
        feature.challenge.decisionDistanceMeters = 56.0;
        feature.challenge.obstacleDistanceMeters = 52.0;
        feature.challenge.bypassStartDistanceMeters = 45.0;
        feature.challenge.bypassEndDistanceMeters = 60.0;
        feature.challenge.profile.enabled = true;
        feature.qualityExempt = true;
        feature.qualityExemptionStartDistanceMeters = 40.0;
        feature.qualityExemptionEndDistanceMeters = 60.0;

        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
        feature.challenge.decisionDistanceMeters = 45.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
    }

    void legacyBermMayUseItsAuthoredTurnRange()
    {
        constexpr double DegreesToRadians =
                3.14159265358979323846 / 180.0;
        WorkoutGameRoadPlan plan = planWithTurns({15.0, 95.0, -15.0});
        plan.generationVersion = WorkoutGameRoadPlan::LegacyGenerationVersion;
        for (WorkoutGameRoadPiece &piece : plan.pieces) {
            piece.bank = WorkoutGameRoadBankProfile();
            piece.relief = WorkoutGameRoadReliefProfile();
        }
        plan.pieces[1].terrain = WorkoutGameTerrainKind::Berm;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
        QVERIFY(!WorkoutGameRoadQuality::audit(plan).contains(
                    WorkoutGameRoadQualityViolation::TurnExceedsBound));

        plan.pieces[1].terrain = WorkoutGameTerrainKind::SmoothTrail;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
        plan.pieces[1].terrain = WorkoutGameTerrainKind::Berm;
        plan.pieces[1].turnRadians = 101.0 * DegreesToRadians;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void unsupportedGenerationAndNonFiniteDataFailClosed()
    {
        WorkoutGameRoadPlan plan = planWithTurns({15.0, -15.0, 15.0});
        plan.generationVersion =
                WorkoutGameRoadPlan::CurrentGenerationVersion + 1;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::UnsupportedVersion);

        plan.generationVersion = WorkoutGameRoadPlan::CurrentGenerationVersion;
        plan.pieces[1].turnRadians =
                std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void excessivePieceCountIsRejectedBeforeTraversal()
    {
        WorkoutGameRoadPlan plan;
        plan.generationVersion = WorkoutGameRoadPlan::CurrentGenerationVersion;
        plan.pieces.resize(WorkoutGameRoadPlan::MaximumPieces + 1);
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::ResourceLimit);
    }

    void snapshotBuilderDeduplicatesCanonicalDefinitionsAndBindings()
    {
        WorkoutGameAssetPhysicsSnapshotBuilder builder;
        std::uint32_t firstDefinition = 99;
        std::uint32_t duplicateDefinition = 99;
        std::uint32_t secondDefinition = 99;
        QVERIFY(builder.internDefinition(
                    triangularDefinition(), firstDefinition));
        QVERIFY(builder.internDefinition(
                    triangularDefinition(), duplicateDefinition));
        QVERIFY(builder.internDefinition(
                    triangularDefinition(640), secondDefinition));
        QCOMPARE(firstDefinition, std::uint32_t(0));
        QCOMPARE(duplicateDefinition, firstDefinition);
        QCOMPARE(secondDefinition, std::uint32_t(1));

        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log-over-greybox");
        binding.variantKey = QStringLiteral("default");
        binding.definitionIndex = firstDefinition;
        binding.nativeForwardOriginMm = -1020;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        std::uint32_t firstBinding = 99;
        std::uint32_t duplicateBinding = 99;
        QVERIFY(builder.internBinding(binding, firstBinding));
        QVERIFY(builder.internBinding(binding, duplicateBinding));
        QCOMPARE(firstBinding, std::uint32_t(0));
        QCOMPARE(duplicateBinding, firstBinding);

        WorkoutGameAssetPhysicsPieceBinding pieceBinding;
        pieceBinding.bindingIndex = firstBinding;
        pieceBinding.definitionIndex = firstDefinition;
        pieceBinding.obstacleAnchorMm = 25000;
        QVERIFY(builder.appendPieceBinding(pieceBinding));
        binding.definitionIndex = secondDefinition;
        std::uint32_t secondBinding = 99;
        QVERIFY(builder.internBinding(binding, secondBinding));
        pieceBinding.bindingIndex = secondBinding;
        pieceBinding.definitionIndex = secondDefinition;
        QVERIFY(builder.appendPieceBinding(pieceBinding));
        const std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
                snapshot = builder.finish();
        QVERIFY(snapshot);
        QCOMPARE(snapshot->physicsDefinitions.size(), std::size_t(2));
        QCOMPARE(snapshot->bindings.size(), std::size_t(2));
        QCOMPARE(snapshot->pieceBindings.size(), std::size_t(2));
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    *snapshot, 2),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready);
    }

    void snapshotCanonicalArraysRejectDuplicateUnusedAndOutOfOrderEntries()
    {
        WorkoutGameCourseAssetPhysicsSnapshot snapshot;
        snapshot.physicsDefinitions = {triangularDefinition(), triangularDefinition(640)};
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log");
        binding.definitionIndex = 0;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        snapshot.bindings.push_back(binding);
        binding.definitionIndex = 1;
        snapshot.bindings.push_back(binding);
        WorkoutGameAssetPhysicsPieceBinding piece;
        piece.bindingIndex = 0;
        piece.definitionIndex = 0;
        snapshot.pieceBindings.push_back(piece);
        piece.bindingIndex = 1;
        piece.definitionIndex = 1;
        snapshot.pieceBindings.push_back(piece);
        using Validator = WorkoutGameAssetPhysicsSnapshotValidator;
        using Status = WorkoutGameAssetPhysicsSnapshotValidationStatus;
        QCOMPARE(Validator::validate(snapshot, 2), Status::Ready);

        auto bad = snapshot;
        bad.physicsDefinitions[1] = bad.physicsDefinitions[0];
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
        bad = snapshot;
        bad.bindings[1] = bad.bindings[0];
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
        bad = snapshot;
        bad.pieceBindings[1].bindingIndex = 0;
        bad.pieceBindings[1].definitionIndex = 0;
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
        bad = snapshot;
        bad.bindings[1].definitionIndex = 0;
        bad.pieceBindings[1].definitionIndex = 0;
        bad.bindings[1].variantKey = QStringLiteral("second");
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
        bad = snapshot;
        std::swap(bad.pieceBindings[0], bad.pieceBindings[1]);
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
        bad = snapshot;
        bad.bindings[0].definitionIndex = 1;
        bad.bindings[1].definitionIndex = 0;
        bad.pieceBindings[0].definitionIndex = 1;
        bad.pieceBindings[1].definitionIndex = 0;
        QCOMPARE(Validator::validate(bad, 2), Status::InvalidSnapshot);
    }

    void snapshotSupportsIndependentPhysicsAndVisualBindings()
    {
        using Snapshot = WorkoutGameCourseAssetPhysicsSnapshot;
        using Status = WorkoutGameAssetPhysicsSnapshotValidationStatus;
        WorkoutGameAssetPhysicsSnapshotBuilder builder(1);
        std::uint32_t definition = Snapshot::NoIndex;
        QVERIFY(builder.internDefinition(triangularDefinition(), definition));
        WorkoutGameAssetPhysicsPieceBinding physicsOnly;
        physicsOnly.definitionIndex = definition;
        QVERIFY(builder.appendPieceBinding(physicsOnly));

        WorkoutGameAssetPhysicsBinding visual;
        visual.assetId = QStringLiteral("EN-03-visual");
        visual.definitionIndex = Snapshot::NoIndex;
        visual.nativeForwardExtentMm = 540;
        visual.nativeUpExtentMm = 540;
        visual.resolvedExtentMm = 540;
        std::uint32_t binding = Snapshot::NoIndex;
        QVERIFY(builder.internBinding(visual, binding));
        WorkoutGameAssetPhysicsPieceBinding visualOnly;
        visualOnly.bindingIndex = binding;
        QVERIFY(builder.appendPieceBinding(visualOnly));
        QVERIFY(builder.appendPieceBinding({})); // Explicit no asset/no profile.
        const auto snapshot = builder.finish();
        QVERIFY(snapshot);
        QCOMPARE(snapshot->catalogSchemaVersion, std::uint32_t(1));
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(*snapshot, 3), Status::Ready);
        auto invalid = *snapshot;
        invalid.catalogSchemaVersion = 2;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(invalid, 3), Status::UnsupportedVersion);
        invalid = *snapshot;
        invalid.bindings[0].definitionIndex = 0; // Visual piece still says no physics.
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(invalid, 3), Status::InvalidSnapshot);
        invalid = *snapshot;
        invalid.pieceBindings[0].flags = Snapshot::LegacyProcedural;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(invalid, 3), Status::InvalidSnapshot);
    }

    void snapshotDefaultVariantAndCapacityDeduplication()
    {
        WorkoutGameAssetPhysicsSnapshotBuilder builder;
        std::uint32_t definitionIndex = 0;
        QVERIFY(builder.internDefinition(triangularDefinition(), definitionIndex));
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log");
        binding.definitionIndex = definitionIndex;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        std::uint32_t index = 999;
        QVERIFY(builder.internBinding(binding, index)); // Empty means default.
        QCOMPARE(index, std::uint32_t(0));
        binding.variantKey = QStringLiteral("invalid variant");
        index = 999;
        QVERIFY(!builder.internBinding(binding, index));
        QCOMPARE(index, std::uint32_t(999));
        for (std::size_t i = 1;
             i < WorkoutGameCourseAssetPhysicsSnapshot::MaximumBindings; ++i) {
            binding.variantKey = QString::number(i);
            QVERIFY(builder.internBinding(binding, index));
            QCOMPARE(index, std::uint32_t(i));
        }
        binding.variantKey.clear();
        QVERIFY(builder.internBinding(binding, index));
        QCOMPARE(index, std::uint32_t(0));
        binding.variantKey = QStringLiteral("one-too-many");
        index = 999;
        QVERIFY(!builder.internBinding(binding, index));
        QCOMPARE(index, std::uint32_t(999));
    }

    void snapshotBindingDeduplicatesAtCapacity()
    {
        WorkoutGameAssetPhysicsSnapshotBuilder builder;
        std::uint32_t definitionIndex = 0;
        QVERIFY(builder.internDefinition(triangularDefinition(), definitionIndex));
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log");
        binding.definitionIndex = definitionIndex;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        std::uint32_t index = 999;
        for (std::size_t i = 0;
             i < WorkoutGameCourseAssetPhysicsSnapshot::MaximumBindings; ++i) {
            binding.variantKey = QString::number(i);
            QVERIFY(builder.internBinding(binding, index));
            WorkoutGameAssetPhysicsPieceBinding piece;
            piece.bindingIndex = index;
            piece.definitionIndex = definitionIndex;
            QVERIFY(builder.appendPieceBinding(piece));
        }
        binding.variantKey = QStringLiteral("0");
        QVERIFY(builder.internBinding(binding, index));
        QCOMPARE(index, std::uint32_t(0));
        const auto snapshot = builder.finish();
        QVERIFY(snapshot);
        QCOMPARE(snapshot->bindings.size(), WorkoutGameCourseAssetPhysicsSnapshot::MaximumBindings);
        QCOMPARE(snapshot->physicsDefinitions.size(), std::size_t(1));
    }

    void legacyAnchorsRejectNonFiniteAndOutOfRangeBeforeRounding()
    {
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -0.1, 250000.001}) {
            auto plan = planWithTurns({15.0});
            plan.pieces[0].geometryAnchorDistanceMeters = invalid;
            QVERIFY(!WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(plan));
        }
        for (const auto &example : std::vector<std::pair<double, std::int32_t>>{
                 {10.00049, 10000}, {10.0005, 10001}, {10.00051, 10001}}) {
            auto plan = planWithTurns({15.0});
            plan.pieces[0].geometryAnchorDistanceMeters = example.first;
            plan.assetPhysicsSnapshot = WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(plan);
            QVERIFY(plan.assetPhysicsSnapshot);
            QCOMPARE(plan.assetPhysicsSnapshot->pieceBindings[0].obstacleAnchorMm, example.second);
            QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                     WorkoutGameRoadPlanValidationStatus::Ready);
        }
    }

    void snapshotAnchorsMustMatchRoadPieces()
    {
        auto plan = planWithTurns({15.0, -15.0, 15.0});
        plan.assetPhysicsSnapshot = WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(plan);
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
        auto changed = std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>(
                *plan.assetPhysicsSnapshot);
        changed->pieceBindings[1].obstacleAnchorMm += 1;
        plan.assetPhysicsSnapshot = changed;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void legacySnapshotMarksEveryPieceForVersionOneAdapter()
    {
        const WorkoutGameRoadPlan plan = planWithTurns(
                {15.0, -15.0, 15.0});
        const std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
                snapshot =
                    WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(plan);
        QVERIFY(snapshot);
        QVERIFY(snapshot->physicsDefinitions.empty());
        QVERIFY(snapshot->bindings.empty());
        QCOMPARE(snapshot->pieceBindings.size(), plan.pieces.size());
        for (std::size_t index = 0;
                index < snapshot->pieceBindings.size(); ++index) {
            const WorkoutGameAssetPhysicsPieceBinding &binding =
                    snapshot->pieceBindings[index];
            QCOMPARE(binding.bindingIndex,
                     WorkoutGameCourseAssetPhysicsSnapshot::NoIndex);
            QCOMPARE(binding.flags,
                     WorkoutGameCourseAssetPhysicsSnapshot::LegacyProcedural);
            QCOMPARE(binding.obstacleAnchorMm,
                     std::int32_t(std::llround(
                         plan.pieces[index].geometryAnchorDistanceMeters
                             * 1000.0)));
        }
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    *snapshot, plan.pieces.size()),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready);
    }

    void snapshotValidationRejectsBadGeometryIndicesAndLimits()
    {
        WorkoutGameAssetPhysicsSnapshotBuilder builder;
        std::uint32_t definitionIndex = 0;
        QVERIFY(builder.internDefinition(
                    triangularDefinition(), definitionIndex));
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log-over-greybox");
        binding.variantKey = QStringLiteral("default");
        binding.definitionIndex = definitionIndex;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        std::uint32_t bindingIndex = 0;
        QVERIFY(builder.internBinding(binding, bindingIndex));
        WorkoutGameAssetPhysicsPieceBinding pieceBinding;
        pieceBinding.bindingIndex = bindingIndex;
        pieceBinding.definitionIndex = definitionIndex;
        QVERIFY(builder.appendPieceBinding(pieceBinding));
        const auto valid = builder.finish();
        QVERIFY(valid);

        WorkoutGameCourseAssetPhysicsSnapshot invalid = *valid;
        invalid.bindings[0].definitionIndex = 42;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        invalid = *valid;
        invalid.physicsDefinitions[0].chains[0].points[1].forwardMm = -270;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        invalid = *valid;
        invalid.physicsDefinitions[0].chains[0].points[1] = {-265, 0};
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        invalid = *valid;
        invalid.physicsDefinitions[0].operation =
                WorkoutGameAssetPhysicsOperation::ReplaceSurface;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        invalid = *valid;
        invalid.physicsDefinitions[0].chains.push_back(
                {{{300, 0}, {320, 0}}});
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        invalid = *valid;
        invalid.physicsDefinitions.resize(
                WorkoutGameCourseAssetPhysicsSnapshot::MaximumDefinitions + 1);
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::ResourceLimit);

        invalid = *valid;
        invalid.snapshotVersion =
                WorkoutGameCourseAssetPhysicsSnapshot::CurrentVersion + 1;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::UnsupportedVersion);

        invalid = *valid;
        invalid.physicsDefinitions[0].profileVersion = 99;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(invalid, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::UnsupportedVersion);
    }

    void sourceSectionsAndInactiveFieldsAreStrictlyValidated()
    {
        WorkoutGameRoadPlan skipped = planWithTurns({15.0, -15.0, 15.0});
        skipped.pieces[1].sourceSectionIndex = 2;
        skipped.pieces[2].sourceSectionIndex = 2;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(skipped, 3),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        WorkoutGameRoadPlan inactive = planWithTurns({15.0, -15.0, 15.0});
        inactive.pieces[0].gapJump.lines[0].gapLengthMeters =
                std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(inactive, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        WorkoutGameRoadPlan missingProfile =
                planWithTurns({15.0, -15.0, 15.0});
        missingProfile.pieces[0].challenge.enabled = true;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(missingProfile, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void generationOnePlansRemainReadableWithoutPhaseTwoMetadata()
    {
        WorkoutGameRoadPlan legacy = planWithTurns({15.0, -15.0, 15.0});
        legacy.generationVersion =
                WorkoutGameRoadPlan::LegacyGenerationVersion;
        for (WorkoutGameRoadPiece &piece : legacy.pieces) {
            piece.bank = WorkoutGameRoadBankProfile();
            piece.relief = WorkoutGameRoadReliefProfile();
        }
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(legacy, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
    }

    void bankAndReliefMetadataUseStrictFiniteBounds()
    {
        WorkoutGameRoadPlan plan = planWithTurns({15.0, -35.0, 15.0});
        WorkoutGameRoadPiece &piece = plan.pieces[1];
        piece.bank.enabled = true;
        piece.bank.startDistanceMeters = piece.startDistanceMeters;
        piece.bank.curveStartDistanceMeters = piece.startDistanceMeters + 2.0;
        piece.bank.curveEndDistanceMeters =
                piece.startDistanceMeters + piece.lengthMeters - 2.0;
        piece.bank.endDistanceMeters =
                piece.startDistanceMeters + piece.lengthMeters;
        piece.bank.socketHalfWidthMeters = 0.68;
        piece.bank.activeHalfWidthMeters = 0.94;
        piece.bank.maximumBankRadians = 0.32;
        piece.bank.maximumLineOffsetMeters = 0.42;
        piece.bank.designSpeedMetersPerSecond = 7.2;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);

        WorkoutGameRoadPlan invalid = plan;
        invalid.pieces[1].bank.maximumBankRadians = 2.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        invalid = plan;
        invalid.pieces[1].bank.curveEndDistanceMeters =
                invalid.pieces[1].bank.curveStartDistanceMeters;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        invalid = plan;
        invalid.pieces[0].relief.phaseRadians =
                std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        invalid = plan;
        invalid.pieces[1].bank.socketHalfWidthMeters += 0.01;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        invalid = planWithTurns({15.0, -15.0, 15.0}, 4.0);
        invalid.pieces[1].relief.constantCoefficientMeters = 5.0;
        invalid.pieces[1].relief.cosineCoefficientMeters = 3.0;
        invalid.pieces[1].relief.sineCoefficientMeters = 0.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        invalid = planWithTurns({15.0, -35.0, 15.0});
        invalid.pieces[1].terrain = WorkoutGameTerrainKind::Berm;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(invalid, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void persistedBankMustNotOverlapANeighbouringFeatureZone()
    {
        WorkoutGameRoadPlan plan = planWithTurns({35.0, 0.0, -15.0}, 20.0);
        WorkoutGameRoadPiece &banked = plan.pieces[0];
        banked.bank.enabled = true;
        banked.bank.startDistanceMeters = 0.0;
        banked.bank.curveStartDistanceMeters = 2.0;
        banked.bank.curveEndDistanceMeters = 18.0;
        banked.bank.endDistanceMeters = 20.0;
        banked.bank.socketHalfWidthMeters = 0.68;
        banked.bank.activeHalfWidthMeters = 0.94;
        banked.bank.maximumBankRadians = 0.32;
        banked.bank.maximumLineOffsetMeters = 0.42;
        banked.bank.designSpeedMetersPerSecond = 7.2;

        WorkoutGameRoadPiece &feature = plan.pieces[1];
        feature.challenge.enabled = true;
        feature.challenge.prepareDistanceMeters = 18.0;
        feature.challenge.decisionDistanceMeters = 30.0;
        feature.challenge.obstacleDistanceMeters = 35.0;
        feature.challenge.bypassStartDistanceMeters = 25.0;
        feature.challenge.bypassEndDistanceMeters = 40.0;
        feature.challenge.profile.enabled = true;
        feature.qualityExempt = true;
        feature.qualityExemptionStartDistanceMeters = 20.0;
        feature.qualityExemptionEndDistanceMeters = 40.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);

        feature.challenge.prepareDistanceMeters = 20.0;
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::Ready);
    }

    void persistedBankMustNotOverlapFeatureGeometryPastItsOwnerPiece()
    {
        WorkoutGameRoadPlan plan = planWithTurns({0.0, 35.0, -15.0}, 20.0);
        WorkoutGameRoadPiece &feature = plan.pieces[0];
        feature.terrain = WorkoutGameTerrainKind::Roots;
        feature.difficulty = 0.7;
        feature.challenge.enabled = true;
        feature.challenge.prepareDistanceMeters = 5.0;
        feature.challenge.decisionDistanceMeters = 19.0;
        feature.challenge.obstacleDistanceMeters = 19.0;
        feature.challenge.bypassStartDistanceMeters = 19.0;
        feature.challenge.bypassEndDistanceMeters = 19.0;
        feature.challenge.profile.enabled = true;
        feature.qualityExempt = true;
        feature.qualityExemptionStartDistanceMeters = 0.0;
        feature.qualityExemptionEndDistanceMeters = 20.0;

        WorkoutGameRoadPiece &banked = plan.pieces[1];
        banked.bank.enabled = true;
        banked.bank.startDistanceMeters = 20.0;
        banked.bank.curveStartDistanceMeters = 22.0;
        banked.bank.curveEndDistanceMeters = 38.0;
        banked.bank.endDistanceMeters = 40.0;
        banked.bank.socketHalfWidthMeters = 0.68;
        banked.bank.activeHalfWidthMeters = 0.94;
        banked.bank.maximumBankRadians = 0.32;
        banked.bank.maximumLineOffsetMeters = 0.42;
        banked.bank.designSpeedMetersPerSecond = 7.2;

        QCOMPARE(WorkoutGameRoadPlanValidator::validate(plan, 1),
                 WorkoutGameRoadPlanValidationStatus::InvalidPlan);
    }

    void bankedOrdinaryTurnsRemainPartOfTheQualityAudit()
    {
        WorkoutGameRoadPlan weak = planWithTurns(
                {0.0, 0.0, 10.0, 0.0, -10.0, 0.0}, 20.0);
        for (WorkoutGameRoadPiece &piece : weak.pieces) {
            if (piece.turnRadians == 0.0) continue;
            piece.bank.enabled = true;
            piece.bank.startDistanceMeters = piece.startDistanceMeters;
            piece.bank.curveStartDistanceMeters = piece.startDistanceMeters + 2.0;
            piece.bank.curveEndDistanceMeters =
                    piece.startDistanceMeters + piece.lengthMeters - 2.0;
            piece.bank.endDistanceMeters =
                    piece.startDistanceMeters + piece.lengthMeters;
            piece.bank.socketHalfWidthMeters = 0.68;
            piece.bank.activeHalfWidthMeters = 0.82;
            piece.bank.maximumBankRadians = 0.18;
            piece.bank.maximumLineOffsetMeters = 0.25;
            piece.bank.designSpeedMetersPerSecond = 6.0;
        }
        const WorkoutGameRoadQualityReport report =
                WorkoutGameRoadQuality::audit(weak);
        QVERIFY(!report.accepted());
        QVERIFY(report.contains(
                WorkoutGameRoadQualityViolation::NearStraightTooLong));
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameRoadPlan)
#include "testWorkoutGameRoadPlan.moc"
