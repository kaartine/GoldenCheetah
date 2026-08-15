/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/AddDeviceWizard.h"
#include "Train/BT40Controller.h"
#include "Train/BT40Device.h"
#include "Train/KurtSmartControl.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <thread>

namespace {

QBluetoothDeviceInfo lifecycleDeviceInfo()
{
    QBluetoothDeviceInfo info(QBluetoothAddress(quint64(1)),
                              QStringLiteral("Lifecycle test device"), 0);
    info.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    return info;
}

QBluetoothDeviceInfo lifecycleUuidDeviceInfo()
{
    QBluetoothDeviceInfo info(
            QBluetoothUuid(
                    QStringLiteral(
                            "{11111111-2222-3333-4444-555555555555}")),
            QStringLiteral("Lifecycle UUID test device"), 0);
    info.setCoreConfigurations(
            QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    return info;
}

BT40Device *addControllerDevice(BT40Controller *controller)
{
    const QBluetoothDeviceInfo info = lifecycleDeviceInfo();
    const bool invoked = QMetaObject::invokeMethod(
            controller, "addDevice", Qt::DirectConnection,
            Q_ARG(QBluetoothDeviceInfo, info));
    if (!invoked) return nullptr;

    const QList<BT40Device *> devices =
            controller->findChildren<BT40Device *>(
                    QString(), Qt::FindDirectChildrenOnly);
    return devices.isEmpty() ? nullptr : devices.first();
}

BT40Device *createDevice(
        BT40Controller *controller,
        BluetoothDeviceTypes::DeviceRole role =
                BluetoothDeviceTypes::DeviceRole::Trainer)
{
    return new BT40Device(controller, lifecycleDeviceInfo(), role);
}

class ControllerStopGuard final
{
public:
    explicit ControllerStopGuard(BT40Controller *controller) :
        controller(controller)
    {
    }

    ~ControllerStopGuard()
    {
        if (!controller) return;

        for (BT40Device *device :
             controller->findChildren<BT40Device *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            for (QLowEnergyController *link :
                 device->findChildren<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly)) {
                link->setStateForTest(
                        QLowEnergyController::UnconnectedState);
            }
        }
        controller->stop();
    }

private:
    BT40Controller *controller;
};

QLowEnergyService *findService(
        BT40Device *device, const QBluetoothUuid &uuid)
{
    for (QLowEnergyService *service :
         device->findChildren<QLowEnergyService *>()) {
        if (service->serviceUuid() == uuid) return service;
    }
    return nullptr;
}

struct ScanThreadRecord {
    QThread *createdOn = nullptr;
    QThread *destroyedOn = nullptr;
    std::atomic_bool entered{false};
};

class ScanOwnedController : public QObject
{
public:
    explicit ScanOwnedController(ScanThreadRecord *record) : record(record)
    {
        record->createdOn = QThread::currentThread();
        record->entered.store(true, std::memory_order_release);
    }

    ~ScanOwnedController() override
    {
        record->destroyedOn = QThread::currentThread();
    }

private:
    ScanThreadRecord *record;
};

class RecordingDeviceScanThread : public DeviceScanThread
{
public:
    explicit RecordingDeviceScanThread(ScanFunction scanFunction) :
        DeviceScanThread(std::move(scanFunction))
    {
    }

    ~RecordingDeviceScanThread() override
    {
        stopAndWait();
    }

    std::atomic_int completions{0};
    DeviceScanResult completedResult;

protected:
    void resultReady(const DeviceScanResult &result) override
    {
        completedResult = result;
        completions.fetch_add(1, std::memory_order_release);
    }
};

}

class TestBt40Lifecycle : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QBluetoothLocalDevice::resetTestState();
        QBluetoothDeviceDiscoveryAgent::resetTestState();
        QLowEnergyController::resetTestCounters();
        QLowEnergyService::resetTestCounters();
    }

    void cleanup()
    {
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(QBluetoothLocalDevice::liveDeviceCount(), 0);
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::liveAgentCount(), 0);
        QCOMPARE(QLowEnergyController::liveControllerCount(), 0);
    }

    void rejectsEmptyHeartRateProfileBeforeScanning()
    {
        DeviceConfiguration config;
        config.type = DEV_BT40_HEARTRATE;
        config.deviceProfile.clear();

        BT40Controller controller(nullptr, &config);
        QSignalSpy notificationSpy(
                &controller, &RealtimeController::setNotification);

        QVERIFY(!controller.doesLoad());
        QCOMPARE(controller.start(), -1);
        QCOMPARE(notificationSpy.count(), 1);
        QVERIFY(!AddDeviceWizard::isDeviceProfileValid(
                DEV_BT40_HEARTRATE, QString()));
        QVERIFY(!AddDeviceWizard::isDeviceProfileValid(
                DEV_BT40_HEARTRATE, QStringLiteral("   ")));
        QVERIFY(AddDeviceWizard::isDeviceProfileValid(
                DEV_BT40_HEARTRATE,
                QStringLiteral("sensor-profile")));
        QVERIFY(AddDeviceWizard::isDeviceProfileValid(DEV_BT40, QString()));
    }

    void classifiesServicesUsingProductionUuidMapping()
    {
        using namespace BluetoothDeviceTypes;

        const QBluetoothUuid heartRate(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        const QBluetoothUuid cyclingPower(
                QBluetoothUuid::ServiceClassUuid::CyclingPower);
        const QBluetoothUuid cadence(
                QBluetoothUuid::ServiceClassUuid::CyclingSpeedAndCadence);

        QCOMPARE(static_cast<int>(BT40Device::classifyService(heartRate)),
                 static_cast<int>(ServiceKind::HeartRate));
        QCOMPARE(static_cast<int>(BT40Device::classifyService(cyclingPower)),
                 static_cast<int>(ServiceKind::TrainerControl));
        QCOMPARE(static_cast<int>(BT40Device::classifyService(cadence)),
                 static_cast<int>(ServiceKind::OtherTelemetry));
        QVERIFY(BT40Device::acceptsServiceForRole(
                DeviceRole::HeartRateOnly, heartRate));
        QVERIFY(!BT40Device::acceptsServiceForRole(
                DeviceRole::HeartRateOnly, cyclingPower));
        QVERIFY(!BT40Device::acceptsServiceForRole(
                DeviceRole::HeartRateOnly, cadence));
        QVERIFY(BT40Device::acceptsServiceForRole(
                DeviceRole::Trainer, cyclingPower));
    }

    void serviceInitializationRunsOncePerService_data()
    {
        QTest::addColumn<bool>("heartRateFirst");
        QTest::newRow("heart-rate-first") << true;
        QTest::newRow("cadence-first") << false;
    }

    void serviceInitializationRunsOncePerService()
    {
        QFETCH(bool, heartRateFirst);

        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        const QBluetoothUuid cadenceService(
                QBluetoothUuid::ServiceClassUuid::CyclingSpeedAndCadence);
        const QBluetoothUuid heartRateMeasurement(
                QBluetoothUuid::CharacteristicType::HeartRateMeasurement);
        const QBluetoothUuid cadenceMeasurement(
                QBluetoothUuid::CharacteristicType::CSCMeasurement);

        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitServiceDiscoveredForTest(cadenceService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyService *heartRate = findService(device, heartRateService);
        QLowEnergyService *cadence = findService(device, cadenceService);
        QVERIFY(heartRate);
        QVERIFY(cadence);
        QCOMPARE(heartRate->discoverCallCount(), 1);
        QCOMPARE(cadence->discoverCallCount(), 1);

        if (heartRateFirst) {
            heartRate->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
            cadence->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
        } else {
            cadence->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
            heartRate->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
        }

        QCOMPARE(heartRate->characteristicLookupCount(heartRateMeasurement), 1);
        QCOMPARE(cadence->characteristicLookupCount(cadenceMeasurement), 1);

        heartRate->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        cadence->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);

        QCOMPARE(heartRate->characteristicLookupCount(heartRateMeasurement), 1);
        QCOMPARE(cadence->characteristicLookupCount(cadenceMeasurement), 1);

        QPointer<QLowEnergyService> oldHeartRate = heartRate;
        QPointer<QLowEnergyService> oldCadence = cadence;
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(oldHeartRate.isNull());
        QVERIFY(oldCadence.isNull());

        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitServiceDiscoveredForTest(cadenceService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        heartRate = findService(device, heartRateService);
        cadence = findService(device, cadenceService);
        QVERIFY(heartRate);
        QVERIFY(cadence);
        heartRate->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        cadence->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        QCOMPARE(heartRate->characteristicLookupCount(heartRateMeasurement), 1);
        QCOMPARE(cadence->characteristicLookupCount(cadenceMeasurement), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void controllerAndBluetoothChildrenShareThreadAffinity()
    {
        BT40Controller controller(nullptr, nullptr);
        QCOMPARE(controller.thread(), QThread::currentThread());

        for (QObject *child : controller.children()) {
            QCOMPARE(child->thread(), controller.thread());
        }

        BT40Device *device = addControllerDevice(&controller);
        QVERIFY(device);
        QCOMPARE(device->thread(), controller.thread());
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QCOMPARE(link->thread(), controller.thread());
    }

    void controllerRoutesTelemetryByPhysicalSource()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *trainer = createDevice(&controller);
        BT40Device *powerMeter = createDevice(&controller);

        controller.setWatts(
                trainer, 240.0, BluetoothTelemetryPriority::Trainer);
        controller.setCadence(
                trainer, 86.0, BluetoothTelemetryPriority::Trainer);
        controller.setWatts(
                powerMeter, 225.0,
                BluetoothTelemetryPriority::DedicatedSensor);
        controller.setWatts(
                trainer, 260.0, BluetoothTelemetryPriority::Trainer);

        RealtimeData data;
        controller.getRealtimeData(data);
        QCOMPARE(data.getWatts(), 225.0);
        QCOMPARE(data.getCadence(), 86.0);

        controller.removeTelemetrySource(powerMeter);
        controller.getRealtimeData(data);
        QCOMPARE(data.getWatts(), 260.0);
        QCOMPARE(data.getCadence(), 86.0);

        delete powerMeter;
        delete trainer;
    }

    void controllerRoutesAndClearsEveryTelemetryMetric()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *sensor = createDevice(&controller);
        const BluetoothTelemetryPriority priority =
                BluetoothTelemetryPriority::DedicatedSensor;

        controller.setBPM(sensor, 147.0f, priority);
        controller.setWatts(sensor, 231.0, priority);
        controller.setWheelRpm(sensor, 120.0, priority);
        controller.setCadence(sensor, 89.0, priority);
        controller.setRespiratoryFrequency(sensor, 32.5, priority);
        controller.setRespiratoryMinuteVolume(sensor, 74.25, priority);
        controller.setVO2_VCO2(sensor, 3150.0, 2780.0, priority);
        controller.setTv(sensor, 2.15, priority);
        controller.setFeO2(sensor, 16.8, priority);

        RealtimeData data;
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 147.0);
        QCOMPARE(data.getWatts(), 231.0);
        QCOMPARE(data.getWheelRpm(), 120.0);
        QCOMPARE(data.getSpeed(), 15.12);
        QCOMPARE(data.getCadence(), 89.0);
        QCOMPARE(data.getRf(), 32.5);
        QCOMPARE(data.getRMV(), 74.25);
        QCOMPARE(data.getVO2(), 3150.0);
        QCOMPARE(data.getVCO2(), 2780.0);
        QCOMPARE(data.getTv(), 2.15);
        QCOMPARE(data.getFeO2(), 16.8);

        controller.setSpeed(sensor, 34.5, priority);
        controller.getRealtimeData(data);
        QCOMPARE(data.getSpeed(), 34.5);

        controller.removeTelemetrySource(sensor);
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 0.0);
        QCOMPARE(data.getWatts(), 0.0);
        QCOMPARE(data.getWheelRpm(), 0.0);
        QCOMPARE(data.getSpeed(), 0.0);
        QCOMPARE(data.getCadence(), 0.0);
        QCOMPARE(data.getRf(), 0.0);
        QCOMPARE(data.getRMV(), 0.0);
        QCOMPARE(data.getVO2(), 0.0);
        QCOMPARE(data.getVCO2(), 0.0);
        QCOMPARE(data.getTv(), 0.0);
        QCOMPARE(data.getFeO2(), 0.0);

        delete sensor;
    }

    void scannerCancellationJoinsAndKeepsWorkerOwnership()
    {
        ScanThreadRecord record;
        RecordingDeviceScanThread scanner(
                [&record](const DeviceScanRequest &request,
                          const DeviceScanThread::CancellationCheck &cancelled) {
                    ScanOwnedController controller(&record);
                    if (request.type == 1) {
                        while (!cancelled()) QThread::msleep(1);
                    }

                    DeviceScanResult result;
                    result.found = true;
                    return result;
                });

        DeviceScanRequest firstRequest;
        firstRequest.type = 1;
        const quint64 firstGeneration = scanner.startScan(firstRequest);
        QVERIFY(firstGeneration > 0);
        QTRY_VERIFY(record.entered.load(std::memory_order_acquire));

        scanner.stopAndWait();

        QVERIFY(!scanner.isRunning());
        QCOMPARE(scanner.completions.load(std::memory_order_acquire), 0);
        QVERIFY(record.createdOn);
        QCOMPARE(record.createdOn, record.destroyedOn);
        QVERIFY(record.createdOn != QThread::currentThread());

        record.entered.store(false, std::memory_order_release);
        DeviceScanRequest secondRequest;
        secondRequest.type = 2;
        const quint64 secondGeneration = scanner.startScan(secondRequest);
        QVERIFY(secondGeneration > firstGeneration);
        QVERIFY(scanner.wait(1000));

        QCOMPARE(scanner.completions.load(std::memory_order_acquire), 1);
        QVERIFY(scanner.completedResult.found);
        QVERIFY(!scanner.completedResult.cancelled);
        QCOMPARE(scanner.completedResult.generation, secondGeneration);
        QVERIFY(record.entered.load(std::memory_order_acquire));
        QCOMPARE(record.createdOn, record.destroyedOn);
    }

    void controllerDestructionWithoutStopDestroysDevices()
    {
        BT40Controller *controller = new BT40Controller(nullptr, nullptr);
        BT40Device *rawDevice = addControllerDevice(controller);
        QCOMPARE(controller->getDeviceInfo().size(), 1);
        QVERIFY(rawDevice);
        QPointer<BT40Device> device = rawDevice;

        delete controller;

        QVERIFY(device.isNull());
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void stopIsIdempotent()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = addControllerDevice(&controller);
        QVERIFY(device);

        QCOMPARE(controller.stop(), 0);
        QCOMPARE(QLowEnergyController::destructionCount(), 1);

        QCOMPARE(controller.stop(), 0);
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void teardownDestroysUnconnectedLinkWithoutDisconnect()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QVERIFY(device);

        delete device;

        QCOMPARE(QLowEnergyController::disconnectCallCount(), 0);
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void teardownDefersActiveLinkUntilDisconnected()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QPointer<QLowEnergyController> link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        link->setStateForTest(QLowEnergyController::ConnectedState);

        device->disconnectDevice();
        delete device;

        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);
        QCOMPARE(QLowEnergyController::destructionCount(), 0);
        QVERIFY(!link.isNull());

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        QVERIFY(link.isNull());
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void teardownDefersClosingLinkWithoutDuplicateDisconnect()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QPointer<QLowEnergyController> link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        link->setStateForTest(QLowEnergyController::ClosingState);

        device->disconnectDevice();
        delete device;

        QCOMPARE(QLowEnergyController::disconnectCallCount(), 0);
        QCOMPARE(QLowEnergyController::destructionCount(), 0);
        QVERIFY(!link.isNull());

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        QVERIFY(link.isNull());
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void reconnectRunsOnlyFromUnconnectedState()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);

        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);

        link->setStateForTest(QLowEnergyController::ClosingState);
        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::connectCallCount(), 2);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void unexpectedLossRequestsRediscoveryAndReconnects()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);
        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);

        device->connectDevice();
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(rediscoverySpy.count(), 1);
        QCOMPARE(QLowEnergyController::connectCallCount(), 2);

        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(restoredSpy.count(), 1);
        QCOMPARE(QLowEnergyController::discoverCallCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void heartRateLinkIsNotRestoredUntilStreamReady()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(restoredSpy.count(), 0);
        QCOMPARE(QLowEnergyController::discoverCallCount(), 1);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QCOMPARE(restoredSpy.count(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void validHeartRateNotificationRestoresAndRoutesTelemetry()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        const QBluetoothUuid heartRateMeasurement(
                QBluetoothUuid::CharacteristicType::
                        HeartRateMeasurement);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyService *service =
                findService(device, heartRateService);
        QVERIFY(service);
        service->setCharacteristicForTest(heartRateMeasurement);
        service->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        QCOMPARE(service->descriptorWriteCount(), 1);

        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);
        service->emitCharacteristicChangedForTest(
                heartRateMeasurement, QByteArray::fromHex("0096"));

        RealtimeData data;
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 150.0);
        QCOMPARE(restoredSpy.count(), 1);

        service->emitCharacteristicChangedForTest(
                heartRateMeasurement, QByteArray::fromHex("019700"));
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 151.0);
        QCOMPARE(restoredSpy.count(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void suspendedAdapterIgnoresGattCallbacks()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        const QBluetoothUuid heartRateMeasurement(
                QBluetoothUuid::CharacteristicType::
                        HeartRateMeasurement);

        device->connectDevice();
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyService *service =
                findService(device, heartRateService);
        QVERIFY(service);
        service->setCharacteristicForTest(heartRateMeasurement);
        service->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        const int initialServiceCount =
                device->findChildren<QLowEnergyService *>().size();
        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);

        device->suspendForAdapterReset();
        service->emitStateChangedForTest(
                QLowEnergyService::InvalidService);
        service->emitErrorForTest(
                QLowEnergyService::DescriptorWriteError);
        service->emitCharacteristicChangedForTest(
                heartRateMeasurement, QByteArray::fromHex("0096"));
        link->emitServiceDiscoveredForTest(
                QBluetoothUuid(
                        QBluetoothUuid::ServiceClassUuid::
                                CyclingPower));
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady",
                Qt::DirectConnection));

        RealtimeData data;
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 0.0);
        QCOMPARE(restoredSpy.count(), 0);
        QCOMPARE(device->findChildren<QLowEnergyService *>().size(),
                 initialServiceCount);
        for (QTimer *timer :
             device->findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            QVERIFY(!timer->isActive());
        }

        device->disconnectDevice();
        link->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void queuedNotificationFromDisconnectedServiceIsIgnored()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        const QBluetoothUuid heartRateMeasurement(
                QBluetoothUuid::CharacteristicType::
                        HeartRateMeasurement);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QPointer<QLowEnergyService> service =
                findService(device, heartRateService);
        QVERIFY(service);
        service->setCharacteristicForTest(heartRateMeasurement);
        service->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        service->emitCharacteristicChangedForTest(
                heartRateMeasurement, QByteArray::fromHex("0096"));

        qRegisterMetaType<QLowEnergyCharacteristic>(
                "QLowEnergyCharacteristic");
        const bool notificationDisconnected = QObject::disconnect(
                service,
                SIGNAL(characteristicChanged(QLowEnergyCharacteristic,QByteArray)),
                device,
                SLOT(updateValue(QLowEnergyCharacteristic,QByteArray)));
        const bool queuedNotificationConnected = QObject::connect(
                service,
                SIGNAL(characteristicChanged(QLowEnergyCharacteristic,QByteArray)),
                device,
                SLOT(updateValue(QLowEnergyCharacteristic,QByteArray)),
                Qt::QueuedConnection);

        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        service->emitCharacteristicChangedForTest(
                heartRateMeasurement, QByteArray::fromHex("0097"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        RealtimeData data;
        controller.getRealtimeData(data);
        QCOMPARE(data.getHr(), 0.0);
        QCOMPARE(restoredSpy.count(), 0);
        QVERIFY(service.isNull());

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
        QVERIFY(notificationDisconnected);
        QVERIFY(queuedNotificationConnected);
    }

    void heartRateSilenceForcesSingleRecovery()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));

        QLowEnergyController::resetTestCounters();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));
        QCOMPARE(rediscoverySpy.count(), 1);
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));
        QCOMPARE(rediscoverySpy.count(), 1);
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void staleHeartRateSampleCannotCancelRecovery()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));

        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QCOMPARE(restoredSpy.count(), 0);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void heartRateReadyAfterDisconnectCannotRestoreConnectingLink()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));

        QSignalSpy restoredSpy(device, &BT40Device::connectionRestored);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(link->state(), QLowEnergyController::ConnectingState);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QCOMPARE(restoredSpy.count(), 0);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void manualHeartRateDisconnectCancelsWatchdog()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));

        device->disconnectDevice();
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));
        QCOMPARE(rediscoverySpy.count(), 0);
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void heartRateGattFailureStartsRecovery_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("invalid-service")
                << 0;
        QTest::newRow("missing-heart-rate-characteristic")
                << 1;
        QTest::newRow("descriptor-write-error")
                << 2;
    }

    void heartRateGattFailureStartsRecovery()
    {
        QFETCH(int, failure);

        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyService *service =
                findService(device, heartRateService);
        QVERIFY(service);
        QLowEnergyController::resetTestCounters();

        if (failure == 0) {
            service->emitStateChangedForTest(
                    QLowEnergyService::InvalidService);
        } else if (failure == 1) {
            service->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
        } else {
            service->emitErrorForTest(
                    QLowEnergyService::DescriptorWriteError);
        }

        QCOMPARE(rediscoverySpy.count(), 1);
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void activeHeartRateReconnectForcesTeardown()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);
        link->setStateForTest(QLowEnergyController::ConnectingState);
        link->emitErrorForTest(
                QLowEnergyController::ConnectionError);

        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void stuckHeartRateLinkGetsFreshController()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);

        device->connectDevice();
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);
        staleLink->setStateForTest(
                QLowEnergyController::ConnectingState);
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);

        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }

        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(QLowEnergyController::connectCallCount(), 2);
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(freshLink);
        QVERIFY(freshLink != staleLink);
        QVERIFY(staleLink.isNull());

        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void repeatedUnconnectedFailuresReplaceHeartRateController()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> initialLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(initialLink);

        device->connectDevice();
        for (int failure = 0; failure < 3; ++failure) {
            QLowEnergyController *link =
                    device->findChild<QLowEnergyController *>(
                            QString(), Qt::FindDirectChildrenOnly);
            QVERIFY(link);
            link->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            link->emitDisconnectedForTest();
            QCoreApplication::sendPostedEvents(
                    nullptr, QEvent::MetaCall);
            QCoreApplication::processEvents();
        }

        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        QVERIFY(freshLink != initialLink);
        QCOMPARE(QLowEnergyController::connectCallCount(), 4);

        if (!initialLink.isNull()) {
            initialLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            initialLink->emitDisconnectedForTest();
            QCoreApplication::sendPostedEvents(
                    nullptr, QEvent::DeferredDelete);
            QVERIFY(initialLink.isNull());
        }

        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void queuedConnectedSignalCannotCancelHeartRateRecovery()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        link->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);
        QCOMPARE(QLowEnergyController::discoverCallCount(), 0);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void queuedDisconnectIsProcessedBeforeRediscoveryReconnect()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40_HEARTRATE;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        BT40Device *device = addControllerDevice(&controller);
        QVERIFY(device);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);
        link->emitServiceDiscoveredForTest(heartRateService);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QPointer<QLowEnergyService> staleService =
                findService(device, heartRateService);
        QVERIFY(staleService);
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateWatchdogExpired", Qt::DirectConnection));

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QBluetoothDeviceInfo refreshed(
                original.address(),
                QStringLiteral("Rediscovered before disconnect callback"), 0);
        refreshed.setCoreConfigurations(
                QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "addDevice", Qt::DirectConnection,
                Q_ARG(QBluetoothDeviceInfo, refreshed)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        const bool staleServiceWasCleared = staleService.isNull();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);
        QVERIFY(staleServiceWasCleared);
    }

    void slowHeartRateConnectionIsNotCancelledAtFirstRetryTick()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(link->state(), QLowEnergyController::ConnectingState);

        const int disconnects =
                QLowEnergyController::disconnectCallCount();
        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        QCOMPARE(QLowEnergyController::disconnectCallCount(),
                 disconnects);
        QCOMPARE(link->state(), QLowEnergyController::ConnectingState);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void stuckHeartRateControllerHasBoundedRetirement()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);

        device->connectDevice();
        staleLink->setStateForTest(
                QLowEnergyController::ConnectingState);
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }

        QCOMPARE(device->findChildren<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 0);
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(staleLink.isNull());

        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void replacementNeverConnectsOverlappingController()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);

        device->connectDevice();
        staleLink->setStateForTest(
                QLowEnergyController::ConnectingState);
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }
        const int overlappingConnects =
                QLowEnergyController::overlappingConnectCallCount();

        if (!staleLink.isNull()) {
            staleLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            staleLink->emitDisconnectedForTest();
        }
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;

        QCOMPARE(overlappingConnects, 0);
    }

    void manualDisconnectCancelsPendingControllerReplacement()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);
        QSignalSpy rediscoveryCancellationSpy(
                device, &BT40Device::reconnectScanCancelled);

        device->connectDevice();
        staleLink->setStateForTest(
                QLowEnergyController::ConnectingState);
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }
        QCOMPARE(device->findChildren<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 0);

        device->disconnectDevice();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QVERIFY(staleLink.isNull());
        QCOMPARE(device->findChildren<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 0);
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);
        QCOMPARE(rediscoveryCancellationSpy.count(), 1);
        delete device;
    }

    void destructionCancelsPendingControllerReplacement()
    {
        BT40Controller controller(nullptr, nullptr);
        QPointer<BT40Device> device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);

        device->connectDevice();
        staleLink->setStateForTest(
                QLowEnergyController::ConnectingState);
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }

        delete device.data();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QVERIFY(device.isNull());
        QVERIFY(staleLink.isNull());
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);
    }

    void queuedStaleDisconnectCannotAffectReplacement()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> staleLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(staleLink);

        device->connectDevice();
        staleLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        staleLink->emitDisconnectedForTest();
        staleLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        for (int attempt = 0; attempt < 2; ++attempt) {
            staleLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect",
                    Qt::DirectConnection));
        }
        QCOMPARE(device->findChildren<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 0);

        delete staleLink.data();
        QVERIFY(staleLink.isNull());
        QVERIFY(QMetaObject::invokeMethod(
                device, "completeLowEnergyControllerReplacement",
                Qt::DirectConnection));
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        const int connectCalls =
                QLowEnergyController::connectCallCount();

        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(device->findChild<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly),
                 freshLink);
        QCOMPARE(freshLink->state(),
                 QLowEnergyController::ConnectingState);
        QCOMPARE(QLowEnergyController::connectCallCount(),
                 connectCalls);

        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void directErrorsNeverDeleteTheirSignalSenderSynchronously()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> initialLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(initialLink);

        device->connectDevice();
        for (int failure = 0; failure < 3; ++failure) {
            QVERIFY(initialLink);
            initialLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            initialLink->emitErrorForTest(
                    QLowEnergyController::ConnectionError);
        }
        const int synchronousDestructions =
                QLowEnergyController::
                        synchronousErrorDestructionCount();

        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;

        QCOMPARE(synchronousDestructions, 0);
    }

    void adapterResetRestoreDefersReconnectUntilDiscovery_data()
    {
        QTest::addColumn<int>("deviceType");
        QTest::addColumn<bool>("uuidIdentity");

        QTest::newRow("trainer-address") << DEV_BT40 << false;
        QTest::newRow("heart-rate-address")
                << DEV_BT40_HEARTRATE << false;
        QTest::newRow("trainer-uuid") << DEV_BT40 << true;
        QTest::newRow("heart-rate-uuid")
                << DEV_BT40_HEARTRATE << true;
    }

    void adapterResetRestoreDefersReconnectUntilDiscovery()
    {
        QFETCH(int, deviceType);
        QFETCH(bool, uuidIdentity);

        const QBluetoothDeviceInfo original =
                uuidIdentity
                        ? lifecycleUuidDeviceInfo()
                        : lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = deviceType;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QPointer<QBluetoothDeviceDiscoveryAgent> discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        QPointer<QBluetoothLocalDevice> localDevice =
                controller.findChild<QBluetoothLocalDevice *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(localDevice);

        QCOMPARE(controller.start(), 0);
        QVERIFY(discovery->isActive());
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::startCallCount(), 1);

        discovery->emitDeviceDiscoveredForTest(original);
        const QList<BT40Device *> devices =
                controller.findChildren<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(devices.size(), 1);
        BT40Device *device = devices.first();
        QLowEnergyController *initialLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(initialLink);
        QVERIFY(!discovery->isActive());

        initialLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        if (deviceType == DEV_BT40_HEARTRATE) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "heartRateStreamReady",
                    Qt::DirectConnection));
        }

        const QList<QTimer *> controllerTimers =
                controller.findChildren<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(controllerTimers.size(), 1);
        QTimer *scanRetryTimer = controllerTimers.first();
        QVERIFY(!scanRetryTimer->isActive());

        QSignalSpy restoredSpy(
                device, &BT40Device::connectionRestored);
        const int initialScanStarts =
                QBluetoothDeviceDiscoveryAgent::startCallCount();
        const int initialConnectCalls =
                QLowEnergyController::connectCallCount();
        QPointer<QLowEnergyController> staleLink(initialLink);

        initialLink->emitConnectedForTest();
        QBluetoothLocalDevice::setAdapterValidForTest(false);
        initialLink->emitErrorForTest(
                QLowEnergyController::InvalidBluetoothAdapterError);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QVERIFY2(scanRetryTimer->isActive(),
                 "adapter-invalid recovery must leave a bounded scan retry armed");
        QCOMPARE(restoredSpy.count(), 0);
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::startCallCount(),
                 initialScanStarts);
        for (QTimer *timer :
             device->findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            QVERIFY2(!timer->isActive(),
                     "stale connected callbacks must not rearm device timers");
        }
        if (deviceType == DEV_BT40_HEARTRATE) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "heartRateStreamReady",
                    Qt::DirectConnection));
            QCOMPARE(restoredSpy.count(), 0);
        }

        initialLink->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        initialLink->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        for (QTimer *timer :
             device->findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            QVERIFY2(!timer->isActive(),
                     "queued disconnects must not undo adapter suspension");
        }
        QCOMPARE(QLowEnergyController::connectCallCount(),
                 initialConnectCalls);
        QCOMPARE(restoredSpy.count(), 0);

        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));
        QVERIFY2(scanRetryTimer->isActive(),
                 "an adapter-invalid retry must rearm itself");
        QVERIFY2(discovery.isNull(),
                 "adapter recovery must discard the stale discovery agent");
        QVERIFY2(localDevice.isNull(),
                 "adapter recovery must discard the stale local adapter");
        QPointer<QBluetoothDeviceDiscoveryAgent> invalidAttemptDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QPointer<QBluetoothLocalDevice> invalidAttemptLocalDevice =
                controller.findChild<QBluetoothLocalDevice *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(invalidAttemptDiscovery);
        QVERIFY(invalidAttemptLocalDevice);
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::liveAgentCount(), 1);
        QCOMPARE(QBluetoothLocalDevice::liveDeviceCount(), 1);
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::startCallCount(),
                 initialScanStarts);
        QCOMPARE(device->findChild<QLowEnergyController *>(
                         QString(), Qt::FindDirectChildrenOnly),
                 initialLink);

        QBluetoothLocalDevice::setAdapterValidForTest(true);
        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));

        QBluetoothDeviceDiscoveryAgent *restoredDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(restoredDiscovery);
        QVERIFY2(invalidAttemptDiscovery.isNull(),
                 "each adapter retry must use a fresh discovery agent");
        QVERIFY2(invalidAttemptLocalDevice.isNull(),
                 "each adapter retry must use a fresh local adapter");
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::liveAgentCount(), 1);
        QCOMPARE(QBluetoothLocalDevice::liveDeviceCount(), 1);
        QVERIFY(restoredDiscovery->isActive());
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::startCallCount(),
                 initialScanStarts + 1);

        restoredDiscovery->emitDeviceDiscoveredForTest(original);
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        QVERIFY(freshLink != staleLink.data());
        QVERIFY(staleLink.isNull());
        QCOMPARE(QLowEnergyController::connectCallCount(),
                 initialConnectCalls + 1);
        QCOMPARE(QLowEnergyController::overlappingConnectCallCount(), 0);
        QCOMPARE(QLowEnergyController::lastRemoteAddressType(),
                 QLowEnergyController::RandomAddress);

        QTimer *reconnectTimer = nullptr;
        for (QTimer *timer :
             device->findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            if (!timer->isSingleShot()) {
                reconnectTimer = timer;
                break;
            }
        }
        QVERIFY(reconnectTimer);
        QVERIFY2(reconnectTimer->isActive(),
                 "post-reset links need a bounded connection watchdog");

        QPointer<QLowEnergyController> stalledLink(freshLink);
        for (int tick = 0; tick < 3; ++tick) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect", Qt::DirectConnection));
        }
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        freshLink = device->findChild<QLowEnergyController *>(
                QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        QVERIFY(stalledLink.isNull());
        QCOMPARE(QLowEnergyController::connectCallCount(),
                 initialConnectCalls + 2);
        QCOMPARE(QLowEnergyController::overlappingConnectCallCount(), 0);
        QCOMPARE(QLowEnergyController::lastRemoteAddressType(),
                 QLowEnergyController::RandomAddress);

        freshLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        if (deviceType == DEV_BT40_HEARTRATE) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "heartRateStreamReady",
                    Qt::DirectConnection));
        } else {
            const QBluetoothUuid cyclingPowerService(
                    QBluetoothUuid::ServiceClassUuid::CyclingPower);
            const QBluetoothUuid cyclingPowerMeasurement(
                    QBluetoothUuid::CharacteristicType::
                            CyclingPowerMeasurement);
            freshLink->emitServiceDiscoveredForTest(
                    cyclingPowerService);
            freshLink->emitDiscoveryFinishedForTest();
            QCoreApplication::sendPostedEvents(
                    nullptr, QEvent::MetaCall);
            QCoreApplication::processEvents();
            QLowEnergyService *service =
                    findService(device, cyclingPowerService);
            QVERIFY(service);
            service->setCharacteristicForTest(
                    cyclingPowerMeasurement);
            service->emitStateChangedForTest(
                    QLowEnergyService::RemoteServiceDiscovered);
        }

        QCOMPARE(restoredSpy.count(), 1);
        QVERIFY(!restoredDiscovery->isActive());

        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);
    }

    void trainerAdapterRecoveryWaitsForGattReadiness()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QPointer<QLowEnergyController> initialLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(initialLink);

        device->connectDevice();
        initialLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QSignalSpy restoredSpy(
                device, &BT40Device::connectionRestored);

        QVERIFY(device->suspendForAdapterReset());
        device->resumeAfterAdapterReset();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(initialLink.isNull());

        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        freshLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QTimer *reconnectTimer = nullptr;
        for (QTimer *timer :
             device->findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            if (!timer->isSingleShot()) {
                reconnectTimer = timer;
                break;
            }
        }
        QVERIFY(reconnectTimer);
        QCOMPARE(restoredSpy.count(), 0);
        QVERIFY2(reconnectTimer->isActive(),
                 "trainer recovery needs a watchdog until GATT is ready");

        freshLink->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QPointer<QLowEnergyController> stalledLink(freshLink);
        for (int tick = 0; tick < 3; ++tick) {
            QVERIFY(QMetaObject::invokeMethod(
                    device, "attemptReconnect", Qt::DirectConnection));
        }
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        freshLink = device->findChild<QLowEnergyController *>(
                QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        QVERIFY(stalledLink.isNull());
        freshLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(restoredSpy.count(), 0);
        QVERIFY(reconnectTimer->isActive());

        const QBluetoothUuid cyclingPowerService(
                QBluetoothUuid::ServiceClassUuid::CyclingPower);
        const QBluetoothUuid cyclingPowerMeasurement(
                QBluetoothUuid::CharacteristicType::
                        CyclingPowerMeasurement);
        freshLink->emitServiceDiscoveredForTest(cyclingPowerService);
        freshLink->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyService *service =
                findService(device, cyclingPowerService);
        QVERIFY(service);
        service->setCharacteristicForTest(
                cyclingPowerMeasurement);
        service->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);

        QCOMPARE(restoredSpy.count(), 1);
        QVERIFY(!reconnectTimer->isActive());

        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void slopeTargetSurvivesAdapterRecovery()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *initialLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(initialLink);
        const QBluetoothUuid cyclingPowerService(
                QBluetoothUuid::ServiceClassUuid::CyclingPower);
        const QBluetoothUuid wahooBrakeControl(
                QStringLiteral(
                        "{A026E005-0A7D-4AB3-97FA-F1500F9FEB8B}"));

        device->connectDevice();
        initialLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        initialLink->emitServiceDiscoveredForTest(
                cyclingPowerService);
        initialLink->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyService *initialService =
                findService(device, cyclingPowerService);
        QVERIFY(initialService);
        initialService->setCharacteristicForTest(
                wahooBrakeControl);
        initialService->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        QCOMPARE(initialService->characteristicWriteCount(), 1);
        for (int command = 0; command < 4; ++command) {
            initialService->emitCharacteristicWrittenForTest(
                    wahooBrakeControl, QByteArray());
        }
        QCOMPARE(initialService->characteristicWriteCount(), 4);

        device->setGradient(5.0);
        QCOMPARE(initialService->characteristicWriteCount(), 5);
        initialService->emitCharacteristicWrittenForTest(
                wahooBrakeControl, QByteArray());

        QVERIFY(device->suspendForAdapterReset());
        const int writesBeforeSuspendedTarget =
                initialService->characteristicWriteCount();
        device->setGradient(7.0);
        QCOMPARE(initialService->characteristicWriteCount(),
                 writesBeforeSuspendedTarget);

        device->resumeAfterAdapterReset();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        freshLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        freshLink->emitServiceDiscoveredForTest(
                cyclingPowerService);
        freshLink->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyService *freshService =
                findService(device, cyclingPowerService);
        QVERIFY(freshService);
        freshService->setCharacteristicForTest(
                wahooBrakeControl);
        freshService->emitStateChangedForTest(
                QLowEnergyService::RemoteServiceDiscovered);
        for (int command = 0; command < 4; ++command) {
            freshService->emitCharacteristicWrittenForTest(
                    wahooBrakeControl, QByteArray());
        }

        QByteArray expectedGradient(3, '\0');
        const int encodedGradient =
                (7.0 / 100.0 + 1.0) * 32768;
        expectedGradient[0] = 0x46;
        expectedGradient[1] = char(encodedGradient);
        expectedGradient[2] = char(encodedGradient >> 8);
        QVERIFY(freshService->characteristicWriteValues().contains(
                expectedGradient));

        device->disconnectDevice();
        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;
    }

    void recoveredDeviceIsNotCoupledToMissingConfiguredPeer()
    {
        const QBluetoothDeviceInfo first = lifecycleDeviceInfo();
        QBluetoothDeviceInfo second(
                QBluetoothAddress(quint64(2)),
                QStringLiteral("Missing lifecycle peer"), 0);
        second.setCoreConfigurations(
                QBluetoothDeviceInfo::LowEnergyCoreConfiguration);

        const auto profileEntry = [](const QBluetoothDeviceInfo &info) {
            return info.name() + QLatin1Char(';')
                    + info.address().toString() + QLatin1Char(';')
                    + info.deviceUuid().toString();
        };
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                profileEntry(first) + QLatin1Char(',')
                + profileEntry(second);

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(first);
        BT40Device *device =
                controller.findChild<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(device);
        QLowEnergyController *initialLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(initialLink);
        initialLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QBluetoothLocalDevice::setAdapterValidForTest(false);
        initialLink->emitErrorForTest(
                QLowEnergyController::InvalidBluetoothAdapterError);
        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(scanRetryTimer->isActive());

        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));
        QBluetoothLocalDevice::setAdapterValidForTest(true);
        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));

        discovery = controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(first);
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        freshLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(!device->isSuspendedForAdapterReset());

        freshLink->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        const bool recoveredDeviceWasResuspended =
                device->isSuspendedForAdapterReset();

        freshLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);

        QVERIFY2(!recoveredDeviceWasResuspended,
                 "a missing peer must not resuspend a recovered device");
    }

    void adapterResetDoesNotReconnectManuallyDisconnectedPeer()
    {
        const QBluetoothDeviceInfo activeInfo = lifecycleDeviceInfo();
        QBluetoothDeviceInfo disconnectedInfo(
                QBluetoothAddress(quint64(2)),
                QStringLiteral("Manually disconnected peer"), 0);
        disconnectedInfo.setCoreConfigurations(
                QBluetoothDeviceInfo::LowEnergyCoreConfiguration);

        const auto profileEntry = [](const QBluetoothDeviceInfo &info) {
            return info.name() + QLatin1Char(';')
                    + info.address().toString() + QLatin1Char(';')
                    + info.deviceUuid().toString();
        };
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                profileEntry(activeInfo) + QLatin1Char(',')
                + profileEntry(disconnectedInfo);

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(activeInfo);
        discovery->emitDeviceDiscoveredForTest(disconnectedInfo);

        BT40Device *activeDevice = nullptr;
        BT40Device *disconnectedDevice = nullptr;
        for (BT40Device *device :
             controller.findChildren<BT40Device *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            if (device->deviceInfo().address()
                    == activeInfo.address()) {
                activeDevice = device;
            } else if (device->deviceInfo().address()
                       == disconnectedInfo.address()) {
                disconnectedDevice = device;
            }
        }
        QVERIFY(activeDevice);
        QVERIFY(disconnectedDevice);

        QLowEnergyController *activeLink =
                activeDevice->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QLowEnergyController *disconnectedLink =
                disconnectedDevice->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(activeLink);
        QVERIFY(disconnectedLink);
        activeLink->emitConnectedForTest();
        disconnectedLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        disconnectedDevice->disconnectDevice();
        disconnectedLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QBluetoothLocalDevice::setAdapterValidForTest(false);
        activeLink->emitErrorForTest(
                QLowEnergyController::InvalidBluetoothAdapterError);

        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(scanRetryTimer->isActive());
        QBluetoothLocalDevice::setAdapterValidForTest(true);
        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));

        discovery = controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        QLowEnergyController::resetTestCounters();
        discovery->emitDeviceDiscoveredForTest(activeInfo);
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyController *freshActiveLink =
                activeDevice->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshActiveLink);
        freshActiveLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        const int activeRecoveryConnects =
                QLowEnergyController::connectCallCount();
        QCOMPARE(activeRecoveryConnects, 1);

        discovery->emitDeviceDiscoveredForTest(disconnectedInfo);
        discovery->emitDeviceDiscoveredForTest(disconnectedInfo);

        QCOMPARE(QLowEnergyController::connectCallCount(),
                 activeRecoveryConnects);
        freshActiveLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        disconnectedLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
    }

    void discoveryAdapterErrorsEnterRecovery_data()
    {
        QTest::addColumn<int>("discoveryError");

        QTest::newRow("invalid-adapter")
                << int(QBluetoothDeviceDiscoveryAgent::
                               InvalidBluetoothAdapterError);
        QTest::newRow("powered-off")
                << int(QBluetoothDeviceDiscoveryAgent::
                               PoweredOffError);
    }

    void discoveryAdapterErrorsEnterRecovery()
    {
        QFETCH(int, discoveryError);

        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(original);
        BT40Device *device =
                controller.findChild<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(device);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(link);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(
                device, "reconnectScanRequested",
                Qt::DirectConnection));
        QVERIFY(discovery->isActive());
        QBluetoothLocalDevice::setAdapterValidForTest(false);
        discovery->emitErrorForTest(
                static_cast<QBluetoothDeviceDiscoveryAgent::Error>(
                        discoveryError));

        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(scanRetryTimer->isActive());
        QVERIFY(device->isSuspendedForAdapterReset());
        link->setStateForTest(
                QLowEnergyController::UnconnectedState);
    }

    void activeDiscoveryCannotBlockAdapterStackRefresh()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QCOMPARE(controller.start(), 0);

        QPointer<QBluetoothDeviceDiscoveryAgent> discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(original);
        BT40Device *device =
                controller.findChild<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(device);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(link);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(
                device, "reconnectScanRequested",
                Qt::DirectConnection));
        QVERIFY(discovery->isActive());
        QBluetoothDeviceDiscoveryAgent::setStopCompletesForTest(false);

        QBluetoothLocalDevice::setAdapterValidForTest(false);
        link->emitErrorForTest(
                QLowEnergyController::InvalidBluetoothAdapterError);

        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(scanRetryTimer->isActive());
        scanRetryTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "startScan", Qt::DirectConnection));

        QVERIFY2(discovery.isNull(),
                 "an active stale scan must not consume adapter recovery");
        QVERIFY(scanRetryTimer->isActive());
        QCOMPARE(QBluetoothDeviceDiscoveryAgent::liveAgentCount(), 1);

        QBluetoothDeviceDiscoveryAgent::setStopCompletesForTest(true);
        link->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);
    }

    void stopRejectsQueuedDiscoveryCallbacks()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);

        QTimer::singleShot(0, discovery, [discovery, original]() {
            discovery->emitDeviceDiscoveredForTest(original);
        });
        QCOMPARE(controller.stop(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        const int lateDevices =
                controller.findChildren<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly).size();
        const int lateConnectCalls =
                QLowEnergyController::connectCallCount();
        for (BT40Device *device :
             controller.findChildren<BT40Device *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            QLowEnergyController *link =
                    device->findChild<QLowEnergyController *>(
                            QString(), Qt::FindDirectChildrenOnly);
            if (link) {
                link->setStateForTest(
                        QLowEnergyController::UnconnectedState);
            }
        }
        QCOMPARE(controller.stop(), 0);

        QCOMPARE(lateDevices, 0);
        QCOMPARE(lateConnectCalls, 0);
    }

    void restartRejectsPreviousSessionDiscoveryCallback()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        QPointer<QBluetoothDeviceDiscoveryAgent> oldDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(oldDiscovery);

        QBluetoothDeviceDiscoveryAgent *oldDiscoveryRaw =
                oldDiscovery.data();
        std::thread emitter([oldDiscoveryRaw, original]() {
            oldDiscoveryRaw->emitDeviceDiscoveredForTest(original);
        });
        emitter.join();
        QCOMPARE(controller.stop(), 0);
        QCOMPARE(controller.start(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        const int crossSessionDevices =
                controller.findChildren<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly).size();
        const int crossSessionConnectCalls =
                QLowEnergyController::connectCallCount();
        QBluetoothDeviceDiscoveryAgent *newDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        const bool stackWasRecreated =
                oldDiscovery.isNull()
                && newDiscovery;

        for (BT40Device *device :
             controller.findChildren<BT40Device *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
            QLowEnergyController *link =
                    device->findChild<QLowEnergyController *>(
                            QString(), Qt::FindDirectChildrenOnly);
            if (link) {
                link->setStateForTest(
                        QLowEnergyController::UnconnectedState);
            }
        }
        QCOMPARE(controller.stop(), 0);

        QVERIFY2(stackWasRecreated,
                 "each training session must own a fresh discovery stack");
        QCOMPARE(crossSessionDevices, 0);
        QCOMPARE(crossSessionConnectCalls, 0);
    }

    void restartRejectsPreviousSessionTerminalCallbacks_data()
    {
        QTest::addColumn<int>("callbackType");

        QTest::newRow("error") << 0;
        QTest::newRow("finished") << 1;
        QTest::newRow("canceled") << 2;
    }

    void restartRejectsPreviousSessionTerminalCallbacks()
    {
        QFETCH(int, callbackType);

        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        QPointer<QBluetoothDeviceDiscoveryAgent> oldDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(oldDiscovery);
        QBluetoothDeviceDiscoveryAgent *oldDiscoveryRaw =
                oldDiscovery.data();

        std::thread emitter([oldDiscoveryRaw, callbackType]() {
            switch (callbackType) {
            case 0:
                oldDiscoveryRaw->emitErrorForTest(
                        QBluetoothDeviceDiscoveryAgent::
                                InputOutputError);
                break;
            case 1:
                oldDiscoveryRaw->emitFinishedForTest();
                break;
            default:
                oldDiscoveryRaw->emitCanceledForTest();
                break;
            }
        });
        emitter.join();
        QCOMPARE(controller.stop(), 0);
        QCOMPARE(controller.start(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QBluetoothDeviceDiscoveryAgent *newDiscovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(oldDiscovery.isNull());
        QVERIFY(newDiscovery);
        QVERIFY(newDiscovery->isActive());
        QVERIFY(scanRetryTimer);
        QVERIFY(!scanRetryTimer->isActive());
        QCOMPARE(controller.findChildren<BT40Device *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 0);
        QCOMPARE(QLowEnergyController::connectCallCount(), 0);
        QCOMPARE(controller.stop(), 0);
    }

    void adapterRecoveryDoesNotLeakAcrossTrainingSessions()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(original);
        BT40Device *firstDevice =
                controller.findChild<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(firstDevice);
        QLowEnergyController *firstLink =
                firstDevice->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(firstLink);
        firstLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QBluetoothLocalDevice::setAdapterValidForTest(false);
        firstLink->emitErrorForTest(
                QLowEnergyController::InvalidBluetoothAdapterError);
        firstLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);

        QBluetoothLocalDevice::setAdapterValidForTest(true);
        QCOMPARE(controller.start(), 0);
        discovery = controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        QVERIFY(discovery->isActive());
        discovery->emitDeviceDiscoveredForTest(original);

        BT40Device *secondDevice =
                controller.findChild<BT40Device *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(secondDevice);
        QLowEnergyController *secondLink =
                secondDevice->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(secondLink);
        secondLink->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        secondLink->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        const bool ordinaryDisconnectWasSuspended =
                secondDevice->isSuspendedForAdapterReset();

        secondLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);

        QVERIFY2(!ordinaryDisconnectWasSuspended,
                 "adapter recovery state must not cross a stop/start boundary");
    }

    void invalidAdapterAtPairingStartFinishesScan()
    {
        QBluetoothLocalDevice::setAdapterValidForTest(false);
        BT40Controller controller(nullptr, nullptr);
        QSignalSpy scanFinishedSpy(
                &controller,
                static_cast<void (BT40Controller::*)(bool)>(
                        &BT40Controller::scanFinished));

        QCOMPARE(controller.start(), 0);
        QCOMPARE(scanFinishedSpy.count(), 1);
        QCOMPARE(scanFinishedSpy.first().at(0).toBool(), false);
        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(!scanRetryTimer->isActive());
        QCOMPARE(controller.stop(), 0);
    }

    void pairingDiscoveryErrorsFinishScan_data()
    {
        QTest::addColumn<int>("discoveryError");

        QTest::newRow("invalid-adapter")
                << int(QBluetoothDeviceDiscoveryAgent::
                               InvalidBluetoothAdapterError);
        QTest::newRow("missing-permissions")
                << int(QBluetoothDeviceDiscoveryAgent::
                               MissingPermissionsError);
    }

    void pairingDiscoveryErrorsFinishScan()
    {
        QFETCH(int, discoveryError);

        BT40Controller controller(nullptr, nullptr);
        QSignalSpy scanFinishedSpy(
                &controller,
                static_cast<void (BT40Controller::*)(bool)>(
                        &BT40Controller::scanFinished));
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);

        discovery->emitErrorForTest(
                static_cast<QBluetoothDeviceDiscoveryAgent::Error>(
                        discoveryError));

        QCOMPARE(scanFinishedSpy.count(), 1);
        QCOMPARE(scanFinishedSpy.first().at(0).toBool(), false);
        QTimer *scanRetryTimer =
                controller.findChild<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(scanRetryTimer);
        QVERIFY(!scanRetryTimer->isActive());
        QCOMPARE(controller.stop(), 0);
    }

    void pairingDiscoveryErrorPreservesFoundDevices()
    {
        BT40Controller controller(nullptr, nullptr);
        QSignalSpy scanFinishedSpy(
                &controller,
                static_cast<void (BT40Controller::*)(bool)>(
                        &BT40Controller::scanFinished));
        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        discovery->emitDeviceDiscoveredForTest(
                lifecycleDeviceInfo());
        QCOMPARE(controller.findChildren<BT40Device *>(
                         QString(), Qt::FindDirectChildrenOnly).size(),
                 1);

        discovery->emitErrorForTest(
                QBluetoothDeviceDiscoveryAgent::InputOutputError);

        QCOMPARE(scanFinishedSpy.count(), 1);
        QCOMPARE(scanFinishedSpy.first().at(0).toBool(), true);
        QCOMPARE(controller.stop(), 0);
    }

    void permanentDiscoveryErrorsDoNotRetry_data()
    {
        QTest::addColumn<int>("discoveryError");

        QTest::newRow("missing-permissions")
                << int(QBluetoothDeviceDiscoveryAgent::
                               MissingPermissionsError);
        QTest::newRow("unsupported-platform")
                << int(QBluetoothDeviceDiscoveryAgent::
                               UnsupportedPlatformError);
        QTest::newRow("unsupported-method")
                << int(QBluetoothDeviceDiscoveryAgent::
                               UnsupportedDiscoveryMethod);
        QTest::newRow("location-disabled")
                << int(QBluetoothDeviceDiscoveryAgent::
                               LocationServiceTurnedOffError);
    }

    void permanentDiscoveryErrorsDoNotRetry()
    {
        QFETCH(int, discoveryError);

        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40_HEARTRATE;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        ControllerStopGuard stopGuard(&controller);
        QSignalSpy notificationSpy(
                &controller, &RealtimeController::setNotification);

        QCOMPARE(controller.start(), 0);
        QBluetoothDeviceDiscoveryAgent *discovery =
                controller.findChild<QBluetoothDeviceDiscoveryAgent *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(discovery);
        QVERIFY(discovery->isActive());

        const QList<QTimer *> controllerTimers =
                controller.findChildren<QTimer *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(controllerTimers.size(), 1);
        QTimer *scanRetryTimer = controllerTimers.first();

        discovery->emitErrorForTest(
                static_cast<QBluetoothDeviceDiscoveryAgent::Error>(
                        discoveryError));

        QVERIFY2(!scanRetryTimer->isActive(),
                 "permanent discovery errors must wait for user action");
        QCOMPARE(notificationSpy.count(), 1);
        QCOMPARE(notificationSpy.first().at(1).toInt(), 4);
    }

    void repeatedInvalidAdapterErrorsSuspendWithoutControllerChurn()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QPointer<QLowEnergyController> initialLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(initialLink);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);

        device->connectDevice();
        for (int failure = 0; failure < 3; ++failure) {
            QVERIFY(initialLink);
            initialLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
            initialLink->emitErrorForTest(
                    QLowEnergyController::InvalidBluetoothAdapterError);
            QCOMPARE(QLowEnergyController::lastRemoteAddressType(),
                     QLowEnergyController::RandomAddress);
        }
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyController *currentLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        const bool controllerWasPreserved =
                currentLink == initialLink.data();
        const int rediscoveryRequests = rediscoverySpy.count();
        const int connectCalls =
                QLowEnergyController::connectCallCount();
        const QLowEnergyController::RemoteAddressType addressType =
                QLowEnergyController::lastRemoteAddressType();

        device->disconnectDevice();
        if (currentLink) {
            currentLink->setStateForTest(
                    QLowEnergyController::UnconnectedState);
        }
        delete device;

        QVERIFY(controllerWasPreserved);
        QCOMPARE(rediscoveryRequests, 1);
        QCOMPARE(connectCalls, 1);
        QCOMPARE(addressType,
                 QLowEnergyController::RandomAddress);
    }

    void trainerReconnectPreservesConfirmedAddressType()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        link->setStateForTest(QLowEnergyController::ConnectingState);
        link->emitErrorForTest(
                QLowEnergyController::ConnectionError);
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        QVERIFY(QMetaObject::invokeMethod(
                device, "attemptReconnect", Qt::DirectConnection));
        const QLowEnergyController::RemoteAddressType addressType =
                QLowEnergyController::lastRemoteAddressType();

        device->disconnectDevice();
        link->setStateForTest(
                QLowEnergyController::UnconnectedState);
        delete device;

        QCOMPARE(addressType,
                 QLowEnergyController::RandomAddress);
    }

    void rediscoveryRemainsPendingUntilHeartRateStreams()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40_HEARTRATE;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        BT40Device *device = addControllerDevice(&controller);
        QVERIFY(device);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QVERIFY(QMetaObject::invokeMethod(
                device, "reconnectScanRequested", Qt::DirectConnection));

        QBluetoothDeviceInfo refreshed(
                original.address(),
                QStringLiteral("Refreshed lifecycle device"), 0);
        refreshed.setCoreConfigurations(
                QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
        QSignalSpy notificationSpy(
                &controller, &RealtimeController::setNotification);
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "addDevice", Qt::DirectConnection,
                Q_ARG(QBluetoothDeviceInfo, refreshed)));

        QCOMPARE(device->deviceInfo().name(), refreshed.name());
        notificationSpy.clear();
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "scanFinished", Qt::DirectConnection));
        QCOMPARE(notificationSpy.count(), 0);

        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QCOMPARE(notificationSpy.count(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        QCOMPARE(controller.stop(), 0);
    }

    void manualDisconnectCancelsPendingRediscovery()
    {
        const QBluetoothDeviceInfo original = lifecycleDeviceInfo();
        DeviceConfiguration config;
        config.type = DEV_BT40_HEARTRATE;
        config.deviceProfile =
                original.name() + QLatin1Char(';')
                + original.address().toString() + QLatin1Char(';')
                + original.deviceUuid().toString();

        BT40Controller controller(nullptr, &config);
        QCOMPARE(controller.start(), 0);
        BT40Device *device = addControllerDevice(&controller);
        QVERIFY(device);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QVERIFY(QMetaObject::invokeMethod(
                device, "reconnectScanRequested", Qt::DirectConnection));

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        QLowEnergyController::resetTestCounters();

        QBluetoothDeviceInfo refreshed(
                original.address(),
                QStringLiteral("Manually disconnected device"), 0);
        refreshed.setCoreConfigurations(
                QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "addDevice", Qt::DirectConnection,
                Q_ARG(QBluetoothDeviceInfo, refreshed)));
        QCOMPARE(QLowEnergyController::connectCallCount(), 0);

        QCOMPARE(controller.stop(), 0);
    }

    void nullServiceObjectIsIgnored()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(
                &controller,
                BluetoothDeviceTypes::DeviceRole::HeartRateOnly);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);
        const QBluetoothUuid heartRateService(
                QBluetoothUuid::ServiceClassUuid::HeartRate);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QLowEnergyController::failNextServiceCreation();
        link->emitServiceDiscoveredForTest(heartRateService);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(device->findChildren<QLowEnergyService *>().size(), 0);
        QCOMPARE(rediscoverySpy.count(), 1);

        device->disconnectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void lowerPriorityControlServiceIsDestroyed_data()
    {
        QTest::addColumn<bool>("higherPriorityFirst");
        QTest::newRow("higher-priority-first") << true;
        QTest::newRow("lower-priority-first") << false;
    }

    void lowerPriorityControlServiceIsDestroyed()
    {
        QFETCH(bool, higherPriorityFirst);

        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        link->emitConnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        const QBluetoothUuid higherPriority =
                s_KurtSmartControlService_UUID;
        const QBluetoothUuid lowerPriority =
                s_FtmsService_UUID;
        link->emitServiceDiscoveredForTest(
                higherPriorityFirst
                        ? higherPriority : lowerPriority);
        link->emitServiceDiscoveredForTest(
                higherPriorityFirst
                        ? lowerPriority : higherPriority);
        link->emitDiscoveryFinishedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(device->findChildren<QLowEnergyService *>().size(), 1);
        QCOMPARE(QLowEnergyService::destructionCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        delete device;
    }

    void manualDisconnectSuppressesReconnect()
    {
        BT40Controller controller(nullptr, nullptr);
        BT40Device *device = createDevice(&controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);
        QSignalSpy rediscoverySpy(
                device, &BT40Device::reconnectScanRequested);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::ConnectedState);
        device->disconnectDevice();
        QCOMPARE(QLowEnergyController::disconnectCallCount(), 1);

        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(rediscoverySpy.count(), 0);
        QCOMPARE(QLowEnergyController::connectCallCount(), 1);

        delete device;
    }

    void queuedDisconnectCallbackCannotOutliveController()
    {
        BT40Controller *controller = new BT40Controller(nullptr, nullptr);
        QPointer<BT40Device> device = createDevice(controller);
        QLowEnergyController *link =
                device->findChild<QLowEnergyController *>();
        QVERIFY(link);

        device->connectDevice();
        link->setStateForTest(QLowEnergyController::UnconnectedState);
        link->emitDisconnectedForTest();

        delete controller;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        const bool destroyedWithController = device.isNull();
        if (!destroyedWithController) {
            link->setStateForTest(QLowEnergyController::UnconnectedState);
            delete device.data();
        }

        QVERIFY(destroyedWithController);
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }

    void stoppedPairingControllerSuppressesLateScanFinished()
    {
        BT40Controller controller(nullptr, nullptr);
        QSignalSpy scanFinishedSpy(
                &controller,
                static_cast<void (BT40Controller::*)(bool)>(
                        &BT40Controller::scanFinished));

        QCOMPARE(controller.stop(), 0);
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "scanFinished", Qt::DirectConnection));

        QCOMPARE(scanFinishedSpy.count(), 0);
    }

    void wizardCleanupStopsAndDeletesControllerIdempotently()
    {
        RealtimeController *owned =
                new BT40Controller(nullptr, nullptr);
        BT40Controller *controller =
                static_cast<BT40Controller *>(owned);
        BT40Device *rawDevice = addControllerDevice(controller);
        QVERIFY(rawDevice);
        QPointer<BT40Device> device = rawDevice;

        AddDeviceWizard::cleanupController(owned);

        QVERIFY(!owned);
        QVERIFY(device.isNull());
        QCOMPARE(QLowEnergyController::destructionCount(), 1);

        AddDeviceWizard::cleanupController(owned);
        QCOMPARE(QLowEnergyController::destructionCount(), 1);
    }
};

QTEST_GUILESS_MAIN(TestBt40Lifecycle)
#include "testBt40Lifecycle.moc"
