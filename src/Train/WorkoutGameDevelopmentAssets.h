/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameDevelopmentAssets_h
#define _GC_WorkoutGameDevelopmentAssets_h

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

class QFileSystemWatcher;
class QTimer;

class WorkoutGameDevelopmentAssets : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY assetsChanged)
    Q_PROPERTY(QString workspace READ workspace CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY assetsChanged)

public:
    explicit WorkoutGameDevelopmentAssets(QObject *parent = nullptr);
    ~WorkoutGameDevelopmentAssets() override;

    bool enabled() const { return developmentEnabled; }
    qulonglong revision() const { return publishedRevision; }
    QString workspace() const { return workspaceRoot; }
    QString lastError() const { return publishedError; }

    Q_INVOKABLE QUrl sourceForAsset(
            const QString &assetId,
            const QString &variantKey,
            qulonglong observedRevision) const;
    Q_INVOKABLE QVariantMap materialProperties(
            const QString &assetId,
            const QString &materialName,
            const QString &fallbackColor,
            double fallbackRoughness,
            double fallbackMetallic,
            qulonglong observedRevision) const;
    Q_INVOKABLE void reportRuntimeError(
            const QString &assetId,
            const QString &message);

signals:
    void assetsChanged();

private slots:
    void scheduleReload();
    void startReload();
    void finishReload();

private:
    struct BuildResult {
        QHash<QString, QUrl> sources;
        QHash<QString, QHash<QString, QVariantMap>> materials;
        QSet<QString> cacheFiles;
        QString error;
    };

    static BuildResult buildAssets(
            const QString &workspaceRoot,
            const QString &cacheRoot,
            qulonglong generation,
            qulonglong workspaceDevice,
            qulonglong workspaceInode);
    static QString sourceKey(
            const QString &assetId,
            const QString &variantKey);
    void configureWorkspace();
    void configureWatcher();
    void refreshWatchedFiles();

    QFileSystemWatcher *watcher = nullptr;
    QTimer *reloadTimer = nullptr;
    QTimer *cacheCleanupTimer = nullptr;
    QTemporaryDir cacheDirectory;
    QFutureWatcher<BuildResult> buildWatcher;
    QHash<QString, QUrl> publishedSources;
    QHash<QString, QHash<QString, QVariantMap>> publishedMaterials;
    QSet<QString> publishedCacheFiles;
    QSet<QString> retiredCacheFiles;
    QString workspaceRoot;
    QString publishedError;
    qulonglong publishedRevision = 0;
    qulonglong nextGeneration = 1;
    qulonglong workspaceDevice = 0;
    qulonglong workspaceInode = 0;
    bool developmentEnabled = false;
    bool reloadRequestedDuringBuild = false;
};

#endif
