/*
 * Controllable test double for the minimal USBXpress API.
 */

#include "FakeSiUSBXp.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {

struct Device
{
    std::string vid;
    std::string pid;
};

struct State
{
    std::vector<Device> devices;
    std::array<SI_STATUS,
               static_cast<size_t>(FakeSiUSBXp::Operation::Count)> results;
    DWORD reportedDeviceCount = 0;
    DWORD reportedRead = 0;
    DWORD reportedWrite = 0;
    FakeSiUSBXp::Snapshot snapshot;
};

State state;
int handleToken;

size_t operationIndex(const FakeSiUSBXp::Operation operation)
{
    return static_cast<size_t>(operation);
}

SI_STATUS resultFor(const FakeSiUSBXp::Operation operation)
{
    return state.results[operationIndex(operation)];
}

void record(const FakeSiUSBXp::Operation operation)
{
    state.snapshot.calls.push_back(operation);
}

void copyProductString(const std::string &value, LPVOID destination)
{
    std::memcpy(destination, value.c_str(), value.size() + 1);
}

}

namespace FakeSiUSBXp
{

void reset()
{
    state = State();
    state.results.fill(SI_SUCCESS);
}

void addDevice(const std::string &vid, const std::string &pid)
{
    state.devices.push_back(Device{vid, pid});
    state.reportedDeviceCount = static_cast<DWORD>(state.devices.size());
}

void setResult(const Operation operation, const SI_STATUS result)
{
    state.results[operationIndex(operation)] = result;
}

void setReportedDeviceCount(const DWORD count)
{
    state.reportedDeviceCount = count;
}

void setReadResult(const SI_STATUS result, const DWORD bytesReturned)
{
    setResult(Operation::Read, result);
    state.reportedRead = bytesReturned;
}

void setWriteResult(const SI_STATUS result, const DWORD bytesWritten)
{
    setResult(Operation::Write, result);
    state.reportedWrite = bytesWritten;
}

Snapshot snapshot()
{
    return state.snapshot;
}

HANDLE testHandle()
{
    return &handleToken;
}

const char *operationName(const Operation operation)
{
    switch (operation) {
    case Operation::GetNumDevices: return "SI_GetNumDevices";
    case Operation::GetProductStringVid: return "SI_GetProductString(VID)";
    case Operation::GetProductStringPid: return "SI_GetProductString(PID)";
    case Operation::Open: return "SI_Open";
    case Operation::FlushBuffers: return "SI_FlushBuffers";
    case Operation::SetTimeouts: return "SI_SetTimeouts";
    case Operation::SetBaudRate: return "SI_SetBaudRate";
    case Operation::SetLineControl: return "SI_SetLineControl";
    case Operation::SetFlowControl: return "SI_SetFlowControl";
    case Operation::Close: return "SI_Close";
    case Operation::WindowsCloseHandle: return "CloseHandle";
    case Operation::Read: return "SI_Read";
    case Operation::Write: return "SI_Write";
    case Operation::Count: return "Count";
    }
    return "unknown";
}

}

extern "C" BOOL CloseHandle(HANDLE)
{
    record(FakeSiUSBXp::Operation::WindowsCloseHandle);
    return TRUE;
}

extern "C" SI_STATUS SI_GetNumDevices(LPDWORD numberOfDevices)
{
    record(FakeSiUSBXp::Operation::GetNumDevices);
    *numberOfDevices = state.reportedDeviceCount;
    return resultFor(FakeSiUSBXp::Operation::GetNumDevices);
}

extern "C" SI_STATUS SI_GetProductString(const DWORD deviceNumber,
                                           LPVOID deviceString,
                                           const DWORD options)
{
    const FakeSiUSBXp::Operation operation =
            options == SI_RETURN_VID
            ? FakeSiUSBXp::Operation::GetProductStringVid
            : FakeSiUSBXp::Operation::GetProductStringPid;
    record(operation);
    state.snapshot.productQueries.push_back(
            FakeSiUSBXp::ProductQuery{deviceNumber, options});

    if (deviceNumber >= state.devices.size() ||
        (options != SI_RETURN_VID && options != SI_RETURN_PID)) {
        static const char empty[] = "";
        copyProductString(empty, deviceString);
        return SI_DEVICE_NOT_FOUND;
    }

    const Device &device = state.devices[deviceNumber];
    copyProductString(options == SI_RETURN_VID ? device.vid : device.pid,
                      deviceString);

    // A failed API's out-parameter is untrusted even when it looks plausible.
    return resultFor(operation);
}

extern "C" SI_STATUS SI_Open(const DWORD deviceNumber, HANDLE *handle)
{
    record(FakeSiUSBXp::Operation::Open);
    state.snapshot.openedDevice = deviceNumber;

    const SI_STATUS result = resultFor(FakeSiUSBXp::Operation::Open);
    if (result == SI_SUCCESS) {
        *handle = FakeSiUSBXp::testHandle();
        state.snapshot.openedHandle = *handle;
    }
    return result;
}

extern "C" SI_STATUS SI_Close(const HANDLE handle)
{
    record(FakeSiUSBXp::Operation::Close);
    ++state.snapshot.closeCalls;
    state.snapshot.closedHandle = handle;
    return resultFor(FakeSiUSBXp::Operation::Close);
}

extern "C" SI_STATUS SI_Read(HANDLE, LPVOID buffer, const DWORD bytesToRead,
                              LPDWORD bytesReturned, LPOVERLAPPED)
{
    record(FakeSiUSBXp::Operation::Read);
    state.snapshot.requestedRead = bytesToRead;
    *bytesReturned = state.reportedRead;
    if (buffer && state.reportedRead > 0) {
        std::memset(buffer, 0x5a,
                    std::min(bytesToRead, state.reportedRead));
    }
    return resultFor(FakeSiUSBXp::Operation::Read);
}

extern "C" SI_STATUS SI_Write(HANDLE, LPVOID, const DWORD bytesToWrite,
                               LPDWORD bytesWritten, LPOVERLAPPED)
{
    record(FakeSiUSBXp::Operation::Write);
    state.snapshot.requestedWrite = bytesToWrite;
    *bytesWritten = state.reportedWrite;
    return resultFor(FakeSiUSBXp::Operation::Write);
}

extern "C" SI_STATUS SI_FlushBuffers(HANDLE, const BYTE flushTransmit,
                                      const BYTE flushReceive)
{
    record(FakeSiUSBXp::Operation::FlushBuffers);
    state.snapshot.flushTransmit = flushTransmit;
    state.snapshot.flushReceive = flushReceive;
    return resultFor(FakeSiUSBXp::Operation::FlushBuffers);
}

extern "C" SI_STATUS SI_SetTimeouts(const DWORD readTimeout,
                                     const DWORD writeTimeout)
{
    record(FakeSiUSBXp::Operation::SetTimeouts);
    state.snapshot.readTimeout = readTimeout;
    state.snapshot.writeTimeout = writeTimeout;
    return resultFor(FakeSiUSBXp::Operation::SetTimeouts);
}

extern "C" SI_STATUS SI_SetBaudRate(HANDLE, const DWORD baudRate)
{
    record(FakeSiUSBXp::Operation::SetBaudRate);
    state.snapshot.baudRate = baudRate;
    return resultFor(FakeSiUSBXp::Operation::SetBaudRate);
}

extern "C" SI_STATUS SI_SetLineControl(HANDLE, const WORD lineControl)
{
    record(FakeSiUSBXp::Operation::SetLineControl);
    state.snapshot.lineControl = lineControl;
    return resultFor(FakeSiUSBXp::Operation::SetLineControl);
}

extern "C" SI_STATUS SI_SetFlowControl(HANDLE, const BYTE ctsMaskCode,
                                        const BYTE rtsMaskCode,
                                        const BYTE dtrMaskCode,
                                        const BYTE dsrMaskCode,
                                        const BYTE dcdMaskCode,
                                        const BOOL flowXonXoff)
{
    record(FakeSiUSBXp::Operation::SetFlowControl);
    state.snapshot.ctsMaskCode = ctsMaskCode;
    state.snapshot.rtsMaskCode = rtsMaskCode;
    state.snapshot.dtrMaskCode = dtrMaskCode;
    state.snapshot.dsrMaskCode = dsrMaskCode;
    state.snapshot.dcdMaskCode = dcdMaskCode;
    state.snapshot.flowXonXoff = flowXonXoff;
    return resultFor(FakeSiUSBXp::Operation::SetFlowControl);
}
