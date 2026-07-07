/*
 * Minimal public USBXpress API surface used by src/Train/USBXpress.cpp.
 */

#ifndef GC_TEST_SIUSBXP_H
#define GC_TEST_SIUSBXP_H

#include <windows.h>

typedef BYTE SI_STATUS;

#define SI_SUCCESS ((SI_STATUS) 0x00)
#define SI_INVALID_HANDLE ((SI_STATUS) 0x01)
#define SI_READ_ERROR ((SI_STATUS) 0x02)
#define SI_WRITE_ERROR ((SI_STATUS) 0x04)
#define SI_DEVICE_IO_FAILED ((SI_STATUS) 0x08)
#define SI_DEVICE_NOT_FOUND ((SI_STATUS) 0xff)

#define SI_RETURN_SERIAL_NUMBER 0x00
#define SI_RETURN_DESCRIPTION 0x01
#define SI_RETURN_LINK_NAME 0x02
#define SI_RETURN_VID 0x03
#define SI_RETURN_PID 0x04

#define SI_HELD_INACTIVE 0x00
#define SI_STATUS_INPUT 0x01

#ifdef __cplusplus
extern "C" {
#endif

SI_STATUS SI_GetNumDevices(LPDWORD numberOfDevices);
SI_STATUS SI_GetProductString(DWORD deviceNumber, LPVOID deviceString,
                              DWORD options);
SI_STATUS SI_Open(DWORD deviceNumber, HANDLE *handle);
SI_STATUS SI_Close(HANDLE handle);
SI_STATUS SI_Read(HANDLE handle, LPVOID buffer, DWORD bytesToRead,
                  LPDWORD bytesReturned, LPOVERLAPPED overlapped);
SI_STATUS SI_Write(HANDLE handle, LPVOID buffer, DWORD bytesToWrite,
                   LPDWORD bytesWritten, LPOVERLAPPED overlapped);
SI_STATUS SI_FlushBuffers(HANDLE handle, BYTE flushTransmit,
                          BYTE flushReceive);
SI_STATUS SI_SetTimeouts(DWORD readTimeout, DWORD writeTimeout);
SI_STATUS SI_SetBaudRate(HANDLE handle, DWORD baudRate);
SI_STATUS SI_SetLineControl(HANDLE handle, WORD lineControl);
SI_STATUS SI_SetFlowControl(HANDLE handle, BYTE ctsMaskCode,
                            BYTE rtsMaskCode, BYTE dtrMaskCode,
                            BYTE dsrMaskCode, BYTE dcdMaskCode,
                            BOOL flowXonXoff);

#ifdef __cplusplus
}
#endif

#endif
