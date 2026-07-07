/*
 * Copyright (c) 2011 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef WIN32
#include <QString>
#include <QDebug>
#include "USBXpress.h"

#ifdef GC_HAVE_USBXPRESS

namespace {

bool getDeviceCount(DWORD &count)
{
    count = 0;
    return SI_GetNumDevices(&count) == SI_SUCCESS;
}

bool getProductValue(const DWORD device, const DWORD option,
                     unsigned int &value)
{
    char buffer[128] = {};
    if (SI_GetProductString(device, buffer, option) != SI_SUCCESS)
        return false;

    buffer[sizeof(buffer) - 1] = '\0';
    bool ok = false;
    value = QString::fromLatin1(buffer).toUInt(&ok, 16);
    return ok;
}

bool isGarminUsb1(const DWORD device)
{
    unsigned int vid = 0;
    unsigned int pid = 0;
    return getProductValue(device, SI_RETURN_VID, vid) &&
            vid == GARMIN_USB1_VID &&
            getProductValue(device, SI_RETURN_PID, pid) &&
            pid == GARMIN_USB1_PID;
}

bool configureGarminUsb1(const HANDLE handle)
{
    return SI_FlushBuffers(handle, 1, 1) == SI_SUCCESS &&
            SI_SetTimeouts(5, 5) == SI_SUCCESS &&
            SI_SetBaudRate(handle, 115200) == SI_SUCCESS &&
            SI_SetLineControl(handle, 0x0800) == SI_SUCCESS &&
            SI_SetFlowControl(handle, SI_STATUS_INPUT, SI_HELD_INACTIVE,
                              SI_HELD_INACTIVE, SI_STATUS_INPUT,
                              SI_STATUS_INPUT, FALSE) == SI_SUCCESS;
}

}

USBXpress::USBXpress() {} // nothing to do - all members are static

bool USBXpress::find()
{
    DWORD numDevices = 0;
    if (!getDeviceCount(numDevices)) return false;

    for (DWORD device = 0; device < numDevices; ++device)
        if (isGarminUsb1(device)) return true;

    return false;
}

int USBXpress::open(HANDLE *handle)
{
    if (!handle) return -1;
    *handle = nullptr;

    DWORD numDevices = 0;
    if (!getDeviceCount(numDevices)) return -1;

    for (DWORD device = 0; device < numDevices; ++device) {
        if (!isGarminUsb1(device)) continue;

        HANDLE candidate = nullptr;
        if (SI_Open(device, &candidate) != SI_SUCCESS) continue;

        if (configureGarminUsb1(candidate)) {
            *handle = candidate;
            return 0;
        }

        SI_Close(candidate);
    }

    return -1;
}

int USBXpress::close(HANDLE *handle)
{
    if (!handle || !*handle) return -1;
    return SI_Close(*handle) == SI_SUCCESS ? 0 : -1;
}


int USBXpress::read(HANDLE *handle, unsigned char *buf, int bytes)
{
    DWORD read;
    if (SI_Read (*handle, buf, (DWORD) bytes, &read, NULL) == SI_SUCCESS)
        return read;
    else return -1;
}

int USBXpress::write(HANDLE *handle, unsigned char *buf, int bytes)
{
    DWORD wrote;
    if (SI_Write (*handle, buf, (DWORD) bytes, &wrote, NULL) == SI_SUCCESS)
        return wrote;
    else
        return -1;
}

#else

// if we don't have USBXpress installed then stubs return fail
USBXpress::USBXpress() {} // nothing to do - all members are static

int USBXpress::open(HANDLE *)
{
    return -1;
}

int USBXpress::close(HANDLE *)
{
    return -1;
}


int USBXpress::read(HANDLE *, unsigned char *, int)
{
    return -1;
}

int USBXpress::write(HANDLE *, unsigned char *, int)
{
    return -1;
}

#endif // USBXpress
#endif // Win32
