/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OAuthCallbackPolicy.h"

#include <QByteArray>
#include <QList>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace {

constexpr int StateByteCount = 32;
constexpr int EncodedStateLength = 43;
constexpr int MaximumCodeLength = 8 * 1024;
constexpr int MaximumErrorLength = 1024;

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool hasUserInfo(const QUrl &url)
{
    return url.authority(QUrl::FullyEncoded)
        .contains(QLatin1Char('@'));
}

bool hasValidPort(const QUrl &url)
{
    const int port = url.port(-1);
    return port == -1 || (port > 0 && port <= 65535);
}

bool isLoopbackHost(const QString &host)
{
    return host.compare(
               QStringLiteral("localhost"),
               Qt::CaseInsensitive) == 0
        || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
}

QString exactPath(const QUrl &url)
{
    const QString path = url.path(QUrl::FullyEncoded);
    return path.isEmpty() ? QStringLiteral("/") : path;
}

bool constantTimeEquals(const QString &left,
                        const QString &right)
{
    const QByteArray leftBytes = left.toLatin1();
    const QByteArray rightBytes = right.toLatin1();
    if (leftBytes.size() != rightBytes.size()) {
        return false;
    }

    unsigned int difference = 0;
    for (qsizetype index = 0;
         index < leftBytes.size();
         ++index) {
        difference |=
            static_cast<unsigned char>(leftBytes.at(index))
            ^ static_cast<unsigned char>(rightBytes.at(index));
    }
    return difference == 0;
}

bool matchesRedirect(const QUrl &callback,
                     const QUrl &expected)
{
    return callback.isValid()
        && !callback.isRelative()
        && !hasUserInfo(callback)
        && !callback.hasFragment()
        && callback.scheme().compare(
               expected.scheme(),
               Qt::CaseInsensitive) == 0
        && callback.host().compare(
               expected.host(),
               Qt::CaseInsensitive) == 0
        && callback.port(-1) == expected.port(-1)
        && exactPath(callback) == exactPath(expected);
}

QList<QString> queryValues(const QUrlQuery &query,
                           const QString &name)
{
    return query.allQueryItemValues(
        name, QUrl::FullyDecoded);
}

} // namespace

namespace OAuthCallbackPolicy {

QString generateState()
{
    QByteArray randomBytes(StateByteCount, char(0));
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int index = 0;
         index < randomBytes.size();
         ++index) {
        randomBytes[index] =
            static_cast<char>(generator->generate() & 0xffu);
    }

    return QString::fromLatin1(
        randomBytes.toBase64(
            QByteArray::Base64UrlEncoding
            | QByteArray::OmitTrailingEquals));
}

bool isWellFormedState(const QString &state)
{
    if (state.size() != EncodedStateLength) {
        return false;
    }

    for (const QChar character : state) {
        const ushort value = character.unicode();
        const bool allowed =
            (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')
            || value == '-'
            || value == '_';
        if (!allowed) {
            return false;
        }
    }

    return true;
}

bool isSecureEndpoint(const QUrl &url)
{
    return url.isValid()
        && !url.isRelative()
        && url.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive) == 0
        && !url.host().isEmpty()
        && !hasUserInfo(url)
        && !url.hasFragment()
        && hasValidPort(url);
}

bool isSecureRedirectUri(const QUrl &url)
{
    if (!url.isValid()
        || url.isRelative()
        || url.host().isEmpty()
        || hasUserInfo(url)
        || url.hasQuery()
        || url.hasFragment()
        || !hasValidPort(url)) {
        return false;
    }

    if (url.scheme().compare(
            QStringLiteral("https"),
            Qt::CaseInsensitive) == 0) {
        return true;
    }

    return url.scheme().compare(
               QStringLiteral("http"),
               Qt::CaseInsensitive) == 0
        && isLoopbackHost(url.host());
}

QUrl authorizationUrlWithState(
    const QUrl &authorizationUrl,
    const QString &state,
    QString *error)
{
    if (error) {
        error->clear();
    }

    if (!isSecureEndpoint(authorizationUrl)) {
        setError(error,
                 QStringLiteral(
                     "OAuth authorization endpoint must use verified HTTPS."));
        return {};
    }
    if (!isWellFormedState(state)) {
        setError(error,
                 QStringLiteral(
                     "OAuth state is missing or malformed."));
        return {};
    }

    QUrl result = authorizationUrl;
    QUrlQuery query(result);
    query.removeAllQueryItems(QStringLiteral("state"));
    query.addQueryItem(QStringLiteral("state"), state);
    result.setQuery(query);
    return result;
}

bool isSuccessfulTokenReply(
    QNetworkReply::NetworkError error)
{
    return error == QNetworkReply::NoError;
}

Session::Session(const QUrl &redirectUri,
                 const QString &state)
    : redirectUri_(redirectUri),
      state_(state),
      valid_(isSecureRedirectUri(redirectUri)
             && isWellFormedState(state))
{
}

bool Session::isValid() const
{
    return valid_;
}

bool Session::isConsumed() const
{
    return consumed_;
}

Outcome Session::consume(const QUrl &callbackUrl)
{
    Outcome outcome;
    if (!valid_
        || consumed_
        || !matchesRedirect(
            callbackUrl, redirectUri_)) {
        return outcome;
    }

    const QUrlQuery query(callbackUrl);
    const QList<QString> states =
        queryValues(query, QStringLiteral("state"));
    const QList<QString> codes =
        queryValues(query, QStringLiteral("code"));
    const QList<QString> errors =
        queryValues(query, QStringLiteral("error"));
    const QList<QString> errorDescriptions =
        queryValues(
            query,
            QStringLiteral("error_description"));

    if (states.size() != 1
        || !constantTimeEquals(
            states.constFirst(), state_)
        || codes.size() > 1
        || errors.size() > 1
        || errorDescriptions.size() > 1
        || (!codes.isEmpty()
            && !errors.isEmpty())) {
        return outcome;
    }

    if (codes.size() == 1
        && !codes.constFirst().isEmpty()
        && codes.constFirst().size()
            <= MaximumCodeLength) {
        consumed_ = true;
        outcome.type =
            OutcomeType::AuthorizationCode;
        outcome.code = codes.constFirst();
        return outcome;
    }

    if (errors.size() == 1
        && !errors.constFirst().isEmpty()
        && errors.constFirst().size()
            <= MaximumErrorLength) {
        QString description =
            errorDescriptions.isEmpty()
                ? QString()
                : errorDescriptions.constFirst();
        if (description.size()
            > MaximumErrorLength) {
            return outcome;
        }

        consumed_ = true;
        outcome.type =
            OutcomeType::AuthorizationError;
        outcome.error =
            description.isEmpty()
                ? errors.constFirst()
                : description;
    }

    return outcome;
}

} // namespace OAuthCallbackPolicy
