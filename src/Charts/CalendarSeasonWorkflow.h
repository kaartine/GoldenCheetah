/*
 * Copyright (c) 2026 Jukka Kaartinen
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_CalendarSeasonWorkflow_h
#define _GC_CalendarSeasonWorkflow_h 1

#include <QList>

#include "Season.h"


constexpr int CalendarAmbiguousRecordIndex = -2;


inline int calendarUniqueSeasonEventIndex(
    const Season &season, const QString &eventId)
{
    if (eventId.isEmpty()) return -1;
    int resolved = -1;
    for (int i = 0; i < season.events.size(); ++i) {
        if (season.events[i].id != eventId) continue;
        if (resolved >= 0)
            return CalendarAmbiguousRecordIndex;
        resolved = i;
    }
    return resolved;
}


inline int calendarUniqueSeasonPhaseIndex(
    const Season &season, const QUuid &phaseId)
{
    if (phaseId.isNull()) return -1;
    int resolved = -1;
    for (int i = 0; i < season.phases.size(); ++i) {
        if (season.phases[i].id() != phaseId) continue;
        if (resolved >= 0)
            return CalendarAmbiguousRecordIndex;
        resolved = i;
    }
    return resolved;
}


inline bool calendarSeasonEventValuesEqual(
    const SeasonEvent &left,
    const SeasonEvent &right)
{
    return left.name == right.name
        && left.date == right.date
        && left.priority == right.priority
        && left.description == right.description
        && left.id == right.id;
}


inline bool calendarSeasonValuesEqual(
    const Season &left,
    const Season &right);


inline bool calendarPhaseValuesEqual(
    const Phase &left,
    const Phase &right)
{
    return calendarSeasonValuesEqual(left, right);
}


inline bool calendarSeasonValuesEqual(
    const Season &left,
    const Season &right)
{
    if (left.id() != right.id()
        || left.getName() != right.getName()
        || left.getType() != right.getType()
        || left.getAbsoluteStart() != right.getAbsoluteStart()
        || left.getAbsoluteEnd() != right.getAbsoluteEnd()
        || left.getOffsetStart() != right.getOffsetStart()
        || left.getOffsetEnd() != right.getOffsetEnd()
        || left.getLength() != right.getLength()
        || left.isYtd() != right.isYtd()
        || left.getSeed() != right.getSeed()
        || left.getLow() != right.getLow()
        || left.load() != right.load()
        || left.events.size() != right.events.size()
        || left.phases.size() != right.phases.size()) {
        return false;
    }
    for (int i = 0; i < left.events.size(); ++i) {
        if (!calendarSeasonEventValuesEqual(
                left.events[i], right.events[i])) {
            return false;
        }
    }
    for (int i = 0; i < left.phases.size(); ++i) {
        if (!calendarPhaseValuesEqual(
                left.phases[i], right.phases[i])) {
            return false;
        }
    }
    return true;
}


class CalendarSeasonSnapshot
{
public:
    explicit CalendarSeasonSnapshot(const Season &season)
        : snapshot(season)
    {
    }

    Season *resolveUnchanged(QList<Season> &seasons) const
    {
        Season *resolved = nullptr;
        for (Season &season : seasons) {
            if (season.id() != snapshot.id()) continue;
            if (resolved) return nullptr;
            resolved = &season;
        }
        return resolved
            && calendarSeasonValuesEqual(*resolved, snapshot)
            ? resolved : nullptr;
    }

    bool matchesCurrent(const Season *season) const
    {
        return season && season->id() == snapshot.id();
    }

    const Season &value() const { return snapshot; }

private:
    Season snapshot;
};

#endif // _GC_CalendarSeasonWorkflow_h
