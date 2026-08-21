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

WorkoutGameRendererDecision WorkoutGameRendererPolicy::decide(
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
    if (forcePainter) {
        return {WorkoutGameRendererBackend::Painter,
                WorkoutGameRendererSelectionReason::ForcedPainter};
    }
    if (widgetOpenGLUnsupported) {
        return {WorkoutGameRendererBackend::Painter,
                WorkoutGameRendererSelectionReason::UnsupportedPlatform};
    }
    if (openGLMajorVersion < 2.0) {
        return {WorkoutGameRendererBackend::Painter,
                WorkoutGameRendererSelectionReason::InsufficientOpenGL};
    }
    return {WorkoutGameRendererBackend::SceneGraph,
            WorkoutGameRendererSelectionReason::Preferred};
}

WorkoutGameRendererBackend WorkoutGameRendererPolicy::choose(
        bool forcePainter,
        const std::string &platformName,
        double openGLMajorVersion)
{
    return decide(forcePainter, platformName, openGLMajorVersion).backend;
}

const char *WorkoutGameRendererPolicy::backendName(
        WorkoutGameRendererBackend backend)
{
    switch (backend) {
    case WorkoutGameRendererBackend::SceneGraph: return "SceneGraph";
    case WorkoutGameRendererBackend::OpenGL: return "OpenGL";
    case WorkoutGameRendererBackend::Painter: return "Painter";
    }
    return "Unknown";
}

const char *WorkoutGameRendererPolicy::reasonName(
        WorkoutGameRendererSelectionReason reason)
{
    switch (reason) {
    case WorkoutGameRendererSelectionReason::Preferred: return "preferred";
    case WorkoutGameRendererSelectionReason::ForcedPainter: return "forced";
    case WorkoutGameRendererSelectionReason::UnsupportedPlatform:
        return "unsupported-platform";
    case WorkoutGameRendererSelectionReason::InsufficientOpenGL:
        return "insufficient-opengl";
    }
    return "unknown";
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
