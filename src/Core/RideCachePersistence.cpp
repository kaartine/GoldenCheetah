/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCachePersistence.h"

bool writeRideCacheAtomically(
    const QString &path,
    const QByteArray &document,
    QString &error,
    const AtomicFileWriterFactory &writerFactory)
{
    return writeFileAtomically(path, document, writerFactory, error);
}
