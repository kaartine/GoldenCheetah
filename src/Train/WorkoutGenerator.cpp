/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGenerator.h"

#include <QCoreApplication>
#include <QIODevice>
#include <QLocale>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace {

constexpr int MaximumWorkoutSeconds = 6 * 60 * 60;
constexpr std::size_t MaximumIntervalCount = 1000;

QString translated(const char *text)
{
    return QCoreApplication::translate("WorkoutGenerator", text);
}

bool validFocus(WorkoutTrainingFocus focus)
{
    switch (focus) {
    case WorkoutTrainingFocus::Endurance:
    case WorkoutTrainingFocus::Tempo:
    case WorkoutTrainingFocus::SweetSpot:
    case WorkoutTrainingFocus::Threshold:
    case WorkoutTrainingFocus::Vo2Max:
    case WorkoutTrainingFocus::Anaerobic:
    case WorkoutTrainingFocus::Sprint:
    case WorkoutTrainingFocus::AnaerobicCapacity20_20:
        return true;
    }
    return false;
}

bool validPercent(double value)
{
    return std::isfinite(value) && value >= 20.0 && value <= 250.0;
}

bool validSeconds(int value, int maximum)
{
    return value >= 0 && value <= maximum;
}

WorkoutGenerationResult invalid(const QString &error)
{
    WorkoutGenerationResult result;
    result.error = error;
    return result;
}

void appendInterval(
        WorkoutGenerationResult &result,
        int seconds,
        double startPercent,
        double endPercent,
        WorkoutGeneratedIntervalRole role)
{
    if (seconds <= 0) return;
    result.intervals.push_back({seconds, startPercent, endPercent, role});
    result.summary.durationSeconds += seconds;
    if (role == WorkoutGeneratedIntervalRole::Work) {
        ++result.summary.workIntervalCount;
    }
}

void calculateSummary(WorkoutGenerationResult &result)
{
    double percentSeconds = 0.0;
    double stress = 0.0;
    for (const WorkoutGeneratedInterval &interval : result.intervals) {
        const double averagePercent =
                (interval.startPercentFtp + interval.endPercentFtp) * 0.5;
        percentSeconds += averagePercent * interval.durationSeconds;

        const double start = interval.startPercentFtp / 100.0;
        const double end = interval.endPercentFtp / 100.0;
        const double meanSquare =
                (start * start + start * end + end * end) / 3.0;
        stress += 100.0 * (double(interval.durationSeconds) / 3600.0)
                * meanSquare;
    }
    if (result.summary.durationSeconds > 0) {
        result.summary.averagePercentFtp = percentSeconds
                / result.summary.durationSeconds;
        result.summary.averageWatts = result.summary.averagePercentFtp
                * result.summary.ftpWatts / 100.0;
    }
    result.summary.estimatedStress = stress;
}

} // namespace

QVector<WorkoutTrainingFocus> WorkoutGenerator::focuses()
{
    return {
        WorkoutTrainingFocus::Endurance,
        WorkoutTrainingFocus::Tempo,
        WorkoutTrainingFocus::SweetSpot,
        WorkoutTrainingFocus::Threshold,
        WorkoutTrainingFocus::Vo2Max,
        WorkoutTrainingFocus::Anaerobic,
        WorkoutTrainingFocus::Sprint,
        WorkoutTrainingFocus::AnaerobicCapacity20_20
    };
}

QString WorkoutGenerator::focusName(WorkoutTrainingFocus focus)
{
    switch (focus) {
    case WorkoutTrainingFocus::Endurance: return translated("Endurance");
    case WorkoutTrainingFocus::Tempo: return translated("Tempo");
    case WorkoutTrainingFocus::SweetSpot: return translated("Sweet spot");
    case WorkoutTrainingFocus::Threshold: return translated("Threshold");
    case WorkoutTrainingFocus::Vo2Max: return translated("VO2max");
    case WorkoutTrainingFocus::Anaerobic: return translated("Anaerobic");
    case WorkoutTrainingFocus::Sprint: return translated("Sprint");
    case WorkoutTrainingFocus::AnaerobicCapacity20_20:
        return translated("20/20 descending sets");
    }
    return translated("Unknown");
}

int WorkoutGenerator::wattsForPercent(int ftpWatts, double percentFtp)
{
    return int(std::lround(double(ftpWatts) * percentFtp / 100.0));
}

WorkoutGenerationSettings WorkoutGenerator::defaultsFor(
        WorkoutTrainingFocus focus)
{
    WorkoutGenerationSettings settings;
    settings.focus = focus;
    switch (focus) {
    case WorkoutTrainingFocus::Endurance:
        break;
    case WorkoutTrainingFocus::Tempo:
        settings.workPercentFtp = 82.0;
        settings.workSeconds = 15 * 60;
        settings.recoverySeconds = 4 * 60;
        settings.repetitionsPerBlock = 2;
        break;
    case WorkoutTrainingFocus::SweetSpot:
        settings.workPercentFtp = 90.0;
        settings.workSeconds = 10 * 60;
        settings.recoverySeconds = 5 * 60;
        settings.repetitionsPerBlock = 3;
        break;
    case WorkoutTrainingFocus::Threshold:
        settings.workPercentFtp = 100.0;
        settings.workSeconds = 8 * 60;
        settings.recoverySeconds = 4 * 60;
        settings.repetitionsPerBlock = 4;
        break;
    case WorkoutTrainingFocus::Vo2Max:
        settings.warmupSeconds = 12 * 60;
        settings.primerSeconds = 60;
        settings.primerPercentFtp = 100.0;
        settings.preWorkRecoverySeconds = 2 * 60;
        settings.workPercentFtp = 115.0;
        settings.workSeconds = 3 * 60;
        settings.recoverySeconds = 3 * 60;
        settings.repetitionsPerBlock = 5;
        settings.cooldownSeconds = 8 * 60;
        break;
    case WorkoutTrainingFocus::Anaerobic:
        settings.warmupSeconds = 12 * 60;
        settings.primerSeconds = 60;
        settings.primerPercentFtp = 110.0;
        settings.preWorkRecoverySeconds = 3 * 60;
        settings.workPercentFtp = 125.0;
        settings.workSeconds = 30;
        settings.recoverySeconds = 30;
        settings.repetitionsPerBlock = 6;
        settings.blockCount = 3;
        settings.blockRecoverySeconds = 5 * 60;
        settings.lastBlockRecoverySeconds = 5 * 60;
        settings.includeRecoveryAfterLastRep = true;
        settings.cooldownSeconds = 8 * 60;
        break;
    case WorkoutTrainingFocus::Sprint:
        settings.warmupSeconds = 15 * 60;
        settings.primerSeconds = 60;
        settings.primerPercentFtp = 120.0;
        settings.preWorkRecoverySeconds = 3 * 60;
        settings.workPercentFtp = 170.0;
        settings.recoveryPercentFtp = 50.0;
        settings.workSeconds = 10;
        settings.recoverySeconds = 50;
        settings.repetitionsPerBlock = 5;
        settings.blockCount = 3;
        settings.blockRecoverySeconds = 5 * 60;
        settings.lastBlockRecoverySeconds = 5 * 60;
        settings.includeRecoveryAfterLastRep = true;
        settings.cooldownSeconds = 10 * 60;
        break;
    case WorkoutTrainingFocus::AnaerobicCapacity20_20:
        settings.warmupSeconds = 60;
        settings.warmupStartPercentFtp = 65.0;
        settings.warmupEndPercentFtp = 65.0;
        settings.primerSeconds = 4 * 60;
        settings.primerPercentFtp = 80.0;
        settings.preWorkRecoverySeconds = 2 * 60;
        settings.workPercentFtp = 130.0;
        settings.recoveryPercentFtp = 55.0;
        settings.workSeconds = 20;
        settings.recoverySeconds = 20;
        settings.repetitionsPerBlock = 14;
        settings.blockCount = 4;
        settings.repetitionDeltaPerBlock = -2;
        settings.blockRecoverySeconds = 4 * 60;
        settings.lastBlockRecoverySeconds = 3 * 60;
        settings.includeRecoveryAfterLastRep = true;
        settings.cooldownSeconds = 6 * 60;
        settings.cooldownStartPercentFtp = 55.0;
        settings.cooldownEndPercentFtp = 55.0;
        break;
    }
    return settings;
}

WorkoutGenerationResult WorkoutGenerator::generate(
        const WorkoutGenerationSettings &settings)
{
    if (!validFocus(settings.focus)) {
        return invalid(translated("Unknown training focus."));
    }
    if (settings.ftpWatts < 50 || settings.ftpWatts > 600) {
        return invalid(translated("FTP must be between 50 and 600 watts."));
    }
    if (!validPercent(settings.warmupStartPercentFtp)
            || !validPercent(settings.warmupEndPercentFtp)
            || !validPercent(settings.primerPercentFtp)
            || !validPercent(settings.workPercentFtp)
            || !validPercent(settings.recoveryPercentFtp)
            || !validPercent(settings.cooldownStartPercentFtp)
            || !validPercent(settings.cooldownEndPercentFtp)) {
        return invalid(translated("Power targets must be between 20% and 250% of FTP."));
    }
    if (!validSeconds(settings.warmupSeconds, 60 * 60)
            || !validSeconds(settings.primerSeconds, 20 * 60)
            || !validSeconds(settings.preWorkRecoverySeconds, 30 * 60)
            || !validSeconds(settings.cooldownSeconds, 60 * 60)
            || settings.workSeconds < 5 || settings.workSeconds > 2 * 60 * 60
            || !validSeconds(settings.recoverySeconds, 60 * 60)
            || !validSeconds(settings.blockRecoverySeconds, 60 * 60)
            || !validSeconds(settings.lastBlockRecoverySeconds, 60 * 60)) {
        return invalid(translated("One or more interval durations are outside the supported range."));
    }
    if (settings.repetitionsPerBlock < 1
            || settings.repetitionsPerBlock > 100
            || settings.blockCount < 1 || settings.blockCount > 20) {
        return invalid(translated("Sets and repetitions are outside the supported range."));
    }

    WorkoutGenerationResult result;
    result.summary.ftpWatts = settings.ftpWatts;
    for (int block = 0; block < settings.blockCount; ++block) {
        const int repetitions = settings.repetitionsPerBlock
                + block * settings.repetitionDeltaPerBlock;
        if (repetitions < 1 || repetitions > 100) {
            return invalid(translated("The repetition progression produces an invalid set."));
        }
        result.summary.repetitionsByBlock.append(repetitions);
    }

    appendInterval(result, settings.warmupSeconds,
                   settings.warmupStartPercentFtp,
                   settings.warmupEndPercentFtp,
                   WorkoutGeneratedIntervalRole::Warmup);
    appendInterval(result, settings.primerSeconds,
                   settings.primerPercentFtp,
                   settings.primerPercentFtp,
                   WorkoutGeneratedIntervalRole::Primer);
    appendInterval(result, settings.preWorkRecoverySeconds,
                   settings.recoveryPercentFtp,
                   settings.recoveryPercentFtp,
                   WorkoutGeneratedIntervalRole::Recovery);

    for (int block = 0; block < settings.blockCount; ++block) {
        const int repetitions = result.summary.repetitionsByBlock.at(block);
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            appendInterval(result, settings.workSeconds,
                           settings.workPercentFtp,
                           settings.workPercentFtp,
                           WorkoutGeneratedIntervalRole::Work);
            const bool finalRepetition = repetition + 1 == repetitions;
            if (!finalRepetition || settings.includeRecoveryAfterLastRep) {
                appendInterval(result, settings.recoverySeconds,
                               settings.recoveryPercentFtp,
                               settings.recoveryPercentFtp,
                               WorkoutGeneratedIntervalRole::Recovery);
            }
        }
        if (block + 1 < settings.blockCount) {
            const bool beforeFinalBlock = block + 2 == settings.blockCount;
            const int seconds = beforeFinalBlock
                    ? settings.lastBlockRecoverySeconds
                    : settings.blockRecoverySeconds;
            appendInterval(result, seconds,
                           settings.recoveryPercentFtp,
                           settings.recoveryPercentFtp,
                           WorkoutGeneratedIntervalRole::BlockRecovery);
        }
    }

    appendInterval(result, settings.cooldownSeconds,
                   settings.cooldownStartPercentFtp,
                   settings.cooldownEndPercentFtp,
                   WorkoutGeneratedIntervalRole::Cooldown);

    if (result.intervals.size() > MaximumIntervalCount) {
        return {
            WorkoutGenerationStatus::TooManyIntervals,
            translated("The workout contains too many intervals."), {}, {}
        };
    }
    if (result.summary.durationSeconds > MaximumWorkoutSeconds) {
        return {
            WorkoutGenerationStatus::TooLong,
            translated("The workout is longer than six hours."), {}, {}
        };
    }

    calculateSummary(result);
    result.status = WorkoutGenerationStatus::Ready;
    return result;
}

QByteArray WorkoutGenerator::mrcCourseData(
        const WorkoutGenerationResult &result)
{
    if (result.status != WorkoutGenerationStatus::Ready
            || result.intervals.empty()) {
        return {};
    }

    QByteArray data;
    QTextStream stream(&data, QIODevice::WriteOnly);
    stream.setLocale(QLocale::c());
    stream.setRealNumberNotation(QTextStream::FixedNotation);
    stream.setRealNumberPrecision(4);
    stream << "[COURSE DATA]\n";
    double currentMinutes = 0.0;
    for (const WorkoutGeneratedInterval &interval : result.intervals) {
        stream << currentMinutes << ' ' << interval.startPercentFtp << '\n';
        currentMinutes += double(interval.durationSeconds) / 60.0;
        stream << currentMinutes << ' ' << interval.endPercentFtp << '\n';
    }
    stream << "[END COURSE DATA]\n";
    stream.flush();
    if (stream.status() != QTextStream::Ok) return {};
    return data;
}
