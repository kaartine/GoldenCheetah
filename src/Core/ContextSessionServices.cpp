/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "SessionServices.h"

#include "Context.h"
#include "ErgFile.h"
#include "HtmlTrainingBridge.h"
#include "MainWindow.h"
#include "RideFileCacheWriteError.h"

#include <QDebug>
#include <QPointer>
#include <QWebEngineProfile>

#include <utility>

namespace {

class ContextAthleteApplicationService final
    : public AthleteApplicationService
{
public:
    ContextAthleteApplicationService()
        : profile_(
              std::make_unique<QWebEngineProfile>(
                  QStringLiteral("Default")))
    {
        profile_->setPersistentCookiesPolicy(
            QWebEngineProfile::ForcePersistentCookies);
    }

    QWebEngineProfile *webEngineProfile() const override
    {
        return profile_.get();
    }

private:
    std::unique_ptr<QWebEngineProfile> profile_;
};

class ContextAthletePersistenceService final
    : public AthletePersistenceService
{
public:
    ContextAthletePersistenceService(
        QObject *owner,
        CacheWriteFailureNotify notify)
        : owner_(owner), notify_(std::move(notify))
    {
    }

    void reportCacheWriteFailure(
        const QString &cachePath,
        const QString &detail) override
    {
        const auto result = coordinator_.report(
            cachePath,
            detail,
            [owner = owner_](
                RideFileCacheWriteErrorCoordinator::Delivery delivery) {
                return RideFileCacheWriteErrorCoordinator::queueForOwner(
                    owner.data(),
                    std::move(delivery));
            },
            notify_);
        if (result == RideFileCacheWriteErrorCoordinator::
                          ReportResult::DispatchFailed) {
            qWarning().noquote()
                << "Cannot queue CPX cache write failure notification";
        }
    }

private:
    QPointer<QObject> owner_;
    CacheWriteFailureNotify notify_;
    RideFileCacheWriteErrorCoordinator coordinator_;
};

class ContextTrainingApplicationService final
    : public TrainingApplicationService
{
public:
    explicit ContextTrainingApplicationService(Context *context)
        : context_(context)
    {
    }

    HtmlTrainingBridge *htmlTrainingBridge() override
    {
        if (!bridge_) {
            bridge_ = std::make_unique<HtmlTrainingBridge>(context_);
            qDebug() << "Context: HtmlTrainingBridge created";
        }
        return bridge_.get();
    }

private:
    Context *context_;
    std::unique_ptr<HtmlTrainingBridge> bridge_;
};

} // namespace

std::unique_ptr<AthleteApplicationService>
createContextAthleteApplicationService()
{
    return std::make_unique<ContextAthleteApplicationService>();
}

std::unique_ptr<AthletePersistenceService>
createContextAthletePersistenceService(
    QObject *owner,
    CacheWriteFailureNotify notify)
{
    return std::make_unique<ContextAthletePersistenceService>(
        owner, std::move(notify));
}

std::unique_ptr<TrainingApplicationService>
createContextTrainingApplicationService(Context *context)
{
    return std::make_unique<ContextTrainingApplicationService>(context);
}

void connectContextMainWindow(
    Context *context,
    MainWindow *mainWindow)
{
    if (!context || !mainWindow) return;
    QObject::connect(
        context, SIGNAL(loadProgress(QString, double)),
        mainWindow, SLOT(loadProgress(QString, double)));
    QObject::connect(
        context, SIGNAL(cacheWriteFailed(QString)),
        mainWindow, SLOT(cacheWriteFailed(QString)));
}

ErgFileBase *asErgFileBase(ErgFile *workout)
{
    return static_cast<ErgFileBase *>(workout);
}
