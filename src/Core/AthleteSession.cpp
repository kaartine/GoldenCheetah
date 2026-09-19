/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AthleteSession.h"

#include "AthleteRefreshLifecycle.h"
#include "SessionServices.h"

#include <QtGlobal>

AthleteSession::AthleteSession(
    std::unique_ptr<AthleteApplicationService> applicationService,
    std::unique_ptr<AthletePersistenceService> persistenceService)
    : applicationService_(std::move(applicationService))
    , persistenceService_(std::move(persistenceService))
    , refreshLifecycle_(std::make_unique<AthleteRefreshLifecycle>())
{
    Q_ASSERT(applicationService_);
    Q_ASSERT(persistenceService_);
    Q_ASSERT(refreshLifecycle_);
}

AthleteSession::~AthleteSession() = default;

QWebEngineProfile *AthleteSession::webEngineProfile() const
{
    return applicationService_->webEngineProfile();
}

AthletePersistenceService &AthleteSession::persistenceService() const
{
    return *persistenceService_;
}

AthleteRefreshLifecycle &AthleteSession::refreshLifecycle()
{
    return *refreshLifecycle_;
}

const AthleteRefreshLifecycle &AthleteSession::refreshLifecycle() const
{
    return *refreshLifecycle_;
}
