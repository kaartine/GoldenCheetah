/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameRendererPolicy_h
#define _GC_WorkoutGameRendererPolicy_h

#include <string>

enum class WorkoutGameRendererBackend
{
    SceneGraph,
    OpenGL,
    Painter
};

enum class WorkoutGameRendererSelectionReason
{
    Preferred,
    ForcedPainter,
    UnsupportedPlatform,
    InsufficientOpenGL
};

struct WorkoutGameRendererDecision
{
    WorkoutGameRendererBackend backend = WorkoutGameRendererBackend::Painter;
    WorkoutGameRendererSelectionReason reason =
            WorkoutGameRendererSelectionReason::Preferred;
};

class WorkoutGameRendererPolicy
{
public:
    static WorkoutGameRendererDecision decide(
            bool forcePainter,
            const std::string &platformName,
            double openGLMajorVersion);
    static WorkoutGameRendererBackend choose(
            bool forcePainter,
            const std::string &platformName,
            double openGLMajorVersion);
    static WorkoutGameRendererBackend afterInitializationFailure(
            WorkoutGameRendererBackend failedBackend);
    static const char *backendName(WorkoutGameRendererBackend backend);
    static const char *reasonName(WorkoutGameRendererSelectionReason reason);
};

#endif
