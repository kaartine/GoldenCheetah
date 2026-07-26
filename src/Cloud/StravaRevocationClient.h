/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_REVOCATION_CLIENT_H
#define GC_STRAVA_REVOCATION_CLIENT_H

#include "StravaNetworkReply.h"
#include "StravaOAuthPolicy.h"

#include <QString>

#include <functional>

class StravaRevocationClient final
{
public:
    static constexpr qsizetype MaximumResponseBytes =
        64 * 1024;

    enum class Status {
        Succeeded,
        InvalidRequest,
        TimedOut,
        Cancelled,
        OversizedResponse,
        InvalidResponse,
        TransportFailure,
        HttpFailure
    };

    struct Result {
        Status status = Status::InvalidRequest;
        int httpStatus = 0;
        QString error;

        bool isSuccess() const
        {
            return status == Status::Succeeded
                && error.isEmpty();
        }
    };

    using CancellationCheck =
        StravaNetworkReply::CancellationCheck;
    using RequestOperation =
        std::function<StravaNetworkReply::Result(
            const StravaOAuthPolicy::RevocationRequest &request,
            qsizetype maximumBytes,
            const CancellationCheck &cancelled)>;

    explicit StravaRevocationClient(
        RequestOperation requestOperation);

    Result revoke(
        const QString &clientId,
        const QString &clientSecret,
        const QString &token,
        StravaOAuthPolicy::RevocationTokenType tokenType,
        const CancellationCheck &cancelled = {}) const;

private:
    RequestOperation requestOperation_;
};

#endif
