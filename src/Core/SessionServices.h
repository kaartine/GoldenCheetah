/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_SESSIONSERVICES_H
#define GC_SESSIONSERVICES_H

#include <functional>
#include <memory>

class Context;
class ErgFile;
class ErgFileBase;
class HtmlTrainingBridge;
class MainWindow;
class QObject;
class QWebEngineProfile;
class QString;

class AthleteApplicationService
{
public:
    virtual ~AthleteApplicationService() = default;
    virtual QWebEngineProfile *webEngineProfile() const = 0;
};

class AthletePersistenceService
{
public:
    virtual ~AthletePersistenceService() = default;
    virtual void reportCacheWriteFailure(
        const QString &cachePath,
        const QString &detail) = 0;
};

class TrainingApplicationService
{
public:
    virtual ~TrainingApplicationService() = default;
    virtual HtmlTrainingBridge *htmlTrainingBridge() = 0;
};

using CacheWriteFailureNotify =
    std::function<void(const QString &)>;

std::unique_ptr<AthleteApplicationService>
createContextAthleteApplicationService();
std::unique_ptr<AthletePersistenceService>
createContextAthletePersistenceService(
    QObject *owner,
    CacheWriteFailureNotify notify);
std::unique_ptr<TrainingApplicationService>
createContextTrainingApplicationService(Context *context);
void connectContextMainWindow(
    Context *context,
    MainWindow *mainWindow);
ErgFileBase *asErgFileBase(ErgFile *workout);

#endif // GC_SESSIONSERVICES_H
