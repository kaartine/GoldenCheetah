/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LibraryScanPersistence.h"

LibraryScanPersistenceResult LibraryScanPersistence::persist(
        QList<QString> &paths,
        QList<QString> &references,
        const QList<QString> &requestedPaths,
        const QList<QString> &requestedReferences,
        const Operation &serialize,
        const Operation &updateDatabase)
{
    const QList<QString> previousPaths = paths;
    const QList<QString> previousReferences = references;
    paths = requestedPaths;
    references = requestedReferences;

    LibraryScanPersistenceResult result;
    if (!serialize(&result.error)) {
        paths = previousPaths;
        references = previousReferences;
        result.status = LibraryScanPersistenceStatus::SerializationFailed;
        return result;
    }

    if (updateDatabase(&result.error)) return result;

    paths = previousPaths;
    references = previousReferences;
    if (!serialize(&result.recoveryError)) {
        result.status = LibraryScanPersistenceStatus::RecoveryFailed;
        return result;
    }

    result.status = LibraryScanPersistenceStatus::DatabaseFailed;
    return result;
}
