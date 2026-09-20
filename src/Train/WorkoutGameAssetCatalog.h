/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef WORKOUT_GAME_ASSET_CATALOG_H
#define WORKOUT_GAME_ASSET_CATALOG_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

class WorkoutGameAssetCatalog final
{
public:
    static constexpr qsizetype MaximumCatalogBytes = 1024 * 1024;
    static constexpr qsizetype MaximumAssets = 256;
    static constexpr qsizetype MaximumProfiles = 512;
    static constexpr qsizetype MaximumProfilePoints = 256;
    static constexpr qsizetype MaximumTotalProfilePoints = 32768;

    enum class Role {
        RiderBike,
        TrailTile,
        Feature,
        Terrain,
        Vegetation,
        Prop,
        Effect,
        Texture,
        Reference
    };

    enum class ResourcePurpose { Runtime, Texture };
    enum class Interaction { VisualOnly, Surface, Obstacle, RideableFeature };
    enum class ProfileOperation { AddObstacle, ReplaceSurface };

    struct Resource {
        qint64 bytes = 0;
        ResourcePurpose purpose = ResourcePurpose::Runtime;
        QString repositoryPath;
        QString url;
    };

    struct Surface {
        quint16 coulombFrictionMilli = 0;
        quint16 restitutionMilli = 0;
    };

    struct RouteProfileBinding {
        QString profileId;
        QString routeKey;
        QString variantKey;
        qint32 nativeForwardOriginMm = 0;
        quint32 nativeForwardExtentMm = 0;
        quint32 nativeUpExtentMm = 0;
    };

    struct Physics {
        Interaction interaction = Interaction::VisualOnly;
        std::optional<Surface> surface;
        QVector<RouteProfileBinding> routeProfiles;
    };

    struct Asset {
        QString assetId;
        Physics physics;
        QVector<Resource> resources;
        Role role = Role::Reference;
    };

    struct Point {
        qint32 forwardMm = 0;
        qint32 heightMm = 0;
    };

    struct Chain {
        QVector<Point> points;
    };

    struct DifficultyScale {
        qint32 nativeExtentMm = 0;
        qint32 baseExtentMm = 0;
        qint32 difficultyExtentMm = 0;
    };

    struct Profile {
        QString profileId;
        quint32 profileVersion = 0;
        ProfileOperation operation = ProfileOperation::AddObstacle;
        QVector<Chain> chains;
        std::optional<DifficultyScale> difficultyScale;
        Surface surface;
    };

    static std::unique_ptr<const WorkoutGameAssetCatalog> load(
            QString *error = nullptr);
    static std::unique_ptr<const WorkoutGameAssetCatalog> fromJson(
            const QByteArray &bytes,
            QString *error = nullptr);

    quint32 schemaVersion() const { return m_schemaVersion; }
    quint32 generatorVersion() const { return m_generatorVersion; }
    const QVector<Asset> &assets() const { return m_assets; }
    const QVector<Profile> &profiles() const { return m_profiles; }

    const Asset *findAsset(const QString &assetId) const;
    const Profile *findProfile(const QString &profileId) const;

private:
    WorkoutGameAssetCatalog() = default;

    quint32 m_schemaVersion = 0;
    quint32 m_generatorVersion = 0;
    QVector<Asset> m_assets;
    QVector<Profile> m_profiles;
};

#endif
