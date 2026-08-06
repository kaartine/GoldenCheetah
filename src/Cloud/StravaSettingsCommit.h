/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_SETTINGS_COMMIT_H
#define GC_STRAVA_SETTINGS_COMMIT_H

#include <functional>

class QObject;

namespace StravaSettingsCommit {

enum class DispatchStatus
{
    Completed,
    NotStarted,
    Pending
};

struct DispatchResult
{
    DispatchStatus status = DispatchStatus::NotStarted;

    bool completed() const
    {
        return status == DispatchStatus::Completed;
    }
};

using CancellationCheck = std::function<bool()>;

DispatchResult runOnCredentialThread(
    const std::function<void()> &operation,
    int timeoutMs,
    const CancellationCheck &cancelled = {});

bool runOnCredentialThreadAsync(
    const std::function<void()> &operation,
    const std::function<void()> &completed);
bool runOnCredentialThreadAsync(
    const std::function<void()> &operation,
    QObject *completionContext,
    const std::function<void()> &completed);
bool credentialThreadShutdownRequested();
bool shutdownCredentialThread();
bool restartCredentialThread();
bool credentialThreadStopped();
#ifdef GC_CREDENTIAL_TEST_HOOKS
int credentialWorkerLiveInstancesForTest();
#endif

} // namespace StravaSettingsCommit

#endif
