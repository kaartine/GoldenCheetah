#ifndef _GC_CredentialSettings_h
#define _GC_CredentialSettings_h 1

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

class QSettings;

class CredentialStore
{
public:
    enum class Status {
        Success,
        NotFound,
        Unavailable,
        Failed
    };

    enum class CreateStatus {
        // The value was created and the operation is complete.
        Created,
        // A value already existed and was not changed.
        AlreadyExists,
        // No create operation was attempted.
        Unsupported,
        // The operation completed without changing the store.
        Unavailable,
        // The operation is complete, but it may have committed the value.
        Indeterminate,
        // The operation completed without changing the store.
        Failed
    };

    struct ReadResult {
        Status status = Status::Failed;
        QString value;
        QString error;
    };

    struct CreateResult {
        CreateStatus status = CreateStatus::Failed;
        QString error;
    };

    virtual ~CredentialStore() = default;
    virtual ReadResult read(const QString &key) = 0;
    virtual CreateResult createIfAbsent(
        const QString &key,
        const QString &value) = 0;
    virtual Status write(const QString &key,
                         const QString &value,
                         QString *error) = 0;
    virtual Status remove(const QString &key,
                          QString *error) = 0;
};

std::unique_ptr<CredentialStore> createPlatformCredentialStore();

class CredentialSettings
{
public:
    enum class ScopeBindingStatus {
        Success,
        Unavailable,
        Conflict
    };

    enum class LocationClaimStatus {
        Success,
        Unavailable,
        Conflict
    };

    struct ScopeBindingResult {
        ScopeBindingStatus status =
            ScopeBindingStatus::Unavailable;
        QString profileId;
        QString scopeId;
        bool legacyLocalScope = false;
        bool created = false;

        bool succeeded() const
        {
            return status == ScopeBindingStatus::Success;
        }
    };

    struct LocationEnrollmentResult {
        LocationClaimStatus status =
            LocationClaimStatus::Unavailable;
        QString identityId;
        bool pending = false;

        bool succeeded() const
        {
            return status == LocationClaimStatus::Success;
        }
    };

    struct LocalMetadataSnapshot {
        bool readable = false;
        bool identityPresent = false;
        QVariant identity;
        bool bindingPresent = false;
        QVariant binding;
        bool scopePresent = false;
        QVariant scope;
    };

    explicit CredentialSettings(
        std::unique_ptr<CredentialStore> store);

    static bool isCredentialKey(const QString &key);
    static QStringList credentialKeysForPrefix(
        const QString &prefix);
    static LocalMetadataSnapshot readLocalMetadata(
        QSettings *settings,
        const QString &identityKey,
        const QString &bindingKey,
        const QString &scopeKey);
    static QString ensureIdentityId(
        QSettings *settings,
        const QString &storageKey,
        const QString &recoveryBindingKey =
            QString(),
        bool *created = nullptr,
        const QString &preferredIdentityId =
            QString());
    static QString ensureScopeId(QSettings *settings,
                                 const QString &storageKey,
                                 const QString &preferredScopeId = QString());
    static ScopeBindingResult ensureScopeBinding(
        QSettings *settings,
        const QString &rootId,
        const QString &bindingKey,
        const QString &scopeKey,
        const QString &authorizedLegacyScopeId =
            QString(),
        const QString &authorizedLegacyProfileId =
            QString(),
        const QString &preferredProfileId =
            QString(),
        const QString &preferredScopeId =
            QString());
    static LocationClaimStatus ensureLocationClaim(
        QSettings *settings,
        const QString &claimKey,
        const QString &identityId,
        const QString &parentId,
        const QString &directoryPath,
        bool allowCreate = true);
    static LocationEnrollmentResult
    ensureLocationEnrollment(
        QSettings *settings,
        const QByteArray &kind,
        const QString &existingIdentityId,
        const QString &parentId,
        const QString &directoryPath,
        bool allowCreate);
    static bool completeLocationEnrollment(
        QSettings *settings,
        const QByteArray &kind,
        const QString &identityId,
        const QString &parentId,
        const QString &directoryPath,
        QSettings *localSettings,
        const QString &localIdentityKey,
        const QString &localBindingKey,
        const QString &localScopeKey,
        const QString &expectedLocalRootId,
        const QString &expectedLocalProfileId,
        const QString &expectedLocalScopeId);
    static QString vaultKey(const QString &scopeId,
                            const QString &credentialKey);
    static bool hardenSettingsFile(QSettings *settings);

    QVariant value(QSettings *settings,
                   const QString &scopeId,
                   const QString &credentialKey,
                   const QString &plaintextKey,
                   const QVariant &defaultValue,
                   bool *authoritativeMiss = nullptr,
                   bool *confirmedVaultValue = nullptr);
    void setValue(QSettings *settings,
                  const QString &scopeId,
                  const QString &credentialKey,
                  const QString &plaintextKey,
                  const QVariant &value);
    bool setValueChecked(QSettings *settings,
                         const QString &scopeId,
                         const QString &credentialKey,
                         const QString &plaintextKey,
                         const QVariant &value);
    void remove(QSettings *settings,
                const QString &scopeId,
                const QString &credentialKey,
                const QString &plaintextKey);
    bool removeChecked(QSettings *settings,
                       const QString &scopeId,
                       const QString &credentialKey,
                       const QString &plaintextKey);
    void migratePlaintext(QSettings *settings,
                          const QString &scopeId,
                          const QString &prefix);
    void clearCache();

private:
    struct CacheEntry {
        bool present = false;
        QString value;
        bool persisted = false;
        QByteArray revision;
        QByteArray transaction;
    };

    bool cached(const QString &key, CacheEntry *entry) const;
    void cache(const QString &key, const CacheEntry &entry);
    void invalidateCache(const QString &key);
    static QString plaintextKey(const QString &credentialKey);
    static QString pendingRemovalKey(
        const QString &scopeId,
        const QString &credentialKey);
    bool persistPendingRemoval(
        QSettings *settings,
        const QString &removalKey,
        const QByteArray &generation);
    bool completePendingRemoval(
        QSettings *settings,
        const QString &vaultKey,
        const QString &credentialKey,
        const QString &plaintextKey,
        const QString &removalKey,
        const QString &cleanupPath,
        const QString &revisionPath,
        const QString &deletionPath);
    bool preparePlaintextCleanup(
        QSettings *settings,
        const QString &key,
        const QString &cleanupPath,
        const QString &expectedPlaintext,
        bool replaceInvalidState = false,
        const QByteArray &writeTransaction = QByteArray());
    bool scrubPlaintext(
        QSettings *settings,
        const QString &key,
        const QString &cleanupPath);
    CredentialStore::ReadResult
    scrubPlaintextMatchingVault(
        QSettings *settings,
        const QString &plaintextKey,
        const QString &vaultKey,
        const QString &cleanupPath,
        const QString &expectedPlaintext,
        const CredentialStore::ReadResult &knownVault,
        bool *scrubbed,
        const QByteArray &authoritativeTransaction =
            QByteArray(),
        const QString &authoritativeVaultValue = QString());
    CredentialStore::ReadResult createAndConfirmMigrationValue(
        const QString &key,
        const QString &legacyValue,
        bool *created,
        bool *fallbackAllowed);
    static void reportStoreError(const QString &operation,
                                 const QString &credentialKey,
                                 const QString &error);

    std::unique_ptr<CredentialStore> store_;
    mutable QMutex cacheMutex_;
    QHash<QString, CacheEntry> cache_;
};

#endif
