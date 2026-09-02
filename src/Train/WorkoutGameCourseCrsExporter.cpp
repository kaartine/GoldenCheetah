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

#include <QString>

#include <cmath>

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

QString segmentLine(const WorkoutGameDistanceCourseSection &section)
{
    return QStringLiteral("%1 %2 0.0\n")
            .arg(section.lengthMeters / 1000.0, 0, 'f', 6)
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

    for (std::size_t index = 0;
            index < document.course.sections.size();
            ++index) {
        const WorkoutGameDistanceCourseSection &section =
                document.course.sections[index];
        if (index > 0) {
            output += QStringLiteral("LAP %1\n").arg(
                    sectionLabel(section));
        }
        output += segmentLine(section);
    }

    output += QStringLiteral("[END COURSE DATA]\n[COURSE TEXT]\n");
    for (const WorkoutGameDistanceCourseSection &section :
            document.course.sections) {
        output += cueLine(section);
    }
    output += QStringLiteral("[END COURSE TEXT]\n");
    return output.toUtf8();
}
