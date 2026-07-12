#ifndef _GC_CredentialSettings_h
#define _GC_CredentialSettings_h 1

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

    struct ReadResult {
        Status status = Status::Failed;
        QString value;
        QString error;
    };

    virtual ~CredentialStore() = default;
    virtual ReadResult read(const QString &key) = 0;
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
    explicit CredentialSettings(
        std::unique_ptr<CredentialStore> store);

    static bool isCredentialKey(const QString &key);
    static QStringList credentialKeysForPrefix(
        const QString &prefix);
    static QString ensureScopeId(QSettings *settings,
                                 const QString &storageKey,
                                 const QString &preferredScopeId = QString());
    static QString vaultKey(const QString &scopeId,
                            const QString &credentialKey);
    static void hardenSettingsFile(QSettings *settings);

    QVariant value(QSettings *settings,
                   const QString &scopeId,
                   const QString &credentialKey,
                   const QString &plaintextKey,
                   const QVariant &defaultValue);
    void setValue(QSettings *settings,
                  const QString &scopeId,
                  const QString &credentialKey,
                  const QString &plaintextKey,
                  const QVariant &value);
    void remove(QSettings *settings,
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
    };

    bool cached(const QString &key, CacheEntry *entry) const;
    void cache(const QString &key, const CacheEntry &entry);
    static QString plaintextKey(const QString &credentialKey);
    static QString pendingRemovalKey(
        const QString &scopeId,
        const QString &credentialKey);
    static void scrubPlaintext(QSettings *settings,
                               const QString &key);
    static void reportStoreError(const QString &operation,
                                 const QString &credentialKey,
                                 const QString &error);

    std::unique_ptr<CredentialStore> store_;
    mutable QMutex cacheMutex_;
    QHash<QString, CacheEntry> cache_;
};

#endif
