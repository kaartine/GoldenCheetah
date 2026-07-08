/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_NETWORK_REPLY_WAIT_H
#define GC_NETWORK_REPLY_WAIT_H

#include <functional>

class QNetworkReply;

enum class NetworkReplyWaitResult
{
    Finished,
    TimedOut,
    Interrupted
};

NetworkReplyWaitResult waitForNetworkReply(
    QNetworkReply *reply,
    int timeoutMs,
    const std::function<bool()> &interrupted = {});

#endif
