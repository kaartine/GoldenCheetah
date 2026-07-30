/*
 * Copyright (c) 2011 Mark Liversedge (liversedge@gmail.com)
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

#include "RideFileCache.h"
#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "Zones.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "WPrime.h" // for wbal zones
#include "LTMSettings.h" // getAllBestsFor needs this

#include <cmath> // for pow()
#include <QByteArrayView>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QtAlgorithms> // for qStableSort

#include <limits>
#include <utility>

static const int maxcache = 25; // lets max out at 25 caches

// predefined binsize for the dist arrays
static const double wattsDelta = 1.0;
static const double wattsKgDelta = 0.01;
static const double nmDelta    = 0.1;
static const double hrDelta    = 1.0;
static const double kphDelta   = 0.1;
static const double cadDelta   = 1.0;
static const double gearDelta  = 0.01; //RideFileCache creates POW(10) * decimals section
static const double smo2Delta  = 1;
static const double wbalDelta  = 1;

#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
static thread_local std::function<void()>
    sourceBoundReadHookForTest;
static thread_local int
    sourceFingerprintReadsForTest = 0;
#endif

static bool computeSourceFingerprint(
    const QString &sourcePath,
    RideFileCRC::ContentFingerprint
        &fingerprint)
{
#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
    ++sourceFingerprintReadsForTest;
#endif
    return RideFileCRC::
        computeFileFingerprint(
            sourcePath, fingerprint);
}

template<typename Operation>
static bool withCurrentSource(
    const QString &sourcePath,
    RideFileCRC::ContentFingerprint *verifiedFingerprint,
    Operation &&operation)
{
#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
    std::function<void()> sourceMutationHook =
        std::move(
            sourceBoundReadHookForTest);
    sourceBoundReadHookForTest = {};
#endif

    RideFileCRC::ContentFingerprint
        cacheFingerprint;
    if (!operation(cacheFingerprint)
        || !cacheFingerprint.isValid()) {
        return false;
    }

#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
    if (sourceMutationHook)
        sourceMutationHook();
#endif

    RideFileCRC::ContentFingerprint current;
    if (!computeSourceFingerprint(
            sourcePath, current)
        || current != cacheFingerprint) {
        return false;
    }

    if (verifiedFingerprint)
        *verifiedFingerprint =
            std::move(current);
    return true;
}

static bool sourceBindingsAreCurrent(
    const QVector<
        QPair<
            QString,
            RideFileCRC::ContentFingerprint>>
        &bindings)
{
    for (const auto &binding : bindings) {
        RideFileCRC::ContentFingerprint
            current;
        if (!RideFileCRC::
                computeFileFingerprint(
                    binding.first,
                    current)
            || current != binding.second) {
            return false;
        }
    }
    return true;
}

template<typename Operation>
static bool readSourceBoundPartialCache(
    const QString &sourcePath,
    const QString &cachePath,
    const double *expectedWeight,
    RideFileCRC::ContentFingerprint *verifiedFingerprint,
    Operation &&operation)
{
    return withCurrentSource(
        sourcePath,
        verifiedFingerprint,
        [&](RideFileCRC::ContentFingerprint
                &cacheFingerprint) {
            QFile cacheFile(cachePath);
            if (!cacheFile.open(
                    QIODevice::ReadOnly
                    | QIODevice::Unbuffered)) {
                return false;
            }

            RideFileCacheIntegrity::PartialReader
                reader(cacheFile);
            if (!reader.isValid()
                || (expectedWeight
                    && reader.header().WEIGHT
                        != *expectedWeight)
                || !operation(reader)
                || !reader.finish()) {
                return false;
            }
            cacheFingerprint =
                reader.sourceFingerprint();
            return true;
        });
}

static bool cacheIsCurrentForSource(
    const QString &sourcePath,
    const QString &cachePath,
    double weight)
{
    return readSourceBoundPartialCache(
        sourcePath,
        cachePath,
        &weight,
        nullptr,
        [](RideFileCacheIntegrity::
               PartialReader &) {
            return true;
        });
}


// cache from ride
RideFileCache::RideFileCache(Context *context, QString fileName, double weight, RideFile *passedride, bool check, bool refresh) :
               crc(0), incomplete(false), context(context),
               rideFileName(fileName), ride(passedride),
               CP(0), WPRIME(0), LTHR(0), CV(0.0), WEIGHT(0.0),
               filter(false), onhome(false)
{
    // resize all the arrays to zero
    wattsMeanMax.resize(0);
    heatMeanMax.resize(0);
    hrMeanMax.resize(0);
    cadMeanMax.resize(0);
    nmMeanMax.resize(0);
    kphMeanMax.resize(0);
    wattsdMeanMax.resize(0);
    caddMeanMax.resize(0);
    nmdMeanMax.resize(0);
    hrdMeanMax.resize(0);
    kphdMeanMax.resize(0);
    xPowerMeanMax.resize(0);
    npMeanMax.resize(0);
    vamMeanMax.resize(0);
    wattsKgMeanMax.resize(0);
    aPowerMeanMax.resize(0);
    aPowerKgMeanMax.resize(0);
    wattsDistribution.resize(0);
    hrDistribution.resize(0);
    cadDistribution.resize(0);
    gearDistribution.resize(0);
    nmDistribution.resize(0);
    kphDistribution.resize(0);
    xPowerDistribution.resize(0);
    npDistribution.resize(0);
    wattsKgDistribution.resize(0);
    aPowerDistribution.resize(0);
    smo2Distribution.resize(0);
    wbalDistribution.resize(0);

    // time in zone are fixed to 10 zone max
    wattsTimeInZone.resize(10);
    wattsCPTimeInZone.resize(4); // zero, I, II, III
    hrTimeInZone.resize(10);
    hrCPTimeInZone.resize(4); // zero, I, II, III
    paceTimeInZone.resize(10);
    paceCPTimeInZone.resize(4); // zero, I, II, III
    wbalTimeInZone.resize(4); // 0-25, 25-50, 50-75, 75% +

    // Get info for ride file and cache file
    const QString cacheRoot =
        context->athlete->home->cache().absolutePath();
    cacheFileName = RideFileCacheIntegrity::cachePathForActivity(
        cacheRoot,
        context->athlete->home->activities().absolutePath(),
        context->athlete->home->planned().absolutePath(),
        rideFileName);
    if (cacheFileName.isEmpty()) {
        computeWithoutPersistentCache(refresh, weight);
        return;
    }
    if ((check
         && cacheIsCurrentForSource(
             rideFileName,
             cacheFileName,
             weight))
        || (!check && readCache(weight))) {
        return;
    }

    // NEED TO UPDATE!!
    if (refresh) {
        incomplete = true;

        // not up-to-date we need to refresh from the ridefile
        if (ride) {

            // we got passed the ride - so update from that
            WEIGHT = ride->getWeight(); // before threads are created
            refreshCache();

        } else {

            // we need to open it to update since we were not passed one
            QStringList errors;
            QFile file(rideFileName);

            ride = RideFileFactory::instance().openRideFile(context, file, errors);

            if (ride) {
                WEIGHT = ride->getWeight(); // before threads are created
                refreshCache();
                delete ride;
            }
            ride = 0;
        }
    } else {
        incomplete = true; // data not available !
    }
}

bool 
RideFileCache::checkStale(Context *context, RideItem*item)
{
    // check if we're stale ?
    // Get info for ride file and cache file
    QString rideFileName;
    rideFileName =
        RideFileCacheIntegrity::activitySourcePath(
            item->path, item->fileName);

    const QString cacheFileName =
        RideFileCacheIntegrity::cachePathForActivity(
            context->athlete->home->cache().absolutePath(),
            context->athlete->home->activities().absolutePath(),
            context->athlete->home->planned().absolutePath(),
            rideFileName);

    return !cacheIsCurrentForSource(
        rideFileName,
        cacheFileName,
        item->getWeight());
}

static bool meanMaxBlockForSeries(
    RideFile::SeriesType series,
    RideFileCacheIntegrity::Block &block)
{
    switch (series) {
    case RideFile::watts:
        block = RideFileCacheIntegrity::WattsMeanMax;
        return true;
    case RideFile::wattsKg:
        block = RideFileCacheIntegrity::WattsKgMeanMax;
        return true;
    case RideFile::hr:
        block = RideFileCacheIntegrity::HrMeanMax;
        return true;
    case RideFile::cad:
        block = RideFileCacheIntegrity::CadMeanMax;
        return true;
    case RideFile::nm:
        block = RideFileCacheIntegrity::NmMeanMax;
        return true;
    case RideFile::kph:
        block = RideFileCacheIntegrity::KphMeanMax;
        return true;
    case RideFile::kphd:
        block = RideFileCacheIntegrity::KphdMeanMax;
        return true;
    case RideFile::wattsd:
        block = RideFileCacheIntegrity::WattsdMeanMax;
        return true;
    case RideFile::cadd:
        block = RideFileCacheIntegrity::CaddMeanMax;
        return true;
    case RideFile::nmd:
        block = RideFileCacheIntegrity::NmdMeanMax;
        return true;
    case RideFile::hrd:
        block = RideFileCacheIntegrity::HrdMeanMax;
        return true;
    case RideFile::xPower:
        block = RideFileCacheIntegrity::XPowerMeanMax;
        return true;
    case RideFile::IsoPower:
        block = RideFileCacheIntegrity::NpMeanMax;
        return true;
    case RideFile::vam:
        block = RideFileCacheIntegrity::VamMeanMax;
        return true;
    case RideFile::aPower:
        block = RideFileCacheIntegrity::APowerMeanMax;
        return true;
    case RideFile::aPowerKg:
        block = RideFileCacheIntegrity::APowerKgMeanMax;
        return true;
    default:
        return false;
    }
}

static bool zoneBlockForSeries(
    RideFile::SeriesType series,
    RideFileCacheIntegrity::ZoneBlock &block)
{
    switch (series) {
    case RideFile::watts:
        block = RideFileCacheIntegrity::WattsTimeInZone;
        return true;
    case RideFile::hr:
        block = RideFileCacheIntegrity::HrTimeInZone;
        return true;
    case RideFile::kph:
        block = RideFileCacheIntegrity::PaceTimeInZone;
        return true;
    case RideFile::wbal:
        block = RideFileCacheIntegrity::WbalTimeInZone;
        return true;
    default:
        return false;
    }
}

static QString sourcePathForRideItem(
    Context *context,
    const RideItem *item)
{
    if (!context || !context->athlete || !context->athlete->home
        || !item) {
        return {};
    }
    return RideFileCacheIntegrity::activitySourcePath(
        item->path, item->fileName);
}

static QString cachePathForRideItem(
    Context *context,
    const RideItem *item)
{
    const QString sourcePath =
        sourcePathForRideItem(context, item);
    if (sourcePath.isEmpty())
        return {};
    return RideFileCacheIntegrity::cachePathForActivity(
        context->athlete->home->cache().absolutePath(),
        context->athlete->home->activities().absolutePath(),
        context->athlete->home->planned().absolutePath(),
        sourcePath);
}

QVector<float> RideFileCache::meanMaxPowerFor(Context *context, QVector<float> &wpk, QDate from, QDate to, QVector<QDate>*dates, QString sport)
{
    QVector<float> returning;
    QVector<float> returningwpk;
    bool first = true;

    // look at all the rides
    QList<RideItem*> rides = context->athlete->rideCache->rides(); // Create a copy of the ride list to prevent a crash
    for (RideItem *item : rides) {

        if (item->dateTime.date() < from || item->dateTime.date() > to) continue; // not one we want

        if (item->sport != sport) continue; // they don't want these

        // get the power data
        if (first == true) {

            // first time through the whole thing is going to be best
            returning = meanMaxPowerFor(
                context,
                returningwpk,
                RideFileCacheIntegrity::activitySourcePath(
                    item->path, item->fileName));

            // set a;; dates to this
            if (dates) {
                dates->resize(returning.size());
                for(int i=0; i<dates->size(); i++) (*dates)[i]=item->dateTime.date();
            }
            first = false;

        } else {

            QVector<float> thiswpk;

            // next time through we should only pick out better times
            QVector<float> ridebest = meanMaxPowerFor(
                context,
                thiswpk,
                RideFileCacheIntegrity::activitySourcePath(
                    item->path, item->fileName));

            // do we need to increase the returning array?
            if (returning.size() < ridebest.size()) returning.resize(ridebest.size());
            if (dates && dates->size() < ridebest.size()) dates->resize(ridebest.size());

            // now update where its a better number
            for (int i=0; i<ridebest.size(); i++) {
                if (ridebest[i] > returning[i]) {
                    returning[i] = ridebest[i];
                    (*dates)[i]=item->dateTime.date();
                }
           }

            // do we need to increase the returning array?
            if (returningwpk.size() < thiswpk.size()) returningwpk.resize(thiswpk.size());

            // now update where its a better number
            for (int i=0; i<thiswpk.size(); i++)
                if (thiswpk[i] > returningwpk[i]) returningwpk[i] =thiswpk[i];
        }
    }

    // set aggregated wpk
    wpk = returningwpk;
    return returning;
}

QVector<float> RideFileCache::meanMaxPowerFor(Context *context, QVector<float>&wpk, QString fileName)
{
    QVector<float> returning;

    const QString cacheFilename =
        RideFileCacheIntegrity::cachePathForActivity(
            context->athlete->home->cache().absolutePath(),
            context->athlete->home->activities().absolutePath(),
            context->athlete->home->planned().absolutePath(),
            fileName);
    if (cacheFilename.isEmpty()) {
        wpk.clear();
        return returning;
    }
    QVector<float> loaded;
    QVector<float> loadedWpk;
    if (readSourceBoundPartialCache(
            fileName,
            cacheFilename,
            nullptr,
            nullptr,
            [&](RideFileCacheIntegrity::
                    PartialReader &reader) {
                return reader.readBlock(
                           RideFileCacheIntegrity::
                               WattsMeanMax,
                           loaded)
                    && reader.readBlock(
                           RideFileCacheIntegrity::
                               WattsKgMeanMax,
                           loadedWpk);
            })) {
        for (float &value : loadedWpk)
            value /= 100.0f;
        wpk = std::move(loadedWpk);
        return loaded;
    }
    wpk.clear();

    // will be empty if no up to date cache
    return returning;

}

// the next 2 are used by the API web services to extract meanmax data from the cache

static bool meanMaxForSource(
    const QString &sourceFilename,
    const QString &cacheFilename,
    RideFile::SeriesType series,
    QVector<float> &values)
{
    values.clear();
    RideFileCacheIntegrity::Block block;
    if (!meanMaxBlockForSeries(series, block))
        return false;

    QVector<float> loaded;
    if (!readSourceBoundPartialCache(
            sourceFilename,
            cacheFilename,
            nullptr,
            nullptr,
            [&](RideFileCacheIntegrity::
                    PartialReader &reader) {
                return reader.readBlock(
                    block, loaded);
            })) {
        return false;
    }

    values = std::move(loaded);
    return true;
}

// API bests for a ride
QVector<float> RideFileCache::meanMaxFor(
    const QString &sourceFilename,
    const QString &cacheFilename,
    RideFile::SeriesType series)
{
    QVector<float> returning;
    meanMaxForSource(
        sourceFilename,
        cacheFilename,
        series,
        returning);
    return returning;

}

// API bests for a date range
QVector<float> RideFileCache::meanMaxFor(
    const QString &activityDir,
    const QString &cacheDir,
    RideFile::SeriesType series,
    QDate from,
    QDate to)
{
    bool first = true;
    QVector<float> returning;

    QSet<QString> visitedBasenames;
    foreach(QString activityFilename,
            QDir(activityDir).entryList(
                QDir::Files | QDir::NoDotAndDotDot)) {
        QDateTime dt;
        if (!RideFile::parseRideFileName(
                activityFilename, &dt)) {
            continue;
        }

        // in range?
        if (dt.date() < from || dt.date() > to) continue;

        const QFileInfo activityInfo(
            QDir(activityDir).filePath(
                activityFilename));
        const QString basename =
            activityInfo.baseName();
        if (basename.isEmpty()
            || visitedBasenames.contains(
                basename)) {
            continue;
        }

        // get data
        QVector<float> current;
        if (!meanMaxForSource(
                activityInfo.absoluteFilePath(),
                QDir(cacheDir).filePath(
                    basename
                    + QStringLiteral(".cpx")),
                series,
                current)) {
            continue;
        }
        visitedBasenames.insert(basename);

        // first ?
        if (first) {
            first = false;
            returning = current;
        } else {
            if (current.size() > returning.size()) returning.resize(current.size());
            for(int i=0; i< current.size(); i++) if (current[i] > returning[i]) returning[i]=current[i];
        }
    }

    // will be empty if no up to date cache
    return returning;

}

RideFileCache::RideFileCache(RideFile *ride) :
               crc(0), incomplete(true), context(ride->context),
               rideFileName(""), ride(ride),
               CP(0), WPRIME(0), LTHR(0), CV(0.0), WEIGHT(0.0),
               filter(false), onhome(false)
{
    // resize all the arrays to zero
    wattsMeanMax.resize(0);
    heatMeanMax.resize(0);
    hrMeanMax.resize(0);
    cadMeanMax.resize(0);
    nmMeanMax.resize(0);
    kphMeanMax.resize(0);
    kphdMeanMax.resize(0);
    wattsdMeanMax.resize(0);
    caddMeanMax.resize(0);
    nmdMeanMax.resize(0);
    hrdMeanMax.resize(0);
    xPowerMeanMax.resize(0);
    npMeanMax.resize(0);
    vamMeanMax.resize(0);
    wattsKgMeanMax.resize(0);
    aPowerMeanMax.resize(0);
    aPowerKgMeanMax.resize(0);
    wattsDistribution.resize(0);
    hrDistribution.resize(0);
    cadDistribution.resize(0);
    gearDistribution.resize(0);
    nmDistribution.resize(0);
    kphDistribution.resize(0);
    xPowerDistribution.resize(0);
    npDistribution.resize(0);
    wattsKgDistribution.resize(0);
    aPowerDistribution.resize(0);
    smo2Distribution.resize(0);
    wbalDistribution.resize(0);

    // time in zone are fixed to 10 zone max
    wattsTimeInZone.resize(10);
    wattsCPTimeInZone.resize(4);
    hrTimeInZone.resize(10);
    hrCPTimeInZone.resize(4);
    paceTimeInZone.resize(10);
    paceCPTimeInZone.resize(4);
    wbalTimeInZone.resize(4);

    WEIGHT = ride->getWeight();
    ride->recalculateDerivedSeries(); // accel and others

    // calculate all the arrays
    compute();
}

#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
bool
RideFileCache::cacheIsCurrentForSourceForTest(
    const QString &sourcePath,
    const QString &cachePath,
    double weight)
{
    return cacheIsCurrentForSource(
        sourcePath, cachePath, weight);
}

void
RideFileCache::setSourceBoundReadHookForTest(
    std::function<void()> hook)
{
    sourceBoundReadHookForTest =
        std::move(hook);
}

void
RideFileCache::
resetSourceFingerprintReadCountForTest()
{
    sourceFingerprintReadsForTest = 0;
}

int
RideFileCache::
sourceFingerprintReadCountForTest()
{
    return sourceFingerprintReadsForTest;
}

bool
RideFileCache::sourceBindingsAreCurrentForTest(
    const QVector<
        QPair<
            QString,
            RideFileCRC::ContentFingerprint>>
        &bindings)
{
    return sourceBindingsAreCurrent(bindings);
}

RideFileCache::RideFileCache(
    RideFile *ride,
    SkipInitialComputeForTest)
    : incomplete(true)
    , context(ride ? ride->context : nullptr)
    , rideFileName()
    , cacheFileName()
    , ride(ride)
    , CP(0)
    , WPRIME(0)
    , LTHR(0)
    , CV(0.0)
    , WEIGHT(ride ? ride->getWeight() : 0.0)
    , filter(false)
    , onhome(false)
{
    wattsTimeInZone.fill(0.0f, 10);
    wattsCPTimeInZone.fill(0.0f, 4);
    hrTimeInZone.fill(0.0f, 10);
    hrCPTimeInZone.fill(0.0f, 4);
    paceTimeInZone.fill(0.0f, 10);
    paceCPTimeInZone.fill(0.0f, 4);
    wbalTimeInZone.fill(0.0f, 4);
}

RideFileCache::RideFileCache(
    RideFile *ride,
    NoPersistentTargetForTest target)
    : RideFileCache(
          ride,
          SkipInitialComputeForTest {})
{
    context = ride ? ride->context : nullptr;
    computeWithoutPersistentCache(
        true, target.weight);
}

bool
RideFileCache::refreshCacheForTest(
    const QString &sourcePath,
    const QString &cachePath,
    const std::function<bool(
        const QString &,
        const RideFileCacheIntegrity::CacheWriteOperation &,
        QString *)> &writeCache,
    const std::function<void(
        const QString &,
        const QString &)> &reportError)
{
    rideFileName = sourcePath;
    cacheFileName = cachePath;
    const PersistenceOperations operations {
        [writeCache](
                const QString &path,
                const RideFileCacheIntegrity::CacheWriteOperation &write,
                const RideFileCacheIntegrity::
                    CachePreCommitValidator &,
                QString *error) {
            return writeCache(path, write, error);
        },
        reportError
    };
    return refreshCache(&operations);
}

bool
RideFileCache::refreshCacheWithValidatorForTest(
    const QString &sourcePath,
    const QString &cachePath,
    const std::function<bool(
        const QString &,
        const RideFileCacheIntegrity::CacheWriteOperation &,
        const RideFileCacheIntegrity::CachePreCommitValidator &,
        QString *)> &writeCache,
    const std::function<void(
        const QString &,
        const QString &)> &reportError)
{
    rideFileName = sourcePath;
    cacheFileName = cachePath;
    const PersistenceOperations operations {
        writeCache,
        reportError
    };
    return refreshCache(&operations);
}
#endif

int
RideFileCache::decimalsFor(RideFile::SeriesType series)
{
    switch (series) {
        case RideFile::secs : return 0; break;
        case RideFile::cad : return 0; break;
        case RideFile::gear : return 2; break;
        case RideFile::hr : return 0; break;
        case RideFile::km : return 3; break;
        case RideFile::kph : return 1; break;
        case RideFile::kphd : return 2; break;
        case RideFile::wattsd : return 0; break;
        case RideFile::cadd : return 0; break;
        case RideFile::nmd : return 2; break;
        case RideFile::hrd : return 0; break;
        case RideFile::nm : return 2; break;
        case RideFile::watts : return 0; break;
        case RideFile::xPower : return 0; break;
        case RideFile::IsoPower : return 0; break;
        case RideFile::alt : return 6; break;
        case RideFile::lon : return 6; break;
        case RideFile::lat : return 6; break;
        case RideFile::headwind : return 1; break;
        case RideFile::slope : return 1; break;
        case RideFile::temp : return 1; break;
        case RideFile::interval : return 0; break;
        case RideFile::vam : return 0; break;
        case RideFile::wattsKg : return 2; break;
        case RideFile::aPower : return 0; break;
        case RideFile::aPowerKg : return 2; break;
        case RideFile::smo2 : return 0; break;
        case RideFile::wbal : return 0; break;
        case RideFile::lrbalance : return 1; break;
        case RideFile::wprime :  return 0; break;
        case RideFile::none : break;
        default : return 2;
    }
    return 2; // default
}

//
// DATA ACCESS
//
QVector<QDate> &
RideFileCache::meanMaxDates(RideFile::SeriesType series)
{
    switch (series) {

        case RideFile::watts:
            return wattsMeanMaxDate;
            break;

        case RideFile::cad:
            return cadMeanMaxDate;
            break;

        case RideFile::hr:
            return hrMeanMaxDate;
            break;

        case RideFile::nm:
            return nmMeanMaxDate;
            break;

        case RideFile::kph:
            return kphMeanMaxDate;
            break;

        case RideFile::kphd:
            return kphdMeanMaxDate;
            break;

        case RideFile::wattsd:
            return wattsdMeanMaxDate;
            break;

        case RideFile::cadd:
            return caddMeanMaxDate;
            break;

        case RideFile::nmd:
            return nmdMeanMaxDate;
            break;

        case RideFile::hrd:
            return hrdMeanMaxDate;
            break;

        case RideFile::xPower:
            return xPowerMeanMaxDate;
            break;

        case RideFile::IsoPower:
            return npMeanMaxDate;
            break;

        case RideFile::vam:
            return vamMeanMaxDate;
            break;

        case RideFile::aPower:
            return aPowerMeanMaxDate;
            break;

        case RideFile::aPowerKg:
            return aPowerKgMeanMaxDate;
            break;

        case RideFile::wattsKg:
            return wattsKgMeanMaxDate;
            break;

        default:
            //? dunno give em power anyway
            return wattsMeanMaxDate;
            break;
    }
}

QList<RideFile::SeriesType> RideFileCache::meanMaxList()
{
        QList<RideFile::SeriesType> list;
        list << RideFile::watts
             << RideFile::wattsKg
             << RideFile::nm
             << RideFile::hr
             << RideFile::cad
             << RideFile::kph
             << RideFile::vam
             << RideFile::IsoPower
             << RideFile::aPower
             << RideFile::aPowerKg
             << RideFile::xPower
             ;

    return list;
}

QVector<double> &
RideFileCache::meanMaxArray(RideFile::SeriesType series)
{
    switch (series) {

        case RideFile::watts:
            return wattsMeanMaxDouble;
            break;

        case RideFile::cad:
            return cadMeanMaxDouble;
            break;

        case RideFile::hr:
            return hrMeanMaxDouble;
            break;

        case RideFile::nm:
            return nmMeanMaxDouble;
            break;

        case RideFile::kph:
            return kphMeanMaxDouble;
            break;

        case RideFile::kphd:
            return kphdMeanMaxDouble;
            break;

        case RideFile::wattsd:
            return wattsdMeanMaxDouble;
            break;

        case RideFile::cadd:
            return caddMeanMaxDouble;
            break;

        case RideFile::nmd:
            return nmdMeanMaxDouble;
            break;

        case RideFile::hrd:
            return hrdMeanMaxDouble;
            break;

        case RideFile::xPower:
            return xPowerMeanMaxDouble;
            break;

        case RideFile::IsoPower:
            return npMeanMaxDouble;
            break;

        case RideFile::vam:
            return vamMeanMaxDouble;
            break;

        case RideFile::aPower:
            return aPowerMeanMaxDouble;
            break;

        case RideFile::aPowerKg:
            return aPowerKgMeanMaxDouble;
            break;

        case RideFile::wattsKg:
            return wattsKgMeanMaxDouble;
            break;

        default:
            //? dunno give em power anyway
            return wattsMeanMaxDouble;
            break;
    }
}

QVector<double> &
RideFileCache::distributionArray(RideFile::SeriesType series)
{
    switch (series) {

        case RideFile::watts:
            return wattsDistributionDouble;
            break;

        case RideFile::cad:
            return cadDistributionDouble;
            break;

       case RideFile::gear:
           return gearDistributionDouble;
           break;

        case RideFile::hr:
            return hrDistributionDouble;
            break;

        case RideFile::nm:
            return nmDistributionDouble;
            break;

        case RideFile::kph:
            return kphDistributionDouble;
            break;

        case RideFile::aPower:
            return aPowerDistributionDouble;
            break;

        case RideFile::smo2:
            return smo2DistributionDouble;
            break;

        case RideFile::wbal:
            return wbalDistributionDouble;
            break;

        case RideFile::wattsKg:
            return wattsKgDistributionDouble;
            break;

        default:
            //? dunno give em power anyway
            return wattsMeanMaxDouble;
            break;
    }
}

RideFileCache *
RideFileCache::createCacheFor(RideFile*rideFile)
{
    return new RideFileCache(rideFile);
}

//
// COMPUTATION
//
bool
RideFileCache::refreshCache(
    const PersistenceOperations *operations)
{
    // Recompute before persistence so a cache write failure does not discard
    // otherwise valid in-memory results.
    if (ride)
        ride->recalculateDerivedSeries();
    if (!compute())
        return false;

    RideFile::SourceFingerprint sourceFingerprint;
    if (!ride
        || !RideFile::captureSourceFingerprint(
            rideFileName, sourceFingerprint)
        || !ride->sourceProvenanceMatches(
            sourceFingerprint)) {
        return false;
    }

    // Publish only data derived from this stable source fingerprint.
    crc = sourceFingerprint.crc;
    RideFileCRC::ContentFingerprint
        persistedSourceFingerprint;
    persistedSourceFingerprint.byteSize =
        sourceFingerprint.size;
    persistedSourceFingerprint.sha256 =
        sourceFingerprint.sha256;
    persistedSourceFingerprint.legacyCrc16 =
        sourceFingerprint.crc;
    if (!persistedSourceFingerprint.isValid())
        return false;

    const auto serializeSnapshot =
        [](RideFileCache &cache,
           const RideFileCRC::ContentFingerprint
               &source,
           QByteArray &payload) {
            payload.clear();
            QBuffer output(&payload);
            if (!output.open(QIODevice::WriteOnly))
                return false;
            QDataStream stream(&output);
            return cache.serialize(
                       &stream, source)
                && stream.status()
                    == QDataStream::Ok;
        };

    QByteArray payload;
    if (!serializeSnapshot(
            *this,
            persistedSourceFingerprint,
            payload)) {
        return false;
    }

    QStringList verificationErrors;
    QFile verificationSource(rideFileName);
    std::unique_ptr<RideFile> verifiedRide(
        RideFileFactory::instance().openRideFile(
            context,
            verificationSource,
            verificationErrors));
    if (!verifiedRide
        || !verifiedRide->sourceProvenanceMatches(
            sourceFingerprint)) {
        return false;
    }

    RideFileCache verifiedCache(verifiedRide.get());
    verifiedCache.crc = sourceFingerprint.crc;
    QByteArray verifiedPayload;
    if (!serializeSnapshot(
            verifiedCache,
            persistedSourceFingerprint,
            verifiedPayload)
        || payload != verifiedPayload) {
        return false;
    }
    sourceFingerprint_ =
        persistedSourceFingerprint;

    QDir().mkpath(QFileInfo(cacheFileName).absolutePath());
    QString writeError;
    const RideFileCacheIntegrity::CacheWriteOperation serializeCache =
        [&payload](QIODevice &output, QString *error) {
            qint64 written = 0;
            while (written < payload.size()) {
                const qint64 count = output.write(
                    payload.constData() + written,
                    payload.size() - written);
                if (count <= 0) {
                    if (error) {
                        *error = QStringLiteral(
                            "Cannot serialize CPX cache");
                    }
                    return false;
                }
                written += count;
            }
            return true;
        };
    bool sourceValidationRejected = false;
    const RideFileCacheIntegrity::CachePreCommitValidator
        validateBeforeCommit =
            [this,
             sourceFingerprint,
             &sourceValidationRejected](QString *error) {
                RideFile::SourceFingerprint current;
                const bool valid =
                    ride
                    && ride->sourceProvenanceMatches(
                        sourceFingerprint)
                    && RideFile::captureSourceFingerprint(
                        rideFileName, current)
                    && current == sourceFingerprint;
                if (!valid) {
                    sourceValidationRejected = true;
                    if (error) {
                        *error = QStringLiteral(
                            "Activity source changed before CPX cache commit");
                    }
                }
                return valid;
            };
    const bool persisted =
        operations && operations->writeCache
        ? operations->writeCache(
              cacheFileName,
              serializeCache,
              validateBeforeCommit,
              &writeError)
        : RideFileCacheIntegrity::writeCacheAtomically(
              cacheFileName,
              serializeCache,
              validateBeforeCommit,
              &writeError);

    if (persisted) {
        // invalidate any incore cache of aggregate
        // that contains this ride in its date range
        QDate date = ride->startTime().date();
        if (context && context->athlete) {
            for (int i=0; i<context->athlete->cpxCache.count();) {
                if (date >= context->athlete->cpxCache.at(i)->start &&
                    date <= context->athlete->cpxCache.at(i)->end) {
                    delete context->athlete->cpxCache.at(i);
                    context->athlete->cpxCache.removeAt(i);
                } else i++;
            }
        }
        return true;
    } else {
        if (sourceValidationRejected)
            return false;
        if (!operations) {
            qWarning().noquote()
                << QStringLiteral("Cannot create cache file %1: %2.")
                       .arg(cacheFileName, writeError);
        }
        if (operations && operations->reportError) {
            operations->reportError(
                cacheFileName, writeError);
        } else if (context) {
            context->reportCacheWriteFailure(
                cacheFileName, writeError);
        }
        return false;
    }
}

// if you already have a cache open and want
// to refresh it from in-memory data then refresh()
// does that
void RideFileCache::refresh(RideFile *file)
{
    // set and refresh
    if (file) ride = file;

    // just call compute!
    compute();
}

bool
RideFileCache::computeWithoutPersistentCache(
    bool refresh,
    double weight)
{
    incomplete = true;
    if (!refresh || !ride)
        return false;
    WEIGHT = weight;
    return compute();
}

// this function is a candidate for supporting
// threaded calculations, each of the computes
// in here could go in its own thread. Users
// with many cores would benefit enormously
bool RideFileCache::RideFileCache::compute()
{
    incomplete = true;
    if (ride == NULL) {
        return false;
    }

    clearPublishedArrays();
    wattsTimeInZone.fill(0.0f, 10);
    wattsCPTimeInZone.fill(0.0f, 4);
    hrTimeInZone.fill(0.0f, 10);
    hrCPTimeInZone.fill(0.0f, 4);
    paceTimeInZone.fill(0.0f, 10);
    paceCPTimeInZone.fill(0.0f, 4);
    wbalTimeInZone.fill(0.0f, 4);

    // all the mean maxes
    MeanMaxComputer thread1(ride, wattsMeanMax, RideFile::watts); thread1.start();
    MeanMaxComputer thread2(ride, hrMeanMax, RideFile::hr); thread2.start();
    MeanMaxComputer thread3(ride, cadMeanMax, RideFile::cad); thread3.start();
    MeanMaxComputer thread4(ride, nmMeanMax, RideFile::nm); thread4.start();
    MeanMaxComputer thread5(ride, kphMeanMax, RideFile::kph); thread5.start();
    MeanMaxComputer thread6(ride, xPowerMeanMax, RideFile::xPower); thread6.start();
    MeanMaxComputer thread7(ride, npMeanMax, RideFile::IsoPower); thread7.start();
    MeanMaxComputer thread8(ride, vamMeanMax, RideFile::vam); thread8.start();
    MeanMaxComputer thread9(ride, wattsKgMeanMax, RideFile::wattsKg); thread9.start();
    MeanMaxComputer thread10(ride, aPowerMeanMax, RideFile::aPower); thread10.start();
    MeanMaxComputer thread11(ride, kphdMeanMax, RideFile::kphd); thread11.start();
    MeanMaxComputer thread12(ride, wattsdMeanMax, RideFile::wattsd); thread12.start();
    MeanMaxComputer thread13(ride, caddMeanMax, RideFile::cadd); thread13.start();
    MeanMaxComputer thread14(ride, nmdMeanMax, RideFile::nmd); thread14.start();
    MeanMaxComputer thread15(ride, hrdMeanMax, RideFile::hrd); thread15.start();
    MeanMaxComputer thread16(ride, aPowerKgMeanMax, RideFile::aPowerKg); thread16.start();

    // all the different distributions
    computeDistribution(wattsDistribution, RideFile::watts);
    computeDistribution(hrDistribution, RideFile::hr);
    computeDistribution(cadDistribution, RideFile::cad);
    computeDistribution(gearDistribution, RideFile::gear);
    computeDistribution(nmDistribution, RideFile::nm);
    computeDistribution(kphDistribution, RideFile::kph);
    computeDistribution(wattsKgDistribution, RideFile::wattsKg);
    computeDistribution(aPowerDistribution, RideFile::aPower);
    computeDistribution(smo2Distribution, RideFile::smo2);
    computeDistribution(wbalDistribution, RideFile::wbal);

    // wait for them threads
    thread1.wait();
    thread2.wait();
    thread3.wait();
    thread4.wait();
    thread5.wait();
    thread6.wait();
    thread7.wait();
    thread8.wait();
    thread9.wait();
    thread10.wait();
    thread11.wait();
    thread12.wait();
    thread13.wait();
    thread14.wait();
    thread15.wait();
    thread16.wait();

    // setup the doubles the users use
    doubleArray(wattsMeanMaxDouble, wattsMeanMax, RideFile::watts);
    doubleArray(hrMeanMaxDouble, hrMeanMax, RideFile::hr);
    doubleArray(cadMeanMaxDouble, cadMeanMax, RideFile::cad);
    doubleArray(nmMeanMaxDouble, nmMeanMax, RideFile::nm);
    doubleArray(kphMeanMaxDouble, kphMeanMax, RideFile::kph);
    doubleArray(kphdMeanMaxDouble, kphdMeanMax, RideFile::kphd);
    doubleArray(wattsdMeanMaxDouble, wattsdMeanMax, RideFile::wattsd);
    doubleArray(caddMeanMaxDouble, caddMeanMax, RideFile::cadd);
    doubleArray(nmdMeanMaxDouble, nmdMeanMax, RideFile::nmd);
    doubleArray(hrdMeanMaxDouble, hrdMeanMax, RideFile::hrd);
    doubleArray(npMeanMaxDouble, npMeanMax, RideFile::IsoPower);
    doubleArray(vamMeanMaxDouble, vamMeanMax, RideFile::vam);
    doubleArray(xPowerMeanMaxDouble, xPowerMeanMax, RideFile::xPower);
    doubleArray(wattsKgMeanMaxDouble, wattsKgMeanMax, RideFile::wattsKg);
    doubleArray(aPowerMeanMaxDouble, aPowerMeanMax, RideFile::aPower);
    doubleArray(aPowerKgMeanMaxDouble, aPowerKgMeanMax, RideFile::aPowerKg);

    doubleArrayForDistribution(wattsDistributionDouble, wattsDistribution);
    doubleArrayForDistribution(hrDistributionDouble, hrDistribution);
    doubleArrayForDistribution(cadDistributionDouble, cadDistribution);
    doubleArrayForDistribution(gearDistributionDouble, gearDistribution);
    doubleArrayForDistribution(nmDistributionDouble, nmDistribution);
    doubleArrayForDistribution(kphDistributionDouble, kphDistribution);
    doubleArrayForDistribution(xPowerDistributionDouble, xPowerDistribution);
    doubleArrayForDistribution(npDistributionDouble, npDistribution);
    doubleArrayForDistribution(wattsKgDistributionDouble, wattsKgDistribution);
    doubleArrayForDistribution(aPowerDistributionDouble, aPowerDistribution);
    doubleArrayForDistribution(smo2DistributionDouble, smo2Distribution);
    doubleArrayForDistribution(wbalDistributionDouble, wbalDistribution);

    incomplete = false;
    return true;
}

//----------------------------------------------------------------------
// Mark Rages' Algorithm for Fast Find of Mean-Max
//----------------------------------------------------------------------

/*

   A Faster Mean-Max Algorithm

   Premises:

   1 - maximum average power for a given interval occurs at maximum
       energy for the interval, because the interval time is fixed;

   2 - the energy in an interval enclosing a smaller interval will
       always be equal or greater than an interval;

   3 - finding maximum of means is a search algorithm, so biggest
       gains are found in reducing the search space as quickly as
       possible.

   Algorithm

   note: I find it easier to reason with concrete numbers, so I will
   describe the algorithm in terms of power and 60 second max-mean:

   To find the maximum average power for one minute:

   1 - integrate the watts over the entire ride to get accumulated
       energy in joules.  This is a monotonic function (assuming watts
       are positive).  The final value is the energy for the whole
       ride.  Once this is done, the energy for any section can be
       found with a single subtraction.

   2 - divide the energy into overlapping two-minute sections.
       Section one = 0:00 -> 2:00, section two = 1:00 -> 3:00, etc.

       Example:  Find 60s MM in 5-minute file

       +----------+----------+----------+----------+----------+
       | minute 1 | minute 2 | minute 3 | minute 4 | minute 5 |
       +----------+----------+----------+----------+----------+
       |             |_MEAN_MAX_|                             |
       +---------------------+---------------------+----------+
       |      segment 1      |      segment 3      |
       +----------+----------+----------+----------+----------+
                  |      segment 2      |      segment 4      |
                  +---------------------+---------------------+

       So no matter where the MEAN_MAX segment is located in time, it
       will be wholly contained in one segment.

       In practice, it is a little faster to make the windows smaller
       and overlap more:
       +----------+----------+----------+----------+----------+
       | minute 1 | minute 2 | minute 3 | minute 4 | minute 5 |
       +----------+----------+----------+----------+----------+
       |             |_MEAN_MAX_|                             |
       +-------------+----------------------------------------+
          |  segment 1  |
          +--+----------+--+
          |  segment 2  |
          +--+----------+--+
             |  segment 3  |
             +--+----------+--+
                |  segment 4  |
                +--+----------+--+
                   |  segment 5  |
                   +--+----------+--+
                      |  segment 6  |
                      +--+----------+--+
                         |  segment 7  |
                         +--+----------+--+
                            |  segment 8  |
                            +--+----------+--+
                               |  segment 9  |
                               +-------------+
                                            ... etc.

       ( This is because whenever the actual mean max energy is
         greater than a segment energy, we can skip the detail
         comparison within that segment altogether.  The exact
         tradeoff for optimum performance depends on the distribution
         of the data.  It's a pretty shallow curve.  Values in the 1
         minute to 1.5 minute range seem to work pretty well. )

   3 - for each two minute section, subtract the accumulated energy at
       the end of the section from the accumulated energy at the
       beginning of the section.  That gives the energy for that section.

   4 - in the first section, go second-by-second to find the maximum
       60-second energy.  This is our candidate for 60-second energy

   5 - go down the sorted list of sections.  If the energy in the next
       section is less than the 60-second energy in the best candidate so
       far, skip to the next section without examining it carefully,
       because the section cannot possibly have a one-minute section with
       greater energy.

       while (section->energy > candidate) {
         candidate=max(candidate, search(section, 60));
         section++;
       }

   6. candidate is the mean max for 60 seconds.

   Enhancements that are not implemented:

     - The two-minute overlapping sections can be reused for 59
       seconds, etc.  The algorithm will degrade to exhaustive search
       if the looked-for interval is much smaller than the enclosing
       interval.

     - The sections can be sorted by energy in reverse order before
       step #4.  Then the search in #5 can be terminated early, the
       first time it fails.  In practice, the comparisons in the
       search outnumber the saved comparisons.  But this might be a
       useful optimization if the windows are reused per the previous
       idea.

*/

static data_t *
integrate_series(cpintdata &data)
{
    // would be better to do pure QT and use QVector -- but no memory leak
    data_t *integrated= (data_t *)malloc(sizeof(data_t)*(data.points.size()+1)); 
    int i;
    data_t acc=0;

    for (i=0; i<data.points.size(); i++) {
        integrated[i]=acc;
        acc+=data.points[i].value;
    }
    integrated[i]=acc;

    return integrated;
}

static data_t
partial_max_mean(data_t *dataseries_i, int start, int end, int length, int *offset)
{
    int i=0;
    data_t candidate=0;

    int best_i=0;

    for (i=start; i<(1+end-length); i++) {
        data_t test_energy=dataseries_i[length+i]-dataseries_i[i];
        if (test_energy>candidate) {
            candidate=test_energy;
            best_i=i;
        }
    }
    if (offset) *offset=best_i;

    return candidate;
}


static data_t
divided_max_mean(data_t *dataseries_i, int datalength, int length, int *offset)
{
    int shift=length;

    //if sorting data the following is an important speedup hack
    if (shift>180) shift=180;

    int window_length=length+shift;

    if (window_length>datalength) window_length=datalength;

    // put down as many windows as will fit without overrunning data
    int start=0;
    int end=0;
    data_t energy=0;

    data_t candidate=0;
    int this_offset=0;

    for (start=0; start+window_length<=datalength; start+=shift) {
        end=start+window_length;
        energy=dataseries_i[end]-dataseries_i[start];

        if (energy < candidate) {
          continue;
        }
        data_t window_mm=partial_max_mean(dataseries_i, start, end, length, &this_offset);

        if (window_mm>candidate) {
            candidate=window_mm;
            if (offset) *offset=this_offset;
        }
    }

    // if the overlapping windows don't extend to the end of the data,
    // let's tack another one on at the end

    if (end<datalength) {
        start=datalength-window_length;
        end=datalength;
        energy=dataseries_i[end]-dataseries_i[start];

        if (energy >= candidate) {

            data_t window_mm=partial_max_mean(dataseries_i, start, end, length, &this_offset);

            if (window_mm>candidate) {
                candidate=window_mm;
                if (offset) *offset=this_offset;
            }
        }
    }

    return candidate;
}


void
MeanMaxComputer::run()
{
    // xPower and IsoPower need watts to be present
    RideFile::SeriesType baseSeries = (series == RideFile::xPower || series == RideFile::IsoPower || series == RideFile::wattsKg) ?
                                      RideFile::watts : series;

    if (series == RideFile::aPowerKg) baseSeries = RideFile::aPower;
    else if (series == RideFile::vam) baseSeries = RideFile::alt;

    // there is a distinction between needing it present and using it in calcs
    RideFile::SeriesType needSeries = baseSeries;
    if (series == RideFile::kphd) needSeries = RideFile::kph;
    if (series == RideFile::wattsd) needSeries = RideFile::watts;
    if (series == RideFile::cadd) needSeries = RideFile::cad;
    if (series == RideFile::nmd) needSeries = RideFile::nm;
    if (series == RideFile::hrd) needSeries = RideFile::hr;

    // only bother if the data series is actually present
    if (ride->isDataPresent(needSeries) == false) return;

    // if we want decimal places only keep to 1 dp max
    // this is a factor that is applied at the end to
    // convert from high-precision double to long
    // e.g. 145.456 becomes 1455 if we want decimals
    // and becomes 145 if we don't
    double decimals =  pow(10, RideFileCache::decimalsFor(series));
    //double decimals = RideFile::decimalsFor(baseSeries) ? 10 : 1;

    // decritize the data series - seems wrong, since it just
    // rounds to the nearest second - what if the recIntSecs
    // is less than a second? Has been used for a long while
    // so going to leave in tact for now - apart from the
    // addition of code to fill in gaps in recording since
    // they affect the IsoPower/xPower algorithm badly and will skew
    // the calculations of >6m since windowsize is used to
    // determine segment duration rather than examining the
    // timestamps on each sample
    // the decrit will also pull timestamps back to start at
    // zero, since some files have a very large start time
    // that creates work for nil effect (but increases compute
    // time drastically).
    cpintdata data;
    data.rec_int_ms = (int) round(ride->recIntSecs() * 1000.0);
    double lastsecs = 0;
    bool first = true;
    double offset = 0;
    foreach (const RideFilePoint *p, ride->dataPoints()) {

        // get offset to apply on all samples if first sample
        if (first == true) {
            offset = p->secs;
            first = false;
        }

        // drag back to start at 1s or whatever recIntSecs() is !
        double psecs = p->secs - offset + ride->recIntSecs();

        // fill in any gaps in recording - use same dodgy rounding as before
        int count = (psecs - lastsecs - ride->recIntSecs()) / ride->recIntSecs();

        // gap more than an hour, damn that ride file is a mess
        if (count > 3600) count = 1;

        for(int i=0; i<count; i++)
            data.points.append(cpintpoint(round(lastsecs+((i+1)*ride->recIntSecs() *1000.0)/1000), 0));
        lastsecs = psecs;

        double secs = round(psecs * 1000.0) / 1000;
        if (secs > 0) data.points.append(cpintpoint(secs, (int) round(p->value(baseSeries)*double(decimals))));
    }


    // don't bother with insufficient data
    if (!data.points.count()) return;

    int total_secs = (int) ceil(data.points.back().secs);

    // don't allow data more than two days
    // was one week, but no single ride is longer
    // than 2 days, even if you are doing RAAM
    if (total_secs > 2*24*60*60) return;

    // don't allow if badly parsed or time goes backwards
    if (total_secs < 0) return;

    //
    // Pre-process the data for IsoPower, xPower and VAM
    //

    // VAM - adjust to Vertical Ascent per Hour
    if (series == RideFile::vam) {

        double lastAlt=0;

        for (int i=0; i<data.points.size(); i++) {

            // handle drops gracefully (and first sample too)
            // if you manage to rise >5m in a second thats a data error too!
            if (!lastAlt || (data.points[i].value - lastAlt) > 5) lastAlt=data.points[i].value;

            // NOTE: It is 360 not 3600 because Altitude is factored for decimal places
            //       since it is the base data series, but we are calculating VAM
            //       And we multiply by 10 at the end!
            double vam = (((data.points[i].value - lastAlt) * 360)/ride->recIntSecs()) * 10;
            if (vam < 0) vam = 0;
            lastAlt = data.points[i].value;
            data.points[i].value = vam;
        }
    }

    // IsoPower - rolling 30s avg ^ 4
    if (series == RideFile::IsoPower) {

        int rollingwindowsize = 30 / ride->recIntSecs();

        // no point doing a rolling average if the
        // sample rate is greater than the rolling average
        // window!!
        if (rollingwindowsize > 1) {

            QVector<double> rolling(rollingwindowsize);
            int index = 0;
            double sum = 0;

            // loop over the data and convert to a rolling
            // average for the given windowsize
            for (int i=0; i<data.points.size(); i++) {

                sum += data.points[i].value;
                sum -= rolling[index];

                rolling[index] = data.points[i].value;
                data.points[i].value = pow(sum/(double)rollingwindowsize,4.0f); // raise rolling average to 4th power

                // move index on/round
                index = (index >= rollingwindowsize-1) ? 0 : index+1;
            }
        }
    }

    // xPower - 25s EWA - uses same algorithm as BikeScore.cpp
    if (series == RideFile::xPower) {

        const double exp = ride->recIntSecs() / ((25.0f / ride->recIntSecs()) + ride->recIntSecs());
        const double rem = 1.0f - exp;

        int rollingwindowsize = 25 / ride->recIntSecs();
        double ewma = 0.0;
        double sum = 0.0; // as we ramp up

        // no point doing a rolling average if the
        // sample rate is greater than the rolling average
        // window!!
        if (rollingwindowsize > 1) {

            // loop over the data and convert to a EWMA
            for (int i=0; i<data.points.size(); i++) {

                // dgr : BikeScore has weighting value from first point
                if (false && i < rollingwindowsize) {

                    // get up to speed
                    sum += data.points[i].value;
                    ewma = sum / (i+1);

                } else {

                    // we're up to speed
                    ewma = (data.points[i].value * exp) + (ewma * rem);
                }
                data.points[i].value = pow(ewma, 4.0f);
            }
        }
    }

    if (series == RideFile::wattsKg || series == RideFile::aPowerKg) {
        for (int i=0; i<data.points.size(); i++) {
            double wattsKg = data.points[i].value / ride->getWeight();
            data.points[i].value = wattsKg;
        }
    }


    // the bests go in here...
    QVector <double> ride_bests(total_secs + 1);

    data_t *dataseries_i = integrate_series(data);

    for (int i=1; i<data.points.size();) {

        int offset;
        data_t c=divided_max_mean(dataseries_i,data.points.size(),i,&offset);

        // snaffle it away
        int sec = i*ride->recIntSecs();
        data_t val = c / (data_t)i;

        if (sec < ride_bests.size()) {
            if (series == RideFile::IsoPower || series == RideFile::xPower)
                ride_bests[sec] = pow(val, 0.25f);
            else
                ride_bests[sec] = val;
        }


        // increments to limit search scope
        if (i<120) i++;
        else if (i<600) i+= 2;
        else if (i<1200) i += 5;
        else if (i<3600) i += 20;
        else if (i<7200) i += 120;
        else i += 300;
    }
    free(dataseries_i);

    //
    // FILL IN THE GAPS AND FILL TARGET ARRAY
    //
    // We want to present a full set of bests for
    // every duration so the data interface for this
    // cache can remain the same, but the level of
    // accuracy/granularity can change in here in the
    // future if some fancy new algorithm arrives
    //

    // XXX seems we can end up with 0 at the end ?
    // XXX don't know why, so, for now, just clean that
    while (ride_bests.size() && ride_bests[ride_bests.size()-1] == 0)
        ride_bests.resize(ride_bests.size()-1);

    double last = 0;

    // only care about first 3 minutes MAX for delta series
    if ((series == RideFile::kphd  || series == RideFile::wattsd || series == RideFile::cadd ||
        series == RideFile::nmd  || series == RideFile::hrd) && ride_bests.count() > 180) {
        ride_bests.resize(180);
        array.resize(180);
    } else {
        array.resize(ride_bests.count());
    }

    // bounds check, it might be empty!
    if (ride_bests.size()) {
        for (int i=ride_bests.size()-1; i; i--) {
            if (ride_bests[i] == 0) ride_bests[i]=last;
            else last = ride_bests[i];

            // convert from double to long, preserving the
            // precision by applying a multiplier
            array[i] = ride_bests[i]; // * decimals; -- we did that earlier
        }
    }
}

// self-contained static routine to perform the fast search algorithm
// on a single series of data, using ints only assuming data is in 1s 
// intervals with no data issues.
void RideFileCache::fastSearch(QVector<int>&input, QVector<int>&ride_bests, QVector<int>&ride_offsets)
{
    // use the raw C structure to reduce overhead and on stack
    // Although with MSVC we have no choice, sadly.
#ifdef Q_CC_MSVC
    data_t *dataseries_i = new data_t[input.count()+1];
#else
    data_t dataseries_i[input.count()+1];
#endif
    data_t acc=0;

    // resize output
    ride_bests.resize(input.count()+1);
    ride_offsets.resize(input.count()+1);

    // aggregate here, instead of using utility function
    int j=0;
    for (; j<input.count(); j++) {
        dataseries_i[j]=acc;
        acc+=input[j];
    }
    dataseries_i[j]=acc;

    // run the algorithm
    for (int i=1; i<input.count();) {

        int offset;
        data_t c=divided_max_mean(dataseries_i,input.count(),i,&offset);

        // snaffle it away
        data_t val = c / (data_t)i;

        // save away
        ride_bests[i] = val;
        ride_offsets[i] = offset;

        // increments to limit search scope
        if (i<120) i++;
        else if (i<600) i+= 2;
        else if (i<1200) i += 5;
        else if (i<3600) i += 20;
        else if (i<7200) i += 120;
        else i += 300;
    }
#ifdef Q_CC_MSVC
    delete[] dataseries_i;
#endif

    // since we minimise the search space over
    // longer durations we need to fill in the gaps
    int last=0;
    for (int i=ride_bests.size()-1; i; i--) {
        if (ride_bests[i] == 0) ride_bests[i]=last;
        else last = ride_bests[i];
    }
}

void
RideFileCache::computeDistribution(QVector<float> &array, RideFile::SeriesType series)
{
    RideFile::SeriesType baseSeries = series;

    if (series == RideFile::wattsKg)
        baseSeries = RideFile::watts;
    else if (series == RideFile::aPowerKg)
        baseSeries = RideFile::aPower;

    // there is a distinction between needing it present and using it in calcs
    RideFile::SeriesType needSeries = baseSeries;
    if (series == RideFile::kphd) needSeries = RideFile::kph;
    if (series == RideFile::wattsd) needSeries = RideFile::watts;
    if (series == RideFile::cadd) needSeries = RideFile::cad;
    if (series == RideFile::nmd) needSeries = RideFile::nm;
    if (series == RideFile::hrd) needSeries = RideFile::hr;
    if (series == RideFile::wbal) needSeries = RideFile::watts;

    // only bother if the data series is actually present
    if (ride->isDataPresent(needSeries) == false) return;

    // get zones that apply, if any
    const Athlete *athlete =
        context ? context->athlete : nullptr;
    int zoneRange =
        athlete && athlete->zones(ride->sport())
        ? athlete->zones(ride->sport())->whichRange(
              ride->startTime().date())
        : -1;
    int hrZoneRange =
        athlete && athlete->hrZones(ride->sport())
        ? athlete->hrZones(ride->sport())->whichRange(
              ride->startTime().date())
        : -1;
    int paceZoneRange =
        athlete && athlete->paceZones(ride->isSwim())
        ? athlete->paceZones(ride->isSwim())->whichRange(
              ride->startTime().date())
        : -1;

    CP=0;
    int AeTP=0;
    if (zoneRange != -1) {
        CP=context->athlete->zones(ride->sport())->getCP(zoneRange);
        AeTP=context->athlete->zones(ride->sport())->getAeT(zoneRange);
    }

    if (zoneRange != -1) WPRIME=context->athlete->zones(ride->sport())->getWprime(zoneRange);
    else WPRIME=0;

    LTHR=0;
    int AeTHR=0;
    if (hrZoneRange != -1) {
        LTHR=context->athlete->hrZones(ride->sport())->getLT(hrZoneRange);
        AeTHR=context->athlete->hrZones(ride->sport())->getAeT(hrZoneRange);
    }

    CV=0;
    double AeTV=0;
    if (paceZoneRange != -1) {
        CV=context->athlete->paceZones(ride->isSwim())->getCV(paceZoneRange);
        AeTV=context->athlete->paceZones(ride->isSwim())->getAeT(paceZoneRange);
    }

    // setup the array based upon the ride
    int decimals = decimalsFor(series); //RideFile::decimalsFor(series) ? 1 : 0;
    double min = RideFile::minimumFor(series) * pow(10, decimals);
    double max = RideFile::maximumFor(series) * pow(10, decimals);

    // lets resize the array to the right size
    // it will also initialise with a default value
    // which for longs is handily zero
    array.resize(max-min+1);

    // wbal uses the wprimeData, not the ridefile
    if (series == RideFile::wbal) {

        // set timeinzone to zero
        wbalTimeInZone.fill(0.0f, 4);
        array.fill(0.0f);

        if (WPRIME <= 0)
            return;

        // lets count them first then turn into percentages
        // after we have traversed all the data
        foreach(int value, ride->wprimeData()->ydata()) {

            // percent is PERCENT OF W' USED
            int percent = 100.0f - ((double (value) / WPRIME) * 100.0f);
            if (percent < 0.0f) percent = 0.0f;
            if (percent > 100.0f) percent = 100.0f;

            // increment counts
            array[percent]++;

            // and zones in 1s increments
            if (percent <= 25.0f) wbalTimeInZone[0]++;
            else if (percent <= 50.0f) wbalTimeInZone[1]++;
            else if (percent <= 75.0f) wbalTimeInZone[2]++;
            else wbalTimeInZone[3]++;
        }

    } else {

        foreach(RideFilePoint *dp, ride->dataPoints()) {
            double value = dp->value(baseSeries);
            if (series == RideFile::wattsKg || series == RideFile::aPowerKg) {
                value /= ride->getWeight();
            }

            float lvalue = value * pow(10, decimals);

            // watts time in zone
            if (series == RideFile::watts && zoneRange != -1) {
                int index = context->athlete->zones(ride->sport())->whichZone(zoneRange, dp->value(series));
                if (index >=0) wattsTimeInZone[index] += ride->recIntSecs();
            }

            // Polarized zones :- I(<AeTP), II (<CP and >0.85*CP), III (>CP)
            if (series == RideFile::watts && zoneRange != -1 && CP) {
                if (dp->value(series) < 1) // I zero watts
                    wattsCPTimeInZone[0] += ride->recIntSecs();
                else if (dp->value(series) < AeTP) // I
                    wattsCPTimeInZone[1] += ride->recIntSecs();
                else if (dp->value(series) < CP) // II
                    wattsCPTimeInZone[2] += ride->recIntSecs();
                else // III
                    wattsCPTimeInZone[3] += ride->recIntSecs();
            }

            // hr time in zone
            if (series == RideFile::hr && hrZoneRange != -1) {
                int index = context->athlete->hrZones(ride->sport())->whichZone(hrZoneRange, dp->value(series));
                if (index >= 0) hrTimeInZone[index] += ride->recIntSecs();
            }

            // Polarized zones :- I(<AeTHR), II (<LTHR and >0.9*LTHR), III (>LTHR)
            if (series == RideFile::hr && hrZoneRange != -1 && LTHR) {
                if (dp->value(series) < 1) // I zero
                    hrCPTimeInZone[0] += ride->recIntSecs();
                else if (dp->value(series) < AeTHR) // I
                    hrCPTimeInZone[1] += ride->recIntSecs();
                else if (dp->value(series) < LTHR) // II
                    hrCPTimeInZone[2] += ride->recIntSecs();
                else // III
                    hrCPTimeInZone[3] += ride->recIntSecs();
            }

            // pace time in zone, only for running and swimming activities
            if (series == RideFile::kph && paceZoneRange != -1 && (ride->isRun() || ride->isSwim())) {
                int index = context->athlete->paceZones(ride->isSwim())->whichZone(paceZoneRange, dp->value(series));
                if (index >= 0) paceTimeInZone[index] += ride->recIntSecs();
            }

            // Polarized Pace Zones: I(<AeTV), II (>=AeTV and <CV), III (>=CV)
            if (series == RideFile::kph && paceZoneRange != -1 && CV && (ride->isRun() || ride->isSwim())) {
                if (dp->value(series) < 0.1) // I zero
                    paceCPTimeInZone[0] += ride->recIntSecs();
                else if (dp->value(series) < AeTV) // I
                    paceCPTimeInZone[1] += ride->recIntSecs();
                else if (dp->value(series) < CV) // II
                    paceCPTimeInZone[2] += ride->recIntSecs();
                else // III
                    paceCPTimeInZone[3] += ride->recIntSecs();
            }

            int offset = lvalue - min;
            if (offset >= 0 && offset < array.size()) array[offset] += ride->recIntSecs();
        }
    }
}

//
// AGGREGATE FOR A GIVEN DATE RANGE
//

// select and update bests
static void meanMaxAggregate(QVector<double> &into, QVector<double> &other, QVector<QDate>&dates, QDate rideDate)
{
    if (into.size() < other.size()) {
        into.resize(other.size());
        dates.resize(other.size());
    }

    for (int i=0; i<other.size(); i++)
        if (other[i] > into[i]) {
            into[i] = other[i];
            dates[i] = rideDate;
        }
}

// resize into and then sum the arrays
static void distAggregate(QVector<double> &into, QVector<double> &other)
{
    if (into.size() < other.size()) into.resize(other.size());
    for (int i=0; i<other.size(); i++) into[i] += other[i];

}

RideFileCache::RideFileCache(Context *context, QDate start, QDate end, bool filter, QStringList files, bool onhome, RideItem *rideItem)
               : start(start), end(end), crc(0), incomplete(false),
                 context(context), rideFileName(""), ride(0),
                 CP(0), WPRIME(0), LTHR(0), CV(0.0), WEIGHT(0.0),
                 filter(filter), onhome(onhome)
{

    // remember parameters for getting heat
    this->filter = filter;
    this->files = files;
    this->onhome = onhome;

    // Oh lets get from the cache if we can -- but not if filtered
    if (!filter && !context->isfiltered && !rideItem) {

        // oh and not if we're onhome and homefiltered
        if ((onhome && !context->ishomefiltered) || !onhome) {
            for (int index = 0;
                 index
                     < context->athlete
                           ->cpxCache.count();) {
                RideFileCache *p =
                    context->athlete
                        ->cpxCache.at(index);
                if (p->start == start && p->end == end) {
                    if (p->aggregateSourceBindingsComplete_
                        && sourceBindingsAreCurrent(
                            p->aggregateSourceBindings_)) {
                        *this = *p;
                        return;
                    }
                    delete p;
                    context->athlete
                        ->cpxCache.removeAt(index);
                    continue;
                }
                ++index;
            }
        }
    }

    // resize all the arrays to zero - expand as neccessary
    xPowerMeanMax.resize(0);
    npMeanMax.resize(0);
    wattsMeanMax.resize(0);
    heatMeanMax.resize(0);
    hrMeanMax.resize(0);
    cadMeanMax.resize(0);
    nmMeanMax.resize(0);
    kphMeanMax.resize(0);
    kphdMeanMax.resize(0);
    wattsdMeanMax.resize(0);
    caddMeanMax.resize(0);
    nmdMeanMax.resize(0);
    hrdMeanMax.resize(0);
    xPowerMeanMax.resize(0);
    npMeanMax.resize(0);
    vamMeanMax.resize(0);
    wattsKgMeanMax.resize(0);
    aPowerMeanMax.resize(0);
    aPowerKgMeanMax.resize(0);
    wattsDistribution.resize(0);
    hrDistribution.resize(0);
    cadDistribution.resize(0);
    nmDistribution.resize(0);
    kphDistribution.resize(0);
    xPowerDistribution.resize(0);
    npDistribution.resize(0);
    wattsKgDistribution.resize(0);
    aPowerDistribution.resize(0);
    smo2Distribution.resize(0);
    wbalDistribution.resize(0);

    // time in zone are fixed to 10 zone max
    wattsTimeInZone.resize(10);
    wattsCPTimeInZone.resize(4);
    hrTimeInZone.resize(10);
    hrCPTimeInZone.resize(4);
    paceTimeInZone.resize(10);
    paceCPTimeInZone.resize(4);
    wbalTimeInZone.resize(4);

    // set cursor busy whilst we aggregate -- bit of feedback
    // and less intrusive than a popup box
    context->mainWindow->setCursor(Qt::WaitCursor);

    // Iterate over the ride files (not the cpx files since they /might/ not
    // exist, or /might/ be out of date.
    foreach (RideItem *item, context->athlete->rideCache->rides()) {

        QDate rideDate = item->dateTime.date();

        if (((filter == true && files.contains(item->fileName)) || filter == false) &&
            rideDate >= start && rideDate <= end) {

            // skip globally filtered values
            if (context->isfiltered && !context->filters.contains(item->fileName)) continue;
            if (onhome && context->ishomefiltered && !context->homeFilters.contains(item->fileName)) continue;
            // skip other sports if rideItem is given
            if (rideItem && (rideItem->sport != item->sport)) continue;

            // get its cached values (will NOT! refresh if needed...)
            // the true means it will check only
            const QString sourcePath =
                RideFileCacheIntegrity::
                    activitySourcePath(
                        item->path,
                        item->fileName);
            RideFileCache rideCache(
                context,
                sourcePath,
                item->getWeight(),
                NULL,
                false,
                false);
            if (rideCache.incomplete == true) {
                // ack, data not available !
                incomplete = true;
            } else if (!rideCache
                            .sourceFingerprint_
                            .isValid()) {
                incomplete = true;
            } else {
                aggregateSourceBindings_.append({
                    sourcePath,
                    rideCache.sourceFingerprint_
                });

                // lets aggregate
                meanMaxAggregate(wattsMeanMaxDouble, rideCache.wattsMeanMaxDouble, wattsMeanMaxDate, rideDate);
                meanMaxAggregate(hrMeanMaxDouble, rideCache.hrMeanMaxDouble, hrMeanMaxDate, rideDate);
                meanMaxAggregate(cadMeanMaxDouble, rideCache.cadMeanMaxDouble, cadMeanMaxDate, rideDate);
                meanMaxAggregate(nmMeanMaxDouble, rideCache.nmMeanMaxDouble, nmMeanMaxDate, rideDate);
                meanMaxAggregate(kphMeanMaxDouble, rideCache.kphMeanMaxDouble, kphMeanMaxDate, rideDate);
                meanMaxAggregate(kphdMeanMaxDouble, rideCache.kphdMeanMaxDouble, kphdMeanMaxDate, rideDate);
                meanMaxAggregate(wattsdMeanMaxDouble, rideCache.wattsdMeanMaxDouble, wattsdMeanMaxDate, rideDate);
                meanMaxAggregate(caddMeanMaxDouble, rideCache.caddMeanMaxDouble, caddMeanMaxDate, rideDate);
                meanMaxAggregate(nmdMeanMaxDouble, rideCache.nmdMeanMaxDouble, nmdMeanMaxDate, rideDate);
                meanMaxAggregate(hrdMeanMaxDouble, rideCache.hrdMeanMaxDouble, hrdMeanMaxDate, rideDate);
                meanMaxAggregate(xPowerMeanMaxDouble, rideCache.xPowerMeanMaxDouble, xPowerMeanMaxDate, rideDate);
                meanMaxAggregate(npMeanMaxDouble, rideCache.npMeanMaxDouble, npMeanMaxDate, rideDate);
                meanMaxAggregate(vamMeanMaxDouble, rideCache.vamMeanMaxDouble, vamMeanMaxDate, rideDate);
                meanMaxAggregate(wattsKgMeanMaxDouble, rideCache.wattsKgMeanMaxDouble, wattsKgMeanMaxDate, rideDate);
                meanMaxAggregate(aPowerMeanMaxDouble, rideCache.aPowerMeanMaxDouble, aPowerMeanMaxDate, rideDate);
                meanMaxAggregate(aPowerKgMeanMaxDouble, rideCache.aPowerKgMeanMaxDouble, aPowerKgMeanMaxDate, rideDate);

                distAggregate(wattsDistributionDouble, rideCache.wattsDistributionDouble);
                distAggregate(hrDistributionDouble, rideCache.hrDistributionDouble);
                distAggregate(cadDistributionDouble, rideCache.cadDistributionDouble);
                distAggregate(gearDistributionDouble, rideCache.gearDistributionDouble);
                distAggregate(nmDistributionDouble, rideCache.nmDistributionDouble);
                distAggregate(kphDistributionDouble, rideCache.kphDistributionDouble);
                distAggregate(xPowerDistributionDouble, rideCache.xPowerDistributionDouble);
                distAggregate(npDistributionDouble, rideCache.npDistributionDouble);
                distAggregate(wattsKgDistributionDouble, rideCache.wattsKgDistributionDouble);
                distAggregate(aPowerDistributionDouble, rideCache.aPowerDistributionDouble);
                distAggregate(smo2DistributionDouble, rideCache.smo2DistributionDouble);
                distAggregate(wbalDistributionDouble, rideCache.wbalDistributionDouble);

                // cumulate timeinzones
                for (int i=0; i<10; i++) {
                    paceTimeInZone[i] += rideCache.paceTimeInZone[i];
                    hrTimeInZone[i] += rideCache.hrTimeInZone[i];
                    wattsTimeInZone[i] += rideCache.wattsTimeInZone[i];
                    if (i<4) {
                        paceCPTimeInZone[i] += rideCache.paceCPTimeInZone[i];
                        hrCPTimeInZone[i] += rideCache.hrCPTimeInZone[i];
                        wattsCPTimeInZone[i] += rideCache.wattsCPTimeInZone[i];
                        wbalTimeInZone[i] += rideCache.wbalTimeInZone[i];
                    }
                }
            }
        }
    }

    // set the cursor back to normal
    context->mainWindow->setCursor(Qt::ArrowCursor);
    aggregateSourceBindingsComplete_ =
        !incomplete;

    // lets add to the cache for others to re-use -- but not if filtered or incomplete
    if (incomplete == false && !context->isfiltered && (!context->ishomefiltered || !onhome) && !filter) {

        if (context->athlete->cpxCache.count() > maxcache) {
            delete(context->athlete->cpxCache.at(0));
            context->athlete->cpxCache.removeAt(0);
        }
        context->athlete->cpxCache.append(new RideFileCache(this));
    }
}

//
// Get heat mean max -- if an aggregated curve
//
QVector<float> &RideFileCache::heatMeanMaxArray()
{
    // not aggregated or already done it return the result
    if (ride || heatMeanMax.count()) return heatMeanMax;

    // make it big enough
    heatMeanMax.resize(wattsMeanMaxDouble.size());

    // ok, we need to iterate again and compute heat based upon
    // how close to the absolute best we've got
    foreach(RideItem *item, context->athlete->rideCache->rides()) {

        QDate rideDate = item->dateTime.date();

        if (((filter == true && files.contains(item->fileName)) || filter == false) &&
            rideDate >= start && rideDate <= end) {

            // skip globally filtered values
            if (context->isfiltered && !context->filters.contains(item->fileName)) continue;
            if (onhome && context->ishomefiltered && !context->homeFilters.contains(item->fileName)) continue;

            // get its cached values (will refresh if needed...)
            RideFileCache rideCache(
                context,
                RideFileCacheIntegrity::activitySourcePath(
                    item->path, item->fileName),
                item->getWeight());

            for(int i=0; i<rideCache.wattsMeanMaxDouble.count() && i<wattsMeanMaxDouble.count(); i++) {

                // is it within 10% of the best we have ?
                if (rideCache.wattsMeanMaxDouble[i] >= (0.9f * wattsMeanMaxDouble[i]))
                    heatMeanMax[i] = heatMeanMax[i] + 1;
            }
        }
    }

    return heatMeanMax;
}

//
// PERSISTANCE
//
bool
RideFileCache::serialize(
    QDataStream *out,
    const RideFileCRC::ContentFingerprint
        &sourceFingerprint)
{
    if (!out
        || !out->device()
        || !sourceFingerprint.isValid()
        || crc
            != sourceFingerprint.legacyCrc16) {
        return false;
    }

    RideFileCacheHeader head {};

    // write header
    head.version = RideFileCacheVersion;
    head.crc = crc;
    head.CP = CP;
    head.WPRIME = WPRIME;
    head.LTHR = LTHR;
    head.CV = CV;
    head.WEIGHT = WEIGHT;

    const auto assignCount =
        [](qsizetype count,
           unsigned int &destination) {
            if (count < 0
                || static_cast<quint64>(count)
                    > std::numeric_limits<
                          unsigned int>::max()) {
                return false;
            }
            destination =
                static_cast<unsigned int>(count);
            return true;
        };
    if (!assignCount(
            wattsMeanMax.size(),
            head.wattsMeanMaxCount)
        || !assignCount(
            hrMeanMax.size(),
            head.hrMeanMaxCount)
        || !assignCount(
            cadMeanMax.size(),
            head.cadMeanMaxCount)
        || !assignCount(
            nmMeanMax.size(),
            head.nmMeanMaxCount)
        || !assignCount(
            kphMeanMax.size(),
            head.kphMeanMaxCount)
        || !assignCount(
            kphdMeanMax.size(),
            head.kphdMeanMaxCount)
        || !assignCount(
            wattsdMeanMax.size(),
            head.wattsdMeanMaxCount)
        || !assignCount(
            caddMeanMax.size(),
            head.caddMeanMaxCount)
        || !assignCount(
            nmdMeanMax.size(),
            head.nmdMeanMaxCount)
        || !assignCount(
            hrdMeanMax.size(),
            head.hrdMeanMaxCount)
        || !assignCount(
            xPowerMeanMax.size(),
            head.xPowerMeanMaxCount)
        || !assignCount(
            npMeanMax.size(),
            head.npMeanMaxCount)
        || !assignCount(
            vamMeanMax.size(),
            head.vamMeanMaxCount)
        || !assignCount(
            wattsKgMeanMax.size(),
            head.wattsKgMeanMaxCount)
        || !assignCount(
            aPowerMeanMax.size(),
            head.aPowerMeanMaxCount)
        || !assignCount(
            aPowerKgMeanMax.size(),
            head.aPowerKgMeanMaxCount)
        || !assignCount(
            wattsDistribution.size(),
            head.wattsDistCount)
        || !assignCount(
            hrDistribution.size(),
            head.hrDistCount)
        || !assignCount(
            cadDistribution.size(),
            head.cadDistCount)
        || !assignCount(
            gearDistribution.size(),
            head.gearDistCount)
        || !assignCount(
            nmDistribution.size(),
            head.nmDistrCount)
        || !assignCount(
            kphDistribution.size(),
            head.kphDistCount)
        || !assignCount(
            xPowerDistribution.size(),
            head.xPowerDistCount)
        || !assignCount(
            npDistribution.size(),
            head.npDistCount)
        || !assignCount(
            wattsKgDistribution.size(),
            head.wattsKgDistCount)
        || !assignCount(
            aPowerDistribution.size(),
            head.aPowerDistCount)
        || !assignCount(
            smo2Distribution.size(),
            head.smo2DistCount)
        || !assignCount(
            wbalDistribution.size(),
            head.wbalDistCount)
        || wattsTimeInZone.size() != 10
        || wattsCPTimeInZone.size() != 4
        || hrTimeInZone.size() != 10
        || hrCPTimeInZone.size() != 4
        || paceTimeInZone.size() != 10
        || paceCPTimeInZone.size() != 4
        || wbalTimeInZone.size() != 4
        || !RideFileCacheIntegrity::
                validateCacheLayout(head)) {
        return false;
    }

    bool writeSucceeded = true;
    QCryptographicHash cacheHash(
        QCryptographicHash::Sha256);
    const auto writeRaw =
        [&](const void *data, qsizetype bytes) {
            if (!writeSucceeded)
                return;
            if (bytes < 0
                || bytes
                    > std::numeric_limits<int>::max()
                || (bytes > 0 && !data)) {
                writeSucceeded = false;
                return;
            }
            const auto *raw =
                static_cast<const char *>(data);
            if (out->writeRawData(
                    raw,
                    static_cast<int>(bytes))
                != bytes) {
                writeSucceeded = false;
                return;
            }
            cacheHash.addData(
                QByteArrayView(raw, bytes));
        };

    writeRaw(&head, sizeof(head));
    const qint64 sourceByteSize =
        sourceFingerprint.byteSize;
    writeRaw(
        &sourceByteSize,
        sizeof(sourceByteSize));
    writeRaw(
        sourceFingerprint.sha256.constData(),
        sourceFingerprint.sha256.size());

    // write meanmax
    writeRaw(wattsMeanMax.data(), sizeof(float) * wattsMeanMax.size());
    writeRaw(wattsKgMeanMax.data(), sizeof(float) * wattsKgMeanMax.size());
    writeRaw(hrMeanMax.data(), sizeof(float) * hrMeanMax.size());
    writeRaw(cadMeanMax.data(), sizeof(float) * cadMeanMax.size());
    writeRaw(nmMeanMax.data(), sizeof(float) * nmMeanMax.size());
    writeRaw(kphMeanMax.data(), sizeof(float) * kphMeanMax.size());
    writeRaw(kphdMeanMax.data(), sizeof(float) * kphdMeanMax.size());
    writeRaw(wattsdMeanMax.data(), sizeof(float) * wattsdMeanMax.size());
    writeRaw(caddMeanMax.data(), sizeof(float) * caddMeanMax.size());
    writeRaw(nmdMeanMax.data(), sizeof(float) * nmdMeanMax.size());
    writeRaw(hrdMeanMax.data(), sizeof(float) * hrdMeanMax.size());
    writeRaw(xPowerMeanMax.data(), sizeof(float) * xPowerMeanMax.size());
    writeRaw(npMeanMax.data(), sizeof(float) * npMeanMax.size());
    writeRaw(vamMeanMax.data(), sizeof(float) * vamMeanMax.size());
    writeRaw(aPowerMeanMax.data(), sizeof(float) * aPowerMeanMax.size());
    writeRaw(aPowerKgMeanMax.data(), sizeof(float) * aPowerKgMeanMax.size());


    // write dist
    writeRaw(wattsDistribution.data(), sizeof(float) * wattsDistribution.size());
    writeRaw(hrDistribution.data(), sizeof(float) * hrDistribution.size());
    writeRaw(cadDistribution.data(), sizeof(float) * cadDistribution.size());
    writeRaw(gearDistribution.data(), sizeof(float) * gearDistribution.size());
    writeRaw(nmDistribution.data(), sizeof(float) * nmDistribution.size());
    writeRaw(kphDistribution.data(), sizeof(float) * kphDistribution.size());
    writeRaw(xPowerDistribution.data(), sizeof(float) * xPowerDistribution.size());
    writeRaw(npDistribution.data(), sizeof(float) * npDistribution.size());
    writeRaw(wattsKgDistribution.data(), sizeof(float) * wattsKgDistribution.size());
    writeRaw(aPowerDistribution.data(), sizeof(float) * aPowerDistribution.size());
    writeRaw(smo2Distribution.data(), sizeof(float) * smo2Distribution.size());
    writeRaw(wbalDistribution.data(), sizeof(float) * wbalDistribution.size());

    // time in zone
    writeRaw(wattsTimeInZone.data(), sizeof(float) * wattsTimeInZone.size());
    writeRaw(wattsCPTimeInZone.data(), sizeof(float) * wattsCPTimeInZone.size());
    writeRaw(hrTimeInZone.data(), sizeof(float) * hrTimeInZone.size());
    writeRaw(hrCPTimeInZone.data(), sizeof(float) * hrCPTimeInZone.size());
    writeRaw(paceTimeInZone.data(), sizeof(float) * paceTimeInZone.size());
    writeRaw(paceCPTimeInZone.data(), sizeof(float) * paceCPTimeInZone.size());
    writeRaw(wbalTimeInZone.data(), sizeof(float) * wbalTimeInZone.size());

    const QByteArray cacheDigest =
        cacheHash.result();
    if (writeSucceeded
        && out->writeRawData(
               cacheDigest.constData(),
               cacheDigest.size())
            != cacheDigest.size()) {
        writeSucceeded = false;
    }

    return writeSucceeded
        && out->status() == QDataStream::Ok;
}

void
RideFileCache::clearPublishedArrays()
{
    sourceFingerprint_ =
        RideFileCRC::ContentFingerprint {};
    aggregateSourceBindings_.clear();
    aggregateSourceBindingsComplete_ = false;

    wattsMeanMax.clear();
    heatMeanMax.clear();
    hrMeanMax.clear();
    cadMeanMax.clear();
    nmMeanMax.clear();
    kphMeanMax.clear();
    kphdMeanMax.clear();
    wattsdMeanMax.clear();
    caddMeanMax.clear();
    nmdMeanMax.clear();
    hrdMeanMax.clear();
    xPowerMeanMax.clear();
    npMeanMax.clear();
    vamMeanMax.clear();
    wattsKgMeanMax.clear();
    aPowerMeanMax.clear();
    aPowerKgMeanMax.clear();

    wattsDistribution.clear();
    hrDistribution.clear();
    cadDistribution.clear();
    gearDistribution.clear();
    nmDistribution.clear();
    kphDistribution.clear();
    kphdDistribution.clear();
    xPowerDistribution.clear();
    npDistribution.clear();
    wattsKgDistribution.clear();
    aPowerDistribution.clear();
    smo2Distribution.clear();
    wbalDistribution.clear();

    wattsTimeInZone.clear();
    wattsCPTimeInZone.clear();
    hrTimeInZone.clear();
    hrCPTimeInZone.clear();
    paceTimeInZone.clear();
    paceCPTimeInZone.clear();
    wbalTimeInZone.clear();

    wattsMeanMaxDouble.clear();
    hrMeanMaxDouble.clear();
    cadMeanMaxDouble.clear();
    nmMeanMaxDouble.clear();
    kphMeanMaxDouble.clear();
    kphdMeanMaxDouble.clear();
    wattsdMeanMaxDouble.clear();
    caddMeanMaxDouble.clear();
    nmdMeanMaxDouble.clear();
    hrdMeanMaxDouble.clear();
    xPowerMeanMaxDouble.clear();
    npMeanMaxDouble.clear();
    vamMeanMaxDouble.clear();
    wattsKgMeanMaxDouble.clear();
    aPowerMeanMaxDouble.clear();
    aPowerKgMeanMaxDouble.clear();

    wattsDistributionDouble.clear();
    hrDistributionDouble.clear();
    gearDistributionDouble.clear();
    cadDistributionDouble.clear();
    nmDistributionDouble.clear();
    kphDistributionDouble.clear();
    xPowerDistributionDouble.clear();
    npDistributionDouble.clear();
    wattsKgDistributionDouble.clear();
    aPowerDistributionDouble.clear();
    smo2DistributionDouble.clear();
    wbalDistributionDouble.clear();
}

bool
RideFileCache::readCache(double expectedWeight)
{
    clearPublishedArrays();
    incomplete = true;
    sourceFingerprint_ =
        RideFileCRC::ContentFingerprint {};

    RideFileCacheIntegrity::CacheData data;
    RideFileCRC::ContentFingerprint
        verifiedFingerprint;
    if (!withCurrentSource(
            rideFileName,
            &verifiedFingerprint,
            [&](RideFileCRC::
                    ContentFingerprint
                        &cacheFingerprint) {
                QFile cacheFile(cacheFileName);
                if (!cacheFile.open(
                        QIODevice::ReadOnly)
                    || !RideFileCacheIntegrity::
                           readCache(
                               cacheFile,
                               data)
                    || data.header.WEIGHT
                        != expectedWeight) {
                    return false;
                }
                cacheFingerprint =
                    data.sourceFingerprint;
                return true;
            })) {
        return false;
    }

    crc = data.header.crc;
    LTHR = data.header.LTHR;
    CP = data.header.CP;
    CV = data.header.CV;
    WEIGHT = data.header.WEIGHT;
    WPRIME = data.header.WPRIME;
    sourceFingerprint_ =
        std::move(verifiedFingerprint);

    wattsMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::WattsMeanMax]);
    wattsKgMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::WattsKgMeanMax]);
    hrMeanMax = std::move(data.blocks[RideFileCacheIntegrity::HrMeanMax]);
    cadMeanMax = std::move(data.blocks[RideFileCacheIntegrity::CadMeanMax]);
    nmMeanMax = std::move(data.blocks[RideFileCacheIntegrity::NmMeanMax]);
    kphMeanMax = std::move(data.blocks[RideFileCacheIntegrity::KphMeanMax]);
    kphdMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::KphdMeanMax]);
    wattsdMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::WattsdMeanMax]);
    caddMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::CaddMeanMax]);
    nmdMeanMax = std::move(data.blocks[RideFileCacheIntegrity::NmdMeanMax]);
    hrdMeanMax = std::move(data.blocks[RideFileCacheIntegrity::HrdMeanMax]);
    xPowerMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::XPowerMeanMax]);
    npMeanMax = std::move(data.blocks[RideFileCacheIntegrity::NpMeanMax]);
    vamMeanMax = std::move(data.blocks[RideFileCacheIntegrity::VamMeanMax]);
    aPowerMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::APowerMeanMax]);
    aPowerKgMeanMax = std::move(
        data.blocks[RideFileCacheIntegrity::APowerKgMeanMax]);

    wattsDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::WattsDistribution]);
    hrDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::HrDistribution]);
    cadDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::CadDistribution]);
    gearDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::GearDistribution]);
    nmDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::NmDistribution]);
    kphDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::KphDistribution]);
    xPowerDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::XPowerDistribution]);
    npDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::NpDistribution]);
    wattsKgDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::WattsKgDistribution]);
    aPowerDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::APowerDistribution]);
    smo2Distribution = std::move(
        data.blocks[RideFileCacheIntegrity::Smo2Distribution]);
    wbalDistribution = std::move(
        data.blocks[RideFileCacheIntegrity::WbalDistribution]);

    wattsTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::WattsTimeInZone]);
    wattsCPTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::WattsCPTimeInZone]);
    hrTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::HrTimeInZone]);
    hrCPTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::HrCPTimeInZone]);
    paceTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::PaceTimeInZone]);
    paceCPTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::PaceCPTimeInZone]);
    wbalTimeInZone = std::move(
        data.zones[RideFileCacheIntegrity::WbalTimeInZone]);

    doubleArray(wattsMeanMaxDouble, wattsMeanMax, RideFile::watts);
    doubleArray(hrMeanMaxDouble, hrMeanMax, RideFile::hr);
    doubleArray(cadMeanMaxDouble, cadMeanMax, RideFile::cad);
    doubleArray(nmMeanMaxDouble, nmMeanMax, RideFile::nm);
    doubleArray(kphMeanMaxDouble, kphMeanMax, RideFile::kph);
    doubleArray(kphdMeanMaxDouble, kphdMeanMax, RideFile::kphd);
    doubleArray(wattsdMeanMaxDouble, wattsdMeanMax, RideFile::wattsd);
    doubleArray(caddMeanMaxDouble, caddMeanMax, RideFile::cadd);
    doubleArray(nmdMeanMaxDouble, nmdMeanMax, RideFile::nmd);
    doubleArray(hrdMeanMaxDouble, hrdMeanMax, RideFile::hrd);
    doubleArray(npMeanMaxDouble, npMeanMax, RideFile::IsoPower);
    doubleArray(vamMeanMaxDouble, vamMeanMax, RideFile::vam);
    doubleArray(xPowerMeanMaxDouble, xPowerMeanMax, RideFile::xPower);
    doubleArray(wattsKgMeanMaxDouble, wattsKgMeanMax, RideFile::wattsKg);
    doubleArray(aPowerMeanMaxDouble, aPowerMeanMax, RideFile::aPower);
    doubleArray(aPowerKgMeanMaxDouble, aPowerKgMeanMax, RideFile::aPowerKg);

    doubleArrayForDistribution(wattsDistributionDouble, wattsDistribution);
    doubleArrayForDistribution(hrDistributionDouble, hrDistribution);
    doubleArrayForDistribution(cadDistributionDouble, cadDistribution);
    doubleArrayForDistribution(gearDistributionDouble, gearDistribution);
    doubleArrayForDistribution(nmDistributionDouble, nmDistribution);
    doubleArrayForDistribution(kphDistributionDouble, kphDistribution);
    doubleArrayForDistribution(
        xPowerDistributionDouble, xPowerDistribution);
    doubleArrayForDistribution(npDistributionDouble, npDistribution);
    doubleArrayForDistribution(
        wattsKgDistributionDouble, wattsKgDistribution);
    doubleArrayForDistribution(
        aPowerDistributionDouble, aPowerDistribution);
    doubleArrayForDistribution(smo2DistributionDouble, smo2Distribution);
    doubleArrayForDistribution(wbalDistributionDouble, wbalDistribution);

    incomplete = false;
    return true;
}

// unpack the longs into a double array
void RideFileCache::doubleArray(QVector<double> &into, QVector<float> &from, RideFile::SeriesType series)
{
    double divisor = pow(10, decimalsFor(series)); // ? 10 : 1;
    into.resize(from.size());
    for(int i=0; i<from.size(); i++) into[i] = double(from[i]) / divisor;

    return;
}

// for Distribution Series the values in Long/Float are ALWAYS Seconds (therefore no decimals adjustment calculation required)
void RideFileCache::doubleArrayForDistribution(QVector<double> &into, QVector<float> &from)
{
    into.resize(from.size());
    for(int i=0; i<from.size(); i++) into[i] = double(from[i]);

    return;
}

static bool bestForCache(
    const QString &sourceFileName,
    const QString &cacheFileName,
    RideFile::SeriesType series,
    int duration,
    const double *expectedWeight,
    double &result);

// get a list of all values for the spec, series and duration and see where the value ranks
int RideFileCache::rank(Context *context, RideFile::SeriesType series, int duration, 
         double value, Specification spec, int &of)
{

    QList<double> values;

    foreach(RideItem*item, context->athlete->rideCache->rides()) {
        if (!spec.pass(item)) continue;

        double bestValue = 0;
        const double expectedWeight =
            item->getWeight();
        if (bestForCache(
                sourcePathForRideItem(
                    context, item),
                cachePathForRideItem(
                    context, item),
                series,
                duration,
                &expectedWeight,
                bestValue)) {
            values << bestValue;
        }
    }

    // sort the list
    std::sort(values.begin(), values.end());

    // get the ranking and count
    of = values.count();

    // where do we fit?
    for (int i=0; i<values.count(); i++)
        if (values.at(i) <= value)
            return (i+1);

    return values.count();
}

static bool bestForCache(
    const QString &sourceFileName,
    const QString &cacheFileName,
    RideFile::SeriesType series,
    int duration,
    const double *expectedWeight,
    double &result)
{
    result = 0;
    RideFileCacheIntegrity::Block block;
    if (!meanMaxBlockForSeries(series, block)
        || duration < 0) {
        return false;
    }

    float loaded = 0.0f;
    if (!readSourceBoundPartialCache(
            sourceFileName,
            cacheFileName,
            expectedWeight,
            nullptr,
            [&](RideFileCacheIntegrity::
                    PartialReader &reader) {
                return reader.readBlockValue(
                    block, duration, loaded);
            })) {
        return false;
    }
    const double divisor =
        pow(10, RideFileCache::decimalsFor(series));
    result = loaded / divisor;
    return true;
}

static bool bestForActivity(
    const QString &cacheRoot,
    const QString &completedRoot,
    const QString &plannedRoot,
    const QString &sourceActivityPath,
    RideFile::SeriesType series,
    int duration,
    const double *expectedWeight,
    double &result)
{
    return bestForCache(
        sourceActivityPath,
        RideFileCacheIntegrity::cachePathForActivity(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourceActivityPath),
        series,
        duration,
        expectedWeight,
        result);
}

static bool queueBestRow(
    RideFileCacheIntegrity::PartialReader &reader,
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    QVector<float> &rawValues,
    QVector<bool> &requestedValues)
{
    rawValues.fill(
        0.0f, requests.size());
    requestedValues.fill(
        false, requests.size());
    if (!reader.isValid())
        return false;
    for (qsizetype index = 0;
         index < requests.size();
         ++index) {
        const auto &request =
            requests.at(index);
        RideFileCacheIntegrity::Block block;
        if (request.second >= 0
            && meanMaxBlockForSeries(
                request.first, block)
            && reader.readBlockValue(
                block,
                request.second,
                rawValues[index])) {
            requestedValues[index] = true;
        } else if (!reader.isValid()) {
            return false;
        }
    }
    return true;
}

static void convertBestRow(
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    const QVector<float> &rawValues,
    const QVector<bool> &requestedValues,
    QVector<double> &values)
{
    values.resize(requests.size());
    for (qsizetype index = 0;
         index < requests.size();
         ++index) {
        values[index] =
            requestedValues.at(index)
            ? rawValues.at(index)
                / pow(
                    10,
                    RideFileCache::decimalsFor(
                        requests.at(index)
                            .first))
            : 0.0;
    }
}

static bool readBestRowForSource(
    const QString &sourceFileName,
    const QString &cacheFileName,
    const double *expectedWeight,
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    QVector<double> &values)
{
    values.clear();
    QVector<float> rawValues;
    QVector<bool> requestedValues;
    if (!readSourceBoundPartialCache(
            sourceFileName,
            cacheFileName,
            expectedWeight,
            nullptr,
            [&](RideFileCacheIntegrity::
                    PartialReader &reader) {
                return queueBestRow(
                    reader,
                    requests,
                    rawValues,
                    requestedValues);
            })) {
        return false;
    }

    convertBestRow(
        requests,
        rawValues,
        requestedValues,
        values);
    return true;
}

#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
static bool readBestRow(
    QIODevice &input,
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    QVector<double> &values)
{
    RideFileCacheIntegrity::PartialReader reader(input);
    QVector<float> rawValues;
    QVector<bool> requestedValues;
    if (!queueBestRow(
            reader,
            requests,
            rawValues,
            requestedValues)
        || !reader.finish()) {
        values.clear();
        return false;
    }
    convertBestRow(
        requests,
        rawValues,
        requestedValues,
        values);
    return true;
}
#endif

double
RideFileCache::best(
    Context *context,
    QString filename,
    RideFile::SeriesType series,
    int duration)
{
    const QString sourcePath =
        RideFileCacheIntegrity::activitySourcePath(
            context->athlete->home
                ->activities()
                .absolutePath(),
            filename);
    double result = 0;
    bestForActivity(
        context->athlete->home
            ->cache()
            .absolutePath(),
        context->athlete->home
            ->activities()
            .absolutePath(),
        context->athlete->home
            ->planned()
            .absolutePath(),
        sourcePath,
        series,
        duration,
        nullptr,
        result);
    return result;
}

double
RideFileCache::best(
    Context *context,
    RideItem *item,
    RideFile::SeriesType series,
    int duration)
{
    const QString sourcePath =
        sourcePathForRideItem(context, item);
    const double expectedWeight =
        item ? item->getWeight() : 0.0;
    double result = 0;
    bestForCache(
        sourcePath,
        cachePathForRideItem(context, item),
        series,
        duration,
        item ? &expectedWeight : nullptr,
        result);
    return result;
}

double
RideFileCache::best(
    Context *context,
    const RideItem *item,
    RideFile::SeriesType series,
    int duration)
{
    double result = 0;
    bestForCache(
        sourcePathForRideItem(context, item),
        cachePathForRideItem(context, item),
        series,
        duration,
        nullptr,
        result);
    return result;
}

static bool tizForCache(
    const QString &sourceFileName,
    const QString &cacheFileName,
    RideFile::SeriesType series,
    int zone,
    const double *expectedWeight,
    int &result)
{
    result = 0;
    if (zone < 1 || zone > 10)
        return false;

    RideFileCacheIntegrity::ZoneBlock block;
    if (!zoneBlockForSeries(series, block))
        return false;

    float loaded = 0.0f;
    if (!readSourceBoundPartialCache(
            sourceFileName,
            cacheFileName,
            expectedWeight,
            nullptr,
            [&](RideFileCacheIntegrity::
                    PartialReader &reader) {
                return reader.readZoneValue(
                    block, zone - 1, loaded);
            })) {
        return false;
    }
    result = loaded;
    return true;
}

static bool tizForActivity(
    const QString &cacheRoot,
    const QString &completedRoot,
    const QString &plannedRoot,
    const QString &sourceActivityPath,
    RideFile::SeriesType series,
    int zone,
    const double *expectedWeight,
    int &result)
{
    return tizForCache(
        sourceActivityPath,
        RideFileCacheIntegrity::cachePathForActivity(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourceActivityPath),
        series,
        zone,
        expectedWeight,
        result);
}

int
RideFileCache::tiz(
    Context *context,
    QString filename,
    RideFile::SeriesType series,
    int zone)
{
    const QString sourcePath =
        RideFileCacheIntegrity::activitySourcePath(
            context->athlete->home
                ->activities()
                .absolutePath(),
            filename);
    int result = 0;
    tizForActivity(
        context->athlete->home
            ->cache()
            .absolutePath(),
        context->athlete->home
            ->activities()
            .absolutePath(),
        context->athlete->home
            ->planned()
            .absolutePath(),
        sourcePath,
        series,
        zone,
        nullptr,
        result);
    return result;
}

int
RideFileCache::tiz(
    Context *context,
    RideItem *item,
    RideFile::SeriesType series,
    int zone)
{
    const QString sourcePath =
        sourcePathForRideItem(context, item);
    const double expectedWeight =
        item ? item->getWeight() : 0.0;
    int result = 0;
    tizForCache(
        sourcePath,
        cachePathForRideItem(context, item),
        series,
        zone,
        item ? &expectedWeight : nullptr,
        result);
    return result;
}

int
RideFileCache::tiz(
    Context *context,
    const RideItem *item,
    RideFile::SeriesType series,
    int zone)
{
    int result = 0;
    tizForCache(
        sourcePathForRideItem(context, item),
        cachePathForRideItem(context, item),
        series,
        zone,
        nullptr,
        result);
    return result;
}

#ifdef GC_RIDE_FILE_CACHE_TEST_HOOKS
double
RideFileCache::bestForActivityForTest(
    const QString &cacheRoot,
    const QString &completedRoot,
    const QString &plannedRoot,
    const QString &sourceActivityPath,
    RideFile::SeriesType series,
    int duration)
{
    double result = 0;
    bestForActivity(
        cacheRoot,
        completedRoot,
        plannedRoot,
        sourceActivityPath,
        series,
        duration,
        nullptr,
        result);
    return result;
}

int
RideFileCache::tizForActivityForTest(
    const QString &cacheRoot,
    const QString &completedRoot,
    const QString &plannedRoot,
    const QString &sourceActivityPath,
    RideFile::SeriesType series,
    int zone)
{
    int result = 0;
    tizForActivity(
        cacheRoot,
        completedRoot,
        plannedRoot,
        sourceActivityPath,
        series,
        zone,
        nullptr,
        result);
    return result;
}

bool
RideFileCache::readBestRowForTest(
    QIODevice &input,
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    QVector<double> &values)
{
    return readBestRow(
        input, requests, values);
}

bool
RideFileCache::readBestRowForSourceForTest(
    const QString &sourcePath,
    const QString &cachePath,
    const QVector<
        QPair<RideFile::SeriesType, int>>
        &requests,
    QVector<double> &values)
{
    return readBestRowForSource(
        sourcePath,
        cachePath,
        nullptr,
        requests,
        values);
}
#endif

// get best values (as passed in the list of MetricDetails between the dates specified
// and return as an array of RideBests)
//
// this is to 're-use' the metric api (especially in the LTM code) for passing back multiple
// bests across multiple rides in one object. We do this so we can optimise the read/seek across
// the CPX files within a single call.
//
// We order the bests requested in the order they will appear in the CPX file so we can open
// and seek forward to each value before putting into the summary metric. Since it is placed
// on the stack as a return parameter we also don't need to worry about memory allocation just
// like the metric code works.
// 
//
QList<RideBest>
RideFileCache::getAllBestsFor(Context *context, QList<MetricDetail> metrics, Specification specification)
{
    QList<RideBest> results;
    QList<MetricDetail> worklist;

    // lets get a worklist
    foreach(MetricDetail x, metrics) {
        if (x.type == METRIC_BEST) {
            worklist << x;
        }
    }
    if (worklist.count() == 0) return results; // no work to do
    QVector<
        QPair<RideFile::SeriesType, int>>
        requests;
    requests.reserve(worklist.size());
    for (const MetricDetail &workitem : worklist) {
        const qint64 seconds =
            static_cast<qint64>(workitem.duration)
            * static_cast<qint64>(
                workitem.duration_units);
        requests.append({
            workitem.series,
            seconds >= 0
                    && seconds
                        <= std::numeric_limits<int>::max()
                ? static_cast<int>(seconds)
                : -1
        });
    }

    // get a list of rides & iterate over them
    foreach(RideItem *ride, context->athlete->rideCache->rides()) {

        if (!specification.pass(ride)) continue;

        // get the ride cache name

        // CPX ?
        const QString cacheFileName =
            cachePathForRideItem(context, ride);
        if (cacheFileName.isEmpty()) continue;
        const QString sourceFileName =
            sourcePathForRideItem(context, ride);
        const double expectedWeight =
            ride->getWeight();
        QVector<double> values;
        if (!readBestRowForSource(
                sourceFileName,
                cacheFileName,
                &expectedWeight,
                requests,
                values)) {
            continue;
        }

        RideBest add;
        add.setFileName(ride->fileName);
        add.setRideDate(ride->dateTime);

        // work through the worklist adding each best
        for (qsizetype index = 0;
             index < worklist.size();
             ++index) {
            add.setForSymbol(
                worklist.at(index).bestSymbol,
                values.at(index));
        }

        // add to the results
        results << add;
    }

    // all done, return results
    return results;
}

QVector<double>
RideFileCache::getAllBestsFor(Context *context, RideFile::SeriesType series, int duration, Specification specification)
{
    QDate earliest(1900,01,01);
    QVector<double> results;

    // get a list of rides & iterate over them
    foreach(RideItem *ride, context->athlete->rideCache->rides()) {

        if (!specification.pass(ride)) continue;

        // get the ride cache name

        // CPX ?
        const QString cacheFileName =
            cachePathForRideItem(context, ride);
        if (cacheFileName.isEmpty()) continue;
        const QString sourceFileName =
            sourcePathForRideItem(context, ride);
        const double expectedWeight =
            ride->getWeight();
        QVector<double> values;
        QVector<
            QPair<RideFile::SeriesType, int>>
            requests;
        if (series != RideFile::none)
            requests.append({series, duration});
        if (!readBestRowForSource(
                sourceFileName,
                cacheFileName,
                &expectedWeight,
                requests,
                values)) {
            continue;
        }

        if (series == RideFile::none) {
            double date= earliest.daysTo(ride->dateTime.date());
            results << date;
        } else {
            results << values.constFirst();
        }
    }

    // all done, return results
    return results;
}

static
const RideMetric *metricForSymbol(QString symbol)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    return factory.rideMetric(symbol);
}

double
RideBest::getForSymbol(QString symbol, bool metric) const
{
    if (metric) return value.value(symbol, 0.0);
    else {
        const RideMetric *m = metricForSymbol(symbol);
        double metricValue = value.value(symbol, 0.0);
        metricValue *= m->conversion();
        metricValue += m->conversionSum();
        return metricValue;
    }
}

int
RideFileCache::bestTime(double km)
{
    // divisor for series and conversion from secs to hours
    double divisor = pow(10, decimalsFor(RideFile::kph)) * 3600.0;
    // linear search over kph mean max array
    int secs = 0;
    while (secs < kphMeanMax.count() &&
           double(kphMeanMax[secs] * secs) / divisor < km) secs++;
    if (secs < kphMeanMax.count()) return secs;
    return RideFile::NIL;
}


double
RideFileCache::binsize(RideFile::SeriesType type)
{

    switch(type) {
    default:
    case RideFile::watts: return wattsDelta;
    case RideFile::wattsKg: return wattsKgDelta;
    case RideFile::nm: return nmDelta;
    case RideFile::hr: return hrDelta;
    case RideFile::kph: return kphDelta;
    case RideFile::cad: return cadDelta;
    case RideFile::gear: return gearDelta;
    case RideFile::smo2: return smo2Delta;
    case RideFile::wbal: return wbalDelta;
    }
}
