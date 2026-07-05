/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_PERSPECTIVESTATESOURCE_H
#define GC_PERSPECTIVESTATESOURCE_H

#include <QByteArray>
#include <QString>

class PerspectiveStateSource
{
public:
    static QByteArray load(const QString &savedFileName,
                           const QString &view,
                           bool useDefault);
};

#endif
