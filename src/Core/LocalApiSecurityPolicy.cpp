/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "LocalApiSecurityPolicy.h"

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QList>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QVariant>

namespace {

constexpr int TokenByteCount = 32;
constexpr int EncodedTokenLength = 43;

const QByteArray BearerPrefix = QByteArrayLiteral("Bearer ");
const QByteArray HttpOriginPrefix = QByteArrayLiteral("http://");
const QString BearerTokenSetting =
    QStringLiteral("security/bearerToken");
const QString HostSetting = QStringLiteral("host");
const QString PortSetting = QStringLiteral("port");
const QString LoopbackHost = QStringLiteral("127.0.0.1");

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool settingsPathIsSafe(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        setError(error,
                 QStringLiteral(
                     "Local API settings do not have a file path."));
        return false;
    }

    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        setError(error,
                 QStringLiteral(
                     "Local API settings path is not a regular file."));
        return false;
    }

    return true;
}

bool secureSettingsFile(const QString &path, QString *error)
{
    QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || !info.isFile()) {
        setError(error,
                 QStringLiteral(
                     "Local API settings were not written to a regular file."));
        return false;
    }

#ifdef Q_OS_UNIX
    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QFile::setPermissions(path, ownerOnly)) {
        setError(error,
                 QStringLiteral(
                     "Cannot restrict Local API settings permissions."));
        return false;
    }

    info.refresh();
    const QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup
        | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup
        | QFileDevice::ReadOther
        | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    const QFileDevice::Permissions permissions = info.permissions();
    if ((permissions & nonOwnerPermissions)
            != QFileDevice::Permissions()
        || !permissions.testFlag(QFileDevice::ReadOwner)
        || !permissions.testFlag(QFileDevice::WriteOwner)) {
        setError(error,
                 QStringLiteral(
                     "Local API settings permissions are not owner-only."));
        return false;
    }
#else
    Q_UNUSED(error)
#endif

    return true;
}

QList<QByteArray> headerValues(
    const QMultiMap<QByteArray, QByteArray> &headers,
    const QByteArray &name)
{
    QList<QByteArray> values;
    for (auto iterator = headers.cbegin();
         iterator != headers.cend();
         ++iterator) {
        if (iterator.key().compare(name, Qt::CaseInsensitive) == 0) {
            values.append(iterator.value());
        }
    }
    return values;
}

bool isLoopbackAuthority(const QByteArray &authority, quint16 port)
{
    if (port == 0) {
        return false;
    }

    const QByteArray suffix =
        QByteArrayLiteral(":") + QByteArray::number(port);
    return authority.compare(
               QByteArrayLiteral("localhost") + suffix,
               Qt::CaseInsensitive) == 0
        || authority == QByteArrayLiteral("127.0.0.1") + suffix
        || authority == QByteArrayLiteral("[::1]") + suffix;
}

bool isAllowedOrigin(const QByteArray &origin, quint16 port)
{
    if (origin.size() <= HttpOriginPrefix.size()
        || origin.left(HttpOriginPrefix.size()).compare(
               HttpOriginPrefix, Qt::CaseInsensitive) != 0) {
        return false;
    }

    return isLoopbackAuthority(
        origin.mid(HttpOriginPrefix.size()), port);
}

bool constantTimeEquals(const QByteArray &left,
                        const QByteArray &right)
{
    if (left.size() != right.size()) {
        return false;
    }

    unsigned int difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index) {
        difference |=
            static_cast<unsigned char>(left.at(index))
            ^ static_cast<unsigned char>(right.at(index));
    }
    return difference == 0;
}

} // namespace

namespace LocalApiSecurityPolicy {

bool ensureSettingsFile(const QString &path,
                        const QByteArray &defaultContents,
                        QString *error)
{
    if (error) {
        error->clear();
    }

    if (!settingsPathIsSafe(path, error)) {
        return false;
    }

    const QFileInfo info(path);
    if (info.exists()) {
        return secureSettingsFile(path, error);
    }

    if (defaultContents.isEmpty()) {
        setError(error,
                 QStringLiteral(
                     "Local API default settings are unavailable."));
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error,
                 QStringLiteral(
                     "Cannot create Local API settings."));
        return false;
    }

#ifdef Q_OS_UNIX
    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(ownerOnly)) {
        file.cancelWriting();
        setError(error,
                 QStringLiteral(
                     "Cannot restrict new Local API settings permissions."));
        return false;
    }
#endif

    const qint64 bytesWritten = file.write(defaultContents);
    if (bytesWritten != defaultContents.size() || !file.commit()) {
        file.cancelWriting();
        setError(error,
                 QStringLiteral(
                     "Cannot write Local API settings."));
        return false;
    }

    return secureSettingsFile(path, error);
}

bool Configuration::isValid() const
{
    return port != 0 && isWellFormedBearerToken(bearerToken);
}

QByteArray generateBearerToken()
{
    QByteArray randomBytes(TokenByteCount, char(0));
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int index = 0; index < randomBytes.size(); ++index) {
        randomBytes[index] =
            static_cast<char>(generator->generate() & 0xffu);
    }

    return randomBytes.toBase64(
        QByteArray::Base64UrlEncoding
        | QByteArray::OmitTrailingEquals);
}

bool isWellFormedBearerToken(const QByteArray &token)
{
    if (token.size() != EncodedTokenLength) {
        return false;
    }

    for (const char character : token) {
        const unsigned char value =
            static_cast<unsigned char>(character);
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

Configuration prepareServerConfiguration(QSettings &settings,
                                         QString *error)
{
    if (error) {
        error->clear();
    }

    bool portIsValid = false;
    const int configuredPort =
        settings.value(PortSetting).toInt(&portIsValid);
    if (!portIsValid
        || configuredPort <= 0
        || configuredPort > 65535) {
        setError(error,
                 QStringLiteral(
                     "Local API port must be between 1 and 65535."));
        return {};
    }

    if (!settings.isWritable()
        || !settingsPathIsSafe(settings.fileName(), error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral(
                "Local API settings are not writable.");
        }
        return {};
    }

    QByteArray bearerToken =
        settings.value(BearerTokenSetting)
            .toString()
            .toLatin1();
    if (!isWellFormedBearerToken(bearerToken)) {
        bearerToken = generateBearerToken();
    }
    if (!isWellFormedBearerToken(bearerToken)) {
        setError(error,
                 QStringLiteral(
                     "Cannot generate a Local API bearer token."));
        return {};
    }

    settings.setValue(HostSetting, LoopbackHost);
    settings.setValue(
        BearerTokenSetting,
        QString::fromLatin1(bearerToken));
    settings.sync();

    if (settings.status() == QSettings::AccessError
        || settings.value(HostSetting).toString() != LoopbackHost
        || settings.value(BearerTokenSetting)
               .toString()
               .toLatin1() != bearerToken) {
        setError(error,
                 QStringLiteral(
                     "Cannot persist secure Local API settings."));
        return {};
    }

    if (!secureSettingsFile(settings.fileName(), error)) {
        return {};
    }

    Configuration configuration;
    configuration.bearerToken = bearerToken;
    configuration.port = static_cast<quint16>(configuredPort);
    return configuration;
}

Decision evaluateRequest(
    const QMultiMap<QByteArray, QByteArray> &headers,
    const QByteArray &expectedBearerToken,
    quint16 port)
{
    if (!isWellFormedBearerToken(expectedBearerToken)) {
        return Decision::RejectAuthorization;
    }

    const QList<QByteArray> hosts =
        headerValues(headers, QByteArrayLiteral("Host"));
    if (hosts.size() != 1
        || !isLoopbackAuthority(hosts.constFirst(), port)) {
        return Decision::RejectHost;
    }

    const QList<QByteArray> origins =
        headerValues(headers, QByteArrayLiteral("Origin"));
    if (origins.size() > 1
        || (origins.size() == 1
            && !isAllowedOrigin(origins.constFirst(), port))) {
        return Decision::RejectOrigin;
    }

    const QList<QByteArray> authorizations =
        headerValues(headers, QByteArrayLiteral("Authorization"));
    if (authorizations.size() != 1) {
        return Decision::RejectAuthorization;
    }

    const QByteArray authorization = authorizations.constFirst();
    if (authorization.size()
            != BearerPrefix.size() + expectedBearerToken.size()
        || authorization.left(BearerPrefix.size()).compare(
               BearerPrefix, Qt::CaseInsensitive) != 0
        || !constantTimeEquals(
               authorization.mid(BearerPrefix.size()),
               expectedBearerToken)) {
        return Decision::RejectAuthorization;
    }

    return Decision::Allow;
}

} // namespace LocalApiSecurityPolicy
