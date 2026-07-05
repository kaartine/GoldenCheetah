/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_TRAINPERSPECTIVESTATE_H
#define GC_TRAINPERSPECTIVESTATE_H

#include <QString>

namespace TrainPerspectiveState {

constexpr int CurrentVersion = 2;

bool migrate(QString &content, const QString &defaultsContent);

}

#endif
