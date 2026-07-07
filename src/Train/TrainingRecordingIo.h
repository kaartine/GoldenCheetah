/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_TrainingRecordingIo_h
#define _GC_TrainingRecordingIo_h

#include <QByteArray>
#include <QIODevice>

namespace TrainingRecordingIo
{

enum class Failure {
    None,
    Open,
    Write,
    Flush
};

struct Result {
    Failure failure = Failure::None;
    qint64 bytesWritten = 0;

    bool ok() const { return failure == Failure::None; }
};

inline Result open(QIODevice &device, QIODevice::OpenMode mode)
{
    return device.open(mode) ? Result() : Result{Failure::Open, 0};
}

template<typename FlushFunction>
Result writeAndFlush(QIODevice &device, const QByteArray &data,
                     FlushFunction flush)
{
    const qint64 written = device.write(data);
    if (written != data.size()) return {Failure::Write, written};
    if (!flush()) return {Failure::Flush, written};
    return {Failure::None, written};
}

class Health
{
public:
    bool isHealthy() const { return firstFailure == Failure::None; }
    Failure failure() const { return firstFailure; }

    bool markFailure(Failure failure)
    {
        if (failure == Failure::None || !isHealthy()) return false;
        firstFailure = failure;
        return true;
    }

    void reset() { firstFailure = Failure::None; }

private:
    Failure firstFailure = Failure::None;
};

}

#endif
