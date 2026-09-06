/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseSourceAdapter.h"
#include "Train/WorkoutGameDistancePlayback.h"
#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameRoadPlan.h"
#include "Train/WorkoutGameRoadQuality.h"

#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cmath>

namespace {

WorkoutGameCourseSourceRequest sampleRequest()
{
    WorkoutGameCourseSourceRequest request;
    double timeMs = 0.0;
    auto append = [&](double durationMs, double watts) {
        if (request.points.empty()) {
            request.points.push_back({timeMs, watts});
        } else if (request.points.back().watts != watts) {
            request.points.push_back({timeMs, watts});
        }
        timeMs += durationMs;
        request.points.push_back({timeMs, watts});
    };
    append(10 * 60000.0, 140.0);
    for (int repetition = 0; repetition < 3; ++repetition) {
        append(4 * 60000.0, 205.0 + repetition * 3.0);
        append(10000.0, 250.0 + repetition * 5.0);
        append(3 * 60000.0, 110.0);
    }
    append(5 * 60000.0, 100.0);
    request.sourceContents = QByteArrayLiteral("abc");
    request.sourceFileName = QStringLiteral(
            "/private/athlete/workouts/three-climbs.erg");
    request.sourceLaps = {
        {10 * 60000, QStringLiteral("Warmup complete")},
        {17 * 60000, QStringLiteral("Second climb")}
    };
    request.sourceTexts = {
        {11 * 60000, 6, QStringLiteral("Cadence 95 rpm")},
        {18 * 60000, 8, QStringLiteral("Stay seated")}
    };
    request.ftpWatts = 190.0;
    return request;
}

WorkoutGameCourseSourceRequest shortCourseRequest(bool uneven)
{
    WorkoutGameCourseSourceRequest request;
    request.sourceContents = uneven
            ? QByteArrayLiteral("uneven") : QByteArrayLiteral("flat");
    request.sourceFileName = uneven
            ? QStringLiteral("short-uneven.erg")
            : QStringLiteral("short-flat.erg");
    request.ftpWatts = 190.0;
    request.seed = 73u;
    if (uneven) {
        request.points = {
            {0.0, 115.0},
            {30000.0, 115.0},
            {30000.0, 225.0},
            {75000.0, 225.0},
            {75000.0, 145.0},
            {105000.0, 145.0}
        };
    } else {
        request.points = {
            {0.0, 155.0},
            {90000.0, 155.0}
        };
    }
    return request;
}

double accumulatedTurn(const WorkoutGameRoadPlan &plan)
{
    double radians = 0.0;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        radians += std::abs(piece.turnRadians);
    }
    return radians;
}

}

class TestWorkoutGameCourseSourceAdapter : public QObject
{
    Q_OBJECT

private slots:
    void roadGenerationParametersAreVersionedAndBackwardCompatible()
    {
        const WorkoutGameCourseSourceRequest source = shortCourseRequest(false);
        const WorkoutGameWorkout normalized =
                WorkoutGameWorkoutAdapter::normalize(source.points);
        QCOMPARE(normalized.status, WorkoutGameWorkoutStatus::Ready);
        WorkoutGameCourseConversionRequest conversionRequest;
        conversionRequest.intervals = normalized.intervals;
        conversionRequest.ftpWatts = source.ftpWatts;
        conversionRequest.seed = source.seed;
        const WorkoutGameCourseConversionResult conversion =
                WorkoutGameCourseConverter::convert(conversionRequest);
        QCOMPARE(conversion.status,
                 WorkoutGameCourseConversionStatus::Ready);
        const WorkoutGameCourse visual =
                WorkoutGameDistancePlayback::visualCourse(conversion.course);

        const WorkoutGameRoadPlan defaultPlan =
                WorkoutGameRoadCourseBuilder::generatePlan(
                    visual, source.ftpWatts);
        const WorkoutGameRoadPlan explicitLegacyScale =
                WorkoutGameRoadCourseBuilder::generatePlan(
                    visual, source.ftpWatts, {
                        WorkoutGameRoadCourseGenerationParameters::CurrentVersion,
                        WorkoutGameCoursePreset::WorkoutFirst
                    });
        QCOMPARE(defaultPlan.pieces.size(), explicitLegacyScale.pieces.size());
        for (std::size_t index = 0;
                index < defaultPlan.pieces.size(); ++index) {
            const WorkoutGameRoadPiece &left = defaultPlan.pieces[index];
            const WorkoutGameRoadPiece &right = explicitLegacyScale.pieces[index];
            QCOMPARE(left.startDistanceMeters, right.startDistanceMeters);
            QCOMPARE(left.lengthMeters, right.lengthMeters);
            QCOMPARE(left.turnRadians, right.turnRadians);
            QCOMPARE(left.entry.xMeters, right.entry.xMeters);
            QCOMPARE(left.entry.zMeters, right.entry.zMeters);
            QCOMPARE(left.exit.xMeters, right.exit.xMeters);
            QCOMPARE(left.exit.zMeters, right.exit.zMeters);
        }

        WorkoutGameRoadCourseGenerationParameters unsupported;
        unsupported.generationVersion =
                WorkoutGameRoadCourseGenerationParameters::CurrentVersion + 1u;
        const WorkoutGameRoadPlan rejected =
                WorkoutGameRoadCourseBuilder::generatePlan(
                    visual, source.ftpWatts, unsupported);
        QVERIFY(rejected.pieces.empty());
    }

    void presetsHaveStrictPairwiseRoadCurvature_data()
    {
        QTest::addColumn<bool>("uneven");
        QTest::newRow("short-flat") << false;
        QTest::newRow("short-uneven") << true;
    }

    void presetsHaveStrictPairwiseRoadCurvature()
    {
        QFETCH(bool, uneven);
        WorkoutGameCourseSourceRequest request = shortCourseRequest(uneven);
        const WorkoutGameWorkout normalized =
                WorkoutGameWorkoutAdapter::normalize(request.points);
        QCOMPARE(normalized.status, WorkoutGameWorkoutStatus::Ready);

        std::array<WorkoutGameCourseSourceResult, 3> results;
        const std::array<WorkoutGameCoursePreset, 3> presets {{
            WorkoutGameCoursePreset::WorkoutFirst,
            WorkoutGameCoursePreset::Balanced,
            WorkoutGameCoursePreset::RideFirst
        }};
        for (std::size_t mode = 0; mode < presets.size(); ++mode) {
            request.preset = presets[mode];
            results[mode] = WorkoutGameCourseSourceAdapter::convert(request);
            QVERIFY2(results[mode].status
                        == WorkoutGameCourseSourceStatus::Ready,
                     qPrintable(QStringLiteral("preset index %1 status %2")
                        .arg(mode).arg(int(results[mode].status))));
            QVERIFY(results[mode].document.course.roadPlan);
            const WorkoutGameRoadPlan &plan =
                    *results[mode].document.course.roadPlan;
            QCOMPARE(WorkoutGameRoadPlanValidator::validate(
                        plan, normalized.intervals.size()),
                     WorkoutGameRoadPlanValidationStatus::Ready);
            QVERIFY(WorkoutGameRoadQuality::audit(plan).accepted());
            QCOMPARE(results[mode].document.sourceIntervals.size(),
                     normalized.intervals.size());
            QCOMPARE(results[mode].document.course.sections.size(),
                     normalized.intervals.size());
            for (std::size_t index = 0;
                    index < normalized.intervals.size(); ++index) {
                const WorkoutGameInterval &source = normalized.intervals[index];
                const WorkoutGameDistanceCourseSection &section =
                        results[mode].document.course.sections[index];
                const WorkoutGameInterval &persisted =
                        results[mode].document.sourceIntervals[index];
                QCOMPARE(persisted.startMs, source.startMs);
                QCOMPARE(persisted.durationMs, source.durationMs);
                QCOMPARE(persisted.startWatts, source.startWatts);
                QCOMPARE(persisted.endWatts, source.endWatts);
                QCOMPARE(section.targetStartWatts, source.startWatts);
                QCOMPARE(section.targetEndWatts, source.endWatts);
                QCOMPARE(section.nominalDurationMs, source.durationMs);
                QCOMPARE(section.minimumDurationMs, source.durationMs);
            }
            const WorkoutGameCourseSourceResult repeated =
                    WorkoutGameCourseSourceAdapter::convert(request);
            QCOMPARE(repeated.status, WorkoutGameCourseSourceStatus::Ready);
            QCOMPARE(WorkoutGameCourseDocumentCodec::encode(
                        repeated.document),
                     WorkoutGameCourseDocumentCodec::encode(
                        results[mode].document));
        }

        const double workoutFirst = accumulatedTurn(
                *results[0].document.course.roadPlan);
        const double balanced = accumulatedTurn(
                *results[1].document.course.roadPlan);
        const double rideFirst = accumulatedTurn(
                *results[2].document.course.roadPlan);
        QVERIFY2(workoutFirst < balanced,
                 qPrintable(QStringLiteral("Workout First %1, Balanced %2")
                     .arg(workoutFirst, 0, 'f', 9)
                     .arg(balanced, 0, 'f', 9)));
        QVERIFY2(balanced < rideFirst,
                 qPrintable(QStringLiteral("Balanced %1, Ride First %2")
                     .arg(balanced, 0, 'f', 9)
                     .arg(rideFirst, 0, 'f', 9)));

    }

    void convertedCourseProducesAnAcceptedPersistableRoadPlan()
    {
        const WorkoutGameCourseSourceRequest source = sampleRequest();
        const WorkoutGameWorkout workout =
                WorkoutGameWorkoutAdapter::normalize(source.points);
        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::Ready);
        WorkoutGameCourseConversionRequest request;
        request.intervals = workout.intervals;
        request.ftpWatts = source.ftpWatts;
        request.preset = source.preset;
        request.roadPhysics = source.roadPhysics;
        request.seed = source.seed;
        const WorkoutGameCourseConversionResult conversion =
                WorkoutGameCourseConverter::convert(request);
        QCOMPARE(conversion.status, WorkoutGameCourseConversionStatus::Ready);
        const WorkoutGameCourse visual =
                WorkoutGameDistancePlayback::visualCourse(conversion.course);
        const WorkoutGameRoadPlan plan =
                WorkoutGameRoadCourseBuilder::generatePlan(
                    visual, source.ftpWatts);

        QVERIFY(!plan.pieces.empty());
        double expectedStart = 0.0;
        for (std::size_t index = 0; index < plan.pieces.size(); ++index) {
            const WorkoutGameRoadPiece &piece = plan.pieces[index];
            QVERIFY2(piece.sourceSectionIndex < conversion.course.sections.size(),
                     qPrintable(QStringLiteral("section index at piece %1").arg(index)));
            QVERIFY2(std::isfinite(piece.startDistanceMeters)
                        && std::abs(piece.startDistanceMeters - expectedStart)
                            <= 1.0e-6,
                     qPrintable(QStringLiteral("start at piece %1").arg(index)));
            QVERIFY2(piece.lengthMeters > 0.0 && piece.lengthMeters <= 500.0,
                     qPrintable(QStringLiteral("length at piece %1").arg(index)));
            QVERIFY2(std::abs(piece.turnRadians) <= 1.4835298641951802 + 1.0e-9,
                     qPrintable(QStringLiteral("turn at piece %1").arg(index)));
            QVERIFY2(!piece.qualityExempt || piece.challenge.enabled,
                     qPrintable(QStringLiteral("exemption at piece %1").arg(index)));
            if (piece.challenge.enabled) {
                QVERIFY2(piece.challenge.prepareDistanceMeters >= 0.0,
                         qPrintable(QStringLiteral("prepare at piece %1").arg(index)));
                QVERIFY2(piece.challenge.prepareDistanceMeters
                            <= piece.challenge.decisionDistanceMeters + 1.0e-6,
                         qPrintable(QStringLiteral("decision order at piece %1").arg(index)));
                QVERIFY2(piece.challenge.decisionDistanceMeters
                            <= piece.challenge.obstacleDistanceMeters + 1.0e-6,
                         qPrintable(QStringLiteral("obstacle order at piece %1").arg(index)));
                QVERIFY2(std::abs(piece.qualityExemptionStartDistanceMeters
                            - piece.startDistanceMeters) <= 1.0e-6,
                         qPrintable(QStringLiteral("exemption start at piece %1").arg(index)));
            }
            expectedStart += piece.lengthMeters;
        }
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(
                    plan, conversion.course.sections.size()),
                 WorkoutGameRoadPlanValidationStatus::Ready);
        const WorkoutGameRoadQualityReport quality =
                WorkoutGameRoadQuality::audit(plan);
        QVERIFY2(quality.accepted(),
                 qPrintable(QStringLiteral("quality violation count: %1")
                    .arg(quality.violations.size())));
    }

    void validSourceBuildsPrivacySafeDocument()
    {
        const WorkoutGameCourseSourceRequest request = sampleRequest();

        const WorkoutGameCourseSourceResult result =
                WorkoutGameCourseSourceAdapter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(result.document.title, QStringLiteral("three-climbs MTB"));
        QCOMPARE(result.document.sourceFileName,
                 QStringLiteral("three-climbs.erg"));
        QCOMPARE(result.document.sourceSha256,
                 QStringLiteral(
                     "ba7816bf8f01cfea414140de5dae2223"
                     "b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(result.document.ftpWatts, 190.0);
        QCOMPARE(result.document.preset, WorkoutGameCoursePreset::Balanced);
        QVERIFY(!result.document.sourceIntervals.empty());
        QCOMPARE(result.document.sourceIntervals.front().startMs,
                 std::int64_t(0));
        QCOMPARE(result.document.sourceLaps.size(), std::size_t(2));
        QCOMPARE(result.document.sourceLaps[0].timeMs, std::int64_t(600000));
        QCOMPARE(result.document.sourceLaps[0].name,
                 QStringLiteral("Warmup complete"));
        QCOMPARE(result.document.sourceTexts.size(), std::size_t(2));
        QCOMPARE(result.document.sourceTexts[0].durationSeconds, 6);
        QCOMPARE(result.document.sourceTexts[0].text,
                 QStringLiteral("Cadence 95 rpm"));
        QCOMPARE(result.document.course.status,
                 WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(result.document.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(result.document.conversionAlgorithmVersion,
                 WorkoutGameCourseDocument::CurrentConversionAlgorithmVersion);
        QVERIFY(result.document.course.seed != 0u);
        QVERIFY(result.document.course.roadPlan);
        QCOMPARE(WorkoutGameRoadPlanValidator::validate(
                    *result.document.course.roadPlan,
                    result.document.course.sections.size()),
                 WorkoutGameRoadPlanValidationStatus::Ready);
        QVERIFY(WorkoutGameRoadQuality::audit(
                    *result.document.course.roadPlan).accepted());
        QCOMPARE(result.summary.distanceMeters,
                 result.document.course.totalDistanceMeters);
        QCOMPARE(request.points.front().watts, 140.0);
    }

    void versionedPrescriptionMetadataIsPersistedExactly()
    {
        WorkoutGameCourseSourceRequest request = sampleRequest();
        const WorkoutGameWorkout workout =
                WorkoutGameWorkoutAdapter::normalize(request.points);
        QCOMPARE(workout.status, WorkoutGameWorkoutStatus::Ready);
        request.prescriptionMetadata.version =
                WorkoutGameCoursePrescriptionMetadata::CurrentVersion;
        request.prescriptionMetadata.intervalRoles.assign(
                workout.intervals.size(),
                WorkoutGameCourseIntervalRole::Prescribed);

        const WorkoutGameCourseSourceResult result =
                WorkoutGameCourseSourceAdapter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(result.document.prescriptionMetadata.version,
                 request.prescriptionMetadata.version);
        QVERIFY(result.document.prescriptionMetadata.intervalRoles
                == request.prescriptionMetadata.intervalRoles);
        const QByteArray encoded =
                WorkoutGameCourseDocumentCodec::encode(result.document);
        QVERIFY(encoded.contains("\"prescriptionMetadata\""));
        QVERIFY(encoded.contains("\"algorithmVersion\":3"));
    }

    void callerTitleAndPresetArePreserved()
    {
        WorkoutGameCourseSourceRequest request = sampleRequest();
        request.title = QStringLiteral("Tuesday trail");
        request.preset = WorkoutGameCoursePreset::RideFirst;

        const WorkoutGameCourseSourceResult result =
                WorkoutGameCourseSourceAdapter::convert(request);

        QCOMPARE(result.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(result.document.title, request.title);
        QCOMPARE(result.document.preset, WorkoutGameCoursePreset::RideFirst);
        QCOMPARE(result.document.generationParameters.gradeScale,
                 WorkoutGameCourseConverter::parametersForPreset(
                         request.preset).gradeScale);
    }

    void invalidInputsFailClosed_data()
    {
        QTest::addColumn<int>("mutation");
        QTest::addColumn<WorkoutGameCourseSourceStatus>("status");
        QTest::newRow("no-points") << 0
                << WorkoutGameCourseSourceStatus::InvalidWorkout;
        QTest::newRow("bad-points") << 1
                << WorkoutGameCourseSourceStatus::InvalidWorkout;
        QTest::newRow("no-source-bytes") << 2
                << WorkoutGameCourseSourceStatus::InvalidSource;
        QTest::newRow("no-source-name") << 3
                << WorkoutGameCourseSourceStatus::InvalidSource;
        QTest::newRow("bad-ftp") << 4
                << WorkoutGameCourseSourceStatus::InvalidFtp;
        QTest::newRow("bad-title") << 5
                << WorkoutGameCourseSourceStatus::InvalidSource;
    }

    void invalidInputsFailClosed()
    {
        QFETCH(int, mutation);
        QFETCH(WorkoutGameCourseSourceStatus, status);
        WorkoutGameCourseSourceRequest request = sampleRequest();
        switch (mutation) {
        case 0: request.points.clear(); break;
        case 1: request.points[1].timeMs = -1.0; break;
        case 2: request.sourceContents.clear(); break;
        case 3: request.sourceFileName.clear(); break;
        case 4: request.ftpWatts = 0.0; break;
        case 5: request.title = QStringLiteral("bad\ntitle"); break;
        }

        const WorkoutGameCourseSourceResult result =
                WorkoutGameCourseSourceAdapter::convert(request);

        QCOMPARE(result.status, status);
        QVERIFY(result.document.course.sections.empty());
    }

    void sameSourceProducesSameDocument()
    {
        const WorkoutGameCourseSourceRequest request = sampleRequest();
        const WorkoutGameCourseSourceResult first =
                WorkoutGameCourseSourceAdapter::convert(request);
        const WorkoutGameCourseSourceResult second =
                WorkoutGameCourseSourceAdapter::convert(request);

        QCOMPARE(first.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(second.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(first.document),
                 WorkoutGameCourseDocumentCodec::encode(second.document));
        QCOMPARE(first.document.course.roadPlan->pieces.size(),
                 second.document.course.roadPlan->pieces.size());
    }

    void storedIntervalsCanBeRegeneratedWithAnotherPreset()
    {
        const WorkoutGameCourseSourceResult original =
                WorkoutGameCourseSourceAdapter::convert(sampleRequest());
        QCOMPARE(original.status, WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(original.document.course.roadPlan);

        const WorkoutGameCourseSourceResult edited =
                WorkoutGameCourseSourceAdapter::regenerate(
                    original.document,
                    WorkoutGameCoursePreset::RideFirst,
                    QStringLiteral("Technical Tuesday"));

        QCOMPARE(edited.status, WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(edited.document.course.roadPlan);
        QCOMPARE(edited.document.title, QStringLiteral("Technical Tuesday"));
        QCOMPARE(edited.document.preset, WorkoutGameCoursePreset::RideFirst);
        QCOMPARE(edited.document.sourceSha256,
                 original.document.sourceSha256);
        QCOMPARE(edited.document.sourceIntervals.size(),
                 original.document.sourceIntervals.size());
        QCOMPARE(edited.document.sourceLaps.size(),
                 original.document.sourceLaps.size());
        QCOMPARE(edited.document.sourceLaps[1].name,
                 original.document.sourceLaps[1].name);
        QCOMPARE(edited.document.sourceTexts.size(),
                 original.document.sourceTexts.size());
        QCOMPARE(edited.document.sourceTexts[1].text,
                 original.document.sourceTexts[1].text);
        QVERIFY(edited.summary.technicalFeatureCount
                >= original.summary.technicalFeatureCount);
        QVERIFY(edited.summary.elevationGainMeters
                > original.summary.elevationGainMeters);
        QVERIFY(accumulatedTurn(*edited.document.course.roadPlan)
                > accumulatedTurn(*original.document.course.roadPlan));
        QCOMPARE(edited.document.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QVERIFY(WorkoutGameRoadQuality::audit(
                    *edited.document.course.roadPlan).accepted());
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(edited.document),
                 WorkoutGameCourseDocumentCodec::encode(
                    WorkoutGameCourseSourceAdapter::regenerate(
                        original.document,
                        WorkoutGameCoursePreset::RideFirst,
                        QStringLiteral("Technical Tuesday")).document));
    }

    void legacyDocumentRegenerationPersistsAsCurrentRoadPlan()
    {
        WorkoutGameCourseSourceResult original =
                WorkoutGameCourseSourceAdapter::convert(sampleRequest());
        QCOMPARE(original.status, WorkoutGameCourseSourceStatus::Ready);
        WorkoutGameCourseDocument legacy = original.document;
        legacy.schemaVersion = 1;
        legacy.sourceLaps.clear();
        legacy.sourceTexts.clear();
        legacy.course.roadPlan.reset();
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(legacy));

        const WorkoutGameCourseSourceResult regenerated =
                WorkoutGameCourseSourceAdapter::regenerate(
                    legacy, legacy.preset, legacy.title);
        QCOMPARE(regenerated.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(regenerated.document.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QVERIFY(regenerated.document.course.roadPlan);
        QCOMPARE(regenerated.document.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::CurrentGenerationVersion);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath(
                QStringLiteral("regenerated-v2.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, regenerated.document, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument reopened;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, reopened, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(reopened.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QVERIFY(reopened.course.roadPlan);
        QCOMPARE(reopened.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::CurrentGenerationVersion);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(reopened),
                 WorkoutGameCourseDocumentCodec::encode(
                     regenerated.document));
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseSourceAdapter)
#include "testWorkoutGameCourseSourceAdapter.moc"
