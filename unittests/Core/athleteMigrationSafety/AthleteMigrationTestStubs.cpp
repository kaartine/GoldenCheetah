/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Charts/LTMSettings.h"
#include "Cloud/CalendarDownload.h"
#include "Cloud/CloudService.h"
#include "Cloud/MeasuresDownload.h"
#include "Core/Athlete.h"
#include "Core/Context.h"
#include "Core/Measures.h"
#include "Core/NamedSearch.h"
#include "Core/RideCache.h"
#include "Core/RideItem.h"
#include "Core/Route.h"
#include "Core/Seasons.h"
#include "Core/Settings.h"
#include "FileIO/CsvRideFile.h"
#include "FileIO/DataProcessor.h"
#include "FileIO/JsonRideFile.h"
#include "FileIO/MeasuresCsvImport.h"
#include "FileIO/RideAutoImportConfig.h"
#include "Gui/Colors.h"
#include "Gui/CompareDateRange.h"
#include "Gui/CompareInterval.h"
#include "Gui/HelpWhatsThis.h"
#include "Gui/MainWindow.h"
#include "Metrics/HrZones.h"
#include "Metrics/PaceZones.h"
#include "Metrics/PMCData.h"
#include "Metrics/RideMetadata.h"
#include "Metrics/Zones.h"
#include "Train/ErgFile.h"
#include "Train/Library.h"
#include "Train/TrainDB.h"
#include "Train/VideoWindow.h"

#include <QCoreApplication>
#include <QHash>
#include <QMessageBox>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

QHash<QString, QVariant> testValues;
QMutex testValuesMutex;
int settingsCrossThreadWrites = 0;
QMutex validContextsMutex;
QSet<Context *> validContexts;
bool throwOnAthleteIdWrite = false;
bool throwOnRideCacheConstruction = false;
bool emitRideCacheLoadComplete = false;
bool throwOnChartLoad = false;
bool measuresWriteFails = false;
bool includeHrvMeasuresGroup = false;
bool trainDbUpgradeRequired = false;
int rideCacheCancelCalls = 0;
int rideCacheRefreshCalls = 0;
TrainDB::LegacyMigrationPlan trainDbMigrationPlan;
LibraryImportResult trainDbImportResult;
QStringList importedLibraryFiles;
int libraryImportCalls = 0;
int legacyFinalizeCalls = 0;
int legacyDropCalls = 0;
int libraryInitialiseCalls = 0;
bool libraryInitialisedBeforeImport = false;
bool finalizedExpectedImport = false;
int rideFileOpenCalls = 0;
QList<QByteArray> rideFilePayloads;
int calendarDownloadConstructionCalls = 0;

QString settingKey(const QString &athlete, const QString &key)
{
    return athlete + QLatin1Char('\n') + key;
}

GSettings testSettings(QStringLiteral("GoldenCheetahTests"),
                       QStringLiteral("AthleteMigrationSafety"));

} // namespace

namespace Utils {
bool qstringascend(const QString &left, const QString &right)
{
    return left < right;
}
}

GSettings *appsettings = &testSettings;
TrainDB *trainDB = nullptr;
QString gcroot;
int OperatingSystem = LINUX;
double dpiXFactor = 1.0;
double dpiYFactor = 1.0;

bool DataProcessorFactory::autoprocess = true;

void resetAthleteMigrationTestSettings()
{
    {
        QMutexLocker locker(&testValuesMutex);
        testValues.clear();
        settingsCrossThreadWrites = 0;
    }
    throwOnAthleteIdWrite = false;
    throwOnRideCacheConstruction = false;
    emitRideCacheLoadComplete = false;
    throwOnChartLoad = false;
    measuresWriteFails = false;
    includeHrvMeasuresGroup = false;
    Athlete::setWaitForRideCacheLoadForTest(false);
    MeasuresDownload::setAutoDownloadProbeForTest({});
    MeasuresDownload::setManualDownloadProbeForTest({});
    rideCacheCancelCalls = 0;
    rideCacheRefreshCalls = 0;
    trainDbUpgradeRequired = false;
    trainDbMigrationPlan = {};
    trainDbImportResult = {};
    importedLibraryFiles.clear();
    libraryImportCalls = 0;
    legacyFinalizeCalls = 0;
    legacyDropCalls = 0;
    libraryInitialiseCalls = 0;
    libraryInitialisedBeforeImport = false;
    gcroot.clear();
    finalizedExpectedImport = false;
    trainDB = nullptr;
    rideFileOpenCalls = 0;
    rideFilePayloads.clear();
    calendarDownloadConstructionCalls = 0;
}

int athleteMigrationCalendarDownloadConstructionCalls()
{
    return calendarDownloadConstructionCalls;
}

void configureAthleteMigrationTrainDbUpgrade(
    TrainDB *database,
    const TrainDB::LegacyMigrationPlan &plan,
    const LibraryImportResult &result)
{
    trainDB = database;
    trainDbUpgradeRequired = true;
    trainDbMigrationPlan = plan;
    trainDbImportResult = result;
}

int athleteMigrationLibraryImportCalls() { return libraryImportCalls; }
QStringList athleteMigrationImportedFiles() { return importedLibraryFiles; }
int athleteMigrationLibraryInitialiseCalls() { return libraryInitialiseCalls; }
bool athleteMigrationLibraryInitialisedBeforeImport()
{
    return libraryInitialisedBeforeImport;
}
int athleteMigrationLegacyFinalizeCalls() { return legacyFinalizeCalls; }
int athleteMigrationLegacyDropCalls() { return legacyDropCalls; }
bool athleteMigrationFinalizedExpectedImport()
{
    return finalizedExpectedImport;
}
int athleteMigrationRideFileOpenCalls() { return rideFileOpenCalls; }
QList<QByteArray> athleteMigrationRideFilePayloads()
{
    return rideFilePayloads;
}

int athleteMigrationSettingsCrossThreadWrites()
{
    QMutexLocker locker(&testValuesMutex);
    return settingsCrossThreadWrites;
}

void setAthleteMigrationThrowOnIdWrite(bool enabled)
{
    throwOnAthleteIdWrite = enabled;
}

void setAthleteMigrationThrowOnRideCacheConstruction(bool enabled)
{
    throwOnRideCacheConstruction = enabled;
}

void setAthleteMigrationEmitRideCacheLoadComplete(bool enabled)
{
    emitRideCacheLoadComplete = enabled;
}

void setAthleteMigrationThrowOnChartLoad(bool enabled)
{
    throwOnChartLoad = enabled;
}

void setAthleteMigrationMeasuresWriteFails(bool enabled)
{
    measuresWriteFails = enabled;
}

void setAthleteMigrationIncludeHrvMeasuresGroup(bool enabled)
{
    includeHrvMeasuresGroup = enabled;
}

int athleteMigrationRideCacheCancelCalls()
{
    return rideCacheCancelCalls;
}

int athleteMigrationRideCacheRefreshCalls()
{
    return rideCacheRefreshCalls;
}

GSettings::GSettings(QString, QString)
{
}

GSettings::~GSettings()
{
}

QVariant GSettings::value(
        const QObject *,
        const QString key,
        const QVariant def)
{
    QMutexLocker locker(&testValuesMutex);
    return testValues.value(settingKey(QString(), key), def);
}

void GSettings::setValue(QString key, QVariant value)
{
    QMutexLocker locker(&testValuesMutex);
    if (QCoreApplication::instance()
        && QThread::currentThread()
                != QCoreApplication::instance()->thread()) {
        ++settingsCrossThreadWrites;
    }
    testValues.insert(settingKey(QString(), key), value);
}

void GSettings::remove(const QString &key)
{
    QMutexLocker locker(&testValuesMutex);
    testValues.remove(settingKey(QString(), key));
}

QVariant GSettings::cvalue(QString athleteName, QString key, QVariant def)
{
    QMutexLocker locker(&testValuesMutex);
    return testValues.value(settingKey(athleteName, key), def);
}

void GSettings::setCValue(QString athleteName, QString key, QVariant value)
{
    QMutexLocker locker(&testValuesMutex);
    if (QCoreApplication::instance()
        && QThread::currentThread()
                != QCoreApplication::instance()->thread()) {
        ++settingsCrossThreadWrites;
    }

    if (throwOnAthleteIdWrite && key == GC_ATHLETE_ID) {
        throw std::runtime_error("injected athlete settings failure");
    }
    testValues.insert(settingKey(athleteName, key), value);
}

bool GSettings::contains(const QString &) const
{
    return false;
}

void GSettings::initializeQSettingsAthlete(QString, QString)
{
}

AppearanceSettings GSettings::defaultAppearanceSettings()
{
    return {};
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

QStringList RideMetadata::sports()
{
    return {};
}

void RideMetadata::readXML(
    QString,
    QList<KeywordDefinition> &,
    QList<FieldDefinition> &,
    QString &,
    QList<DefaultDefinition> &)
{
}

bool RideMetadata::serialize(
    QString,
    const QList<KeywordDefinition> &,
    const QList<FieldDefinition> &,
    QString,
    const QList<DefaultDefinition> &,
    QString *,
    const AtomicFileWriterFactory &)
{
    return true;
}

bool MainWindow::isStarting() const
{
    return false;
}

Context::Context(MainWindow *window)
    : mainWindow(window)
{
    nav = nullptr;
    viewIndex = 0;
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
    {
        QMutexLocker locker(&validContextsMutex);
        validContexts.insert(this);
    }
}

Context::~Context()
{
    QMutexLocker locker(&validContextsMutex);
    validContexts.remove(this);
}

bool Context::isValid(Context *context)
{
    QMutexLocker locker(&validContextsMutex);
    return validContexts.contains(context);
}

DateRange::DateRange(QDate from, QDate to, QString name, QColor color)
    : from(from), to(to), name(std::move(name)), color(color),
      valid(from.isValid() && to.isValid())
{
}

DateRange::DateRange(const DateRange &other)
    : from(other.from), to(other.to), name(other.name), color(other.color),
      id(other.id), valid(from.isValid() && to.isValid())
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

PlanFilter::PlanFilter(PlanFilterType type)
    : type(type)
{
}

Specification::Specification()
    : it(nullptr), recintsecs(0), ri(nullptr)
{
}

void Specification::setDateRange(DateRange range)
{
    dr = std::move(range);
}

bool Specification::pass(RideItem *) const
{
    return true;
}

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

Context *createAthleteMigrationTestContext()
{
    return new Context(nullptr);
}

RideItem::RideItem()
{
}

RideItem::~RideItem()
{
}

void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}
double RideItem::getForSymbol(QString, bool) { return 0.0; }

QString RideFile::sportTag(QString sport)
{
    return sport;
}

bool RideFile::parseRideFileName(const QString &name, QDateTime *dateTime)
{
    const QDateTime parsed = QDateTime::fromString(
            name.left(19), QStringLiteral("yyyy_MM_dd_HH_mm_ss"));
    if (!parsed.isValid()) return false;
    if (dateTime) *dateTime = parsed;
    return true;
}

void RideFile::recalculateDerivedSeries(bool) {}

void Zones::initializeZoneParameters() {}
bool Zones::read(QFile &) { return true; }
int Zones::getRangeSize() const { return 0; }

void HrZones::initializeZoneParameters() {}
bool HrZones::read(QFile &) { return true; }
int HrZones::getRangeSize() const { return 0; }

void PaceZones::initializeZoneParameters() {}
bool PaceZones::read(QFile &) { return true; }

void Seasons::readSeasons() {}
void NamedSearches::read() {}

void EditNamedSearches::addClicked() {}
void EditNamedSearches::updateClicked() {}
void EditNamedSearches::upClicked() {}
void EditNamedSearches::downClicked() {}
void EditNamedSearches::deleteClicked() {}
void EditNamedSearches::selectionChanged() {}

void RideAutoImportConfig::readConfig() {}

Routes::Routes(Context *context, const QDir &home)
    : home(home), context(context)
{
}

Routes::~Routes() = default;

Measures::Measures(QDir dir, bool withData)
    : dir(dir), withData(withData)
{
    MeasuresGroup *body = new MeasuresGroup(dir, withData);
    body->setSymbol(QStringLiteral("Body"));
    body->setName(QStringLiteral("Body"));
    groups.append(body);
    if (includeHrvMeasuresGroup) {
        MeasuresGroup *hrv = new MeasuresGroup(dir, withData);
        hrv->setSymbol(QStringLiteral("Hrv"));
        hrv->setName(QStringLiteral("Hrv"));
        groups.append(hrv);
    }
}

Measures::~Measures()
{
    qDeleteAll(groups);
}

QStringList Measures::getGroupSymbols() const
{
    QStringList symbols;
    for (const MeasuresGroup *group : groups) {
        if (group) symbols.append(group->getSymbol());
    }
    return symbols;
}

MeasuresGroup *Measures::getGroup(int group)
{
    return group >= 0 && group < groups.size()
        ? groups.at(group)
        : nullptr;
}

void MeasuresGroup::setMeasures(QList<Measure> &measures)
{
    measures_ = measures;
}

bool MeasuresGroup::write(
        QString *error, const AtomicFileWriterFactory &)
{
    if (!measuresWriteFails) return true;

    const QString message =
            QStringLiteral("injected measures write failure");
    if (error) {
        *error = message;
    } else {
        QMessageBox::critical(
            nullptr, QStringLiteral("Problem Saving Measures"),
            message);
    }
    return false;
}

RideCache::RideCache(Context *context)
    : context(context),
      model_(nullptr),
      exiting(false),
      progress_(0.0),
      estimator(nullptr),
      first(false)
{
    if (throwOnRideCacheConstruction) {
        throw std::runtime_error("injected ride cache construction failure");
    }
    if (emitRideCacheLoadComplete) {
        QTimer::singleShot(
            0, this,
            [this]() { emit loadComplete(); });
    }
}

RideCache::~RideCache() = default;

void RideCache::load() {}
void RideCache::postLoad() {}
void RideCache::save(bool, QString) {}
void RideCache::cleanupThread(RideCacheRefreshThread *) {}
int RideCache::find(RideItem *) { return -1; }
void RideCache::configChanged(qint32) {}
void RideCache::progressing(int) {}
void RideCache::cancel() { ++rideCacheCancelCalls; }
QList<QDateTime> RideCache::getAllDates() { return {}; }
void RideCache::refresh() { ++rideCacheRefreshCalls; }
void RideCache::itemChanged() {}
void RideCache::garbageCollect() {}
void RideCache::initEstimates() {}
void RideCache::addRide(QString, bool, bool, bool, bool) {}
bool RideCache::removeCurrentRide() { return false; }

void LTMSettings::readChartXML(QDir, bool, QList<LTMSettings> &)
{
    if (throwOnChartLoad)
        throw std::runtime_error("injected chart load failure");
}
void LTMSettings::writeChartXML(QDir, QList<LTMSettings>) {}

void PMCData::invalidate() {}

HelpWhatsThis::HelpWhatsThis(QObject *parent)
    : QObject(parent)
{
}

QString HelpWhatsThis::getWhatsThisText(GCHelp)
{
    return {};
}

bool HelpWhatsThis::eventFilter(QObject *, QEvent *)
{
    return false;
}

MeasuresCsvImport::MeasuresCsvImport(
        Context *context, QWidget *parent)
    : context(context), parent(parent)
{
}

MeasuresCsvImport::~MeasuresCsvImport() = default;

bool MeasuresCsvImport::getMeasures(
        MeasuresGroup *, QString &, QDateTime, QDateTime,
        QList<Measure> &)
{
    return false;
}

CalendarDownload::CalendarDownload(Context *context)
    : context(context), nam(nullptr)
{
    ++calendarDownloadConstructionCalls;
}

bool CalendarDownload::download() { return false; }
void CalendarDownload::downloadFinished(QNetworkReply *) {}

QColor GCColor::getColor(int)
{
    return {};
}

void GCColor::setColor(int, QColor) {}
void GCColor::applyTheme(int) {}

TrainDB::TrainDB(QDir home)
    : home(std::move(home)), db(nullptr), sessionid(QStringLiteral("test"))
{
}

TrainDB::~TrainDB() = default;

QStringList TrainDB::getWorkouts() const { return {}; }
QHash<QString, QString> TrainDB::getWorkoutHashes() const { return {}; }
void TrainDB::deferTagSignals(bool) {}
bool TrainDB::isDeferredTagSignals() { return false; }
void TrainDB::catchupTagSignals() {}
int TrainDB::addTag(const QString &) { return -1; }
bool TrainDB::updateTag(int, const QString &) { return false; }
bool TrainDB::deleteTag(int) { return false; }
bool TrainDB::deleteTag(const QString &) { return false; }
int TrainDB::getTagId(const QString &) const { return -1; }
QString TrainDB::getTagLabel(int) const { return {}; }
bool TrainDB::hasTag(int) const { return false; }
bool TrainDB::hasTag(const QString &) const { return false; }
QList<TagStore::Tag> TrainDB::getTags() const { return {}; }
QList<int> TrainDB::getTagIds() const { return {}; }
QStringList TrainDB::getTagLabels() const { return {}; }
QStringList TrainDB::getTagLabels(const QList<int>) const { return {}; }
int TrainDB::countTagUsage(int) const { return 0; }
bool TrainDB::upgradeDefaultEntriesWorkout() { return true; }
bool TrainDB::needsUpgrade() const { return trainDbUpgradeRequired; }
TrainDB::LegacyMigrationPlan TrainDB::legacyMigrationPlan() const
{
    return trainDbMigrationPlan;
}
QStringList TrainDB::getMigrateableWorkoutPaths() const
{
    return trainDbMigrationPlan.workouts;
}
QStringList TrainDB::getMigrateableVideoPaths() const
{
    return trainDbMigrationPlan.videos;
}
QStringList TrainDB::getMigrateableVideoSyncPaths() const
{
    return trainDbMigrationPlan.videoSyncs;
}
bool TrainDB::dropLegacyTables() const
{
    ++legacyDropCalls;
    return false;
}
bool TrainDB::finalizeLegacyMigration(
    const LegacyMigrationPlan &plan,
    const LibraryImportResult &result) const
{
    ++legacyFinalizeCalls;
    finalizedExpectedImport = plan.valid == trainDbMigrationPlan.valid
        && plan.workouts == trainDbMigrationPlan.workouts
        && plan.videos == trainDbMigrationPlan.videos
        && plan.videoSyncs == trainDbMigrationPlan.videoSyncs
        && result.completed == trainDbImportResult.completed
        && result.requestedFiles == trainDbImportResult.requestedFiles
        && result.importedWorkouts == trainDbImportResult.importedWorkouts
        && result.importedVideos == trainDbImportResult.importedVideos
        && result.importedVideoSyncs == trainDbImportResult.importedVideoSyncs
        && result.failedFiles == trainDbImportResult.failedFiles;
    return finalizedExpectedImport;
}

DataProcessorFactory::~DataProcessorFactory() = default;

DataProcessorFactory &DataProcessorFactory::instance()
{
    static DataProcessorFactory instance;
    return instance;
}

QMap<QString, DataProcessor *>
DataProcessorFactory::getProcessors(bool) const
{
    return {};
}

bool DataProcessorFactory::autoProcess(
        RideFile *, QString, QString)
{
    return false;
}

void RideMetadata::setLinkedDefaults(RideFile *) {}

QString DataProcessor::configKeyAutomation(const QString &id)
{
    return id;
}

QString DataProcessor::configKeyApply(const QString &id)
{
    return id;
}

MediaHelper::MediaHelper() = default;
MediaHelper::~MediaHelper() = default;
bool MediaHelper::isMedia(QString) { return false; }

bool ErgFile::isWorkout(QString) { return false; }

RideFileFactory &RideFileFactory::instance()
{
    static RideFileFactory instance;
    return instance;
}

QStringList RideFileFactory::suffixes() const { return {}; }

RideFile *RideFileFactory::openRideFile(
    Context *, QFile &file, QStringList &, QList<RideFile *> *) const
{
    ++rideFileOpenCalls;
    const bool openedHere = !file.isOpen();
    if (openedHere) file.open(QIODevice::ReadOnly);
    if (file.isOpen()) {
        file.seek(0);
        rideFilePayloads.append(file.readAll());
    } else {
        rideFilePayloads.append(QByteArray());
    }
    if (openedHere) file.close();
    return nullptr;
}

bool RideFileFactory::writeRideFile(
        Context *, const RideFile *, QFile &, QString) const
{
    return false;
}

RideFile *CsvFileReader::openRideFile(
        QFile &, QStringList &, QList<RideFile *> *) const
{
    return nullptr;
}

bool CsvFileReader::writeRideFile(
        Context *, const RideFile *, QFile &, CsvType) const
{
    return false;
}

RideFile *JsonFileReader::openRideFile(
    QFile &, QStringList &, QList<RideFile *> *) const
{
    return nullptr;
}

bool JsonFileReader::writeRideFile(
    Context *, const RideFile *, QFile &) const
{
    return false;
}

bool JsonFileReader::writeRideFile(
    Context *, const RideFile *, QFile &, QString &error, bool, bool) const
{
    error = QStringLiteral("injected migration write failure");
    return false;
}

void Library::initialise(QDir)
{
    ++libraryInitialiseCalls;
}

LibraryImportResult Library::importFiles(
    Context *, QStringList files, LibraryBatchImportConfirmation)
{
    ++libraryImportCalls;
    libraryInitialisedBeforeImport = libraryInitialiseCalls > 0;
    importedLibraryFiles = std::move(files);
    return trainDbImportResult;
}
