/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameChallengeGeometry_h
#define _GC_WorkoutGameChallengeGeometry_h

#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"

#include <algorithm>
#include <utility>

inline std::pair<double, double> workoutGameChallengeGeometrySpan(
        const WorkoutGameRoadPiece &piece)
{
    switch (piece.terrain) {
    case WorkoutGameTerrainKind::Roots: {
        const auto profile = WorkoutGameRootGeometry::profile(piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::RockGarden: {
        const auto profile = WorkoutGameRockGardenGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::RockSlab: {
        const auto profile = WorkoutGameRockSlabGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Skinny: {
        const auto profile = WorkoutGameSkinnyGeometry::profile(
                piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Climb: {
        const auto profile = WorkoutGameClimbGeometry::profile(piece.difficulty);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::Berm: {
        const auto profile = WorkoutGameBermGeometry::profile(piece);
        return {profile.startMeters, profile.endMeters};
    }
    case WorkoutGameTerrainKind::GapJump:
        if (piece.gapJump.enabled) {
            return {
                piece.gapJump.splitStartDistanceMeters
                    - piece.challenge.obstacleDistanceMeters,
                piece.gapJump.mergeEndDistanceMeters
                    - piece.challenge.obstacleDistanceMeters
            };
        }
        return {0.0, 0.0};
    default: {
        const auto profile = WorkoutGameFeatureGeometry::profile(
                piece.terrain, piece.difficulty);
        return profile.ready
                ? std::pair<double, double>{
                    profile.startMeters, profile.endMeters}
                : std::pair<double, double>{0.0, 0.0};
    }
    }
}

inline std::pair<double, double> workoutGameChallengeProtectedSpan(
        const WorkoutGameRoadPiece &piece)
{
    const auto [featureStart, featureEnd] =
            workoutGameChallengeGeometrySpan(piece);
    const double obstacle = piece.challenge.obstacleDistanceMeters;
    return {
        std::min({piece.challenge.prepareDistanceMeters,
                  piece.challenge.bypassStartDistanceMeters,
                  obstacle + featureStart}),
        std::max({piece.challenge.bypassEndDistanceMeters,
                  obstacle,
                  obstacle + featureEnd})
    };
}

#endif
