/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_USER_METRIC_REGISTRY_SAFETY_TEST_TYPES_H
#define GC_USER_METRIC_REGISTRY_SAFETY_TEST_TYPES_H

#include <QtGlobal>

struct Context;

struct TestAthleteContext
{
    quint64 value = 0;
};

inline Context *asContext(TestAthleteContext *context)
{
    return reinterpret_cast<Context *>(context);
}

inline const TestAthleteContext *asTestContext(const Context *context)
{
    return reinterpret_cast<const TestAthleteContext *>(context);
}

#endif
