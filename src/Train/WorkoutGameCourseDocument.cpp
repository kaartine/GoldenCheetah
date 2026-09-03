/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseDocument.h"

#include "WorkoutGameCourseCrsExporter.h"
#include "WorkoutGameDistancePlayback.h"
#include "WorkoutGameRoadCourse.h"
#include "WorkoutGameRoadPlan.h"
#include "WorkoutGameRoadQuality.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace {

QString presetName(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        return QStringLiteral("workout-first");
    case WorkoutGameCoursePreset::Balanced:
        return QStringLiteral("balanced");
    case WorkoutGameCoursePreset::RideFirst:
        return QStringLiteral("ride-first");
    }
    return {};
}

bool parsePreset(const QString &name, WorkoutGameCoursePreset &preset)
{
    if (name == QStringLiteral("workout-first")) {
        preset = WorkoutGameCoursePreset::WorkoutFirst;
        return true;
    }
    if (name == QStringLiteral("balanced")) {
        preset = WorkoutGameCoursePreset::Balanced;
        return true;
    }
    if (name == QStringLiteral("ride-first")) {
        preset = WorkoutGameCoursePreset::RideFirst;
        return true;
    }
    return false;
}

QString featureName(WorkoutGameFeature feature)
{
    switch (feature) {
    case WorkoutGameFeature::WarmupTrail: return QStringLiteral("warmup-trail");
    case WorkoutGameFeature::Trail: return QStringLiteral("trail");
    case WorkoutGameFeature::FlowTrail: return QStringLiteral("flow-trail");
    case WorkoutGameFeature::Climb: return QStringLiteral("climb");
    case WorkoutGameFeature::SprintJump: return QStringLiteral("sprint-jump");
    case WorkoutGameFeature::RecoveryDescent: return QStringLiteral("recovery-descent");
    case WorkoutGameFeature::CooldownDescent: return QStringLiteral("cooldown-descent");
    }
    return {};
}

bool parseFeature(const QString &name, WorkoutGameFeature &feature)
{
    const std::pair<const char *, WorkoutGameFeature> values[] = {
        {"warmup-trail", WorkoutGameFeature::WarmupTrail},
        {"trail", WorkoutGameFeature::Trail},
        {"flow-trail", WorkoutGameFeature::FlowTrail},
        {"climb", WorkoutGameFeature::Climb},
        {"sprint-jump", WorkoutGameFeature::SprintJump},
        {"recovery-descent", WorkoutGameFeature::RecoveryDescent},
        {"cooldown-descent", WorkoutGameFeature::CooldownDescent}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            feature = value.second;
            return true;
        }
    }
    return false;
}

QString terrainName(WorkoutGameTerrainKind terrain)
{
    switch (terrain) {
    case WorkoutGameTerrainKind::SmoothTrail: return QStringLiteral("smooth-trail");
    case WorkoutGameTerrainKind::Roots: return QStringLiteral("roots");
    case WorkoutGameTerrainKind::RockGarden: return QStringLiteral("rock-garden");
    case WorkoutGameTerrainKind::Rollers: return QStringLiteral("rollers");
    case WorkoutGameTerrainKind::Climb: return QStringLiteral("climb");
    case WorkoutGameTerrainKind::BunnyHop: return QStringLiteral("bunny-hop");
    case WorkoutGameTerrainKind::Drop: return QStringLiteral("drop");
    case WorkoutGameTerrainKind::Berm: return QStringLiteral("berm");
    case WorkoutGameTerrainKind::Skinny: return QStringLiteral("skinny");
    case WorkoutGameTerrainKind::LogOver: return QStringLiteral("log-over");
    case WorkoutGameTerrainKind::Tabletop: return QStringLiteral("tabletop");
    case WorkoutGameTerrainKind::RockSlab: return QStringLiteral("rock-slab");
    case WorkoutGameTerrainKind::GapJump: return QStringLiteral("gap-jump");
    }
    return {};
}

bool parseTerrain(const QString &name, WorkoutGameTerrainKind &terrain)
{
    const std::pair<const char *, WorkoutGameTerrainKind> values[] = {
        {"smooth-trail", WorkoutGameTerrainKind::SmoothTrail},
        {"roots", WorkoutGameTerrainKind::Roots},
        {"rock-garden", WorkoutGameTerrainKind::RockGarden},
        {"rollers", WorkoutGameTerrainKind::Rollers},
        {"climb", WorkoutGameTerrainKind::Climb},
        {"bunny-hop", WorkoutGameTerrainKind::BunnyHop},
        {"drop", WorkoutGameTerrainKind::Drop},
        {"berm", WorkoutGameTerrainKind::Berm},
        {"skinny", WorkoutGameTerrainKind::Skinny},
        {"log-over", WorkoutGameTerrainKind::LogOver},
        {"tabletop", WorkoutGameTerrainKind::Tabletop},
        {"rock-slab", WorkoutGameTerrainKind::RockSlab},
        {"gap-jump", WorkoutGameTerrainKind::GapJump}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            terrain = value.second;
            return true;
        }
    }
    return false;
}

QString animationName(WorkoutGameRoadAnimation animation)
{
    switch (animation) {
    case WorkoutGameRoadAnimation::None: return QStringLiteral("none");
    case WorkoutGameRoadAnimation::Absorb: return QStringLiteral("absorb");
    case WorkoutGameRoadAnimation::Pump: return QStringLiteral("pump");
    case WorkoutGameRoadAnimation::Climb: return QStringLiteral("climb");
    case WorkoutGameRoadAnimation::Jump: return QStringLiteral("jump");
    case WorkoutGameRoadAnimation::Drop: return QStringLiteral("drop");
    case WorkoutGameRoadAnimation::Balance: return QStringLiteral("balance");
    case WorkoutGameRoadAnimation::LeanLeft: return QStringLiteral("lean-left");
    case WorkoutGameRoadAnimation::LeanRight: return QStringLiteral("lean-right");
    }
    return {};
}

bool parseAnimation(const QString &name, WorkoutGameRoadAnimation &animation)
{
    const std::pair<const char *, WorkoutGameRoadAnimation> values[] = {
        {"none", WorkoutGameRoadAnimation::None},
        {"absorb", WorkoutGameRoadAnimation::Absorb},
        {"pump", WorkoutGameRoadAnimation::Pump},
        {"climb", WorkoutGameRoadAnimation::Climb},
        {"jump", WorkoutGameRoadAnimation::Jump},
        {"drop", WorkoutGameRoadAnimation::Drop},
        {"balance", WorkoutGameRoadAnimation::Balance},
        {"lean-left", WorkoutGameRoadAnimation::LeanLeft},
        {"lean-right", WorkoutGameRoadAnimation::LeanRight}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            animation = value.second;
            return true;
        }
    }
    return false;
}

QString challengeCueName(WorkoutGameChallengeCue cue)
{
    switch (cue) {
    case WorkoutGameChallengeCue::None: return QStringLiteral("none");
    case WorkoutGameChallengeCue::CarrySpeed: return QStringLiteral("carry-speed");
    case WorkoutGameChallengeCue::Jump: return QStringLiteral("jump");
    case WorkoutGameChallengeCue::HoldLine: return QStringLiteral("hold-line");
    case WorkoutGameChallengeCue::Climb: return QStringLiteral("climb");
    }
    return {};
}

bool parseChallengeCue(const QString &name, WorkoutGameChallengeCue &cue)
{
    const std::pair<const char *, WorkoutGameChallengeCue> values[] = {
        {"none", WorkoutGameChallengeCue::None},
        {"carry-speed", WorkoutGameChallengeCue::CarrySpeed},
        {"jump", WorkoutGameChallengeCue::Jump},
        {"hold-line", WorkoutGameChallengeCue::HoldLine},
        {"climb", WorkoutGameChallengeCue::Climb}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            cue = value.second;
            return true;
        }
    }
    return false;
}

QString gapLineName(WorkoutGameGapJumpLine line)
{
    switch (line) {
    case WorkoutGameGapJumpLine::None: return QStringLiteral("none");
    case WorkoutGameGapJumpLine::Short: return QStringLiteral("short");
    case WorkoutGameGapJumpLine::Medium: return QStringLiteral("medium");
    case WorkoutGameGapJumpLine::Long: return QStringLiteral("long");
    }
    return {};
}

bool parseGapLine(const QString &name, WorkoutGameGapJumpLine &line)
{
    if (name == QStringLiteral("short")) {
        line = WorkoutGameGapJumpLine::Short;
        return true;
    }
    if (name == QStringLiteral("medium")) {
        line = WorkoutGameGapJumpLine::Medium;
        return true;
    }
    if (name == QStringLiteral("long")) {
        line = WorkoutGameGapJumpLine::Long;
        return true;
    }
    return false;
}

QJsonObject physicsToJson(const WorkoutGameRoadPhysicsParameters &physics)
{
    return {
        {QStringLiteral("totalMassKg"), physics.totalMassKg},
        {QStringLiteral("dragAreaSquareMeters"), physics.dragAreaSquareMeters},
        {QStringLiteral("rollingResistanceCoefficient"), physics.rollingResistanceCoefficient},
        {QStringLiteral("airDensityKgPerCubicMeter"), physics.airDensityKgPerCubicMeter},
        {QStringLiteral("drivetrainEfficiency"), physics.drivetrainEfficiency},
        {QStringLiteral("rotatingMassFactor"), physics.rotatingMassFactor},
        {QStringLiteral("lowSpeedMetersPerSecond"), physics.lowSpeedMetersPerSecond},
        {QStringLiteral("maximumDriveForceNewtons"), physics.maximumDriveForceNewtons},
        {QStringLiteral("maximumBrakeForceNewtons"), physics.maximumBrakeForceNewtons},
        {QStringLiteral("maximumSpeedMetersPerSecond"), physics.maximumSpeedMetersPerSecond}
    };
}

bool finiteNumber(const QJsonObject &object, const char *key, double &value)
{
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isDouble()) return false;
    value = json.toDouble(std::numeric_limits<double>::quiet_NaN());
    return std::isfinite(value);
}

bool integerNumber(
        const QJsonObject &object,
        const char *key,
        std::int64_t &value)
{
    double number = 0.0;
    if (!finiteNumber(object, key, number)
            || std::floor(number) != number
            || number < double(std::numeric_limits<std::int64_t>::min())
            || number > double(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    value = std::int64_t(number);
    return true;
}

bool unsignedNumber(
        const QJsonObject &object,
        const char *key,
        std::uint32_t &value)
{
    std::int64_t number = 0;
    if (!integerNumber(object, key, number)
            || number < 0
            || std::uint64_t(number) > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = std::uint32_t(number);
    return true;
}

bool sizeNumber(
        const QJsonObject &object,
        const char *key,
        std::size_t &value)
{
    std::int64_t number = 0;
    if (!integerNumber(object, key, number) || number < 0
            || std::uint64_t(number)
                > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = std::size_t(number);
    return true;
}

bool safeUnsigned64Number(
        const QJsonObject &object,
        const char *key,
        std::uint64_t &value)
{
    constexpr double MaximumExactJsonInteger = 9007199254740991.0;
    double number = 0.0;
    if (!finiteNumber(object, key, number)
            || number < 0.0
            || number > MaximumExactJsonInteger
            || std::floor(number) != number) {
        return false;
    }
    value = std::uint64_t(number);
    return true;
}

bool parsePhysics(
        const QJsonObject &object,
        WorkoutGameRoadPhysicsParameters &physics)
{
    return finiteNumber(object, "totalMassKg", physics.totalMassKg)
            && finiteNumber(object, "dragAreaSquareMeters", physics.dragAreaSquareMeters)
            && finiteNumber(object, "rollingResistanceCoefficient", physics.rollingResistanceCoefficient)
            && finiteNumber(object, "airDensityKgPerCubicMeter", physics.airDensityKgPerCubicMeter)
            && finiteNumber(object, "drivetrainEfficiency", physics.drivetrainEfficiency)
            && finiteNumber(object, "rotatingMassFactor", physics.rotatingMassFactor)
            && finiteNumber(object, "lowSpeedMetersPerSecond", physics.lowSpeedMetersPerSecond)
            && finiteNumber(object, "maximumDriveForceNewtons", physics.maximumDriveForceNewtons)
            && finiteNumber(object, "maximumBrakeForceNewtons", physics.maximumBrakeForceNewtons)
            && finiteNumber(object, "maximumSpeedMetersPerSecond", physics.maximumSpeedMetersPerSecond);
}

QJsonObject generationToJson(
        const WorkoutGameDistanceCourseGenerationParameters &parameters)
{
    return {
        {QStringLiteral("physics"), physicsToJson(parameters.roadPhysics)},
        {QStringLiteral("recoveryIntensity"), parameters.recoveryIntensity},
        {QStringLiteral("shortClimbIntensity"), parameters.shortClimbIntensity},
        {QStringLiteral("gradeScale"), parameters.gradeScale},
        {QStringLiteral("technicality"), parameters.technicality},
        {QStringLiteral("workMinimumDurationScale"), parameters.workMinimumDurationScale},
        {QStringLiteral("workMaximumDurationScale"), parameters.workMaximumDurationScale},
        {QStringLiteral("recoveryMinimumDurationScale"), parameters.recoveryMinimumDurationScale},
        {QStringLiteral("recoveryMaximumDurationScale"), parameters.recoveryMaximumDurationScale},
        {QStringLiteral("shortClimbMaximumDurationMs"), double(parameters.shortClimbMaximumDurationMs)},
        {QStringLiteral("simulationStepMs"), double(parameters.simulationStepMs)},
        {QStringLiteral("maximumWorkoutDurationMs"), double(parameters.maximumWorkoutDurationMs)},
        {QStringLiteral("maximumSections"), double(parameters.maximumSections)}
    };
}

bool parseGeneration(
        const QJsonObject &object,
        WorkoutGameDistanceCourseGenerationParameters &parameters)
{
    parameters.technicality = 0.55;
    std::int64_t maximumSections = 0;
    if (!object.value(QStringLiteral("physics")).isObject()
            || !parsePhysics(
                object.value(QStringLiteral("physics")).toObject(),
                parameters.roadPhysics)
            || !finiteNumber(object, "recoveryIntensity", parameters.recoveryIntensity)
            || !finiteNumber(object, "shortClimbIntensity", parameters.shortClimbIntensity)
            || !finiteNumber(object, "gradeScale", parameters.gradeScale)
            || !finiteNumber(object, "workMinimumDurationScale", parameters.workMinimumDurationScale)
            || !finiteNumber(object, "workMaximumDurationScale", parameters.workMaximumDurationScale)
            || !finiteNumber(object, "recoveryMinimumDurationScale", parameters.recoveryMinimumDurationScale)
            || !finiteNumber(object, "recoveryMaximumDurationScale", parameters.recoveryMaximumDurationScale)
            || !integerNumber(object, "shortClimbMaximumDurationMs", parameters.shortClimbMaximumDurationMs)
            || !integerNumber(object, "simulationStepMs", parameters.simulationStepMs)
            || !integerNumber(object, "maximumWorkoutDurationMs", parameters.maximumWorkoutDurationMs)
            || !integerNumber(object, "maximumSections", maximumSections)
            || maximumSections < 0
            || std::uint64_t(maximumSections) > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    if (object.contains(QStringLiteral("technicality"))
            && !finiteNumber(object, "technicality", parameters.technicality)) {
        return false;
    }
    parameters.maximumSections = std::size_t(maximumSections);
    return true;
}

QJsonArray intervalsToJson(const std::vector<WorkoutGameInterval> &intervals)
{
    QJsonArray result;
    for (const WorkoutGameInterval &interval : intervals) {
        result.append(QJsonObject {
            {QStringLiteral("startMs"), double(interval.startMs)},
            {QStringLiteral("durationMs"), double(interval.durationMs)},
            {QStringLiteral("startWatts"), interval.startWatts},
            {QStringLiteral("endWatts"), interval.endWatts}
        });
    }
    return result;
}

bool parseIntervals(
        const QJsonArray &values,
        std::vector<WorkoutGameInterval> &intervals)
{
    if (values.size() > 10000) return false;
    intervals.clear();
    intervals.reserve(std::size_t(values.size()));
    std::int64_t expectedStart = 0;
    for (const QJsonValue &value : values) {
        if (!value.isObject()) return false;
        WorkoutGameInterval interval;
        const QJsonObject object = value.toObject();
        if (!integerNumber(object, "startMs", interval.startMs)
                || !integerNumber(object, "durationMs", interval.durationMs)
                || !finiteNumber(object, "startWatts", interval.startWatts)
                || !finiteNumber(object, "endWatts", interval.endWatts)
                || interval.startMs != expectedStart
                || interval.durationMs <= 0
                || interval.startWatts < 0.0
                || interval.endWatts < 0.0
                || interval.durationMs
                    > std::numeric_limits<std::int64_t>::max() - expectedStart) {
            return false;
        }
        expectedStart += interval.durationMs;
        intervals.push_back(interval);
    }
    return true;
}

bool validIntervals(
        const std::vector<WorkoutGameInterval> &intervals,
        std::int64_t nominalDurationMs)
{
    if (intervals.empty() || intervals.size() > 10000) return false;
    std::int64_t expectedStart = 0;
    for (const WorkoutGameInterval &interval : intervals) {
        if (interval.startMs != expectedStart
                || interval.durationMs <= 0
                || !std::isfinite(interval.startWatts)
                || !std::isfinite(interval.endWatts)
                || interval.startWatts < 0.0
                || interval.endWatts < 0.0
                || interval.durationMs
                    > std::numeric_limits<std::int64_t>::max() - expectedStart) {
            return false;
        }
        expectedStart += interval.durationMs;
    }
    return expectedStart == nominalDurationMs;
}

QJsonObject sectionToJson(const WorkoutGameDistanceCourseSection &section)
{
    return {
        {QStringLiteral("feature"), featureName(section.feature)},
        {QStringLiteral("terrain"), terrainName(section.terrain)},
        {QStringLiteral("sourceStartMs"), double(section.sourceStartMs)},
        {QStringLiteral("nominalDurationMs"), double(section.nominalDurationMs)},
        {QStringLiteral("minimumDurationMs"), double(section.minimumDurationMs)},
        {QStringLiteral("maximumDurationMs"), double(section.maximumDurationMs)},
        {QStringLiteral("startDistanceMeters"), section.startDistanceMeters},
        {QStringLiteral("lengthMeters"), section.lengthMeters},
        {QStringLiteral("startElevationMeters"), section.startElevationMeters},
        {QStringLiteral("endElevationMeters"), section.endElevationMeters},
        {QStringLiteral("targetStartWatts"), section.targetStartWatts},
        {QStringLiteral("targetEndWatts"), section.targetEndWatts},
        {QStringLiteral("gradePercent"), section.gradePercent},
        {QStringLiteral("difficulty"), section.difficulty},
        {QStringLiteral("visualVariant"), double(section.visualVariant)},
        {QStringLiteral("adjustableConnector"), section.adjustableConnector}
    };
}

bool parseSection(
        const QJsonObject &object,
        WorkoutGameDistanceCourseSection &section)
{
    const QJsonValue feature = object.value(QStringLiteral("feature"));
    const QJsonValue terrain = object.value(QStringLiteral("terrain"));
    const QJsonValue adjustable = object.value(QStringLiteral("adjustableConnector"));
    return feature.isString()
            && terrain.isString()
            && adjustable.isBool()
            && parseFeature(feature.toString(), section.feature)
            && parseTerrain(terrain.toString(), section.terrain)
            && integerNumber(object, "sourceStartMs", section.sourceStartMs)
            && integerNumber(object, "nominalDurationMs", section.nominalDurationMs)
            && integerNumber(object, "minimumDurationMs", section.minimumDurationMs)
            && integerNumber(object, "maximumDurationMs", section.maximumDurationMs)
            && finiteNumber(object, "startDistanceMeters", section.startDistanceMeters)
            && finiteNumber(object, "lengthMeters", section.lengthMeters)
            && finiteNumber(object, "startElevationMeters", section.startElevationMeters)
            && finiteNumber(object, "endElevationMeters", section.endElevationMeters)
            && finiteNumber(object, "targetStartWatts", section.targetStartWatts)
            && finiteNumber(object, "targetEndWatts", section.targetEndWatts)
            && finiteNumber(object, "gradePercent", section.gradePercent)
            && finiteNumber(object, "difficulty", section.difficulty)
            && unsignedNumber(object, "visualVariant", section.visualVariant)
            && (section.adjustableConnector = adjustable.toBool(), true);
}

QJsonObject courseToJson(const WorkoutGameDistanceCourse &course)
{
    QJsonArray sections;
    for (const WorkoutGameDistanceCourseSection &section : course.sections) {
        sections.append(sectionToJson(section));
    }
    return {
        {QStringLiteral("seed"), double(course.seed)},
        {QStringLiteral("nominalDurationMs"), double(course.nominalDurationMs)},
        {QStringLiteral("totalDistanceMeters"), course.totalDistanceMeters},
        {QStringLiteral("elevationGainMeters"), course.elevationGainMeters},
        {QStringLiteral("elevationLossMeters"), course.elevationLossMeters},
        {QStringLiteral("sections"), sections}
    };
}

bool parseCourse(const QJsonObject &object, WorkoutGameDistanceCourse &course)
{
    const QJsonValue sectionsValue = object.value(QStringLiteral("sections"));
    if (!sectionsValue.isArray()) return false;
    const QJsonArray sections = sectionsValue.toArray();
    if (sections.isEmpty() || sections.size() > 10000) return false;

    course.status = WorkoutGameDistanceCourseStatus::Ready;
    if (!unsignedNumber(object, "seed", course.seed)
            || !integerNumber(object, "nominalDurationMs", course.nominalDurationMs)
            || !finiteNumber(object, "totalDistanceMeters", course.totalDistanceMeters)
            || !finiteNumber(object, "elevationGainMeters", course.elevationGainMeters)
            || !finiteNumber(object, "elevationLossMeters", course.elevationLossMeters)) {
        return false;
    }
    course.sections.reserve(std::size_t(sections.size()));
    for (const QJsonValue &value : sections) {
        WorkoutGameDistanceCourseSection section;
        if (!value.isObject() || !parseSection(value.toObject(), section)) return false;
        course.sections.push_back(section);
    }
    return true;
}

QJsonObject connectorToJson(const WorkoutGameRoadConnector &connector)
{
    return {
        {QStringLiteral("xMeters"), connector.xMeters},
        {QStringLiteral("zMeters"), connector.zMeters},
        {QStringLiteral("elevationMeters"), connector.elevationMeters},
        {QStringLiteral("headingRadians"), connector.headingRadians},
        {QStringLiteral("halfWidthMeters"), connector.halfWidthMeters},
        {QStringLiteral("gradePercent"), connector.gradePercent}
    };
}

bool parseConnector(
        const QJsonValue &value,
        WorkoutGameRoadConnector &connector)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    return finiteNumber(object, "xMeters", connector.xMeters)
            && finiteNumber(object, "zMeters", connector.zMeters)
            && finiteNumber(object, "elevationMeters",
                            connector.elevationMeters)
            && finiteNumber(object, "headingRadians",
                            connector.headingRadians)
            && finiteNumber(object, "halfWidthMeters",
                            connector.halfWidthMeters)
            && finiteNumber(object, "gradePercent", connector.gradePercent);
}

QJsonObject challengeProfileToJson(
        const WorkoutGameFeatureChallengeProfile &profile)
{
    return {
        {QStringLiteral("enabled"), profile.enabled},
        {QStringLiteral("cue"), challengeCueName(profile.cue)},
        {QStringLiteral("measurementStartProgress"),
         profile.measurementStartProgress},
        {QStringLiteral("decisionProgress"), profile.decisionProgress},
        {QStringLiteral("minimumEffortRatio"), profile.minimumEffortRatio},
        {QStringLiteral("minimumCadenceRpm"), profile.minimumCadenceRpm},
        {QStringLiteral("minimumSpeedKph"), profile.minimumSpeedKph},
        {QStringLiteral("maximumSpeedKph"), profile.maximumSpeedKph},
        {QStringLiteral("minimumAdherence"), profile.minimumAdherence},
        {QStringLiteral("bonusPoints"), double(profile.bonusPoints)}
    };
}

bool parseChallengeProfile(
        const QJsonValue &value,
        WorkoutGameFeatureChallengeProfile &profile)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue enabled = object.value(QStringLiteral("enabled"));
    const QJsonValue cue = object.value(QStringLiteral("cue"));
    return enabled.isBool()
            && cue.isString()
            && (profile.enabled = enabled.toBool(), true)
            && parseChallengeCue(cue.toString(), profile.cue)
            && finiteNumber(object, "measurementStartProgress",
                            profile.measurementStartProgress)
            && finiteNumber(object, "decisionProgress",
                            profile.decisionProgress)
            && finiteNumber(object, "minimumEffortRatio",
                            profile.minimumEffortRatio)
            && finiteNumber(object, "minimumCadenceRpm",
                            profile.minimumCadenceRpm)
            && finiteNumber(object, "minimumSpeedKph",
                            profile.minimumSpeedKph)
            && finiteNumber(object, "maximumSpeedKph",
                            profile.maximumSpeedKph)
            && finiteNumber(object, "minimumAdherence",
                            profile.minimumAdherence)
            && safeUnsigned64Number(object, "bonusPoints",
                                    profile.bonusPoints);
}

QJsonObject challengeToJson(const WorkoutGameRoadChallengeGate &challenge)
{
    return {
        {QStringLiteral("prepareDistanceMeters"),
         challenge.prepareDistanceMeters},
        {QStringLiteral("decisionDistanceMeters"),
         challenge.decisionDistanceMeters},
        {QStringLiteral("obstacleDistanceMeters"),
         challenge.obstacleDistanceMeters},
        {QStringLiteral("bypassStartDistanceMeters"),
         challenge.bypassStartDistanceMeters},
        {QStringLiteral("bypassEndDistanceMeters"),
         challenge.bypassEndDistanceMeters},
        {QStringLiteral("bypassLateralMeters"),
         challenge.bypassLateralMeters},
        {QStringLiteral("profile"),
         challengeProfileToJson(challenge.profile)}
    };
}

bool parseChallenge(
        const QJsonValue &value,
        WorkoutGameRoadChallengeGate &challenge)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    challenge.enabled = true;
    return finiteNumber(object, "prepareDistanceMeters",
                        challenge.prepareDistanceMeters)
            && finiteNumber(object, "decisionDistanceMeters",
                            challenge.decisionDistanceMeters)
            && finiteNumber(object, "obstacleDistanceMeters",
                            challenge.obstacleDistanceMeters)
            && finiteNumber(object, "bypassStartDistanceMeters",
                            challenge.bypassStartDistanceMeters)
            && finiteNumber(object, "bypassEndDistanceMeters",
                            challenge.bypassEndDistanceMeters)
            && finiteNumber(object, "bypassLateralMeters",
                            challenge.bypassLateralMeters)
            && parseChallengeProfile(
                object.value(QStringLiteral("profile")), challenge.profile);
}

QJsonObject gapLineToJson(const WorkoutGameRoadGapJumpLine &line)
{
    return {
        {QStringLiteral("id"), gapLineName(line.id)},
        {QStringLiteral("takeoffDistanceMeters"), line.takeoffDistanceMeters},
        {QStringLiteral("landingDistanceMeters"), line.landingDistanceMeters},
        {QStringLiteral("lateralMeters"), line.lateralMeters},
        {QStringLiteral("gapLengthMeters"), line.gapLengthMeters},
        {QStringLiteral("minimumSpeedMetersPerSecond"),
         line.minimumSpeedMetersPerSecond},
        {QStringLiteral("nominalFlightSeconds"), line.nominalFlightSeconds},
        {QStringLiteral("lipHeightMeters"), line.lipHeightMeters},
        {QStringLiteral("landingDropMeters"), line.landingDropMeters}
    };
}

bool parseGapLine(
        const QJsonValue &value,
        WorkoutGameRoadGapJumpLine &line)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    return id.isString()
            && parseGapLine(id.toString(), line.id)
            && finiteNumber(object, "takeoffDistanceMeters",
                            line.takeoffDistanceMeters)
            && finiteNumber(object, "landingDistanceMeters",
                            line.landingDistanceMeters)
            && finiteNumber(object, "lateralMeters", line.lateralMeters)
            && finiteNumber(object, "gapLengthMeters", line.gapLengthMeters)
            && finiteNumber(object, "minimumSpeedMetersPerSecond",
                            line.minimumSpeedMetersPerSecond)
            && finiteNumber(object, "nominalFlightSeconds",
                            line.nominalFlightSeconds)
            && finiteNumber(object, "lipHeightMeters", line.lipHeightMeters)
            && finiteNumber(object, "landingDropMeters",
                            line.landingDropMeters);
}

QJsonObject gapJumpToJson(const WorkoutGameRoadGapJumpGate &gap)
{
    QJsonArray lines;
    for (const WorkoutGameRoadGapJumpLine &line : gap.lines) {
        lines.append(gapLineToJson(line));
    }
    return {
        {QStringLiteral("prepareDistanceMeters"), gap.prepareDistanceMeters},
        {QStringLiteral("launchWindowStartDistanceMeters"),
         gap.launchWindowStartDistanceMeters},
        {QStringLiteral("lockDistanceMeters"), gap.lockDistanceMeters},
        {QStringLiteral("splitStartDistanceMeters"),
         gap.splitStartDistanceMeters},
        {QStringLiteral("mergeEndDistanceMeters"), gap.mergeEndDistanceMeters},
        {QStringLiteral("lines"), lines}
    };
}

bool parseGapJump(
        const QJsonValue &value,
        WorkoutGameRoadGapJumpGate &gap)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue linesValue = object.value(QStringLiteral("lines"));
    if (!linesValue.isArray()) return false;
    const QJsonArray lines = linesValue.toArray();
    if (lines.size() != int(gap.lines.size())) return false;
    gap.enabled = true;
    if (!finiteNumber(object, "prepareDistanceMeters",
                      gap.prepareDistanceMeters)
            || !finiteNumber(object, "launchWindowStartDistanceMeters",
                             gap.launchWindowStartDistanceMeters)
            || !finiteNumber(object, "lockDistanceMeters",
                             gap.lockDistanceMeters)
            || !finiteNumber(object, "splitStartDistanceMeters",
                             gap.splitStartDistanceMeters)
            || !finiteNumber(object, "mergeEndDistanceMeters",
                             gap.mergeEndDistanceMeters)) {
        return false;
    }
    for (int index = 0; index < lines.size(); ++index) {
        if (!parseGapLine(lines[index], gap.lines[std::size_t(index)])) {
            return false;
        }
    }
    return true;
}

QJsonObject roadPieceToJson(const WorkoutGameRoadPiece &piece)
{
    QJsonObject result {
        {QStringLiteral("sourceSectionIndex"),
         double(piece.sourceSectionIndex)},
        {QStringLiteral("terrain"), terrainName(piece.terrain)},
        {QStringLiteral("animation"), animationName(piece.animation)},
        {QStringLiteral("startDistanceMeters"), piece.startDistanceMeters},
        {QStringLiteral("lengthMeters"), piece.lengthMeters},
        {QStringLiteral("turnRadians"), piece.turnRadians},
        {QStringLiteral("riseMeters"), piece.riseMeters},
        {QStringLiteral("difficulty"), piece.difficulty},
        {QStringLiteral("reliefScale"), piece.reliefScale},
        {QStringLiteral("geometryAnchorDistanceMeters"),
         piece.geometryAnchorDistanceMeters},
        {QStringLiteral("entry"), connectorToJson(piece.entry)},
        {QStringLiteral("exit"), connectorToJson(piece.exit)},
        {QStringLiteral("qualityExempt"), piece.qualityExempt},
        {QStringLiteral("qualityExemptionStartDistanceMeters"),
         piece.qualityExemptionStartDistanceMeters},
        {QStringLiteral("qualityExemptionEndDistanceMeters"),
         piece.qualityExemptionEndDistanceMeters}
    };
    if (piece.challenge.enabled) {
        result.insert(QStringLiteral("challenge"),
                      challengeToJson(piece.challenge));
    }
    if (piece.gapJump.enabled) {
        result.insert(QStringLiteral("gapJump"), gapJumpToJson(piece.gapJump));
    }
    return result;
}

bool parseRoadPiece(const QJsonObject &object, WorkoutGameRoadPiece &piece)
{
    const QJsonValue terrain = object.value(QStringLiteral("terrain"));
    const QJsonValue animation = object.value(QStringLiteral("animation"));
    const QJsonValue qualityExempt =
            object.value(QStringLiteral("qualityExempt"));
    if (!terrain.isString() || !animation.isString()
            || !qualityExempt.isBool()
            || !sizeNumber(object, "sourceSectionIndex",
                           piece.sourceSectionIndex)
            || !parseTerrain(terrain.toString(), piece.terrain)
            || !parseAnimation(animation.toString(), piece.animation)
            || !finiteNumber(object, "startDistanceMeters",
                             piece.startDistanceMeters)
            || !finiteNumber(object, "lengthMeters", piece.lengthMeters)
            || !finiteNumber(object, "turnRadians", piece.turnRadians)
            || !finiteNumber(object, "riseMeters", piece.riseMeters)
            || !finiteNumber(object, "difficulty", piece.difficulty)
            || !finiteNumber(object, "reliefScale", piece.reliefScale)
            || !finiteNumber(object, "geometryAnchorDistanceMeters",
                             piece.geometryAnchorDistanceMeters)
            || !parseConnector(object.value(QStringLiteral("entry")),
                               piece.entry)
            || !parseConnector(object.value(QStringLiteral("exit")),
                               piece.exit)
            || !finiteNumber(object,
                    "qualityExemptionStartDistanceMeters",
                    piece.qualityExemptionStartDistanceMeters)
            || !finiteNumber(object,
                    "qualityExemptionEndDistanceMeters",
                    piece.qualityExemptionEndDistanceMeters)) {
        return false;
    }
    piece.qualityExempt = qualityExempt.toBool();
    const QJsonValue challenge = object.value(QStringLiteral("challenge"));
    const QJsonValue gapJump = object.value(QStringLiteral("gapJump"));
    if (!challenge.isUndefined() && !parseChallenge(challenge, piece.challenge)) {
        return false;
    }
    if (!gapJump.isUndefined() && !parseGapJump(gapJump, piece.gapJump)) {
        return false;
    }
    return true;
}

QJsonObject roadPlanToJson(const WorkoutGameRoadPlan &plan)
{
    QJsonArray pieces;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        pieces.append(roadPieceToJson(piece));
    }
    return {
        {QStringLiteral("generationVersion"),
         double(plan.generationVersion)},
        {QStringLiteral("pieces"), pieces}
    };
}

WorkoutGameCourseDocumentStatus parseRoadPlan(
        const QJsonValue &value,
        std::size_t sourceSectionCount,
        std::shared_ptr<const WorkoutGameRoadPlan> &destination)
{
    if (!value.isObject()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonObject object = value.toObject();
    std::uint32_t generationVersion = 0;
    if (!unsignedNumber(object, "generationVersion", generationVersion)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (generationVersion != WorkoutGameRoadPlan::CurrentGenerationVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    const QJsonValue piecesValue = object.value(QStringLiteral("pieces"));
    if (!piecesValue.isArray()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonArray pieces = piecesValue.toArray();
    if (pieces.size() > int(WorkoutGameRoadPlan::MaximumPieces)) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    auto plan = std::make_shared<WorkoutGameRoadPlan>();
    plan->generationVersion = generationVersion;
    plan->pieces.reserve(std::size_t(pieces.size()));
    for (const QJsonValue &pieceValue : pieces) {
        WorkoutGameRoadPiece piece;
        if (!pieceValue.isObject()
                || !parseRoadPiece(pieceValue.toObject(), piece)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        plan->pieces.push_back(piece);
    }
    const WorkoutGameRoadPlanValidationStatus validation =
            WorkoutGameRoadPlanValidator::validate(
                *plan, sourceSectionCount);
    if (validation == WorkoutGameRoadPlanValidationStatus::ResourceLimit) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    if (validation == WorkoutGameRoadPlanValidationStatus::UnsupportedVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    if (validation != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(*plan).accepted()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    destination = std::move(plan);
    return WorkoutGameCourseDocumentStatus::Ready;
}

bool roadPlanMatchesCourse(const WorkoutGameDistanceCourse &course)
{
    if (!course.roadPlan
            || WorkoutGameRoadPlanValidator::validate(
                *course.roadPlan, course.sections.size())
                != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(*course.roadPlan).accepted()) {
        return false;
    }
    std::vector<double> sectionLengths(course.sections.size(), 0.0);
    for (const WorkoutGameRoadPiece &piece : course.roadPlan->pieces) {
        if (piece.sourceSectionIndex >= course.sections.size()
                || piece.terrain
                    != course.sections[piece.sourceSectionIndex].terrain) {
            return false;
        }
        sectionLengths[piece.sourceSectionIndex] += piece.lengthMeters;
    }
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        if (std::abs(sectionLengths[index] - course.sections[index].lengthMeters)
                > 1.0e-5) {
            return false;
        }
    }
    const WorkoutGameRoadPiece &last = course.roadPlan->pieces.back();
    return std::abs(last.startDistanceMeters + last.lengthMeters
                    - course.totalDistanceMeters) <= 1.0e-5;
}

bool documentForPersistence(
        const WorkoutGameCourseDocument &source,
        WorkoutGameCourseDocument &destination)
{
    destination = source;
    if (destination.schemaVersion
            == WorkoutGameCourseDocumentCodec::CurrentSchemaVersion) {
        return WorkoutGameCourseDocumentCodec::valid(destination);
    }
    if (destination.schemaVersion != 1
            || !WorkoutGameCourseDocumentCodec::valid(destination)) {
        return false;
    }
    const WorkoutGameCourse visual =
            WorkoutGameDistancePlayback::visualCourse(destination.course);
    const WorkoutGameRoadPlan plan =
            WorkoutGameRoadCourseBuilder::generatePlan(
                visual, destination.ftpWatts);
    if (WorkoutGameRoadPlanValidator::validate(
                plan, destination.course.sections.size())
            != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(plan).accepted()) {
        return false;
    }
    destination.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    destination.course.roadPlan =
            std::make_shared<const WorkoutGameRoadPlan>(plan);
    return WorkoutGameCourseDocumentCodec::valid(destination);
}

bool writeAtomically(
        const QString &path,
        const QByteArray &data,
        QString &error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
            || file.write(data) != data.size()
            || !file.commit()) {
        error = file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

}

bool WorkoutGameCourseDocumentCodec::valid(
        const WorkoutGameCourseDocument &document)
{
    static const QRegularExpression sha256Pattern(
            QStringLiteral("^[0-9a-f]{64}$"));
    const QFileInfo sourceInfo(document.sourceFileName);
    const bool sourceIntervalsValid = document.sourceIntervals.empty()
            || validIntervals(document.sourceIntervals,
                              document.course.nominalDurationMs);
    const bool schemaValid = document.schemaVersion == 1
            ? !document.course.roadPlan
            : document.schemaVersion == CurrentSchemaVersion
                && roadPlanMatchesCourse(document.course);
    return schemaValid
            && !document.title.trimmed().isEmpty()
            && document.title.size() <= 200
            && !document.title.contains(QLatin1Char('\n'))
            && !document.title.contains(QLatin1Char('\r'))
            && !document.sourceFileName.isEmpty()
            && sourceInfo.fileName() == document.sourceFileName
            && !document.sourceFileName.contains(QLatin1Char('/'))
            && !document.sourceFileName.contains(QLatin1Char('\\'))
            && sha256Pattern.match(document.sourceSha256).hasMatch()
            && std::isfinite(document.ftpWatts)
            && document.ftpWatts > 0.0
            && document.ftpWatts <= 3000.0
            && sourceIntervalsValid
            && !presetName(document.preset).isEmpty()
            && WorkoutGameDistanceCourseBuilder::validParameters(
                document.generationParameters)
            && WorkoutGameDistanceCourseBuilder::validCourse(document.course);
}

QByteArray WorkoutGameCourseDocumentCodec::encode(
        const WorkoutGameCourseDocument &document)
{
    if (!valid(document)) return {};
    QJsonObject source {
        {QStringLiteral("fileName"), document.sourceFileName},
        {QStringLiteral("sha256"), document.sourceSha256}
    };
    if (!document.sourceIntervals.empty()) {
        source.insert(QStringLiteral("intervals"),
                      intervalsToJson(document.sourceIntervals));
    }
    const QJsonObject conversion {
        {QStringLiteral("ftpWatts"), document.ftpWatts},
        {QStringLiteral("preset"), presetName(document.preset)},
        {QStringLiteral("parameters"), generationToJson(
            document.generationParameters)}
    };
    QJsonObject root {
        {QStringLiteral("schemaVersion"), document.schemaVersion},
        {QStringLiteral("title"), document.title},
        {QStringLiteral("source"), source},
        {QStringLiteral("conversion"), conversion},
        {QStringLiteral("course"), courseToJson(document.course)}
    };
    if (document.schemaVersion == CurrentSchemaVersion) {
        root.insert(QStringLiteral("roadPlan"),
                    roadPlanToJson(*document.course.roadPlan));
    }
    const QByteArray encoded =
            QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
    return encoded.size() <= MaximumDocumentBytes ? encoded : QByteArray();
}

WorkoutGameCourseDocumentStatus WorkoutGameCourseDocumentCodec::decode(
        const QByteArray &json,
        WorkoutGameCourseDocument &document)
{
    document = WorkoutGameCourseDocument();
    if (json.size() > MaximumDocumentBytes) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        return WorkoutGameCourseDocumentStatus::InvalidJson;
    }
    const QJsonObject root = parsed.object();
    std::int64_t schemaVersion = 0;
    if (!integerNumber(root, "schemaVersion", schemaVersion)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (schemaVersion != 1 && schemaVersion != CurrentSchemaVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    const QJsonValue title = root.value(QStringLiteral("title"));
    const QJsonValue sourceValue = root.value(QStringLiteral("source"));
    const QJsonValue conversionValue = root.value(QStringLiteral("conversion"));
    const QJsonValue courseValue = root.value(QStringLiteral("course"));
    if (!title.isString()
            || !sourceValue.isObject()
            || !conversionValue.isObject()
            || !courseValue.isObject()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }

    const QJsonObject source = sourceValue.toObject();
    const QJsonObject conversion = conversionValue.toObject();
    const QJsonValue sourceFileName = source.value(QStringLiteral("fileName"));
    const QJsonValue sourceSha256 = source.value(QStringLiteral("sha256"));
    const QJsonValue preset = conversion.value(QStringLiteral("preset"));
    const QJsonValue parameters = conversion.value(QStringLiteral("parameters"));
    document.schemaVersion = int(schemaVersion);
    document.title = title.toString();
    if (!sourceFileName.isString()
            || !sourceSha256.isString()
            || !preset.isString()
            || !parameters.isObject()
            || !finiteNumber(conversion, "ftpWatts", document.ftpWatts)
            || !parsePreset(preset.toString(), document.preset)
            || !parseGeneration(
                parameters.toObject(), document.generationParameters)
            || !parseCourse(courseValue.toObject(), document.course)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    document.sourceFileName = sourceFileName.toString();
    document.sourceSha256 = sourceSha256.toString();
    const QJsonValue intervals = source.value(QStringLiteral("intervals"));
    if (!intervals.isUndefined()
            && (!intervals.isArray()
                || !parseIntervals(intervals.toArray(), document.sourceIntervals))) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonValue roadPlan = root.value(QStringLiteral("roadPlan"));
    if (document.schemaVersion == 1) {
        if (!roadPlan.isUndefined()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
    } else {
        const WorkoutGameCourseDocumentStatus planStatus = parseRoadPlan(
                roadPlan, document.course.sections.size(),
                document.course.roadPlan);
        if (planStatus != WorkoutGameCourseDocumentStatus::Ready) {
            return planStatus;
        }
    }
    return valid(document)
            ? WorkoutGameCourseDocumentStatus::Ready
            : WorkoutGameCourseDocumentStatus::InvalidDocument;
}

QString WorkoutGameCourseDocumentStore::sidecarPathForCourse(
        const QString &coursePath)
{
    const QFileInfo info(coursePath);
    return info.dir().filePath(
            info.completeBaseName() + QStringLiteral(".gcmtb.json"));
}

WorkoutGameCourseDocumentStatus WorkoutGameCourseDocumentStore::saveNewArtifact(
        const QString &coursePath,
        const WorkoutGameCourseDocument &document,
        QString &error)
{
    error.clear();
    WorkoutGameCourseDocument persisted;
    if (!documentForPersistence(document, persisted)) {
        error = QStringLiteral("Invalid MTB course document");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QByteArray metadata = WorkoutGameCourseDocumentCodec::encode(persisted);
    const QByteArray course = WorkoutGameCourseCrsExporter::encode(persisted);
    if (metadata.isEmpty() || course.isEmpty()) {
        error = QStringLiteral("Invalid MTB course document");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QString sidecarPath = sidecarPathForCourse(coursePath);
    if (QFileInfo::exists(coursePath) || QFileInfo::exists(sidecarPath)) {
        error = QStringLiteral("The MTB course or its metadata already exists");
        return WorkoutGameCourseDocumentStatus::Conflict;
    }
    if (!writeAtomically(sidecarPath, metadata, error)) {
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    if (!writeAtomically(coursePath, course, error)) {
        const bool rolledBack = QFile::remove(sidecarPath);
        if (!rolledBack) error += QStringLiteral("; metadata rollback failed");
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    return WorkoutGameCourseDocumentStatus::Ready;
}

WorkoutGameCourseDocumentStatus WorkoutGameCourseDocumentStore::replaceArtifact(
        const QString &coursePath,
        const WorkoutGameCourseDocument &document,
        QString &error)
{
    WorkoutGameCourseDocument existing;
    const WorkoutGameCourseDocumentStatus loadStatus =
            loadForCourse(coursePath, existing, error);
    if (loadStatus != WorkoutGameCourseDocumentStatus::Ready) return loadStatus;

    WorkoutGameCourseDocument persisted;
    if (!documentForPersistence(document, persisted)) {
        error = QStringLiteral("Invalid MTB course document");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QByteArray metadata = WorkoutGameCourseDocumentCodec::encode(persisted);
    const QByteArray course = WorkoutGameCourseCrsExporter::encode(persisted);
    if (metadata.isEmpty() || course.isEmpty()) {
        error = QStringLiteral("Invalid MTB course document");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }

    const QByteArray oldCourse = WorkoutGameCourseCrsExporter::encode(existing);
    if (!writeAtomically(coursePath, course, error)) {
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    if (!writeAtomically(sidecarPathForCourse(coursePath), metadata, error)) {
        QString rollbackError;
        if (!writeAtomically(coursePath, oldCourse, rollbackError)) {
            error += QStringLiteral("; course rollback failed: ") + rollbackError;
        }
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    return WorkoutGameCourseDocumentStatus::Ready;
}

WorkoutGameCourseDocumentStatus WorkoutGameCourseDocumentStore::loadForCourse(
        const QString &coursePath,
        WorkoutGameCourseDocument &document,
        QString &error)
{
    error.clear();
    QFile file(sidecarPathForCourse(coursePath));
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    const QByteArray data = file.read(
            WorkoutGameCourseDocumentCodec::MaximumDocumentBytes + 1);
    if (file.error() != QFile::NoError) {
        error = file.errorString();
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    const WorkoutGameCourseDocumentStatus status =
            WorkoutGameCourseDocumentCodec::decode(data, document);
    if (status != WorkoutGameCourseDocumentStatus::Ready) {
        error = QStringLiteral("Invalid MTB course metadata");
        return status;
    }

    const QByteArray expectedCourse =
            WorkoutGameCourseCrsExporter::encode(document);
    if (expectedCourse.isEmpty()) {
        error = QStringLiteral("Invalid MTB course metadata");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    QFile course(coursePath);
    if (!course.open(QIODevice::ReadOnly)) {
        error = course.errorString();
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    const QByteArray actualCourse = course.read(expectedCourse.size() + 1);
    if (course.error() != QFile::NoError) {
        error = course.errorString();
        return WorkoutGameCourseDocumentStatus::IoError;
    }
    if (actualCourse != expectedCourse) {
        error = QStringLiteral(
                "MTB course metadata does not match the course file");
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    return WorkoutGameCourseDocumentStatus::Ready;
}
