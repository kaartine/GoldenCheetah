/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCompetition.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace {

constexpr std::int64_t MaximumReplayDurationMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t RecordingIntervalMs = 5000;
constexpr double Pi = 3.14159265358979323846;
constexpr std::array<double, 3> AiEfficiency = {0.84, 0.97, 0.72};
constexpr std::array<int, 3> AiLane = {-1, 1, -2};
constexpr double VisualRiderSpacing = 0.03;
constexpr double MaximumVisualGap = 0.15;

template<typename Integer>
bool parseInteger(std::string_view input, Integer &value)
{
    if (input.empty()) return false;
    Integer parsed = 0;
    const char *begin = input.data();
    const char *end = begin + input.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) return false;
    value = parsed;
    return true;
}

std::uint32_t mix(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

bool splitOnce(
        std::string_view input,
        char separator,
        std::string_view &left,
        std::string_view &right)
{
    const std::size_t position = input.find(separator);
    if (position == std::string_view::npos) return false;
    left = input.substr(0, position);
    right = input.substr(position + 1);
    return true;
}

}

bool WorkoutGameGhostCodec::isValid(const WorkoutGameGhostReplay &replay)
{
    if (replay.courseSeed == 0
            || replay.durationMs <= 0
            || replay.durationMs > MaximumReplayDurationMs
            || replay.points.size() < 2
            || replay.points.size() > MaximumPoints) {
        return false;
    }

    std::int64_t priorTime = -1;
    std::uint64_t priorScore = 0;
    bool first = true;
    for (const WorkoutGameGhostPoint &point : replay.points) {
        if (point.timeMs < 0
                || point.timeMs > replay.durationMs
                || point.timeMs <= priorTime
                || (!first && point.score < priorScore)
                || (point.route != WorkoutGameRoute::MainLine
                    && point.route != WorkoutGameRoute::SafeBypass)) {
            return false;
        }
        priorTime = point.timeMs;
        priorScore = point.score;
        first = false;
    }
    return true;
}

std::string WorkoutGameGhostCodec::encode(
        const WorkoutGameGhostReplay &replay)
{
    if (!isValid(replay)) return {};

    std::string encoded = "WG1|" + std::to_string(replay.courseSeed)
            + '|' + std::to_string(replay.durationMs) + '|';
    for (std::size_t index = 0; index < replay.points.size(); ++index) {
        if (index > 0) encoded.push_back(';');
        const WorkoutGameGhostPoint &point = replay.points[index];
        encoded += std::to_string(point.timeMs);
        encoded.push_back(',');
        encoded += std::to_string(point.score);
        encoded.push_back(',');
        encoded.push_back(point.route == WorkoutGameRoute::SafeBypass
                ? '1' : '0');
        if (encoded.size() > MaximumEncodedBytes) return {};
    }
    return encoded;
}

bool WorkoutGameGhostCodec::decode(
        const std::string &encoded,
        WorkoutGameGhostReplay &replay)
{
    replay = WorkoutGameGhostReplay();
    if (encoded.empty() || encoded.size() > MaximumEncodedBytes) return false;

    std::string_view input(encoded);
    std::string_view version;
    std::string_view remaining;
    if (!splitOnce(input, '|', version, remaining) || version != "WG1") {
        return false;
    }
    std::string_view seedText;
    if (!splitOnce(remaining, '|', seedText, remaining)
            || !parseInteger(seedText, replay.courseSeed)) {
        replay = WorkoutGameGhostReplay();
        return false;
    }
    std::string_view durationText;
    if (!splitOnce(remaining, '|', durationText, remaining)
            || !parseInteger(durationText, replay.durationMs)
            || remaining.empty()) {
        replay = WorkoutGameGhostReplay();
        return false;
    }

    while (!remaining.empty()) {
        if (replay.points.size() >= MaximumPoints) {
            replay = WorkoutGameGhostReplay();
            return false;
        }
        const std::size_t separator = remaining.find(';');
        const std::string_view record = remaining.substr(0, separator);
        if (record.empty()) {
            replay = WorkoutGameGhostReplay();
            return false;
        }

        std::string_view timeText;
        std::string_view fields;
        std::string_view scoreText;
        std::string_view routeText;
        if (!splitOnce(record, ',', timeText, fields)
                || !splitOnce(fields, ',', scoreText, routeText)
                || routeText.size() != 1) {
            replay = WorkoutGameGhostReplay();
            return false;
        }
        WorkoutGameGhostPoint point;
        unsigned int route = 0;
        if (!parseInteger(timeText, point.timeMs)
                || !parseInteger(scoreText, point.score)
                || !parseInteger(routeText, route)
                || route > 1u) {
            replay = WorkoutGameGhostReplay();
            return false;
        }
        point.route = route == 1u
                ? WorkoutGameRoute::SafeBypass
                : WorkoutGameRoute::MainLine;
        replay.points.push_back(point);

        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1);
        if (remaining.empty()) {
            replay = WorkoutGameGhostReplay();
            return false;
        }
    }

    if (!isValid(replay)) {
        replay = WorkoutGameGhostReplay();
        return false;
    }
    return true;
}

bool WorkoutGameGhostCodec::isBetter(
        const WorkoutGameGhostReplay &candidate,
        const WorkoutGameGhostReplay &current)
{
    if (!isValid(candidate)) return false;
    if (!isValid(current)) return true;
    if (candidate.courseSeed != current.courseSeed
            || candidate.durationMs != current.durationMs) {
        return false;
    }

    const WorkoutGameGhostPoint &candidateLast = candidate.points.back();
    const WorkoutGameGhostPoint &currentLast = current.points.back();
    if (candidateLast.timeMs != currentLast.timeMs) {
        return candidateLast.timeMs > currentLast.timeMs;
    }
    return candidateLast.score > currentLast.score;
}

bool WorkoutGameGhostRecorder::configure(
        std::uint32_t courseSeed,
        std::int64_t durationMs)
{
    reset();
    if (courseSeed == 0
            || durationMs <= 0
            || durationMs > MaximumReplayDurationMs) {
        return false;
    }
    recorded.courseSeed = courseSeed;
    recorded.durationMs = durationMs;
    return true;
}

void WorkoutGameGhostRecorder::reset()
{
    recorded = WorkoutGameGhostReplay();
}

void WorkoutGameGhostRecorder::record(
        const WorkoutGameSimulationSnapshot &snapshot)
{
    if (recorded.courseSeed == 0
            || !snapshot.ready
            || snapshot.workoutTimeMs < 0
            || snapshot.workoutTimeMs > recorded.durationMs
            || recorded.points.size() >= WorkoutGameGhostCodec::MaximumPoints) {
        return;
    }

    WorkoutGameGhostPoint point;
    point.timeMs = snapshot.workoutTimeMs;
    point.score = snapshot.score;
    point.route = snapshot.route;
    if (recorded.points.empty()) {
        recorded.points.push_back(point);
        return;
    }

    const WorkoutGameGhostPoint &last = recorded.points.back();
    if (point.timeMs < last.timeMs || point.score < last.score) return;
    if (point.timeMs == last.timeMs) {
        recorded.points.back() = point;
        return;
    }
    const bool routeChanged = point.route != last.route;
    const bool intervalReached = point.timeMs - last.timeMs >= RecordingIntervalMs;
    if (routeChanged || intervalReached || snapshot.finished) {
        recorded.points.push_back(point);
    }
}

bool WorkoutGameCompetition::configure(
        const WorkoutGameCourse &course,
        const WorkoutGameGhostReplay &ghostReplay)
{
    configuredCourse = WorkoutGameCourse();
    configuredGhost = WorkoutGameGhostReplay();
    if (course.status != WorkoutGameCourseStatus::Ready
            || course.seed == 0
            || course.durationMs <= 0
            || course.sections.empty()) {
        return false;
    }

    configuredCourse = course;
    if (WorkoutGameGhostCodec::isValid(ghostReplay)
            && ghostReplay.courseSeed == course.seed
            && ghostReplay.durationMs == course.durationMs) {
        configuredGhost = ghostReplay;
    }
    return true;
}

std::uint64_t WorkoutGameCompetition::aiScore(
        int identity,
        std::int64_t workoutTimeMs) const
{
    if (identity < 0 || identity >= int(AiEfficiency.size())) return 0;
    const double seconds = std::max(0.0, double(workoutTimeMs) / 1000.0);
    const std::uint32_t random = mix(
            configuredCourse.seed ^ (0x9e3779b9u * std::uint32_t(identity + 1)));
    const double phase = 2.0 * Pi * double(random & 0xffffu) / 65536.0;
    const double omega = 2.0 * Pi / (43.0 + 7.0 * identity);
    const double amplitude = 0.08;
    const double positiveIntegral = seconds
            + amplitude * (std::cos(phase)
                    - std::cos(omega * seconds + phase)) / omega;
    const double score = 100.0 * AiEfficiency[std::size_t(identity)]
            * std::max(0.0, positiveIntegral);
    if (!std::isfinite(score) || score <= 0.0) return 0;
    return std::uint64_t(std::floor(std::min(
            score, double(std::numeric_limits<std::uint64_t>::max()))));
}

bool WorkoutGameCompetition::ghostAt(
        std::int64_t workoutTimeMs,
        WorkoutGameGhostPoint &point) const
{
    if (!WorkoutGameGhostCodec::isValid(configuredGhost)
            || workoutTimeMs < 0
            || workoutTimeMs > configuredCourse.durationMs) {
        return false;
    }

    if (workoutTimeMs <= configuredGhost.points.front().timeMs) {
        point = configuredGhost.points.front();
        return true;
    }
    if (workoutTimeMs >= configuredGhost.points.back().timeMs) {
        point = configuredGhost.points.back();
        return true;
    }

    const auto upper = std::upper_bound(
            configuredGhost.points.begin(), configuredGhost.points.end(),
            workoutTimeMs,
            [](std::int64_t time, const WorkoutGameGhostPoint &candidate) {
                return time < candidate.timeMs;
            });
    if (upper == configuredGhost.points.begin()) {
        point = *upper;
        return true;
    }
    if (upper == configuredGhost.points.end()) {
        point = configuredGhost.points.back();
        return true;
    }

    const WorkoutGameGhostPoint &before = *(upper - 1);
    const WorkoutGameGhostPoint &after = *upper;
    const double fraction = double(workoutTimeMs - before.timeMs)
            / double(after.timeMs - before.timeMs);
    point.timeMs = workoutTimeMs;
    point.score = before.score + std::uint64_t(std::llround(
            fraction * double(after.score - before.score)));
    point.route = before.route;
    return true;
}

void WorkoutGameCompetition::assignVisualProgress(
        const WorkoutGameSimulationSnapshot &player,
        std::vector<WorkoutGameCompetitorSnapshot> &competitors) const
{
    std::vector<std::size_t> ahead;
    std::vector<std::size_t> behind;
    for (std::size_t index = 0; index < competitors.size(); ++index) {
        if (competitors[index].score > player.score) ahead.push_back(index);
        else if (competitors[index].score < player.score) behind.push_back(index);
        else {
            competitors[index].relativeProgress = std::clamp(
                    competitors[index].lane * VisualRiderSpacing,
                    -MaximumVisualGap, MaximumVisualGap);
        }
    }

    std::sort(ahead.begin(), ahead.end(), [&](std::size_t left, std::size_t right) {
        return competitors[left].score < competitors[right].score;
    });
    std::sort(behind.begin(), behind.end(), [&](std::size_t left, std::size_t right) {
        return competitors[left].score > competitors[right].score;
    });

    for (std::size_t rank = 0; rank < ahead.size(); ++rank) {
        competitors[ahead[rank]].relativeProgress = std::min(
                MaximumVisualGap, VisualRiderSpacing * double(rank + 1));
    }
    for (std::size_t rank = 0; rank < behind.size(); ++rank) {
        competitors[behind[rank]].relativeProgress = -std::min(
                MaximumVisualGap, VisualRiderSpacing * double(rank + 1));
    }
    for (WorkoutGameCompetitorSnapshot &rider : competitors) {
        rider.courseProgress = std::clamp(
                player.courseProgress + rider.relativeProgress, 0.0, 1.0);
    }
}

WorkoutGameCompetitionSnapshot WorkoutGameCompetition::update(
        const WorkoutGameSimulationSnapshot &player) const
{
    WorkoutGameCompetitionSnapshot result;
    if (configuredCourse.status != WorkoutGameCourseStatus::Ready
            || !player.ready
            || player.workoutTimeMs < 0
            || player.workoutTimeMs > configuredCourse.durationMs
            || !std::isfinite(player.courseProgress)
            || player.courseProgress < 0.0
            || player.courseProgress > 1.0) {
        return result;
    }

    result.ready = true;
    result.competitors.reserve(4);
    for (int identity = 0; identity < int(AiEfficiency.size()); ++identity) {
        WorkoutGameCompetitorSnapshot rider;
        rider.kind = WorkoutGameCompetitorKind::Ai;
        rider.identity = identity;
        rider.lane = AiLane[std::size_t(identity)];
        rider.score = aiScore(identity, player.workoutTimeMs);
        result.competitors.push_back(rider);
    }

    WorkoutGameGhostPoint ghostPoint;
    if (ghostAt(player.workoutTimeMs, ghostPoint)) {
        WorkoutGameCompetitorSnapshot rider;
        rider.kind = WorkoutGameCompetitorKind::Ghost;
        rider.identity = 0;
        rider.lane = 2;
        rider.score = ghostPoint.score;
        rider.route = ghostPoint.route;
        result.competitors.push_back(rider);
    }

    assignVisualProgress(player, result.competitors);

    result.playerRank = 1;
    for (const WorkoutGameCompetitorSnapshot &rider : result.competitors) {
        if (rider.score > player.score) ++result.playerRank;
    }
    result.totalRiders = int(result.competitors.size()) + 1;
    return result;
}
