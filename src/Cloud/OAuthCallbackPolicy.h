/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OAuthCallbackPolicy_h
#define GC_OAuthCallbackPolicy_h

#include <QNetworkReply>
#include <QString>
#include <QUrl>

namespace OAuthCallbackPolicy {

enum class OutcomeType {
    Reject,
    AuthorizationCode,
    AuthorizationError
};

struct Outcome {
    OutcomeType type = OutcomeType::Reject;
    QString code;
    QString error;
};

QString generateState();
bool isWellFormedState(const QString &state);
bool isSecureEndpoint(const QUrl &url);
bool isSecureRedirectUri(const QUrl &url);
QUrl authorizationUrlWithState(const QUrl &authorizationUrl,
                               const QString &state,
                               QString *error = nullptr);
bool isSuccessfulTokenReply(QNetworkReply::NetworkError error);

class Session
{
public:
    Session(const QUrl &redirectUri, const QString &state);

    bool isValid() const;
    bool isConsumed() const;
    Outcome consume(const QUrl &callbackUrl);

private:
    QUrl redirectUri_;
    QString state_;
    bool valid_ = false;
    bool consumed_ = false;
};

} // namespace OAuthCallbackPolicy

#endif
