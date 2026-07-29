/*
 * Deterministic local-adapter and discovery-agent test doubles for BT40
 * lifecycle tests.
 */

#include "QBluetoothDeviceDiscoveryAgent"
#include "QBluetoothLocalDevice"

bool QBluetoothLocalDevice::adapterAvailable = true;
int QBluetoothLocalDevice::adapterGeneration = 1;
int QBluetoothLocalDevice::liveDevices = 0;

QBluetoothLocalDevice::QBluetoothLocalDevice(QObject *parent) :
    QObject(parent),
    validAtCreation(adapterAvailable),
    creationGeneration(adapterGeneration)
{
    ++liveDevices;
}

QBluetoothLocalDevice::~QBluetoothLocalDevice()
{
    --liveDevices;
}

bool
QBluetoothLocalDevice::isValid() const
{
    return validAtCreation
            && adapterAvailable
            && creationGeneration == adapterGeneration;
}

void
QBluetoothLocalDevice::resetTestState()
{
    adapterAvailable = true;
    ++adapterGeneration;
}

void
QBluetoothLocalDevice::setAdapterValidForTest(bool valid)
{
    if (adapterAvailable == valid) return;
    if (!valid) ++adapterGeneration;
    adapterAvailable = valid;
}

int
QBluetoothLocalDevice::liveDeviceCount()
{
    return liveDevices;
}

int QBluetoothDeviceDiscoveryAgent::startCalls = 0;
int QBluetoothDeviceDiscoveryAgent::stopCalls = 0;
int QBluetoothDeviceDiscoveryAgent::liveAgents = 0;
bool QBluetoothDeviceDiscoveryAgent::stopCompletes = true;

QBluetoothDeviceDiscoveryAgent::QBluetoothDeviceDiscoveryAgent(QObject *parent)
    : QObject(parent)
{
    ++liveAgents;
}

QBluetoothDeviceDiscoveryAgent::~QBluetoothDeviceDiscoveryAgent()
{
    --liveAgents;
}

bool
QBluetoothDeviceDiscoveryAgent::isActive() const
{
    return active;
}

void
QBluetoothDeviceDiscoveryAgent::setLowEnergyDiscoveryTimeout(int timeout)
{
    discoveryTimeoutMs = timeout;
}

int
QBluetoothDeviceDiscoveryAgent::lowEnergyDiscoveryTimeout() const
{
    return discoveryTimeoutMs;
}

void
QBluetoothDeviceDiscoveryAgent::start()
{
    start(ClassicMethod | LowEnergyMethod);
}

void
QBluetoothDeviceDiscoveryAgent::start(DiscoveryMethods)
{
    if (active) return;
    active = true;
    ++startCalls;
}

void
QBluetoothDeviceDiscoveryAgent::stop()
{
    if (!active) return;
    ++stopCalls;
    if (!stopCompletes) return;
    active = false;
    emit canceled();
}

void
QBluetoothDeviceDiscoveryAgent::emitDeviceDiscoveredForTest(
        const QBluetoothDeviceInfo &info)
{
    emit deviceDiscovered(info);
}

void
QBluetoothDeviceDiscoveryAgent::emitErrorForTest(Error error)
{
    active = false;
    emit errorOccurred(error);
}

void
QBluetoothDeviceDiscoveryAgent::emitFinishedForTest()
{
    active = false;
    emit finished();
}

void
QBluetoothDeviceDiscoveryAgent::emitCanceledForTest()
{
    active = false;
    emit canceled();
}

void
QBluetoothDeviceDiscoveryAgent::resetTestState()
{
    startCalls = 0;
    stopCalls = 0;
    stopCompletes = true;
}

void
QBluetoothDeviceDiscoveryAgent::setStopCompletesForTest(bool completes)
{
    stopCompletes = completes;
}

int
QBluetoothDeviceDiscoveryAgent::startCallCount()
{
    return startCalls;
}

int
QBluetoothDeviceDiscoveryAgent::stopCallCount()
{
    return stopCalls;
}

int
QBluetoothDeviceDiscoveryAgent::liveAgentCount()
{
    return liveAgents;
}
