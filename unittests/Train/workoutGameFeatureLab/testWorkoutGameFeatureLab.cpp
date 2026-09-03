/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameFeatureLab.h"
#include "Train/WorkoutGameBermGeometry.h"
#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameSkinnyGeometry.h"

#include <QTest>

#include <algorithm>
#include <cmath>

class TestWorkoutGameFeatureLab : public QObject
{
    Q_OBJECT

private slots:
    void courseContainsEveryTechnicalFeatureInOrder()
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
            WorkoutGameTerrainKind::GapJump,
            WorkoutGameTerrainKind::Drop,
            WorkoutGameTerrainKind::Rollers,
            WorkoutGameTerrainKind::Climb,
            WorkoutGameTerrainKind::BunnyHop,
            WorkoutGameTerrainKind::Skinny,
            WorkoutGameTerrainKind::RockSlab
        };
        QCOMPARE(challenged, expected);
        QCOMPARE(course.durationMs, std::int64_t(122500));
        QCOMPARE(course.sections.back().terrain,
                 WorkoutGameTerrainKind::SmoothTrail);
        QCOMPARE(course.sections.back().startMs, std::int64_t(117500));
        QCOMPARE(course.sections.back().durationMs, std::int64_t(5000));
        QCOMPARE(course.sections.back().lengthMeters, 30.0);
        QCOMPARE(course.sections.back().targetWatts, 110.0);
    }

    void courseContainsOneRepresentativeGapJump()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const auto first = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameSection &section) {
                    return section.terrain == WorkoutGameTerrainKind::GapJump;
                });
        QVERIFY(first != course.sections.end());
        QCOMPARE(std::count_if(
                    course.sections.begin(), course.sections.end(),
                    [](const WorkoutGameSection &section) {
                        return section.terrain
                                == WorkoutGameTerrainKind::GapJump;
                    }), 1);
        QCOMPARE(first->feature, WorkoutGameFeature::SprintJump);
        QCOMPARE(first->challengeCount, 1);
        QVERIFY(first->targetWatts >= 200.0);
        QVERIFY(first->lengthMeters >= 84.0);
        QCOMPARE((first - 1)->terrain, WorkoutGameTerrainKind::SmoothTrail);
        QCOMPARE((first + 1)->terrain, WorkoutGameTerrainKind::SmoothTrail);
    }

    void bermSequenceAlternatesDirectionAngleAndRollingRelief()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 200.0);
        QVERIFY(road.ready);

        std::vector<const WorkoutGameRoadPiece *> berms;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            if (piece.terrain == WorkoutGameTerrainKind::Berm) {
                berms.push_back(&piece);
            }
        }
        QCOMPARE(berms.size(), std::size_t(6));
        for (std::size_t index = 0; index < berms.size(); ++index) {
            const WorkoutGameRoadPiece &piece = *berms[index];
            QVERIFY(!piece.challenge.enabled);
            const auto profile = WorkoutGameBermGeometry::profile(
                    piece.difficulty);
            QCOMPARE(std::abs(piece.turnRadians),
                     profile.turnMagnitudeRadians);
            QVERIFY(piece.reliefScale >= 1.35);
            if (index > 0) {
                QVERIFY(piece.turnRadians * berms[index - 1]->turnRadians < 0.0);
                QVERIFY(std::abs(piece.turnRadians)
                        > std::abs(berms[index - 1]->turnRadians));
            }
        }

        int rollingTransitions = 0;
        for (const WorkoutGameSection &section : course.sections) {
            if (section.terrain == WorkoutGameTerrainKind::Rollers
                    && section.challengeCount == 0
                    && section.reliefScale > 1.0) {
                ++rollingTransitions;
            }
        }
        QCOMPARE(rollingTransitions, 7);
    }

    void skinnyIsLongAndHasNoChickenLine()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        const auto found = std::find_if(
                course.sections.begin(), course.sections.end(),
                [](const WorkoutGameSection &section) {
                    return section.terrain == WorkoutGameTerrainKind::Skinny;
                });
        QVERIFY(found != course.sections.end());
        QVERIFY(found->lengthMeters >= 44.0);

        const auto profile = WorkoutGameSkinnyGeometry::profile(
                found->difficulty);
        QVERIFY(profile.deckEndMeters - profile.deckStartMeters >= 16.0);
        QCOMPARE(profile.halfWidthMeters(0.0),
                 profile.socketHalfWidthMeters);
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
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, 4999), 210.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(course, 5000), 110.0);
        QCOMPARE(WorkoutGameFeatureLab::targetWattsAt(
                    course, course.durationMs), 0.0);
    }

    void passScenarioCompletesEveryFeature()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));

        for (std::int64_t time = 0; time <= course.durationMs; time += 50) {
            const WorkoutGameSimulationSnapshot snapshot = simulation.update(
                    WorkoutGameFeatureLab::input(
                        course, time, WorkoutGameFeatureLabScenario::Pass));
            if (snapshot.activeSection >= 0
                    && snapshot.featureOutcome
                        == WorkoutGameFeatureOutcome::Active
                    && course.sections[std::size_t(snapshot.activeSection)]
                        .terrain == WorkoutGameTerrainKind::GapJump) {
                // Gap jumps are committed by the distance-based runtime in
                // production; this simulation-only matrix supplies that
                // already-tested external decision explicitly.
                QVERIFY(simulation.commitGapJumpOutcome(
                        snapshot.activeSection,
                        WorkoutGameFeatureOutcome::Completed,
                        1.0));
            }
        }

        for (std::size_t index = 0; index < course.sections.size(); ++index) {
            if (course.sections[index].challengeCount <= 0) continue;
            QVERIFY2(simulation.sectionOutcomes()[index]
                            == WorkoutGameFeatureOutcome::Completed,
                     qPrintable(QStringLiteral(
                         "feature-lab pass scenario bypassed section %1 "
                         "(terrain %2)")
                         .arg(index)
                         .arg(int(course.sections[index].terrain))));
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

    void missedSkinnyRemainsOnMainLine()
    {
        const WorkoutGameCourse course = WorkoutGameFeatureLab::course(200.0);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, 200.0));

        bool observedMissedSkinny = false;
        for (std::int64_t time = 0; time <= course.durationMs; time += 50) {
            const WorkoutGameSimulationSnapshot snapshot = simulation.update(
                    WorkoutGameFeatureLab::input(
                        course, time, WorkoutGameFeatureLabScenario::Bypass));
            if (snapshot.activeSection >= 0
                    && course.sections[std::size_t(snapshot.activeSection)].terrain
                        == WorkoutGameTerrainKind::Skinny
                    && snapshot.featureOutcome
                        == WorkoutGameFeatureOutcome::Bypassed) {
                QCOMPARE(snapshot.route, WorkoutGameRoute::MainLine);
                observedMissedSkinny = true;
            }
        }
        QVERIFY(observedMissedSkinny);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameFeatureLab)
#include "testWorkoutGameFeatureLab.moc"
