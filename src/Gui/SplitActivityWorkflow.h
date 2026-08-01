/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_SPLIT_ACTIVITY_WORKFLOW_H
#define GC_SPLIT_ACTIVITY_WORKFLOW_H

#include <QString>

#include <functional>

struct SplitActivitySourceIdentity
{
    QString fileName;
    QString path;
    bool planned = false;
};

struct SplitActivityContentSnapshot
{
    quintptr rideDataIdentity = 0;
    quint64 revision = 0;
};

inline bool splitActivitySourceIsCurrent(
    bool contextAvailable,
    bool cacheAvailable,
    bool sourceAvailable,
    bool sourceInCache,
    const SplitActivitySourceIdentity &expected,
    const SplitActivitySourceIdentity &current)
{
    return contextAvailable
        && cacheAvailable
        && sourceAvailable
        && sourceInCache
        && !expected.fileName.isEmpty()
        && !expected.path.isEmpty()
        && expected.fileName == current.fileName
        && expected.path == current.path
        && expected.planned == current.planned;
}

inline bool splitActivitySourceSnapshotIsCurrent(
    bool contextAvailable,
    bool cacheAvailable,
    bool sourceAvailable,
    bool sourceInCache,
    const SplitActivitySourceIdentity &expectedIdentity,
    const SplitActivitySourceIdentity &currentIdentity,
    const SplitActivityContentSnapshot &expectedContent,
    const SplitActivityContentSnapshot &currentContent)
{
    return splitActivitySourceIsCurrent(
        contextAvailable, cacheAvailable,
        sourceAvailable, sourceInCache,
        expectedIdentity, currentIdentity)
        && expectedContent.rideDataIdentity != 0
        && expectedContent.rideDataIdentity
            == currentContent.rideDataIdentity
        && expectedContent.revision == currentContent.revision;
}

using SplitSourcePreflight =
    std::function<bool()>;
using SplitSourceRefresh =
    std::function<void()>;

inline bool prepareSplitSourceBeforeSelection(
    bool keepOriginal,
    const SplitSourcePreflight &sourceCurrent,
    const SplitSourcePreflight &preflight,
    const SplitSourceRefresh &refresh)
{
    if (!sourceCurrent || !sourceCurrent())
        return false;
    if (keepOriginal) return true;
    if (!preflight || !refresh
        || !preflight()) {
        return false;
    }
    if (!sourceCurrent()) return false;
    refresh();
    return sourceCurrent();
}

#endif
