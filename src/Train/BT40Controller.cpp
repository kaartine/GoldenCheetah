/*
 * Copyright (c) 2013 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2016 Arto Jantunen (viiru@iki.fi)
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

#include <QProgressDialog>
#include "BT40Controller.h"
#include "DeviceTypes.h"
#include "RealtimeData.h"

namespace {
constexpr int InitialScanRetryDelayMs = 2000;
constexpr int MaximumScanRetryDelayMs = 30000;
}

BT40Controller::BT40Controller(TrainSidebar *parent, DeviceConfiguration *dc) : RealtimeController(parent, dc)
{
    localDevice = new QBluetoothLocalDevice(this);
    discoveryAgent = new QBluetoothDeviceDiscoveryAgent(this);
    discoveryAgent->setLowEnergyDiscoveryTimeout(20000);
    localDc = dc;
    running = false;
    load = 0;
    gradient = 0;
    mode = RT_MODE_SLOPE;
    windSpeed = 0;
    weight = 80;
    rollingResistance = 0.0033;
    windResistance = 0.6;
    wheelSize = 2100;
    telemetryClock.start();
    appliedTelemetrySources.fill(0);
    appliedTelemetryAvailable.fill(false);

    if (localDc && !localDc->deviceProfile.isEmpty())
    {
        foreach (QString deviceInfoString, localDc->deviceProfile.split(","))
        {
            DeviceInfo deviceInfo(deviceInfoString);
            if (deviceInfo.isValid())
            {
                allowedDevices.append(DeviceInfo(deviceInfoString));
            }
        }
    }

    connect(discoveryAgent, SIGNAL(deviceDiscovered(const QBluetoothDeviceInfo&)),
	    this, SLOT(addDevice(const QBluetoothDeviceInfo&)));
    connect(discoveryAgent, SIGNAL(errorOccurred(QBluetoothDeviceDiscoveryAgent::Error)),
	    this, SLOT(deviceScanError(QBluetoothDeviceDiscoveryAgent::Error)));
    connect(discoveryAgent, SIGNAL(finished()), this, SLOT(scanFinished()));
    connect(discoveryAgent, SIGNAL(canceled()), this, SLOT(scanFinished()));

    scanRetryTimer = new QTimer(this);
    scanRetryTimer->setSingleShot(true);
    scanRetryTimer->setInterval(InitialScanRetryDelayMs);
    connect(scanRetryTimer, SIGNAL(timeout()), this, SLOT(startScan()));
    scanRetryDelayMs = InitialScanRetryDelayMs;
    missingDeviceNoticeShown = false;
}

BT40Controller::~BT40Controller()
{
    stop();
}

void
BT40Controller::setDevice(QString)
{
    // not required
}

QList<QBluetoothDeviceInfo>
BT40Controller::getDeviceInfo()
{
    QList<QBluetoothDeviceInfo> deviceInfo;
    foreach(BT40Device* dev, devices)
    {
        deviceInfo.append(dev->deviceInfo());
    }

    return deviceInfo;
}

int
BT40Controller::start()
{
    if (isHeartRateOnly() && allowedDevices.isEmpty()) {
        running = false;
        emit setNotification(tr("Bluetooth heart-rate device profile is empty"), 4);
        return -1;
    }

    telemetry = RealtimeData();
    telemetryRouter.clear();
    appliedTelemetrySources.fill(0);
    appliedTelemetryAvailable.fill(false);
    telemetryClock.restart();
    running = true;
    resetScanRetryState();
    startScan();
    return 0;
}

void
BT40Controller::startScan()
{
    if (!running || !localDevice->isValid() || discoveryAgent->isActive()) return;

    qDebug() << "Starting Bluetooth Low Energy scan";
    discoveryAgent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}


int
BT40Controller::restart()
{
    return 0;
}


int
BT40Controller::pause()
{
    return 0;
}


int
BT40Controller::stop()
{
    running = false;
    resetScanRetryState();
    if (discoveryAgent->isActive()) discoveryAgent->stop();

    const QList<BT40Device *> ownedDevices = devices;
    devices.clear();
    devicesAwaitingRediscovery.clear();

    foreach (BT40Device *device, ownedDevices) {
        if (!device) continue;
        device->disconnect(this);
        device->disconnectDevice();
        delete device;
    }

    telemetryRouter.clear();
    appliedTelemetrySources.fill(0);
    appliedTelemetryAvailable.fill(false);
    telemetry = RealtimeData();
    return 0;
}

bool
BT40Controller::find()
{
    return localDevice->isValid();
}

bool
BT40Controller::discover(QString)
{
    return true;
}


bool BT40Controller::doesPush() { return false; }
bool BT40Controller::doesPull() { return true; }
bool BT40Controller::doesLoad() { return !isHeartRateOnly(); }

bool BT40Controller::isHeartRateOnly() const
{
    return localDc && BluetoothDeviceTypes::roleForType(localDc->type) ==
            BluetoothDeviceTypes::DeviceRole::HeartRateOnly;
}

/*
 * gets called from the GUI to get updated telemetry.
 * so whilst we are at it we check button status too and
 * act accordingly.
 *
 */
void
BT40Controller::getRealtimeData(RealtimeData &rtData)
{
    refreshTelemetry();
    rtData = telemetry;
    processRealtimeData(rtData);
}

void BT40Controller::pushRealtimeData(RealtimeData &) { } // update realtime data with current values

void
BT40Controller::addDevice(const QBluetoothDeviceInfo &info)
{
    if (info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration) {
        // Check if device is already created for this uuid/address
        // At least on MacOS the deviceDiscovered signal can/will be sent multiple times
        // for the same device during discovery.
        foreach(BT40Device* dev, devices)
        {
            if (info.address().isNull())
            {
                // On MacOS there's no address, so check deviceUuid
                if (dev->deviceInfo().deviceUuid() == info.deviceUuid())
                {
                    if (devicesAwaitingRediscovery.remove(dev)) {
                        qDebug() << "Rediscovered Bluetooth device" << info.name()
                                 << info.deviceUuid();
                        dev->connectDevice();
                        if (allConfiguredDevicesFound() && discoveryAgent->isActive()) {
                            discoveryAgent->stop();
                        }
                    }
                    return;
                }
            } else {
                if (dev->deviceInfo().address() == info.address())
                {
                    if (devicesAwaitingRediscovery.remove(dev)) {
                        qDebug() << "Rediscovered Bluetooth device" << info.name()
                                 << info.address();
                        dev->connectDevice();
                        if (allConfiguredDevicesFound() && discoveryAgent->isActive()) {
                            discoveryAgent->stop();
                        }
                    }
                    return;
                }
            }
        }

        if (deviceAllowed(info))
        {
            const BluetoothDeviceTypes::DeviceRole role = localDc
                    ? BluetoothDeviceTypes::roleForType(localDc->type)
                    : BluetoothDeviceTypes::DeviceRole::Trainer;
            BT40Device* dev = new BT40Device(this, info, role);
            devices.append(dev);

            // Only connect to device if we really want
            // to use them for a workout
            if(localDc)
            {
                // When start() is called, it initiates the device scan and returns immediately.
                // Then, commands like setWeight() may come before any device is discovered.
                // In that case, the weight is stored but is sent to an empty list of devices.
                // However, when devices are added, the stored parameters are sent.
                if (!isHeartRateOnly()) {
                    dev->setWheelCircumference(wheelSize);
                    dev->setRollingResistance(rollingResistance);
                    dev->setWindResistance(windResistance);
                    dev->setWeight(weight);
                    dev->setWindSpeed(windSpeed);
                    dev->setMode(mode);
                    if (mode == RT_MODE_ERGO) dev->setLoad(load);
                    else dev->setGradient(gradient);
                }

                connect(dev, &BT40Device::setNotification, this, &BT40Controller::setNotification);
                connect(dev, &BT40Device::reconnectScanRequested,
                        this, &BT40Controller::rescanDevice);
                connect(dev, &BT40Device::connectionRestored,
                        this, &BT40Controller::deviceConnectionRestored);
                dev->connectDevice();

                if (allConfiguredDevicesFound() && discoveryAgent->isActive()) {
                    discoveryAgent->stop();
                }
            }
        }
    }
}

bool
BT40Controller::deviceAllowed(const QBluetoothDeviceInfo& info)
{
    // Check for device configuration and only
    // connect to configured sensors.
    //
    // We can still connect to all available devices
    // is the device profile is empty
    if (allowedDevices.size() == 0)
    {
        return !localDc || BluetoothDeviceTypes::permitsEmptyProfile(localDc->type);
    }

    foreach (const DeviceInfo deviceInfo, allowedDevices)
    {
        if (info.address().isNull())
        {
            // macOS
            if (info.deviceUuid().toString() == deviceInfo.getUuid())
            {
                return true;
            }
        }
        else
        {
            if (info.address().toString() == deviceInfo.getAddress())
            {
                return true;
            }
        }
    }

    return false;
}

bool
BT40Controller::allConfiguredDevicesFound() const
{
    if (!devicesAwaitingRediscovery.isEmpty()) return false;
    if (allowedDevices.isEmpty())
        return (!localDc || BluetoothDeviceTypes::permitsEmptyProfile(localDc->type)) && !devices.isEmpty();

    return devices.size() >= allowedDevices.size();
}

void
BT40Controller::resetScanRetryState()
{
    scanRetryTimer->stop();
    scanRetryDelayMs = InitialScanRetryDelayMs;
    missingDeviceNoticeShown = false;
}

void
BT40Controller::scheduleScanRetry(const QString &firstNotice)
{
    if (!running || !localDc || allConfiguredDevicesFound()) return;

    if (!missingDeviceNoticeShown) {
        emit setNotification(firstNotice, 3);
        missingDeviceNoticeShown = true;
    }

    qDebug() << "Retrying Bluetooth scan in" << scanRetryDelayMs << "ms";
    scanRetryTimer->start(scanRetryDelayMs);
    scanRetryDelayMs = qMin(scanRetryDelayMs * 2, MaximumScanRetryDelayMs);
}

void
BT40Controller::rescanDevice()
{
    BT40Device *device = qobject_cast<BT40Device*>(sender());
    if (!running || !localDc || !device || !devices.contains(device)) return;

    if (!devicesAwaitingRediscovery.contains(device)) {
        devicesAwaitingRediscovery.insert(device);
        qDebug() << "Scheduling Bluetooth rediscovery for"
                 << device->deviceInfo().name();
    }

    // BT40Device already reports the connection loss once. Keep the
    // controller's subsequent background scans quiet.
    missingDeviceNoticeShown = true;
    scanRetryTimer->stop();
    startScan();
}

void
BT40Controller::deviceConnectionRestored()
{
    BT40Device *device = qobject_cast<BT40Device*>(sender());
    if (!device || !devicesAwaitingRediscovery.remove(device)) return;

    qDebug() << "Bluetooth connection restored for"
             << device->deviceInfo().name();
    if (allConfiguredDevicesFound()) {
        resetScanRetryState();
        if (discoveryAgent->isActive()) discoveryAgent->stop();
    }
}

void
BT40Controller::scanFinished()
{
    const bool foundAnyDevices = !devices.isEmpty();

    if (!running) {
        qDebug() << "BT scan stopped";
        return;
    }

    // The pairing wizard has no configured allow-list and performs one scan.
    if (!localDc) {
        emit setNotification(tr("Bluetooth scan finished"), 2);
        emit scanFinished(foundAnyDevices);
        qDebug() << "BT scan finished with" << devices.size() << "device(s)";
        return;
    }

    if (allConfiguredDevicesFound()) {
        resetScanRetryState();
        emit setNotification(tr("All configured Bluetooth devices found"), 2);
        qDebug() << "BT scan found all" << devices.size() << "configured device(s)";
        return;
    }

    qDebug() << "BT scan found" << devices.size() << "of" << allowedDevices.size()
             << "configured device(s); retrying";
    scheduleScanRetry(tr("Some Bluetooth devices are unavailable; retrying in background"));
}


void
BT40Controller::deviceScanError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    qWarning() << "Error while scanning BT devices:" << error;
    if (running && localDc && !allConfiguredDevicesFound()) {
        scheduleScanRetry(tr("Bluetooth scan unavailable; retrying in background"));
    }
}

uint8_t
BT40Controller::getCalibrationType() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return caltype;
        }
    }
    return CALIBRATION_TYPE_NOT_SUPPORTED;
}

uint8_t
BT40Controller::getCalibrationState() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return dev->getCalibrationState();
        }
    }
    return CALIBRATION_STATE_IDLE;
}

double
BT40Controller::getCalibrationTargetSpeed() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return dev->getCalibrationTargetSpeed();
        }
    }
    return 0;
}

uint16_t
BT40Controller::getCalibrationSpindownTime() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return dev->getCalibrationSpindownTime();
        }
    }
    return 0;
}

uint16_t
BT40Controller::getCalibrationZeroOffset() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return dev->getCalibrationZeroOffset();
        }
    }
    return 0;
}

uint16_t
BT40Controller::getCalibrationSlope() {
    for (auto* dev : devices) {
        uint8_t caltype = dev->getCalibrationType();
        if (caltype != CALIBRATION_TYPE_NOT_SUPPORTED) {
            return dev->getCalibrationSlope();
        }
    }
    return 0;
}

void BT40Controller::setBPM(
        BT40Device *source, float value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::HeartRate,
                     value, priority);
}

void BT40Controller::setWatts(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::Power,
                     value, priority);
}

void BT40Controller::setWheelRpm(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::WheelRpm,
                     value, priority);
    publishTelemetry(source, BluetoothTelemetryMetric::Speed,
                     value * wheelSize / 1000.0 * 60.0 / 1000.0, priority);
}

void BT40Controller::setSpeed(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::Speed,
                     value, priority);
}

void BT40Controller::setCadence(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::Cadence,
                     value, priority);
}

void BT40Controller::setRespiratoryFrequency(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::RespiratoryFrequency,
                     value, priority);
}

void BT40Controller::setRespiratoryMinuteVolume(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source,
                     BluetoothTelemetryMetric::RespiratoryMinuteVolume,
                     value, priority);
}

void BT40Controller::setVO2_VCO2(
        BT40Device *source, double vo2, double vco2,
        BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::Vo2, vo2, priority);
    publishTelemetry(source, BluetoothTelemetryMetric::Vco2, vco2, priority);
}

void BT40Controller::setTv(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::TidalVolume,
                     value, priority);
}

void BT40Controller::setFeO2(
        BT40Device *source, double value, BluetoothTelemetryPriority priority)
{
    publishTelemetry(source, BluetoothTelemetryMetric::FeO2,
                     value, priority);
}

void BT40Controller::removeTelemetrySource(BT40Device *source)
{
    if (!source) return;
    telemetryRouter.removeSource(reinterpret_cast<quintptr>(source));
    refreshTelemetry();
}

qint64 BT40Controller::telemetryNowMs() const
{
    return telemetryClock.isValid() ? telemetryClock.elapsed() : 0;
}

void BT40Controller::publishTelemetry(
        BT40Device *source, BluetoothTelemetryMetric metric, double value,
        BluetoothTelemetryPriority priority)
{
    if (!source) return;
    const quintptr sourceId = reinterpret_cast<quintptr>(source);
    const qint64 nowMs = telemetryNowMs();
    if (!telemetryRouter.publish(sourceId, metric, value, priority, nowMs)) {
        return;
    }
    refreshTelemetryMetric(metric, nowMs, sourceId);
}

void BT40Controller::refreshTelemetry()
{
    const qint64 nowMs = telemetryNowMs();
    for (int index = 0; index < BluetoothTelemetryRouter::MetricCount; ++index) {
        refreshTelemetryMetric(
                static_cast<BluetoothTelemetryMetric>(index), nowMs);
    }
}

void BT40Controller::refreshTelemetryMetric(
        BluetoothTelemetryMetric metric, qint64 nowMs, quintptr forceSource)
{
    const int index = static_cast<int>(metric);
    if (index < 0 || index >= BluetoothTelemetryRouter::MetricCount) return;

    const BluetoothTelemetryValue resolved =
            telemetryRouter.resolve(metric, nowMs);
    const size_t stateIndex = static_cast<size_t>(index);
    const bool ownerChanged =
            appliedTelemetryAvailable[stateIndex] != resolved.available ||
            appliedTelemetrySources[stateIndex] != resolved.source;
    const bool ownerPublished =
            resolved.available && resolved.source == forceSource;
    if (!ownerChanged && !ownerPublished) return;

    const double value = resolved.available ? resolved.value : 0.0;
    switch (metric) {
    case BluetoothTelemetryMetric::HeartRate:
        telemetry.setHr(value);
        break;
    case BluetoothTelemetryMetric::Power:
        telemetry.setWatts(value);
        break;
    case BluetoothTelemetryMetric::WheelRpm:
        telemetry.setWheelRpm(value, true);
        break;
    case BluetoothTelemetryMetric::Speed:
        telemetry.setSpeed(value);
        break;
    case BluetoothTelemetryMetric::Cadence:
        telemetry.setCadence(value);
        break;
    case BluetoothTelemetryMetric::RespiratoryFrequency:
        telemetry.setRf(value);
        break;
    case BluetoothTelemetryMetric::RespiratoryMinuteVolume:
        telemetry.setRMV(value);
        break;
    case BluetoothTelemetryMetric::Vo2:
        telemetry.setVO2_VCO2(value, telemetry.getVCO2());
        break;
    case BluetoothTelemetryMetric::Vco2:
        telemetry.setVO2_VCO2(telemetry.getVO2(), value);
        break;
    case BluetoothTelemetryMetric::TidalVolume:
        telemetry.setTv(value);
        break;
    case BluetoothTelemetryMetric::FeO2:
        telemetry.setFeO2(value);
        break;
    case BluetoothTelemetryMetric::Count:
        return;
    }

    appliedTelemetryAvailable[stateIndex] = resolved.available;
    appliedTelemetrySources[stateIndex] = resolved.source;
}

void BT40Controller::setLoad(double l)
{
  load = l;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setLoad(l);
  }
}

void BT40Controller::setGradient(double g) 
{
  gradient = g;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setGradient(g);
  }
}

void BT40Controller::setMode(int m)
{
  mode = m;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setMode(m);
  }
}

void BT40Controller::setWindSpeed(double s)
{
  windSpeed = s;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setWindSpeed(s);
  }
}

void BT40Controller::setWeight(double w)
{
  weight = w;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setWeight(w);
  }
}

void BT40Controller::setRollingResistance(double rr)
{
  rollingResistance = rr;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setRollingResistance(rr);
  }
}

void BT40Controller::setWindResistance(double wr)
{
  windResistance = wr;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setWindResistance(wr);
  }
}

void BT40Controller::setWheelCircumference(double wc)
{
  wheelSize = wc;
  if (isHeartRateOnly()) return;
  for (auto* dev: devices) {
    dev->setWheelCircumference(wc);
  }
}

DeviceInfo::DeviceInfo(QString data)
{
    QStringList deviceInfo = data.split(";");
    if (deviceInfo.size() == 3)
    {
        name = deviceInfo[0];
        address = deviceInfo[1];
        uuid = deviceInfo[2];
    }
}

DeviceInfo::DeviceInfo(QString name, QString address, QString uuid)
    : name(name), address(address), uuid(uuid)
{
}

QString DeviceInfo::getName() const
{
    return name;
}

QString DeviceInfo::getUuid() const
{
    return uuid;
}

QString DeviceInfo::getAddress() const
{
    return address;
}

bool DeviceInfo::isValid() const
{
    // Linux and Windows will report an address and macOS will report an uuid.
    // We can still check for empty values, because we save
    // 00:00:00:00:00:00 or {00000000-0000-0000-0000-000000000000}
    // for unavailable identifier.
    // This also means, the allow list is not portable. Users have to create
    // profiles for Windows/Linux or macOS.
    return !name.isEmpty() && !address.isEmpty() && !uuid.isEmpty();
}
