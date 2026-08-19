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
        {"skinny", WorkoutGameTerrainKind::Skinny}
    };
    for (const auto &value : values) {
        if (name == QLatin1String(value.first)) {
            terrain = value.second;
            return true;
        }
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
    return document.schemaVersion == CurrentSchemaVersion
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
    const QJsonObject root {
        {QStringLiteral("schemaVersion"), document.schemaVersion},
        {QStringLiteral("title"), document.title},
        {QStringLiteral("source"), source},
        {QStringLiteral("conversion"), conversion},
        {QStringLiteral("course"), courseToJson(document.course)}
    };
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
    if (schemaVersion != CurrentSchemaVersion) {
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
    const QByteArray metadata = WorkoutGameCourseDocumentCodec::encode(document);
    const QByteArray course = WorkoutGameCourseCrsExporter::encode(document);
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

    const QByteArray metadata = WorkoutGameCourseDocumentCodec::encode(document);
    const QByteArray course = WorkoutGameCourseCrsExporter::encode(document);
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
