/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPENGL_VERSION_PROBE_H
#define GC_OPENGL_VERSION_PROBE_H

#include <QString>

class QOpenGLContext;
class QSurface;

namespace OpenGLVersionProbe {

QString versionFromContext(
    QOpenGLContext &context, QSurface *surface);
QString detect();

}

#endif
