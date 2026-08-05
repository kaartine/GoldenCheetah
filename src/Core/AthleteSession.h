/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_ATHLETESESSION_H
#define GC_ATHLETESESSION_H

#include <memory>

class AthleteApplicationService;
class AthletePersistenceService;
class QWebEngineProfile;

class AthleteSession final
{
public:
    AthleteSession(
        std::unique_ptr<AthleteApplicationService> applicationService,
        std::unique_ptr<AthletePersistenceService> persistenceService);
    ~AthleteSession();

    AthleteSession(const AthleteSession &) = delete;
    AthleteSession &operator=(const AthleteSession &) = delete;

    QWebEngineProfile *webEngineProfile() const;
    AthletePersistenceService &persistenceService() const;

private:
    std::unique_ptr<AthleteApplicationService> applicationService_;
    std::unique_ptr<AthletePersistenceService> persistenceService_;
};

#endif // GC_ATHLETESESSION_H
