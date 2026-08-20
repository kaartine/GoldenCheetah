/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRendererPolicy.h"

#include <algorithm>
#include <cctype>

WorkoutGameRendererBackend WorkoutGameRendererPolicy::choose(
        bool forcePainter,
        const std::string &platformName,
        double openGLMajorVersion)
{
    std::string normalizedPlatform = platformName;
    std::transform(
            normalizedPlatform.begin(), normalizedPlatform.end(),
            normalizedPlatform.begin(),
            [](unsigned char character) { return std::tolower(character); });
    const bool widgetOpenGLUnsupported = normalizedPlatform == "offscreen"
            || normalizedPlatform == "minimal";
    return forcePainter
            || widgetOpenGLUnsupported
            || openGLMajorVersion < 2.0
            ? WorkoutGameRendererBackend::Painter
            : WorkoutGameRendererBackend::SceneGraph;
}

WorkoutGameRendererBackend WorkoutGameRendererPolicy::afterInitializationFailure(
        WorkoutGameRendererBackend failedBackend)
{
    switch (failedBackend) {
    case WorkoutGameRendererBackend::SceneGraph:
        return WorkoutGameRendererBackend::OpenGL;
    case WorkoutGameRendererBackend::OpenGL:
    case WorkoutGameRendererBackend::Painter:
        return WorkoutGameRendererBackend::Painter;
    }
    return WorkoutGameRendererBackend::Painter;
}
