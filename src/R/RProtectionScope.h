/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RPROTECTIONSCOPE_H
#define GC_RPROTECTIONSCOPE_H

#include <Rinternals.h>

class RProtectionScope
{
public:
    RProtectionScope() = default;
    ~RProtectionScope()
    {
        if (count_ > 0) UNPROTECT(count_);
    }

    RProtectionScope(const RProtectionScope &) = delete;
    RProtectionScope &operator=(const RProtectionScope &) = delete;

    SEXP protectValue(SEXP value)
    {
        PROTECT(value);
        ++count_;
        return value;
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

#endif
