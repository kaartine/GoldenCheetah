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

namespace {

QBluetoothDeviceInfo lifecycleDeviceInfo()
{
    QBluetoothDeviceInfo info(QBluetoothAddress(quint64(1)),
                              QStringLiteral("Lifecycle test device"), 0);
    info.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
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
        QLowEnergyController::resetTestCounters();
        QLowEnergyService::resetTestCounters();
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
        QCOMPARE(QLowEnergyController::connectCallCount(), 2);
        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>();
        QVERIFY(freshLink);
        QVERIFY(freshLink != staleLink);

        staleLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        staleLink->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
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

        QLowEnergyController *freshLink =
                device->findChild<QLowEnergyController *>(
                        QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(freshLink);
        QVERIFY(freshLink != initialLink);
        QCOMPARE(QLowEnergyController::connectCallCount(), 4);

        initialLink->setStateForTest(
                QLowEnergyController::UnconnectedState);
        initialLink->emitDisconnectedForTest();
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
        QVERIFY(initialLink.isNull());

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

        QTimer *retirementTimer = staleLink->findChild<QTimer *>(
                QStringLiteral("bt40-stale-controller-retirement"));
        QVERIFY(retirementTimer);
        QVERIFY(QMetaObject::invokeMethod(
                retirementTimer, "timeout", Qt::DirectConnection));
        QCoreApplication::sendPostedEvents(
                nullptr, QEvent::DeferredDelete);
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

        QVERIFY(QMetaObject::invokeMethod(
                device, "heartRateStreamReady", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(
                &controller, "scanFinished", Qt::DirectConnection));
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
