/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameTrainerTargetPlanner.h"

#include <QStringList>
#include <QTest>

#include <cmath>
#include <limits>
#include <vector>

namespace {

class FakePhysicalTrainer final : public TrainerTargetDevice
{
public:
    void setLoad(double value) override
    {
        loadValues.push_back(value);
        calls.append(QStringLiteral("load"));
    }

    void setGradient(double value) override
    {
        gradientValues.push_back(value);
        calls.append(QStringLiteral("gradient"));
    }

    void setWindResistance(double value) override
    {
        windValues.push_back(value);
        calls.append(QStringLiteral("wind"));
    }

    QStringList calls;
    std::vector<double> loadValues;
    std::vector<double> gradientValues;
    std::vector<double> windValues;
};

WorkoutGameTrainerTargetInput sampleInput(WorkoutGameCoursePreset preset)
{
    WorkoutGameTrainerTargetInput input;
    input.preset = preset;
    input.targetPowerSupported = true;
    input.prescribedWatts = 112.0;
    input.gradePercent = -4.0;
    input.terrain = WorkoutGameTerrainKind::GapJump;
    input.sectionProgress = 0.72;
    input.sectionDurationMs = 60000;
    input.windResistance = 0.37;
    input.workoutPosition = 125.0;
    return input;
}

}

class TestWorkoutGameTrainerTargetPlanner : public QObject
{
    Q_OBJECT

private slots:
    void workoutFirstSendsTheOriginalPowerTarget()
    {
        const TrainerTarget target = WorkoutGameTrainerTargetPlanner::plan(
                sampleInput(WorkoutGameCoursePreset::WorkoutFirst));

        QCOMPARE(target.mode, TrainerTargetMode::Erg);
        QCOMPARE(target.value, 112.0);
        QCOMPARE(target.workoutPosition, 125.0);
    }

    void balancedKeepsThePrescriptionAndAddsBoundedTerrainVariation()
    {
        WorkoutGameTrainerTargetInput beforeInput =
                sampleInput(WorkoutGameCoursePreset::Balanced);
        beforeInput.sectionProgress = 0.20;
        const TrainerTarget before =
                WorkoutGameTrainerTargetPlanner::plan(beforeInput);
        const TrainerTarget effort = WorkoutGameTrainerTargetPlanner::plan(
                sampleInput(WorkoutGameCoursePreset::Balanced));

        QCOMPARE(before.mode, TrainerTargetMode::Erg);
        QCOMPARE(effort.mode, TrainerTargetMode::Erg);
        QVERIFY(before.value < 112.0);
        QVERIFY(effort.value > 112.0);
        QVERIFY(before.value >= 112.0 * 0.92);
        QVERIFY(effort.value <= 112.0 * 1.08);
        QCOMPARE(WorkoutGameTrainerTargetPlanner::workoutPowerWatts(
                     WorkoutGameCoursePreset::Balanced, 400.0,
                     WorkoutGameTerrainKind::GapJump, 0.72, 60000),
                 420.0);
    }

    void balancedTerrainEffortIsIndependentOfGrade()
    {
        WorkoutGameTrainerTargetInput downhill =
                sampleInput(WorkoutGameCoursePreset::Balanced);
        WorkoutGameTrainerTargetInput uphill = downhill;
        uphill.gradePercent = 12.0;

        QCOMPARE(WorkoutGameTrainerTargetPlanner::plan(downhill).value,
                 WorkoutGameTrainerTargetPlanner::plan(uphill).value);
    }

    void balancedTerrainEffortPreservesSectionAverage()
    {
        constexpr int SampleCount = 10000;
        const std::vector<WorkoutGameTerrainKind> terrains = {
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::Rollers,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::BunnyHop,
            WorkoutGameTerrainKind::Skinny,
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::RockSlab,
            WorkoutGameTerrainKind::GapJump
        };
        for (WorkoutGameTerrainKind terrain : terrains) {
            double total = 0.0;
            for (int index = 0; index < SampleCount; ++index) {
                total += WorkoutGameTrainerTargetPlanner::terrainEffortSignal(
                        terrain,
                        (double(index) + 0.5) / double(SampleCount),
                        60000);
            }
            QVERIFY(std::abs(total / double(SampleCount)) < 1.0e-6);
        }
    }

    void smoothTrailKeepsTheOriginalBalancedTarget()
    {
        WorkoutGameTrainerTargetInput input =
                sampleInput(WorkoutGameCoursePreset::Balanced);
        input.terrain = WorkoutGameTerrainKind::SmoothTrail;

        QCOMPARE(WorkoutGameTrainerTargetPlanner::plan(input).value, 112.0);
    }

    void balancedPowerIsActuallyDispatchedAsLoadToAPhysicalTrainer()
    {
        FakePhysicalTrainer trainer;
        TrainerTargetCoordinator coordinator;
        const TrainerTarget target = WorkoutGameTrainerTargetPlanner::plan(
                sampleInput(WorkoutGameCoursePreset::Balanced));

        QCOMPARE(coordinator.apply(target, {&trainer}),
                 TrainerTargetResult::Applied);
        QCOMPARE(trainer.calls, QStringList({QStringLiteral("load")}));
        QCOMPARE(trainer.loadValues.size(), std::size_t(1));
        QCOMPARE(trainer.loadValues.front(), target.value);
        QVERIFY(trainer.gradientValues.empty());
        QVERIFY(trainer.windValues.empty());
    }

    void rideFirstRetainsCourseSlopeControl()
    {
        const TrainerTarget target = WorkoutGameTrainerTargetPlanner::plan(
                sampleInput(WorkoutGameCoursePreset::RideFirst));

        QCOMPARE(target.mode, TrainerTargetMode::Slope);
        QCOMPARE(target.value, -4.0);
        QCOMPARE(target.windResistance, 0.37);
    }

    void missingPowerCapabilityFallsBackToSlope()
    {
        WorkoutGameTrainerTargetInput input =
                sampleInput(WorkoutGameCoursePreset::Balanced);
        input.targetPowerSupported = false;

        const TrainerTarget target = WorkoutGameTrainerTargetPlanner::plan(input);

        QCOMPARE(target.mode, TrainerTargetMode::Slope);
        QCOMPARE(target.value, -4.0);
    }

    void invalidValuesNeverReachTrainerCommands()
    {
        WorkoutGameTrainerTargetInput input =
                sampleInput(WorkoutGameCoursePreset::Balanced);
        input.prescribedWatts = std::numeric_limits<double>::quiet_NaN();
        input.gradePercent = std::numeric_limits<double>::infinity();
        input.windResistance = -1.0;
        input.workoutPosition = -10.0;

        const TrainerTarget target = WorkoutGameTrainerTargetPlanner::plan(input);

        QCOMPARE(target.mode, TrainerTargetMode::Slope);
        QCOMPARE(target.value, 0.0);
        QCOMPARE(target.windResistance, 0.0);
        QCOMPARE(target.workoutPosition, 0.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameTrainerTargetPlanner)
#include "testWorkoutGameTrainerTargetPlanner.moc"
