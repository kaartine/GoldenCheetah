/*
 * Deterministic QLowEnergyController test double for BT40 lifecycle tests.
 */

#include "QLowEnergyController"
#include "QLowEnergyService"

int QLowEnergyController::connectCalls = 0;
int QLowEnergyController::disconnectCalls = 0;
int QLowEnergyController::discoverCalls = 0;
int QLowEnergyController::destructions = 0;
int QLowEnergyController::liveControllers = 0;
int QLowEnergyController::overlappingConnectCalls = 0;
int QLowEnergyController::errorEmissionDepth = 0;
int QLowEnergyController::synchronousErrorDestructions = 0;
QLowEnergyController::RemoteAddressType
        QLowEnergyController::lastAddressType = PublicAddress;
bool QLowEnergyController::failServiceCreation = false;

QLowEnergyController *
QLowEnergyController::createCentral(const QBluetoothDeviceInfo &, QObject *parent)
{
    return new QLowEnergyController(parent);
}

QLowEnergyController::QLowEnergyController(QObject *parent) : QObject(parent)
{
    ++liveControllers;
}

QLowEnergyController::~QLowEnergyController()
{
    if (errorEmissionDepth > 0)
        ++synchronousErrorDestructions;
    --liveControllers;
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
    lastAddressType = type;
}

void
QLowEnergyController::connectToDevice()
{
    if (liveControllers > 1)
        ++overlappingConnectCalls;
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
    if (failServiceCreation) {
        failServiceCreation = false;
        return nullptr;
    }
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
    currentState = ConnectedState;
    emit connected();
}

void
QLowEnergyController::emitDisconnectedForTest()
{
    currentState = UnconnectedState;
    emit disconnected();
}

void
QLowEnergyController::emitErrorForTest(Error error)
{
    ++errorEmissionDepth;
    emit errorOccurred(error);
    --errorEmissionDepth;
}

void
QLowEnergyController::emitServiceDiscoveredForTest(
        const QBluetoothUuid &serviceUuid)
{
    currentState = DiscoveringState;
    emit serviceDiscovered(serviceUuid);
}

void
QLowEnergyController::emitDiscoveryFinishedForTest()
{
    currentState = DiscoveredState;
    emit discoveryFinished();
}

void
QLowEnergyController::resetTestCounters()
{
    connectCalls = 0;
    disconnectCalls = 0;
    discoverCalls = 0;
    destructions = 0;
    overlappingConnectCalls = 0;
    synchronousErrorDestructions = 0;
    lastAddressType = PublicAddress;
    failServiceCreation = false;
}

int QLowEnergyController::connectCallCount() { return connectCalls; }
int QLowEnergyController::disconnectCallCount() { return disconnectCalls; }
int QLowEnergyController::discoverCallCount() { return discoverCalls; }
int QLowEnergyController::destructionCount() { return destructions; }
int QLowEnergyController::liveControllerCount() { return liveControllers; }
int QLowEnergyController::overlappingConnectCallCount()
{
    return overlappingConnectCalls;
}
int QLowEnergyController::synchronousErrorDestructionCount()
{
    return synchronousErrorDestructions;
}
QLowEnergyController::RemoteAddressType
QLowEnergyController::lastRemoteAddressType()
{
    return lastAddressType;
}

void
QLowEnergyController::failNextServiceCreation()
{
    failServiceCreation = true;
}
