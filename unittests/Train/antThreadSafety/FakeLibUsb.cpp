/*
 * Copyright (c) 2026
 *
 * Deterministic fake ANT transport for the DEV-003 regression tests.
 */

#include "LibUsb.h"

#include <QThread>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

const unsigned char AntSyncByte = 0xa4;
const unsigned char AntResetSystem = 0x4a;
const unsigned char AntNotificationStartup = 0x6f;
const std::chrono::milliseconds IdleReadWait(2);
const std::chrono::milliseconds InterleaveWait(500);

struct State
{
    FakeAntTransport::Snapshot snapshot;
    bool producerBarrier = false;
    bool workerWaiting = false;
    bool producerWaiting = false;
    unsigned long generation = 0;
    bool interleaveArmed = false;
    int interleaveMessages = 0;
    quintptr firstMessageThread = 0;
    QByteArray readBytes;
};

std::mutex stateMutex;
std::condition_variable stateChanged;
State state;

bool isMessage(const QByteArray &bytes)
{
    return !bytes.isEmpty() &&
            static_cast<unsigned char>(bytes.at(0)) == AntSyncByte;
}

QByteArray startupNotification()
{
    QByteArray frame;
    frame.append(static_cast<char>(AntSyncByte));
    frame.append(static_cast<char>(1));
    frame.append(static_cast<char>(AntNotificationStartup));
    frame.append(static_cast<char>(0));
    frame.append(static_cast<char>(AntSyncByte ^ 1 ^
                                   AntNotificationStartup));
    return frame;
}

void completeProducerBarrier()
{
    state.workerWaiting = false;
    state.producerWaiting = false;
    ++state.generation;
    stateChanged.notify_all();
}

}

namespace FakeAntTransport
{

void reset()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    Q_ASSERT(state.snapshot.liveInstances == 0);
    Q_ASSERT(!state.snapshot.leased);
    state = State();
}

void enableProducerReadBarrier()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    state.producerBarrier = true;
    state.workerWaiting = false;
    state.producerWaiting = false;
    ++state.generation;
    stateChanged.notify_all();
}

bool synchronizeProducerWithRead(const int timeoutMs)
{
    std::unique_lock<std::mutex> lock(stateMutex);
    if (!state.producerBarrier || !state.snapshot.leased) return false;

    const unsigned long generation = state.generation;
    state.producerWaiting = true;
    if (state.workerWaiting) completeProducerBarrier();
    else stateChanged.notify_all();

    const bool matched = stateChanged.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), [&]() {
                return !state.producerBarrier ||
                        state.generation != generation;
            });
    if (!matched) {
        state.producerWaiting = false;
        stateChanged.notify_all();
    }
    return matched && state.generation != generation;
}

void disableProducerReadBarrier()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    state.producerBarrier = false;
    state.workerWaiting = false;
    state.producerWaiting = false;
    ++state.generation;
    stateChanged.notify_all();
}

void armMessageInterleave()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    state.interleaveArmed = true;
    state.interleaveMessages = 0;
    state.firstMessageThread = 0;
    state.snapshot.firstMessageBlocked = false;
    state.snapshot.interleaveTimedOut = false;
}

bool waitForFirstMessageBlocked(const int timeoutMs)
{
    std::unique_lock<std::mutex> lock(stateMutex);
    return stateChanged.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), []() {
                return state.snapshot.firstMessageBlocked;
            });
}

bool waitForMessageWrites(const int count, const int timeoutMs)
{
    std::unique_lock<std::mutex> lock(stateMutex);
    return stateChanged.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), [count]() {
                return state.snapshot.messageWrites >= count;
            });
}

void queueReadBytes(const QByteArray &bytes)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    state.readBytes.append(bytes);
    stateChanged.notify_all();
}

Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return state.snapshot;
}

}

LibUsb::LibUsb(int)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    ++state.snapshot.liveInstances;
}

LibUsb::~LibUsb()
{
    close();
    std::lock_guard<std::mutex> lock(stateMutex);
    --state.snapshot.liveInstances;
    stateChanged.notify_all();
}

int LibUsb::open()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (state.snapshot.leased) return -1;
    opened = true;
    state.snapshot.leased = true;
    ++state.snapshot.openCount;
    stateChanged.notify_all();
    return 0;
}

void LibUsb::close()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!opened) return;
    opened = false;
    state.snapshot.leased = false;
    state.producerBarrier = false;
    state.workerWaiting = false;
    state.producerWaiting = false;
    ++state.generation;
    ++state.snapshot.closeCount;
    stateChanged.notify_all();
}

int LibUsb::read(char *buffer, const int bytes)
{
    return read(buffer, bytes, 10000);
}

int LibUsb::read(char *buffer, const int bytes, const int timeout)
{
    std::unique_lock<std::mutex> lock(stateMutex);
    if (!opened) return -ENODEV;

    if (!state.readBytes.isEmpty() && bytes > 0) {
        const int count = std::min(bytes,
                                   static_cast<int>(state.readBytes.size()));
        std::copy_n(state.readBytes.constData(), count, buffer);
        state.readBytes.remove(0, count);
        return count;
    }

    if (!state.producerBarrier) {
        ++state.snapshot.blockedReaders;
        stateChanged.notify_all();
        stateChanged.wait_for(lock, IdleReadWait);
        --state.snapshot.blockedReaders;
        stateChanged.notify_all();
        return opened ? -ETIMEDOUT : -ENODEV;
    }

    const unsigned long generation = state.generation;
    ++state.snapshot.blockedReaders;
    state.workerWaiting = true;
    if (state.producerWaiting) completeProducerBarrier();
    else stateChanged.notify_all();

    const bool matched = stateChanged.wait_for(
            lock, std::chrono::milliseconds(timeout), [&]() {
                return !opened || !state.producerBarrier ||
                        state.generation != generation;
            });

    --state.snapshot.blockedReaders;
    stateChanged.notify_all();

    if (!opened) return -ENODEV;
    if (matched && state.producerBarrier &&
        state.generation != generation && bytes > 0) {
        buffer[0] = 0;
        return 1;
    }

    if (state.generation == generation) state.workerWaiting = false;
    return -ETIMEDOUT;
}

int LibUsb::write(char *buffer, const int bytes)
{
    return write(buffer, bytes, 125);
}

int LibUsb::write(char *buffer, const int bytes, int)
{
    const QByteArray payload(buffer, bytes);
    const quintptr threadId =
            reinterpret_cast<quintptr>(QThread::currentThreadId());

    std::unique_lock<std::mutex> lock(stateMutex);
    if (!opened) return -ENODEV;

    FakeAntTransport::WriteCall call;
    call.bytes = payload;
    call.threadId = threadId;
    state.snapshot.writes.append(call);

    if (isMessage(payload)) {
        ++state.snapshot.messageWrites;
        if (payload.size() > 2 &&
            static_cast<unsigned char>(payload.at(2)) == AntResetSystem) {
            state.readBytes.append(startupNotification());
            stateChanged.notify_all();
        }

        stateChanged.notify_all();

        if (state.interleaveArmed && state.interleaveMessages == 0) {
            state.interleaveMessages = 1;
            state.firstMessageThread = threadId;
            state.snapshot.firstMessageBlocked = true;
            stateChanged.notify_all();

            const bool interleaved = stateChanged.wait_for(
                    lock, InterleaveWait, []() {
                        return state.interleaveMessages >= 2;
                    });
            state.snapshot.interleaveTimedOut = !interleaved;
            state.snapshot.firstMessageBlocked = false;
            stateChanged.notify_all();
        } else if (state.interleaveArmed &&
                   state.interleaveMessages == 1 &&
                   threadId != state.firstMessageThread) {
            state.interleaveMessages = 2;
            stateChanged.notify_all();
        }
    }

    return bytes;
}

bool LibUsb::find()
{
    return true;
}
