#include "WorkoutGameDevelopmentAssets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <limits>
#include <memory>

namespace {

quint32 readLittleEndian32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

void appendLittleEndian32(QByteArray &bytes, quint32 value)
{
    char encoded[4];
    qToLittleEndian<quint32>(
            value, reinterpret_cast<uchar *>(encoded));
    bytes.append(encoded, 4);
}

QJsonObject readJson(const QString &path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(input.readAll()).object();
}

QJsonObject readGlbJson(const QString &path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return QJsonObject();
    const QByteArray bytes = input.readAll();
    if (bytes.size() < 20) return QJsonObject();
    const quint32 length = readLittleEndian32(bytes, 12);
    return QJsonDocument::fromJson(bytes.mid(20, length).trimmed()).object();
}

bool writeJsonAtomically(const QString &path, const QJsonObject &object)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return output.write(bytes) == bytes.size() && output.commit();
}

qsizetype writeGlbJsonAtomically(
        const QString &path,
        const QJsonObject &document)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return -1;
    const QByteArray original = input.readAll();
    input.close();
    if (original.size() < 20) return -1;
    const quint32 originalJsonLength = readLittleEndian32(original, 12);
    const qsizetype tailOffset = 20 + originalJsonLength;
    if (tailOffset > original.size()) return -1;

    QByteArray json = QJsonDocument(document).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0) json.append(' ');
    const QByteArray tail = original.mid(tailOffset);
    const qsizetype total = 20 + json.size() + tail.size();
    if (total > std::numeric_limits<quint32>::max()) return -1;
    QByteArray output;
    appendLittleEndian32(output, 0x46546c67);
    appendLittleEndian32(output, 2);
    appendLittleEndian32(output, static_cast<quint32>(total));
    appendLittleEndian32(output, static_cast<quint32>(json.size()));
    appendLittleEndian32(output, 0x4e4f534a);
    output.append(json);
    output.append(tail);

    QSaveFile destination(path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)
            || destination.write(output) != output.size()
            || !destination.commit()) {
        return -1;
    }
    return output.size();
}

double srgbToLinear(int channel)
{
    const double value = channel / 255.0;
    return value <= 0.04045
            ? value / 12.92
            : std::pow((value + 0.055) / 1.055, 2.4);
}

class Fixture
{
public:
    Fixture()
    {
        root.reset(new QTemporaryDir());
        QVERIFY(root->isValid());
        QDir directory(root->path());
        QVERIFY(directory.mkpath(QStringLiteral(
                "contrib/workout-game-assets/manifests")));
        QVERIFY(directory.mkpath(QStringLiteral(
                "contrib/workout-game-assets/generated")));
    }

    void addAsset(const QString &manifestName)
    {
        const QString repository = QFileInfo(
                QFINDTESTDATA("../../../COPYING")).absolutePath();
        QVERIFY(!repository.isEmpty());
        const QString sourceManifest = QDir(repository).filePath(
                QStringLiteral("contrib/workout-game-assets/manifests/")
                + manifestName);
        QVERIFY2(!sourceManifest.isEmpty(), qPrintable(manifestName));
        QJsonObject manifest = readJson(sourceManifest);
        QVERIFY(!manifest.isEmpty());
        QString glbRelative;
        for (const QJsonValue &value : manifest.value(
                QStringLiteral("files")).toArray()) {
            const QString path = value.toObject().value(
                    QStringLiteral("path")).toString();
            if (path.endsWith(QStringLiteral(".glb"))) glbRelative = path;
        }
        QVERIFY(!glbRelative.isEmpty());
        const QString sourceGlb = QDir(repository).filePath(glbRelative);
        QVERIFY2(!sourceGlb.isEmpty(), qPrintable(glbRelative));
        const QString targetGlb = QDir(root->path()).filePath(glbRelative);
        QVERIFY(QDir().mkpath(QFileInfo(targetGlb).absolutePath()));
        QVERIFY(QFile::copy(sourceGlb, targetGlb));
        const QString targetManifest = manifestPath(manifestName);
        QVERIFY(writeJsonAtomically(targetManifest, manifest));
    }

    QString manifestPath(const QString &name) const
    {
        return QDir(root->path()).filePath(
                QStringLiteral("contrib/workout-game-assets/manifests/") + name);
    }

    QString glbPath(const QString &name) const
    {
        return QDir(root->path()).filePath(
                QStringLiteral("contrib/workout-game-assets/generated/") + name);
    }

    std::unique_ptr<QTemporaryDir> root;
};

} // namespace

class TestWorkoutGameDevelopmentAssets : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        qunsetenv("GC_WORKOUT_GAME_ASSET_WORKSPACE");
    }

    void cleanup()
    {
        qunsetenv("GC_WORKOUT_GAME_ASSET_WORKSPACE");
    }

    void disabledWithoutExplicitWorkspace()
    {
        WorkoutGameDevelopmentAssets assets;
        QVERIFY(!assets.enabled());
        QCOMPARE(assets.revision(), qulonglong(0));
        QVERIFY(assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(),
                assets.revision()).isEmpty());
    }

    void materializesCanonicalGlbAndRuntimeVariant()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        fixture.addAsset(QStringLiteral("EN-08-forest-floor-props.json"));
        const QString logManifest = fixture.manifestPath(
                QStringLiteral("FT-02-log-over-greybox.json"));
        QJsonObject manifest = readJson(logManifest);
        manifest.insert(QStringLiteral("materialOverrides"), QJsonArray({
            QJsonObject({
                {QStringLiteral("materialName"),
                    QStringLiteral("MAT_LogOverBark_Grey")},
                {QStringLiteral("baseColorSrgb"), QStringLiteral("#123456")},
                {QStringLiteral("roughness"), 0.37},
                {QStringLiteral("metallic"), 0.14},
            }),
        }));
        QVERIFY(writeJsonAtomically(logManifest, manifest));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());

        WorkoutGameDevelopmentAssets assets;
        QVERIFY(assets.enabled());
        QSignalSpy changed(&assets,
                &WorkoutGameDevelopmentAssets::assetsChanged);
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(assets.lastError(), QString());

        const QUrl logUrl = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(),
                assets.revision());
        QVERIFY(logUrl.isLocalFile());
        QVERIFY(QFileInfo::exists(logUrl.toLocalFile()));
        const QJsonObject logGlb = readGlbJson(logUrl.toLocalFile());
        QVERIFY(!logGlb.isEmpty());
        QJsonObject bark;
        for (const QJsonValue &value : logGlb.value(
                QStringLiteral("materials")).toArray()) {
            const QJsonObject material = value.toObject();
            if (material.value(QStringLiteral("name")).toString()
                    == QStringLiteral("MAT_LogOverBark_Grey")) {
                bark = material;
            }
        }
        QVERIFY(!bark.isEmpty());
        const QJsonObject pbr = bark.value(
                QStringLiteral("pbrMetallicRoughness")).toObject();
        const QJsonArray color = pbr.value(
                QStringLiteral("baseColorFactor")).toArray();
        QCOMPARE(color.size(), 4);
        QVERIFY(std::abs(color.at(0).toDouble() - srgbToLinear(0x12)) < 1.0e-9);
        QVERIFY(std::abs(color.at(1).toDouble() - srgbToLinear(0x34)) < 1.0e-9);
        QVERIFY(std::abs(color.at(2).toDouble() - srgbToLinear(0x56)) < 1.0e-9);
        QCOMPARE(pbr.value(QStringLiteral("roughnessFactor")).toDouble(), 0.37);
        QCOMPARE(pbr.value(QStringLiteral("metallicFactor")).toDouble(), 0.14);

        const QVariantMap properties = assets.materialProperties(
                QStringLiteral("FT-02-log-over-greybox"),
                QStringLiteral("MAT_LogOverBark_Grey"),
                QStringLiteral("#000000"),
                1.0,
                0.0,
                assets.revision());
        QCOMPARE(properties.value(QStringLiteral("baseColor")).toString(),
                QStringLiteral("#123456"));

        const QUrl variantUrl = assets.sourceForAsset(
                QStringLiteral("EN-08-forest-floor-props"),
                QStringLiteral("0"),
                assets.revision());
        QVERIFY(variantUrl.isLocalFile());
        const QJsonObject variant = readGlbJson(variantUrl.toLocalFile());
        const QJsonArray nodes = variant.value(QStringLiteral("nodes")).toArray();
        const int sceneIndex = variant.value(QStringLiteral("scene")).toInt();
        const QJsonArray roots = variant.value(QStringLiteral("scenes"))
                .toArray().at(sceneIndex).toObject().value(
                    QStringLiteral("nodes")).toArray();
        QCOMPARE(roots.size(), 1);
        QCOMPARE(nodes.at(roots.at(0).toInt()).toObject().value(
                QStringLiteral("name")).toString(),
                QStringLiteral("GEO_GraniteLow_LOD0"));
    }

    void atomicManifestSaveReloadsAndInvalidGlbKeepsLastGoodAsset()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QUrl original = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(),
                assets.revision());
        QVERIFY(original.isLocalFile());

        const QString manifestPath = fixture.manifestPath(
                QStringLiteral("FT-02-log-over-greybox.json"));
        QJsonObject manifest = readJson(manifestPath);
        manifest.insert(QStringLiteral("materialOverrides"), QJsonArray({
            QJsonObject({
                {QStringLiteral("materialName"),
                    QStringLiteral("MAT_LogOverBark_Grey")},
                {QStringLiteral("baseColorSrgb"), QStringLiteral("#abcdef")},
                {QStringLiteral("roughness"), 0.61},
                {QStringLiteral("metallic"), 0.03},
            }),
        }));
        QVERIFY(writeJsonAtomically(manifestPath, manifest));
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(2), 5000);
        const QUrl updated = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(),
                assets.revision());
        QVERIFY(updated.isLocalFile());
        QVERIFY(updated != original);
        QCOMPARE(assets.materialProperties(
                    QStringLiteral("FT-02-log-over-greybox"),
                    QStringLiteral("MAT_LogOverBark_Grey"),
                    QStringLiteral("#000000"), 1.0, 0.0,
                    assets.revision()).value(
                        QStringLiteral("baseColor")).toString(),
                QStringLiteral("#abcdef"));

        QFile glb(fixture.glbPath(QStringLiteral("WG_LogOver_Greybox.glb")));
        QVERIFY(glb.open(QIODevice::Append));
        QCOMPARE(glb.write("x", 1), qint64(1));
        glb.close();
        QTRY_VERIFY_WITH_TIMEOUT(!assets.lastError().isEmpty(), 5000);
        QCOMPARE(assets.revision(), qulonglong(2));
        QCOMPARE(assets.sourceForAsset(
                    QStringLiteral("FT-02-log-over-greybox"),
                    QString(), assets.revision()),
                updated);
    }

    void runtimeRejectsExternalUrisAndKeepsLastGoodAsset()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QUrl original = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(), assets.revision());

        const QString glbPath = fixture.glbPath(
                QStringLiteral("WG_LogOver_Greybox.glb"));
        QJsonObject glb = readGlbJson(glbPath);
        QJsonArray buffers = glb.value(QStringLiteral("buffers")).toArray();
        QVERIFY(!buffers.isEmpty());
        QJsonObject buffer = buffers.first().toObject();
        buffer.insert(QStringLiteral("uri"),
                      QStringLiteral("data:application/octet-stream;base64,AA=="));
        buffers[0] = buffer;
        glb.insert(QStringLiteral("buffers"), buffers);
        const qsizetype newSize = writeGlbJsonAtomically(glbPath, glb);
        QVERIFY(newSize > 0);

        const QString manifestPath = fixture.manifestPath(
                QStringLiteral("FT-02-log-over-greybox.json"));
        QJsonObject manifest = readJson(manifestPath);
        QJsonObject technical = manifest.value(
                QStringLiteral("technical")).toObject();
        technical.insert(QStringLiteral("glbBytes"), newSize);
        manifest.insert(QStringLiteral("technical"), technical);
        QVERIFY(writeJsonAtomically(manifestPath, manifest));

        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("external buffer URI")),
                5000);
        QCOMPARE(assets.revision(), qulonglong(1));
        QCOMPARE(assets.sourceForAsset(
                    QStringLiteral("FT-02-log-over-greybox"),
                    QString(), assets.revision()),
                original);
    }

    void atomicallyReplacedManifestDirectoryIsWatchedAgain()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QUrl original = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(), assets.revision());

        const QString assetRoot = QDir(fixture.root->path()).filePath(
                QStringLiteral("contrib/workout-game-assets"));
        QDir directory(assetRoot);
        QVERIFY(directory.rename(QStringLiteral("manifests"),
                                 QStringLiteral("manifests-old")));
        QVERIFY(directory.mkdir(QStringLiteral("manifests")));
        const QString name = QStringLiteral("FT-02-log-over-greybox.json");
        QVERIFY(QFile::copy(
                directory.filePath(QStringLiteral("manifests-old/") + name),
                directory.filePath(QStringLiteral("manifests/") + name)));

        QTRY_VERIFY_WITH_TIMEOUT(assets.revision() > qulonglong(1), 5000);
        const QUrl reloaded = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(), assets.revision());
        QVERIFY(reloaded.isLocalFile());
        QVERIFY(reloaded != original);

        QJsonObject manifest = readJson(directory.filePath(
                QStringLiteral("manifests/") + name));
        manifest.insert(QStringLiteral("materialOverrides"), QJsonArray({
            QJsonObject({
                {QStringLiteral("materialName"),
                    QStringLiteral("MAT_LogOverBark_Grey")},
                {QStringLiteral("baseColorSrgb"), QStringLiteral("#234567")},
                {QStringLiteral("roughness"), 0.45},
                {QStringLiteral("metallic"), 0.12},
            }),
        }));
        const qulonglong beforeEdit = assets.revision();
        QVERIFY(writeJsonAtomically(
                directory.filePath(QStringLiteral("manifests/") + name),
                manifest));
        QTRY_VERIFY_WITH_TIMEOUT(assets.revision() > beforeEdit, 5000);
        QCOMPARE(assets.materialProperties(
                    QStringLiteral("FT-02-log-over-greybox"),
                    QStringLiteral("MAT_LogOverBark_Grey"),
                    QStringLiteral("#000000"), 1.0, 0.0,
                    assets.revision()).value(
                        QStringLiteral("baseColor")).toString(),
                QStringLiteral("#234567"));
    }

    void runtimeRejectsManifestOutsideSchemaAndPolicy()
    {
        Fixture fixture;
        const QString name = QStringLiteral("FT-02-log-over-greybox.json");
        fixture.addAsset(name);
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QUrl original = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"), QString(),
                assets.revision());
        const QString path = fixture.manifestPath(name);
        const QJsonObject valid = readJson(path);

        QJsonObject missing = valid;
        missing.remove(QStringLiteral("displayName"));
        QVERIFY(writeJsonAtomically(path, missing));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(QStringLiteral("displayName")), 5000);
        QCOMPARE(assets.revision(), qulonglong(1));
        QCOMPARE(assets.sourceForAsset(
                    QStringLiteral("FT-02-log-over-greybox"), QString(),
                    assets.revision()), original);

        QVERIFY(writeJsonAtomically(path, valid));
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(2), 5000);
        QJsonObject unknown = valid;
        unknown.insert(QStringLiteral("unexpectedRuntimeField"), true);
        QVERIFY(writeJsonAtomically(path, unknown));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("unknown manifest field")), 5000);
        QCOMPARE(assets.revision(), qulonglong(2));

        QJsonObject conditional = valid;
        QJsonObject license = conditional.value(
                QStringLiteral("license")).toObject();
        license.insert(QStringLiteral("decision"),
                       QStringLiteral("conditional"));
        license.insert(QStringLiteral("conditions"), QJsonArray());
        conditional.insert(QStringLiteral("license"), license);
        QVERIFY(writeJsonAtomically(path, conditional));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("license is not approved")), 5000);
        QCOMPARE(assets.revision(), qulonglong(2));
    }

    void runtimeRejectsMalformedOptionalManifestFields()
    {
        Fixture fixture;
        const QString name = QStringLiteral("FT-02-log-over-greybox.json");
        fixture.addAsset(name);
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QString path = fixture.manifestPath(name);
        const QJsonObject valid = readJson(path);

        QJsonObject malformedProcessing = valid;
        malformedProcessing.insert(QStringLiteral("processing"), QJsonObject({
            {QStringLiteral("tool"), true},
        }));
        QVERIFY(writeJsonAtomically(path, malformedProcessing));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("processing field type")), 5000);
        QCOMPARE(assets.revision(), qulonglong(1));

        QVERIFY(writeJsonAtomically(path, valid));
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(2), 5000);
        QJsonObject malformedReview = valid;
        QJsonObject review = malformedReview.value(
                QStringLiteral("review")).toObject();
        review.insert(QStringLiteral("reviewer"), 42);
        malformedReview.insert(QStringLiteral("review"), review);
        QVERIFY(writeJsonAtomically(path, malformedReview));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("review field type")), 5000);
        QCOMPARE(assets.revision(), qulonglong(2));
    }

    void runtimeRejectsWrongTypedGlbCollections()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);
        const QUrl original = assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"), QString(),
                assets.revision());

        const QString glbPath = fixture.glbPath(
                QStringLiteral("WG_LogOver_Greybox.glb"));
        QJsonObject glb = readGlbJson(glbPath);
        glb.insert(QStringLiteral("nodes"), QStringLiteral("not-an-array"));
        const qsizetype newSize = writeGlbJsonAtomically(glbPath, glb);
        QVERIFY(newSize > 0);
        const QString manifestPath = fixture.manifestPath(
                QStringLiteral("FT-02-log-over-greybox.json"));
        QJsonObject manifest = readJson(manifestPath);
        QJsonObject technical = manifest.value(
                QStringLiteral("technical")).toObject();
        technical.insert(QStringLiteral("glbBytes"), newSize);
        manifest.insert(QStringLiteral("technical"), technical);
        QVERIFY(writeJsonAtomically(manifestPath, manifest));

        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("invalid array field: nodes")), 5000);
        QCOMPARE(assets.revision(), qulonglong(1));
        QCOMPARE(assets.sourceForAsset(
                    QStringLiteral("FT-02-log-over-greybox"), QString(),
                    assets.revision()), original);
    }

    void runtimeRejectsUnsupportedGlbCollections_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("error");
        QTest::newRow("wrong-typed-textures")
                << QStringLiteral("textures")
                << QJsonValue(QJsonObject())
                << QStringLiteral("invalid array field: textures");
        QTest::newRow("non-empty-skins")
                << QStringLiteral("skins")
                << QJsonValue(QJsonArray({QJsonObject()}))
                << QStringLiteral("unsupported non-empty GLB collection: skins");
        QTest::newRow("non-empty-animations")
                << QStringLiteral("animations")
                << QJsonValue(QJsonArray({QJsonObject({
                    {QStringLiteral("name"), QStringLiteral("unsafe")},
                })}))
                << QStringLiteral("unsupported non-empty GLB collection: animations");
    }

    void runtimeRejectsUnsupportedGlbCollections()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, error);
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);

        const QString glbPath = fixture.glbPath(
                QStringLiteral("WG_LogOver_Greybox.glb"));
        QJsonObject glb = readGlbJson(glbPath);
        glb.insert(field, value);
        const qsizetype newSize = writeGlbJsonAtomically(glbPath, glb);
        QVERIFY(newSize > 0);
        const QString manifestPath = fixture.manifestPath(
                QStringLiteral("FT-02-log-over-greybox.json"));
        QJsonObject manifest = readJson(manifestPath);
        QJsonObject technical = manifest.value(
                QStringLiteral("technical")).toObject();
        technical.insert(QStringLiteral("glbBytes"), newSize);
        manifest.insert(QStringLiteral("technical"), technical);
        QVERIFY(writeJsonAtomically(manifestPath, manifest));

        QTRY_VERIFY_WITH_TIMEOUT(assets.lastError().contains(error), 5000);
        QCOMPARE(assets.revision(), qulonglong(1));
    }

    void runtimeRejectsVariantExpansionBeyondSchema()
    {
        Fixture fixture;
        const QString name = QStringLiteral("EN-08-forest-floor-props.json");
        fixture.addAsset(name);
        const QString path = fixture.manifestPath(name);
        QJsonObject manifest = readJson(path);
        QJsonArray variants;
        for (int index = 0; index < 33; ++index) {
            variants.append(QJsonObject({
                {QStringLiteral("key"), QString::number(index)},
                {QStringLiteral("rootNodes"), QJsonArray({
                    QStringLiteral("GEO_GraniteLow_LOD0")})},
            }));
        }
        manifest.insert(QStringLiteral("runtimeVariants"), variants);
        QVERIFY(writeJsonAtomically(path, manifest));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());

        WorkoutGameDevelopmentAssets assets;
        QTRY_VERIFY_WITH_TIMEOUT(
                assets.lastError().contains(
                    QStringLiteral("too many runtime variants")), 5000);
        QCOMPARE(assets.revision(), qulonglong(0));
    }

    void atomicallyReplacedGeneratedDirectoryIsWatchedAgain()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        WorkoutGameDevelopmentAssets assets;
        QTRY_COMPARE_WITH_TIMEOUT(assets.revision(), qulonglong(1), 5000);

        const QString assetRoot = QDir(fixture.root->path()).filePath(
                QStringLiteral("contrib/workout-game-assets"));
        QDir directory(assetRoot);
        QVERIFY(directory.rename(QStringLiteral("generated"),
                                 QStringLiteral("generated-old")));
        QVERIFY(directory.mkdir(QStringLiteral("generated")));
        const QString glbName = QStringLiteral("WG_LogOver_Greybox.glb");
        QVERIFY(QFile::copy(
                directory.filePath(QStringLiteral("generated-old/") + glbName),
                directory.filePath(QStringLiteral("generated/") + glbName)));
        QTRY_VERIFY_WITH_TIMEOUT(assets.revision() > qulonglong(1), 5000);

        QFile glb(directory.filePath(QStringLiteral("generated/") + glbName));
        QVERIFY(glb.open(QIODevice::Append));
        QCOMPARE(glb.write("x", 1), qint64(1));
        glb.close();
        QTRY_VERIFY_WITH_TIMEOUT(!assets.lastError().isEmpty(), 5000);
    }

    void replacedWorkspaceIdentityIsRejected()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        const QString originalRoot = fixture.root->path();
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                originalRoot.toLocal8Bit());
        auto assets = std::make_unique<WorkoutGameDevelopmentAssets>();
        QTRY_COMPARE_WITH_TIMEOUT(assets->revision(), qulonglong(1), 5000);

        const QString movedRoot = originalRoot + QStringLiteral("-moved");
        QVERIFY(QDir().rename(originalRoot, movedRoot));
        QVERIFY(QDir().mkpath(QDir(originalRoot).filePath(
                QStringLiteral("contrib/workout-game-assets/manifests"))));
        QVERIFY(QDir().mkpath(QDir(originalRoot).filePath(
                QStringLiteral("contrib/workout-game-assets/generated"))));
        QVERIFY(QFile::copy(
                QDir(movedRoot).filePath(QStringLiteral(
                    "contrib/workout-game-assets/manifests/FT-02-log-over-greybox.json")),
                QDir(originalRoot).filePath(QStringLiteral(
                    "contrib/workout-game-assets/manifests/FT-02-log-over-greybox.json"))));
        QVERIFY(QFile::copy(
                QDir(movedRoot).filePath(QStringLiteral(
                    "contrib/workout-game-assets/generated/WG_LogOver_Greybox.glb")),
                QDir(originalRoot).filePath(QStringLiteral(
                    "contrib/workout-game-assets/generated/WG_LogOver_Greybox.glb"))));
        QVERIFY(QMetaObject::invokeMethod(
                assets.get(), "startReload", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(
                assets->lastError().contains(
                    QStringLiteral("workspace changed")), 5000);
        QCOMPARE(assets->revision(), qulonglong(1));
        assets.reset();
        QVERIFY(QDir(movedRoot).removeRecursively());
    }

    void destructionWaitsForAnActiveBuild()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        fixture.addAsset(QStringLiteral("EN-08-forest-floor-props.json"));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());
        auto assets = std::make_unique<WorkoutGameDevelopmentAssets>();
        QVERIFY(QMetaObject::invokeMethod(
                assets.get(), "startReload", Qt::DirectConnection));
        assets.reset();
        QVERIFY(true);
    }

#ifndef Q_OS_WIN
    void symlinkedCanonicalGlbIsRejected()
    {
        Fixture fixture;
        fixture.addAsset(QStringLiteral("FT-02-log-over-greybox.json"));
        const QString canonical = fixture.glbPath(
                QStringLiteral("WG_LogOver_Greybox.glb"));
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString external = QDir(outside.path()).filePath(
                QStringLiteral("outside.glb"));
        QVERIFY(QFile::copy(canonical, external));
        QVERIFY(QFile::remove(canonical));
        QVERIFY(QFile::link(external, canonical));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE",
                fixture.root->path().toLocal8Bit());

        WorkoutGameDevelopmentAssets assets;
        QVERIFY(assets.enabled());
        QTRY_VERIFY_WITH_TIMEOUT(!assets.lastError().isEmpty(), 5000);
        QCOMPARE(assets.revision(), qulonglong(0));
        QVERIFY(assets.sourceForAsset(
                QStringLiteral("FT-02-log-over-greybox"),
                QString(), assets.revision()).isEmpty());
    }

    void symlinkedWorkspaceIsRejected()
    {
        Fixture fixture;
        QTemporaryDir aliases;
        QVERIFY(aliases.isValid());
        const QString alias = QDir(aliases.path()).filePath(
                QStringLiteral("workspace-link"));
        QVERIFY(QFile::link(fixture.root->path(), alias));
        qputenv("GC_WORKOUT_GAME_ASSET_WORKSPACE", alias.toLocal8Bit());

        WorkoutGameDevelopmentAssets assets;
        QVERIFY(!assets.enabled());
        QVERIFY(assets.lastError().contains(QStringLiteral("invalid")));
    }
#endif
};

QTEST_GUILESS_MAIN(TestWorkoutGameDevelopmentAssets)

#include "testWorkoutGameDevelopmentAssets.moc"
