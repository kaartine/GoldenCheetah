/*
 * Copyright (c) 2026 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#ifndef PLANBUNDLE_H
#define PLANBUNDLE_H

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>
#include <QPointer>
#include <QSet>

#include "RideFile.h"
#include "RideItem.h"
#include "Context.h"
#include "Season.h"


struct PlanExportDescription {
    QString name;
    QString author;
    QString sport;
    QString description;
    QString copyright;
    bool preferOriginal = true;
    QDate rangeStart;
    QDate rangeEnd;
    QStringList activityFiles;
    QString planFile;
};


class PlanMetadata {
public:
    static const QString ManifestFilename;
    static const QString ReadmeFilename;

    int bundleVersion = -1;
    QString name;
    QString author;
    QString sport;
    QString description;
    QString copyright;
    QDateTime exportedAt;
    int durationDays = 0;
    int frontGapDays = 0;
    int backGapDays = 0;

    void reset();
    bool isValid() const;
    QList<QString> save(const QDir &dir) const;
    bool load(const QDir &dir);

private:
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &obj);
};


struct PlanResult {
    QStringList errors;
    QStringList warnings;

    bool ok() const;
    void addError(const QString &error);
    void addWarning(const QString &warning);
    void reset();
};


struct PlanImportScheduleActivity {
    QDateTime sourceDateTime;
    bool selected = true;
};


struct PlanImportSchedule {
    QList<QDateTime> targetDateTimes;
    QDate targetRangeEnd;
    int durationDays = 0;
    int daysToAdd = 0;
};


inline PlanImportSchedule calculatePlanImportSchedule(
    const QList<PlanImportScheduleActivity> &activities,
    const QDate &targetRangeStart,
    bool includeGapDays,
    int frontGapDays,
    int backGapDays)
{
    PlanImportSchedule schedule;
    schedule.targetRangeEnd = targetRangeStart;
    if (activities.isEmpty()) return schedule;

    QDate firstActivity;
    QDate lastActivity;
    QDate firstSelected;
    QDate lastSelected;
    for (const PlanImportScheduleActivity &activity : activities) {
        const QDate date = activity.sourceDateTime.date();
        if (!firstActivity.isValid() || date < firstActivity)
            firstActivity = date;
        if (!lastActivity.isValid() || date > lastActivity)
            lastActivity = date;
        if (!activity.selected) continue;
        if (!firstSelected.isValid() || date < firstSelected)
            firstSelected = date;
        if (!lastSelected.isValid() || date > lastSelected)
            lastSelected = date;
    }

    if (includeGapDays) {
        schedule.durationDays =
            firstActivity.daysTo(lastActivity) + 1
            + frontGapDays + backGapDays;
        schedule.daysToAdd =
            firstActivity.daysTo(targetRangeStart) + frontGapDays;
    } else if (firstSelected.isValid()) {
        schedule.durationDays =
            firstSelected.daysTo(lastSelected) + 1;
        schedule.daysToAdd =
            firstSelected.daysTo(targetRangeStart);
    }
    if (schedule.durationDays > 0) {
        schedule.targetRangeEnd = targetRangeStart.addDays(
            schedule.durationDays - 1);
    }

    if (!includeGapDays && !firstSelected.isValid()) {
        schedule.daysToAdd =
            firstActivity.daysTo(targetRangeStart);
    }
    for (const PlanImportScheduleActivity &activity : activities) {
        schedule.targetDateTimes.append(
            activity.sourceDateTime.addDays(schedule.daysToAdd));
    }
    return schedule;
}


inline bool planImportHasLinkedConflict(
    const PlanImportScheduleActivity &activity,
    const QDateTime &targetDateTime,
    const QSet<QDateTime> &existingLinked)
{
    return activity.selected
        && existingLinked.contains(targetDateTime);
}


inline bool planExportCopiedAllActivities(
    qsizetype requested, int copied)
{
    return requested > 0 && copied == requested;
}


class RideFileSelection {
public:
    RideFileSelection(RideFile *rideFile, bool sel, const QDateTime &dt);

    RideFile *getRideFile() const;

    bool selected = false;
    QDateTime targetDateTime;

private:
    RideFile *rideFile = nullptr;
};


class PlanBundleReader {
    Q_DISABLE_COPY(PlanBundleReader)

public:
    explicit PlanBundleReader(Context *context, const QDate &targetDate);
    virtual ~PlanBundleReader();

    PlanMetadata getMetadata() const;
    PlanResult loadBundle(const QString &bundlePath);
    bool isValid() const;
    bool isNull() const;
    PlanResult importBundle();
    QDate getTargetRangeStart() const;
    QDate getTargetRangeEnd() const;
    int getDuration() const;
    int getNumActivities() const;
    int getNumSelectedActivities() const;
    const QStringList &getActivitiesToRemove() const;
    QList<RideItem*> getRideItemsToRemove() const;
    bool isIncludeGapDays() const;
    void setIncludeGapDays(bool includeGapDays);
    bool setActivitySelected(int index, bool selected);
    const QSet<QDateTime> &getExistingLinked() const;

    PlanResult getLastValidationResult() const;
    PlanResult getLastImportResult() const;

    QList<RideFileSelection> rideFiles;

private:
    QPointer<Context> context;
    PlanMetadata metadata;
    QTemporaryDir *tempDir = nullptr;
    PlanResult lastValidationResult;
    PlanResult lastImportResult;
    QDate targetRangeStart;
    QDate targetRangeEnd;
    int duration = 0;
    int daysToAdd = 0;
    bool includeGapDays = true;
    QStringList toDelete;
    QSet<QDateTime> existingLinked;
    QDir plannedDir;
    QDir workoutDir;
    QHash<QString, QString> trainDBHashes;

    void reset();
    void calculateRange();
    void findConflicts();
    bool cleanAndCopyActivity(
        RideFile *rideFile,
        const QDateTime &targetDateTime) const;
    bool processWorkout(RideFile *rideFile);
    void validate();
};


namespace PlanBundle {
    QString getRideName(RideItem const * const rideItem);
    QString getRideName(RideFile const * const rideFile);
    QString getRideSport(RideItem const * const rideItem);
    QString getRideSport(RideFile const * const rideFile);
    QDate getRideDate(RideItem const * const rideItem, bool preferOriginal);
    QDate getRideDate(RideFile const * const rideFile, bool preferOriginal);

    bool exportBundle(Context *context, const PlanExportDescription &description);
}

#endif
