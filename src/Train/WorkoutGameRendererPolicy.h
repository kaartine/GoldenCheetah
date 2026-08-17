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
    OpenGL,
    Painter
};

class WorkoutGameRendererPolicy
{
public:
    static WorkoutGameRendererBackend choose(
            bool forcePainter,
            const std::string &platformName,
            double openGLMajorVersion);
    static WorkoutGameRendererBackend afterInitializationFailure();
};

#endif
