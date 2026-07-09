/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OpenDataEndpointPolicy_h
#define GC_OpenDataEndpointPolicy_h

#include <QList>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace OpenDataEndpointPolicy {

QUrl discoveryUrl();
bool isAllowedServerRoot(const QUrl &url);
QList<QUrl> allowedServerRootsFromDiscovery(const QByteArray &response,
                                            QString *error = nullptr);
QUrl metricsUrl(const QUrl &serverRoot);
QNetworkRequest makeRequest(const QUrl &url);
bool isSuccessfulResponse(QNetworkReply::NetworkError error,
                          int httpStatus,
                          const QUrl &redirectTarget);

} // namespace OpenDataEndpointPolicy

#endif
