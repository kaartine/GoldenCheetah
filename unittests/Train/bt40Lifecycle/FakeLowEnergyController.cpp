/*
 * Deterministic QLowEnergyController test double for BT40 lifecycle tests.
 */

#include "QLowEnergyController"
#include "QLowEnergyService"

int QLowEnergyController::connectCalls = 0;
int QLowEnergyController::disconnectCalls = 0;
int QLowEnergyController::discoverCalls = 0;
int QLowEnergyController::destructions = 0;

QLowEnergyController *
QLowEnergyController::createCentral(const QBluetoothDeviceInfo &, QObject *parent)
{
    return new QLowEnergyController(parent);
}

QLowEnergyController::QLowEnergyController(QObject *parent) : QObject(parent)
{
}

QLowEnergyController::~QLowEnergyController()
{
    ++destructions;
}

QLowEnergyController::ControllerState
QLowEnergyController::state() const
{
    return currentState;
}

void
QLowEnergyController::setRemoteAddressType(RemoteAddressType type)
{
    addressType = type;
}

void
QLowEnergyController::connectToDevice()
{
    ++connectCalls;
    currentState = ConnectingState;
}

void
QLowEnergyController::disconnectFromDevice()
{
    ++disconnectCalls;
    currentState = ClosingState;
}

void
QLowEnergyController::discoverServices()
{
    ++discoverCalls;
}

QLowEnergyService *
QLowEnergyController::createServiceObject(
        const QBluetoothUuid &serviceUuid, QObject *parent)
{
    return new QLowEnergyService(serviceUuid, parent ? parent : this);
}

void
QLowEnergyController::setStateForTest(ControllerState state)
{
    currentState = state;
}

void
QLowEnergyController::emitConnectedForTest()
{
    emit connected();
}

void
QLowEnergyController::emitDisconnectedForTest()
{
    emit disconnected();
}

void
QLowEnergyController::emitErrorForTest(Error error)
{
    emit errorOccurred(error);
}

void
QLowEnergyController::emitServiceDiscoveredForTest(
        const QBluetoothUuid &serviceUuid)
{
    emit serviceDiscovered(serviceUuid);
}

void
QLowEnergyController::emitDiscoveryFinishedForTest()
{
    emit discoveryFinished();
}

void
QLowEnergyController::resetTestCounters()
{
    connectCalls = 0;
    disconnectCalls = 0;
    discoverCalls = 0;
    destructions = 0;
}

int QLowEnergyController::connectCallCount() { return connectCalls; }
int QLowEnergyController::disconnectCallCount() { return disconnectCalls; }
int QLowEnergyController::discoverCallCount() { return discoverCalls; }
int QLowEnergyController::destructionCount() { return destructions; }
