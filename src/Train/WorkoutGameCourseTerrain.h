/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseTerrain_h
#define _GC_WorkoutGameCourseTerrain_h

#include "WorkoutGameCourse.h"
#include "WorkoutGameCoursePrescription.h"

#include <cstddef>
#include <cstdint>

class WorkoutGameCourseTerrain
{
public:
    static constexpr std::uint32_t CurrentGenerationVersion = 1;

    static bool paletteEligible(WorkoutGameFeature feature);
    static void apply(
            WorkoutGameSection &section,
            WorkoutGameCoursePreset preset,
            std::size_t paletteIndex,
            std::size_t paletteCount,
            std::uint32_t seed,
            bool sourceRecovery);
    static double curvatureDegreesPer100m(WorkoutGameCoursePreset preset);
};

#endif
