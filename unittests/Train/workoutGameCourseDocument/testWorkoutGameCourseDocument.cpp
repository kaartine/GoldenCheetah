/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseCrsExporter.h"
#include "Train/WorkoutGameCourseDocument.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

WorkoutGameCourseDocument sampleDocument()
{
    WorkoutGameCourseDocument document;
    document.title = QStringLiteral("Three climbs MTB");
    document.sourceFileName = QStringLiteral("three-climbs.erg");
    document.sourceSha256 = QString(64, QLatin1Char('a'));
    document.ftpWatts = 190.0;
    document.preset = WorkoutGameCoursePreset::Balanced;
    document.generationParameters =
            WorkoutGameCourseConverter::parametersForPreset(document.preset);

    document.course.status = WorkoutGameDistanceCourseStatus::Ready;
    document.course.seed = 12u;
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
    climb.visualVariant = 3u;

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
    descent.visualVariant = 5u;
    descent.adjustableConnector = true;

    document.course.sections = {climb, descent};
    return document;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

}

class TestWorkoutGameCourseDocument : public QObject
{
    Q_OBJECT

private slots:
    void canonicalJsonRoundTrips()
    {
        const WorkoutGameCourseDocument source = sampleDocument();
        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(source);
        WorkoutGameCourseDocument decoded;

        const WorkoutGameCourseDocumentStatus status =
                WorkoutGameCourseDocumentCodec::decode(encoded, decoded);

        QCOMPARE(status, WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 1);
        QCOMPARE(decoded.title, source.title);
        QCOMPARE(decoded.sourceFileName, source.sourceFileName);
        QCOMPARE(decoded.sourceSha256, source.sourceSha256);
        QCOMPARE(decoded.ftpWatts, source.ftpWatts);
        QCOMPARE(decoded.preset, source.preset);
        QCOMPARE(decoded.course.seed, source.course.seed);
        QCOMPARE(decoded.course.sections.size(), source.course.sections.size());
        QCOMPARE(decoded.course.sections[0].targetEndWatts, 250.0);
        QCOMPARE(decoded.course.sections[1].adjustableConnector, true);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);
    }

    void privateOrMalformedSourceIdentityIsRejected_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QString>("hash");
        QTest::newRow("absolute-path")
                << QStringLiteral("/home/private/workout.erg")
                << QString(64, QLatin1Char('a'));
        QTest::newRow("relative-path")
                << QStringLiteral("folder/workout.erg")
                << QString(64, QLatin1Char('a'));
        QTest::newRow("short-hash")
                << QStringLiteral("workout.erg")
                << QStringLiteral("abc");
        QTest::newRow("non-hex-hash")
                << QStringLiteral("workout.erg")
                << QString(64, QLatin1Char('z'));
    }

    void privateOrMalformedSourceIdentityIsRejected()
    {
        QFETCH(QString, fileName);
        QFETCH(QString, hash);
        WorkoutGameCourseDocument document = sampleDocument();
        document.sourceFileName = fileName;
        document.sourceSha256 = hash;

        QVERIFY(WorkoutGameCourseDocumentCodec::encode(document).isEmpty());
    }

    void malformedUnsupportedAndOversizedJsonAreRejected()
    {
        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode("not-json", decoded),
                 WorkoutGameCourseDocumentStatus::InvalidJson);

        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        root.insert(QStringLiteral("schemaVersion"), 99);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);

        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QByteArray(2 * 1024 * 1024, 'x'), decoded),
                 WorkoutGameCourseDocumentStatus::ResourceLimit);
    }

    void crsExportContainsMetricSegmentsLapsAndPowerCues()
    {
        const QByteArray crs = WorkoutGameCourseCrsExporter::encode(
                sampleDocument());

        QVERIFY(crs.startsWith("[COURSE HEADER]\n"));
        QVERIFY(crs.contains("DISTANCE GRADE WIND\n"));
        QVERIFY(crs.contains("0.100000 5.000 0.0\n"));
        QVERIFY(crs.contains("LAP Recovery descent\n"));
        QVERIFY(crs.contains("0.200000 -4.000 0.0\n"));
        QVERIFY(crs.contains("Target 200 W - Climb 8\n"));
        QVERIFY(crs.contains("Target 100 W - Recovery descent 8\n"));
        QVERIFY(crs.endsWith("[END COURSE TEXT]\n"));
    }

    void newArtifactPairIsAtomicAndConflictSafe()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString crsPath = directory.filePath(QStringLiteral("course.crs"));
        const QString sidecarPath =
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(crsPath);
        QString error;

        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    crsPath, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QVERIFY(error.isEmpty());
        QVERIFY(QFileInfo::exists(crsPath));
        QVERIFY(QFileInfo::exists(sidecarPath));
        const QByteArray originalCrs = readAll(crsPath);
        const QByteArray originalSidecar = readAll(sidecarPath);

        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    crsPath, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Conflict);
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAll(crsPath), originalCrs);
        QCOMPARE(readAll(sidecarPath), originalSidecar);

        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    crsPath, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(loaded.title, QStringLiteral("Three climbs MTB"));
    }

    void sidecarNameDoesNotReplaceUnrelatedSuffixes()
    {
        QCOMPARE(WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    QStringLiteral("/tmp/ride.mtb.crs")),
                 QStringLiteral("/tmp/ride.mtb.gcmtb.json"));
        QCOMPARE(WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    QStringLiteral("/tmp/ride")),
                 QStringLiteral("/tmp/ride.gcmtb.json"));
    }

    void modifiedCourseDoesNotLoadWithStaleMetadata()
    {
        QTemporaryDir directory;
        const QString crsPath = directory.filePath(QStringLiteral("course.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    crsPath, sampleDocument(), error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QFile course(crsPath);
        QVERIFY(course.open(QIODevice::Append));
        QCOMPARE(course.write("; modified\n"), qint64(11));
        course.close();

        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    crsPath, loaded, error),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWorkoutGameCourseDocument)
#include "testWorkoutGameCourseDocument.moc"
