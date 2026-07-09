/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "MapPageSecurityPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace {

constexpr qsizetype MaximumTileTemplateLength = 4096;

int defaultPort(const QString &scheme)
{
    return scheme == QStringLiteral("https") ? 443 : 80;
}

bool hasUserInfo(const QUrl &url)
{
    return url.authority(QUrl::FullyEncoded)
        .contains(QLatin1Char('@'));
}

bool isLoopbackHost(const QString &host)
{
    return host.compare(
               QStringLiteral("localhost"),
               Qt::CaseInsensitive) == 0
        || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
}

bool hasControlCharacter(const QString &value)
{
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f) {
            return true;
        }
    }
    return false;
}

bool isValidNonce(const QString &nonce)
{
    if (nonce.size() < 16 || nonce.size() > 128) {
        return false;
    }

    for (const QChar character : nonce) {
        const ushort code = character.unicode();
        if (!((code >= 'a' && code <= 'z')
              || (code >= 'A' && code <= 'Z')
              || (code >= '0' && code <= '9')
              || code == '-'
              || code == '_')) {
            return false;
        }
    }
    return true;
}

QString normalizedResourcePath(const QUrl &url)
{
    QString path = url.path();
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    return path;
}

bool isAllowedQrcResource(
    MapPageSecurityPolicy::ResourceType resourceType,
    const QUrl &url)
{
    if (!url.host().isEmpty()
        || url.hasQuery()
        || !url.fragment().isEmpty()) {
        return false;
    }

    const QString path = normalizedResourcePath(url);
    switch (resourceType) {
    case MapPageSecurityPolicy::ResourceType::MainFrame:
        return path
            == QStringLiteral("/web/ride-map");
    case MapPageSecurityPolicy::ResourceType::Script:
        return path
                == QStringLiteral(
                    "/web/leaflet/leaflet.js")
            || path
                == QStringLiteral(
                    "/qtwebchannel/qwebchannel.js");
    case MapPageSecurityPolicy::ResourceType::StyleSheet:
        return path
            == QStringLiteral(
                "/web/leaflet/leaflet.css");
    case MapPageSecurityPolicy::ResourceType::Image:
        return path.startsWith(
                   QStringLiteral(
                       "/web/leaflet/images/"))
            || path.startsWith(
                   QStringLiteral("/images/maps/"));
    default:
        return false;
    }
}

QString hostForCsp(const QString &host)
{
    if (host.contains(QLatin1Char(':'))) {
        return QLatin1Char('[')
            + host
            + QLatin1Char(']');
    }
    return host;
}

} // namespace

namespace MapPageSecurityPolicy {

bool TileEndpoint::isValid() const
{
    return valid_;
}

bool TileEndpoint::allowsImage(const QUrl &url) const
{
    if (!valid_
        || !url.isValid()
        || url.isRelative()
        || hasUserInfo(url)
        || !url.fragment().isEmpty()
        || url.scheme().compare(
               scheme_, Qt::CaseInsensitive) != 0
        || url.port(defaultPort(scheme_)) != port_) {
        return false;
    }

    const QString host = url.host().toLower();
    if (!subdomains_) {
        return host == host_;
    }

    if (host.size() != hostSuffix_.size() + 2
        || host.at(1) != QLatin1Char('.')
        || host.mid(2) != hostSuffix_) {
        return false;
    }

    const QChar subdomain = host.at(0);
    return subdomain == QLatin1Char('a')
        || subdomain == QLatin1Char('b')
        || subdomain == QLatin1Char('c');
}

QString TileEndpoint::cspSource() const
{
    if (!valid_) {
        return QString();
    }

    QString source = scheme_ + QStringLiteral("://");
    if (subdomains_) {
        source += QStringLiteral("*.");
        source += hostSuffix_;
    } else {
        source += hostForCsp(host_);
    }

    if (port_ != defaultPort(scheme_)) {
        source += QLatin1Char(':')
            + QString::number(port_);
    }
    return source;
}

int safeMapType(int requested)
{
    Q_UNUSED(requested)
    return 0;
}

TileEndpoint tileEndpoint(const QString &tileTemplate)
{
    TileEndpoint endpoint;
    if (tileTemplate.isEmpty()
        || tileTemplate.size()
            > MaximumTileTemplateLength
        || tileTemplate.trimmed() != tileTemplate
        || hasControlCharacter(tileTemplate)) {
        return endpoint;
    }

    const int subdomainCount =
        tileTemplate.count(QStringLiteral("{s}"));
    const bool hasSubdomains = subdomainCount != 0;
    const bool validSubdomainPosition =
        tileTemplate.startsWith(
            QStringLiteral("https://{s}."),
            Qt::CaseInsensitive)
        || tileTemplate.startsWith(
            QStringLiteral("http://{s}."),
            Qt::CaseInsensitive);
    if ((hasSubdomains
         && (subdomainCount != 1
             || !validSubdomainPosition))
        || (!hasSubdomains
            && validSubdomainPosition)) {
        return endpoint;
    }

    QString candidate = tileTemplate;
    candidate.replace(
        QStringLiteral("{-y}"),
        QStringLiteral("0"));
    candidate.replace(
        QStringLiteral("{s}"),
        QStringLiteral("a"));
    candidate.replace(
        QStringLiteral("{z}"),
        QStringLiteral("0"));
    candidate.replace(
        QStringLiteral("{x}"),
        QStringLiteral("0"));
    candidate.replace(
        QStringLiteral("{y}"),
        QStringLiteral("0"));
    candidate.replace(
        QStringLiteral("{r}"),
        QStringLiteral("0"));
    if (candidate.contains(QLatin1Char('{'))
        || candidate.contains(QLatin1Char('}'))) {
        return endpoint;
    }

    const QUrl url(candidate, QUrl::StrictMode);
    if (!url.isValid()
        || url.isRelative()
        || url.host().isEmpty()
        || hasUserInfo(url)
        || !url.fragment().isEmpty()
        || url.toEncoded().size()
            > MaximumTileTemplateLength) {
        return endpoint;
    }

    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    if ((scheme != QStringLiteral("https")
         && scheme != QStringLiteral("http"))
        || (scheme == QStringLiteral("http")
            && !isLoopbackHost(host))
        || host.startsWith(QLatin1Char('.'))
        || host.endsWith(QLatin1Char('.'))) {
        return endpoint;
    }

    endpoint.scheme_ = scheme;
    endpoint.port_ = url.port(defaultPort(scheme));
    endpoint.subdomains_ = hasSubdomains;
    if (hasSubdomains) {
        if (!host.startsWith(QStringLiteral("a."))
            || host.size() <= 2) {
            return TileEndpoint();
        }
        endpoint.hostSuffix_ = host.mid(2);
    } else {
        endpoint.host_ = host;
    }
    endpoint.valid_ = true;
    return endpoint;
}

bool allowsRequest(ResourceType resourceType,
                   const QUrl &url,
                   const TileEndpoint &tileEndpoint)
{
    if (!tileEndpoint.isValid()
        || !url.isValid()
        || url.isRelative()) {
        return false;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("qrc")) {
        return isAllowedQrcResource(
            resourceType, url);
    }
    if (resourceType == ResourceType::MainFrame) {
        return scheme == QStringLiteral("data");
    }
    if (resourceType == ResourceType::Image
        && scheme == QStringLiteral("data")) {
        return true;
    }
    return resourceType == ResourceType::Image
        && tileEndpoint.allowsImage(url);
}

bool allowsMainFrameNavigation(const QUrl &url)
{
    if (!url.isValid()
        || url.isRelative()
        || url.scheme().compare(
               QStringLiteral("qrc"),
               Qt::CaseInsensitive) != 0) {
        return false;
    }

    return isAllowedQrcResource(
        ResourceType::MainFrame, url);
}

void MainFrameNavigationGate::authorizeSetHtml()
{
    trustedSetHtmlPending_ = true;
}

void MainFrameNavigationGate::reset()
{
    trustedSetHtmlPending_ = false;
}

bool MainFrameNavigationGate::allowsNavigation(
    const QUrl &url)
{
    const bool trustedSetHtmlPending =
        trustedSetHtmlPending_;
    trustedSetHtmlPending_ = false;

    if (allowsMainFrameNavigation(url)) {
        return true;
    }

    return trustedSetHtmlPending
        && url.isValid()
        && !url.isRelative()
        && url.scheme().compare(
               QStringLiteral("data"),
               Qt::CaseInsensitive) == 0;
}

QString javaScriptStringLiteral(const QString &value)
{
    QJsonArray values;
    values.append(value);
    QByteArray json =
        QJsonDocument(values).toJson(
            QJsonDocument::Compact);
    if (json.size() < 2) {
        return QStringLiteral("\"\"");
    }
    json.remove(0, 1);
    json.chop(1);

    QString literal = QString::fromUtf8(json);
    literal.replace(
        QStringLiteral("<"),
        QStringLiteral("\\u003c"));
    literal.replace(
        QStringLiteral(">"),
        QStringLiteral("\\u003e"));
    literal.replace(
        QStringLiteral("&"),
        QStringLiteral("\\u0026"));
    literal.replace(
        QChar(0x2028),
        QStringLiteral("\\u2028"));
    literal.replace(
        QChar(0x2029),
        QStringLiteral("\\u2029"));
    return literal;
}

QString contentSecurityPolicy(
    const QString &nonce,
    const TileEndpoint &tileEndpoint)
{
    if (!isValidNonce(nonce)
        || !tileEndpoint.isValid()) {
        return QString();
    }

    return QStringLiteral(
               "default-src 'none'; "
               "script-src qrc: 'nonce-%1'; "
               "style-src qrc: 'unsafe-inline'; "
               "img-src qrc: data: %2; "
               "connect-src 'none'; "
               "font-src 'none'; "
               "object-src 'none'; "
               "frame-src 'none'; "
               "worker-src 'none'; "
               "base-uri 'none'; "
               "form-action 'none'")
        .arg(nonce, tileEndpoint.cspSource());
}

QString leafletScriptUrl()
{
    return QStringLiteral(
        "qrc:/web/leaflet/leaflet.js");
}

QString leafletStyleSheetUrl()
{
    return QStringLiteral(
        "qrc:/web/leaflet/leaflet.css");
}

} // namespace MapPageSecurityPolicy
