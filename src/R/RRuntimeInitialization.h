/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef GC_R_RUNTIME_INITIALIZATION_H
#define GC_R_RUNTIME_INITIALIZATION_H

// Synchronize the process owner's observable state on both normal and
// exceptional exits. An embedded runtime can become process-global before its
// remaining wrapper setup completes, so the outer owner must never mistake a
// post-initialization exception for a safe-to-destroy pre-init failure.
template <typename Runtime, typename Synchronize>
void initializeRuntimeAndSynchronize(Runtime *runtime, Synchronize synchronize)
{
    try {
        runtime->initialize();
    } catch (...) {
        synchronize(runtime);
        throw;
    }
    synchronize(runtime);
}

#endif
