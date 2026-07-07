/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/BluetoothDeviceTypes.h"
#include "Train/TrainingDeviceSelection.h"

#include <QTest>

class TestDeviceSelection : public QObject
{
    Q_OBJECT

private slots:

    void preservesStableBluetoothTypeIds()
    {
        QCOMPARE(DEV_BT40, 0x2000);
        QCOMPARE(DEV_BT40_HEARTRATE, 0x2001);
    }

    void classifiesHeartRateProfiles()
    {
        using namespace BluetoothDeviceTypes;

        QVERIFY(acceptsDiscoveredDevice(DEV_BT40_HEARTRATE, true));
        QVERIFY(!acceptsDiscoveredDevice(DEV_BT40_HEARTRATE, false));
        QVERIFY(acceptsDiscoveredDevice(DEV_BT40, true));
        QVERIFY(acceptsDiscoveredDevice(DEV_BT40, false));
        QVERIFY(!permitsEmptyProfile(DEV_BT40_HEARTRATE));
        QVERIFY(permitsEmptyProfile(DEV_BT40));
    }

    void restrictsHeartRateOnlyServicesAndControl()
    {
        using namespace BluetoothDeviceTypes;

        QVERIFY(acceptsService(DeviceRole::HeartRateOnly, ServiceKind::HeartRate));
        QVERIFY(!acceptsService(DeviceRole::HeartRateOnly, ServiceKind::TrainerControl));
        QVERIFY(!acceptsService(DeviceRole::HeartRateOnly, ServiceKind::OtherTelemetry));
        QVERIFY(acceptsService(DeviceRole::Trainer, ServiceKind::TrainerControl));
        QVERIFY(allowsTrainerControl(DeviceRole::Trainer));
        QVERIFY(!allowsTrainerControl(DeviceRole::HeartRateOnly));
    }

    void selectsOneHeartRateCompanion()
    {
        const QList<int> types = QList<int>()
                << DEV_BT40 << DEV_BT40_HEARTRATE << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0;

        const TrainingDeviceSelection::Selection selection =
                TrainingDeviceSelection::select(selected, types);

        QCOMPARE(selection.active, QList<int>() << 0 << 1);
        QCOMPARE(selection.heartRateSource, 1);
    }

    void honorsExplicitHeartRateSelection()
    {
        const QList<int> types = QList<int>()
                << DEV_BT40 << DEV_BT40_HEARTRATE << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0 << 2;

        const TrainingDeviceSelection::Selection selection =
                TrainingDeviceSelection::select(selected, types);

        QCOMPARE(selection.active, selected);
        QCOMPARE(selection.heartRateSource, 2);
    }

    void collapsesMultipleHeartRateSelections()
    {
        const QList<int> types = QList<int>()
                << DEV_BT40 << DEV_BT40_HEARTRATE << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0 << 2 << 1;

        const TrainingDeviceSelection::Selection selection =
                TrainingDeviceSelection::select(selected, types);

        QCOMPARE(selection.active, QList<int>() << 0 << 2);
        QCOMPARE(selection.heartRateSource, 2);
    }

    void filtersInvalidAndDuplicateIndexesBeforeActivation()
    {
        const QList<int> types = QList<int>() << DEV_BT40 << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << -1 << 4 << 0 << 0;

        const TrainingDeviceSelection::Selection selection =
                TrainingDeviceSelection::select(selected, types);

        QCOMPARE(selection.active, QList<int>() << 0 << 1);
        QCOMPARE(selection.heartRateSource, 1);
    }

    void refusesActivationWithoutAValidSelection()
    {
        const QList<int> types = QList<int>() << DEV_BT40 << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << -1 << 4;

        const TrainingDeviceSelection::Selection selection =
                TrainingDeviceSelection::select(selected, types);

        QVERIFY(selection.active.isEmpty());
        QCOMPARE(selection.heartRateSource, -1);
    }

    void routesHeartRateOnlyFromChosenSource()
    {
        QVERIFY(TrainingDeviceSelection::routesHeartRate(2, 2));
        QVERIFY(!TrainingDeviceSelection::routesHeartRate(2, 1));
        QVERIFY(!TrainingDeviceSelection::routesHeartRate(-1, 1));
    }

    void preservesExplicitHeartRateSourceWithMultipleDevices()
    {
        const QList<int> active = QList<int>() << 0 << 1 << 2;

        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         2, 1, true, active), 2);
        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         0, 1, true, active), 0);
        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         -1, 1, true, active), -1);
        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         0, 1, false, active), 1);
    }

    void automaticHeartRateSourceFallsBackToSelectedDevice()
    {
        const QList<int> active = QList<int>() << 0;

        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         0, -1, false, active), 0);
        QCOMPARE(BluetoothDeviceTypes::resolveHeartRateSource(
                         3, -1, false, active), -1);
    }

    void controllerStartFailureRollsBackPartialStart()
    {
        const QList<int> active = QList<int>() << 0 << 1 << 2;
        QList<int> startCalls;
        QList<int> stopCalls;

        const BluetoothDeviceTypes::ControllerStartResult result =
                BluetoothDeviceTypes::startControllers(
                        active,
                        [&startCalls](int device) {
                            startCalls.append(device);
                            return device == 1 ? -7 : 0;
                        },
                        [&stopCalls](int device) {
                            stopCalls.append(device);
                        });

        QVERIFY(!result.success);
        QCOMPARE(result.failedDevice, 1);
        QCOMPARE(result.errorCode, -7);
        QCOMPARE(startCalls, QList<int>() << 0 << 1);
        QCOMPARE(stopCalls, QList<int>() << 1 << 0);
    }

    void controllerStartSuccessDoesNotStopDevices()
    {
        const QList<int> active = QList<int>() << 2 << 4;
        QList<int> startCalls;
        QList<int> stopCalls;

        const BluetoothDeviceTypes::ControllerStartResult result =
                BluetoothDeviceTypes::startControllers(
                        active,
                        [&startCalls](int device) {
                            startCalls.append(device);
                            return 0;
                        },
                        [&stopCalls](int device) {
                            stopCalls.append(device);
                        });

        QVERIFY(result.success);
        QCOMPARE(result.failedDevice, -1);
        QCOMPARE(result.errorCode, 0);
        QCOMPARE(startCalls, active);
        QVERIFY(stopCalls.isEmpty());
    }

    void lifecyclePolicyPreventsDuplicateConnectAndDisconnect()
    {
        using namespace BluetoothDeviceTypes;

        QVERIFY(shouldConnect(true, LinkState::Unconnected));
        QVERIFY(!shouldConnect(true, LinkState::Active));
        QVERIFY(!shouldConnect(true, LinkState::Closing));
        QVERIFY(!shouldConnect(false, LinkState::Unconnected));

        QVERIFY(!shouldDisconnect(LinkState::Unconnected));
        QVERIFY(shouldDisconnect(LinkState::Active));
        QVERIFY(!shouldDisconnect(LinkState::Closing));
    }

    void lifecyclePolicyReconnectsOnlyRequestedUnconnectedLinks()
    {
        using namespace BluetoothDeviceTypes;

        QVERIFY(shouldReconnect(true, LinkState::Unconnected));
        QVERIFY(!shouldReconnect(false, LinkState::Unconnected));
        QVERIFY(!shouldReconnect(true, LinkState::Active));
        QVERIFY(!shouldReconnect(true, LinkState::Closing));
    }

    void addsHeartRateDeviceToSelectedTrainer()
    {
        const QList<int> types = QList<int>() << DEV_BT40 << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0;

        const QList<int> active = TrainingDeviceSelection::withHeartRateCompanions(selected, types);

        QCOMPARE(active, QList<int>() << 0 << 1);
        QCOMPARE(TrainingDeviceSelection::heartRateSource(active, types), 1);
    }

    void doesNotActivateAnotherTrainer()
    {
        const QList<int> types = QList<int>()
                << DEV_BT40 << DEV_BT40 << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0;

        QCOMPARE(TrainingDeviceSelection::withHeartRateCompanions(selected, types),
                 QList<int>() << 0 << 2);
    }

    void doesNotDuplicateSelectedHeartRateDevice()
    {
        const QList<int> types = QList<int>() << DEV_BT40 << DEV_BT40_HEARTRATE;
        const QList<int> selected = QList<int>() << 0 << 1;

        const QList<int> active = TrainingDeviceSelection::withHeartRateCompanions(selected, types);

        QCOMPARE(active, selected);
        QCOMPARE(TrainingDeviceSelection::heartRateSource(active, types), 1);
    }

    void ignoresInvalidIndexes()
    {
        const QList<int> types = QList<int>() << DEV_BT40;
        const QList<int> selected = QList<int>() << -1 << 4 << 0;

        QCOMPARE(TrainingDeviceSelection::heartRateSource(selected, types), -1);
    }
};

QTEST_MAIN(TestDeviceSelection)
#include "testDeviceSelection.moc"
