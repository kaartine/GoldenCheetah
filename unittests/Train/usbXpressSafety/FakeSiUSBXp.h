/*
 * Controllable test double for the minimal USBXpress API.
 */

#ifndef GC_TEST_FAKE_SIUSBXP_H
#define GC_TEST_FAKE_SIUSBXP_H

#include <SiUSBXp.h>

#include <string>
#include <vector>

namespace FakeSiUSBXp
{

enum class Operation
{
    GetNumDevices,
    GetProductStringVid,
    GetProductStringPid,
    Open,
    FlushBuffers,
    SetTimeouts,
    SetBaudRate,
    SetLineControl,
    SetFlowControl,
    Close,
    WindowsCloseHandle,
    Read,
    Write,
    Count
};

struct ProductQuery
{
    DWORD deviceNumber = 0;
    DWORD option = 0;
};

struct Snapshot
{
    std::vector<Operation> calls;
    std::vector<ProductQuery> productQueries;
    DWORD openedDevice = 0;
    HANDLE openedHandle = nullptr;
    int closeCalls = 0;
    HANDLE closedHandle = nullptr;
    BYTE flushTransmit = 0;
    BYTE flushReceive = 0;
    DWORD readTimeout = 0;
    DWORD writeTimeout = 0;
    DWORD baudRate = 0;
    WORD lineControl = 0;
    BYTE ctsMaskCode = 0;
    BYTE rtsMaskCode = 0;
    BYTE dtrMaskCode = 0;
    BYTE dsrMaskCode = 0;
    BYTE dcdMaskCode = 0;
    BOOL flowXonXoff = TRUE;
    DWORD requestedRead = 0;
    DWORD requestedWrite = 0;
};

void reset();
void addDevice(const std::string &vid, const std::string &pid);
void setResult(Operation operation, SI_STATUS result);
void setReportedDeviceCount(DWORD count);
void setReadResult(SI_STATUS result, DWORD bytesReturned);
void setWriteResult(SI_STATUS result, DWORD bytesWritten);

Snapshot snapshot();
HANDLE testHandle();
const char *operationName(Operation operation);

}

#endif
