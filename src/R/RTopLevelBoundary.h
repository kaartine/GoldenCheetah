/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_R_TOP_LEVEL_BOUNDARY_H
#define GC_R_TOP_LEVEL_BOUNDARY_H

#include <Rinternals.h>

// Execute allocation/error-capable R C API code behind R's own top-level
// context. Callbacks passed here must use only C/POD state: an R longjmp skips
// C++ destructors inside the callback, but returns to this boundary before it
// can cross the caller's C++ ownership scopes.
inline bool executeAtRTopLevel(void (*callback)(void *), void *data)
{
    return R_ToplevelExec(callback, data) == TRUE;
}

#endif
