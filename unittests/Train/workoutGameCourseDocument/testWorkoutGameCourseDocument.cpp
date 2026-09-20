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
#include "Train/WorkoutGameDistancePlayback.h"
#include "Train/WorkoutGameRoadCourse.h"
#include "Train/WorkoutGameRoadPlan.h"
#include "Train/WorkoutGameRoadQuality.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

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
    climb.referenceEffortStartWatts = 150.0;
    climb.referenceEffortEndWatts = 250.0;
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
    descent.referenceEffortStartWatts = 100.0;
    descent.referenceEffortEndWatts = 100.0;
    descent.gradePercent = -4.0;
    descent.difficulty = 0.2;
    descent.visualVariant = 5u;
    descent.adjustableConnector = true;

    document.course.sections = {climb, descent};

    auto roadPlan = std::make_shared<WorkoutGameRoadPlan>();
    roadPlan->generationVersion =
            WorkoutGameRoadPlan::CurrentGenerationVersion;
    WorkoutGameRoadConnector connector;
    for (int index = 0; index < 15; ++index) {
        WorkoutGameRoadPiece piece;
        piece.sourceSectionIndex = index < 5 ? 0u : 1u;
        piece.terrain = index < 5
                ? WorkoutGameTerrainKind::Climb
                : WorkoutGameTerrainKind::Drop;
        piece.startDistanceMeters = double(index) * 20.0;
        piece.lengthMeters = 20.0;
        piece.turnRadians = (index & 1) == 0 ? 0.30 : -0.30;
        piece.relief.enabled = true;
        piece.relief.phaseRadians = 0.15 * index;
        piece.relief.constantCoefficientMeters = 0.75;
        piece.relief.cosineCoefficientMeters = 0.20;
        piece.relief.sineCoefficientMeters = 0.18;
        piece.riseMeters = index < 5 ? 1.0 : -0.8;
        piece.difficulty = index < 5 ? 0.7 : 0.2;
        piece.geometryAnchorDistanceMeters =
                piece.startDistanceMeters + 10.0;
        if (index == 4) {
            piece.bank.enabled = true;
            piece.bank.startDistanceMeters = piece.startDistanceMeters;
            piece.bank.curveStartDistanceMeters =
                    piece.startDistanceMeters + 2.0;
            piece.bank.curveEndDistanceMeters =
                    piece.startDistanceMeters + 18.0;
            piece.bank.endDistanceMeters =
                    piece.startDistanceMeters + 20.0;
            piece.bank.socketHalfWidthMeters = 0.68;
            piece.bank.activeHalfWidthMeters = 0.92;
            piece.bank.maximumBankRadians = 0.30;
            piece.bank.maximumLineOffsetMeters = 0.40;
            piece.bank.designSpeedMetersPerSecond = 7.0;
        }
        piece.entry = connector;
        piece.exit = connector;
        piece.exit.zMeters += 20.0;
        piece.exit.elevationMeters += piece.riseMeters;
        piece.exit.headingRadians += piece.turnRadians;
        connector = piece.exit;
        roadPlan->pieces.push_back(piece);
    }
    roadPlan->assetPhysicsSnapshot =
            WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(*roadPlan);
    document.course.roadPlan = roadPlan;
    return document;
}

WorkoutGameCourseDocument samplePhysicsDocument()
{
    auto document = sampleDocument();
    document.schemaVersion = WorkoutGameCourseDocumentCodec::AssetPhysicsSchemaVersion;
    return document;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void setSectionTerrain(
        WorkoutGameCourseDocument &document,
        std::size_t sectionIndex,
        WorkoutGameTerrainKind terrain)
{
    document.course.sections[sectionIndex].terrain = terrain;
    auto plan = std::make_shared<WorkoutGameRoadPlan>(
            *document.course.roadPlan);
    for (WorkoutGameRoadPiece &piece : plan->pieces) {
        if (piece.sourceSectionIndex == sectionIndex) piece.terrain = terrain;
    }
    document.course.roadPlan = std::move(plan);
}

}

class TestWorkoutGameCourseDocument : public QObject
{
    Q_OBJECT

private slots:
    void defaultWriterStaysAtSchemaSixUntilRuntimeIsReady()
    {
        QCOMPARE(WorkoutGameCourseDocumentCodec::CurrentSchemaVersion, 6);
        QCOMPARE(WorkoutGameCourseDocument().schemaVersion, 6);
        const auto encoded = WorkoutGameCourseDocumentCodec::encode(sampleDocument());
        QVERIFY(!encoded.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(encoded).object().value("schemaVersion").toInt(), 6);
        QVERIFY(!encoded.contains("assetPhysicsSnapshot"));
    }

    void canonicalJsonRoundTrips()
    {
        const WorkoutGameCourseDocument source = sampleDocument();
        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(source);
        WorkoutGameCourseDocument decoded;

        const WorkoutGameCourseDocumentStatus status =
                WorkoutGameCourseDocumentCodec::decode(encoded, decoded);

        QCOMPARE(status, WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(decoded.conversionAlgorithmVersion,
                 WorkoutGameCourseDocument::CurrentConversionAlgorithmVersion);
        QVERIFY(encoded.contains("\"algorithmVersion\":6"));
        QVERIFY(encoded.contains("\"terrainVariationPercent\":15"));
        QVERIFY(encoded.contains("\"variationLengthMeters\":60"));
        QVERIFY(encoded.contains("\"referenceGear\":6"));
        QVERIFY(encoded.contains("\"referenceEffortStartWatts\""));
        QVERIFY(!encoded.contains("\"assetPhysicsSnapshot\""));
        QVERIFY(!encoded.contains("\"sha256\""));
        QCOMPARE(decoded.title, source.title);
        QCOMPARE(decoded.sourceFileName, source.sourceFileName);
        QVERIFY(decoded.sourceSha256.isEmpty());
        QCOMPARE(decoded.ftpWatts, source.ftpWatts);
        QCOMPARE(decoded.preset, source.preset);
        QCOMPARE(decoded.course.seed, source.course.seed);
        QCOMPARE(decoded.course.sections.size(), source.course.sections.size());
        QCOMPARE(decoded.course.sections[0].targetEndWatts, 250.0);
        QCOMPARE(decoded.course.sections[1].adjustableConnector, true);
        QVERIFY(decoded.course.roadPlan);
        QCOMPARE(decoded.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::BankAndReliefGenerationVersion);
        QVERIFY(decoded.course.roadPlan->assetPhysicsSnapshot);
        QCOMPARE(decoded.course.roadPlan->assetPhysicsSnapshot
                    ->pieceBindings.size(),
                 decoded.course.roadPlan->pieces.size());
        QCOMPARE(decoded.course.roadPlan->pieces.size(), std::size_t(15));
        QCOMPARE(decoded.course.roadPlan->pieces[4].turnRadians, 0.30);
        QVERIFY(decoded.course.roadPlan->pieces[4].bank.enabled);
        QCOMPARE(decoded.course.roadPlan->pieces[4].bank.maximumBankRadians,
                 0.30);
        QCOMPARE(decoded.course.roadPlan->pieces[4]
                    .relief.constantCoefficientMeters, 0.75);
        QVERIFY(encoded.contains("\"bank\""));
        QVERIFY(encoded.contains("\"relief\""));
        QVERIFY(WorkoutGameRoadQuality::audit(
                    *decoded.course.roadPlan).accepted());
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);
    }

    void boundedPhysicsDefinitionsAndBindingsRoundTripWithoutHashes()
    {
        WorkoutGameCourseDocument source = samplePhysicsDocument();
        auto plan = std::make_shared<WorkoutGameRoadPlan>(
                *source.course.roadPlan);
        WorkoutGameAssetPhysicsSnapshotBuilder builder(1);
        WorkoutGameAssetPhysicsDefinition definition;
        definition.operation = WorkoutGameAssetPhysicsOperation::AddObstacle;
        definition.coulombFrictionMilli = 1100;
        definition.chains = {{{{-270, 0}, {0, 540}, {270, 0}}}};
        std::uint32_t definitionIndex = 0;
        QVERIFY(builder.internDefinition(definition, definitionIndex));
        WorkoutGameAssetPhysicsBinding binding;
        binding.assetId = QStringLiteral("FT-02-log-over-greybox");
        binding.variantKey.clear(); // The default variant is represented by an empty key.
        binding.definitionIndex = definitionIndex;
        binding.nativeForwardOriginMm = -1020;
        binding.nativeForwardExtentMm = 540;
        binding.nativeUpExtentMm = 540;
        binding.resolvedExtentMm = 540;
        std::uint32_t bindingIndex = 0;
        QVERIFY(builder.internBinding(binding, bindingIndex));
        for (std::size_t index = 0; index < plan->pieces.size(); ++index) {
            WorkoutGameAssetPhysicsPieceBinding pieceBinding;
            pieceBinding.obstacleAnchorMm = std::int32_t(std::llround(
                    plan->pieces[index].geometryAnchorDistanceMeters * 1000.0));
            pieceBinding.obstacleAnchorMicrometerRemainder =
                    std::int16_t(std::llround(
                        plan->pieces[index].geometryAnchorDistanceMeters
                            * 1000000.0)
                        - std::int64_t(pieceBinding.obstacleAnchorMm) * 1000);
            if (index == 2) {
                pieceBinding.bindingIndex = bindingIndex;
                pieceBinding.definitionIndex = definitionIndex;
            } else {
                pieceBinding.flags =
                        WorkoutGameCourseAssetPhysicsSnapshot::LegacyProcedural;
            }
            QVERIFY(builder.appendPieceBinding(pieceBinding));
        }
        plan->assetPhysicsSnapshot = builder.finish();
        source.course.roadPlan = plan;

        const QByteArray encoded =
                WorkoutGameCourseDocumentCodec::encode(source);
        QVERIFY(!encoded.isEmpty());
        QVERIFY(encoded.contains("\"coulombFrictionMilli\":1100"));
        QVERIFY(encoded.contains("\"assetId\":\"FT-02-log-over-greybox\""));
        QVERIFY(!encoded.contains("sha256"));
        QVERIFY(!encoded.contains("digest"));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QVERIFY(decoded.course.roadPlan->assetPhysicsSnapshot);
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot =
                *decoded.course.roadPlan->assetPhysicsSnapshot;
        QCOMPARE(snapshot.physicsDefinitions.size(), std::size_t(1));
        QCOMPARE(snapshot.catalogSchemaVersion, std::uint32_t(1));
        QCOMPARE(snapshot.pieceBindings[2].definitionIndex, std::uint32_t(0));
        QCOMPARE(snapshot.bindings.size(), std::size_t(1));
        QCOMPARE(snapshot.pieceBindings[2].bindingIndex, std::uint32_t(0));
        QCOMPARE(snapshot.physicsDefinitions[0].chains[0].points[1].heightMm,
                 std::int32_t(540));
        QVERIFY(snapshot.bindings[0].variantKey.isEmpty());
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);

        // An unsupported profile must retain the version-specific status.
        QJsonObject root = QJsonDocument::fromJson(encoded).object();
        auto serializedPlan = root.value("roadPlan").toObject();
        auto serializedSnapshot = serializedPlan.value("assetPhysicsSnapshot").toObject();
        auto definitions = serializedSnapshot.value("physicsDefinitions").toArray();
        auto newerDefinition = definitions[0].toObject();
        newerDefinition["profileVersion"] = 99;
        definitions[0] = newerDefinition;
        serializedSnapshot["physicsDefinitions"] = definitions;
        serializedPlan["assetPhysicsSnapshot"] = serializedSnapshot;
        root["roadPlan"] = serializedPlan;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);
    }

    void legacyWriterCannotDiscardResolvedPhysics()
    {
        auto source = samplePhysicsDocument();
        auto plan = std::make_shared<WorkoutGameRoadPlan>(*source.course.roadPlan);
        auto snapshot = std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>(
                *plan->assetPhysicsSnapshot);
        snapshot->catalogSchemaVersion = 1;
        WorkoutGameAssetPhysicsDefinition definition;
        definition.chains = {{{{-270, 0}, {0, 540}, {270, 0}}}};
        snapshot->physicsDefinitions.push_back(definition);
        snapshot->pieceBindings[0].definitionIndex = 0;
        snapshot->pieceBindings[0].flags = 0;
        plan->assetPhysicsSnapshot = snapshot;
        source.course.roadPlan = plan;
        const auto encoded = WorkoutGameCourseDocumentCodec::encode(source);
        QVERIFY(!encoded.isEmpty());
        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.course.roadPlan->assetPhysicsSnapshot->pieceBindings[0].definitionIndex,
                 std::uint32_t(0));
        QCOMPARE(decoded.course.roadPlan->assetPhysicsSnapshot->pieceBindings[0].bindingIndex,
                 WorkoutGameCourseAssetPhysicsSnapshot::NoIndex);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto coursePath = directory.filePath(QStringLiteral("prepared-schema7.crs"));
        const auto crs = WorkoutGameCourseCrsExporter::encode(source);
        QVERIFY(!crs.isEmpty());
        QFile courseFile(coursePath);
        QVERIFY(courseFile.open(QIODevice::WriteOnly));
        QCOMPARE(courseFile.write(crs), qint64(crs.size()));
        courseFile.close();
        QFile metadata(WorkoutGameCourseDocumentStore::sidecarPathForCourse(coursePath));
        QVERIFY(metadata.open(QIODevice::WriteOnly));
        QCOMPARE(metadata.write(encoded), qint64(encoded.size()));
        metadata.close();
        WorkoutGameCourseDocument playable;
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(coursePath, playable, error),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);
        QVERIFY(!playable.course.roadPlan);
        QVERIFY(!error.isEmpty());
        source.schemaVersion = 6;
        QVERIFY(WorkoutGameCourseDocumentCodec::encode(source).isEmpty());
    }

    void snapshotChecksStructureBeforeEncodedSize()
    {
        const auto canonical = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(samplePhysicsDocument())).object();
        auto root = canonical;
        auto plan = root.value("roadPlan").toObject();
        auto snapshot = plan.value("assetPhysicsSnapshot").toObject();
        // Unknown fields must not be serialized into a second large allocation
        // just to discover that this is not a snapshot we can interpret.
        snapshot["extra"] = QString(300000, 'x');
        plan["assetPhysicsSnapshot"] = snapshot;
        root["roadPlan"] = plan;
        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
        snapshot["snapshotVersion"] = 99;
        plan["assetPhysicsSnapshot"] = snapshot;
        root["roadPlan"] = plan;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);
    }

    void addedTerrainKindsRoundTrip()
    {
        for (WorkoutGameTerrainKind terrain : {
                WorkoutGameTerrainKind::LogOver,
                WorkoutGameTerrainKind::Tabletop,
                WorkoutGameTerrainKind::RockSlab,
                WorkoutGameTerrainKind::GapJump}) {
            WorkoutGameCourseDocument source = sampleDocument();
            setSectionTerrain(source, 0, terrain);
            WorkoutGameCourseDocument decoded;
            QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                        WorkoutGameCourseDocumentCodec::encode(source), decoded),
                     WorkoutGameCourseDocumentStatus::Ready);
            QCOMPARE(decoded.course.sections[0].terrain, terrain);
        }
    }

    void terrainEnumValuesRemainStableAndGapJumpAppends()
    {
        QCOMPARE(int(WorkoutGameTerrainKind::SmoothTrail), 0);
        QCOMPARE(int(WorkoutGameTerrainKind::Roots), 1);
        QCOMPARE(int(WorkoutGameTerrainKind::Rollers), 2);
        QCOMPARE(int(WorkoutGameTerrainKind::Climb), 3);
        QCOMPARE(int(WorkoutGameTerrainKind::RockGarden), 4);
        QCOMPARE(int(WorkoutGameTerrainKind::BunnyHop), 5);
        QCOMPARE(int(WorkoutGameTerrainKind::Drop), 6);
        QCOMPARE(int(WorkoutGameTerrainKind::Skinny), 7);
        QCOMPARE(int(WorkoutGameTerrainKind::Berm), 8);
        QCOMPARE(int(WorkoutGameTerrainKind::LogOver), 9);
        QCOMPARE(int(WorkoutGameTerrainKind::Tabletop), 10);
        QCOMPARE(int(WorkoutGameTerrainKind::RockSlab), 11);
        QCOMPARE(int(WorkoutGameTerrainKind::GapJump), 12);
    }

    void gapJumpRoundTripsWithoutChangingSchemaAndUsesSpecificCrsCue()
    {
        WorkoutGameCourseDocument source = sampleDocument();
        source.course.sections[0].feature = WorkoutGameFeature::SprintJump;
        setSectionTerrain(source, 0, WorkoutGameTerrainKind::GapJump);

        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(source);
        QVERIFY(encoded.contains("\"terrain\":\"gap-jump\""));
        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(decoded.course.sections[0].feature,
                 WorkoutGameFeature::SprintJump);
        QCOMPARE(decoded.course.sections[0].terrain,
                 WorkoutGameTerrainKind::GapJump);

        const QByteArray crs = WorkoutGameCourseCrsExporter::encode(decoded);
        QVERIFY(crs.contains("Gap jump"));
    }

    void existingVersionOneTerrainDocumentsRemainCanonical()
    {
        for (WorkoutGameTerrainKind terrain : {
                WorkoutGameTerrainKind::SmoothTrail,
                WorkoutGameTerrainKind::Roots,
                WorkoutGameTerrainKind::Rollers,
                WorkoutGameTerrainKind::Climb,
                WorkoutGameTerrainKind::RockGarden,
                WorkoutGameTerrainKind::BunnyHop,
                WorkoutGameTerrainKind::Drop,
                WorkoutGameTerrainKind::Skinny,
                WorkoutGameTerrainKind::Berm,
                WorkoutGameTerrainKind::LogOver,
                WorkoutGameTerrainKind::Tabletop,
                WorkoutGameTerrainKind::RockSlab}) {
            WorkoutGameCourseDocument source = sampleDocument();
            source.schemaVersion = 1;
            source.course.roadPlan.reset();
            source.course.sections[0].terrain = terrain;
            const QByteArray versionOne =
                    WorkoutGameCourseDocumentCodec::encode(source);
            QVERIFY(!versionOne.contains("gap-jump"));

            WorkoutGameCourseDocument decoded;
            QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                        versionOne, decoded),
                     WorkoutGameCourseDocumentStatus::Ready);
            QCOMPARE(decoded.schemaVersion, 1);
            QVERIFY(!decoded.course.roadPlan);
            QCOMPARE(decoded.course.sections[0].terrain, terrain);
            QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded),
                     versionOne);
        }
    }

    void versionOneLoadsWithoutRoadPlanAndExplicitSaveUpgradesToCurrent()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 1;
        legacy.course.roadPlan.reset();
        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(!encoded.contains("roadPlan"));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 1);
        QVERIFY(!decoded.course.roadPlan);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("legacy.crs"));
        QString error;
        const WorkoutGameCourseDocumentStatus saveStatus =
                WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, decoded, error);
        QVERIFY2(saveStatus == WorkoutGameCourseDocumentStatus::Ready,
                 qPrintable(error));
        WorkoutGameCourseDocument upgraded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    path, upgraded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(upgraded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(upgraded.conversionAlgorithmVersion,
                 WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion);
        QVERIFY(upgraded.course.roadPlan);
        QVERIFY(WorkoutGameRoadQuality::audit(
                    *upgraded.course.roadPlan).accepted());
    }

    void versionTwoRemainsCanonicalAndReadable()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        root.insert(QStringLiteral("schemaVersion"), 2);
        QJsonObject legacyPlan =
                root.value(QStringLiteral("roadPlan")).toObject();
        legacyPlan.insert(QStringLiteral("generationVersion"),
                          int(WorkoutGameRoadPlan
                              ::BankAndReliefGenerationVersion));
        legacyPlan.remove(QStringLiteral("assetPhysicsSnapshot"));
        root.insert(QStringLiteral("roadPlan"), legacyPlan);
        QJsonObject source = root.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('a')));
        root.insert(QStringLiteral("source"), source);
        QJsonObject conversion =
                root.value(QStringLiteral("conversion")).toObject();
        conversion.remove(QStringLiteral("algorithmVersion"));
        root.insert(QStringLiteral("conversion"), conversion);
        const QByteArray versionTwo =
                QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(versionTwo, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 2);
        QCOMPARE(decoded.conversionAlgorithmVersion,
                 WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), versionTwo);
    }

    void versionFiveHashLoadsAndExplicitSaveUpgradesWithoutIt()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 5;
        const QByteArray legacyJson =
                WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(legacyJson.contains("\"sha256\""));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    legacyJson, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 5);
        QCOMPARE(decoded.sourceSha256, legacy.sourceSha256);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath(
                QStringLiteral("legacy-v5.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, decoded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument upgraded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, upgraded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(upgraded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QVERIFY(upgraded.sourceSha256.isEmpty());

        QFile sidecar(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    coursePath));
        QVERIFY(sidecar.open(QIODevice::ReadOnly));
        QVERIFY(!sidecar.readAll().contains("\"sha256\""));
    }

    void versionSixRetainsLegacyMarkerAndDefaultWriterVersion()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 6;
        legacy.conversionAlgorithmVersion = 6;
        auto legacyPlan = std::make_shared<WorkoutGameRoadPlan>(
                *legacy.course.roadPlan);
        legacyPlan->generationVersion =
                WorkoutGameRoadPlan::BankAndReliefGenerationVersion;
        legacyPlan->assetPhysicsSnapshot.reset();
        legacy.course.roadPlan = legacyPlan;
        const QByteArray versionSix =
                WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(!versionSix.isEmpty());
        QVERIFY(!versionSix.contains("assetPhysicsSnapshot"));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    versionSix, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 6);
        QCOMPARE(decoded.conversionAlgorithmVersion, 6);
        QCOMPARE(decoded.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::BankAndReliefGenerationVersion);
        QVERIFY(decoded.course.roadPlan->assetPhysicsSnapshot);
        QCOMPARE(decoded.course.roadPlan->assetPhysicsSnapshot
                    ->pieceBindings.size(),
                 decoded.course.roadPlan->pieces.size());
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), versionSix);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath(
                QStringLiteral("legacy-v6.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, decoded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument upgraded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, upgraded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(upgraded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(upgraded.conversionAlgorithmVersion, 6);
        QCOMPARE(upgraded.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::BankAndReliefGenerationVersion);
        QVERIFY(upgraded.course.roadPlan->assetPhysicsSnapshot);
        const QByteArray upgradedJson =
                WorkoutGameCourseDocumentCodec::encode(upgraded);
        QVERIFY(!upgradedJson.contains("assetPhysicsSnapshot"));
        QVERIFY(!upgradedJson.contains("sha256"));
        QVERIFY(!upgradedJson.contains("digest"));
    }

    void versionFourHashLoadsAndReplacementUpgradesWithoutIt()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 4;
        legacy.conversionAlgorithmVersion = 5;
        const QByteArray legacyJson =
                WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(legacyJson.contains("\"sha256\""));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    legacyJson, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 4);
        QCOMPARE(decoded.sourceSha256, legacy.sourceSha256);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath(
                QStringLiteral("legacy-v4.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, decoded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        decoded.title = QStringLiteral("Replaced legacy v4");
        QCOMPARE(WorkoutGameCourseDocumentStore::replaceArtifact(
                    coursePath, decoded, error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseDocument upgraded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, upgraded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(upgraded.schemaVersion,
                 WorkoutGameCourseDocumentCodec::CurrentSchemaVersion);
        QCOMPARE(upgraded.title, decoded.title);
        QVERIFY(upgraded.sourceSha256.isEmpty());

        QFile sidecar(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    coursePath));
        QVERIFY(sidecar.open(QIODevice::ReadOnly));
        QVERIFY(!sidecar.readAll().contains("\"sha256\""));
    }

    void versionThreeRemainsCanonicalAndRejectsVersionFourAnnotations()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        root.insert(QStringLiteral("schemaVersion"), 3);
        QJsonObject legacyPlan =
                root.value(QStringLiteral("roadPlan")).toObject();
        legacyPlan.insert(QStringLiteral("generationVersion"),
                          int(WorkoutGameRoadPlan
                              ::BankAndReliefGenerationVersion));
        legacyPlan.remove(QStringLiteral("assetPhysicsSnapshot"));
        root.insert(QStringLiteral("roadPlan"), legacyPlan);
        QJsonObject legacySource =
                root.value(QStringLiteral("source")).toObject();
        legacySource.insert(
                QStringLiteral("sha256"), QString(64, QLatin1Char('a')));
        root.insert(QStringLiteral("source"), legacySource);
        QJsonObject conversion =
                root.value(QStringLiteral("conversion")).toObject();
        conversion.insert(QStringLiteral("algorithmVersion"), 2);
        root.insert(QStringLiteral("conversion"), conversion);
        const QByteArray versionThree =
                QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    versionThree, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.schemaVersion, 3);
        QCOMPARE(decoded.conversionAlgorithmVersion, 2);
        QVERIFY(decoded.sourceLaps.empty());
        QVERIFY(decoded.sourceTexts.empty());
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), versionThree);

        conversion.insert(QStringLiteral("algorithmVersion"), 3);
        root.insert(QStringLiteral("conversion"), conversion);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);

        QJsonObject annotatedRoot = root;
        conversion.insert(QStringLiteral("algorithmVersion"), 2);
        annotatedRoot.insert(QStringLiteral("conversion"), conversion);
        QJsonObject source =
                annotatedRoot.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("laps"), QJsonArray {
            QJsonObject {
                {QStringLiteral("timeMs"), 1000},
                {QStringLiteral("name"), QStringLiteral("Hidden v4 lap")}
            }
        });
        annotatedRoot.insert(QStringLiteral("source"), source);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(annotatedRoot).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
    }

    void algorithmFourCurrentSchemaRemainsReadable()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject conversion =
                root.value(QStringLiteral("conversion")).toObject();
        conversion.insert(QStringLiteral("algorithmVersion"), 4);
        root.insert(QStringLiteral("conversion"), conversion);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.conversionAlgorithmVersion, 4);
    }

    void currentSchemaRejectsUnannotatedSourceAndGeneratedDurationDifference()
    {
        WorkoutGameCourseDocument source = sampleDocument();
        source.sourceIntervals = {
            {0, 10000, 150.0, 250.0},
            {10000, 20600, 100.0, 100.0}
        };
        QCOMPARE(source.course.nominalDurationMs, std::int64_t(30000));

        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(source);
        QVERIFY(encoded.isEmpty());
    }

    void distanceDrivenSchemaIgnoresLegacyRecoveryExposureLimits()
    {
        WorkoutGameCourseDocument source = sampleDocument();
        source.sourceIntervals = {
            {0, 10000, 150.0, 250.0},
            {10000, 20000, 100.0, 100.0}
        };
        QCOMPARE(source.course.sections[1].minimumDurationMs,
                 std::int64_t(14000));

        QVERIFY(!WorkoutGameCourseDocumentCodec::encode(source).isEmpty());

        source.schemaVersion = 4;
        source.conversionAlgorithmVersion = 5;
        QVERIFY(WorkoutGameCourseDocumentCodec::encode(source).isEmpty());
    }

    void currentSchemaRoundTripsExplicitVersionedCooldownMetadata()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject source = root.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("intervals"), QJsonArray {
            QJsonObject {
                {QStringLiteral("startMs"), 0},
                {QStringLiteral("durationMs"), 10000},
                {QStringLiteral("startWatts"), 150.0},
                {QStringLiteral("endWatts"), 250.0}
            },
            QJsonObject {
                {QStringLiteral("startMs"), 10000},
                {QStringLiteral("durationMs"), 20600},
                {QStringLiteral("startWatts"), 100.0},
                {QStringLiteral("endWatts"), 100.0}
            }
        });
        source.insert(QStringLiteral("prescriptionMetadata"), QJsonObject {
            {QStringLiteral("version"), 1},
            {QStringLiteral("intervalRoles"), QJsonArray {
                QStringLiteral("prescribed"),
                QStringLiteral("non-prescriptive-cooldown")
            }}
        });
        root.insert(QStringLiteral("source"), source);
        const QByteArray encoded =
                QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.sourceIntervals.back().startMs
                    + decoded.sourceIntervals.back().durationMs,
                 std::int64_t(30600));
        QCOMPARE(decoded.course.nominalDurationMs, std::int64_t(30000));
        const QByteArray repeated = WorkoutGameCourseDocumentCodec::encode(decoded);
        QVERIFY(repeated.contains("\"prescriptionMetadata\""));
        QVERIFY(repeated.contains("\"version\":1"));
        QVERIFY(repeated.contains("\"non-prescriptive-cooldown\""));
    }

    void unknownOrMismatchedPrescriptionMetadataFailsClosed()
    {
        QJsonObject canonical = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject source = canonical.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("intervals"), QJsonArray {
            QJsonObject {
                {QStringLiteral("startMs"), 0},
                {QStringLiteral("durationMs"), 10000},
                {QStringLiteral("startWatts"), 150.0},
                {QStringLiteral("endWatts"), 250.0}
            },
            QJsonObject {
                {QStringLiteral("startMs"), 10000},
                {QStringLiteral("durationMs"), 20000},
                {QStringLiteral("startWatts"), 100.0},
                {QStringLiteral("endWatts"), 100.0}
            }
        });
        source.insert(QStringLiteral("prescriptionMetadata"), QJsonObject {
            {QStringLiteral("version"), 99},
            {QStringLiteral("intervalRoles"), QJsonArray {
                QStringLiteral("prescribed"),
                QStringLiteral("prescribed")
            }}
        });
        canonical.insert(QStringLiteral("source"), source);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(canonical).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);

        QJsonObject metadata = source.value(
                QStringLiteral("prescriptionMetadata")).toObject();
        metadata.insert(QStringLiteral("version"), 1);
        metadata.insert(QStringLiteral("intervalRoles"), QJsonArray());
        source.insert(QStringLiteral("prescriptionMetadata"), metadata);
        canonical.insert(QStringLiteral("source"), source);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(canonical).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        metadata.insert(QStringLiteral("intervalRoles"), QJsonArray {
            QStringLiteral("prescribed"),
            QStringLiteral("non-prescriptive-transition")
        });
        source.insert(QStringLiteral("prescriptionMetadata"), metadata);
        canonical.insert(QStringLiteral("source"), source);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(canonical).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
    }

    void unknownConversionAlgorithmVersionFailsClosed()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject conversion =
                root.value(QStringLiteral("conversion")).toObject();
        conversion.insert(QStringLiteral("algorithmVersion"), 99);
        root.insert(QStringLiteral("conversion"), conversion);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);
    }

    void unknownRoadGenerationVersionFailsClosed()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject roadPlan = root.value(QStringLiteral("roadPlan")).toObject();
        roadPlan.insert(QStringLiteral("generationVersion"), 99);
        root.insert(QStringLiteral("roadPlan"), roadPlan);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);
    }

    void legacyRoadGenerationOneRemainsCanonicalAndReadable()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 6;
        legacy.conversionAlgorithmVersion = 6;
        auto plan = std::make_shared<WorkoutGameRoadPlan>(
                *legacy.course.roadPlan);
        plan->generationVersion =
                WorkoutGameRoadPlan::LegacyGenerationVersion;
        for (WorkoutGameRoadPiece &piece : plan->pieces) {
            piece.bank = WorkoutGameRoadBankProfile();
            piece.relief = WorkoutGameRoadReliefProfile();
        }
        legacy.course.roadPlan = plan;
        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(legacy);
        QVERIFY(!encoded.contains("\"bank\""));
        QVERIFY(!encoded.contains("\"relief\""));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::LegacyGenerationVersion);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(decoded), encoded);
    }

    void savingSchemaTwoGenerationOneRegeneratesCurrentRoadMetadata()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 2;
        legacy.conversionAlgorithmVersion =
                WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion;
        auto plan = std::make_shared<WorkoutGameRoadPlan>(
                *legacy.course.roadPlan);
        plan->generationVersion =
                WorkoutGameRoadPlan::LegacyGenerationVersion;
        for (WorkoutGameRoadPiece &piece : plan->pieces) {
            piece.bank = WorkoutGameRoadBankProfile();
            piece.relief = WorkoutGameRoadReliefProfile();
        }
        legacy.course.roadPlan = plan;
        QCOMPARE(legacy.schemaVersion, 2);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(
                QStringLiteral("generation-one.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, legacy, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    path, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QVERIFY(loaded.course.roadPlan);
        QCOMPARE(loaded.course.roadPlan->generationVersion,
                 WorkoutGameRoadPlan::BankAndReliefGenerationVersion);
        QVERIFY(std::all_of(
                loaded.course.roadPlan->pieces.begin(),
                loaded.course.roadPlan->pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.relief.enabled;
                }));
    }

    void savingVersionThreePreservesItsAlgorithmAndStoredRoadShape()
    {
        WorkoutGameCourseDocument versionThree = sampleDocument();
        versionThree.schemaVersion = 3;
        versionThree.conversionAlgorithmVersion = 2;
        versionThree.preset = WorkoutGameCoursePreset::RideFirst;
        versionThree.generationParameters =
                WorkoutGameCourseConverter::parametersForPreset(
                    versionThree.preset);
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(versionThree));

        const std::shared_ptr<const WorkoutGameRoadPlan> expected =
                versionThree.course.roadPlan;
        QVERIFY(expected);
        QVERIFY(!expected->pieces.empty());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(
                QStringLiteral("version-three-ride-first.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, versionThree, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    path, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(loaded.conversionAlgorithmVersion, 2);
        QCOMPARE(loaded.preset, WorkoutGameCoursePreset::RideFirst);
        QCOMPARE(loaded.course.roadPlan->pieces.size(), expected->pieces.size());
        for (std::size_t index = 0; index < expected->pieces.size(); ++index) {
            QCOMPARE(loaded.course.roadPlan->pieces[index].turnRadians,
                     expected->pieces[index].turnRadians);
        }
    }

    void legacyBermMigratesToUnscoredPersistedBank()
    {
        WorkoutGameCourseDocument legacy = sampleDocument();
        legacy.schemaVersion = 1;
        legacy.course.sections[0].feature = WorkoutGameFeature::Trail;
        legacy.course.sections[0].terrain = WorkoutGameTerrainKind::Berm;
        legacy.course.sections[0].gradePercent = 0.0;
        legacy.course.roadPlan.reset();
        QVERIFY(WorkoutGameCourseDocumentCodec::valid(legacy));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(
                QStringLiteral("legacy-berm.crs"));
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    path, legacy, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    path, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QVERIFY(loaded.course.roadPlan);
        const auto migrated = std::find_if(
                loaded.course.roadPlan->pieces.begin(),
                loaded.course.roadPlan->pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(migrated != loaded.course.roadPlan->pieces.end());
        QVERIFY(migrated->bank.enabled);
        QVERIFY(!migrated->challenge.enabled);
        QVERIFY(!migrated->qualityExempt);
    }

    void malformedBankAndReliefMetadataAreRejected()
    {
        const QJsonObject canonical = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        WorkoutGameCourseDocument decoded;

        QJsonObject invalidBank = canonical;
        QJsonObject plan = invalidBank.value(QStringLiteral("roadPlan")).toObject();
        QJsonArray pieces = plan.value(QStringLiteral("pieces")).toArray();
        QJsonObject piece = pieces[4].toObject();
        QJsonObject bank = piece.value(QStringLiteral("bank")).toObject();
        bank.insert(QStringLiteral("maximumBankRadians"), 2.0);
        piece.insert(QStringLiteral("bank"), bank);
        pieces[4] = piece;
        plan.insert(QStringLiteral("pieces"), pieces);
        invalidBank.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(invalidBank).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        QJsonObject invalidRelief = canonical;
        plan = invalidRelief.value(QStringLiteral("roadPlan")).toObject();
        pieces = plan.value(QStringLiteral("pieces")).toArray();
        piece = pieces[0].toObject();
        QJsonObject relief = piece.value(QStringLiteral("relief")).toObject();
        relief.insert(QStringLiteral("phaseRadians"), QStringLiteral("nan"));
        piece.insert(QStringLiteral("relief"), relief);
        pieces[0] = piece;
        plan.insert(QStringLiteral("pieces"), pieces);
        invalidRelief.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(invalidRelief).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
    }

    void malformedAndOversizedRoadPlansAreRejected()
    {
        const QJsonObject canonical = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        WorkoutGameCourseDocument decoded;

        QJsonObject missing = canonical;
        missing.remove(QStringLiteral("roadPlan"));
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(missing).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        QJsonObject malformed = canonical;
        QJsonObject malformedPlan = malformed.value(
                QStringLiteral("roadPlan")).toObject();
        QJsonArray malformedPieces = malformedPlan.value(
                QStringLiteral("pieces")).toArray();
        QJsonObject malformedPiece = malformedPieces[0].toObject();
        malformedPiece.insert(QStringLiteral("turnRadians"),
                              QStringLiteral("nan"));
        malformedPieces[0] = malformedPiece;
        malformedPlan.insert(QStringLiteral("pieces"), malformedPieces);
        malformed.insert(QStringLiteral("roadPlan"), malformedPlan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(malformed).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        QJsonObject oversized = canonical;
        QJsonObject oversizedPlan = oversized.value(
                QStringLiteral("roadPlan")).toObject();
        QJsonArray excessivePieces;
        for (std::size_t index = 0;
             index <= WorkoutGameRoadPlan::MaximumPieces; ++index) {
            excessivePieces.append(QJsonObject());
        }
        oversizedPlan.insert(QStringLiteral("pieces"), excessivePieces);
        oversized.insert(QStringLiteral("roadPlan"), oversizedPlan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(oversized).toJson(
                        QJsonDocument::Compact), decoded),
                 WorkoutGameCourseDocumentStatus::ResourceLimit);
    }

    void malformedUnsupportedAndOversizedPhysicsSnapshotsFailClosed()
    {
        const QJsonObject canonical = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(samplePhysicsDocument()))
                .object();
        WorkoutGameCourseDocument decoded;

        QJsonObject missing = canonical;
        QJsonObject plan = missing.value(QStringLiteral("roadPlan")).toObject();
        plan.remove(QStringLiteral("assetPhysicsSnapshot"));
        missing.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(missing).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        QJsonObject unsupported = canonical;
        plan = unsupported.value(QStringLiteral("roadPlan")).toObject();
        QJsonObject snapshot = plan.value(
                QStringLiteral("assetPhysicsSnapshot")).toObject();
        snapshot.insert(QStringLiteral("snapshotVersion"), 99);
        plan.insert(QStringLiteral("assetPhysicsSnapshot"), snapshot);
        unsupported.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(unsupported).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::UnsupportedVersion);

        QJsonObject mismatched = canonical;
        plan = mismatched.value(QStringLiteral("roadPlan")).toObject();
        snapshot = plan.value(QStringLiteral("assetPhysicsSnapshot")).toObject();
        snapshot.insert(QStringLiteral("pieceBindings"), QJsonArray());
        plan.insert(QStringLiteral("assetPhysicsSnapshot"), snapshot);
        mismatched.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(mismatched).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);

        for (const int remainder : {-501, 501}) {
            QJsonObject invalidAnchor = canonical;
            plan = invalidAnchor.value(QStringLiteral("roadPlan")).toObject();
            snapshot = plan.value(
                    QStringLiteral("assetPhysicsSnapshot")).toObject();
            QJsonArray pieceBindings = snapshot.value(
                    QStringLiteral("pieceBindings")).toArray();
            QJsonObject pieceBinding = pieceBindings.at(0).toObject();
            pieceBinding.insert(
                    QStringLiteral("obstacleAnchorMicrometerRemainder"),
                    remainder);
            pieceBindings[0] = pieceBinding;
            snapshot.insert(QStringLiteral("pieceBindings"), pieceBindings);
            plan.insert(QStringLiteral("assetPhysicsSnapshot"), snapshot);
            invalidAnchor.insert(QStringLiteral("roadPlan"), plan);
            QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                        QJsonDocument(invalidAnchor).toJson(), decoded),
                     WorkoutGameCourseDocumentStatus::InvalidDocument);
        }

        QJsonObject oversized = canonical;
        plan = oversized.value(QStringLiteral("roadPlan")).toObject();
        snapshot = plan.value(QStringLiteral("assetPhysicsSnapshot")).toObject();
        QJsonArray definitions;
        for (std::size_t index = 0;
             index <= WorkoutGameCourseAssetPhysicsSnapshot::MaximumDefinitions;
             ++index) {
            definitions.append(QJsonObject());
        }
        snapshot.insert(QStringLiteral("physicsDefinitions"), definitions);
        plan.insert(QStringLiteral("assetPhysicsSnapshot"), snapshot);
        oversized.insert(QStringLiteral("roadPlan"), plan);
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(oversized).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::ResourceLimit);
    }

    void privateSourcePathIsRejected_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::newRow("absolute-path")
                << QStringLiteral("/home/private/workout.erg");
        QTest::newRow("relative-path")
                << QStringLiteral("folder/workout.erg");
    }

    void privateSourcePathIsRejected()
    {
        QFETCH(QString, fileName);
        WorkoutGameCourseDocument document = sampleDocument();
        document.sourceFileName = fileName;

        QVERIFY(WorkoutGameCourseDocumentCodec::encode(document).isEmpty());
    }

    void legacyMalformedSourceHashIsRejected_data()
    {
        QTest::addColumn<QString>("hash");
        QTest::newRow("missing-hash") << QString();
        QTest::newRow("short-hash") << QStringLiteral("abc");
        QTest::newRow("non-hex-hash") << QString(64, QLatin1Char('z'));
    }

    void legacyMalformedSourceHashIsRejected()
    {
        QFETCH(QString, hash);
        WorkoutGameCourseDocument document = sampleDocument();
        document.schemaVersion = 5;
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

        const QByteArray oversized(
                WorkoutGameCourseDocumentCodec::MaximumDocumentBytes + 1,
                'x');
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    oversized, decoded),
                 WorkoutGameCourseDocumentStatus::ResourceLimit);
    }

    void legacyDocumentWithoutTechnicalityStillLoads()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject conversion = root.value(QStringLiteral("conversion")).toObject();
        QJsonObject parameters = conversion.value(QStringLiteral("parameters")).toObject();
        parameters.remove(QStringLiteral("technicality"));
        conversion.insert(QStringLiteral("parameters"), parameters);
        root.insert(QStringLiteral("conversion"), conversion);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.generationParameters.technicality, 0.55);
        QVERIFY(decoded.sourceIntervals.empty());
    }

    void discontinuousStoredSourceProfileIsRejected()
    {
        QJsonObject root = QJsonDocument::fromJson(
                WorkoutGameCourseDocumentCodec::encode(sampleDocument()))
                .object();
        QJsonObject source = root.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("intervals"), QJsonArray {
            QJsonObject {
                {QStringLiteral("startMs"), 1000},
                {QStringLiteral("durationMs"), 30000},
                {QStringLiteral("startWatts"), 150.0},
                {QStringLiteral("endWatts"), 150.0}
            }
        });
        root.insert(QStringLiteral("source"), source);

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                    QJsonDocument(root).toJson(), decoded),
                 WorkoutGameCourseDocumentStatus::InvalidDocument);
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

    void sourceLapsAndInstructionsRoundTripAndMapToCourseDistance()
    {
        WorkoutGameCourseDocument source = sampleDocument();
        source.sourceLaps = {
            {15000, QStringLiteral("Tempo block")}
        };
        source.sourceTexts = {
            {5000, 6, QStringLiteral("Hold 95 rpm")},
            {25000, 8, QStringLiteral("Relax shoulders")}
        };

        const QByteArray encoded = WorkoutGameCourseDocumentCodec::encode(source);
        QVERIFY(encoded.contains("\"laps\""));
        QVERIFY(encoded.contains("\"texts\""));

        WorkoutGameCourseDocument decoded;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(encoded, decoded),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(decoded.sourceLaps.size(), std::size_t(1));
        QCOMPARE(decoded.sourceLaps[0].timeMs, std::int64_t(15000));
        QCOMPARE(decoded.sourceLaps[0].name, QStringLiteral("Tempo block"));
        QCOMPARE(decoded.sourceTexts.size(), std::size_t(2));
        QCOMPARE(decoded.sourceTexts[1].durationSeconds, 8);
        QCOMPARE(decoded.sourceTexts[1].text,
                 QStringLiteral("Relax shoulders"));

        const QByteArray crs = WorkoutGameCourseCrsExporter::encode(decoded);
        QVERIFY(crs.contains("0.050000 -4.000 0.0\nLAP Tempo block\n"));
        QVERIFY(crs.contains("0.050000 Hold 95 rpm 6\n"));
        QVERIFY(crs.contains("0.250000 Relax shoulders 8\n"));
    }

    void invalidSourceInstructionsFailClosed()
    {
        WorkoutGameCourseDocument source = sampleDocument();
        source.sourceTexts = {
            {5000, 6, QStringLiteral("unsafe\ncue")}
        };
        QVERIFY(WorkoutGameCourseDocumentCodec::encode(source).isEmpty());

        source.sourceTexts = {
            {source.course.nominalDurationMs + 1, 6,
             QStringLiteral("outside workout")}
        };
        QVERIFY(WorkoutGameCourseDocumentCodec::encode(source).isEmpty());
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

    void existingArtifactCanBeReplacedAsAValidatedPair()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString crsPath = directory.filePath(QStringLiteral("course.crs"));
        QString error;
        WorkoutGameCourseDocument original = sampleDocument();
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    crsPath, original, error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseDocument replacement = original;
        replacement.title = QStringLiteral("Edited course");
        QCOMPARE(WorkoutGameCourseDocumentStore::replaceArtifact(
                    crsPath, replacement, error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    crsPath, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(loaded.title, replacement.title);
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
