/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenGLVersionProbe.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurface>

namespace OpenGLVersionProbe {

QString versionFromContext(
    QOpenGLContext &context, QSurface *surface)
{
    if (!surface || !context.isValid()
        || !context.makeCurrent(surface)) {
        return QString();
    }

    QOpenGLFunctions *const functions = context.functions();
    const auto *const versionBytes = functions
        ? functions->glGetString(GL_VERSION) : nullptr;
    const QString version = versionBytes
        ? QString::fromLatin1(
              reinterpret_cast<const char *>(versionBytes))
        : QString();
    context.doneCurrent();
    return version;
}

QString detect()
{
    QOpenGLContext context;
    if (!context.create()) return QString();

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid()) return QString();

    return versionFromContext(context, &surface);
}

}
