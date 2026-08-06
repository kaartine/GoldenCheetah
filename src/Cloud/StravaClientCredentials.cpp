/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaClientCredentials.h"

#include "StravaOAuthPolicy.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace {

constexpr qsizetype MaximumVaultRecordBytes = 16 * 1024;
constexpr int VaultRecordVersion = 1;

bool decodeVaultRecord(
    const QString &encoded,
    QString *clientId,
    QString *clientSecret)
{
    if (!clientId || !clientSecret) return false;
    clientId->clear();
    clientSecret->clear();
    const QByteArray bytes = encoded.toUtf8();
    if (bytes.isEmpty()
        || bytes.size() > MaximumVaultRecordBytes) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt(-1)
            != VaultRecordVersion
        || !object.value(QStringLiteral("client_id")).isString()
        || !object.value(QStringLiteral("client_secret")).isString()) {
        return false;
    }

    const QString decodedClientId =
        object.value(QStringLiteral("client_id")).toString();
    const QString decodedClientSecret =
        object.value(QStringLiteral("client_secret")).toString();
    if (!StravaOAuthPolicy::hasUsableCredentials(
            decodedClientId, decodedClientSecret)) {
        return false;
    }
    *clientId = decodedClientId;
    *clientSecret = decodedClientSecret;
    return true;
}

QString encodeVaultRecord(
    const QString &clientId,
    const QString &clientSecret)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), VaultRecordVersion);
    object.insert(QStringLiteral("client_id"), clientId);
    object.insert(QStringLiteral("client_secret"), clientSecret);
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

StravaClientCredentials::Resolution failure(
    StravaClientCredentials::Status status,
    const QString &error)
{
    StravaClientCredentials::Resolution result;
    result.status = status;
    result.error = error;
    return result;
}

} // namespace

namespace StravaClientCredentials {

Resolution resolve(
    Vault &vault,
    const QString &compileTimeClientId,
    const QString &compileTimeClientSecret)
{
    const VaultReadResult runtime = vault.read();
    if (runtime.status == VaultStatus::Unavailable) {
        return failure(
            Status::VaultUnavailable,
            QStringLiteral(
                "Strava OAuth client credentials could not be read securely."));
    }
    if (runtime.status == VaultStatus::Present) {
        QString clientId;
        QString clientSecret;
        if (!decodeVaultRecord(
                runtime.value, &clientId, &clientSecret)) {
            return failure(
                Status::InvalidRuntimeCredentials,
                QStringLiteral(
                    "Stored Strava OAuth client credentials are incomplete or invalid."));
        }
        Resolution result;
        result.status = Status::Available;
        result.credentials = {
            clientId,
            clientSecret,
            Source::RuntimeVault
        };
        return result;
    }
    if (runtime.status != VaultStatus::NotFound) {
        return failure(
            Status::VaultUnavailable,
            QStringLiteral(
                "Strava OAuth client credentials could not be read securely."));
    }

    if (!StravaOAuthPolicy::hasUsableCredentials(
            compileTimeClientId,
            compileTimeClientSecret)) {
        return failure(
            Status::Missing,
            QStringLiteral(
                "Strava OAuth client credentials are not configured."));
    }

    Resolution result;
    result.status = Status::Available;
    result.credentials = {
        compileTimeClientId,
        compileTimeClientSecret,
        Source::CompileTimeFallback
    };
    return result;
}

MutationResult store(
    Vault &vault,
    const QString &clientId,
    const QString &clientSecret)
{
    if (!StravaOAuthPolicy::hasUsableCredentials(
            clientId, clientSecret)) {
        return {
            false,
            QStringLiteral(
                "Enter a valid numeric Strava client ID and client secret.")
        };
    }
    if (!vault.write(encodeVaultRecord(
            clientId, clientSecret))) {
        return {
            false,
            QStringLiteral(
                "Strava OAuth client credentials could not be stored securely.")
        };
    }
    return {true, QString()};
}

MutationResult clear(Vault &vault)
{
    if (!vault.remove()) {
        return {
            false,
            QStringLiteral(
                "Strava OAuth client credentials could not be removed securely.")
        };
    }
    return {true, QString()};
}

MutationResult applyUserEdit(
    Vault &vault,
    const QString &clientId,
    const QString &clientSecret,
    UserAction action)
{
    if (action == UserAction::Remove) {
        return clear(vault);
    }
    if (action != UserAction::SaveIfProvided) {
        return {
            false,
            QStringLiteral(
                "The Strava OAuth credential action is invalid.")
        };
    }
    if (!clientId.isEmpty() || !clientSecret.isEmpty()) {
        return store(vault, clientId, clientSecret);
    }

    const Resolution current = resolve(vault, QString(), QString());
    if (current.status == Status::VaultUnavailable) {
        return {false, current.error};
    }
    if (current.status == Status::InvalidRuntimeCredentials) {
        return {
            false,
            QStringLiteral(
                "Stored Strava OAuth client credentials are invalid. "
                "Replace them or remove them explicitly.")
        };
    }
    return {true, QString()};
}

} // namespace StravaClientCredentials
