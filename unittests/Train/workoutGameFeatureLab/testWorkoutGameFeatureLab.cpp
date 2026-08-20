/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"

#include <QTest>

class TestWorkoutGameFeatureLab : public QObject
{
    Q_OBJECT

private slots:
    void courseContainsTheFirstFeatureSetInOrder()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(
                200.0, 712u);

        QCOMPARE(course.status, WorkoutGameCourseStatus::Ready);
        QCOMPARE(course.seed, std::uint32_t(712));
        QVERIFY(course.durationMs > 0);

        std::vector<WorkoutGameTerrainKind> challenged;
        for (const WorkoutGameSection &section : course.sections) {
            if (section.challengeCount > 0) challenged.push_back(section.terrain);
        }
        const std::vector<WorkoutGameTerrainKind> expected = {
            WorkoutGameTerrainKind::LogOver,
            WorkoutGameTerrainKind::Roots,
            WorkoutGameTerrainKind::RockGarden,
            WorkoutGameTerrainKind::Tabletop,
            WorkoutGameTerrainKind::Drop
        };
        QCOMPARE(challenged, expected);
    }

    void generatedCourseIsDeterministicAndRejectsInvalidFtp()
    {
        const WorkoutGameCourse first = WorkoutGameFeatureLab::course(200.0, 99u);
        const WorkoutGameCourse second = WorkoutGameFeatureLab::course(200.0, 99u);
        QCOMPARE(first.durationMs, second.durationMs);
        QCOMPARE(first.sections.size(), second.sections.size());
        for (std::size_t index = 0; index < first.sections.size(); ++index) {
            QCOMPARE(first.sections[index].terrain, second.sections[index].terrain);
            QCOMPARE(first.sections[index].startMs, second.sections[index].startMs);
            QCOMPARE(first.sections[index].targetWatts,
                     second.sections[index].targetWatts);
            QCOMPARE(first.sections[index].visualVariant,
                     second.sections[index].visualVariant);
        }
        QCOMPARE(WorkoutGameFeatureLab::course(0.0).status,
                 WorkoutGameCourseStatus::InvalidFtp);
    }

    void targetLookupHonorsSectionAndCourseBoundaries()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);

        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, -1), 0.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, 0), 210.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, 11999), 210.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, 12000), 110.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(
                    course, course.durationMs), 0.0);
    }

    void passScenarioCompletesEveryFeature()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));

        for (std::int64_t time = 0; time <= course.durationMs; time += 50) {
            simulation.update(WorkoutGameFeatureLab::input(
                    course, time, WorkoutGameFeatureLabScenario::Pass));
        }

        for (std::size_t index = 0; index < course.sections.size(); ++index) {
            if (course.sections[index].challengeCount <= 0) continue;
            QCOMPARE(simulation.sectionOutcomes()[index],
                     WorkoutGameFeatureOutcome::Completed);
        }
    }

    void bypassScenarioMissesEveryFeatureSafely()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));

        for (std::int64_t time = 0; time <= course.durationMs; time += 50) {
            simulation.update(WorkoutGameFeatureLab::input(
                    course, time, WorkoutGameFeatureLabScenario::Bypass));
        }

        for (std::size_t index = 0; index < course.sections.size(); ++index) {
            if (course.sections[index].challengeCount <= 0) continue;
            QCOMPARE(simulation.sectionOutcomes()[index],
                     WorkoutGameFeatureOutcome::Bypassed);
        }
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureLab)
#include "testWorkoutGameFeatureLab.moc"
