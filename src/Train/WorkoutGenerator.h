/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGenerator_h
#define _GC_WorkoutGenerator_h

#include <QByteArray>
#include <QString>
#include <QVector>

#include <vector>

enum class WorkoutTrainingFocus
{
    Endurance,
    Tempo,
    SweetSpot,
    Threshold,
    Vo2Max,
    Anaerobic,
    Sprint,
    AnaerobicCapacity20_20
};

enum class WorkoutGeneratedIntervalRole
{
    Warmup,
    Primer,
    Work,
    Recovery,
    BlockRecovery,
    Cooldown
};

struct WorkoutGenerationSettings
{
    WorkoutTrainingFocus focus = WorkoutTrainingFocus::Endurance;
    int ftpWatts = 190;

    int warmupSeconds = 10 * 60;
    double warmupStartPercentFtp = 50.0;
    double warmupEndPercentFtp = 70.0;
    int primerSeconds = 0;
    double primerPercentFtp = 90.0;
    int preWorkRecoverySeconds = 0;

    double workPercentFtp = 68.0;
    double recoveryPercentFtp = 55.0;
    int workSeconds = 45 * 60;
    int recoverySeconds = 0;
    int repetitionsPerBlock = 1;
    int blockCount = 1;
    int repetitionDeltaPerBlock = 0;
    int blockRecoverySeconds = 0;
    int lastBlockRecoverySeconds = 0;
    bool includeRecoveryAfterLastRep = false;

    int cooldownSeconds = 5 * 60;
    double cooldownStartPercentFtp = 55.0;
    double cooldownEndPercentFtp = 45.0;
};

struct WorkoutGeneratedInterval
{
    int durationSeconds = 0;
    double startPercentFtp = 0.0;
    double endPercentFtp = 0.0;
    WorkoutGeneratedIntervalRole role = WorkoutGeneratedIntervalRole::Work;
};

enum class WorkoutGenerationStatus
{
    Ready,
    InvalidSettings,
    TooManyIntervals,
    TooLong
};

struct WorkoutGenerationSummary
{
    int ftpWatts = 0;
    int durationSeconds = 0;
    int workIntervalCount = 0;
    double averagePercentFtp = 0.0;
    double averageWatts = 0.0;
    double estimatedStress = 0.0;
    QVector<int> repetitionsByBlock;
};

struct WorkoutGenerationResult
{
    WorkoutGenerationStatus status = WorkoutGenerationStatus::InvalidSettings;
    QString error;
    std::vector<WorkoutGeneratedInterval> intervals;
    WorkoutGenerationSummary summary;
};

class WorkoutGenerator
{
public:
    static QVector<WorkoutTrainingFocus> focuses();
    static QString focusName(WorkoutTrainingFocus focus);
    static WorkoutGenerationSettings defaultsFor(WorkoutTrainingFocus focus);
    static WorkoutGenerationResult generate(
            const WorkoutGenerationSettings &settings);
    static QByteArray mrcCourseData(const WorkoutGenerationResult &result);
};

#endif // _GC_WorkoutGenerator_h
