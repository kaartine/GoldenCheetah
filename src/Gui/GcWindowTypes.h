/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_GCWINDOWTYPES_H
#define GC_GCWINDOWTYPES_H

#include <QMetaType>

namespace GcWindowTypes {

enum gcwinid {
    None = 0,
    Aerolab = 1,
    AllPlot = 2,
    CriticalPower = 3,
    Diary = 4,
    GoogleMap = 5,
    Histogram = 6,
    LTM = 7,
    Model = 8,
    PerformanceManager = 9,
    PfPv = 10,
    Race = 11,
    RideEditor = 13,
    RideSummary = 14,
    Scatter = 15,
    Summary = 16,
    Train = 17,
    TreeMap = 18,
    WeeklySummary = 19,
    HrPw = 20,
    VideoPlayer = 21,
    DialWindow = 22,
    MetadataWindow = 23,
    RealtimePlot = 24,
    WorkoutPlot = 25,
    MapWindow = 26,
    StreetViewWindow = 27,
    BingMap = 28,
    RealtimeControls = 29,
    ActivityNavigator = 30,
    SpinScanPlot = 31,
    DateRangeSummary = 32,
    CriticalPowerSummary = 33,
    Distribution = 34,
    RouteSegment = 35,
    WorkoutWindow = 36,
    RideMapWindow = 37,
    RConsole = 38,
    RConsoleSeason = 39,
    SeasonPlan = 40,
    WebPageWindow = 41,
    Overview = 42,
    Python = 43,
    PythonSeason = 44,
    UserTrends = 45,
    UserAnalysis = 46,
    OverviewTrends = 47,
    LiveMapWebPageWindow = 48,
    OverviewAnalysisBlank = 49,
    OverviewTrendsBlank = 50,
    ElevationChart = 51,
    Calendar = 52,
    Agenda = 53,
    UserPlan = 54,
    OverviewPlan = 55,
    OverviewPlanBlank = 56,
    PlanAdherence = 57,
    HtmlTraining = 58,
    WorkoutGame = 59
};

} // namespace GcWindowTypes

using GcWinID = GcWindowTypes::gcwinid;
Q_DECLARE_METATYPE(GcWinID)

#endif
