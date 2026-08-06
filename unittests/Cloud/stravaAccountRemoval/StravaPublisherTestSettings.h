/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_PUBLISHER_TEST_SETTINGS_H
#define GC_STRAVA_PUBLISHER_TEST_SETTINGS_H

#include <QString>
#include <QVariant>

#include <functional>

namespace StravaPublisherTestSettings {

void reset();
void seedAuthorization(
    const QString &accountKey,
    const QString &accessToken,
    const QString &refreshToken);
void setValue(
    const QString &accountKey,
    const QString &key,
    const QVariant &value);
QVariant value(
    const QString &accountKey,
    const QString &key,
    const QVariant &fallback = {});

void setCredentialReadsAvailable(bool available);
void setPendingStateWriteFailure(bool fail);
void setRevokedStateWriteFailure(bool fail);
void setTokenRemovalFailure(bool fail);
void setPendingStateHook(std::function<void()> hook);

int pendingStateWrites();
int tokenRemovalWrites();

} // namespace StravaPublisherTestSettings

#endif
