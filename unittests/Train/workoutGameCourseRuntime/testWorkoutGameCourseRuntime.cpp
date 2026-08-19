/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseRuntime.h"
#include "Train/WorkoutGameCourseDocument.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

namespace {

WorkoutGameCourseDocument sampleDocument()
{
    WorkoutGameCourseDocument document;
    document.title = QStringLiteral("Runtime MTB");
    document.sourceFileName = QStringLiteral("runtime.erg");
    document.sourceSha256 = QString(64, QLatin1Char('b'));
    document.ftpWatts = 190.0;
    document.preset = WorkoutGameCoursePreset::Balanced;
    document.generationParameters =
            WorkoutGameCourseConverter::parametersForPreset(document.preset);
    document.course.status = WorkoutGameDistanceCourseStatus::Ready;
    document.course.seed = 19u;
    document.course.nominalDurationMs = 30000;
    document.course.totalDistanceMeters = 300.0;
    document.course.elevationGainMeters = 5.0;
    document.course.elevationLossMeters = 8.0;

    WorkoutGameDistanceCourseSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.nominalDurationMs = 10000;
    climb.minimumDurationMs = 9000;
    climb.maximumDurationMs = 12500;
    climb.lengthMeters = 100.0;
    climb.targetStartWatts = 150.0;
    climb.targetEndWatts = 250.0;
    climb.gradePercent = 5.0;
    climb.endElevationMeters = 5.0;
    climb.difficulty = 0.7;

    WorkoutGameDistanceCourseSection descent;
    descent.feature = WorkoutGameFeature::RecoveryDescent;
    descent.terrain = WorkoutGameTerrainKind::Drop;
    descent.sourceStartMs = 10000;
    descent.nominalDurationMs = 20000;
    descent.minimumDurationMs = 14000;
    descent.maximumDurationMs = 30000;
    descent.startDistanceMeters = 100.0;
    descent.lengthMeters = 200.0;
    descent.startElevationMeters = 5.0;
    descent.endElevationMeters = -3.0;
    descent.targetStartWatts = 100.0;
    descent.targetEndWatts = 100.0;
    descent.gradePercent = -4.0;
    descent.difficulty = 0.2;
    descent.adjustableConnector = true;
    document.course.sections = {climb, descent};
    return document;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
            && file.write(contents) == contents.size();
}

}

class TestWorkoutGameCourseRuntime : public QObject
{
    Q_OBJECT

private slots:
    void validArtifactConfiguresDistanceRuntime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("runtime.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseRuntime runtime;
        QCOMPARE(runtime.configure(path), WorkoutGameCourseRuntimeStatus::Ready);
        QVERIFY(runtime.enabled());
        QCOMPARE(runtime.ftpWatts(), 190.0);
        QCOMPARE(runtime.visualCourse().status, WorkoutGameCourseStatus::Ready);
        QCOMPARE(runtime.visualCourse().durationMs, std::int64_t(30000));

        const WorkoutGameDistancePlaybackSnapshot first =
                runtime.atWorkoutPosition(50);
        QVERIFY(first.ready);
        QCOMPARE(first.sectionIndex, std::size_t(0));
        QCOMPARE(first.nominalTimeMs, std::int64_t(5000));
        QCOMPARE(first.targetWatts, 200.0);

        const WorkoutGameDistancePlaybackSnapshot second =
                runtime.atWorkoutPosition(200);
        QCOMPARE(second.sectionIndex, std::size_t(1));
        QCOMPARE(second.nominalTimeMs, std::int64_t(20000));
        QCOMPARE(second.targetWatts, 100.0);
    }

    void missingOrInvalidMetadataFailsClosed()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("plain.crs"));
        QVERIFY(writeFile(path, QByteArrayLiteral("plain course")));
        WorkoutGameCourseRuntime runtime;

        QCOMPARE(runtime.configure(path),
                 WorkoutGameCourseRuntimeStatus::MetadataUnavailable);
        QVERIFY(!runtime.enabled());
        QVERIFY(!runtime.atWorkoutPosition(10).ready);

        QVERIFY(writeFile(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(path),
                QByteArrayLiteral("not-json")));
        QCOMPARE(runtime.configure(path),
                 WorkoutGameCourseRuntimeStatus::InvalidMetadata);
        QVERIFY(!runtime.enabled());
        QCOMPARE(runtime.visualCourse().status, WorkoutGameCourseStatus::EmptyWorkout);
    }

    void reconfigureClearsPriorCourse()
    {
        QTemporaryDir directory;
        const QString validPath = directory.filePath(QStringLiteral("valid.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    validPath, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);
        const QString missingPath = directory.filePath(QStringLiteral("missing.crs"));
        QVERIFY(writeFile(missingPath, QByteArrayLiteral("plain")));

        WorkoutGameCourseRuntime runtime;
        QCOMPARE(runtime.configure(validPath), WorkoutGameCourseRuntimeStatus::Ready);
        QCOMPARE(runtime.configure(missingPath),
                 WorkoutGameCourseRuntimeStatus::MetadataUnavailable);

        QVERIFY(!runtime.enabled());
        QCOMPARE(runtime.ftpWatts(), 0.0);
        QVERIFY(!runtime.atWorkoutPosition(50).ready);
    }

    void scalesGeneratorTargetWithVirtualGear()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("runtime.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseRuntime runtime;
        QCOMPARE(runtime.configure(path), WorkoutGameCourseRuntimeStatus::Ready);

        QCOMPARE(runtime.generatedTargetWattsAt(50, 1.0), 200.0);
        QCOMPARE(runtime.generatedTargetWattsAt(50, 1.25), 250.0);
        QCOMPARE(runtime.generatedTargetWattsAt(50, 0.5), 100.0);
        QCOMPARE(runtime.generatedTargetWattsAt(200, 2.0), 200.0);
    }

    void rejectsUnavailableOrInvalidGeneratorTargets()
    {
        WorkoutGameCourseRuntime runtime;
        QCOMPARE(runtime.generatedTargetWattsAt(50, 1.0), -1.0);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("runtime.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(runtime.configure(path), WorkoutGameCourseRuntimeStatus::Ready);

        QCOMPARE(runtime.generatedTargetWattsAt(50, 0.0), -1.0);
        QCOMPARE(runtime.generatedTargetWattsAt(
                     50, std::numeric_limits<double>::infinity()), -1.0);
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseRuntime)
#include "testWorkoutGameCourseRuntime.moc"
