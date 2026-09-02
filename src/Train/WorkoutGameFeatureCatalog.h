/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameFeatureCatalog_h
#define _GC_WorkoutGameFeatureCatalog_h

#include "WorkoutGameWorld.h"

enum class WorkoutGameTrailPropKind
{
    Pebble,
    Root,
    RollerMarker,
    ClimbMarker,
    Rock,
    Log,
    DropMarker,
    Plank,
    BermMarker,
    TabletopMarker,
    Slab
};

struct WorkoutGameTerrainDefinition
{
    WorkoutGameTrailPropKind prop = WorkoutGameTrailPropKind::Pebble;
    double propSpacingMeters = 7.5;
    double trailWidthScale = 1.0;
    bool technical = false;
    bool jumpable = false;
};

class WorkoutGameFeatureCatalog
{
public:
    static constexpr WorkoutGameTerrainDefinition definition(
            WorkoutGameTerrainKind terrain)
    {
        switch (terrain) {
        case WorkoutGameTerrainKind::SmoothTrail:
            return {WorkoutGameTrailPropKind::Pebble, 7.5, 1.0, false, false};
        case WorkoutGameTerrainKind::Roots:
            return {WorkoutGameTrailPropKind::Root, 3.6, 1.0, true, false};
        case WorkoutGameTerrainKind::Rollers:
            return {WorkoutGameTrailPropKind::RollerMarker, 6.0, 1.0, true, false};
        case WorkoutGameTerrainKind::Climb:
            return {WorkoutGameTrailPropKind::ClimbMarker, 7.0, 1.0, false, false};
        case WorkoutGameTerrainKind::RockGarden:
            return {WorkoutGameTrailPropKind::Rock, 4.1, 1.09, true, false};
        case WorkoutGameTerrainKind::BunnyHop:
            return {WorkoutGameTrailPropKind::Log, 11.0, 1.0, true, true};
        case WorkoutGameTerrainKind::Drop:
            return {WorkoutGameTrailPropKind::DropMarker, 13.0, 1.0, false, false};
        case WorkoutGameTerrainKind::Skinny:
            return {WorkoutGameTrailPropKind::Plank, 5.0, 0.72, true, false};
        case WorkoutGameTerrainKind::Berm:
            return {WorkoutGameTrailPropKind::BermMarker, 9.0, 1.17, true, false};
        case WorkoutGameTerrainKind::LogOver:
            return {WorkoutGameTrailPropKind::Log, 12.0, 1.0, true, true};
        case WorkoutGameTerrainKind::Tabletop:
            return {WorkoutGameTrailPropKind::TabletopMarker, 16.0, 1.08, true, true};
        case WorkoutGameTerrainKind::RockSlab:
            return {WorkoutGameTrailPropKind::Slab, 8.0, 1.04, true, false};
        case WorkoutGameTerrainKind::GapJump:
            return {WorkoutGameTrailPropKind::TabletopMarker, 18.0, 1.08, true, true};
        }
        return {};
    }
};

#endif
