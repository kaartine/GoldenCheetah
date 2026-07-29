/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHEBACKGROUNDSAVER_H
#define GC_RIDECACHEBACKGROUNDSAVER_H

#include <functional>
#include <memory>

#include <QString>

namespace RideCacheSave {
struct Snapshot;
}

class RideCacheBackgroundSaver
{
public:
    using Writer = std::function<bool(
        const RideCacheSave::Snapshot&, QString&)>;
    inline static constexpr qint64 WaitTimeoutMs = 30000;

    explicit RideCacheBackgroundSaver(Writer writer = Writer());
    ~RideCacheBackgroundSaver();

    RideCacheBackgroundSaver(
        const RideCacheBackgroundSaver &) = delete;
    RideCacheBackgroundSaver &operator=(
        const RideCacheBackgroundSaver &) = delete;

    bool isRunning() const;
    bool enqueue(
        const std::shared_ptr<
            const RideCacheSave::Snapshot> &snapshot);
    bool writeAndWait(
        const std::shared_ptr<
            const RideCacheSave::Snapshot> &snapshot,
        QString &error,
        qint64 timeoutMs = WaitTimeoutMs);
    bool drain(QString *error = nullptr);
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif // GC_RIDECACHEBACKGROUNDSAVER_H
