/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_STRAVA_CREDENTIAL_PUBLISHER_H
#define GC_STRAVA_CREDENTIAL_PUBLISHER_H

#include "StravaTokenPublication.h"

#include <QString>

#include <functional>

namespace StravaCredentialPublisher {

struct Request
{
    QString accountKey;
    QString expectedRefreshToken;
    StravaTokenPublication::TokenPair replacement;
    QString refreshedAt;
    StravaTokenPublication::PublicationMode mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;

    bool isValid() const;
};

using CancellationCheck = std::function<bool()>;

StravaTokenPublication::PublicationResult publish(
    const Request &request,
    int timeoutMs = 30000,
    const CancellationCheck &cancelled = {});

} // namespace StravaCredentialPublisher

#endif
