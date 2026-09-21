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

#include <algorithm>
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

bool integerNumber(
        const QJsonObject &object,
        const char *key,
        std::int64_t &value);

QString intervalRoleName(WorkoutGameCourseIntervalRole role)
{
    switch (role) {
    case WorkoutGameCourseIntervalRole::Prescribed:
        return QStringLiteral("prescribed");
    case WorkoutGameCourseIntervalRole::NonPrescriptiveWarmup:
        return QStringLiteral("non-prescriptive-warmup");
    case WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown:
        return QStringLiteral("non-prescriptive-cooldown");
    case WorkoutGameCourseIntervalRole::NonPrescriptiveTransition:
        return QStringLiteral("non-prescriptive-transition");
    }
    return {};
}

bool parseIntervalRole(
        const QString &name,
        WorkoutGameCourseIntervalRole &role)
{
    const std::pair<const char *, WorkoutGameCourseIntervalRole> values[] = {
        {"prescribed", WorkoutGameCourseIntervalRole::Prescribed},
        {"non-prescriptive-warmup",
            WorkoutGameCourseIntervalRole::NonPrescriptiveWarmup},
        {"non-prescriptive-cooldown",
            WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown},
        {"non-prescriptive-transition",
            WorkoutGameCourseIntervalRole::NonPrescriptiveTransition}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            role = value.second;
            return true;
        }
    }
    return false;
}

QJsonObject prescriptionMetadataToJson(
        const WorkoutGameCoursePrescriptionMetadata &metadata)
{
    QJsonArray roles;
    for (WorkoutGameCourseIntervalRole role : metadata.intervalRoles) {
        roles.append(intervalRoleName(role));
    }
    return {
        {QStringLiteral("version"), metadata.version},
        {QStringLiteral("intervalRoles"), roles}
    };
}

WorkoutGameCourseDocumentStatus parsePrescriptionMetadata(
        const QJsonValue &value,
        WorkoutGameCoursePrescriptionMetadata &metadata)
{
    metadata = WorkoutGameCoursePrescriptionMetadata();
    if (value.isUndefined()) return WorkoutGameCourseDocumentStatus::Ready;
    if (!value.isObject()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonObject object = value.toObject();
    std::int64_t version = 0;
    if (!integerNumber(object, "version", version)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (version != WorkoutGameCoursePrescriptionMetadata::CurrentVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    const QJsonValue roles = object.value(QStringLiteral("intervalRoles"));
    if (!roles.isArray() || roles.toArray().size() > 10000) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    metadata.version = int(version);
    metadata.intervalRoles.reserve(std::size_t(roles.toArray().size()));
    for (const QJsonValue &value : roles.toArray()) {
        WorkoutGameCourseIntervalRole role;
        if (!value.isString() || !parseIntervalRole(value.toString(), role)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        metadata.intervalRoles.push_back(role);
    }
    return WorkoutGameCourseDocumentStatus::Ready;
}

QJsonArray sourceLapsToJson(
        const std::vector<WorkoutGameCourseSourceLap> &laps)
{
    QJsonArray values;
    for (const WorkoutGameCourseSourceLap &lap : laps) {
        values.append(QJsonObject {
            {QStringLiteral("timeMs"), double(lap.timeMs)},
            {QStringLiteral("name"), lap.name}
        });
    }
    return values;
}

WorkoutGameCourseDocumentStatus parseSourceLaps(
        const QJsonValue &value,
        std::vector<WorkoutGameCourseSourceLap> &laps)
{
    laps.clear();
    if (value.isUndefined()) return WorkoutGameCourseDocumentStatus::Ready;
    if (!value.isArray()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (value.toArray().size()
            > int(WorkoutGameCourseDocumentCodec::MaximumSourceAnnotations)) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const QJsonObject object = entry.toObject();
        std::int64_t timeMs = 0;
        const QJsonValue name = object.value(QStringLiteral("name"));
        if (!integerNumber(object, "timeMs", timeMs) || !name.isString()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        laps.push_back({timeMs, name.toString()});
    }
    return WorkoutGameCourseDocumentStatus::Ready;
}

QJsonArray sourceTextsToJson(
        const std::vector<WorkoutGameCourseSourceText> &texts)
{
    QJsonArray values;
    for (const WorkoutGameCourseSourceText &text : texts) {
        values.append(QJsonObject {
            {QStringLiteral("timeMs"), double(text.timeMs)},
            {QStringLiteral("durationSeconds"), text.durationSeconds},
            {QStringLiteral("text"), text.text}
        });
    }
    return values;
}

WorkoutGameCourseDocumentStatus parseSourceTexts(
        const QJsonValue &value,
        std::vector<WorkoutGameCourseSourceText> &texts)
{
    texts.clear();
    if (value.isUndefined()) return WorkoutGameCourseDocumentStatus::Ready;
    if (!value.isArray()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (value.toArray().size()
            > int(WorkoutGameCourseDocumentCodec::MaximumSourceAnnotations)) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const QJsonObject object = entry.toObject();
        std::int64_t timeMs = 0;
        std::int64_t durationSeconds = 0;
        const QJsonValue text = object.value(QStringLiteral("text"));
        if (!integerNumber(object, "timeMs", timeMs)
                || !integerNumber(
                    object, "durationSeconds", durationSeconds)
                || durationSeconds < std::numeric_limits<int>::min()
                || durationSeconds > std::numeric_limits<int>::max()
                || !text.isString()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        texts.push_back({timeMs, int(durationSeconds), text.toString()});
    }
    return WorkoutGameCourseDocumentStatus::Ready;
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
        {QStringLiteral("terrainVariationPercent"),
         parameters.terrainVariationPercent},
        {QStringLiteral("variationLengthMeters"),
         parameters.variationLengthMeters},
        {QStringLiteral("referenceGear"), parameters.referenceGear},
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
    parameters.terrainVariationPercent = 15.0;
    parameters.variationLengthMeters = 60.0;
    parameters.referenceGear = 6;
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
    std::int64_t referenceGear = parameters.referenceGear;
    if ((object.contains(QStringLiteral("terrainVariationPercent"))
            && !finiteNumber(object, "terrainVariationPercent",
                             parameters.terrainVariationPercent))
            || (object.contains(QStringLiteral("variationLengthMeters"))
                && !finiteNumber(object, "variationLengthMeters",
                                 parameters.variationLengthMeters))
            || (object.contains(QStringLiteral("referenceGear"))
                && !integerNumber(object, "referenceGear", referenceGear))) {
        return false;
    }
    if (referenceGear < std::numeric_limits<int>::min()
            || referenceGear > std::numeric_limits<int>::max()) {
        return false;
    }
    parameters.referenceGear = int(referenceGear);
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

std::vector<WorkoutGameInterval> generatedIntervals(
        const WorkoutGameDistanceCourse &course,
        const std::vector<WorkoutGameInterval> &sourceIntervals)
{
    std::vector<WorkoutGameInterval> result;
    result.reserve(sourceIntervals.size());
    std::size_t sectionIndex = 0;
    for (const WorkoutGameInterval &source : sourceIntervals) {
        const std::int64_t sourceEndMs = source.startMs + source.durationMs;
        WorkoutGameInterval generated;
        generated.startMs = source.startMs;
        bool haveSection = false;
        while (sectionIndex < course.sections.size()
                && course.sections[sectionIndex].sourceStartMs < sourceEndMs) {
            const WorkoutGameDistanceCourseSection &section =
                    course.sections[sectionIndex++];
            if (section.sourceStartMs < source.startMs) return {};
            if (!haveSection) {
                generated.startWatts = section.targetStartWatts;
                haveSection = true;
            }
            generated.durationMs += section.nominalDurationMs;
            generated.endWatts = section.targetEndWatts;
        }
        if (!haveSection) return {};
        result.push_back(generated);
    }
    if (sectionIndex != course.sections.size()) return {};
    return result;
}

std::size_t sourceIntervalAt(
        const std::vector<WorkoutGameInterval> &sourceIntervals,
        std::int64_t sourceStartMs)
{
    const auto upper = std::upper_bound(
            sourceIntervals.begin(), sourceIntervals.end(), sourceStartMs,
            [](std::int64_t value, const WorkoutGameInterval &interval) {
                return value < interval.startMs;
            });
    if (upper == sourceIntervals.begin()) return sourceIntervals.size();
    const auto candidate = std::prev(upper);
    if (sourceStartMs >= candidate->startMs + candidate->durationMs) {
        return sourceIntervals.size();
    }
    return std::size_t(std::distance(sourceIntervals.begin(), candidate));
}

QJsonObject sectionToJson(const WorkoutGameDistanceCourseSection &section)
{
    QJsonObject result {
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
        {QStringLiteral("referenceEffortStartWatts"),
         section.referenceEffortStartWatts},
        {QStringLiteral("referenceEffortEndWatts"),
         section.referenceEffortEndWatts},
        {QStringLiteral("gradePercent"), section.gradePercent},
        {QStringLiteral("difficulty"), section.difficulty},
        {QStringLiteral("visualVariant"), double(section.visualVariant)},
        {QStringLiteral("adjustableConnector"), section.adjustableConnector}
    };
    if (section.challengeCount >= 0) {
        result.insert(QStringLiteral("challengeCount"), section.challengeCount);
    }
    return result;
}

bool parseSection(
        const QJsonObject &object,
        WorkoutGameDistanceCourseSection &section)
{
    const QJsonValue feature = object.value(QStringLiteral("feature"));
    const QJsonValue terrain = object.value(QStringLiteral("terrain"));
    const QJsonValue adjustable = object.value(QStringLiteral("adjustableConnector"));
    std::int64_t challengeCount = -1;
    const QJsonValue challengeCountValue = object.value(
            QStringLiteral("challengeCount"));
    if (!challengeCountValue.isUndefined()
            && (!integerNumber(object, "challengeCount", challengeCount)
                || challengeCount < 0 || challengeCount > 1000000)) {
        return false;
    }
    section.challengeCount = int(challengeCount);
    const bool parsed = feature.isString()
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
    if (!parsed) return false;
    const bool hasReferenceStart = object.contains(
            QStringLiteral("referenceEffortStartWatts"));
    const bool hasReferenceEnd = object.contains(
            QStringLiteral("referenceEffortEndWatts"));
    if (hasReferenceStart != hasReferenceEnd) return false;
    if (hasReferenceStart) {
        return finiteNumber(object, "referenceEffortStartWatts",
                            section.referenceEffortStartWatts)
                && finiteNumber(object, "referenceEffortEndWatts",
                                section.referenceEffortEndWatts);
    }
    section.referenceEffortStartWatts = section.targetStartWatts;
    section.referenceEffortEndWatts = section.targetEndWatts;
    return true;
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

QJsonObject bankToJson(const WorkoutGameRoadBankProfile &bank)
{
    return {
        {QStringLiteral("enabled"), bank.enabled},
        {QStringLiteral("startDistanceMeters"), bank.startDistanceMeters},
        {QStringLiteral("curveStartDistanceMeters"),
         bank.curveStartDistanceMeters},
        {QStringLiteral("curveEndDistanceMeters"), bank.curveEndDistanceMeters},
        {QStringLiteral("endDistanceMeters"), bank.endDistanceMeters},
        {QStringLiteral("socketHalfWidthMeters"), bank.socketHalfWidthMeters},
        {QStringLiteral("activeHalfWidthMeters"), bank.activeHalfWidthMeters},
        {QStringLiteral("maximumBankRadians"), bank.maximumBankRadians},
        {QStringLiteral("maximumLineOffsetMeters"),
         bank.maximumLineOffsetMeters},
        {QStringLiteral("designSpeedMetersPerSecond"),
         bank.designSpeedMetersPerSecond}
    };
}

bool parseBank(const QJsonValue &value, WorkoutGameRoadBankProfile &bank)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue enabled = object.value(QStringLiteral("enabled"));
    return enabled.isBool()
            && (bank.enabled = enabled.toBool(), true)
            && finiteNumber(object, "startDistanceMeters",
                            bank.startDistanceMeters)
            && finiteNumber(object, "curveStartDistanceMeters",
                            bank.curveStartDistanceMeters)
            && finiteNumber(object, "curveEndDistanceMeters",
                            bank.curveEndDistanceMeters)
            && finiteNumber(object, "endDistanceMeters",
                            bank.endDistanceMeters)
            && finiteNumber(object, "socketHalfWidthMeters",
                            bank.socketHalfWidthMeters)
            && finiteNumber(object, "activeHalfWidthMeters",
                            bank.activeHalfWidthMeters)
            && finiteNumber(object, "maximumBankRadians",
                            bank.maximumBankRadians)
            && finiteNumber(object, "maximumLineOffsetMeters",
                            bank.maximumLineOffsetMeters)
            && finiteNumber(object, "designSpeedMetersPerSecond",
                            bank.designSpeedMetersPerSecond);
}

QJsonObject reliefToJson(const WorkoutGameRoadReliefProfile &relief)
{
    return {
        {QStringLiteral("enabled"), relief.enabled},
        {QStringLiteral("phaseRadians"), relief.phaseRadians},
        {QStringLiteral("constantCoefficientMeters"),
         relief.constantCoefficientMeters},
        {QStringLiteral("cosineCoefficientMeters"),
         relief.cosineCoefficientMeters},
        {QStringLiteral("sineCoefficientMeters"),
         relief.sineCoefficientMeters}
    };
}

bool parseRelief(
        const QJsonValue &value,
        WorkoutGameRoadReliefProfile &relief)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue enabled = object.value(QStringLiteral("enabled"));
    return enabled.isBool()
            && (relief.enabled = enabled.toBool(), true)
            && finiteNumber(object, "phaseRadians", relief.phaseRadians)
            && finiteNumber(object, "constantCoefficientMeters",
                            relief.constantCoefficientMeters)
            && finiteNumber(object, "cosineCoefficientMeters",
                            relief.cosineCoefficientMeters)
            && finiteNumber(object, "sineCoefficientMeters",
                            relief.sineCoefficientMeters);
}

QString assetPhysicsOperationName(WorkoutGameAssetPhysicsOperation operation)
{
    switch (operation) {
    case WorkoutGameAssetPhysicsOperation::AddObstacle:
        return QStringLiteral("add-obstacle");
    case WorkoutGameAssetPhysicsOperation::ReplaceSurface:
        return QStringLiteral("replace-surface");
    }
    return {};
}

bool parseAssetPhysicsOperation(
        const QString &name,
        WorkoutGameAssetPhysicsOperation &operation)
{
    if (name == QStringLiteral("add-obstacle")) {
        operation = WorkoutGameAssetPhysicsOperation::AddObstacle;
        return true;
    }
    if (name == QStringLiteral("replace-surface")) {
        operation = WorkoutGameAssetPhysicsOperation::ReplaceSurface;
        return true;
    }
    return false;
}

bool signed32Number(
        const QJsonObject &object,
        const char *key,
        std::int32_t &value)
{
    std::int64_t number = 0;
    if (!integerNumber(object, key, number)
            || number < std::numeric_limits<std::int32_t>::min()
            || number > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    value = std::int32_t(number);
    return true;
}

QJsonObject assetPhysicsSnapshotToJson(
        const WorkoutGameCourseAssetPhysicsSnapshot &snapshot)
{
    QJsonArray definitions;
    for (const WorkoutGameAssetPhysicsDefinition &definition :
            snapshot.physicsDefinitions) {
        QJsonArray chains;
        for (const WorkoutGameAssetPhysicsChain &chain : definition.chains) {
            QJsonArray points;
            for (const WorkoutGameAssetPhysicsPoint &point : chain.points) {
                points.append(QJsonObject {
                    {QStringLiteral("forwardMm"), point.forwardMm},
                    {QStringLiteral("heightMm"), point.heightMm}
                });
            }
            chains.append(QJsonObject {
                {QStringLiteral("points"), points}
            });
        }
        definitions.append(QJsonObject {
            {QStringLiteral("profileVersion"),
             double(definition.profileVersion)},
            {QStringLiteral("operation"),
             assetPhysicsOperationName(definition.operation)},
            {QStringLiteral("coulombFrictionMilli"),
             int(definition.coulombFrictionMilli)},
            {QStringLiteral("restitutionMilli"),
             int(definition.restitutionMilli)},
            {QStringLiteral("chains"), chains}
        });
    }
    QJsonArray bindings;
    for (const WorkoutGameAssetPhysicsBinding &binding : snapshot.bindings) {
        bindings.append(QJsonObject {
            {QStringLiteral("assetId"), binding.assetId},
            {QStringLiteral("variantKey"), binding.variantKey},
            {QStringLiteral("definitionIndex"),
             double(binding.definitionIndex)},
            {QStringLiteral("nativeForwardOriginMm"),
             binding.nativeForwardOriginMm},
            {QStringLiteral("nativeForwardExtentMm"),
             double(binding.nativeForwardExtentMm)},
            {QStringLiteral("nativeUpExtentMm"),
             double(binding.nativeUpExtentMm)},
            {QStringLiteral("resolvedExtentMm"),
             double(binding.resolvedExtentMm)}
        });
    }
    QJsonArray legacyFt02Records;
    for (const WorkoutGameLegacyFt02Record &record :
            snapshot.legacyFt02Records) {
        legacyFt02Records.append(QJsonObject {
            {QStringLiteral("recordVersion"), double(record.recordVersion)},
            {QStringLiteral("enabled"), record.enabled},
            {QStringLiteral("startMetersBinary64"),
             WorkoutGameLegacyBinary64::encode(record.startMeters)},
            {QStringLiteral("endMetersBinary64"),
             WorkoutGameLegacyBinary64::encode(record.endMeters)},
            {QStringLiteral("heightMetersBinary64"),
             WorkoutGameLegacyBinary64::encode(record.heightMeters)},
            {QStringLiteral("obstacleAnchorMetersBinary64"),
             WorkoutGameLegacyBinary64::encode(
                 record.obstacleAnchorMeters)}
        });
    }
    QJsonArray pieceBindings;
    for (const WorkoutGameAssetPhysicsPieceBinding &binding :
            snapshot.pieceBindings) {
        pieceBindings.append(QJsonObject {
            {QStringLiteral("definitionIndex"), double(binding.definitionIndex)},
            {QStringLiteral("bindingIndex"), double(binding.bindingIndex)},
            {QStringLiteral("legacyFt02RecordIndex"),
             double(binding.legacyFt02RecordIndex)},
            {QStringLiteral("obstacleAnchorMm"), binding.obstacleAnchorMm},
            {QStringLiteral("obstacleAnchorMicrometerRemainder"),
             binding.obstacleAnchorMicrometerRemainder},
            {QStringLiteral("flags"), double(binding.flags)}
        });
    }
    return {
        {QStringLiteral("snapshotVersion"), double(snapshot.snapshotVersion)},
        {QStringLiteral("catalogSchemaVersion"), double(snapshot.catalogSchemaVersion)},
        {QStringLiteral("physicsDefinitions"), definitions},
        {QStringLiteral("bindings"), bindings},
        {QStringLiteral("legacyFt02Records"), legacyFt02Records},
        {QStringLiteral("pieceBindings"), pieceBindings}
    };
}

WorkoutGameCourseDocumentStatus snapshotStatusToDocumentStatus(
        WorkoutGameAssetPhysicsSnapshotValidationStatus status)
{
    switch (status) {
    case WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready:
        return WorkoutGameCourseDocumentStatus::Ready;
    case WorkoutGameAssetPhysicsSnapshotValidationStatus::UnsupportedVersion:
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    case WorkoutGameAssetPhysicsSnapshotValidationStatus::ResourceLimit:
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    case WorkoutGameAssetPhysicsSnapshotValidationStatus::InvalidSnapshot:
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    return WorkoutGameCourseDocumentStatus::InvalidDocument;
}

WorkoutGameCourseDocumentStatus parseAssetPhysicsSnapshot(
        const QJsonValue &value,
        std::size_t roadPieceCount,
        std::shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>
            &destination)
{
    if (!value.isObject()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonObject object = value.toObject();
    auto snapshot = std::make_shared<WorkoutGameCourseAssetPhysicsSnapshot>();
    if (!unsignedNumber(
                object, "snapshotVersion", snapshot->snapshotVersion)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (snapshot->snapshotVersion
            != WorkoutGameCourseAssetPhysicsSnapshot::CurrentVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    if (!unsignedNumber(object, "catalogSchemaVersion", snapshot->catalogSchemaVersion)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (snapshot->catalogSchemaVersion > 1) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    if (object.size() != 6) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonValue definitionsValue =
            object.value(QStringLiteral("physicsDefinitions"));
    const QJsonValue bindingsValue = object.value(QStringLiteral("bindings"));
    const QJsonValue legacyFt02RecordsValue = object.value(
            QStringLiteral("legacyFt02Records"));
    const QJsonValue pieceBindingsValue =
            object.value(QStringLiteral("pieceBindings"));
    if (!definitionsValue.isArray() || !bindingsValue.isArray()
            || !legacyFt02RecordsValue.isArray()
            || !pieceBindingsValue.isArray()) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    const QJsonArray definitions = definitionsValue.toArray();
    const QJsonArray bindings = bindingsValue.toArray();
    const QJsonArray legacyFt02Records = legacyFt02RecordsValue.toArray();
    const QJsonArray pieceBindings = pieceBindingsValue.toArray();
    if (definitions.size()
                > int(WorkoutGameCourseAssetPhysicsSnapshot
                    ::MaximumDefinitions)
            || bindings.size()
                > int(WorkoutGameCourseAssetPhysicsSnapshot::MaximumBindings)
            || legacyFt02Records.size()
                > int(WorkoutGameCourseAssetPhysicsSnapshot
                    ::MaximumLegacyRecords)
            || pieceBindings.size()
                > int(WorkoutGameCourseAssetPhysicsSnapshot
                    ::MaximumPieceBindings)) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    if (legacyFt02Records.size()
            > int(WorkoutGameCourseAssetPhysicsSnapshot
                ::MaximumLegacyScalarFields / 4)) {
        return WorkoutGameCourseDocumentStatus::ResourceLimit;
    }
    std::size_t totalPoints = 0;
    snapshot->physicsDefinitions.reserve(std::size_t(definitions.size()));
    for (const QJsonValue &definitionValue : definitions) {
        if (!definitionValue.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const QJsonObject definitionObject = definitionValue.toObject();
        const QJsonValue operation =
                definitionObject.value(QStringLiteral("operation"));
        const QJsonValue chainsValue =
                definitionObject.value(QStringLiteral("chains"));
        WorkoutGameAssetPhysicsDefinition definition;
        std::uint32_t friction = 0;
        std::uint32_t restitution = 0;
        if (!unsignedNumber(definitionObject, "profileVersion",
                            definition.profileVersion)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        if (definition.profileVersion != WorkoutGameAssetPhysicsDefinition::CurrentProfileVersion) {
            return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
        }
        if (definitionObject.size() != 5 || !operation.isString()
                || !parseAssetPhysicsOperation(
                    operation.toString(), definition.operation)
                || !unsignedNumber(definitionObject,
                    "coulombFrictionMilli", friction)
                || friction > std::numeric_limits<std::uint16_t>::max()
                || !unsignedNumber(definitionObject,
                    "restitutionMilli", restitution)
                || restitution > std::numeric_limits<std::uint16_t>::max()
                || !chainsValue.isArray()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        definition.coulombFrictionMilli = std::uint16_t(friction);
        definition.restitutionMilli = std::uint16_t(restitution);
        const QJsonArray chains = chainsValue.toArray();
        if (chains.size() > int(WorkoutGameCourseAssetPhysicsSnapshot
                    ::MaximumChainsPerDefinition)) {
            return WorkoutGameCourseDocumentStatus::ResourceLimit;
        }
        definition.chains.reserve(std::size_t(chains.size()));
        std::size_t definitionPoints = 0;
        for (const QJsonValue &chainValue : chains) {
            if (!chainValue.isObject() || chainValue.toObject().size() != 1) {
                return WorkoutGameCourseDocumentStatus::InvalidDocument;
            }
            const QJsonValue pointsValue = chainValue.toObject().value(
                    QStringLiteral("points"));
            if (!pointsValue.isArray()) {
                return WorkoutGameCourseDocumentStatus::InvalidDocument;
            }
            const QJsonArray points = pointsValue.toArray();
            definitionPoints += std::size_t(points.size());
            totalPoints += std::size_t(points.size());
            if (definitionPoints > WorkoutGameCourseAssetPhysicsSnapshot
                        ::MaximumPointsPerDefinition
                    || totalPoints > WorkoutGameCourseAssetPhysicsSnapshot
                        ::MaximumTotalPoints) {
                return WorkoutGameCourseDocumentStatus::ResourceLimit;
            }
            WorkoutGameAssetPhysicsChain chain;
            chain.points.reserve(std::size_t(points.size()));
            for (const QJsonValue &pointValue : points) {
                if (!pointValue.isObject()) {
                    return WorkoutGameCourseDocumentStatus::InvalidDocument;
                }
                WorkoutGameAssetPhysicsPoint point;
                const QJsonObject pointObject = pointValue.toObject();
                if (pointObject.size() != 2 || !signed32Number(
                            pointObject, "forwardMm", point.forwardMm)
                        || !signed32Number(
                            pointObject, "heightMm", point.heightMm)) {
                    return WorkoutGameCourseDocumentStatus::InvalidDocument;
                }
                chain.points.push_back(point);
            }
            definition.chains.push_back(std::move(chain));
        }
        snapshot->physicsDefinitions.push_back(std::move(definition));
    }
    snapshot->bindings.reserve(std::size_t(bindings.size()));
    for (const QJsonValue &bindingValue : bindings) {
        if (!bindingValue.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const QJsonObject bindingObject = bindingValue.toObject();
        const QJsonValue assetId =
                bindingObject.value(QStringLiteral("assetId"));
        const QJsonValue variantKey =
                bindingObject.value(QStringLiteral("variantKey"));
        WorkoutGameAssetPhysicsBinding binding;
        if (bindingObject.size() != 7 || !assetId.isString() || !variantKey.isString()
                || !unsignedNumber(bindingObject, "definitionIndex",
                    binding.definitionIndex)
                || !signed32Number(bindingObject, "nativeForwardOriginMm",
                    binding.nativeForwardOriginMm)
                || !unsignedNumber(bindingObject, "nativeForwardExtentMm",
                    binding.nativeForwardExtentMm)
                || !unsignedNumber(bindingObject, "nativeUpExtentMm",
                    binding.nativeUpExtentMm)
                || !unsignedNumber(bindingObject, "resolvedExtentMm",
                    binding.resolvedExtentMm)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        binding.assetId = assetId.toString();
        binding.variantKey = variantKey.toString();
        snapshot->bindings.push_back(std::move(binding));
    }
    snapshot->legacyFt02Records.reserve(
            std::size_t(legacyFt02Records.size()));
    for (const QJsonValue &recordValue : legacyFt02Records) {
        if (!recordValue.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const QJsonObject recordObject = recordValue.toObject();
        WorkoutGameLegacyFt02Record record;
        if (!unsignedNumber(
                    recordObject, "recordVersion", record.recordVersion)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        if (record.recordVersion
                != WorkoutGameLegacyFt02Record::CurrentVersion) {
            return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
        }
        const QJsonValue enabled = recordObject.value(
                QStringLiteral("enabled"));
        const QJsonValue start = recordObject.value(
                QStringLiteral("startMetersBinary64"));
        const QJsonValue end = recordObject.value(
                QStringLiteral("endMetersBinary64"));
        const QJsonValue height = recordObject.value(
                QStringLiteral("heightMetersBinary64"));
        const QJsonValue anchor = recordObject.value(
                QStringLiteral("obstacleAnchorMetersBinary64"));
        if (recordObject.size() != 6 || !enabled.isBool()
                || !start.isString() || !end.isString()
                || !height.isString() || !anchor.isString()
                || !WorkoutGameLegacyBinary64::decode(
                    start.toString(), record.startMeters)
                || !WorkoutGameLegacyBinary64::decode(
                    end.toString(), record.endMeters)
                || !WorkoutGameLegacyBinary64::decode(
                    height.toString(), record.heightMeters)
                || !WorkoutGameLegacyBinary64::decode(
                    anchor.toString(), record.obstacleAnchorMeters)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        record.enabled = enabled.toBool();
        snapshot->legacyFt02Records.push_back(record);
    }
    snapshot->pieceBindings.reserve(std::size_t(pieceBindings.size()));
    for (const QJsonValue &pieceBindingValue : pieceBindings) {
        if (!pieceBindingValue.isObject()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        WorkoutGameAssetPhysicsPieceBinding binding;
        const QJsonObject bindingObject = pieceBindingValue.toObject();
        std::int32_t anchorRemainder = 0;
        if (bindingObject.size() != 6
                || !unsignedNumber(bindingObject, "definitionIndex", binding.definitionIndex)
                || !unsignedNumber(bindingObject, "bindingIndex",
                            binding.bindingIndex)
                || !unsignedNumber(bindingObject, "legacyFt02RecordIndex",
                            binding.legacyFt02RecordIndex)
                || !signed32Number(bindingObject, "obstacleAnchorMm",
                    binding.obstacleAnchorMm)
                || !signed32Number(bindingObject,
                    "obstacleAnchorMicrometerRemainder", anchorRemainder)
                || anchorRemainder < -500 || anchorRemainder > 500
                || !unsignedNumber(bindingObject, "flags", binding.flags)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        binding.obstacleAnchorMicrometerRemainder =
                std::int16_t(anchorRemainder);
        snapshot->pieceBindings.push_back(binding);
    }
    const auto validation = WorkoutGameAssetPhysicsSnapshotValidator::validate(
            *snapshot, roadPieceCount);
    const auto status = snapshotStatusToDocumentStatus(validation);
    if (status == WorkoutGameCourseDocumentStatus::Ready) {
        // The shape, array bounds, strings and scalars have now been checked.
        // Only a bounded known snapshot reaches the encoded-size calculation.
        if (QJsonDocument(object).toJson(QJsonDocument::Compact).size()
                > WorkoutGameCourseAssetPhysicsSnapshot::MaximumEncodedBytes) {
            return WorkoutGameCourseDocumentStatus::ResourceLimit;
        }
        destination = std::move(snapshot);
    }
    return status;
}

QJsonObject roadPieceToJson(
        const WorkoutGameRoadPiece &piece,
        std::uint32_t generationVersion)
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
    if (generationVersion
            >= WorkoutGameRoadPlan::BankAndReliefGenerationVersion) {
        result.insert(QStringLiteral("bank"), bankToJson(piece.bank));
        result.insert(QStringLiteral("relief"), reliefToJson(piece.relief));
    }
    return result;
}

bool parseRoadPiece(
        const QJsonObject &object,
        std::uint32_t generationVersion,
        WorkoutGameRoadPiece &piece)
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
    const QJsonValue bank = object.value(QStringLiteral("bank"));
    const QJsonValue relief = object.value(QStringLiteral("relief"));
    if (generationVersion
            >= WorkoutGameRoadPlan::BankAndReliefGenerationVersion) {
        if (!parseBank(bank, piece.bank)
                || !parseRelief(relief, piece.relief)) {
            return false;
        }
    } else if (!bank.isUndefined() || !relief.isUndefined()) {
        return false;
    }
    return true;
}

QJsonObject roadPlanToJson(
        const WorkoutGameRoadPlan &plan,
        bool includeAssetPhysicsSnapshot)
{
    const std::uint32_t encodedGeneration = includeAssetPhysicsSnapshot
            ? plan.generationVersion
            : std::min(plan.generationVersion,
                WorkoutGameRoadPlan::BankAndReliefGenerationVersion);
    QJsonArray pieces;
    for (const WorkoutGameRoadPiece &piece : plan.pieces) {
        pieces.append(roadPieceToJson(piece, encodedGeneration));
    }
    QJsonObject object {
        {QStringLiteral("generationVersion"),
         double(encodedGeneration)},
        {QStringLiteral("pieces"), pieces}
    };
    if (includeAssetPhysicsSnapshot && plan.assetPhysicsSnapshot) {
        object.insert(QStringLiteral("assetPhysicsSnapshot"),
                      assetPhysicsSnapshotToJson(
                          *plan.assetPhysicsSnapshot));
    }
    return object;
}

WorkoutGameCourseDocumentStatus parseRoadPlan(
        const QJsonValue &value,
        int schemaVersion,
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
    if (generationVersion != WorkoutGameRoadPlan::LegacyGenerationVersion
            && generationVersion
                != WorkoutGameRoadPlan::BankAndReliefGenerationVersion
            && generationVersion
                != WorkoutGameRoadPlan::CurrentGenerationVersion) {
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
    }
    if ((schemaVersion >= WorkoutGameCourseDocumentCodec::AssetPhysicsSchemaVersion
                && generationVersion
                    != WorkoutGameRoadPlan::CurrentGenerationVersion)
            || (schemaVersion
                    < WorkoutGameCourseDocumentCodec::AssetPhysicsSchemaVersion
                && generationVersion
                    == WorkoutGameRoadPlan::CurrentGenerationVersion)) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
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
                || !parseRoadPiece(
                    pieceValue.toObject(), generationVersion, piece)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        plan->pieces.push_back(piece);
    }
    const QJsonValue snapshotValue =
            object.value(QStringLiteral("assetPhysicsSnapshot"));
    if (schemaVersion >= WorkoutGameCourseDocumentCodec::AssetPhysicsSchemaVersion) {
        const WorkoutGameCourseDocumentStatus snapshotStatus =
                parseAssetPhysicsSnapshot(
                    snapshotValue, plan->pieces.size(),
                    plan->assetPhysicsSnapshot);
        if (snapshotStatus != WorkoutGameCourseDocumentStatus::Ready) {
            return snapshotStatus;
        }
    } else {
        if (!snapshotValue.isUndefined()) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        plan->assetPhysicsSnapshot =
                WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(*plan);
        if (!plan->assetPhysicsSnapshot) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
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
    const auto hasBankAndReliefRoadPlan = [&destination]() {
        return destination.course.roadPlan
                && destination.course.roadPlan->generationVersion
                    >= WorkoutGameRoadPlan::BankAndReliefGenerationVersion;
    };
    if (destination.schemaVersion
            == WorkoutGameCourseDocumentCodec::CurrentSchemaVersion) {
        if (!WorkoutGameCourseDocumentCodec::valid(destination)) return false;
        return true;
    }
    else if ((destination.schemaVersion != 1
                && destination.schemaVersion != 2
                && destination.schemaVersion != 3
                && destination.schemaVersion != 4
                && destination.schemaVersion != 5
                && destination.schemaVersion != 6)
            || !WorkoutGameCourseDocumentCodec::valid(destination)) {
        return false;
    }
    if (hasBankAndReliefRoadPlan()) {
        auto upgradedPlan = std::make_shared<WorkoutGameRoadPlan>(
                *destination.course.roadPlan);
        upgradedPlan->generationVersion =
                WorkoutGameRoadPlan::CurrentGenerationVersion;
        if (!upgradedPlan->assetPhysicsSnapshot) {
            upgradedPlan->assetPhysicsSnapshot =
                    WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(
                        *upgradedPlan);
        }
        destination.course.roadPlan = std::move(upgradedPlan);
        destination.schemaVersion =
                WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
        destination.sourceSha256.clear();
        if (source.schemaVersion < 3) {
            destination.conversionAlgorithmVersion =
                    WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion;
            destination.prescriptionMetadata =
                    WorkoutGameCoursePrescriptionMetadata();
        }
        return WorkoutGameCourseDocumentCodec::valid(destination);
    }
    const WorkoutGameCourse visual =
            WorkoutGameDistancePlayback::visualCourse(destination.course);
    const WorkoutGameCoursePreset roadPreset =
            destination.conversionAlgorithmVersion
                    >= 4
                ? destination.preset
                : WorkoutGameCoursePreset::WorkoutFirst;
    const WorkoutGameRoadPlan plan =
            WorkoutGameRoadCourseBuilder::generatePlan(
                visual, destination.ftpWatts, {
                    WorkoutGameRoadCourseGenerationParameters::CurrentVersion,
                    roadPreset
                });
    if (WorkoutGameRoadPlanValidator::validate(
                plan, destination.course.sections.size())
            != WorkoutGameRoadPlanValidationStatus::Ready
            || !WorkoutGameRoadQuality::audit(plan).accepted()) {
        return false;
    }
    destination.schemaVersion =
            WorkoutGameCourseDocumentCodec::CurrentSchemaVersion;
    destination.sourceSha256.clear();
    if (source.schemaVersion < 3) {
        destination.conversionAlgorithmVersion =
                WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion;
        destination.prescriptionMetadata =
                WorkoutGameCoursePrescriptionMetadata();
    }
    destination.course.roadPlan =
            std::make_shared<const WorkoutGameRoadPlan>(plan);
    auto persistedPlan = std::make_shared<WorkoutGameRoadPlan>(
            *destination.course.roadPlan);
    persistedPlan->generationVersion =
            WorkoutGameRoadPlan::CurrentGenerationVersion;
    persistedPlan->assetPhysicsSnapshot =
            WorkoutGameAssetPhysicsSnapshotBuilder::legacyFor(
                *persistedPlan);
    destination.course.roadPlan = std::move(persistedPlan);
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
    if (document.schemaVersion < AssetPhysicsSchemaVersion
            && document.course.roadPlan && document.course.roadPlan->assetPhysicsSnapshot) {
        const auto &snapshot = *document.course.roadPlan->assetPhysicsSnapshot;
        // An older writer may omit only the adapter metadata reconstructed on
        // legacy reads. Never silently discard explicitly resolved geometry.
        if (snapshot.catalogSchemaVersion != 0 || !snapshot.physicsDefinitions.empty()
                || !snapshot.bindings.empty()
                || !snapshot.legacyFt02Records.empty()
                || !std::all_of(snapshot.pieceBindings.begin(), snapshot.pieceBindings.end(),
                    [](const auto &piece) {
                        return piece.flags
                                    == WorkoutGameCourseAssetPhysicsSnapshot
                                        ::LegacyProceduralV1
                                && piece.legacyFt02RecordIndex
                                    == WorkoutGameCourseAssetPhysicsSnapshot
                                        ::NoIndex;
                    })) return false;
    }
    static const QRegularExpression sha256Pattern(
            QStringLiteral("^[0-9a-f]{64}$"));
    const QFileInfo sourceInfo(document.sourceFileName);
    const auto validAnnotationText = [](const QString &value,
                                        qsizetype maximumLength,
                                        bool allowEmpty) {
        return (allowEmpty || !value.trimmed().isEmpty())
                && value.size() <= maximumLength
                && !value.contains(QLatin1Char('\n'))
                && !value.contains(QLatin1Char('\r'));
    };
    bool sourceAnnotationsValid =
            document.sourceLaps.size() <= MaximumSourceAnnotations
            && document.sourceTexts.size() <= MaximumSourceAnnotations;
    std::int64_t previousLapMs = 0;
    for (const WorkoutGameCourseSourceLap &lap : document.sourceLaps) {
        if (lap.timeMs < previousLapMs || lap.timeMs < 0
                || lap.timeMs > document.course.nominalDurationMs
                || !validAnnotationText(lap.name, 200, true)) {
            sourceAnnotationsValid = false;
            break;
        }
        previousLapMs = lap.timeMs;
    }
    std::int64_t previousTextMs = 0;
    for (const WorkoutGameCourseSourceText &text : document.sourceTexts) {
        if (text.timeMs < previousTextMs || text.timeMs < 0
                || text.timeMs > document.course.nominalDurationMs
                || text.durationSeconds < 0 || text.durationSeconds > 3600
                || !validAnnotationText(text.text, 500, false)) {
            sourceAnnotationsValid = false;
            break;
        }
        previousTextMs = text.timeMs;
    }
    bool sourceIntervalsValid = document.sourceIntervals.empty();
    if (!document.sourceIntervals.empty()) {
        if (document.schemaVersion < 3) {
            sourceIntervalsValid = validIntervals(
                    document.sourceIntervals, document.course.nominalDurationMs);
        } else {
            const WorkoutGameCoursePrescriptionAudit audit =
                    WorkoutGameCoursePrescription::audit(
                        document.sourceIntervals,
                        generatedIntervals(
                            document.course, document.sourceIntervals),
                        document.ftpWatts,
                        document.preset,
                        document.prescriptionMetadata);
            sourceIntervalsValid = audit.status
                    == WorkoutGameCoursePrescriptionStatus::Ready;
            if (sourceIntervalsValid && document.schemaVersion < 5) {
                const WorkoutGameCourseModeContract contract =
                        WorkoutGameCoursePrescription::contractFor(
                            document.preset);
                std::vector<std::int64_t> minimumExposureMs(
                        document.sourceIntervals.size(), 0);
                for (const WorkoutGameDistanceCourseSection &section :
                        document.course.sections) {
                    const std::size_t sourceIndex = sourceIntervalAt(
                            document.sourceIntervals, section.sourceStartMs);
                    if (sourceIndex >= minimumExposureMs.size()) {
                        sourceIntervalsValid = false;
                        break;
                    }
                    minimumExposureMs[sourceIndex] += section.minimumDurationMs;
                }
                for (std::size_t index = 0;
                        sourceIntervalsValid
                            && index < document.sourceIntervals.size(); ++index) {
                    const bool prescribedRecovery =
                            WorkoutGameCoursePrescription::isRecovery(
                                document.sourceIntervals[index],
                                document.ftpWatts)
                            && WorkoutGameCoursePrescription::roleAt(
                                document.prescriptionMetadata, index)
                                == WorkoutGameCourseIntervalRole::Prescribed;
                    if (prescribedRecovery
                            && double(minimumExposureMs[index]) + 1.0
                                < double(document.sourceIntervals[index]
                                            .durationMs)
                                    * contract.minimumRecoveryExposure) {
                        sourceIntervalsValid = false;
                        break;
                    }
                }
                if (sourceIntervalsValid && document.course.roadPlan) {
                    for (const WorkoutGameRoadPiece &piece
                            : document.course.roadPlan->pieces) {
                        if (piece.sourceSectionIndex
                                >= document.course.sections.size()) {
                            sourceIntervalsValid = false;
                            break;
                        }
                        if (document.conversionAlgorithmVersion >= 4) continue;
                        const std::size_t sourceIndex = sourceIntervalAt(
                                document.sourceIntervals,
                                document.course.sections[
                                    piece.sourceSectionIndex].sourceStartMs);
                        if (sourceIndex >= document.sourceIntervals.size()
                                || (WorkoutGameCoursePrescription::isRecovery(
                                        document.sourceIntervals[sourceIndex],
                                        document.ftpWatts)
                                    && piece.challenge.enabled)) {
                            sourceIntervalsValid = false;
                            break;
                        }
                    }
                }
            }
        }
    } else if (document.prescriptionMetadata.present()) {
        sourceIntervalsValid = false;
    }
    const bool sourceAnnotationsAllowed =
            document.schemaVersion >= 4
            || (document.sourceLaps.empty() && document.sourceTexts.empty());
    const int maximumAlgorithmVersion = document.schemaVersion == 3
            ? 2 : document.schemaVersion == 4
                ? 5 : WorkoutGameCourseDocument::CurrentConversionAlgorithmVersion;
    const bool assetPhysicsRoadPlanValid = document.course.roadPlan
            && document.course.roadPlan->generationVersion
                == WorkoutGameRoadPlan::CurrentGenerationVersion
            && document.course.roadPlan->assetPhysicsSnapshot
            && WorkoutGameAssetPhysicsSnapshotValidator::validate(
                    *document.course.roadPlan->assetPhysicsSnapshot,
                    document.course.roadPlan->pieces.size())
                == WorkoutGameAssetPhysicsSnapshotValidationStatus::Ready
            && QJsonDocument(assetPhysicsSnapshotToJson(
                    *document.course.roadPlan->assetPhysicsSnapshot))
                    .toJson(QJsonDocument::Compact).size()
                <= WorkoutGameCourseAssetPhysicsSnapshot::MaximumEncodedBytes;
    const bool schemaValid = document.schemaVersion == 1
            ? !document.course.roadPlan
                && !document.prescriptionMetadata.present()
                && sourceAnnotationsAllowed
            : (document.schemaVersion == 2
                    || document.schemaVersion == 3
                    || document.schemaVersion == 4
                    || document.schemaVersion == 5
                    || document.schemaVersion == 6
                    || document.schemaVersion == AssetPhysicsSchemaVersion)
                && roadPlanMatchesCourse(document.course)
                && (document.schemaVersion != AssetPhysicsSchemaVersion
                    || assetPhysicsRoadPlanValid)
                && sourceAnnotationsAllowed
                && (document.schemaVersion == 2
                    ? !document.prescriptionMetadata.present()
                    : document.conversionAlgorithmVersion
                            >= WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion
                        && document.conversionAlgorithmVersion
                            <= maximumAlgorithmVersion);
    return schemaValid
            && !document.title.trimmed().isEmpty()
            && document.title.size() <= 200
            && !document.title.contains(QLatin1Char('\n'))
            && !document.title.contains(QLatin1Char('\r'))
            && !document.sourceFileName.isEmpty()
            && sourceInfo.fileName() == document.sourceFileName
            && !document.sourceFileName.contains(QLatin1Char('/'))
            && !document.sourceFileName.contains(QLatin1Char('\\'))
            && (document.schemaVersion >= 6
                || sha256Pattern.match(document.sourceSha256).hasMatch())
            && std::isfinite(document.ftpWatts)
            && document.ftpWatts > 0.0
            && document.ftpWatts <= 3000.0
            && sourceIntervalsValid
            && sourceAnnotationsValid
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
        {QStringLiteral("fileName"), document.sourceFileName}
    };
    if (document.schemaVersion < 6) {
        source.insert(QStringLiteral("sha256"), document.sourceSha256);
    }
    if (!document.sourceIntervals.empty()) {
        source.insert(QStringLiteral("intervals"),
                      intervalsToJson(document.sourceIntervals));
    }
    if (document.schemaVersion >= 4) {
        if (!document.sourceLaps.empty()) {
            source.insert(QStringLiteral("laps"),
                          sourceLapsToJson(document.sourceLaps));
        }
        if (!document.sourceTexts.empty()) {
            source.insert(QStringLiteral("texts"),
                          sourceTextsToJson(document.sourceTexts));
        }
    }
    QJsonObject conversion {
        {QStringLiteral("ftpWatts"), document.ftpWatts},
        {QStringLiteral("preset"), presetName(document.preset)},
        {QStringLiteral("parameters"), generationToJson(
            document.generationParameters)}
    };
    if (document.schemaVersion >= 3) {
        conversion.insert(QStringLiteral("algorithmVersion"),
                          document.conversionAlgorithmVersion);
        if (document.prescriptionMetadata.present()) {
            source.insert(QStringLiteral("prescriptionMetadata"),
                          prescriptionMetadataToJson(
                              document.prescriptionMetadata));
        }
    }
    QJsonObject root {
        {QStringLiteral("schemaVersion"), document.schemaVersion},
        {QStringLiteral("title"), document.title},
        {QStringLiteral("source"), source},
        {QStringLiteral("conversion"), conversion},
        {QStringLiteral("course"), courseToJson(document.course)}
    };
    if (document.schemaVersion >= 2) {
        root.insert(QStringLiteral("roadPlan"),
                    roadPlanToJson(
                        *document.course.roadPlan,
                        document.schemaVersion >= AssetPhysicsSchemaVersion));
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
    if (schemaVersion != 1 && schemaVersion != 2 && schemaVersion != 3
            && schemaVersion != 4 && schemaVersion != 5
            && schemaVersion != 6
            && schemaVersion != AssetPhysicsSchemaVersion) {
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
    document.conversionAlgorithmVersion =
            WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion;
    document.title = title.toString();
    if (!sourceFileName.isString()
            || (schemaVersion < 6 && !sourceSha256.isString())
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
    document.sourceSha256 = schemaVersion < 6
            ? sourceSha256.toString() : QString();
    if (document.schemaVersion >= 3) {
        std::int64_t algorithmVersion = 0;
        if (!integerNumber(
                    conversion, "algorithmVersion", algorithmVersion)) {
            return WorkoutGameCourseDocumentStatus::InvalidDocument;
        }
        const int maximumAlgorithmVersion = document.schemaVersion == 3
                ? 2 : document.schemaVersion == 4
                    ? 5
                    : WorkoutGameCourseDocument::CurrentConversionAlgorithmVersion;
        if (algorithmVersion
                    < WorkoutGameCourseDocument::LegacyConversionAlgorithmVersion
                || algorithmVersion
                    > maximumAlgorithmVersion) {
            return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
        }
        document.conversionAlgorithmVersion = int(algorithmVersion);
        const WorkoutGameCourseDocumentStatus metadataStatus =
                parsePrescriptionMetadata(
                    source.value(QStringLiteral("prescriptionMetadata")),
                    document.prescriptionMetadata);
        if (metadataStatus != WorkoutGameCourseDocumentStatus::Ready) {
            return metadataStatus;
        }
    } else if (conversion.contains(QStringLiteral("algorithmVersion"))
            || source.contains(QStringLiteral("prescriptionMetadata"))) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
    if (document.schemaVersion >= 4) {
        const WorkoutGameCourseDocumentStatus lapsStatus = parseSourceLaps(
                source.value(QStringLiteral("laps")), document.sourceLaps);
        if (lapsStatus != WorkoutGameCourseDocumentStatus::Ready) {
            return lapsStatus;
        }
        const WorkoutGameCourseDocumentStatus textsStatus = parseSourceTexts(
                source.value(QStringLiteral("texts")), document.sourceTexts);
        if (textsStatus != WorkoutGameCourseDocumentStatus::Ready) {
            return textsStatus;
        }
    } else if (source.contains(QStringLiteral("laps"))
            || source.contains(QStringLiteral("texts"))) {
        return WorkoutGameCourseDocumentStatus::InvalidDocument;
    }
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
                roadPlan, document.schemaVersion,
                document.course.sections.size(),
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
    if (document.schemaVersion > WorkoutGameCourseDocumentCodec::CurrentSchemaVersion) {
        // Codec preparation does not imply support by playback consumers.
        // Enable this format only with the resolver and legacy parity work.
        document = WorkoutGameCourseDocument();
        error = QStringLiteral("Asset physics course playback is not supported by this build");
        return WorkoutGameCourseDocumentStatus::UnsupportedVersion;
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
