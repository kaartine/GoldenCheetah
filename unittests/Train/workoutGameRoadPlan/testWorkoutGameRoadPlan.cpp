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
