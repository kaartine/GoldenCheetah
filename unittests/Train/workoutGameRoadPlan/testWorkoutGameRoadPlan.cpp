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
};

QTEST_APPLESS_MAIN(TestWorkoutGameRoadPlan)
#include "testWorkoutGameRoadPlan.moc"
