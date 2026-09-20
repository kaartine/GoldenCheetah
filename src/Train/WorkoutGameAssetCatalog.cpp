/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameAssetCatalog.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace {

constexpr quint32 CatalogSchemaVersion = 1;
constexpr quint32 CatalogGeneratorVersion = 4;
constexpr qsizetype MaximumResourcesPerAsset = 1024;
constexpr qsizetype MaximumTotalResources = 8192;
constexpr qint64 MaximumResourceBytes = 64 * 1024 * 1024;
constexpr qsizetype MaximumRouteProfilesPerAsset = 5;
constexpr qsizetype MaximumChainsPerProfile = 8;

class CatalogParser
{
public:
    bool fail(const QString &message)
    {
        if (m_error.isEmpty()) m_error = message;
        return false;
    }

    const QString &error() const { return m_error; }

    bool exactKeys(const QJsonObject &object,
                   std::initializer_list<const char *> required,
                   std::initializer_list<const char *> optional = {})
    {
        QSet<QString> allowed;
        for (const char *key : required) {
            const QString name = QString::fromLatin1(key);
            allowed.insert(name);
            if (!object.contains(name)) {
                return fail(QStringLiteral("missing field: ") + name);
            }
        }
        for (const char *key : optional) {
            allowed.insert(QString::fromLatin1(key));
        }
        for (auto iterator = object.constBegin();
             iterator != object.constEnd(); ++iterator) {
            if (!allowed.contains(iterator.key())) {
                return fail(QStringLiteral("unknown field: ") + iterator.key());
            }
        }
        return true;
    }

    bool integer(const QJsonValue &value, qint64 minimum, qint64 maximum,
                 qint64 &result, const QString &name)
    {
        if (!value.isDouble()) return fail(name + QStringLiteral(" is not an integer"));
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number
                || number < double(minimum) || number > double(maximum)) {
            return fail(name + QStringLiteral(" is outside its integer range"));
        }
        result = qint64(number);
        return true;
    }

    bool asciiString(const QJsonValue &value, qsizetype minimumLength,
                     qsizetype maximumLength, QString &result,
                     const QString &name)
    {
        if (!value.isString()) return fail(name + QStringLiteral(" is not a string"));
        result = value.toString();
        if (result.size() < minimumLength || result.size() > maximumLength) {
            return fail(name + QStringLiteral(" has an invalid length"));
        }
        for (const QChar character : result) {
            const ushort code = character.unicode();
            if (code < 0x20 || code > 0x7e) {
                return fail(name + QStringLiteral(" must contain printable ASCII"));
            }
        }
        return true;
    }

private:
    QString m_error;
};

bool matches(const QString &value, const char *pattern)
{
    return QRegularExpression(QString::fromLatin1(pattern))
            .match(value).hasMatch();
}

bool safePath(const QString &value, const QString &prefix)
{
    if (!value.startsWith(prefix) || value.endsWith(QLatin1Char('/'))
            || value.contains(QLatin1Char('\\'))
            || value.contains(QLatin1Char('?'))
            || value.contains(QLatin1Char('#'))) {
        return false;
    }
    const QStringList parts = value.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".")
                || part == QStringLiteral("..")) {
            return false;
        }
    }
    return true;
}

bool parseRole(CatalogParser &parser, const QJsonValue &value,
               WorkoutGameAssetCatalog::Role &result)
{
    QString text;
    if (!parser.asciiString(value, 1, 32, text, QStringLiteral("asset role"))) {
        return false;
    }
    using Role = WorkoutGameAssetCatalog::Role;
    if (text == QStringLiteral("rider-bike")) result = Role::RiderBike;
    else if (text == QStringLiteral("trail-tile")) result = Role::TrailTile;
    else if (text == QStringLiteral("feature")) result = Role::Feature;
    else if (text == QStringLiteral("terrain")) result = Role::Terrain;
    else if (text == QStringLiteral("vegetation")) result = Role::Vegetation;
    else if (text == QStringLiteral("prop")) result = Role::Prop;
    else if (text == QStringLiteral("effect")) result = Role::Effect;
    else if (text == QStringLiteral("texture")) result = Role::Texture;
    else if (text == QStringLiteral("reference")) result = Role::Reference;
    else return parser.fail(QStringLiteral("unknown asset role"));
    return true;
}

bool parsePurpose(CatalogParser &parser, const QJsonValue &value,
                  WorkoutGameAssetCatalog::ResourcePurpose &result,
                  QString &sortKey)
{
    if (!parser.asciiString(value, 1, 16, sortKey,
                            QStringLiteral("resource purpose"))) {
        return false;
    }
    using Purpose = WorkoutGameAssetCatalog::ResourcePurpose;
    if (sortKey == QStringLiteral("runtime")) result = Purpose::Runtime;
    else if (sortKey == QStringLiteral("texture")) result = Purpose::Texture;
    else return parser.fail(QStringLiteral("unknown resource purpose"));
    return true;
}

bool parseInteraction(CatalogParser &parser, const QJsonValue &value,
                      WorkoutGameAssetCatalog::Interaction &result)
{
    QString text;
    if (!parser.asciiString(value, 1, 32, text,
                            QStringLiteral("physics interaction"))) {
        return false;
    }
    using Interaction = WorkoutGameAssetCatalog::Interaction;
    if (text == QStringLiteral("visual-only")) result = Interaction::VisualOnly;
    else if (text == QStringLiteral("surface")) result = Interaction::Surface;
    else if (text == QStringLiteral("obstacle")) result = Interaction::Obstacle;
    else if (text == QStringLiteral("rideable-feature")) {
        result = Interaction::RideableFeature;
    } else {
        return parser.fail(QStringLiteral("unknown physics interaction"));
    }
    return true;
}

bool parseSurface(CatalogParser &parser, const QJsonValue &value,
                  WorkoutGameAssetCatalog::Surface &result)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("surface is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"coulombFrictionMilli", "restitutionMilli"})) {
        return false;
    }
    qint64 friction = 0;
    qint64 restitution = 0;
    if (!parser.integer(object.value(QStringLiteral("coulombFrictionMilli")),
                        0, 2000, friction, QStringLiteral("Coulomb friction"))
            || !parser.integer(object.value(QStringLiteral("restitutionMilli")),
                               0, 250, restitution,
                               QStringLiteral("restitution"))) {
        return false;
    }
    result.coulombFrictionMilli = quint16(friction);
    result.restitutionMilli = quint16(restitution);
    return true;
}

bool parseBinding(CatalogParser &parser, const QJsonValue &value,
                  WorkoutGameAssetCatalog::RouteProfileBinding &result)
{
    if (!value.isObject()) {
        return parser.fail(QStringLiteral("route profile binding is not an object"));
    }
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"nativeForwardExtentMm", "nativeForwardOriginMm",
             "nativeUpExtentMm", "profileId", "routeKey", "variantKey"})) {
        return false;
    }
    if (!parser.asciiString(object.value(QStringLiteral("profileId")), 1, 128,
                            result.profileId, QStringLiteral("profile ID"))
            || !matches(result.profileId, "^[A-Za-z0-9-]+$")) {
        return parser.fail(QStringLiteral("profile ID is malformed"));
    }
    if (!parser.asciiString(object.value(QStringLiteral("routeKey")), 1, 32,
                            result.routeKey, QStringLiteral("route key"))
            || !matches(result.routeKey, "^[a-z0-9-]+$")) {
        return parser.fail(QStringLiteral("route key is malformed"));
    }
    if (!parser.asciiString(object.value(QStringLiteral("variantKey")), 0, 32,
                            result.variantKey, QStringLiteral("variant key"))
            || !matches(result.variantKey, "^[A-Za-z0-9_-]*$")) {
        return parser.fail(QStringLiteral("variant key is malformed"));
    }
    qint64 forwardOrigin = 0;
    qint64 forwardExtent = 0;
    qint64 upExtent = 0;
    if (!parser.integer(object.value(QStringLiteral("nativeForwardOriginMm")),
                        -64000, 64000, forwardOrigin,
                        QStringLiteral("native forward origin"))
            || !parser.integer(
                    object.value(QStringLiteral("nativeForwardExtentMm")),
                    1, 128000, forwardExtent,
                    QStringLiteral("native forward extent"))
            || !parser.integer(object.value(QStringLiteral("nativeUpExtentMm")),
                               1, 16000, upExtent,
                               QStringLiteral("native up extent"))) {
        return false;
    }
    result.nativeForwardOriginMm = qint32(forwardOrigin);
    result.nativeForwardExtentMm = quint32(forwardExtent);
    result.nativeUpExtentMm = quint32(upExtent);
    return true;
}

bool parsePhysics(CatalogParser &parser, const QJsonValue &value,
                  WorkoutGameAssetCatalog::Physics &result)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("physics is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object, {"authority", "interaction"},
                          {"surface", "routeProfiles"})) {
        return false;
    }
    QString authority;
    if (!parser.asciiString(object.value(QStringLiteral("authority")), 1, 16,
                            authority, QStringLiteral("physics authority"))
            || authority != QStringLiteral("external")) {
        return parser.fail(QStringLiteral("unsupported physics authority"));
    }
    if (!parseInteraction(parser, object.value(QStringLiteral("interaction")),
                          result.interaction)) {
        return false;
    }
    if (object.contains(QStringLiteral("surface"))) {
        WorkoutGameAssetCatalog::Surface surface;
        if (!parseSurface(parser, object.value(QStringLiteral("surface")), surface)) {
            return false;
        }
        result.surface = surface;
    }
    if (object.contains(QStringLiteral("routeProfiles"))) {
        const QJsonValue bindingsValue = object.value(QStringLiteral("routeProfiles"));
        if (!bindingsValue.isArray()) {
            return parser.fail(QStringLiteral("routeProfiles is not an array"));
        }
        const QJsonArray bindings = bindingsValue.toArray();
        if (bindings.isEmpty() || bindings.size() > MaximumRouteProfilesPerAsset) {
            return parser.fail(QStringLiteral("routeProfiles count is invalid"));
        }
        QString previousRoute;
        QString previousProfile;
        QSet<QString> routeKeys;
        QSet<QString> profileIds;
        for (const QJsonValue &bindingValue : bindings) {
            WorkoutGameAssetCatalog::RouteProfileBinding binding;
            if (!parseBinding(parser, bindingValue, binding)) return false;
            if ((!previousRoute.isEmpty()
                    && std::tie(binding.routeKey, binding.profileId)
                            <= std::tie(previousRoute, previousProfile))
                    || routeKeys.contains(binding.routeKey)
                    || profileIds.contains(binding.profileId)) {
                return parser.fail(QStringLiteral(
                        "route profile bindings are unsorted or duplicated"));
            }
            previousRoute = binding.routeKey;
            previousProfile = binding.profileId;
            routeKeys.insert(binding.routeKey);
            profileIds.insert(binding.profileId);
            result.routeProfiles.push_back(std::move(binding));
        }
    }
    const bool visualOnly = result.interaction
            == WorkoutGameAssetCatalog::Interaction::VisualOnly;
    if (visualOnly && (result.surface.has_value()
            || !result.routeProfiles.isEmpty())) {
        return parser.fail(QStringLiteral("visual-only physics has contact data"));
    }
    if (!visualOnly && !result.surface.has_value()) {
        return parser.fail(QStringLiteral("interactive physics has no surface"));
    }
    return true;
}

bool parseResource(CatalogParser &parser, const QJsonValue &value,
                   WorkoutGameAssetCatalog::Resource &result,
                   QString &purposeSortKey)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("resource is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"bytes", "purpose", "repositoryPath", "url"})) {
        return false;
    }
    qint64 bytes = 0;
    if (!parser.integer(object.value(QStringLiteral("bytes")), 1,
                        MaximumResourceBytes, bytes,
                        QStringLiteral("resource bytes"))) {
        return false;
    }
    result.bytes = bytes;
    if (!parsePurpose(parser, object.value(QStringLiteral("purpose")),
                      result.purpose, purposeSortKey)) {
        return false;
    }
    if (!parser.asciiString(object.value(QStringLiteral("repositoryPath")),
                            1, 512, result.repositoryPath,
                            QStringLiteral("repository path"))
            || !safePath(result.repositoryPath, QStringLiteral("src/"))) {
        return parser.fail(QStringLiteral("repository path is malformed"));
    }
    if (!parser.asciiString(object.value(QStringLiteral("url")), 1, 512,
                            result.url, QStringLiteral("resource URL"))
            || !safePath(result.url, QStringLiteral("qrc:/"))) {
        return parser.fail(QStringLiteral("resource URL is malformed"));
    }
    return true;
}

bool parseAsset(CatalogParser &parser, const QJsonValue &value,
                WorkoutGameAssetCatalog::Asset &result,
                qsizetype &totalResources)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("asset is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"assetId", "physics", "resources", "role"})) {
        return false;
    }
    if (!parser.asciiString(object.value(QStringLiteral("assetId")), 1, 128,
                            result.assetId, QStringLiteral("asset ID"))
            || !matches(result.assetId, "^[A-Z]{2}-[0-9]{2}(-[a-z0-9-]+)?$")) {
        return parser.fail(QStringLiteral("asset ID is malformed"));
    }
    if (!parsePhysics(parser, object.value(QStringLiteral("physics")),
                      result.physics)
            || !parseRole(parser, object.value(QStringLiteral("role")),
                          result.role)) {
        return false;
    }
    const QJsonValue resourcesValue = object.value(QStringLiteral("resources"));
    if (!resourcesValue.isArray()) {
        return parser.fail(QStringLiteral("resources is not an array"));
    }
    const QJsonArray resources = resourcesValue.toArray();
    if (resources.isEmpty() || resources.size() > MaximumResourcesPerAsset
            || totalResources + resources.size() > MaximumTotalResources) {
        return parser.fail(QStringLiteral("resource count is invalid"));
    }
    QString previousPath;
    QString previousPurpose;
    for (const QJsonValue &resourceValue : resources) {
        WorkoutGameAssetCatalog::Resource resource;
        QString purpose;
        if (!parseResource(parser, resourceValue, resource, purpose)) return false;
        if (!previousPath.isEmpty()
                && std::tie(resource.repositoryPath, purpose)
                        <= std::tie(previousPath, previousPurpose)) {
            return parser.fail(QStringLiteral(
                    "resources are unsorted or duplicated"));
        }
        previousPath = resource.repositoryPath;
        previousPurpose = purpose;
        result.resources.push_back(std::move(resource));
    }
    totalResources += resources.size();
    return true;
}

bool parsePoint(CatalogParser &parser, const QJsonValue &value,
                WorkoutGameAssetCatalog::Point &result)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("profile point is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object, {"forwardMm", "heightMm"})) return false;
    qint64 forward = 0;
    qint64 height = 0;
    if (!parser.integer(object.value(QStringLiteral("forwardMm")),
                        -64000, 64000, forward, QStringLiteral("forwardMm"))
            || !parser.integer(object.value(QStringLiteral("heightMm")),
                               -16000, 16000, height,
                               QStringLiteral("heightMm"))) {
        return false;
    }
    result.forwardMm = qint32(forward);
    result.heightMm = qint32(height);
    return true;
}

bool parseChain(CatalogParser &parser, const QJsonValue &value,
                WorkoutGameAssetCatalog::Chain &result)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("profile chain is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object, {"points"})) return false;
    const QJsonValue pointsValue = object.value(QStringLiteral("points"));
    if (!pointsValue.isArray()) return parser.fail(QStringLiteral("points is not an array"));
    const QJsonArray points = pointsValue.toArray();
    if (points.size() < 2
            || points.size() > WorkoutGameAssetCatalog::MaximumProfilePoints) {
        return parser.fail(QStringLiteral("chain point count is invalid"));
    }
    for (const QJsonValue &pointValue : points) {
        WorkoutGameAssetCatalog::Point point;
        if (!parsePoint(parser, pointValue, point)) return false;
        if (!result.points.isEmpty()
                && point.forwardMm <= result.points.constLast().forwardMm) {
            return parser.fail(QStringLiteral(
                    "profile points are not strictly ordered"));
        }
        result.points.push_back(point);
    }
    return true;
}

bool parseDifficulty(CatalogParser &parser, const QJsonValue &value,
                     WorkoutGameAssetCatalog::DifficultyScale &result)
{
    if (!value.isObject()) {
        return parser.fail(QStringLiteral("difficultyScale is not an object"));
    }
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"baseExtentMm", "difficultyExtentMm", "nativeExtentMm"})) {
        return false;
    }
    qint64 nativeExtent = 0;
    qint64 baseExtent = 0;
    qint64 difficultyExtent = 0;
    if (!parser.integer(object.value(QStringLiteral("nativeExtentMm")),
                        1, 64000, nativeExtent,
                        QStringLiteral("native extent"))
            || !parser.integer(object.value(QStringLiteral("baseExtentMm")),
                               1, 64000, baseExtent,
                               QStringLiteral("base extent"))
            || !parser.integer(object.value(QStringLiteral("difficultyExtentMm")),
                               -64000, 64000, difficultyExtent,
                               QStringLiteral("difficulty extent"))) {
        return false;
    }
    const qint64 minimumExtent = baseExtent + std::min<qint64>(0, difficultyExtent);
    const qint64 maximumExtent = baseExtent + std::max<qint64>(0, difficultyExtent);
    if (minimumExtent <= 0 || maximumExtent > 64000) {
        return parser.fail(QStringLiteral("difficulty extent is invalid"));
    }
    result.nativeExtentMm = qint32(nativeExtent);
    result.baseExtentMm = qint32(baseExtent);
    result.difficultyExtentMm = qint32(difficultyExtent);
    return true;
}

bool parseProfile(CatalogParser &parser, const QJsonValue &value,
                  WorkoutGameAssetCatalog::Profile &result,
                  qsizetype &totalPoints)
{
    if (!value.isObject()) return parser.fail(QStringLiteral("profile is not an object"));
    const QJsonObject object = value.toObject();
    if (!parser.exactKeys(object,
            {"chains", "kind", "operation", "profileId", "profileVersion",
             "surface"},
            {"difficultyScale"})) {
        return false;
    }
    if (!parser.asciiString(object.value(QStringLiteral("profileId")), 1, 128,
                            result.profileId, QStringLiteral("profile ID"))
            || !matches(result.profileId, "^[A-Za-z0-9-]+$")) {
        return parser.fail(QStringLiteral("profile ID is malformed"));
    }
    QString kind;
    if (!parser.asciiString(object.value(QStringLiteral("kind")), 1, 32,
                            kind, QStringLiteral("profile kind"))
            || kind != QStringLiteral("height-offset-polyline")) {
        return parser.fail(QStringLiteral("unsupported profile kind"));
    }
    QString operation;
    if (!parser.asciiString(object.value(QStringLiteral("operation")), 1, 32,
                            operation, QStringLiteral("profile operation"))) {
        return false;
    }
    if (operation == QStringLiteral("add-obstacle")) {
        result.operation = WorkoutGameAssetCatalog::ProfileOperation::AddObstacle;
    } else if (operation == QStringLiteral("replace-surface")) {
        result.operation = WorkoutGameAssetCatalog::ProfileOperation::ReplaceSurface;
    } else {
        return parser.fail(QStringLiteral("unsupported profile operation"));
    }
    qint64 version = 0;
    if (!parser.integer(object.value(QStringLiteral("profileVersion")),
                        1, std::numeric_limits<quint32>::max(), version,
                        QStringLiteral("profile version"))) {
        return false;
    }
    result.profileVersion = quint32(version);
    if (!parseSurface(parser, object.value(QStringLiteral("surface")),
                      result.surface)) {
        return false;
    }
    if (object.contains(QStringLiteral("difficultyScale"))) {
        WorkoutGameAssetCatalog::DifficultyScale difficulty;
        if (!parseDifficulty(parser,
                object.value(QStringLiteral("difficultyScale")), difficulty)) {
            return false;
        }
        result.difficultyScale = difficulty;
    }
    const QJsonValue chainsValue = object.value(QStringLiteral("chains"));
    if (!chainsValue.isArray()) return parser.fail(QStringLiteral("chains is not an array"));
    const QJsonArray chains = chainsValue.toArray();
    if (chains.isEmpty() || chains.size() > MaximumChainsPerProfile) {
        return parser.fail(QStringLiteral("profile chain count is invalid"));
    }
    qsizetype profilePoints = 0;
    qint32 previousChainEnd = std::numeric_limits<qint32>::min();
    bool hasPreviousChain = false;
    for (const QJsonValue &chainValue : chains) {
        WorkoutGameAssetCatalog::Chain chain;
        if (!parseChain(parser, chainValue, chain)) return false;
        if (hasPreviousChain
                && chain.points.constFirst().forwardMm <= previousChainEnd) {
            return parser.fail(QStringLiteral(
                    "profile chains overlap or are unsorted"));
        }
        previousChainEnd = chain.points.constLast().forwardMm;
        hasPreviousChain = true;
        profilePoints += chain.points.size();
        if (profilePoints > WorkoutGameAssetCatalog::MaximumProfilePoints
                || totalPoints + profilePoints
                        > WorkoutGameAssetCatalog::MaximumTotalProfilePoints) {
            return parser.fail(QStringLiteral("profile point budget exceeded"));
        }
        result.chains.push_back(std::move(chain));
    }
    totalPoints += profilePoints;
    return true;
}

QString resourceFileName(const QString &url)
{
    return QStringLiteral(":") + url.mid(4);
}

} // namespace

std::unique_ptr<const WorkoutGameAssetCatalog>
WorkoutGameAssetCatalog::fromJson(const QByteArray &bytes, QString *error)
{
    if (error) error->clear();
    auto reject = [error](const QString &message) {
        if (error) *error = message;
        return std::unique_ptr<const WorkoutGameAssetCatalog>();
    };
    if (bytes.isEmpty() || bytes.size() > MaximumCatalogBytes) {
        return reject(QStringLiteral("asset catalog byte size is invalid"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(QStringLiteral("asset catalog JSON is invalid"));
    }

    CatalogParser parser;
    const QJsonObject root = document.object();
    if (!parser.exactKeys(root,
            {"assets", "generatorVersion", "profiles", "schemaVersion"})) {
        return reject(parser.error());
    }
    qint64 schemaVersion = 0;
    qint64 generatorVersion = 0;
    if (!parser.integer(root.value(QStringLiteral("schemaVersion")), 1,
                        std::numeric_limits<quint32>::max(), schemaVersion,
                        QStringLiteral("schema version"))
            || !parser.integer(root.value(QStringLiteral("generatorVersion")), 1,
                               std::numeric_limits<quint32>::max(),
                               generatorVersion,
                               QStringLiteral("generator version"))) {
        return reject(parser.error());
    }
    if (schemaVersion != CatalogSchemaVersion
            || generatorVersion != CatalogGeneratorVersion) {
        return reject(QStringLiteral("asset catalog version is unsupported"));
    }
    if (!root.value(QStringLiteral("profiles")).isArray()
            || !root.value(QStringLiteral("assets")).isArray()) {
        return reject(QStringLiteral("asset catalog collections are invalid"));
    }
    const QJsonArray profileValues = root.value(QStringLiteral("profiles")).toArray();
    const QJsonArray assetValues = root.value(QStringLiteral("assets")).toArray();
    if (profileValues.size() > MaximumProfiles
            || assetValues.isEmpty() || assetValues.size() > MaximumAssets) {
        return reject(QStringLiteral("asset catalog collection size is invalid"));
    }

    std::unique_ptr<WorkoutGameAssetCatalog> catalog(
            new WorkoutGameAssetCatalog());
    catalog->m_schemaVersion = quint32(schemaVersion);
    catalog->m_generatorVersion = quint32(generatorVersion);
    catalog->m_profiles.reserve(profileValues.size());
    catalog->m_assets.reserve(assetValues.size());

    qsizetype totalPoints = 0;
    QString previousProfileId;
    QSet<QString> profileIds;
    for (const QJsonValue &profileValue : profileValues) {
        Profile profile;
        if (!parseProfile(parser, profileValue, profile, totalPoints)) {
            return reject(parser.error());
        }
        if ((!previousProfileId.isEmpty() && profile.profileId <= previousProfileId)
                || profileIds.contains(profile.profileId)) {
            return reject(QStringLiteral("profiles are unsorted or duplicated"));
        }
        previousProfileId = profile.profileId;
        profileIds.insert(profile.profileId);
        catalog->m_profiles.push_back(std::move(profile));
    }

    qsizetype totalResources = 0;
    QString previousAssetId;
    QHash<QString, QString> urlOwners;
    QHash<QString, QString> pathOwners;
    for (const QJsonValue &assetValue : assetValues) {
        Asset asset;
        if (!parseAsset(parser, assetValue, asset, totalResources)) {
            return reject(parser.error());
        }
        if (!previousAssetId.isEmpty() && asset.assetId <= previousAssetId) {
            return reject(QStringLiteral("assets are unsorted or duplicated"));
        }
        previousAssetId = asset.assetId;
        for (const RouteProfileBinding &binding : asset.physics.routeProfiles) {
            if (!profileIds.contains(binding.profileId)) {
                return reject(QStringLiteral(
                        "route profile binding references a missing profile"));
            }
        }
        for (const Resource &resource : asset.resources) {
            const QString identity = resource.repositoryPath
                    + QLatin1Char('\n') + QString::number(resource.bytes);
            const auto url = urlOwners.constFind(resource.url);
            if (url != urlOwners.constEnd() && *url != identity) {
                return reject(QStringLiteral("resource URL ownership conflicts"));
            }
            const QString reverseIdentity = resource.url
                    + QLatin1Char('\n') + QString::number(resource.bytes);
            const auto path = pathOwners.constFind(resource.repositoryPath);
            if (path != pathOwners.constEnd() && *path != reverseIdentity) {
                return reject(QStringLiteral("resource path ownership conflicts"));
            }
            urlOwners.insert(resource.url, identity);
            pathOwners.insert(resource.repositoryPath, reverseIdentity);
        }
        catalog->m_assets.push_back(std::move(asset));
    }
    return std::unique_ptr<const WorkoutGameAssetCatalog>(catalog.release());
}

std::unique_ptr<const WorkoutGameAssetCatalog>
WorkoutGameAssetCatalog::load(QString *error)
{
    if (error) error->clear();
    QFile input(QStringLiteral(":/json/workout-game-asset-catalog.json"));
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0
            || input.size() > MaximumCatalogBytes) {
        if (error) *error = QStringLiteral("packaged asset catalog is unavailable");
        return {};
    }
    const QByteArray bytes = input.read(MaximumCatalogBytes + 1);
    if (bytes.size() != input.size()) {
        if (error) *error = QStringLiteral("packaged asset catalog could not be read");
        return {};
    }
    std::unique_ptr<const WorkoutGameAssetCatalog> catalog = fromJson(bytes, error);
    if (!catalog) return {};

    QSet<QString> checkedUrls;
    for (const Asset &asset : catalog->assets()) {
        for (const Resource &resource : asset.resources) {
            if (checkedUrls.contains(resource.url)) continue;
            QFile packaged(resourceFileName(resource.url));
            if (!packaged.open(QIODevice::ReadOnly)
                    || packaged.size() != resource.bytes) {
                if (error) {
                    *error = QStringLiteral("packaged asset resource is unavailable");
                }
                return {};
            }
            checkedUrls.insert(resource.url);
        }
    }
    return catalog;
}

const WorkoutGameAssetCatalog::Asset *
WorkoutGameAssetCatalog::findAsset(const QString &assetId) const
{
    const auto iterator = std::lower_bound(m_assets.constBegin(), m_assets.constEnd(),
            assetId, [](const Asset &asset, const QString &value) {
                return asset.assetId < value;
            });
    return iterator != m_assets.constEnd() && iterator->assetId == assetId
            ? &*iterator : nullptr;
}

const WorkoutGameAssetCatalog::Profile *
WorkoutGameAssetCatalog::findProfile(const QString &profileId) const
{
    const auto iterator = std::lower_bound(
            m_profiles.constBegin(), m_profiles.constEnd(), profileId,
            [](const Profile &profile, const QString &value) {
                return profile.profileId < value;
            });
    return iterator != m_profiles.constEnd() && iterator->profileId == profileId
            ? &*iterator : nullptr;
}
