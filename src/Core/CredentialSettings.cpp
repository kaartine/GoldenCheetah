#include "CredentialSettings.h"

#include "Settings.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <utility>

namespace {

QMutex &credentialOperationMutex()
{
    static QMutex mutex;
    return mutex;
}

QString credentialStateDirectory()
{
    QString root;
#ifdef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
    root = qEnvironmentVariable(
        "GC_CREDENTIAL_TEST_STATE_ROOT");
#endif
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation);
    }
    if (root.isEmpty())
        return {};

    const QString path = QDir(root).filePath(
        QStringLiteral("GoldenCheetah/credential-locks"));
    if (!QDir().mkpath(path))
        return {};
    const QFileInfo directory(path);
    if (!directory.isDir() || directory.isSymLink())
        return {};
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner)) {
        return {};
    }
#endif
    const QString canonical = directory.canonicalFilePath();
    return canonical.isEmpty() ? directory.absoluteFilePath()
                               : canonical;
}

QString canonicalSettingsFileName(QSettings *settings)
{
    if (!settings || settings->fileName().isEmpty())
        return {};
#ifdef Q_OS_WIN
    if (settings->format() == QSettings::NativeFormat) {
        QString registryPath = settings->fileName().trimmed();
        registryPath.replace(
            QLatin1Char('/'), QLatin1Char('\\'));
        while (registryPath.contains(
                   QStringLiteral("\\\\"))) {
            registryPath.replace(
                QStringLiteral("\\\\"),
                QStringLiteral("\\"));
        }
        while (registryPath.size() > 1
               && registryPath.endsWith(
                   QLatin1Char('\\'))) {
            registryPath.chop(1);
        }
        return QStringLiteral("registry:")
            + registryPath.toCaseFolded();
    }
#endif
    const QFileInfo file(settings->fileName());
    const QString canonical = file.canonicalFilePath();
    if (!canonical.isEmpty())
        return QStringLiteral("file:") + canonical;

    const QFileInfo parent(file.absolutePath());
    const QString canonicalParent =
        parent.canonicalFilePath();
    if (!canonicalParent.isEmpty()) {
        return QStringLiteral("file:")
            + QDir(canonicalParent).filePath(
                file.fileName());
    }
    return QStringLiteral("file:")
        + QDir::cleanPath(file.absoluteFilePath());
}

class CredentialOperationGuard
{
public:
    explicit CredentialOperationGuard(
        const QString &operationId)
    {
        if (!credentialOperationMutex().tryLock())
            return;
        localLocked_ = true;

        const QString directory =
            credentialStateDirectory();
        if (directory.isEmpty())
            return;
        const QByteArray digest = QCryptographicHash::hash(
            operationId.toUtf8(),
            QCryptographicHash::Sha256).toHex();
        stateBasePath_ = QDir(directory).filePath(
            QString::fromLatin1(digest));
        processLock_ = std::make_unique<QLockFile>(
            stateBasePath_ + QStringLiteral(".lock"));
        processLock_->setStaleLockTime(0);
        if (!processLock_->tryLock(0))
            return;
        admitted_ = true;
    }

    ~CredentialOperationGuard()
    {
        if (processLock_ && processLock_->isLocked())
            processLock_->unlock();
        if (localLocked_)
            credentialOperationMutex().unlock();
    }

    explicit operator bool() const
    {
        return admitted_;
    }

    QString revisionPath() const
    {
        return stateBasePath_ + QStringLiteral(".revision");
    }

private:
    bool localLocked_ = false;
    bool admitted_ = false;
    QString stateBasePath_;
    std::unique_ptr<QLockFile> processLock_;
    Q_DISABLE_COPY(CredentialOperationGuard)
};

struct CredentialRevision
{
    bool readable = false;
    QByteArray value;
};

CredentialRevision readCredentialRevision(
    const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo information(path);
    if (!information.exists())
        return {true, QByteArray()};
    if (!information.isFile() || information.isSymLink())
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray contents = file.read(34);
    if (!file.atEnd())
        return {};
    const QByteArray revision = contents.trimmed();
    if (revision.size() != 32) return {};
    for (const char character : revision) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return {};
        }
    }
    return {true, revision};
}

bool advanceCredentialRevision(
    const QString &path,
    QByteArray *revision)
{
    if (path.isEmpty())
        return false;
    const QFileInfo existing(path);
    if (existing.exists()
        && (!existing.isFile() || existing.isSymLink())) {
        return false;
    }
    const QByteArray next =
        QUuid::createUuid().toRfc4122().toHex();
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
#ifdef Q_OS_UNIX
    if (!file.setPermissions(
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
#endif
    if (file.write(next) != next.size()
        || file.write("\n", 1) != 1
        || !file.commit()) {
        return false;
    }
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner)) {
        return false;
    }
#endif
    const CredentialRevision persisted =
        readCredentialRevision(path);
    if (!persisted.readable
        || persisted.value != next) {
        return false;
    }
    if (revision) *revision = next;
    return true;
}

CredentialRevision currentCredentialRevision(
    const QString &path)
{
    CredentialRevision current =
        readCredentialRevision(path);
    if (current.readable && !current.value.isEmpty())
        return current;

    QByteArray baseline;
    if (!advanceCredentialRevision(path, &baseline))
        return {};
    return {true, baseline};
}

std::unique_ptr<QSettings> freshExactSettings(
    QSettings *settings)
{
    if (!settings || settings->fileName().isEmpty())
        return {};
    auto exact = std::make_unique<QSettings>(
        settings->fileName(), settings->format());
    exact->setFallbacksEnabled(false);
    exact->setAtomicSyncRequired(true);
    const QString group = settings->group();
    if (!group.isEmpty())
        exact->beginGroup(group);
    return exact;
}

struct ExactSetting
{
    bool readable = false;
    bool present = false;
    QVariant value;
};

ExactSetting readExactSetting(
    QSettings *settings,
    const QString &key)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return {};
    exact->sync();
    if (exact->status() != QSettings::NoError)
        return {};
    const bool present = exact->contains(key);
    return {true, present,
            present ? exact->value(key) : QVariant()};
}

bool writeExactSetting(
    QSettings *settings,
    const QString &key,
    const QVariant &value)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->setValue(key, value);
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    return exact->status() == QSettings::NoError
        && exact->contains(key)
        && exact->value(key) == value;
}

bool removeExactSetting(
    QSettings *settings,
    const QString &key)
{
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->remove(key);
    exact->sync();
    CredentialSettings::hardenSettingsFile(settings);
    return exact->status() == QSettings::NoError
        && !exact->contains(key);
}

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
    const QString settingsIdentity =
        canonicalSettingsFileName(settings);
    if (settingsIdentity.isEmpty()) return QString();
    const QString operationId =
        QStringLiteral("scope\n")
        + settingsIdentity + QLatin1Char('\n')
        + settings->group()
        + QLatin1Char('\n') + storageKey;
    CredentialOperationGuard operation(operationId);
    if (!operation)
        return QString();

    const ExactSetting storedSetting =
        readExactSetting(settings, storageKey);
    if (!storedSetting.readable)
        return QString();
    const QString stored = storedSetting.value.toString();
    const QUuid storedId(stored);
    if (!storedId.isNull()) {
        const QString scopeId =
            storedId.toString(QUuid::WithoutBraces);
        if (stored != scopeId
            && !writeExactSetting(
                settings, storageKey, scopeId)) {
            return QString();
        }
        return scopeId;
    }

    const QUuid preferredId(preferredScopeId);
    const QString scopeId = preferredId.isNull()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : preferredId.toString(QUuid::WithoutBraces);
    if (!writeExactSetting(settings, storageKey, scopeId)) {
        qWarning() << "Cannot persist credential store scope:"
                   << settings->fileName();
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
    CredentialOperationGuard operation(key);
    if (!operation)
        return defaultValue;
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    const ExactSetting removal =
        readExactSetting(settings, removalKey);
    if (!removal.readable)
        return defaultValue;
    if (removal.present && removal.value.toBool()) {
        completePendingRemoval(
            settings, key, credentialKey, plaintextKey,
            removalKey, operation.revisionPath());
        return defaultValue;
    }

    const ExactSetting plaintext =
        readExactSetting(settings, plaintextKey);
    if (!plaintext.readable)
        return defaultValue;

    const CredentialRevision revision =
        currentCredentialRevision(operation.revisionPath());
    CacheEntry entry;
    bool haveCached =
        revision.readable && cached(key, &entry);
    if (haveCached && entry.revision != revision.value) {
        invalidateCache(key);
        haveCached = false;
    } else if (!revision.readable) {
        invalidateCache(key);
    }

    if (haveCached) {
        if (entry.persisted && plaintext.present) {
            scrubPlaintext(settings, plaintextKey);
        }
        if (entry.present || !plaintext.present) {
            return entry.present ? QVariant(entry.value) : defaultValue;
        }
    }

    const CredentialStore::ReadResult result = store_
        ? store_->read(key)
        : CredentialStore::ReadResult{
              CredentialStore::Status::Unavailable,
              QString(), QStringLiteral("No credential store")};
    if (result.status == CredentialStore::Status::Success) {
        entry = {true, result.value, true, revision.value};
        const bool scrubbed =
            scrubPlaintext(settings, plaintextKey);
        if (revision.readable && scrubbed) {
            cache(key, entry);
        } else {
            invalidateCache(key);
        }
        return entry.value;
    }

    if (plaintext.present) {
        const QString legacy = plaintext.value.toString();
        if (legacy.isEmpty()) {
            const bool scrubbed =
                scrubPlaintext(settings, plaintextKey);
            if (result.status
                == CredentialStore::Status::NotFound) {
                if (scrubbed && revision.readable) {
                    entry.revision = revision.value;
                    cache(key, entry);
                } else {
                    invalidateCache(key);
                }
            } else {
                invalidateCache(key);
                reportStoreError(
                    QStringLiteral("read"), credentialKey,
                    result.error);
            }
            return defaultValue;
        }

        // Only an authoritative miss permits a migration write. A transient
        // read failure may be hiding a newer credential already in the vault.
        if (result.status != CredentialStore::Status::NotFound) {
            reportStoreError(
                QStringLiteral("read"), credentialKey,
                result.error);
            hardenSettingsFile(settings);
            return defaultValue;
        }

        QByteArray migrationRevision;
        if (!advanceCredentialRevision(
                operation.revisionPath(),
                &migrationRevision)) {
            invalidateCache(key);
            reportStoreError(
                QStringLiteral("migrate"), credentialKey,
                QStringLiteral(
                    "Cannot persist credential revision"));
            hardenSettingsFile(settings);
            return legacy;
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
            hardenSettingsFile(settings);
        }
        entry = {true, legacy, migrated, migrationRevision};
        if (!migrated) {
            cache(key, entry);
        } else if (scrubPlaintext(settings, plaintextKey)) {
            cache(key, entry);
        } else {
            invalidateCache(key);
        }
        return entry.value;
    }

    if (result.status != CredentialStore::Status::NotFound) {
        reportStoreError(
            QStringLiteral("read"), credentialKey, result.error);
    } else if (revision.readable) {
        entry.revision = revision.value;
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
    setValueChecked(
        settings, scopeId, credentialKey, plaintextKey, value);
}

bool CredentialSettings::setValueChecked(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QVariant &value)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return false;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    CredentialOperationGuard operation(key);
    if (!operation)
        return false;
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    const QString secret = value.toString();
    if (secret.isEmpty()) {
        if (!persistPendingRemoval(settings, removalKey))
            return false;
        return completePendingRemoval(
            settings, key, credentialKey,
            plaintextKey, removalKey,
            operation.revisionPath());
    }

    const ExactSetting removal =
        readExactSetting(settings, removalKey);
    if (!removal.readable)
        return false;
    if (removal.present && removal.value.toBool()) {
        if (!completePendingRemoval(
                settings, key, credentialKey,
                plaintextKey, removalKey,
                operation.revisionPath())) {
            return false;
        }
    } else if (removal.present
               && !scrubPlaintext(settings, removalKey)) {
        return false;
    }

    QByteArray revision;
    if (!advanceCredentialRevision(
            operation.revisionPath(), &revision)) {
        invalidateCache(key);
        reportStoreError(
            QStringLiteral("write"), credentialKey,
            QStringLiteral(
                "Cannot persist credential revision"));
        return false;
    }

    QString error;
    const CredentialStore::Status status = store_
        ? store_->write(key, secret, &error)
        : CredentialStore::Status::Unavailable;
    const bool persisted = status == CredentialStore::Status::Success;
    bool scrubbed = false;
    if (persisted) {
        scrubbed = scrubPlaintext(settings, plaintextKey);
    } else {
        reportStoreError(
            QStringLiteral("write"), credentialKey, error);
        hardenSettingsFile(settings);
    }
    if (!persisted || scrubbed) {
        cache(key, {true, secret, persisted, revision});
    } else {
        invalidateCache(key);
    }
    return persisted && scrubbed;
}

void CredentialSettings::remove(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    removeChecked(
        settings, scopeId, credentialKey, plaintextKey);
}

bool CredentialSettings::persistPendingRemoval(
    QSettings *settings,
    const QString &removalKey)
{
    if (writeExactSetting(settings, removalKey, true))
        return true;

    qWarning() << "Cannot persist pending credential removal:"
               << settings->fileName();
    return false;
}

bool CredentialSettings::completePendingRemoval(
    QSettings *settings,
    const QString &key,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QString &removalKey,
    const QString &revisionPath)
{
    if (!scrubPlaintext(settings, plaintextKey))
        return false;

    QByteArray revision;
    if (!advanceCredentialRevision(
            revisionPath, &revision)) {
        invalidateCache(key);
        reportStoreError(
            QStringLiteral("remove"), credentialKey,
            QStringLiteral(
                "Cannot persist credential revision"));
        return false;
    }

    QString error;
    const CredentialStore::Status status = store_
        ? store_->remove(key, &error)
        : CredentialStore::Status::Unavailable;
    if (status != CredentialStore::Status::Success
        && status != CredentialStore::Status::NotFound) {
        reportStoreError(
            QStringLiteral("remove"), credentialKey, error);
        hardenSettingsFile(settings);
        invalidateCache(key);
        return false;
    }

    if (!scrubPlaintext(settings, removalKey)) {
        invalidateCache(key);
        return false;
    }

    cache(key, {false, QString(), false, revision});
    return true;
}

bool CredentialSettings::removeChecked(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    if (!settings || scopeId.isEmpty()
        || !isCredentialKey(credentialKey)) {
        return false;
    }

    const QString key = vaultKey(scopeId, credentialKey);
    CredentialOperationGuard operation(key);
    if (!operation)
        return false;
    const QString removalKey =
        pendingRemovalKey(scopeId, credentialKey);
    if (!persistPendingRemoval(settings, removalKey))
        return false;
    return completePendingRemoval(
        settings, key, credentialKey, plaintextKey,
        removalKey, operation.revisionPath());
}

void CredentialSettings::migratePlaintext(
    QSettings *settings,
    const QString &scopeId,
    const QString &prefix)
{
    if (!settings || scopeId.isEmpty()) return;
    for (const QString &key : credentialKeysForPrefix(prefix)) {
        const QString storedKey = plaintextKey(key);
        const ExactSetting stored =
            readExactSetting(settings, storedKey);
        if (stored.readable && stored.present) {
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

void CredentialSettings::invalidateCache(const QString &key)
{
    QMutexLocker locker(&cacheMutex_);
    cache_.remove(key);
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

bool CredentialSettings::scrubPlaintext(
    QSettings *settings,
    const QString &key)
{
    if (!settings || key.isEmpty()) return false;
    const ExactSetting stored =
        readExactSetting(settings, key);
    if (!stored.readable)
        return false;
    if (!stored.present)
        return true;

    if (removeExactSetting(settings, key))
        return true;
    qWarning() << "Cannot persist credential settings cleanup:"
               << settings->fileName();
    return false;
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
