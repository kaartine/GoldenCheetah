/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameRoadQuality.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double Epsilon = 1.0e-9;

struct Exemption
{
    double start = 0.0;
    double end = 0.0;
};

struct Bend
{
    double ordinaryDistance = 0.0;
    double turnDegrees = 0.0;
};

void addOnce(
        WorkoutGameRoadQualityReport &report,
        WorkoutGameRoadQualityViolation violation)
{
    if (!report.contains(violation)) report.violations.push_back(violation);
}

std::vector<Exemption> exemptions(const WorkoutGameRoadPlan &plan)
{
    std::vector<Exemption> result;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        if (piece.terrain == WorkoutGameTerrainKind::Berm
                && !piece.challenge.enabled) {
            result.push_back({piece.startDistanceMeters,
                              piece.startDistanceMeters
                                + piece.lengthMeters});
        } else if (piece.challenge.enabled && piece.qualityExempt) {
            result.push_back({piece.qualityExemptionStartDistanceMeters,
                              piece.qualityExemptionEndDistanceMeters});
        }
    }
    std::sort(result.begin(), result.end(), [](const Exemption &left,
                                               const Exemption &right) {
        return left.start < right.start;
    });
    std::vector<Exemption> merged;
    for (const Exemption &candidate : result) {
        if (merged.empty() || candidate.start > merged.back().end + Epsilon) {
            merged.push_back(candidate);
        } else {
            merged.back().end = std::max(merged.back().end, candidate.end);
        }
    }
    return merged;
}

bool exemptAt(double distance, const std::vector<Exemption> &zones)
{
    for (const Exemption &zone : zones) {
        if (distance < zone.start) return false;
        if (distance <= zone.end) return true;
    }
    return false;
}

double ordinaryDistanceAt(
        double distance,
        const std::vector<Exemption> &zones)
{
    double removed = 0.0;
    for (const Exemption &zone : zones) {
        if (distance <= zone.start) break;
        removed += std::max(0.0,
                std::min(distance, zone.end) - zone.start);
    }
    return distance - removed;
}

}

bool WorkoutGameRoadQualityReport::contains(
        WorkoutGameRoadQualityViolation violation) const
{
    return std::find(violations.begin(), violations.end(), violation)
            != violations.end();
}

WorkoutGameRoadQualityReport WorkoutGameRoadQuality::audit(
        const WorkoutGameRoadPlan &plan)
{
    WorkoutGameRoadQualityReport report;
    if (plan.pieces.empty()) {
        addOnce(report, WorkoutGameRoadQualityViolation::NearStraightTooLong);
        return report;
    }

    const std::vector<Exemption> zones = exemptions(plan);
    const double routeEnd = plan.pieces.back().startDistanceMeters
            + plan.pieces.back().lengthMeters;
    const double ordinaryEnd = ordinaryDistanceAt(routeEnd, zones);
    const double deliberateRadians = DeliberateBendDegrees * Pi / 180.0;
    const double nearNinetyRadians = NearNinetyMinimumDegrees * Pi / 180.0;
    const double maximumTurnRadians = MaximumTurnDegrees * Pi / 180.0;
    std::vector<Bend> bends;
    std::vector<double> nearNinety;

    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        const double magnitude = std::abs(piece.turnRadians);
        if (piece.terrain != WorkoutGameTerrainKind::Berm
                && magnitude > maximumTurnRadians + Epsilon) {
            addOnce(report, WorkoutGameRoadQualityViolation::TurnExceedsBound);
        }
        const double center = piece.startDistanceMeters
                + piece.lengthMeters * 0.5;
        if (exemptAt(center, zones) || magnitude + Epsilon < deliberateRadians) {
            continue;
        }
        const double ordinaryCenter = ordinaryDistanceAt(center, zones);
        bends.push_back({ordinaryCenter, piece.turnRadians * 180.0 / Pi});
        if (magnitude + Epsilon >= nearNinetyRadians) {
            nearNinety.push_back(ordinaryCenter);
        }
    }

    double straightRunMeters = 0.0;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        const double center = piece.startDistanceMeters
                + piece.lengthMeters * 0.5;
        if (exemptAt(center, zones)) continue;
        const double turnDegrees = std::abs(piece.turnRadians) * 180.0 / Pi;
        if (turnDegrees + Epsilon < DeliberateBendDegrees) {
            straightRunMeters += piece.lengthMeters;
        } else {
            const double distanceToDeliberateHeadingChange =
                    piece.lengthMeters * DeliberateBendDegrees / turnDegrees;
            straightRunMeters += distanceToDeliberateHeadingChange;
            if (straightRunMeters
                    > MaximumNearStraightMeters + Epsilon) {
                addOnce(report,
                        WorkoutGameRoadQualityViolation::NearStraightTooLong);
            }
            straightRunMeters = 0.0;
        }
        if (straightRunMeters > MaximumNearStraightMeters + Epsilon) {
            addOnce(report,
                    WorkoutGameRoadQualityViolation::NearStraightTooLong);
        }
    }

    if (ordinaryEnd + Epsilon >= RollingWindowMeters) {
        std::vector<double> starts {0.0, ordinaryEnd - RollingWindowMeters};
        for (const Bend &bend : bends) {
            starts.push_back(std::clamp(
                    bend.ordinaryDistance + Epsilon,
                    0.0, ordinaryEnd - RollingWindowMeters));
        }
        for (double start : starts) {
            const double end = start + RollingWindowMeters;
            std::vector<const Bend *> inside;
            for (const Bend &bend : bends) {
                if (bend.ordinaryDistance + Epsilon >= start
                        && bend.ordinaryDistance <= end + Epsilon) {
                    inside.push_back(&bend);
                }
            }
            if (inside.size() < std::size_t(MinimumAlternatingBends)) {
                addOnce(report,
                        WorkoutGameRoadQualityViolation::RollingWindowTooFewBends);
            }
            double accumulated = 0.0;
            bool alternating = true;
            for (std::size_t index = 0; index < inside.size(); ++index) {
                accumulated += std::abs(inside[index]->turnDegrees);
                if (index > 0 && inside[index - 1]->turnDegrees
                        * inside[index]->turnDegrees >= 0.0) {
                    alternating = false;
                }
            }
            if (!alternating) {
                addOnce(report,
                        WorkoutGameRoadQualityViolation::RollingWindowDoesNotAlternate);
            }
            if (accumulated + Epsilon < MinimumAccumulatedTurnDegrees) {
                addOnce(report,
                        WorkoutGameRoadQualityViolation::RollingWindowTooLittleTurn);
            }
        }
    }

    if (ordinaryEnd + Epsilon >= MaximumNearNinetySpacingMeters) {
        double previous = 0.0;
        for (double distance : nearNinety) {
            if (distance - previous
                    > MaximumNearNinetySpacingMeters + Epsilon) {
                addOnce(report,
                        WorkoutGameRoadQualityViolation::NearNinetyTurnMissing);
            }
            previous = distance;
        }
        if (ordinaryEnd - previous
                > MaximumNearNinetySpacingMeters + Epsilon) {
            addOnce(report,
                    WorkoutGameRoadQualityViolation::NearNinetyTurnMissing);
        }
    }
    return report;
}
