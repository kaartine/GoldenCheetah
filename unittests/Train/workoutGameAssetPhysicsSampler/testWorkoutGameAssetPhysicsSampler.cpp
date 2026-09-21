/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAssetPhysicsSampler.h"

#include <QRegularExpression>
#include <QTest>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

WorkoutGameCourseAssetPhysicsSnapshot triangleSnapshot()
{
    WorkoutGameCourseAssetPhysicsSnapshot snapshot;
    snapshot.catalogSchemaVersion = 1;
    WorkoutGameAssetPhysicsDefinition definition;
    definition.coulombFrictionMilli = 1100;
    definition.restitutionMilli = 25;
    definition.chains = {{
        {{-270, 0}, {0, 540}, {270, 0}}
    }};
    snapshot.physicsDefinitions.push_back(definition);

    WorkoutGameAssetPhysicsBinding asset;
    asset.assetId = QStringLiteral("FT-02-log-over-greybox");
    asset.definitionIndex = 0;
    asset.nativeForwardExtentMm = 540;
    asset.nativeUpExtentMm = 540;
    asset.resolvedExtentMm = 540;
    snapshot.bindings.push_back(asset);

    WorkoutGameAssetPhysicsPieceBinding piece;
    piece.definitionIndex = 0;
    piece.bindingIndex = 0;
    piece.obstacleAnchorMm = 10000;
    snapshot.pieceBindings.push_back(piece);
    return snapshot;
}

WorkoutGameCourseAssetPhysicsSnapshot legacyFt02Snapshot(bool enabled = true)
{
    WorkoutGameCourseAssetPhysicsSnapshot snapshot;
    WorkoutGameLegacyFt02Record record;
    record.enabled = enabled;
    record.startMeters = -0.270049;
    record.endMeters = 0.270049;
    record.heightMeters = 0.540098;
    record.obstacleAnchorMeters = 12.3456789;
    snapshot.legacyFt02Records.push_back(record);

    WorkoutGameAssetPhysicsPieceBinding piece;
    piece.flags = WorkoutGameCourseAssetPhysicsSnapshot::LegacyProceduralV1;
    piece.legacyFt02RecordIndex = 0;
    piece.obstacleAnchorMm = 12346;
    piece.obstacleAnchorMicrometerRemainder = -321;
    snapshot.pieceBindings.push_back(piece);
    return snapshot;
}

bool identical(double left, double right)
{
    return std::memcmp(&left, &right, sizeof(double)) == 0;
}

}

class TestWorkoutGameAssetPhysicsSampler : public QObject
{
    Q_OBJECT

private slots:
    void interpolatesProfileAndExposesMaterial()
    {
        const auto snapshot = triangleSnapshot();
        const auto approach = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 9.865);
        QVERIFY(approach.bound);
        QVERIFY(approach.surfacePresent);
        QVERIFY(std::abs(approach.offsetMeters - 0.27) < 1e-12);
        QCOMPARE(approach.coulombFriction, 1.1);
        QCOMPARE(approach.restitution, 0.025);

        const auto crest = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 10.0);
        QVERIFY(crest.surfacePresent);
        QCOMPARE(crest.offsetMeters, 0.54);

        const auto outside = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 9.0);
        QVERIFY(outside.bound);
        QVERIFY(!outside.surfacePresent);
        QCOMPARE(outside.offsetMeters, 0.0);
    }

    void canonicalBinary64PreservesEveryFiniteBit()
    {
        const std::vector<double> values {
            0.0,
            -0.0,
            0.50049,
            12.3456789,
            std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::max()
        };
        for (double value : values) {
            const QString encoded = WorkoutGameLegacyBinary64::encode(value);
            QCOMPARE(encoded.size(), 16);
            QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{16}$"))
                        .match(encoded).hasMatch());
            double decoded = 0.0;
            QVERIFY(WorkoutGameLegacyBinary64::decode(encoded, decoded));
            QVERIFY(identical(decoded, value));
        }

        double unchanged = 7.0;
        for (const QString &invalid : {
                 QStringLiteral("000000000000000"),
                 QStringLiteral("00000000000000000"),
                 QStringLiteral("000000000000000G"),
                 QStringLiteral("000000000000000A"),
                 QStringLiteral("7ff0000000000000"),
                 QStringLiteral("7ff8000000000000")}) {
            double decoded = unchanged;
            QVERIFY(!WorkoutGameLegacyBinary64::decode(invalid, decoded));
            QCOMPARE(decoded, unchanged);
        }
        QVERIFY(WorkoutGameLegacyBinary64::encode(
                    std::numeric_limits<double>::infinity()).isEmpty());
        QVERIFY(WorkoutGameLegacyBinary64::encode(
                    std::numeric_limits<double>::quiet_NaN()).isEmpty());
    }

    void samplesFrozenLegacyGeometryWithoutCollisionOrMaterialBinding()
    {
        const auto snapshot = legacyFt02Snapshot();
        const auto crest = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 12.3456789);
        QVERIFY(crest.bound);
        QVERIFY(crest.surfacePresent);
        QVERIFY(!crest.obstacleContact);
        QVERIFY(!crest.materialDefined);
        QVERIFY(identical(crest.offsetMeters, 0.540098));

        const auto transform = WorkoutGameAssetPhysicsSampler::renderTransform(
                snapshot, 0);
        QCOMPARE(transform.status, WorkoutGameAssetRenderFitStatus::Ready);
        QCOMPARE(transform.assetId,
                 QStringLiteral("FT-02-log-over-greybox"));
        QVERIFY(identical(transform.obstacleAnchorMeters, 12.3456789));
        QVERIFY(identical(transform.forwardScale, 0.540098 / 0.54));
        QVERIFY(identical(transform.upScale, 0.540098 / 0.54));
        QVERIFY(WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                    snapshot, 0).empty());

        const auto disabled = WorkoutGameAssetPhysicsSampler::sample(
                legacyFt02Snapshot(false), 0, 12.3456789);
        QVERIFY(disabled.bound);
        QVERIFY(!disabled.surfacePresent);
        QCOMPARE(disabled.offsetMeters, 0.0);
        QCOMPARE(WorkoutGameAssetPhysicsSampler::renderTransform(
                     legacyFt02Snapshot(false), 0).status,
                 WorkoutGameAssetRenderFitStatus::Unbound);

        auto asymmetric = legacyFt02Snapshot();
        asymmetric.legacyFt02Records[0].startMeters = -0.20;
        asymmetric.legacyFt02Records[0].endMeters = 0.34;
        asymmetric.legacyFt02Records[0].heightMeters = 0.81;
        const auto asymmetricTransform =
                WorkoutGameAssetPhysicsSampler::renderTransform(
                    asymmetric, 0);
        QCOMPARE(asymmetricTransform.status,
                 WorkoutGameAssetRenderFitStatus::Ready);
        QVERIFY(identical(asymmetricTransform.forwardScale, 1.0));
        QVERIFY(identical(asymmetricTransform.upScale, 1.5));
        QVERIFY(identical(asymmetricTransform.forwardExtentMeters, 0.54));
        QVERIFY(identical(asymmetricTransform.upExtentMeters, 0.81));
        QVERIFY(identical(asymmetricTransform.assetStartDistanceMeters,
                          12.3456789 - 0.20 - 0.75));
    }

    void validatesLegacyRecordVersionsReferencesAndLimits()
    {
        auto snapshot = legacyFt02Snapshot();
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(snapshot, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready);

        snapshot.legacyFt02Records[0].recordVersion = 99;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(snapshot, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::UnsupportedVersion);
        snapshot = legacyFt02Snapshot();
        snapshot.legacyFt02Records[0].heightMeters =
                std::numeric_limits<double>::infinity();
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(snapshot, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);
        snapshot = legacyFt02Snapshot();
        snapshot.pieceBindings[0].legacyFt02RecordIndex = 1;
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(snapshot, 1),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot);

        snapshot = WorkoutGameCourseAssetPhysicsSnapshot();
        snapshot.legacyFt02Records.resize(
                WorkoutGameCourseAssetPhysicsSnapshot::MaximumLegacyRecords + 1);
        QCOMPARE(WorkoutGameAssetPhysicsSnapshotValidator::validate(snapshot, 0),
                 WorkoutGameAssetPhysicsSnapshotValidationStatus::ResourceLimit);
    }

    void returnsExactSortedBreakpoints()
    {
        const auto snapshot = triangleSnapshot();
        const auto points = WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                snapshot, 0);
        QCOMPARE(points.size(), std::size_t(3));
        QCOMPARE(points[0], 9.73);
        QCOMPARE(points[1], 10.0);
        QCOMPARE(points[2], 10.27);
    }

    void resolvesOneCompleteRenderTransform()
    {
        auto snapshot = triangleSnapshot();
        snapshot.bindings[0].nativeForwardOriginMm = -1020;
        snapshot.bindings[0].nativeUpExtentMm = 540;
        snapshot.bindings[0].resolvedExtentMm = 700;

        const WorkoutGameAssetRenderTransform transform =
                WorkoutGameAssetPhysicsSampler::renderTransform(snapshot, 0);
        QCOMPARE(transform.status, WorkoutGameAssetRenderFitStatus::Ready);
        QCOMPARE(transform.assetId,
                 QStringLiteral("FT-02-log-over-greybox"));
        QCOMPARE(transform.obstacleAnchorMeters, 10.0);
        QCOMPARE(transform.forwardScale, 700.0 / 540.0);
        QCOMPARE(transform.upScale, 700.0 / 540.0);
        QCOMPARE(transform.forwardExtentMeters, 0.7);
        QCOMPARE(transform.upExtentMeters, 0.7);
        QCOMPARE(transform.assetStartDistanceMeters,
                 10.0 - 1.02 * transform.forwardScale);

        snapshot.bindings[0].nativeUpExtentMm = 0;
        QCOMPARE(WorkoutGameAssetPhysicsSampler::renderTransform(snapshot, 0)
                    .status,
                 WorkoutGameAssetRenderFitStatus::Invalid);
    }

    void rejectsRenderMetadataThatDoesNotDescribeThePackagedFt02Mesh()
    {
        auto snapshot = triangleSnapshot();
        snapshot.bindings[0].nativeForwardOriginMm = -1020;

        for (const auto &corrupt : {
                 std::pair<std::int32_t, std::uint32_t>{-900, 540},
                 std::pair<std::int32_t, std::uint32_t>{-1020, 350}}) {
            snapshot.bindings[0].nativeForwardOriginMm = corrupt.first;
            snapshot.bindings[0].nativeUpExtentMm = corrupt.second;
            QCOMPARE(WorkoutGameAssetPhysicsSampler::renderTransform(
                         snapshot, 0).status,
                     WorkoutGameAssetRenderFitStatus::Invalid);
        }
    }

    void handlesUnboundLegacyAndInvalidIndicesWithoutDereferencing()
    {
        auto snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].definitionIndex =
                WorkoutGameCourseAssetPhysicsSnapshot::NoIndex;
        auto sample = WorkoutGameAssetPhysicsSampler::sample(
                snapshot, 0, 10.0);
        QVERIFY(!sample.bound);
        QVERIFY(WorkoutGameAssetPhysicsSampler::breakpointsMeters(
                    snapshot, 0).empty());

        snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].flags =
                WorkoutGameCourseAssetPhysicsSnapshot::LegacyProceduralV1;
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 0, 10.0);
        QVERIFY(!sample.bound);

        snapshot = triangleSnapshot();
        snapshot.pieceBindings[0].definitionIndex = 99;
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 0, 10.0);
        QVERIFY(!sample.bound);
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 99, 10.0);
        QVERIFY(!sample.bound);

        snapshot = legacyFt02Snapshot();
        snapshot.legacyFt02Records[0].heightMeters = 17.0;
        sample = WorkoutGameAssetPhysicsSampler::sample(snapshot, 0, 10.0);
        QVERIFY(!sample.bound);
        QCOMPARE(WorkoutGameAssetPhysicsSampler::renderTransform(
                     snapshot, 0).status,
                 WorkoutGameAssetRenderFitStatus::Unbound);
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameAssetPhysicsSampler)

#include "testWorkoutGameAssetPhysicsSampler.moc"
