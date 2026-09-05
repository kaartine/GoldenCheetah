/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseCrsExporter.h"

#include "WorkoutGameCourseDocument.h"
#include "WorkoutGameDistancePlayback.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

QString featureLabel(WorkoutGameFeature feature)
{
    switch (feature) {
    case WorkoutGameFeature::WarmupTrail: return QStringLiteral("Warmup trail");
    case WorkoutGameFeature::Trail: return QStringLiteral("Trail");
    case WorkoutGameFeature::FlowTrail: return QStringLiteral("Flow trail");
    case WorkoutGameFeature::Climb: return QStringLiteral("Climb");
    case WorkoutGameFeature::SprintJump: return QStringLiteral("Sprint jump");
    case WorkoutGameFeature::RecoveryDescent: return QStringLiteral("Recovery descent");
    case WorkoutGameFeature::CooldownDescent: return QStringLiteral("Cooldown descent");
    }
    return {};
}

QString sectionLabel(const WorkoutGameDistanceCourseSection &section)
{
    return section.terrain == WorkoutGameTerrainKind::GapJump
            ? QStringLiteral("Gap jump")
            : featureLabel(section.feature);
}

QString segmentLine(
        double lengthMeters,
        const WorkoutGameDistanceCourseSection &section)
{
    return QStringLiteral("%1 %2 0.0\n")
            .arg(lengthMeters / 1000.0, 0, 'f', 6)
            .arg(section.gradePercent, 0, 'f', 3);
}

QString cueLine(const WorkoutGameDistanceCourseSection &section)
{
    const int targetWatts = int(std::lround(
            (section.targetStartWatts + section.targetEndWatts) * 0.5));
    return QStringLiteral("%1 Target %2 W - %3 8\n")
            .arg(section.startDistanceMeters / 1000.0, 0, 'f', 6)
            .arg(targetWatts)
            .arg(sectionLabel(section));
}

struct DistanceLap
{
    double distanceMeters = 0.0;
    QString name;
};

std::vector<DistanceLap> mappedSourceLaps(
        const WorkoutGameCourseDocument &document)
{
    std::vector<DistanceLap> result;
    result.reserve(document.sourceLaps.size());
    for (const WorkoutGameCourseSourceLap &lap : document.sourceLaps) {
        result.push_back({WorkoutGameDistancePlayback::distanceAtNominalTime(
                              document.course, lap.timeMs),
                          lap.name.simplified()});
    }
    std::stable_sort(result.begin(), result.end(),
        [](const DistanceLap &left, const DistanceLap &right) {
            return left.distanceMeters < right.distanceMeters;
        });
    return result;
}

}

QByteArray WorkoutGameCourseCrsExporter::encode(
        const WorkoutGameCourseDocument &document)
{
    if (!WorkoutGameCourseDocumentCodec::valid(document)) return {};

    QString output;
    output.reserve(512 + int(document.course.sections.size()) * 96);
    output += QStringLiteral(
            "[COURSE HEADER]\n"
            "VERSION=1\n"
            "UNITS=METRIC\n"
            "SOURCE=GoldenCheetah MTB\n"
            "DESCRIPTION=%1\n"
            "FILE NAME=%2\n"
            "DISTANCE GRADE WIND\n"
            "[END COURSE HEADER]\n"
            "[COURSE DATA]\n")
            .arg(document.title, document.sourceFileName);

    const std::vector<DistanceLap> sourceLaps = mappedSourceLaps(document);
    std::size_t nextSourceLap = 0;
    constexpr double DistanceEpsilon = 1.0e-6;
    for (std::size_t index = 0; index < document.course.sections.size(); ++index) {
        const WorkoutGameDistanceCourseSection &section =
                document.course.sections[index];
        if (sourceLaps.empty() && index > 0) {
            output += QStringLiteral("LAP %1\n").arg(
                    sectionLabel(section));
        }
        double cursor = section.startDistanceMeters;
        const double sectionEnd = cursor + section.lengthMeters;
        while (nextSourceLap < sourceLaps.size()
                && sourceLaps[nextSourceLap].distanceMeters
                    <= sectionEnd + DistanceEpsilon) {
            const DistanceLap &lap = sourceLaps[nextSourceLap];
            if (lap.distanceMeters + DistanceEpsilon < cursor) {
                ++nextSourceLap;
                continue;
            }
            const double beforeLap = std::clamp(
                    lap.distanceMeters - cursor, 0.0, sectionEnd - cursor);
            if (beforeLap > DistanceEpsilon) {
                output += segmentLine(beforeLap, section);
                cursor += beforeLap;
            }
            output += QStringLiteral("LAP %1\n").arg(lap.name);
            ++nextSourceLap;
        }
        const double remaining = sectionEnd - cursor;
        if (remaining > DistanceEpsilon) {
            output += segmentLine(remaining, section);
        }
    }

    output += QStringLiteral("[END COURSE DATA]\n[COURSE TEXT]\n");
    for (const WorkoutGameDistanceCourseSection &section :
            document.course.sections) {
        output += cueLine(section);
    }
    for (const WorkoutGameCourseSourceText &text : document.sourceTexts) {
        output += QStringLiteral("%1 %2 %3\n")
                .arg(WorkoutGameDistancePlayback::distanceAtNominalTime(
                         document.course, text.timeMs) / 1000.0,
                     0, 'f', 6)
                .arg(text.text.simplified())
                .arg(text.durationSeconds);
    }
    output += QStringLiteral("[END COURSE TEXT]\n");
    return output.toUtf8();
}
