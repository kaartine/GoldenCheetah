/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHEMUTATIONSCOPE_H
#define GC_RIDECACHEMUTATIONSCOPE_H

#include <memory>

class RideCache;
class QString;

class RideCacheMutationScope final
{
public:
    RideCacheMutationScope(RideCache *cache, QString &error);
    ~RideCacheMutationScope();

    RideCacheMutationScope(const RideCacheMutationScope &) = delete;
    RideCacheMutationScope &operator=(
        const RideCacheMutationScope &) = delete;

    bool ready() const;
    bool ownersStable() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif // GC_RIDECACHEMUTATIONSCOPE_H
