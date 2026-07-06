#include "DataProcessor.h"
#include "Estimator.h"
#include "MainWindow.h"
#include "RideItem.h"
#include "Settings.h"

#include <QHash>
#include <stdexcept>
#include <utility>

namespace {

QHash<QString, QVariant> settingsValues;
int autoProcessCalls = 0;
bool throwFromAutoProcess = false;

} // namespace

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
      dateTime(ride ? ride->startTime() : QDateTime()),
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
    if (ride_) {
        connect(ride_, &RideFile::modified, this, &RideItem::modified);
        connect(ride_, &RideFile::saved, this, &RideItem::saved);
        connect(ride_, &RideFile::reverted, this, &RideItem::reverted);
    }
}

RideItem::~RideItem()
{
    if (ride_) {
        disconnect(ride_, nullptr, this, nullptr);
    }
}

RideFile *RideItem::ride(bool)
{
    return ride_;
}

void RideItem::setRide(RideFile *ride)
{
    if (ride_) {
        disconnect(ride_, nullptr, this, nullptr);
    }
    ride_ = ride;
    if (ride_) {
        connect(ride_, &RideFile::modified, this, &RideItem::modified);
        connect(ride_, &RideFile::saved, this, &RideItem::saved);
        connect(ride_, &RideFile::reverted, this, &RideItem::reverted);
    }
    setDirty(true);
}

void RideItem::setDirty(bool dirty)
{
    isdirty = dirty;
}

void RideItem::setFileName(QString ridePath, QString rideFileName)
{
    path = std::move(ridePath);
    fileName = std::move(rideFileName);
}

QString RideItem::getLinkedFileName() const
{
    return metadata_.value(QStringLiteral("Linked Filename"));
}

void RideItem::setLinkedFileName(const QString &linkedFileName)
{
    metadata_.insert(QStringLiteral("Linked Filename"), linkedFileName);
    if (ride_) {
        ride_->setTag(QStringLiteral("Linked Filename"), linkedFileName);
    }
    setDirty(true);
}

void RideItem::modified()
{
    setDirty(true);
}

void RideItem::reverted()
{
    setDirty(false);
}

void RideItem::saved()
{
    setDirty(false);
}

void RideItem::notifyRideDataChanged()
{
    emit rideDataChanged();
}

void RideItem::notifyRideMetadataChanged()
{
    emit rideMetadataChanged();
}

bool MainWindow::filenameWillChange(RideItem *, QString *newName) const
{
    if (newName) {
        newName->clear();
    }
    return false;
}

DataProcessor::Automation DataProcessor::getAutomation() const
{
    return DataProcessor::Manual;
}

DataProcessorFactory *DataProcessorFactory::instance_ = nullptr;

DataProcessorFactory &DataProcessorFactory::instance()
{
    if (!instance_) {
        instance_ = new DataProcessorFactory();
    }
    return *instance_;
}

QMap<QString, DataProcessor *>
DataProcessorFactory::getProcessors(bool) const
{
    return {};
}

bool
DataProcessorFactory::autoProcess(RideFile *, QString, QString)
{
    ++autoProcessCalls;
    if (throwFromAutoProcess) {
        throw std::runtime_error("injected save processor failure");
    }
    return false;
}

void resetAtomicActivitySaveProcessorStub()
{
    autoProcessCalls = 0;
    throwFromAutoProcess = false;
}

void setAtomicActivitySaveProcessorFailure(bool enabled)
{
    throwFromAutoProcess = enabled;
}

int atomicActivitySaveProcessorCalls()
{
    return autoProcessCalls;
}
GSettings::GSettings(QString, QString)
    : newFormat(false),
      systemsettings(nullptr),
      oldsystemsettings(nullptr),
      global(nullptr)
{
}

GSettings::GSettings(QString, QSettings::Format)
    : newFormat(false),
      systemsettings(nullptr),
      oldsystemsettings(nullptr),
      global(nullptr)
{
}

GSettings::~GSettings() = default;

QVariant GSettings::value(const QObject *, const QString key,
                          const QVariant defaultValue)
{
    return settingsValues.value(key, defaultValue);
}

void GSettings::setValue(QString key, QVariant value)
{
    settingsValues.insert(std::move(key), std::move(value));
}

namespace {

GSettings testSettings(QStringLiteral("GoldenCheetah"),
                       QStringLiteral("AtomicActivitySaveTest"));

} // namespace

GSettings *appsettings = &testSettings;

void Estimator::refresh()
{
}
