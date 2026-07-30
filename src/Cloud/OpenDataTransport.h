/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_TRANSPORT_H
#define GC_OPEN_DATA_TRANSPORT_H

#include "OpenDataExport.h"

#include <QList>
#include <QNetworkRequest>
#include <QUrl>

#include <functional>

class QNetworkReply;

namespace OpenDataTransport {

struct Policy
{
    QUrl discoveryUrl;
    std::function<QList<QUrl>(const QByteArray &, QString *)>
        parseServerRoots;
    std::function<QUrl(const QUrl &)> metricsUrl;
    std::function<QNetworkRequest(const QUrl &)> makeRequest;
    std::function<void(const QNetworkReply *)> replyObserver;
    int requestTimeoutMs = 30 * 1000;
    int serverSearchTimeoutMs = 60 * 1000;
    int uploadTimeoutMs = 5 * 60 * 1000;
};

Policy productionPolicy();

OpenDataExport::UploadResult upload(
    const OpenDataExport::Request &request,
    const OpenDataExport::CancellationCheck &cancelled,
    const OpenDataExport::ProgressCallback &progress,
    const QByteArray &secret,
    const Policy &policy);

} // namespace OpenDataTransport

#endif
