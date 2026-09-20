/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameWorldGroundProfile_h
#define _GC_WorkoutGameWorldGroundProfile_h

#include <vector>

struct WorkoutGameRoadCourse;

struct WorkoutGameWorldGroundMaterial
{
    bool assetDefined = false;
    double coulombFriction = 1.1;
    double restitution = 0.0;
};

class WorkoutGameWorldGroundProfile
{
public:
    static std::vector<double> mergeBreakpoints(
            const WorkoutGameRoadCourse &course,
            double distanceBaseMeters,
            double riderStartMeters,
            double localStartMeters,
            double localEndMeters,
            std::vector<double> samplePoints);

    static WorkoutGameWorldGroundMaterial materialAt(
            const WorkoutGameRoadCourse &course,
            double courseDistanceMeters);

    static WorkoutGameWorldGroundMaterial baseMaterialAt(
            const WorkoutGameRoadCourse &course,
            double courseDistanceMeters);
};

#endif
