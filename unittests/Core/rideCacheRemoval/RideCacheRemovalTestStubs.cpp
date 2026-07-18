#include "LTMSettings.h"
#include "Athlete.h"
#include "Context.h"
#include "DataProcessor.h"
#include "Estimator.h"
#include "ErgFile.h"
#include "RideCache.h"
#include "RideCacheModel.h"
#include "RideItem.h"

#include <QDir>
#include <QFileInfo>

#include <cstring>
#include <utility>

GlobalContext::GlobalContext()
    : rideMetadata(nullptr),
      colorEngine(nullptr),
      useMetricUnits(true)
{
}

GlobalContext *GlobalContext::context()
{
    static GlobalContext instance;
    return &instance;
}

void GlobalContext::notifyConfigChanged(qint32) {}
void GlobalContext::readConfig(qint32) {}
void GlobalContext::userMetricsConfigChanged() {}

RealtimeData::RealtimeData()
    : mode(ErgFileFormat::unknown)
{
    std::memset(name, 0, sizeof(name));
    std::memset(spinScan, 0, sizeof(spinScan));
    hr = watts = altWatts = altDistance = speed = wheelRpm = load = slope =
        lrbalance = cadence = smo2 = thb = lte = rte = lps = rps = 0.0;
    rppb = rppe = rpppb = rpppe = lppb = lppe = lpppb = lpppe = 0.0;
    rightPCO = leftPCO = torque = RTorque = LTorque = 0.0;
    latitude = longitude = altitude = 0.0;
    vo2 = vco2 = rf = rmv = tv = feo2 = 0.0;
    position = RealtimeData::seated;
    temp = skinTemp = coreTemp = heatStrain = 0.0;
    wheelRpmSampleTime = {};
    distance = routeDistance = distanceRemaining = 0.0;
    lapDistance = lapDistanceRemaining = virtualSpeed = wbal = 0.0;
    hhb = o2hb = rer = 0.0;
    lap = msecs = lapMsecs = lapMsecsRemaining = ergMsecsRemaining = 0;
    trainerStatusAvailable = false;
    trainerReady = trainerRunning = true;
    trainerCalibRequired = trainerConfigRequired = trainerBrakeFault = false;
}

Context::Context(MainWindow *window)
    : mainWindow(window)
{
    nav = nullptr;
    viewIndex = 0;
    showSidebar = showLowbar = showToolbar = showTabbar = false;
    style = 0;
    scopehighlighted = false;
    rideNavigator = nullptr;
    tab = nullptr;
    athlete = nullptr;
    ride = nullptr;
    workout = nullptr;
    videosync = nullptr;
    now = 0;
    isfiltered = false;
    ishomefiltered = false;
    isRunning = false;
    isPaused = false;
    isCompareIntervals = false;
    isCompareDateRanges = false;
    webEngineProfile = nullptr;
    m_HtmlTrainingBridge = nullptr;
}

Context::~Context() = default;

DateRange::DateRange(QDate from, QDate to, QString name, QColor color)
    : from(from),
      to(to),
      name(std::move(name)),
      color(color),
      valid(from.isValid() && to.isValid())
{
}

DateRange::DateRange(const DateRange &other)
    : from(other.from),
      to(other.to),
      name(other.name),
      color(other.color),
      id(other.id),
      valid(from.isValid() && to.isValid())
{
}

DateRange &DateRange::operator=(const DateRange &other)
{
    from = other.from;
    to = other.to;
    name = other.name;
    color = other.color;
    id = other.id;
    valid = from.isValid() && to.isValid();
    return *this;
}

CompareInterval::~CompareInterval() = default;
CompareDateRange::~CompareDateRange() = default;

void Context::notifyCompareIntervals(bool state)
{
    isCompareIntervals = state;
    emit compareIntervalsStateChanged(state);
}

void Context::notifyCompareIntervalsChanged()
{
    if (isCompareIntervals) emit compareIntervalsChanged();
}

void Context::notifyCompareDateRanges(bool state)
{
    isCompareDateRanges = state;
    emit compareDateRangesStateChanged(state);
}

void Context::notifyCompareDateRangesChanged()
{
    if (isCompareDateRanges) emit compareDateRangesChanged();
}

void Context::notifyConfigChanged(qint32 state)
{
    emit configChanged(state);
}

AthleteDirectoryStructure::AthleteDirectoryStructure(const QDir home)
    : myhome(home),
      athlete_activities(QStringLiteral("activities")),
      athlete_tmp_activities(QStringLiteral("tempActivities")),
      athlete_imports(QStringLiteral("imports")),
      athlete_records(QStringLiteral("records")),
      athlete_downloads(QStringLiteral("downloads")),
      athlete_fileBackup(QStringLiteral("bak")),
      athlete_config(QStringLiteral("config")),
      athlete_cache(QStringLiteral("cache")),
      athlete_calendar(QStringLiteral("calendar")),
      athlete_workouts(QStringLiteral("workouts")),
      athlete_logs(QStringLiteral("logs")),
      athlete_temp(QStringLiteral("temp")),
      athlete_quarantine(QStringLiteral("quarantine")),
      athlete_planned(QStringLiteral("planned")),
      athlete_snippets(QStringLiteral("snippets")),
      athlete_media(QStringLiteral("media"))
{
}

AthleteDirectoryStructure::~AthleteDirectoryStructure() = default;

void AthleteDirectoryStructure::createAllSubdirs()
{
    const QStringList directories = {
        athlete_activities,
        athlete_tmp_activities,
        athlete_imports,
        athlete_records,
        athlete_downloads,
        athlete_fileBackup,
        athlete_config,
        athlete_cache,
        athlete_calendar,
        athlete_workouts,
        athlete_logs,
        athlete_temp,
        athlete_quarantine,
        athlete_planned,
        athlete_snippets,
        athlete_media
    };
    for (const QString &directory : directories) {
        myhome.mkpath(directory);
    }
}

Athlete::Athlete(Context *athleteContext, const QDir &homeDir)
    : context(athleteContext)
{
    home = new AthleteDirectoryStructure(homeDir);
    home->createAllSubdirs();
    if (context) context->athlete = this;
}

Athlete::~Athlete()
{
    rideCache = nullptr;
    delete home;
    home = nullptr;
}

void Athlete::checkCPX(RideItem *) {}
void Athlete::configChanged(qint32) {}
void Athlete::loadComplete() {}

RideItem::RideItem()
    : RideItem(nullptr, nullptr)
{
}

RideItem::RideItem(RideFile *ride, Context *rideContext)
    : ride_(ride),
      fileCache_(nullptr),
      context(rideContext),
      isdirty(false),
      isstale(true),
      isedit(false),
      skipsave(false),
      path(),
      fileName(),
      dateTime(),
      color(Qt::black),
      planned(false),
      sport(),
      isBike(false),
      isRun(false),
      isSwim(false),
      isXtrain(false),
      isAero(false),
      samples(false),
      zoneRange(-1),
      hrZoneRange(-1),
      paceZoneRange(-1),
      fingerprint(0),
      metacrc(0),
      crc(0),
      timestamp(0),
      dbversion(0),
      udbversion(0),
      weight(0)
{
}

RideItem::~RideItem() = default;

RideFile *RideItem::ride(bool)
{
    return ride_;
}

QString RideItem::getLinkedFileName() const
{
    return metadata_.value(QStringLiteral("Linked Filename"));
}

void RideItem::setLinkedFileName(const QString &fileName)
{
    metadata_.insert(QStringLiteral("Linked Filename"), fileName);
}

void RideItem::clearLinkedFileName()
{
    metadata_.remove(QStringLiteral("Linked Filename"));
}

bool RideItem::hasLinkedActivity() const
{
    return !getLinkedFileName().isEmpty();
}

void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}

RideCacheModel::RideCacheModel(
    Context *modelContext, RideCache *cache)
    : context(modelContext),
      rideCache(cache),
      factory(nullptr),
      columns_(0)
{
}

int RideCacheModel::rowCount(const QModelIndex &) const
{
    return rideCache ? rideCache->count() : 0;
}

int RideCacheModel::columnCount(const QModelIndex &) const
{
    return columns_;
}

Qt::ItemFlags RideCacheModel::flags(const QModelIndex &) const
{
    return Qt::NoItemFlags;
}

QVariant RideCacheModel::headerData(
    int, Qt::Orientation, int) const
{
    return {};
}

bool RideCacheModel::setHeaderData(
    int, Qt::Orientation, const QVariant &, int)
{
    return false;
}

QVariant RideCacheModel::data(const QModelIndex &, int) const
{
    return {};
}

void RideCacheModel::configChanged(qint32) {}
void RideCacheModel::refreshUpdate(QDate) {}
void RideCacheModel::refreshStart() {}
void RideCacheModel::refreshEnd() {}
void RideCacheModel::itemChanged(RideItem *) {}
void RideCacheModel::itemAdded(RideItem *) {}
void RideCacheModel::beginReset() { beginResetModel(); }
void RideCacheModel::endReset() { endResetModel(); }

void RideCacheModel::startInsert(int first, int last)
{
    beginInsertRows(QModelIndex(), first, last);
}

void RideCacheModel::endInsert()
{
    endInsertRows();
}

void RideCacheModel::rowsChanged(QVector<int>) {}

void RideCacheModel::startRemove(int row)
{
    beginRemoveRows(QModelIndex(), row, row);
}

void RideCacheModel::endRemove(int)
{
    endRemoveRows();
}

Estimator::Estimator(Context *estimatorContext)
    : context(estimatorContext),
      abort(false)
{
}

Estimator::~Estimator() = default;

void Estimator::run() {}
void Estimator::stop() {}
void Estimator::refresh() {}
void Estimator::calculate() {}

Performance Estimator::getPerformanceForDate(QDate, QString)
{
    return Performance(QDate(), 0.0, 0.0, 0.0);
}

RideCache::RideCache(Context *cacheContext)
    : context(cacheContext),
      directory(cacheContext->athlete->home->activities()),
      plannedDirectory(cacheContext->athlete->home->planned()),
      model_(new RideCacheModel(cacheContext, this)),
      exiting(false),
      progress_(100.0),
      estimator(new Estimator(cacheContext)),
      first(false)
{
}

RideCache::~RideCache()
{
    qDeleteAll(rides_);
    rides_.clear();
    qDeleteAll(delete_);
    delete_.clear();
    delete model_;
    model_ = nullptr;
    delete estimator;
    estimator = nullptr;
}

void RideCache::load() {}
void RideCache::postLoad() {}
void RideCache::save(bool, QString) {}
void RideCache::cleanupThread(RideCacheRefreshThread *) {}
int RideCache::find(RideItem *) { return -1; }
void RideCache::configChanged(qint32) {}
void RideCache::progressing(int) {}
void RideCache::cancel() {}
void RideCache::itemChanged() {}
void RideCache::garbageCollect() {}
void RideCache::initEstimates() {}
void RideCache::refresh() {}

RideItem *RideCache::getLinkedActivity(RideItem *)
{
    return nullptr;
}

bool RideCache::saveActivity(RideItem *, QString &)
{
    return true;
}

DataProcessorFactory *DataProcessorFactory::instance_ = nullptr;
bool DataProcessorFactory::autoprocess = true;

DataProcessorFactory &DataProcessorFactory::instance()
{
    if (!instance_) instance_ = new DataProcessorFactory();
    return *instance_;
}

bool DataProcessorFactory::autoProcess(RideFile *, QString, QString)
{
    return false;
}
