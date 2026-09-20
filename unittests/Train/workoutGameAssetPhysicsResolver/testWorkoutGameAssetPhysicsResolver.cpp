/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAssetPhysicsResolver.h"
#include "Train/WorkoutGameAssetCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <array>
#include <limits>
#include <memory>

namespace {

WorkoutGameRoadPiece logOver(double difficulty, double anchorMeters = 10.0)
{
    WorkoutGameRoadPiece piece;
    piece.terrain = WorkoutGameTerrainKind::LogOver;
    piece.difficulty = difficulty;
    piece.geometryAnchorDistanceMeters = anchorMeters;
    piece.challenge.enabled = true;
    piece.challenge.obstacleDistanceMeters = anchorMeters;
    return piece;
}

std::unique_ptr<const WorkoutGameAssetCatalog> visualOnlyCatalog(
        const QString &assetId)
{
    const QJsonObject root {
        {QStringLiteral("assets"), QJsonArray {
            QJsonObject {
                {QStringLiteral("assetId"), assetId},
                {QStringLiteral("physics"), QJsonObject {
                    {QStringLiteral("authority"), QStringLiteral("external")},
                    {QStringLiteral("interaction"), QStringLiteral("visual-only")},
                }},
                {QStringLiteral("resources"), QJsonArray {
                    QJsonObject {
                        {QStringLiteral("bytes"), 1},
                        {QStringLiteral("purpose"), QStringLiteral("runtime")},
                        {QStringLiteral("repositoryPath"),
                            QStringLiteral("src/Train/qml/TestAsset.qml")},
                        {QStringLiteral("url"),
                            QStringLiteral("qrc:/qml/TestAsset.qml")},
                    },
                }},
                {QStringLiteral("role"), QStringLiteral("feature")},
            },
        }},
        {QStringLiteral("generatorVersion"), 4},
        {QStringLiteral("profiles"), QJsonArray()},
        {QStringLiteral("schemaVersion"), 1},
    };
    QString error;
    auto catalog = WorkoutGameAssetCatalog::fromJson(
            QJsonDocument(root).toJson(QJsonDocument::Compact), &error);
    Q_ASSERT_X(catalog, "visualOnlyCatalog", qPrintable(error));
    return catalog;
}

}

class TestWorkoutGameAssetPhysicsResolver : public QObject
{
    Q_OBJECT

private slots:
    void resolvesFt02AtThreeDifficulties()
    {
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));

        struct Expected {
            double difficulty;
            std::uint32_t extent;
            std::array<std::int32_t, 11> forward;
            std::array<std::int32_t, 11> height;
        };
        const std::array<Expected, 3> cases {{
            {0.0, 440,
             {-220, -204, -203, -156, -84, 0,
                 84, 156, 203, 204, 220},
             {0, 162, 169, 310, 407, 440,
                 407, 310, 169, 162, 0}},
            {0.5, 540,
             {-270, -250, -249, -191, -103, 0,
                 103, 191, 249, 250, 270},
             {0, 202, 208, 382, 499, 540,
                 499, 382, 208, 202, 0}},
            {1.0, 640,
             {-320, -296, -295, -226, -122, 0,
                 122, 226, 295, 296, 320},
             {0, 242, 247, 453, 591, 640,
                 591, 453, 247, 242, 0}},
        }};

        for (const Expected &expected : cases) {
            const auto result = WorkoutGameAssetPhysicsResolver::resolve(
                    *catalog, {logOver(expected.difficulty)});
            QCOMPARE(result.status,
                     WorkoutGameAssetPhysicsResolveStatus::Ready);
            QVERIFY(result.snapshot);
            QCOMPARE(result.snapshot->catalogSchemaVersion, std::uint32_t(1));
            QCOMPARE(result.snapshot->physicsDefinitions.size(), std::size_t(1));
            QCOMPARE(result.snapshot->bindings.size(), std::size_t(1));
            QCOMPARE(result.snapshot->pieceBindings.size(), std::size_t(1));

            const auto &definition = result.snapshot->physicsDefinitions[0];
            QCOMPARE(definition.operation,
                     WorkoutGameAssetPhysicsOperation::AddObstacle);
            QCOMPARE(definition.coulombFrictionMilli, std::uint16_t(1100));
            QCOMPARE(definition.restitutionMilli, std::uint16_t(0));
            QCOMPARE(definition.chains.size(), std::size_t(1));
            QCOMPARE(definition.chains[0].points.size(), std::size_t(11));
            for (std::size_t index = 0; index < 11; ++index) {
                QCOMPARE(definition.chains[0].points[index].forwardMm,
                         expected.forward[index]);
                QCOMPARE(definition.chains[0].points[index].heightMm,
                         expected.height[index]);
            }

            const auto &binding = result.snapshot->bindings[0];
            QCOMPARE(binding.assetId,
                     QStringLiteral("FT-02-log-over-greybox"));
            QCOMPARE(binding.variantKey, QString());
            QCOMPARE(binding.definitionIndex, std::uint32_t(0));
            QCOMPARE(binding.nativeForwardOriginMm, std::int32_t(-1020));
            QCOMPARE(binding.nativeForwardExtentMm, std::uint32_t(540));
            QCOMPARE(binding.nativeUpExtentMm, std::uint32_t(540));
            QCOMPARE(binding.resolvedExtentMm, expected.extent);

            const auto &piece = result.snapshot->pieceBindings[0];
            QCOMPARE(piece.definitionIndex, std::uint32_t(0));
            QCOMPARE(piece.bindingIndex, std::uint32_t(0));
            QCOMPARE(piece.obstacleAnchorMm, std::int32_t(10000));
            QCOMPARE(piece.flags, std::uint32_t(0));
        }
    }

    void deduplicatesByResolvedDifficultyInFirstUseOrder()
    {
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));

        const auto result = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog,
                {logOver(0.5, 10.0), logOver(0.501, 20.0),
                 logOver(0.503, 30.0), logOver(0.5, 40.0)});
        QCOMPARE(result.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
        QVERIFY(result.snapshot);
        QCOMPARE(result.snapshot->physicsDefinitions.size(), std::size_t(2));
        QCOMPARE(result.snapshot->bindings.size(), std::size_t(2));
        QCOMPARE(result.snapshot->bindings[0].resolvedExtentMm,
                 std::uint32_t(540));
        QCOMPARE(result.snapshot->bindings[1].resolvedExtentMm,
                 std::uint32_t(541));
        const std::array<std::uint32_t, 4> expected {0, 0, 1, 0};
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(result.snapshot->pieceBindings[index].definitionIndex,
                     expected[index]);
            QCOMPARE(result.snapshot->pieceBindings[index].bindingIndex,
                     expected[index]);
        }
    }

    void leavesUnmatchedPiecesExplicitlyUnbound()
    {
        const auto catalog = visualOnlyCatalog(QStringLiteral("AA-00-test"));
        QVERIFY(catalog);
        WorkoutGameRoadPiece piece;
        piece.terrain = WorkoutGameTerrainKind::Roots;
        piece.geometryAnchorDistanceMeters = 12.345;

        const auto result = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, {piece});
        QCOMPARE(result.status, WorkoutGameAssetPhysicsResolveStatus::Ready);
        QVERIFY(result.snapshot);
        QVERIFY(result.snapshot->physicsDefinitions.empty());
        QVERIFY(result.snapshot->bindings.empty());
        QCOMPARE(result.snapshot->pieceBindings.size(), std::size_t(1));
        const auto &binding = result.snapshot->pieceBindings[0];
        QCOMPARE(binding.definitionIndex,
                 WorkoutGameCourseAssetPhysicsSnapshot::NoIndex);
        QCOMPARE(binding.bindingIndex,
                 WorkoutGameCourseAssetPhysicsSnapshot::NoIndex);
        QCOMPARE(binding.obstacleAnchorMm, std::int32_t(12345));
    }

    void failsAtomicallyWhenRequiredAssetOrProfileIsMissing()
    {
        const auto missingAsset = visualOnlyCatalog(QStringLiteral("AA-00-test"));
        QVERIFY(missingAsset);
        auto result = WorkoutGameAssetPhysicsResolver::resolve(
                *missingAsset, {logOver(0.5)});
        QCOMPARE(result.status,
                 WorkoutGameAssetPhysicsResolveStatus::MissingAsset);
        QVERIFY(!result.snapshot);

        const auto missingProfile = visualOnlyCatalog(
                QStringLiteral("FT-02-log-over-greybox"));
        QVERIFY(missingProfile);
        result = WorkoutGameAssetPhysicsResolver::resolve(
                *missingProfile, {logOver(0.5)});
        QCOMPARE(result.status,
                 WorkoutGameAssetPhysicsResolveStatus::MissingProfile);
        QVERIFY(!result.snapshot);
    }

    void rejectsMismatchedAndNonFiniteAnchors()
    {
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));

        WorkoutGameRoadPiece mismatch = logOver(0.5);
        mismatch.challenge.obstacleDistanceMeters += 0.001;
        auto result = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, {mismatch});
        QCOMPARE(result.status,
                 WorkoutGameAssetPhysicsResolveStatus::InvalidAnchor);
        QVERIFY(!result.snapshot);

        WorkoutGameRoadPiece invalid = logOver(0.5);
        invalid.geometryAnchorDistanceMeters =
                std::numeric_limits<double>::quiet_NaN();
        result = WorkoutGameAssetPhysicsResolver::resolve(*catalog, {invalid});
        QCOMPARE(result.status,
                 WorkoutGameAssetPhysicsResolveStatus::InvalidAnchor);
        QVERIFY(!result.snapshot);
    }

    void rejectsInvalidDifficultyAndResourceOverflow()
    {
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));

        for (double difficulty : {
                 -0.001, 1.001,
                 std::numeric_limits<double>::quiet_NaN()}) {
            const auto result = WorkoutGameAssetPhysicsResolver::resolve(
                    *catalog, {logOver(difficulty)});
            QCOMPARE(result.status,
                     WorkoutGameAssetPhysicsResolveStatus::InvalidDifficulty);
            QVERIFY(!result.snapshot);
        }

        std::vector<WorkoutGameRoadPiece> pieces(
                WorkoutGameCourseAssetPhysicsSnapshot::MaximumPieceBindings + 1);
        const auto result = WorkoutGameAssetPhysicsResolver::resolve(
                *catalog, pieces);
        QCOMPARE(result.status,
                 WorkoutGameAssetPhysicsResolveStatus::ResourceLimit);
        QVERIFY(!result.snapshot);
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameAssetPhysicsResolver)

#include "testWorkoutGameAssetPhysicsResolver.moc"
