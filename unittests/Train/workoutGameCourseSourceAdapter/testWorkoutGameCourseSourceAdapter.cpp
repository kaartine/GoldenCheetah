/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseSourceAdapter.h"

#include <QTest>

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
    request.ftpWatts = 190.0;
    return request;
}

}

class TestWorkoutGameCourseSourceAdapter : public QObject
{
    Q_OBJECT

private slots:
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
        QCOMPARE(result.document.course.status,
                 WorkoutGameDistanceCourseStatus::Ready);
        QCOMPARE(result.summary.distanceMeters,
                 result.document.course.totalDistanceMeters);
        QCOMPARE(request.points.front().watts, 140.0);
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
        QCOMPARE(result.document.generationParameters.gradeScale, 1.18);
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
    }

    void storedIntervalsCanBeRegeneratedWithAnotherPreset()
    {
        const WorkoutGameCourseSourceResult original =
                WorkoutGameCourseSourceAdapter::convert(sampleRequest());

        const WorkoutGameCourseSourceResult edited =
                WorkoutGameCourseSourceAdapter::regenerate(
                    original.document,
                    WorkoutGameCoursePreset::RideFirst,
                    QStringLiteral("Technical Tuesday"));

        QCOMPARE(edited.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(edited.document.title, QStringLiteral("Technical Tuesday"));
        QCOMPARE(edited.document.preset, WorkoutGameCoursePreset::RideFirst);
        QCOMPARE(edited.document.sourceSha256,
                 original.document.sourceSha256);
        QCOMPARE(edited.document.sourceIntervals.size(),
                 original.document.sourceIntervals.size());
        QVERIFY(edited.summary.technicalFeatureCount
                > original.summary.technicalFeatureCount);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseSourceAdapter)
#include "testWorkoutGameCourseSourceAdapter.moc"
