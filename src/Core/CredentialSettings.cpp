#include "CredentialSettings.h"

#include "Settings.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QUuid>

#include <utility>

namespace {

const QSet<QString> &credentialKeys()
{
    static const QSet<QString> keys = {
        QStringLiteral(GC_RWGPSPASS),
        QStringLiteral(GC_RWGPS_AUTH_TOKEN),
        QStringLiteral(GC_TTBPASS),
        QStringLiteral(GC_SPORTPLUSHEALTHPASS),
        QStringLiteral(GC_SELPASS),
        QStringLiteral(GC_WIKEY),
        QStringLiteral(GC_DVPASS),
        QStringLiteral(GC_DROPBOX_TOKEN),
        QStringLiteral(GC_WITHINGS_TOKEN),
        QStringLiteral(GC_WITHINGS_SECRET),
        QStringLiteral(GC_NOKIA_TOKEN),
        QStringLiteral(GC_NOKIA_REFRESH_TOKEN),
        QStringLiteral(GC_STRAVA_TOKEN),
        QStringLiteral(GC_STRAVA_REFRESH_TOKEN),
        QStringLiteral(GC_CYCLINGANALYTICS_TOKEN),
        QStringLiteral(GC_SIXCYCLE_PASS),
        QStringLiteral(GC_AZUM_ACCESS_TOKEN),
        QStringLiteral(GC_AZUM_REFRESH_TOKEN),
        QStringLiteral(GC_AZUM_USERKEY),
        QStringLiteral(GC_TREDICT_TOKEN),
        QStringLiteral(GC_TREDICT_REFRESH_TOKEN),
        QStringLiteral(GC_POLARFLOW_TOKEN),
        QStringLiteral(GC_SPORTTRACKS_TOKEN),
        QStringLiteral(GC_SPORTTRACKS_REFRESH_TOKEN),
        QStringLiteral(GC_XERTPASS),
        QStringLiteral(GC_XERT_TOKEN),
        QStringLiteral(GC_XERT_REFRESH_TOKEN),
        QStringLiteral(GC_NOLIO_ACCESS_TOKEN),
        QStringLiteral(GC_NOLIO_REFRESH_TOKEN)
    };
    return keys;
}

} // namespace

CredentialSettings::CredentialSettings(
    std::unique_ptr<CredentialStore> store)
    : store_(std::move(store))
{
}

bool CredentialSettings::isCredentialKey(const QString &key)
{
    return credentialKeys().contains(key);
}

QStringList CredentialSettings::credentialKeysForPrefix(
    const QString &prefix)
{
    QStringList result;
    for (const QString &key : credentialKeys()) {
        if (key.startsWith(prefix)) result.append(key);
    }
    result.sort();
    return result;
}

QString CredentialSettings::ensureScopeId(
    QSettings *settings,
    const QString &storageKey,
    const QString &preferredScopeId)
{
    if (!settings || storageKey.isEmpty()) return QString();

    const QString stored = settings->value(storageKey).toString();
    const QUuid storedId(stored);
    if (!storedId.isNull()) {
        const QString scopeId =
            storedId.toString(QUuid::WithoutBraces);
        if (stored != scopeId) {
            settings->setValue(storageKey, scopeId);
            settings->sync();
        }
        hardenSettingsFile(settings);
        return scopeId;
    }

    const QUuid preferredId(preferredScopeId);
    const QString scopeId = preferredId.isNull()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : preferredId.toString(QUuid::WithoutBraces);
    settings->setValue(storageKey, scopeId);
    settings->sync();
    hardenSettingsFile(settings);
    if (settings->status() != QSettings::NoError) {
        qWarning() << "Cannot persist credential store scope:"
                   << settings->fileName();
        settings->remove(storageKey);
        return QString();
    }
    return scopeId;
}

QString CredentialSettings::vaultKey(
    const QString &scopeId,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        credentialKey.toUtf8(), QCryptographicHash::Sha256).toHex();
    return scopeId + QLatin1Char('/')
        + QString::fromLatin1(digest);
}

void CredentialSettings::hardenSettingsFile(QSettings *settings)
{
#ifdef Q_OS_UNIX
    if (!settings) return;
    const QString fileName = settings->fileName();
    if (fileName.isEmpty() || !QFileInfo::exists(fileName)) return;
    if (!QFile::setPermissions(
            fileName,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        qWarning() << "Cannot restrict settings file permissions:"
                   << fileName;
    }
#else
    Q_UNUSED(settings)
#endif
}

QVariant CredentialSettings::value(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &defaultValue)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return defaultValue;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    CacheEntry entry;
    const bool haveCached = cached(key, &entry);
    if (settings->value(removalKey, false).toBool()) {
        scrubPlaintext(settings, plaintextKey);
        if (haveCached) {
            return entry.present && !entry.persisted
                ? QVariant(entry.value) : defaultValue;
        }

        QString error;
        const CredentialStore::Status status = store_
            ? store_->remove(key, &error)
            : CredentialStore::Status::Unavailable;
        if (status == CredentialStore::Status::Success
            || status == CredentialStore::Status::NotFound) {
            scrubPlaintext(settings, removalKey);
        } else {
            reportStoreError(
                QStringLiteral("remove"), credentialKey, error);
            settings->sync();
            hardenSettingsFile(settings);
        }
        cache(key, CacheEntry());
        return defaultValue;
    }

    if (haveCached) {
        if (entry.persisted && settings->contains(plaintextKey)) {
            scrubPlaintext(settings, plaintextKey);
        }
        if (entry.present || !settings->contains(plaintextKey)) {
            return entry.present ? QVariant(entry.value) : defaultValue;
        }
    }

    const CredentialStore::ReadResult result = store_
        ? store_->read(key)
        : CredentialStore::ReadResult{
              CredentialStore::Status::Unavailable,
              QString(), QStringLiteral("No credential store")};
    if (result.status == CredentialStore::Status::Success) {
        entry = {true, result.value, true};
        scrubPlaintext(settings, plaintextKey);
        cache(key, entry);
        return entry.value;
    }

    if (settings->contains(plaintextKey)) {
        const QString legacy =
            settings->value(plaintextKey).toString();
        if (legacy.isEmpty()) {
            scrubPlaintext(settings, plaintextKey);
            if (result.status == CredentialStore::Status::NotFound) {
                cache(key, entry);
            } else {
                reportStoreError(
                    QStringLiteral("read"), credentialKey,
                    result.error);
            }
            return defaultValue;
        }

        QString error;
        const CredentialStore::Status writeStatus = store_
            ? store_->write(key, legacy, &error)
            : CredentialStore::Status::Unavailable;
        const bool migrated =
            writeStatus == CredentialStore::Status::Success;
        if (!migrated) {
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                error.isEmpty() ? result.error : error);
            settings->sync();
            hardenSettingsFile(settings);
        }
        entry = {true, legacy, migrated};
        if (migrated) scrubPlaintext(settings, plaintextKey);
        cache(key, entry);
        return entry.value;
    }

    if (result.status != CredentialStore::Status::NotFound) {
        reportStoreError(
            QStringLiteral("read"), credentialKey, result.error);
    } else {
        cache(key, entry);
    }
    return defaultValue;
}

void CredentialSettings::setValue(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &value)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    const QString secret = value.toString();
    if (secret.isEmpty()) {
        remove(settings, scopeId, credentialKey, plaintextKey);
        return;
    }

    QString error;
    const CredentialStore::Status status = store_
        ? store_->write(key, secret, &error)
        : CredentialStore::Status::Unavailable;
    const bool persisted = status == CredentialStore::Status::Success;
    if (persisted) {
        settings->remove(removalKey);
        scrubPlaintext(settings, plaintextKey);
    } else {
        reportStoreError(
            QStringLiteral("write"), credentialKey, error);
        settings->sync();
        hardenSettingsFile(settings);
    }
    cache(key, {true, secret, persisted});
}

void CredentialSettings::remove(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return;
    }

    scrubPlaintext(settings, plaintextKey);
    const QString key = vaultKey(scopeId, credentialKey);
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    QString error;
    const CredentialStore::Status status = store_
        ? store_->remove(key, &error)
        : CredentialStore::Status::Unavailable;
    if (status == CredentialStore::Status::Success
        || status == CredentialStore::Status::NotFound) {
        scrubPlaintext(settings, removalKey);
    } else {
        settings->setValue(removalKey, true);
        settings->sync();
        hardenSettingsFile(settings);
        reportStoreError(
            QStringLiteral("remove"), credentialKey, error);
    }
    cache(key, CacheEntry());
}

void CredentialSettings::migratePlaintext(
    QSettings *settings,
    const QString &scopeId,
    const QString &prefix)
{
    if (!settings || scopeId.isEmpty()) return;
    for (const QString &key : credentialKeysForPrefix(prefix)) {
        const QString storedKey = plaintextKey(key);
        if (settings->contains(storedKey)) {
            value(settings, scopeId, key, storedKey, QVariant());
        }
    }
}

void CredentialSettings::clearCache()
{
    QMutexLocker locker(&cacheMutex_);
    cache_.clear();
}

bool CredentialSettings::cached(
    const QString &key,
    CacheEntry *entry) const
{
    QMutexLocker locker(&cacheMutex_);
    const auto found = cache_.constFind(key);
    if (found == cache_.cend()) return false;
    if (entry) *entry = found.value();
    return true;
}

void CredentialSettings::cache(
    const QString &key,
    const CacheEntry &entry)
{
    QMutexLocker locker(&cacheMutex_);
    cache_.insert(key, entry);
}

QString CredentialSettings::plaintextKey(
    const QString &credentialKey)
{
    const qsizetype marker = credentialKey.indexOf(QLatin1Char('>'));
    return marker >= 0 ? credentialKey.mid(marker + 1) : credentialKey;
}

QString CredentialSettings::pendingRemovalKey(
    const QString &scopeId,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        vaultKey(scopeId, credentialKey).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral("credential_store/pending_remove/")
        + QString::fromLatin1(digest);
}

void CredentialSettings::scrubPlaintext(
    QSettings *settings,
    const QString &key)
{
    if (!settings || key.isEmpty()) return;
    if (settings->contains(key)) settings->remove(key);
    settings->sync();
    hardenSettingsFile(settings);
}

void CredentialSettings::reportStoreError(
    const QString &operation,
    const QString &credentialKey,
    const QString &error)
{
    qWarning() << "Credential store" << operation
               << "failed for" << credentialKey
               << (error.isEmpty()
                       ? QStringLiteral("unknown error")
                       : error)
               << "New plaintext credential writes are disabled.";
}
