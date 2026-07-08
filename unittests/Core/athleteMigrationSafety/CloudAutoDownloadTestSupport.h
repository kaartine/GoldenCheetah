/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_CLOUD_AUTO_DOWNLOAD_TEST_SUPPORT_H
#define GC_CLOUD_AUTO_DOWNLOAD_TEST_SUPPORT_H

#include <QString>

class CloudServiceAutoDownload;

enum class TestCloudCompletionMode {
    Inline,
    InlineWithNestedEvents,
    Queued,
    QueuedSettingsTwice,
    QueuedClearsUrlTwice,
    Never,
    Reject,
    Duplicate,
    BlockInOpen,
    BlockInDirectory,
    BlockInRead,
    BlockInBaseAbort,
    BlockInCompletion
};

void configureBlockingCloudAutoDownload(const QString &athlete);
bool waitForBlockingCloudRead(int timeoutMs);
bool releaseBlockingCloudRead(CloudServiceAutoDownload *receiver);
void cleanupBlockingCloudRead();

void configureControlledCloudAutoDownload(
        const QString &athlete,
        TestCloudCompletionMode mode,
        int entryCount = 1,
        int timeoutMs = 50);
void disableControlledCloudAutoDownload(const QString &athlete);
void enableSuccessfulFollowUpCloudAutoDownload(const QString &athlete);
bool waitForControlledCloudReads(int count, int timeoutMs);
bool waitForControlledCloudCompletions(int count, int timeoutMs);
bool waitForControlledCloudProviderDestroyed(int timeoutMs);
bool waitForControlledCloudBlockedCall(int timeoutMs);
int controlledCloudCrossThreadProviderAccesses();
int controlledCloudDestroyedProviderAccesses();
int controlledCloudDestroyedDuringCall();
int controlledCloudAbortCalls();
int controlledCloudBaseReplyAbortCalls();
int controlledCloudProviderOperations();
int controlledCloudReentrantReadCalls();
void cleanupControlledCloudAutoDownload();

int cloudAutoDownloadTestBuffersAllocated();
int cloudAutoDownloadTestBuffersReleased();
int cloudAutoDownloadTestBuffersOutstanding();
int athleteMigrationSettingsCrossThreadWrites();

#endif
