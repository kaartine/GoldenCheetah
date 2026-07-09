/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_MapPageSecurityPolicy_h
#define GC_MapPageSecurityPolicy_h

#include <QString>
#include <QUrl>

namespace MapPageSecurityPolicy {

enum class ResourceType {
    MainFrame,
    Script,
    StyleSheet,
    Image,
    Other
};

class TileEndpoint
{
public:
    bool isValid() const;
    bool allowsImage(const QUrl &url) const;
    QString cspSource() const;

private:
    friend TileEndpoint tileEndpoint(
        const QString &tileTemplate);

    bool valid_ = false;
    bool subdomains_ = false;
    QString scheme_;
    QString host_;
    QString hostSuffix_;
    int port_ = -1;
};

class MainFrameNavigationGate
{
public:
    void authorizeSetHtml();
    void reset();
    bool allowsNavigation(const QUrl &url);

private:
    bool trustedSetHtmlPending_ = false;
};

int safeMapType(int requested);
TileEndpoint tileEndpoint(const QString &tileTemplate);
bool allowsRequest(ResourceType resourceType,
                   const QUrl &url,
                   const TileEndpoint &tileEndpoint);
bool allowsMainFrameNavigation(const QUrl &url);
QString javaScriptStringLiteral(const QString &value);
QString contentSecurityPolicy(const QString &nonce,
                              const TileEndpoint &tileEndpoint);
QString leafletScriptUrl();
QString leafletStyleSheetUrl();

} // namespace MapPageSecurityPolicy

#endif
