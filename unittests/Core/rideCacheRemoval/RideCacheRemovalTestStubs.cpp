#include "LTMSettings.h"
#include "LinkedActivityRemovalJournal.h"
#include "Athlete.h"
#include "Context.h"
#include "DataProcessor.h"
#include "Estimator.h"
#include "ErgFile.h"
#include "RideCache.h"
#include "RideCacheModel.h"
#include "RideItem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <cstring>
#include <cstdlib>
#include <functional>
#include <utility>

namespace {

int rideCacheRefreshCount = 0;
int estimatorRefreshCount = 0;
QString removalCleanupFailurePath;
QString removalMoveFailurePath;
QString removalMoveFailureTargetPath;
QString removalPartialMoveFailurePath;
QString removalPartialMoveFailureTargetPath;
bool removalPartialMoveRemovesSource = false;
QString removalMoveMutationPath;
QByteArray removalMoveMutationContents;
QString removalMoveActionPath;
std::function<void()> removalMoveAction;
QString removalSyncFailurePath;
int removalSyncFailureCount = 0;
QString removalSaveFailureFileName;
QSet<int> removalSaveFailureCalls;
int removalSaveCallCount = 0;
QString removalSaveActionFileName;
int removalSaveActionCall = 0;
int removalSaveActionCallCount = 0;
std::function<void()> removalSaveAction;
QString removalSaveRenameFileName;
QString removalSaveRenameTargetDirectory;
QString removalSaveRenameTargetFileName;
QString removalPersistedSaveFileName;
QByteArray removalPersistedSaveContents;
QString removalDirectPersistFileName;
int removalDirectPersistCall = 0;
int removalDirectPersistCallCount = 0;
QByteArray removalDirectPersistContents;
RideFile *removalLastProcessedRide = nullptr;
bool removalProcessorFailure = false;
std::function<void()> removalProcessorAction;
std::function<void()> removalLinkMutationAction;
int removalLinkMutationCall = 0;
int removalLinkMutationTriggerCall = 0;
int removalCancelCount = 0;
int removalTransitionOccurrence = 0;

} // namespace

void resetRideCacheRemovalRefreshCounts()
{
    rideCacheRefreshCount = 0;
    estimatorRefreshCount = 0;
    removalCleanupFailurePath.clear();
    removalMoveFailurePath.clear();
    removalMoveFailureTargetPath.clear();
    removalPartialMoveFailurePath.clear();
    removalPartialMoveFailureTargetPath.clear();
    removalPartialMoveRemovesSource = false;
    removalMoveMutationPath.clear();
    removalMoveMutationContents.clear();
    removalMoveActionPath.clear();
    removalMoveAction = {};
    removalSyncFailurePath.clear();
    removalSyncFailureCount = 0;
    removalSaveFailureFileName.clear();
    removalSaveFailureCalls.clear();
    removalSaveCallCount = 0;
    removalSaveActionFileName.clear();
    removalSaveActionCall = 0;
    removalSaveActionCallCount = 0;
    removalSaveAction = {};
    removalSaveRenameFileName.clear();
    removalSaveRenameTargetDirectory.clear();
    removalSaveRenameTargetFileName.clear();
    removalPersistedSaveFileName.clear();
    removalPersistedSaveContents.clear();
    removalDirectPersistFileName.clear();
    removalDirectPersistCall = 0;
    removalDirectPersistCallCount = 0;
    removalDirectPersistContents.clear();
    removalLastProcessedRide = nullptr;
    removalProcessorFailure = false;
    removalProcessorAction = {};
    removalLinkMutationAction = {};
    removalLinkMutationCall = 0;
    removalLinkMutationTriggerCall = 0;
    removalCancelCount = 0;
    removalTransitionOccurrence = 0;
}

int rideCacheRemovalRefreshCount()
{
    return rideCacheRefreshCount;
}

int rideCacheRemovalEstimatorRefreshCount()
{
    return estimatorRefreshCount;
}

void setRideCacheRemovalCleanupFailurePath(const QString &path)
{
    removalCleanupFailurePath = path;
}

bool rideCacheRemovalShouldFailCleanup(const QString &path)
{
    return !removalCleanupFailurePath.isEmpty()
        && path == removalCleanupFailurePath;
}

void setRideCacheRemovalMoveFailurePath(const QString &path)
{
    removalMoveFailurePath = path;
}

bool rideCacheRemovalShouldFailMove(
    const QString &sourcePath,
    const QString &targetPath)
{
    if ((!removalPartialMoveFailurePath.isEmpty()
         && sourcePath == removalPartialMoveFailurePath)
        || (!removalPartialMoveFailureTargetPath.isEmpty()
            && targetPath
                == removalPartialMoveFailureTargetPath)) {
        removalPartialMoveFailurePath.clear();
        removalPartialMoveFailureTargetPath.clear();
        QFile::copy(sourcePath, targetPath);
        if (removalPartialMoveRemovesSource)
            QFile::remove(sourcePath);
        removalPartialMoveRemovesSource = false;
        return true;
    }
    if (!removalMoveFailurePath.isEmpty()
        && sourcePath == removalMoveFailurePath) {
        return true;
    }
    if (!removalMoveFailureTargetPath.isEmpty()
        && targetPath
            == removalMoveFailureTargetPath) {
        removalMoveFailureTargetPath.clear();
        return true;
    }
    return false;
}

void setRideCacheRemovalMoveFailureTargetPath(
    const QString &path)
{
    removalMoveFailureTargetPath = path;
}

void setRideCacheRemovalPartialMoveFailurePath(
    const QString &path,
    bool removeSource)
{
    removalPartialMoveFailurePath = path;
    removalPartialMoveFailureTargetPath.clear();
    removalPartialMoveRemovesSource = removeSource;
}

void setRideCacheRemovalPartialMoveFailureTargetPath(
    const QString &path,
    bool removeSource)
{
    removalPartialMoveFailurePath.clear();
    removalPartialMoveFailureTargetPath = path;
    removalPartialMoveRemovesSource = removeSource;
}

void setRideCacheRemovalMoveMutation(
    const QString &path,
    const QByteArray &contents)
{
    removalMoveMutationPath = path;
    removalMoveMutationContents = contents;
}

void setRideCacheRemovalMoveAction(
    const QString &path,
    const std::function<void()> &action)
{
    removalMoveActionPath = path;
    removalMoveAction = action;
}

void rideCacheRemovalMutateBeforeMove(
    const QString &sourcePath,
    const QString &)
{
    if (!removalMoveActionPath.isEmpty()
        && sourcePath == removalMoveActionPath
        && removalMoveAction) {
        removalMoveActionPath.clear();
        const std::function<void()> action =
            std::move(removalMoveAction);
        removalMoveAction = {};
        action();
    }
    if (removalMoveMutationPath.isEmpty()
        || sourcePath != removalMoveMutationPath) {
        return;
    }

    removalMoveMutationPath.clear();
    QFile file(sourcePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(removalMoveMutationContents);
    file.flush();
}

void setRideCacheRemovalSyncFailurePath(const QString &path)
{
    removalSyncFailurePath = path;
    removalSyncFailureCount = 1;
}

void setRideCacheRemovalSyncFailureCount(
    const QString &path,
    int count)
{
    removalSyncFailurePath = path;
    removalSyncFailureCount = count;
}

bool rideCacheRemovalShouldFailSync(const QString &path)
{
    if (removalSyncFailurePath.isEmpty()
        || path != removalSyncFailurePath) {
        return false;
    }
    --removalSyncFailureCount;
    if (removalSyncFailureCount <= 0) {
        removalSyncFailurePath.clear();
        removalSyncFailureCount = 0;
    }
    return true;
}

void rideCacheRemovalTransitionReached(
    const char *transition)
{
    const QByteArray requested =
        qgetenv("GC_RIDE_CACHE_REMOVAL_CRASH_PHASE");
    if (!requested.isEmpty()
        && requested == transition) {
        ++removalTransitionOccurrence;
        bool validOccurrence = false;
        const int requestedOccurrence =
            qEnvironmentVariableIntValue(
                "GC_RIDE_CACHE_REMOVAL_CRASH_OCCURRENCE",
                &validOccurrence);
        if (!validOccurrence
            || requestedOccurrence
                == removalTransitionOccurrence) {
            std::_Exit(86);
        }
    }
}

void setRideCacheRemovalSaveFailureFileName(
    const QString &fileName)
{
    removalSaveFailureFileName = fileName;
    removalSaveFailureCalls = {1};
    removalSaveCallCount = 0;
}

void setRideCacheRemovalSaveFailureOnCall(
    const QString &fileName,
    int call)
{
    removalSaveFailureFileName = fileName;
    removalSaveFailureCalls = {call};
    removalSaveCallCount = 0;
}

void setRideCacheRemovalSaveFailureCalls(
    const QString &fileName,
    const QSet<int> &calls)
{
    removalSaveFailureFileName = fileName;
    removalSaveFailureCalls = calls;
    removalSaveCallCount = 0;
}

int rideCacheRemovalSaveCallCount()
{
    return removalSaveCallCount;
}

int rideCacheRemovalCancelCount()
{
    return removalCancelCount;
}

void setRideCacheRemovalSaveActionOnCall(
    const QString &fileName,
    int call,
    const std::function<void()> &action)
{
    removalSaveActionFileName = fileName;
    removalSaveActionCall = call;
    removalSaveActionCallCount = 0;
    removalSaveAction = action;
}

void setRideCacheRemovalSuccessfulSaveRename(
    const QString &fileName,
    const QString &targetDirectory,
    const QString &targetFileName)
{
    removalSaveRenameFileName = fileName;
    removalSaveRenameTargetDirectory = targetDirectory;
    removalSaveRenameTargetFileName = targetFileName;
}

void setRideCacheRemovalPersistedSaveContents(
    const QString &fileName,
    const QByteArray &contents)
{
    removalPersistedSaveFileName = fileName;
    removalPersistedSaveContents = contents;
}

void setRideCacheRemovalPersistedSaveContentsOnCall(
    const QString &fileName,
    int call,
    const QByteArray &contents)
{
    removalDirectPersistFileName = fileName;
    removalDirectPersistCall = call;
    removalDirectPersistCallCount = 0;
    removalDirectPersistContents = contents;
}

bool rideCacheRemovalLinkedSaveKeepsPath(
    RideItem *item)
{
    return item
        && (removalSaveRenameFileName.isEmpty()
            || item->fileName
                != removalSaveRenameFileName);
}

RideFile *rideCacheRemovalLastProcessedRide()
{
    return removalLastProcessedRide;
}

void setRideCacheRemovalProcessorFailure(bool fail)
{
    removalProcessorFailure = fail;
}

void setRideCacheRemovalProcessorAction(
    const std::function<void()> &action)
{
    removalProcessorAction = action;
}

void setRideCacheRemovalLinkMutationActionOnCall(
    int call,
    const std::function<void()> &action)
{
    removalLinkMutationAction = action;
    removalLinkMutationCall = 0;
    removalLinkMutationTriggerCall = call;
}

void runRideCacheRemovalLinkMutationAction()
{
    if (!removalLinkMutationAction) return;
    ++removalLinkMutationCall;
    if (removalLinkMutationCall
        != removalLinkMutationTriggerCall) {
        return;
    }
    const std::function<void()> action =
        std::move(removalLinkMutationAction);
    removalLinkMutationTriggerCall = 0;
    action();
}

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
    if (ride_
        && !path.isEmpty()
        && !QFileInfo(
                QDir(path).filePath(fileName))
                .isFile()) {
        return nullptr;
    }
    return ride_;
}

void RideItem::close()
{
    ride_ = nullptr;
}

QString RideItem::getLinkedFileName() const
{
    return metadata_.value(QStringLiteral("Linked Filename"));
}

void RideItem::setLinkedFileName(const QString &fileName)
{
    metadata_.insert(QStringLiteral("Linked Filename"), fileName);
    runRideCacheRemovalLinkMutationAction();
}

void RideItem::clearLinkedFileName()
{
    metadata_.remove(QStringLiteral("Linked Filename"));
    runRideCacheRemovalLinkMutationAction();
}

bool RideItem::hasLinkedActivity() const
{
    return !getLinkedFileName().isEmpty();
}

void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::rideFileDestroyed(QObject *rideFile)
{
    if (ride_ == rideFile) ride_ = nullptr;
}
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
void Estimator::refresh() { ++estimatorRefreshCount; }
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
    LinkedActivityRemoval::Journal::reconcileAll(
        cacheContext->athlete->home->root().absolutePath(),
        startupRecoveryError_);
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
void RideCache::cancel() { ++removalCancelCount; }
void RideCache::itemChanged() {}
void RideCache::garbageCollect() {}
void RideCache::initEstimates() {}
void RideCache::refresh() { ++rideCacheRefreshCount; }

RideItem *RideCache::getLinkedActivity(RideItem *item)
{
    if (!item) return nullptr;
    const QString linkedFileName =
        item->getLinkedFileName();
    for (RideItem *candidate : rides_) {
        if (candidate
            && candidate != item
            && candidate->planned
                != item->planned
            && candidate->fileName
                == linkedFileName) {
            return candidate;
        }
    }
    return nullptr;
}

bool RideCache::saveActivity(RideItem *item, QString &error)
{
    if (item
        && item->fileName
            == removalSaveFailureFileName) {
        ++removalSaveCallCount;
        if (removalSaveFailureCalls.contains(
                removalSaveCallCount)) {
            error = QStringLiteral(
                "injected linked activity save failure");
            return false;
        }
    }
    if (item
        && item->fileName
            == removalSaveRenameFileName) {
        const QString targetDirectory =
            removalSaveRenameTargetDirectory;
        const QString targetFileName =
            removalSaveRenameTargetFileName;
        const QString sourcePath =
            QDir(item->path).filePath(item->fileName);
        const QString targetPath =
            QDir(targetDirectory).filePath(targetFileName);
        removalSaveRenameFileName.clear();
        removalSaveRenameTargetDirectory.clear();
        removalSaveRenameTargetFileName.clear();
        if (!QDir().mkpath(targetDirectory)
            || !QFile::rename(sourcePath, targetPath)) {
            error = QStringLiteral(
                "injected linked activity save rename failed");
            return false;
        }
        item->path = targetDirectory;
        item->fileName = targetFileName;
    }
    if (item
        && item->fileName
            == removalSaveActionFileName) {
        ++removalSaveActionCallCount;
        if (removalSaveActionCallCount
                == removalSaveActionCall
            && removalSaveAction) {
            const std::function<void()> action =
                std::move(removalSaveAction);
            removalSaveActionCall = 0;
            action();
        }
    }
    if (item
        && item->fileName
            == removalDirectPersistFileName) {
        ++removalDirectPersistCallCount;
        if (removalDirectPersistCallCount
                == removalDirectPersistCall) {
            QFile file(QDir(item->path).filePath(item->fileName));
            if (!file.open(
                    QIODevice::WriteOnly
                    | QIODevice::Truncate)
                || file.write(removalDirectPersistContents)
                    != removalDirectPersistContents.size()
                || !file.flush()) {
                error = file.errorString();
                return false;
            }
        }
    }
    return true;
}

bool RideCache::saveActivity(
    RideItem *item,
    QString &error,
    const AtomicFileWriterFactory &writerFactory)
{
    const QString originalFileName = item
        ? item->fileName : QString();
    if (!saveActivity(item, error))
        return false;
    if (!item) {
        error = QStringLiteral("No activity given");
        return false;
    }

    const QString targetPath =
        QDir(item->path).filePath(item->fileName);
    QByteArray contents;
    if (originalFileName
            == removalPersistedSaveFileName) {
        contents = removalPersistedSaveContents;
    } else {
        QFile source(targetPath);
        if (!source.open(QIODevice::ReadOnly)) {
            error = source.errorString();
            return false;
        }
        contents = source.readAll();
        if (source.error() != QFileDevice::NoError) {
            error = source.errorString();
            return false;
        }
    }
    return writeFileAtomically(
        targetPath, contents, writerFactory,
        error, true);
}

DataProcessorFactory *DataProcessorFactory::instance_ = nullptr;
bool DataProcessorFactory::autoprocess = true;

DataProcessorFactory &DataProcessorFactory::instance()
{
    if (!instance_) instance_ = new DataProcessorFactory();
    return *instance_;
}

bool DataProcessorFactory::autoProcess(
    RideFile *ride, QString, QString)
{
    if (removalProcessorFailure)
        throw QStringLiteral(
            "injected activity processor failure");
    if (removalProcessorAction) {
        const std::function<void()> action =
            std::move(removalProcessorAction);
        removalProcessorAction = {};
        action();
    }
    removalLastProcessedRide = ride;
    return false;
}
