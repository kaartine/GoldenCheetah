/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "LibUsb.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

#include <cerrno>

namespace {

QMutex stateMutex;
QWaitCondition stateChanged;
FakeAntTransport::Snapshot state;
int readDelayMs = 350;

}

namespace FakeAntTransport
{

void reset()
{
    QMutexLocker locker(&stateMutex);
    Q_ASSERT(state.liveInstances == 0);
    Q_ASSERT(!state.leased);
    state = Snapshot();
    readDelayMs = 350;
}

void setReadDelay(int delayMs)
{
    QMutexLocker locker(&stateMutex);
    readDelayMs = delayMs;
}

bool waitForBlockedReader(int timeoutMs)
{
    QMutexLocker locker(&stateMutex);
    QElapsedTimer timer;
    timer.start();

    while (state.blockedReaders == 0) {
        const qint64 remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0 ||
            !stateChanged.wait(&stateMutex, static_cast<unsigned long>(remaining))) {
            return state.blockedReaders > 0;
        }
    }
    return true;
}

Snapshot snapshot()
{
    QMutexLocker locker(&stateMutex);
    return state;
}

}

LibUsb::LibUsb(int)
{
    QMutexLocker locker(&stateMutex);
    ++state.liveInstances;
}

LibUsb::~LibUsb()
{
    close();

    QMutexLocker locker(&stateMutex);
    --state.liveInstances;
    stateChanged.wakeAll();
}

int LibUsb::open()
{
    QMutexLocker locker(&stateMutex);
    if (state.leased) return -1;

    opened = true;
    state.leased = true;
    ++state.openCount;
    stateChanged.wakeAll();
    return 0;
}

void LibUsb::close()
{
    QMutexLocker locker(&stateMutex);
    if (!opened) return;

    opened = false;
    state.leased = false;
    ++state.closeCount;
    stateChanged.wakeAll();
}

int LibUsb::read(char *buffer, int bytes)
{
    return read(buffer, bytes, readDelayMs);
}

int LibUsb::read(char *, int, int timeout)
{
    QMutexLocker locker(&stateMutex);
    if (!opened) return -ENODEV;

    ++state.blockedReaders;
    stateChanged.wakeAll();
    stateChanged.wait(&stateMutex, static_cast<unsigned long>(timeout));
    --state.blockedReaders;
    stateChanged.wakeAll();
    return -ETIMEDOUT;
}

int LibUsb::write(char *buffer, int bytes)
{
    return write(buffer, bytes, 125);
}

int LibUsb::write(char *, int bytes, int)
{
    QMutexLocker locker(&stateMutex);
    return opened ? bytes : -ENODEV;
}

bool LibUsb::find()
{
    return true;
}
