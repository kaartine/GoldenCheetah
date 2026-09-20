/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameAssetCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace {

QJsonObject surface(int friction = 1100, int restitution = 0)
{
    return {
        {QStringLiteral("coulombFrictionMilli"), friction},
        {QStringLiteral("restitutionMilli"), restitution},
    };
}

QJsonObject binding(const QString &profileId = QStringLiteral("profile-1"),
                    const QString &routeKey = QStringLiteral("main"))
{
    return {
        {QStringLiteral("nativeForwardExtentMm"), 540},
        {QStringLiteral("nativeForwardOriginMm"), -270},
        {QStringLiteral("nativeUpExtentMm"), 540},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("routeKey"), routeKey},
        {QStringLiteral("variantKey"), QString()},
    };
}

QJsonObject profile(const QString &profileId = QStringLiteral("profile-1"))
{
    return {
        {QStringLiteral("chains"), QJsonArray({
            QJsonObject({
                {QStringLiteral("points"), QJsonArray({
                    QJsonObject({
                        {QStringLiteral("forwardMm"), -270},
                        {QStringLiteral("heightMm"), 0},
                    }),
                    QJsonObject({
                        {QStringLiteral("forwardMm"), 270},
                        {QStringLiteral("heightMm"), 540},
                    }),
                })},
            }),
        })},
        {QStringLiteral("difficultyScale"), QJsonObject({
            {QStringLiteral("baseExtentMm"), 440},
            {QStringLiteral("difficultyExtentMm"), 100},
            {QStringLiteral("nativeExtentMm"), 540},
        })},
        {QStringLiteral("kind"), QStringLiteral("height-offset-polyline")},
        {QStringLiteral("operation"), QStringLiteral("add-obstacle")},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("profileVersion"), 1},
        {QStringLiteral("surface"), surface()},
    };
}

QJsonObject resource(const QString &path = QStringLiteral(
                             "src/Train/qml/TestAsset.qml"),
                     const QString &url = QStringLiteral(
                             "qrc:/qml/TestAsset.qml"))
{
    return {
        {QStringLiteral("bytes"), 1},
        {QStringLiteral("purpose"), QStringLiteral("runtime")},
        {QStringLiteral("repositoryPath"), path},
        {QStringLiteral("url"), url},
    };
}

QJsonObject asset(const QString &assetId = QStringLiteral("AA-00-test"),
                  const QString &profileId = QStringLiteral("profile-1"))
{
    return {
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("physics"), QJsonObject({
            {QStringLiteral("authority"), QStringLiteral("external")},
            {QStringLiteral("interaction"),
                QStringLiteral("rideable-feature")},
            {QStringLiteral("routeProfiles"), QJsonArray({
                binding(profileId),
            })},
            {QStringLiteral("surface"), surface()},
        })},
        {QStringLiteral("resources"), QJsonArray({resource()})},
        {QStringLiteral("role"), QStringLiteral("feature")},
    };
}

QJsonObject validRoot()
{
    return {
        {QStringLiteral("assets"), QJsonArray({asset()})},
        {QStringLiteral("generatorVersion"), 4},
        {QStringLiteral("profiles"), QJsonArray({profile()})},
        {QStringLiteral("schemaVersion"), 1},
    };
}

QByteArray encode(const QJsonObject &root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

bool rejected(const QJsonObject &root)
{
    QString error;
    const auto catalog = WorkoutGameAssetCatalog::fromJson(encode(root), &error);
    return !catalog && !error.isEmpty();
}

QJsonObject firstAsset(QJsonObject root)
{
    return root.value(QStringLiteral("assets")).toArray().at(0).toObject();
}

QJsonObject firstProfile(QJsonObject root)
{
    return root.value(QStringLiteral("profiles")).toArray().at(0).toObject();
}

void replaceFirstAsset(QJsonObject &root, const QJsonObject &replacement)
{
    QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    assets[0] = replacement;
    root.insert(QStringLiteral("assets"), assets);
}

void replaceFirstProfile(QJsonObject &root, const QJsonObject &replacement)
{
    QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    profiles[0] = replacement;
    root.insert(QStringLiteral("profiles"), profiles);
}

} // namespace

class TestWorkoutGameAssetCatalog : public QObject
{
    Q_OBJECT

private slots:
    void loadsPackagedCatalog()
    {
        QString error;
        const auto catalog = WorkoutGameAssetCatalog::load(&error);
        QVERIFY2(catalog, qPrintable(error));
        QCOMPARE(catalog->schemaVersion(), quint32(1));
        QCOMPARE(catalog->generatorVersion(), quint32(4));
        QVERIFY(!catalog->assets().isEmpty());

        const auto *asset = catalog->findAsset(
                QStringLiteral("FT-02-log-over-greybox"));
        QVERIFY(asset);
        QCOMPARE(asset->physics.routeProfiles.size(), 1);
        const auto &fit = asset->physics.routeProfiles.constFirst();
        QCOMPARE(fit.variantKey, QString());
        QCOMPARE(fit.nativeForwardOriginMm, qint32(-1020));
        QCOMPARE(fit.nativeForwardExtentMm, quint32(540));
        QCOMPARE(fit.nativeUpExtentMm, quint32(540));

        const auto *profile = catalog->findProfile(
                QStringLiteral("FT-02-log-over-v1"));
        QVERIFY(profile);
        QCOMPARE(profile->operation,
                 WorkoutGameAssetCatalog::ProfileOperation::AddObstacle);
        QCOMPARE(profile->chains.constFirst().points.size(), 13);
        QCOMPARE(profile->chains.constFirst().points.constFirst().forwardMm,
                 qint32(-276));
        QCOMPARE(profile->chains.constFirst().points.constLast().forwardMm,
                 qint32(276));
        QVERIFY(!catalog->findAsset(QStringLiteral("ZZ-99-missing")));
        QVERIFY(!catalog->findProfile(QStringLiteral("missing")));
    }

    void decodesTypedCatalogAllOrNothing()
    {
        QString error = QStringLiteral("stale");
        const auto catalog = WorkoutGameAssetCatalog::fromJson(
                encode(validRoot()), &error);
        QVERIFY2(catalog, qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(catalog->assets().size(), 1);
        QCOMPARE(catalog->profiles().size(), 1);
        QCOMPARE(catalog->assets().constFirst().physics.interaction,
                 WorkoutGameAssetCatalog::Interaction::RideableFeature);
        QCOMPARE(catalog->profiles().constFirst().surface.coulombFrictionMilli,
                 quint16(1100));
    }

    void rejectsMalformedDocumentAndByteBounds()
    {
        QString error;
        QVERIFY(!WorkoutGameAssetCatalog::fromJson(QByteArray(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!WorkoutGameAssetCatalog::fromJson("[]", &error));
        QVERIFY(!WorkoutGameAssetCatalog::fromJson("{", &error));
        QVERIFY(!WorkoutGameAssetCatalog::fromJson(
                QByteArray(WorkoutGameAssetCatalog::MaximumCatalogBytes + 1,
                           ' '), &error));

        QJsonObject root = validRoot();
        root.insert(QStringLiteral("assets"), QStringLiteral("wrong"));
        QVERIFY(rejected(root));
        root = validRoot();
        root.insert(QStringLiteral("assets"), QJsonArray());
        QVERIFY(rejected(root));
    }

    void rejectsUnknownFieldsLegacyHashesAndVersions()
    {
        QJsonObject root = validRoot();
        root.insert(QStringLiteral("catalogSha256"), QString(64, QLatin1Char('a')));
        QVERIFY(rejected(root));

        root = validRoot();
        QJsonObject first = firstAsset(root);
        QJsonArray resources = first.value(QStringLiteral("resources")).toArray();
        QJsonObject firstResource = resources.at(0).toObject();
        firstResource.insert(QStringLiteral("sha256"),
                             QString(64, QLatin1Char('a')));
        resources[0] = firstResource;
        first.insert(QStringLiteral("resources"), resources);
        replaceFirstAsset(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        QJsonObject firstPoint = firstProfile(root);
        firstPoint.insert(QStringLiteral("unexpected"), true);
        replaceFirstProfile(root, firstPoint);
        QVERIFY(rejected(root));

        root = validRoot();
        root.insert(QStringLiteral("schemaVersion"), 2);
        QVERIFY(rejected(root));
        root = validRoot();
        root.insert(QStringLiteral("generatorVersion"), 3);
        QVERIFY(rejected(root));
        root.insert(QStringLiteral("generatorVersion"), 5);
        QVERIFY(rejected(root));
    }

    void rejectsDuplicateOrUnsortedCatalogEntries()
    {
        QJsonObject root = validRoot();
        root.insert(QStringLiteral("assets"), QJsonArray({asset(), asset()}));
        QVERIFY(rejected(root));

        root = validRoot();
        root.insert(QStringLiteral("assets"), QJsonArray({
            asset(QStringLiteral("BB-00-test")),
            asset(QStringLiteral("AA-00-test")),
        }));
        QVERIFY(rejected(root));

        root = validRoot();
        root.insert(QStringLiteral("profiles"), QJsonArray({profile(), profile()}));
        QVERIFY(rejected(root));

        root = validRoot();
        root.insert(QStringLiteral("profiles"), QJsonArray({
            profile(QStringLiteral("profile-2")),
            profile(QStringLiteral("profile-1")),
        }));
        QVERIFY(rejected(root));

        root = validRoot();
        QJsonObject first = firstAsset(root);
        first.insert(QStringLiteral("resources"),
                     QJsonArray({resource(), resource()}));
        replaceFirstAsset(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        first = firstAsset(root);
        first.insert(QStringLiteral("resources"), QJsonArray({
            resource(QStringLiteral("src/Train/qml/B.qml"),
                     QStringLiteral("qrc:/qml/B.qml")),
            resource(QStringLiteral("src/Train/qml/A.qml"),
                     QStringLiteral("qrc:/qml/A.qml")),
        }));
        replaceFirstAsset(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        root.insert(QStringLiteral("profiles"), QJsonArray({
            profile(QStringLiteral("profile-1")),
            profile(QStringLiteral("profile-2")),
        }));
        first = firstAsset(root);
        QJsonObject physics = first.value(QStringLiteral("physics")).toObject();
        physics.insert(QStringLiteral("routeProfiles"), QJsonArray({
            binding(QStringLiteral("profile-2"), QStringLiteral("second")),
            binding(QStringLiteral("profile-1"), QStringLiteral("first")),
        }));
        first.insert(QStringLiteral("physics"), physics);
        replaceFirstAsset(root, first);
        QVERIFY(rejected(root));
    }

    void rejectsCollectionCountBounds()
    {
        QJsonObject root = validRoot();
        QJsonArray assets;
        for (int index = 0; index <= WorkoutGameAssetCatalog::MaximumAssets;
             ++index) {
            assets.append(asset(QStringLiteral("AA-00-%1").arg(
                    index, 3, 10, QLatin1Char('0'))));
        }
        root.insert(QStringLiteral("assets"), assets);
        QVERIFY(rejected(root));

        root = validRoot();
        QJsonArray profiles;
        for (int index = 0; index <= WorkoutGameAssetCatalog::MaximumProfiles;
             ++index) {
            profiles.append(profile(QStringLiteral("profile-%1").arg(
                    index, 3, 10, QLatin1Char('0'))));
        }
        root.insert(QStringLiteral("profiles"), profiles);
        QVERIFY(rejected(root));
    }

    void rejectsMalformedResourceFieldsAndIdentityConflicts()
    {
        const auto withResource = [](QJsonObject replacement) {
            QJsonObject root = validRoot();
            QJsonObject first = firstAsset(root);
            first.insert(QStringLiteral("resources"), QJsonArray({replacement}));
            replaceFirstAsset(root, first);
            return root;
        };

        QJsonObject changed = resource();
        changed.insert(QStringLiteral("bytes"), 0);
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("bytes"), 1.5);
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("purpose"), QStringLiteral("source"));
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("repositoryPath"),
                       QStringLiteral("src/Train/../secret"));
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("repositoryPath"),
                       QStringLiteral("src/Train/\u00e4.qml"));
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("url"),
                       QStringLiteral("file:/tmp/TestAsset.qml"));
        QVERIFY(rejected(withResource(changed)));
        changed = resource();
        changed.insert(QStringLiteral("url"),
                       QStringLiteral("qrc:/qml/../TestAsset.qml"));
        QVERIFY(rejected(withResource(changed)));

        QJsonObject root = validRoot();
        QJsonObject second = asset(QStringLiteral("AB-00-test"));
        QJsonObject secondResource = resource(
                QStringLiteral("src/Train/qml/Other.qml"),
                QStringLiteral("qrc:/qml/TestAsset.qml"));
        second.insert(QStringLiteral("resources"), QJsonArray({secondResource}));
        root.insert(QStringLiteral("assets"), QJsonArray({
            asset(QStringLiteral("AA-00-test")), second,
        }));
        QVERIFY(rejected(root));
    }

    void rejectsMissingProfileAndInvalidBindingFit()
    {
        QJsonObject root = validRoot();
        QJsonObject first = firstAsset(root);
        QJsonObject physics = first.value(QStringLiteral("physics")).toObject();
        QJsonObject fit = binding(QStringLiteral("missing"));
        physics.insert(QStringLiteral("routeProfiles"), QJsonArray({fit}));
        first.insert(QStringLiteral("physics"), physics);
        replaceFirstAsset(root, first);
        QVERIFY(rejected(root));

        const auto invalidFit = [](const QString &field, const QJsonValue &value) {
            QJsonObject root = validRoot();
            QJsonObject first = firstAsset(root);
            QJsonObject physics = first.value(QStringLiteral("physics")).toObject();
            QJsonObject fit = binding();
            fit.insert(field, value);
            physics.insert(QStringLiteral("routeProfiles"), QJsonArray({fit}));
            first.insert(QStringLiteral("physics"), physics);
            replaceFirstAsset(root, first);
            return root;
        };
        QVERIFY(rejected(invalidFit(QStringLiteral("variantKey"),
                                    QStringLiteral("bad key"))));
        QVERIFY(rejected(invalidFit(QStringLiteral("nativeForwardOriginMm"),
                                    -64001)));
        QVERIFY(rejected(invalidFit(QStringLiteral("nativeForwardExtentMm"), 0)));
        QVERIFY(rejected(invalidFit(QStringLiteral("nativeForwardExtentMm"),
                                    128001)));
        QVERIFY(rejected(invalidFit(QStringLiteral("nativeUpExtentMm"), 16001)));
    }

    void rejectsPointChainAndAggregateProfileBounds()
    {
        const auto withChains = [](const QJsonArray &chains) {
            QJsonObject root = validRoot();
            QJsonObject first = firstProfile(root);
            first.insert(QStringLiteral("chains"), chains);
            replaceFirstProfile(root, first);
            return root;
        };
        const auto chain = [](int first, int last) {
            return QJsonObject({
                {QStringLiteral("points"), QJsonArray({
                    QJsonObject({
                        {QStringLiteral("forwardMm"), first},
                        {QStringLiteral("heightMm"), 0},
                    }),
                    QJsonObject({
                        {QStringLiteral("forwardMm"), last},
                        {QStringLiteral("heightMm"), 0},
                    }),
                })},
            });
        };

        QVERIFY(rejected(withChains(QJsonArray({chain(0, 0)}))));
        QVERIFY(rejected(withChains(QJsonArray({chain(-64001, 0)}))));
        QJsonObject excessiveHeight = chain(0, 1);
        QJsonArray points = excessiveHeight.value(
                QStringLiteral("points")).toArray();
        QJsonObject point = points[1].toObject();
        point.insert(QStringLiteral("heightMm"), 16001);
        points[1] = point;
        excessiveHeight.insert(QStringLiteral("points"), points);
        QVERIFY(rejected(withChains(QJsonArray({excessiveHeight}))));
        QVERIFY(rejected(withChains(QJsonArray({chain(-2, 2), chain(1, 3)}))));

        QJsonArray tooManyChains;
        for (int index = 0; index < 9; ++index) {
            tooManyChains.append(chain(index * 3, index * 3 + 1));
        }
        QVERIFY(rejected(withChains(tooManyChains)));

        QJsonArray firstPoints;
        QJsonArray secondPoints;
        for (int index = -300; index < -171; ++index) {
            firstPoints.append(QJsonObject({
                {QStringLiteral("forwardMm"), index},
                {QStringLiteral("heightMm"), 0},
            }));
        }
        for (int index = 0; index < 129; ++index) {
            secondPoints.append(QJsonObject({
                {QStringLiteral("forwardMm"), index},
                {QStringLiteral("heightMm"), 0},
            }));
        }
        QVERIFY(rejected(withChains(QJsonArray({
            QJsonObject({{QStringLiteral("points"), firstPoints}}),
            QJsonObject({{QStringLiteral("points"), secondPoints}}),
        }))));
    }

    void rejectsInvalidDifficultyContactAndOperation()
    {
        QJsonObject root = validRoot();
        QJsonObject first = firstProfile(root);
        first.insert(QStringLiteral("operation"), QStringLiteral("teleport"));
        replaceFirstProfile(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        first = firstProfile(root);
        first.insert(QStringLiteral("operation"),
                     QStringLiteral("replace-surface"));
        replaceFirstProfile(root, first);
        QVERIFY(!rejected(root));

        root = validRoot();
        first = firstProfile(root);
        QJsonObject difficulty = first.value(
                QStringLiteral("difficultyScale")).toObject();
        difficulty.insert(QStringLiteral("baseExtentMm"), 10);
        difficulty.insert(QStringLiteral("difficultyExtentMm"), -10);
        first.insert(QStringLiteral("difficultyScale"), difficulty);
        replaceFirstProfile(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        first = firstProfile(root);
        first.insert(QStringLiteral("surface"), surface(2001, 0));
        replaceFirstProfile(root, first);
        QVERIFY(rejected(root));

        root = validRoot();
        QJsonObject firstAssetObject = firstAsset(root);
        QJsonObject physics = firstAssetObject.value(
                QStringLiteral("physics")).toObject();
        physics.insert(QStringLiteral("surface"), surface(1000, 251));
        firstAssetObject.insert(QStringLiteral("physics"), physics);
        replaceFirstAsset(root, firstAssetObject);
        QVERIFY(rejected(root));
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameAssetCatalog)

#include "testWorkoutGameAssetCatalog.moc"
