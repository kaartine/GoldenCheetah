/*
 * Copyright (c) 2008 Sean C. Rhea (srhea@srhea.net)
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

#include "RideMetric.h"
#include "RideItem.h"
#include "IntervalItem.h"
#include "Specification.h"
#include "UserMetricSettings.h"
#include "TimeUtils.h"
#include "Zones.h"
#include "HrZones.h"

#include <QSet>

#include <mutex>

RideMetric *createUserMetricForRegistry(UserMetricSettings settings);

// DB Schema Version - YOU MUST UPDATE THIS IF THE SCHEMA VERSION CHANGES!!!
// Schema version will change if a) the default metadata.xml is updated
//                            or b) new metrics are added / old changed
//                            or c) the metricDB tables structures change

// Revision History
// Rev Date         Who                What Changed
//-----------------------------------------------------------------------
//
// ******* Prior to version 29 no revision history was maintained *******
//
// 29  5th Sep 2011 Mark Liversedge    Added color to the ride fields
// 30  8th Sep 2011 Mark Liversedge    Metadata 'data' field for data present string
// 31  22  Nov 2011 Mark Liversedge    Added Average WPK metric
// 32  9th Dec 2011 Damien Grauser     Temperature data flag (metadata field 'Data')
// 33  17  Dec 2011 Damien Grauser     Added ResponseIndex and EfficiencyFactor
// 34  15  Jan 2012 Mark Liversedge    Added Average and Max Temperature and Metric->conversionSum()
// 35  13  Feb 2012 Mark Liversedge    Max/Avg Cadence adjusted conversion
// 36  18  Feb 2012 Mark Liversedge    Added Pace (min/mile) and 250m, 500m Pace metrics
// 37  06  Apr 2012 Rainer Clasen      Added non-zero average Power (watts)
// 38  8th Jul 2012 Mark Liversedge    Computes metrics for manual files now
// 39  18  Aug 2012 Mark Liversedge    New metric LRBalance
// 40  20  Oct 2012 Mark Liversedge    Lucene search/filter and checkbox metadata field
// 41  27  Oct 2012 Mark Liversedge    Lucene switched to StandardAnalyzer and search all texts by default
// 42  03  Dec 2012 Mark Liversedge    W/KG ridefilecache changes - force a rebuild.
// 43  24  Jan 2012 Mark Liversedge    TRIMP update
// 44  19  Apr 2013 Mark Liversedge    Aerobic Decoupling precision reduced to 1pt
// 45  09  May 2013 Mark Liversedge    Added 2,3,8 and 90m peak power for fatigue profiling
// 46  13  May 2013 Mark Liversedge    Handle absence of speed in metric calculations
// 47  17  May 2013 Mark Liversedge    Reimplementation of w/kg and ride->getWeight()
// 48  22  May 2013 Mark Liversedge    Removing local measures.xml, till v3.1
// 49  29  Oct 2013 Mark Liversedge    Added percentage time in zone
// 50  29  Oct 2013 Mark Liversedge    Added percentage time in heartrate zone
// 51  05  Nov 2013 Mark Liversedge    Added average aPower
// 52  05  Nov 2013 Mark Liversedge    Added EOA - Effect of Altitude
// 53  18  Dec 2013 Mark Liversedge    Added Fatigue Index (for power)
// 54  07  Jan 2014 Mark Liversedge    Revised Estimated VO2MAX metric formula
// 55  20  Jan 2014 Mark Liversedge    Added back Minimum W'bal metric and MaxMatch
// 56  20  Jan 2014 Mark Liversedge    Added W' TAU to be able to track it
// 57  20  Jan 2014 Mark Liversedge    Added W' Expenditure for total energy spent above CP
// 58  23  Jan 2014 Mark Liversedge    W' work rename and calculate without reference to WPrime class (speed)
// 59  24  Jan 2014 Mark Liversedge    Added Maximum W' exp which is same as W'bal bur expressed as used not left
// 60  05  Feb 2014 Mark Liversedge    Added Critical Power as a metric -- retrieves from settings for now
// 61  15  Feb 2014 Mark Liversedge    Fixed W' Work (for recintsecs not 1s!).
// 62  06  Mar 2014 Mark Liversedge    Fixed Fatigue Index to find peak then watch for decay, primarily useful in sprint intervals
// 63  06  Mar 2014 Mark Liversedge    Added Pacing Index AP as %age of Max Power
// 64  17  Mar 2014 Mark Liversedge    Added W' and CP work to PMC metrics
// 65  17  Mar 2014 Mark Liversedge    Added Aerobic TISS prototype
// 66  18  Mar 2014 Mark Liversedge    Updated aPower calculation
// 67  22  Mar 2014 Mark Liversedge    Added Anaerobic TISS prototype
// 68  22  Mar 2014 Mark Liversedge    Added dTISS prototype
// 69  23  Mar 2014 Mark Liversedge    Updated Gompertz constansts for An-TISS sigmoid
// 70  27  Mar 2014 Mark Liversedge    Add file CRC to refresh only if contents change (not just timestamps)
// 71  14  Apr 2014 Mark Liversedge    Added average lef/right vector metrics (Pedal Smoothness / Torque Effectiveness)
// 72  24  Apr 2014 Mark Liversedge    Andy Froncioni's faster algorithm for W' bal
// 73  11  May 2014 Mark Liversedge    Default color of 1,1,1 now uses CPLOTMARKER for ride color, change version to force rebuild
// 74  20  May 2014 Mark Liversedge    Added Athlete Weight
// 75  25  May 2014 Mark Liversedge    W' work calculation changed to only include energy above CP
// 76  14  Jun 2014 Mark Liversedge    Add new 'present' field that uses Data tag data
// 77  18  Jun 2014 Mark Liversedge    Add BikeStress per hour metric
// 78  19  Jun 2014 Mark Liversedge    Do not include zeroes in average L/R pedal smoothness/torque effectiveness
// 79  20  Jun 2014 Mark Liversedge    Change the way average temperature is handled
// 80  13  Jul 2014 Mark Liversedge    W' work + Below CP work = Work
// 81  16  Aug 2014 Joern Rischmueller Added 'Elevation Loss'
// 82  23  Aug 2014 Mark Liversedge    Added W'bal Matches
// 83  05  Sep 2014 Joern Rischmueller Added 'Time Carrying' and 'Elevation Gain Carrying'
// 84  08  Sep 2014 Mark Liversedge    Added HrPw Ratio
// 85  09  Sep 2014 Mark Liversedge    Added HrNp Ratio
// 86  26  Sep 2014 Mark Liversedge    Added isRun first class var
// 87  11  Oct 2014 Mark Liversedge    W'bal inegrator fixed up by Dave Waterworth
// 88  14  Oct 2014 Mark Liversedge    Pace Zone Metrics
// 89  07  Nov 2014 Ale Martinez       GOVSS
// 90  08  Nov 2014 Mark Liversedge    Update data flags for Moxy and Garmin Running Dynamics
// 91  16  Nov 2014 Damien Grauser     Do not include values if data not present in TimeInZone and HRTimeInZone
// 92  21  Nov 2014 Mark Liversedge    Added Watts:RPE ratio
// 93  26  Nov 2014 Mark Liversedge    Added Min, Max, Avg SmO2
// 94  02  Dec 2014 Ale Martinez       Added xPace
// 95  08  Dec 2014 Mark Liversedge    Deprecated Measures table
// 96  06  Jan 2015 Damien Grauser     Garmin Cycling Dynamics
// 97  07  Jan 2015 Mark Liversedge    Added isSwim first class variable
// 98  10  Jan 2015 Ale Martinez       Added Triscore and SwimScore metrics
// 99  14  Jan 2015 Damien Grauser     Added TotalCalories
// 100 05  Feb 2015 Ale Martinez       Use duration not time moving when its 0 (rpe metrics)
// 101 05  Feb 2015 Mark Liversedge    aPower versions of Coggan metrics aIsoPower et al
// 102 05  Feb 2015 Mark Liversedge    aPower versions of Skiba metrics aBikeScore et al
// 103 23  Feb 2015 Mark Liversedge    Added total heartbeats
// 104 24  Apr 2015 Mark Liversedge    Added Workbeat stress (Work * Heartbeats) / 10000
// 105 09  May 2015 Ale Martinez       Added PeakPace and PeakPaceSwim metrics
// 106 09  May 2015 Mark Liversedge    Added MMP Percentage - %age of power for duration vs CP model
// 107 29  May 2015 Mark Liversedge    Added AP as percent of maximum
// 108 29  May 2015 Mark Liversedge    Added W' Power - average power contribution from W'
// 109 29  May 2015 Mark Liversedge    Added Sustained Time In Zone metrics
// 110 12  Jun 2015 Mark Liversedge    Added climb rating
// 111 14  Jun 2015 Mark Liversedge    Added W'bal time in zone metrics
// 112 18  Jun 2015 Mark Liversedge    Added Core Temp average and max
// 113 27  Jun 2015 Mark Liversedge    Added Average/Min/Max tHb
// 114 17  Jul 2015 Ale Martinez       Added Distance Swim
// 115 18  Jul 2015 Mark Liversedge    Added Withings Fat, Fat Percent, Lean Body Weight
// 116 21  Aug 2015 Ale Martinez       TRIMP Zonal Points fallback when Average HR has been entered manually
// 117 29  Aug 2015 Mark Liversedge    Min non-zero HR
// 118 16  Sep 2015 Damien Grauser     Use FTP for BikeStress and IF
// 119 20  Oct 2015 Ale Martinez       Added VDOT and TPace for Running
// 120 3   Nov 2015 Mark Liversedge    Added Above CP time in W'bal zones
// 121 3   Nov 2015 Mark Liversedge    Added Work in W'bal zones
// 122 7   Nov 2015 Mark Liversedge    Added HR Zones 9 and 10
// 123 19  Nov 2015 Mark Liversedge    Force recompute of BikeStress/IF after logic fix
// 124 03  Dec 2015 Mark Liversedge    Min Temp
// 125 08  Dec 2015 Ale Martinez       Support metrics in Calendar Text
// 126 08  Mar 2016 Mark Liversedge    Added count of To Exhaustions
// 127 25  Mar 2016 Mark Liversedge    Best R metric for Exhaustion Points
// 128 15  May 2016 Mark Liversedge    Add ActivityCRC so R scripts can use when caching
// 129 10  Jul 2016 Damien Grauser     Average Running Cadence
// 130 12  Jul 2016 Ale Martinez       Added Best Times for common distances
// 131 20  Jul 2016 Damien Grauser     Average Running Vertical Oscillation and Ground Contact Time
// 132 21  Jul 2016 Ale Martinez       Added SwimMetrics (Stroke Rate et al)
// 133 22  Jul 2016 Damien Grauser     Added Efficiency Index
// 134 22  Jul 2016 Damien Grauser     Add Stride length
// 135 10  Aug 2016 Ale Martinez       Added Average Swim Pace for the 4 Strokes
// 136 17  Oct 2016 Ale Martinez       Changed Best Times units to minutes
// 137 16  Feb 2017 Leif Warland       Added HrvMetrics
// 138 01  Mar 2017 Mark Liversedge	   Added elapsed_time metric for intervals
// 139 88  Mar 2017 Leif Warland       Added SDANN and SDNNIDX to HRV metrics
// 140 08  Apr 2017 Ale Martinez       Added Peak Pace Hr metrics
// 141 14  Apr 2017 Joern Rischmueller Added 'Athlete Bones', 'Athlete Muscles' Body Measures metric
// 142 25  Jul 2017 Ale Martinez       Added HRV metrics at rest (Measures)
// 143 07  Feb 2018 Walter Buerki      Daniels Points using GAP when no power
// 144 02  Apr 2018 Mark Liversedge    Force refresh for compatibility metrics
// 145 06  Apr 2018 Ale Martinez       Python Scripts in UserMetric honor RideItem and Specification
// 146 21  Apr 2018 Ale Martinez       TriScore Fallback to TRIMP Zonal Points
// 147 06  May 2018 Ale Martinez       Added PeakHr metrics and HrZone
// 148 27  Jul 2018 Ale Martinez       Changed Hrv Measures to retrun 0 when no record for the date
// 149 04  Jan 2019 Mark Liversedge    PowerIndex metric to score performance tests/intervals/rides vs typical athlete PD curve
// 150 28  Mar 2019 Ale Martinez       Additional Running Dynamics metrics
// 151 14  May 2019 Ale Martinez       Added Time Recording and use it in Time in Zone Percentage
// 152 20  May 2019 Ale Martinez       Fixed Time in Zone Percentages to aggregate properly
// 153  8  Dec 2019 Mark Liversedge    Regenerate after v3.5 RC2/RC2X re-issue
// 154 18  Apr 2020 Mark Liversedge    Added PeakPowerIndex (only for full rides)
// 155 27  Jun 2020 Mark Liversedge    Added Ride Date as days since 1900,01,01
// 156 18  Mar 2021 Ale Martinez       Added Time and % in Zones I, II and III
// 157 27  May 2021 Ale Martinez       Added Pace Row
// 158 28  Feb 2024 Ale Martinez       Enabled Pace for Walking
// 159 28  Apr 2024 Ale Martinez       Fix Avg Speed aggregation
// 160 22  Nov 2024 Ale Martinez       Added Swim Stroke metric for lap swims

int DBSchemaVersion = 160;

QList<QString> RideMetricFactory::compatibilitymetrics;

// user defined metrics are loaded by the ridecache on startup
// and then reloaded by ridecache if they change
QList<UserMetricSettings> _userMetrics;
std::atomic<quint16> UserMetricSchemaVersion{0};

class RideMetricRegistryState
{
public:
    RideMetricRegistryState() = default;

    RideMetricRegistryState(const RideMetricRegistryState &other)
        : metricNames(other.metricNames), metricTypes(other.metricTypes),
          metrics(other.metrics), dependencyMap(other.dependencyMap),
          userMetricSchemaVersion(other.userMetricSchemaVersion)
    {
    }

    QStringList metricNames;
    QVector<RideMetric::MetricType> metricTypes;
    QHash<QString, RideMetricPtr> metrics;
    QHash<QString, QVector<QString>> dependencyMap;
    quint16 userMetricSchemaVersion = 0;
    mutable std::once_flag dependenciesChecked;
};

thread_local std::shared_ptr<const RideMetricRegistryState>
    RideMetricFactory::constructionState_;

namespace {

const QStringList &emptyMetricNames()
{
    static const QStringList empty;
    return empty;
}

const QVector<QString> &emptyDependencies()
{
    static const QVector<QString> empty;
    return empty;
}

} // namespace

RideMetricRegistrySnapshot::RideMetricRegistrySnapshot(
    std::shared_ptr<const RideMetricRegistryState> state)
    : state_(std::move(state))
{
}

int RideMetricRegistrySnapshot::metricCount() const
{
    return state_ ? state_->metricNames.size() : 0;
}

const QStringList &RideMetricRegistrySnapshot::allMetrics() const
{
    return state_ ? state_->metricNames : emptyMetricNames();
}

QString RideMetricRegistrySnapshot::metricName(int index) const
{
    return state_ && index >= 0 && index < state_->metricNames.size()
        ? state_->metricNames.at(index) : QString();
}

RideMetric::MetricType
RideMetricRegistrySnapshot::metricType(int index) const
{
    return state_ && index >= 0 && index < state_->metricTypes.size()
        ? state_->metricTypes.at(index) : RideMetric::Total;
}

const RideMetric *
RideMetricRegistrySnapshot::rideMetric(const QString &symbol) const
{
    return state_ ? state_->metrics.value(symbol).data() : nullptr;
}

bool RideMetricRegistrySnapshot::haveMetric(const QString &symbol) const
{
    return state_ && state_->metrics.contains(symbol);
}

RideMetric *RideMetricRegistrySnapshot::newMetric(const QString &symbol) const
{
    if (!state_) return nullptr;

    std::call_once(state_->dependenciesChecked, [state = state_]() {
        for (auto it = state->dependencyMap.cbegin();
             it != state->dependencyMap.cend(); ++it) {
            for (const QString &dependency : it.value()) {
                if (!state->metrics.contains(dependency))
                    qDebug() << "metric dep error:" << dependency;
            }
        }
    });

    const RideMetricPtr definition = state_->metrics.value(symbol);
    return definition ? definition->clone() : nullptr;
}

const QVector<QString> &
RideMetricRegistrySnapshot::dependencies(const QString &symbol) const
{
    if (!state_ || !state_->metrics.contains(symbol))
        return emptyDependencies();
    const auto it = state_->dependencyMap.constFind(symbol);
    return it == state_->dependencyMap.cend()
        ? emptyDependencies() : it.value();
}

quint16 RideMetricRegistrySnapshot::userMetricSchemaVersion() const
{
    return state_ ? state_->userMetricSchemaVersion : 0;
}

RideMetricFactory::RideMetricFactory()
    : state_(std::make_shared<const RideMetricRegistryState>())
{
}

RideMetricFactory &RideMetricFactory::instance()
{
    // Metric registrars have static lifetime and can run during global
    // teardown. Preserve the historical process-lifetime factory while
    // retaining thread-safe function-local initialization.
    static RideMetricFactory *const factory = new RideMetricFactory;
    return *factory;
}

RideMetricRegistrySnapshot RideMetricFactory::snapshot() const
{
    if (constructionState_)
        return RideMetricRegistrySnapshot(constructionState_);
    return RideMetricRegistrySnapshot(
        std::atomic_load_explicit(&state_, std::memory_order_acquire));
}

int RideMetricFactory::metricCount() const
{
    return snapshot().metricCount();
}

QHash<QString, RideMetric *> RideMetricFactory::metricHash() const
{
    const RideMetricRegistrySnapshot current = snapshot();
    QHash<QString, RideMetric *> result;
    if (!current.state_) return result;
    for (auto it = current.state_->metrics.cbegin();
         it != current.state_->metrics.cend(); ++it) {
        result.insert(it.key(), it.value().data());
    }
    return result;
}

void RideMetricFactory::initialize()
{
    QMutexLocker locker(&writerMutex_);
    const auto current =
        std::atomic_load_explicit(&state_, std::memory_order_acquire);
    auto next = std::make_shared<RideMetricRegistryState>(*current);
    QHash<QString, RideMetricPtr> initialized;

    for (const QString &name : current->metricNames) {
        const RideMetricPtr definition = current->metrics.value(name);
        RideMetricPtr copy(definition ? definition->clone() : nullptr);
        if (!copy) {
            qWarning() << "cannot initialize metric clone:" << name;
            return;
        }
        copy->setIndex(definition->index());
        copy->initialize();
        initialized.insert(name, copy);
    }
    next->metrics = initialized;
    std::atomic_store_explicit(
        &state_, std::shared_ptr<const RideMetricRegistryState>(next),
        std::memory_order_release);
}

QStringList RideMetricFactory::allMetrics() const
{
    return snapshot().allMetrics();
}

QString RideMetricFactory::metricName(int index) const
{
    return snapshot().metricName(index);
}

RideMetric::MetricType RideMetricFactory::metricType(int index) const
{
    return snapshot().metricType(index);
}

const RideMetric *RideMetricFactory::rideMetric(QString name) const
{
    return snapshot().rideMetric(name);
}

bool RideMetricFactory::haveMetric(const QString &symbol) const
{
    return snapshot().haveMetric(symbol);
}

RideMetric *RideMetricFactory::newMetric(const QString &symbol) const
{
    return snapshot().newMetric(symbol);
}

void RideMetricFactory::removeUserMetrics()
{
    QMutexLocker locker(&writerMutex_);
    const auto current =
        std::atomic_load_explicit(&state_, std::memory_order_acquire);
    auto next = std::make_shared<RideMetricRegistryState>();
    next->userMetricSchemaVersion = current->userMetricSchemaVersion;

    for (const QString &name : current->metricNames) {
        const RideMetricPtr metric = current->metrics.value(name);
        if (metric && metric->isUser()) break;
        next->metricNames.append(name);
        next->metricTypes.append(current->metricTypes.at(
            next->metricTypes.size()));
        next->metrics.insert(name, metric);
        const auto dependencies = current->dependencyMap.constFind(name);
        if (dependencies != current->dependencyMap.cend())
            next->dependencyMap.insert(name, dependencies.value());
    }

    if (next->metricNames.size() == current->metricNames.size()) return;
    std::atomic_store_explicit(
        &state_, std::shared_ptr<const RideMetricRegistryState>(next),
        std::memory_order_release);
}

bool RideMetricFactory::replaceUserMetrics(
    const QList<UserMetricSettings> &settings, quint16 schemaVersion)
{
    QMutexLocker locker(&writerMutex_);
    const auto current =
        std::atomic_load_explicit(&state_, std::memory_order_acquire);
    auto next = std::make_shared<RideMetricRegistryState>();

    for (const QString &name : current->metricNames) {
        const RideMetricPtr metric = current->metrics.value(name);
        if (metric && metric->isUser()) break;
        next->metricNames.append(name);
        next->metricTypes.append(current->metricTypes.at(
            next->metricTypes.size()));
        next->metrics.insert(name, metric);
        const auto dependencies = current->dependencyMap.constFind(name);
        if (dependencies != current->dependencyMap.cend())
            next->dependencyMap.insert(name, dependencies.value());
    }
    next->userMetricSchemaVersion = schemaVersion;

    const auto previousConstructionState = constructionState_;
    constructionState_ = next;
    struct ConstructionStateRestore {
        std::shared_ptr<const RideMetricRegistryState> previous;
        ~ConstructionStateRestore()
        {
            RideMetricFactory::constructionState_ = std::move(previous);
        }
    } restore{previousConstructionState};

    for (const UserMetricSettings &setting : settings) {
        if (next->metrics.contains(setting.symbol)) continue;
        RideMetricPtr metric(createUserMetricForRegistry(setting));
        metric->setIndex(next->metrics.size());
        next->metrics.insert(setting.symbol, metric);
        next->metricNames.append(setting.symbol);
        next->metricTypes.append(metric->type());
    }

    std::atomic_store_explicit(
        &state_, std::shared_ptr<const RideMetricRegistryState>(next),
        std::memory_order_release);
    UserMetricSchemaVersion.store(schemaVersion, std::memory_order_release);
    return true;
}

bool RideMetricFactory::addMetric(
    const RideMetric &metric, const QVector<QString> *dependencies)
{
    RideMetricPtr registered(metric.clone());
    if (!registered) return false;

    QMutexLocker locker(&writerMutex_);
    const auto current =
        std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (current->metrics.contains(metric.symbol())) return false;

    auto next = std::make_shared<RideMetricRegistryState>(*current);
    registered->setIndex(next->metrics.size());
    next->metrics.insert(metric.symbol(), registered);
    next->metricNames.append(metric.symbol());
    next->metricTypes.append(metric.type());
    if (dependencies)
        next->dependencyMap.insert(metric.symbol(), *dependencies);

    std::atomic_store_explicit(
        &state_, std::shared_ptr<const RideMetricRegistryState>(next),
        std::memory_order_release);
    return true;
}

QVector<QString>
RideMetricFactory::dependencies(const QString &symbol) const
{
    return snapshot().dependencies(symbol);
}

quint16 RideMetricFactory::userMetricSchemaVersion() const
{
    return snapshot().userMetricSchemaVersion();
}

quint16
RideMetric::userMetricFingerprint(QList<UserMetricSettings> these)
{
    // run through loaded metrics and compute a fingerprint CRC
    QByteArray fingers;
    foreach(UserMetricSettings x, these)
        fingers += x.fingerprint.toLocal8Bit();

    return qChecksum(fingers);
}

QHash<QString,RideMetricPtr>
RideMetric::computeMetrics(RideItem *item, Specification spec, const QStringList &metrics)
{
    return computeMetrics(
        item, spec, metrics, RideMetricFactory::instance().snapshot());
}

QHash<QString,RideMetricPtr>
RideMetric::computeMetrics(
    RideItem *item, Specification spec, const QStringList &metrics,
    const RideMetricRegistrySnapshot &factory)
{

    // Keep the historical builtin-before-user root ordering, but de-duplicate
    // requests before constructing the reachable dependency graph.
    QStringList builtinRoots;
    QStringList userRoots;
    QSet<QString> requestedSymbols;
    foreach (const QString &metric, metrics) {
        if (!factory.haveMetric(metric) || requestedSymbols.contains(metric))
            continue;

        requestedSymbols.insert(metric);
        if (factory.rideMetric(metric)->isUser())
            userRoots.append(metric);
        else
            builtinRoots.append(metric);
    }
    const bool hasUserMetrics = !userRoots.isEmpty();
    const QStringList roots = builtinRoots + userRoots;

    // Discover only the graph reachable from requested metrics. Preserve root
    // and declared dependency order so every valid graph has a stable order.
    QStringList discovered = roots;
    QSet<QString> discoveredSet(requestedSymbols);
    QHash<QString, QVector<QString>> graph;
    QHash<QString, QStringList> dependents;
    QSet<QString> invalid;

    for (int i = 0; i < discovered.size(); ++i) {
        const QString symbol = discovered.at(i);
        QVector<QString> knownDependencies;
        QSet<QString> seenDependencies;

        foreach (const QString &dependency, factory.dependencies(symbol)) {
            if (seenDependencies.contains(dependency)) continue;
            seenDependencies.insert(dependency);

            if (!factory.haveMetric(dependency)) {
                invalid.insert(symbol);
                continue;
            }

            knownDependencies.append(dependency);
            dependents[dependency].append(symbol);
            if (!discoveredSet.contains(dependency)) {
                discoveredSet.insert(dependency);
                discovered.append(dependency);
            }
        }
        graph.insert(symbol, knownDependencies);
    }

    // A metric with a missing dependency, and every metric depending on it,
    // is unresolvable even when the remainder of its graph is acyclic.
    QStringList invalidQueue;
    foreach (const QString &symbol, discovered)
        if (invalid.contains(symbol)) invalidQueue.append(symbol);

    for (int i = 0; i < invalidQueue.size(); ++i) {
        foreach (const QString &dependent, dependents.value(invalidQueue.at(i))) {
            if (invalid.contains(dependent)) continue;
            invalid.insert(dependent);
            invalidQueue.append(dependent);
        }
    }

    // Kahn's algorithm resolves the valid acyclic portion. Cycle members and
    // their dependents retain a positive indegree and are never scheduled.
    QHash<QString, int> indegree;
    QStringList ready;
    foreach (const QString &symbol, discovered) {
        if (invalid.contains(symbol)) continue;
        indegree.insert(symbol, graph.value(symbol).size());
        if (indegree.value(symbol) == 0) ready.append(symbol);
    }

    QStringList topologicalOrder;
    QSet<QString> resolved;
    for (int i = 0; i < ready.size(); ++i) {
        const QString symbol = ready.at(i);
        if (resolved.contains(symbol)) continue;

        resolved.insert(symbol);
        topologicalOrder.append(symbol);
        foreach (const QString &dependent, dependents.value(symbol)) {
            if (invalid.contains(dependent) || resolved.contains(dependent))
                continue;

            const int remaining = indegree.value(dependent) - 1;
            indegree.insert(dependent, remaining);
            if (remaining == 0) ready.append(dependent);
        }
    }

    // Do not compute an otherwise valid dependency when all requested roots
    // that need it are unresolvable.
    QSet<QString> needed;
    QStringList neededQueue;
    foreach (const QString &root, roots) {
        if (!resolved.contains(root) || needed.contains(root)) continue;
        needed.insert(root);
        neededQueue.append(root);
    }
    for (int i = 0; i < neededQueue.size(); ++i) {
        foreach (const QString &dependency, graph.value(neededQueue.at(i))) {
            if (!resolved.contains(dependency) || needed.contains(dependency))
                continue;
            needed.insert(dependency);
            neededQueue.append(dependency);
        }
    }

    // resize the metric array in the interval if needed
    if (spec.interval() && spec.interval()->metrics().size() < factory.metricCount()) 
        spec.interval()->metrics().resize(factory.metricCount());

    // resize the metric array in the interval if needed
    if (!spec.interval() && item->metrics().size() < factory.metricCount())
        item->metrics().resize(factory.metricCount());

    QHash<QString, RideMetricPtr> owned;
    QHash<QString, RideMetric *> done;
    foreach (const QString &symbol, topologicalOrder) {
        if (!needed.contains(symbol)) continue;

        bool dependenciesReady = true;
        foreach (const QString &dependency, graph.value(symbol)) {
            if (!done.contains(dependency)) {
                dependenciesReady = false;
                break;
            }
        }
        if (!dependenciesReady) continue;

        // Hold ownership before compute so null clones and exceptional metric
        // implementations cannot leak already-computed instances.
        RideMetricPtr metric(factory.newMetric(symbol));
        if (metric.isNull()) continue;

        RideMetric *m = metric.data();
        m->setValue(0.0);
        m->setCount(0);
        m->compute(item, spec, done);

        // override the computed value if set by user, but not for intervals
        if (!spec.interval() && item->ride() && item->ride()->metricOverrides.contains(symbol))
            m->override(item->ride()->metricOverrides.value(symbol));

        owned.insert(symbol, metric);
        done.insert(symbol, m);

        // User metrics interrogate the value array rather than dependency
        // pointers, so publish every resolved dependency before they run.
        if (hasUserMetrics) {
            if (spec.interval()) spec.interval()->metrics()[m->index()] = m->value();
            else item->metrics()[m->index()] = m->value();
        }
    }

    QHash<QString,RideMetricPtr> result;
    foreach (const QString &symbol, metrics) {
        const RideMetricPtr metric = owned.value(symbol);
        if (!metric.isNull()) result.insert(symbol, metric);
    }

    return result;
}

double 
RideMetric::getForSymbol(QString symbol, const QHash<QString,RideMetric*> *p)
{
    if (p == NULL ) return RideFile::NIL;

    RideMetric *m=p->value(symbol, NULL);

    if (m == NULL) return RideFile::NIL;

    // ok, lets get the value
    return m->value();
}

QString 
RideMetric::toString(bool useMetricUnits) const
{
    if (isDate()) { QDate date(1900,01,01); date = date.addDays(this->value(useMetricUnits)); return date.toString("dd MMM yy"); }
    if (isTime()) return time_to_string(value(useMetricUnits));
    return QString("%1").arg(value(useMetricUnits), 0, 'f', this->precision());
}

QString
RideMetric::toString(double v) const
{
    if (isDate()) { QDate date(1900,01,01); date = date.addDays(v); return date.toString("dd MMM yy"); }
    if (isTime()) return time_to_string(v);
    return QString("%1").arg(v, 0, 'f', this->precision());
}
