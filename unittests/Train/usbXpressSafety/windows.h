/*
 * Minimal Windows type shim for compiling USBXpress.cpp on Linux.
 * This is test-only and intentionally does not emulate the Windows API.
 */

#ifndef GC_TEST_USBXPRESS_WINDOWS_H
#define GC_TEST_USBXPRESS_WINDOWS_H

#include <stddef.h>
#include <stdint.h>

typedef void *HANDLE;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *LPVOID;
typedef DWORD *LPDWORD;

typedef struct _OVERLAPPED {
    uintptr_t testReserved;
} OVERLAPPED;

typedef struct _DCB {
    DWORD testReserved;
} DCB;

typedef OVERLAPPED *LPOVERLAPPED;

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

BOOL CloseHandle(HANDLE handle);

#ifdef __cplusplus
}
#endif

#endif
