/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaPublisherTestSettings.h"

#include "Cloud/StravaSettingsCommit.h"
#include "Core/Settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QTemporaryDir>
#include <QtGlobal>
#include <QUuid>

#include <utility>

namespace {

struct TestSettingsState
{
    QMutex mutex;
    QHash<QString, QVariant> values;
    bool credentialReadsAvailable = true;
    bool failPendingStateWrite = false;
    bool failRevokedStateWrite = false;
    bool failTokenRemoval = false;
    int pendingWrites = 0;
    int removalWrites = 0;
    std::function<void()> pendingHook;
    QTemporaryDir transactionRoot;
};

TestSettingsState &testState()
{
    static TestSettingsState state;
    return state;
}

QString storedKey(const QString &accountKey, const QString &key)
{
    return accountKey + QLatin1Char('\n') + key;
}

GSettings testSettings(
    QStringLiteral("GoldenCheetahTests"),
    QStringLiteral("StravaAccountRemoval"));

} // namespace

GSettings *appsettings = &testSettings;

namespace StravaPublisherTestSettings {

void reset()
{
    const bool stopped =
        StravaSettingsCommit::shutdownCredentialThread();
    Q_ASSERT(stopped);
    if (stopped) {
        const bool restarted =
            StravaSettingsCommit::restartCredentialThread();
        Q_ASSERT(restarted);
    }

    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.values.clear();
    state.credentialReadsAvailable = true;
    state.failPendingStateWrite = false;
    state.failRevokedStateWrite = false;
    state.failTokenRemoval = false;
    state.pendingWrites = 0;
    state.removalWrites = 0;
    state.pendingHook = {};
    const QString root = state.transactionRoot.path();
    QDir(root).removeRecursively();
    QDir().mkpath(root);
}

void seedAuthorization(
    const QString &accountKey,
    const QString &accessToken,
    const QString &refreshToken)
{
    setValue(accountKey, GC_STRAVA_TOKEN, accessToken);
    setValue(accountKey, GC_STRAVA_REFRESH_TOKEN, refreshToken);
    setValue(
        accountKey,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("active"));
    setValue(accountKey, GC_STRAVA_REMOTE_GRANT_UNCERTAIN, false);
    setValue(
        accountKey,
        GC_STRAVA_AUTHORIZATION_REVISION,
        QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void setValue(
    const QString &accountKey,
    const QString &key,
    const QVariant &value)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    const QString combined = storedKey(accountKey, key);
    if (!value.isValid() || value.isNull())
        state.values.remove(combined);
    else
        state.values.insert(combined, value);
}

QVariant value(
    const QString &accountKey,
    const QString &key,
    const QVariant &fallback)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    return state.values.value(storedKey(accountKey, key), fallback);
}

void setCredentialReadsAvailable(bool available)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.credentialReadsAvailable = available;
}

void setPendingStateWriteFailure(bool fail)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.failPendingStateWrite = fail;
}

void setRevokedStateWriteFailure(bool fail)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.failRevokedStateWrite = fail;
}

void setTokenRemovalFailure(bool fail)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.failTokenRemoval = fail;
}

void setPendingStateHook(std::function<void()> hook)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    state.pendingHook = std::move(hook);
}

int pendingStateWrites()
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    return state.pendingWrites;
}

int tokenRemovalWrites()
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    return state.removalWrites;
}

} // namespace StravaPublisherTestSettings

GSettings::GSettings(QString, QString)
{
}

GSettings::~GSettings()
{
}

void GSettings::setValue(QString key, QVariant value)
{
    StravaPublisherTestSettings::setValue(QString(), key, value);
}

QVariant GSettings::cvalue(
    QString accountKey,
    QString key,
    QVariant fallback)
{
    return StravaPublisherTestSettings::value(
        accountKey, key, fallback);
}

GSettings::CredentialReadResult GSettings::credentialCValueChecked(
    const QString &accountKey,
    const QString &key)
{
    TestSettingsState &state = testState();
    QMutexLocker locker(&state.mutex);
    if (!state.credentialReadsAvailable)
        return {CredentialReadStatus::Unavailable, {}};
    const QString combined = storedKey(accountKey, key);
    if (!state.values.contains(combined))
        return {CredentialReadStatus::NotFound, {}};
    return {CredentialReadStatus::Present, state.values.value(combined)};
}

bool GSettings::setCValueChecked(
    QString accountKey,
    QString key,
    QVariant value)
{
    std::function<void()> pendingHook;
    {
        TestSettingsState &state = testState();
        QMutexLocker locker(&state.mutex);
        const QString text = value.toString();
        if (key == GC_STRAVA_AUTHORIZATION_STATE
            && text == QStringLiteral("revocation_pending")) {
            ++state.pendingWrites;
            if (state.failPendingStateWrite) return false;
            pendingHook = state.pendingHook;
        }
        if (key == GC_STRAVA_AUTHORIZATION_STATE
            && text == QStringLiteral("revoked")
            && state.failRevokedStateWrite) {
            return false;
        }
        if ((key == GC_STRAVA_TOKEN
             || key == GC_STRAVA_REFRESH_TOKEN)
            && (!value.isValid() || value.toString().isEmpty())) {
            ++state.removalWrites;
            if (state.failTokenRemoval) return false;
        }

        const QString combined = storedKey(accountKey, key);
        if (!value.isValid() || value.isNull())
            state.values.remove(combined);
        else
            state.values.insert(combined, value);
    }
    if (pendingHook) pendingHook();
    return true;
}

bool GSettings::syncCValueChecked(
    const QString &,
    const QString &)
{
    return true;
}

QString GSettings::athleteConfigDirectory(
    const QString &accountKey) const
{
    TestSettingsState &state = testState();
    if (!state.transactionRoot.isValid()
        || accountKey.trimmed().isEmpty()) {
        return {};
    }
    const QString directory = state.transactionRoot.filePath(
        QString::fromLatin1(
            QCryptographicHash::hash(
                accountKey.toUtf8(),
                QCryptographicHash::Sha256).toHex()));
    return QDir().mkpath(directory)
        ? QFileInfo(directory).canonicalFilePath()
        : QString();
}
