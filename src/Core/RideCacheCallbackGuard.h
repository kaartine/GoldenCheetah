/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDE_CACHE_CALLBACK_GUARD_H
#define GC_RIDE_CACHE_CALLBACK_GUARD_H

#include <QPointer>

class RideCacheCallbackGuard
{
public:
    RideCacheCallbackGuard(
        QObject *cache, QObject *context,
        QObject *ride, QObject *estimator)
        : cache_(cache), context_(context),
          ride_(ride), estimator_(estimator)
    {
    }

    bool allAlive() const
    {
        return cache_ && context_ && ride_ && estimator_;
    }

private:
    QPointer<QObject> cache_;
    QPointer<QObject> context_;
    QPointer<QObject> ride_;
    QPointer<QObject> estimator_;
};

#endif
