/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaClientCredentials.h"

#include "Secrets.h"
#include "Settings.h"
#include "StravaOAuthPolicy.h"

#include <QVariant>

#include <utility>

namespace {

class SettingsVault final : public StravaClientCredentials::Vault
{
public:
    SettingsVault(GSettings *settings, QString accountKey)
        : settings_(settings), accountKey_(std::move(accountKey))
    {
    }

    StravaClientCredentials::VaultReadResult read() override
    {
        if (!settings_ || accountKey_.trimmed().isEmpty()) {
            return {
                StravaClientCredentials::VaultStatus::Unavailable,
                QString()
            };
        }
        const GSettings::CredentialReadResult result =
            settings_->credentialCValueChecked(
                accountKey_, GC_STRAVA_CLIENT_CREDENTIALS);
        switch (result.status) {
        case GSettings::CredentialReadStatus::Present:
            return {
                StravaClientCredentials::VaultStatus::Present,
                result.value.toString()
            };
        case GSettings::CredentialReadStatus::NotFound:
            return {
                StravaClientCredentials::VaultStatus::NotFound,
                QString()
            };
        case GSettings::CredentialReadStatus::Unavailable:
            break;
        }
        return {
            StravaClientCredentials::VaultStatus::Unavailable,
            QString()
        };
    }

    bool write(const QString &value) override
    {
        return settings_
            && !accountKey_.trimmed().isEmpty()
            && settings_->setCValueChecked(
                accountKey_, GC_STRAVA_CLIENT_CREDENTIALS,
                value);
    }

    bool remove() override
    {
        return settings_
            && !accountKey_.trimmed().isEmpty()
            && settings_->setCValueChecked(
                accountKey_, GC_STRAVA_CLIENT_CREDENTIALS,
                QVariant());
    }

private:
    GSettings *settings_ = nullptr;
    QString accountKey_;
};

SettingsVault accountVault(const QString &accountKey)
{
    return SettingsVault(appsettings, accountKey);
}

} // namespace

namespace StravaClientCredentials {

Resolution resolveForAccount(const QString &accountKey)
{
    SettingsVault vault = accountVault(accountKey);
    return resolve(
        vault,
        QStringLiteral(GC_STRAVA_CLIENT_ID),
        QStringLiteral(GC_STRAVA_CLIENT_SECRET));
}

Resolution runtimeForAccount(const QString &accountKey)
{
    SettingsVault vault = accountVault(accountKey);
    return resolve(vault, QString(), QString());
}

MutationResult storeForAccount(
    const QString &accountKey,
    const QString &clientId,
    const QString &clientSecret)
{
    SettingsVault vault = accountVault(accountKey);
    return store(vault, clientId, clientSecret);
}

MutationResult clearForAccount(const QString &accountKey)
{
    SettingsVault vault = accountVault(accountKey);
    return clear(vault);
}

MutationResult applyUserEditForAccount(
    const QString &accountKey,
    const QString &clientId,
    const QString &clientSecret,
    UserAction action)
{
    SettingsVault vault = accountVault(accountKey);
    return applyUserEdit(
        vault, clientId, clientSecret, action);
}

bool compileTimeFallbackIsConfigured()
{
    return StravaOAuthPolicy::hasUsableCredentials(
        QStringLiteral(GC_STRAVA_CLIENT_ID),
        QStringLiteral(GC_STRAVA_CLIENT_SECRET));
}

} // namespace StravaClientCredentials
