/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_LIBRARY_SCAN_PERSISTENCE_H
#define GC_LIBRARY_SCAN_PERSISTENCE_H

#include <QList>
#include <QString>

#include <functional>

enum class LibraryScanPersistenceStatus {
    Succeeded,
    SerializationFailed,
    DatabaseFailed,
    RecoveryFailed
};

struct LibraryScanPersistenceResult {
    LibraryScanPersistenceStatus status =
            LibraryScanPersistenceStatus::Succeeded;
    QString error;
    QString recoveryError;

    bool succeeded() const
    {
        return status == LibraryScanPersistenceStatus::Succeeded;
    }
};

class LibraryScanPersistence
{
public:
    using Operation = std::function<bool(QString *)>;

    static LibraryScanPersistenceResult persist(
            QList<QString> &paths,
            QList<QString> &references,
            const QList<QString> &requestedPaths,
            const QList<QString> &requestedReferences,
            const Operation &serialize,
            const Operation &updateDatabase);
};

#endif
