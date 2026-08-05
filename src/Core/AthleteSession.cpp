/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AthleteSession.h"

#include "SessionServices.h"

#include <QtGlobal>

AthleteSession::AthleteSession(
    std::unique_ptr<AthleteApplicationService> applicationService,
    std::unique_ptr<AthletePersistenceService> persistenceService)
    : applicationService_(std::move(applicationService))
    , persistenceService_(std::move(persistenceService))
{
    Q_ASSERT(applicationService_);
    Q_ASSERT(persistenceService_);
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
