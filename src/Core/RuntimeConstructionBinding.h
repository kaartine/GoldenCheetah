/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RUNTIMECONSTRUCTIONBINDING_H
#define GC_RUNTIMECONSTRUCTIONBINDING_H

// Temporarily exposes a runtime only to its own native callbacks. The public
// compatibility alias remains unpublished until the process owner observes a
// Ready runtime. Nested construction never replaces the active candidate.
template <typename Runtime>
class RuntimeConstructionBinding
{
public:
    RuntimeConstructionBinding(Runtime *&slot, Runtime *candidate)
        : slot_(slot), candidate_(candidate), acquired_(slot == nullptr)
    {
        if (acquired_) slot_ = candidate_;
    }

    ~RuntimeConstructionBinding()
    {
        if (acquired_ && slot_ == candidate_) slot_ = nullptr;
    }

    RuntimeConstructionBinding(const RuntimeConstructionBinding &) = delete;
    RuntimeConstructionBinding &operator=(const RuntimeConstructionBinding &) = delete;

    bool acquired() const { return acquired_; }

private:
    Runtime *&slot_;
    Runtime *candidate_;
    bool acquired_;
};

#endif
