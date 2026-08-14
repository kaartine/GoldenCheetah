#include <QtTest>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#endif

#include "Core/CredentialSettings.h"
#include "Core/CredentialStoreQtKeychain.h"
#include "Core/Settings.h"
#include "Cloud/StravaSettingsCommit.h"
#include "Gui/Colors.h"

double dpiXFactor = 1.0;
double dpiYFactor = 1.0;

namespace {

struct FakeStoreState
{
    QHash<QString, QString> values;
    bool failReads = false;
    int failNextReads = 0;
    CredentialStore::Status failedReadStatus =
        CredentialStore::Status::Unavailable;
    bool supportCreates = true;
    bool indeterminateCreates = false;
    bool commitIndeterminateCreates = false;
    QString indeterminateCommittedValue;
    bool failedCreates = false;
    bool failWrites = false;
    bool commitFailedWrites = false;
    bool failRemoves = false;
    int reads = 0;
    int writes = 0;
    int creates = 0;
    int overwrites = 0;
    int removes = 0;
    QString removeAfterReadKey;
    std::function<void()> beforeRead;
    std::function<void()> afterRead;
    std::function<void()> beforeCreateIfAbsent;
    std::function<void()> beforeWrite;
    std::function<void()> beforeRemove;
};

class FakeCredentialStore : public CredentialStore
{
public:
    explicit FakeCredentialStore(
        std::shared_ptr<FakeStoreState> state)
        : state_(std::move(state))
    {
    }

    ReadResult read(const QString &key) override
    {
        ++state_->reads;
        if (state_->beforeRead) state_->beforeRead();
        if (state_->failReads
            || state_->failNextReads > 0) {
            if (state_->failNextReads > 0)
                --state_->failNextReads;
            return {state_->failedReadStatus, QString(),
                    QStringLiteral("unavailable")};
        }
        if (!state_->values.contains(key)) {
            return {Status::NotFound, QString(), QString()};
        }
        const QString value = state_->values.value(key);
        if (state_->removeAfterReadKey == key) {
            state_->removeAfterReadKey.clear();
            state_->values.remove(key);
        }
        if (state_->afterRead) state_->afterRead();
        return {Status::Success, value, QString()};
    }

    CreateResult createIfAbsent(
        const QString &key,
        const QString &value) override
    {
        ++state_->writes;
        ++state_->creates;
        if (!state_->supportCreates) {
            return {
                CreateStatus::Unsupported,
                QStringLiteral("unsupported")
            };
        }
        if (state_->beforeCreateIfAbsent) {
            state_->beforeCreateIfAbsent();
        }
        if (state_->values.contains(key)) {
            return {CreateStatus::AlreadyExists, QString()};
        }
        if (state_->indeterminateCreates) {
            if (state_->commitIndeterminateCreates) {
                state_->values.insert(
                    key,
                    state_->indeterminateCommittedValue.isNull()
                        ? value
                        : state_->indeterminateCommittedValue);
            }
            return {
                CreateStatus::Indeterminate,
                QStringLiteral("indeterminate")
            };
        }
        if (state_->failedCreates) {
            return {
                CreateStatus::Failed,
                QStringLiteral("failed")
            };
        }
        if (state_->failWrites) {
            if (state_->commitFailedWrites) {
                state_->values.insert(key, value);
                return {
                    CreateStatus::Indeterminate,
                    QStringLiteral("indeterminate")
                };
            }
            return {
                CreateStatus::Unavailable,
                QStringLiteral("unavailable")
            };
        }
        state_->values.insert(key, value);
        return {CreateStatus::Created, QString()};
    }

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
        ++state_->writes;
        ++state_->overwrites;
        if (state_->beforeWrite) state_->beforeWrite();
        if (state_->failWrites) {
            if (state_->commitFailedWrites) {
                state_->values.insert(key, value);
            }
            if (error) *error = QStringLiteral("unavailable");
            return Status::Unavailable;
        }
        state_->values.insert(key, value);
        return Status::Success;
    }

    Status remove(const QString &key,
                  QString *error) override
    {
        ++state_->removes;
        if (state_->beforeRemove) state_->beforeRemove();
        if (state_->failRemoves) {
            if (error) *error = QStringLiteral("unavailable");
            return Status::Unavailable;
        }
        state_->values.remove(key);
        return Status::Success;
    }

private:
    std::shared_ptr<FakeStoreState> state_;
};

class FileCredentialStore : public CredentialStore
{
public:
    explicit FileCredentialStore(
        QString path,
        std::function<void()> afterMissingUnderLock = {},
        std::function<void()> lockContended = {})
        : path_(std::move(path)),
          afterMissingUnderLock_(
              std::move(afterMissingUnderLock)),
          lockContended_(std::move(lockContended))
    {
    }

    ReadResult read(const QString &key) override
    {
        QSettings settings(path_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            return {Status::Unavailable, QString(),
                    QStringLiteral("vault read failed")};
        }
        if (!settings.contains(key)) {
            return {Status::NotFound, QString(), QString()};
        }
        return {Status::Success,
                settings.value(key).toString(), QString()};
    }

    CreateResult createIfAbsent(
        const QString &key,
        const QString &value) override
    {
        QLockFile lock(
            path_ + QStringLiteral(".credential-store.lock"));
        if (!lock.tryLock(0)) {
            if (lockContended_)
                lockContended_();
            if (!lock.tryLock(5000)) {
                return {
                    CreateStatus::Unavailable,
                    QStringLiteral("vault lock failed")
                };
            }
        }

        QSettings settings(path_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            return {
                CreateStatus::Unavailable,
                QStringLiteral("vault read failed")
            };
        }
        if (settings.contains(key)) {
            return {CreateStatus::AlreadyExists, QString()};
        }
        if (afterMissingUnderLock_)
            afterMissingUnderLock_();

        settings.setValue(key, value);
        settings.sync();
        const bool written =
            settings.status() == QSettings::NoError
            && settings.value(key).toString() == value;
        return written
            ? CreateResult{
                CreateStatus::Created, QString()}
            : CreateResult{
                CreateStatus::Indeterminate,
                QStringLiteral("vault create failed")};
    }

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
        QLockFile lock(
            path_ + QStringLiteral(".credential-store.lock"));
        if (!lock.tryLock(5000)) {
            if (error) *error =
                QStringLiteral("vault lock failed");
            return Status::Unavailable;
        }
        QSettings settings(path_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.setValue(key, value);
        settings.sync();
        const bool written =
            settings.status() == QSettings::NoError
            && settings.value(key).toString() == value;
        if (!written && error) {
            *error = QStringLiteral("vault write failed");
        }
        return written ? Status::Success : Status::Unavailable;
    }

    Status remove(const QString &key,
                  QString *error) override
    {
        QLockFile lock(
            path_ + QStringLiteral(".credential-store.lock"));
        if (!lock.tryLock(5000)) {
            if (error) *error =
                QStringLiteral("vault lock failed");
            return Status::Unavailable;
        }
        QSettings settings(path_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.remove(key);
        settings.sync();
        const bool removed =
            settings.status() == QSettings::NoError
            && !settings.contains(key);
        if (!removed && error) {
            *error = QStringLiteral("vault removal failed");
        }
        return removed ? Status::Success : Status::Unavailable;
    }

private:
    QString path_;
    std::function<void()> afterMissingUnderLock_;
    std::function<void()> lockContended_;
};

class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(
        QByteArray name,
        const QByteArray &value)
        : name_(std::move(name)),
          existed_(qEnvironmentVariableIsSet(name_.constData())),
          previous_(qgetenv(name_.constData()))
    {
        qputenv(name_.constData(), value);
    }

    ~ScopedEnvironmentVariable()
    {
        reset();
    }

    void reset()
    {
        if (!active_)
            return;
        if (existed_) {
            qputenv(name_.constData(), previous_);
        } else {
            qunsetenv(name_.constData());
        }
        active_ = false;
    }

    Q_DISABLE_COPY(ScopedEnvironmentVariable)

private:
    QByteArray name_;
    bool existed_;
    QByteArray previous_;
    bool active_ = true;
};

class ScopedUnsetEnvironmentVariable
{
public:
    explicit ScopedUnsetEnvironmentVariable(QByteArray name)
        : name_(std::move(name)),
          existed_(qEnvironmentVariableIsSet(name_.constData())),
          previous_(qgetenv(name_.constData()))
    {
        qunsetenv(name_.constData());
    }

    ~ScopedUnsetEnvironmentVariable()
    {
        if (existed_) {
            qputenv(name_.constData(), previous_);
        }
    }

    Q_DISABLE_COPY(ScopedUnsetEnvironmentVariable)

private:
    QByteArray name_;
    bool existed_;
    QByteArray previous_;
};

std::shared_ptr<FakeStoreState> &factoryState()
{
    static std::shared_ptr<FakeStoreState> state =
        std::make_shared<FakeStoreState>();
    return state;
}

bool &useQtKeychainFactory()
{
    static bool enabled = false;
    return enabled;
}

std::unique_ptr<CredentialStore> fakeStore(
    const std::shared_ptr<FakeStoreState> &state)
{
    return std::make_unique<FakeCredentialStore>(state);
}

QString plainKey(QString key)
{
    key.remove(QRegularExpression(QStringLiteral("^<.*>")));
    return key;
}

QHash<QString, QVariant> settingsValuesWithPrefix(
    QSettings *settings,
    const QString &prefix)
{
    QHash<QString, QVariant> values;
    if (!settings)
        return values;
    for (const QString &key :
         settings->allKeys().filter(prefix)) {
        values.insert(key, settings->value(key));
    }
    return values;
}

struct TestLocationRecord
{
    bool valid = false;
    QString kind;
    QString identityId;
    QString parentId;
    QString directoryPath;
};

TestLocationRecord parseTestLocationRecord(
    const QVariant &stored)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            stored.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    const QJsonObject object = document.object();
    if (object.size() != 5
        || object.value(
                QStringLiteral("version")).toInt()
            != 1
        || !object.value(
                QStringLiteral("kind")).isString()
        || !object.value(
                QStringLiteral("identity_id")).isString()
        || !object.value(
                QStringLiteral("parent_id")).isString()
        || !object.value(
                QStringLiteral("directory_path")).isString()) {
        return {};
    }
    const QString kind =
        object.value(
            QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("root")
        && kind != QStringLiteral("profile")
        && kind != QStringLiteral("scope")) {
        return {};
    }
    const QString identityId =
        object.value(
            QStringLiteral("identity_id")).toString();
    const QString parentId =
        object.value(
            QStringLiteral("parent_id")).toString();
    const QString directoryPath =
        object.value(
            QStringLiteral("directory_path")).toString();
    const QUuid identity(identityId);
    const QUuid parent(parentId);
    if (identity.isNull()
        || identity.toString(QUuid::WithoutBraces)
            != identityId
        || (!parentId.isEmpty()
            && (parent.isNull()
                || parent.toString(
                       QUuid::WithoutBraces)
                    != parentId))
        || !QDir::isAbsolutePath(directoryPath)
        || QDir::cleanPath(directoryPath)
            != directoryPath) {
        return {};
    }
    return {
        true, kind, identityId,
        parentId, directoryPath
    };
}

TestLocationRecord parseTestLocationClaim(
    const QVariant &stored)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            stored.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    const QJsonObject object = document.object();
    if (object.size() != 4
        || object.value(
                QStringLiteral("version")).toInt()
            != 1
        || !object.value(
                QStringLiteral("identity_id")).isString()
        || !object.value(
                QStringLiteral("parent_id")).isString()
        || !object.value(
                QStringLiteral("directory_path")).isString()) {
        return {};
    }
    const QString identityId =
        object.value(
            QStringLiteral("identity_id")).toString();
    const QString parentId =
        object.value(
            QStringLiteral("parent_id")).toString();
    const QString directoryPath =
        object.value(
            QStringLiteral("directory_path")).toString();
    const QUuid identity(identityId);
    const QUuid parent(parentId);
    if (identity.isNull()
        || identity.toString(QUuid::WithoutBraces)
            != identityId
        || (!parentId.isEmpty()
            && (parent.isNull()
                || parent.toString(
                       QUuid::WithoutBraces)
                    != parentId))
        || !QDir::isAbsolutePath(directoryPath)
        || QDir::cleanPath(directoryPath)
            != directoryPath) {
        return {};
    }
    return {
        true, QString(), identityId,
        parentId, directoryPath
    };
}

struct TestScopeBinding
{
    bool valid = false;
    QString rootId;
    QString profileId;
    QString scopeId;
    QString origin;
};

TestScopeBinding parseTestScopeBinding(
    const QVariant &stored)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            stored.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    const QJsonObject object = document.object();
    if (object.size() != 5
        || object.value(
                QStringLiteral("version")).toInt()
            != 2
        || !object.value(
                QStringLiteral("root_id")).isString()
        || !object.value(
                QStringLiteral("profile_id")).isString()
        || !object.value(
                QStringLiteral("scope_id")).isString()
        || !object.value(
                QStringLiteral("origin")).isString()) {
        return {};
    }
    const QString rootId =
        object.value(
            QStringLiteral("root_id")).toString();
    const QString profileId =
        object.value(
            QStringLiteral("profile_id")).toString();
    const QString scopeId =
        object.value(
            QStringLiteral("scope_id")).toString();
    const QString origin =
        object.value(
            QStringLiteral("origin")).toString();
    const QUuid root(rootId);
    const QUuid profile(profileId);
    const QUuid scope(scopeId);
    if (root.isNull()
        || root.toString(QUuid::WithoutBraces)
            != rootId
        || profile.isNull()
        || profile.toString(QUuid::WithoutBraces)
            != profileId
        || scope.isNull()
        || scope.toString(QUuid::WithoutBraces)
            != scopeId
        || (origin != QStringLiteral("fresh")
            && origin
                != QStringLiteral("legacy_local"))) {
        return {};
    }
    return {
        true, rootId, profileId,
        scopeId, origin
    };
}

QString testLocationClaimKey(
    const QByteArray &kind,
    const QString &identityId)
{
    const QByteArray digest = QCryptographicHash::hash(
        kind + '\n' + identityId.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral(
        "credential_store/location_claims/")
        + QString::fromLatin1(digest);
}

QString testLocationEnrollmentKey(
    const QByteArray &kind,
    const QString &parentId,
    const QString &directoryPath)
{
    QByteArray identity = QByteArrayLiteral("v1");
    const auto appendField =
        [&identity](const QByteArray &field) {
            identity.append('\0');
            identity.append(
                QByteArray::number(field.size()));
            identity.append('\0');
            identity.append(field);
        };
    appendField(kind);
    appendField(parentId.toUtf8());
    appendField(directoryPath.toUtf8());
    const QByteArray digest = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral(
        "credential_store/location_enrollments/")
        + QString::fromLatin1(digest);
}

QString testLocationBindingKey(
    const QByteArray &kind,
    const QString &directoryPath)
{
    QByteArray identity = QByteArrayLiteral("v1");
    const auto appendField =
        [&identity](const QByteArray &field) {
            identity.append('\0');
            identity.append(
                QByteArray::number(field.size()));
            identity.append('\0');
            identity.append(field);
        };
    appendField(kind);
    appendField(directoryPath.toUtf8());
    const QByteArray digest = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral(
        "credential_store/location_bindings/")
        + QString::fromLatin1(digest);
}

QString testLocationClaimPayload(
    const QString &identityId,
    const QString &parentId,
    const QString &directoryPath)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(
        QStringLiteral("identity_id"), identityId);
    object.insert(
        QStringLiteral("parent_id"), parentId);
    object.insert(
        QStringLiteral("directory_path"),
        directoryPath);
    return QString::fromUtf8(
        QJsonDocument(object).toJson(
            QJsonDocument::Compact));
}

QString testLocationRecordPayload(
    const QByteArray &kind,
    const QString &identityId,
    const QString &parentId,
    const QString &directoryPath)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(
        QStringLiteral("kind"),
        QString::fromLatin1(kind));
    object.insert(
        QStringLiteral("identity_id"), identityId);
    object.insert(
        QStringLiteral("parent_id"), parentId);
    object.insert(
        QStringLiteral("directory_path"),
        directoryPath);
    return QString::fromUtf8(
        QJsonDocument(object).toJson(
            QJsonDocument::Compact));
}

QString testScopeBindingPayload(
    const QString &rootId,
    const QString &profileId,
    const QString &scopeId)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), 2);
    object.insert(QStringLiteral("root_id"), rootId);
    object.insert(
        QStringLiteral("profile_id"), profileId);
    object.insert(
        QStringLiteral("scope_id"), scopeId);
    object.insert(
        QStringLiteral("origin"),
        QStringLiteral("fresh"));
    return QString::fromUtf8(
        QJsonDocument(object).toJson(
            QJsonDocument::Compact));
}

QString legacyCredentialScopeMappingKey(
    const QString &athleteName)
{
    const QByteArray identity = athleteName.isEmpty()
        ? QByteArray("global")
        : QByteArray("athlete:") + athleteName.toUtf8();
    const QByteArray digest = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("credential_store/scopes/")
        + QString::fromLatin1(digest);
}

QString pendingRemovalTestKey(
    const QString &scope,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        CredentialSettings::vaultKey(scope, credentialKey).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral("credential_store/pending_remove/")
        + QString::fromLatin1(digest);
}

QString pendingRemovalGenerationTestKey(
    const QString &scope,
    const QString &credentialKey)
{
    const QString marker =
        pendingRemovalTestKey(scope, credentialKey);
    const QString prefix =
        QStringLiteral("credential_store/pending_remove/");
    return QStringLiteral(
        "credential_store/pending_remove_generation/")
        + marker.mid(prefix.size());
}

QString legacyFallbackBlockTestKey(
    const QString &scope,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        scope.toUtf8() + '\n' + credentialKey.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral(
        "credential_store/legacy_fallback_blocks/")
        + QString::fromLatin1(digest);
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
}

bool writeSignalFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write("ready") == 5;
}

bool waitForFile(
    const QString &path,
    int timeoutMs,
    QProcess *process = nullptr)
{
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(path)
           && timer.elapsed() < timeoutMs
           && (!process
               || process->state() != QProcess::NotRunning)) {
        QTest::qWait(10);
    }
    return QFileInfo::exists(path);
}

QString credentialOperationFile(
    const QString &root,
    const QString &operationId,
    const QString &suffix)
{
    const QByteArray digest = QCryptographicHash::hash(
        operationId.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QDir(root).filePath(
        QStringLiteral("GoldenCheetah/credential-locks/")
        + QString::fromLatin1(digest) + suffix);
}

QString credentialCleanupFile(
    const QString &root,
    const QString &vaultKey,
    QSettings *settings,
    const QString &plaintextKey)
{
    if (!settings)
        return {};
    const QFileInfo settingsFile(settings->fileName());
    const QString canonical =
        settingsFile.canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    const QString group = settings->group();
    QString absoluteKey = group.isEmpty()
        ? plaintextKey
        : group + QLatin1Char('/') + plaintextKey;
#ifdef Q_OS_WIN
    if (settings->format() == QSettings::IniFormat)
        absoluteKey = absoluteKey.toCaseFolded();
#endif
    QByteArray sourceIdentity = QByteArrayLiteral("v1");
    const auto appendField =
        [&sourceIdentity](const QByteArray &field) {
            sourceIdentity.append('\0');
            sourceIdentity.append(
                QByteArray::number(field.size()));
            sourceIdentity.append('\0');
            sourceIdentity.append(field);
        };
    appendField(
        (QStringLiteral("file:") + canonical).toUtf8());
    appendField(absoluteKey.toUtf8());
    const QByteArray sourceDigest =
        QCryptographicHash::hash(
            sourceIdentity,
            QCryptographicHash::Sha256).toHex();
    return credentialOperationFile(root, vaultKey, QString())
        + QLatin1Char('.')
        + QString::fromLatin1(sourceDigest)
        + QStringLiteral(".cleanup");
}

bool writePrivateStateFile(
    const QString &path,
    const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
#ifdef Q_OS_UNIX
    if (!file.setPermissions(
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner)) {
        return false;
    }
#endif
    return file.write(contents) == contents.size()
        && file.flush();
}

bool isCredentialTestTransaction(
    const QByteArray &transaction)
{
    if (transaction.size() != 32)
        return false;
    for (const char character : transaction) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool writeCredentialPhaseFile(
    const QString &path,
    const QByteArray &phase,
    QByteArray *transaction = nullptr)
{
    const QByteArray generated =
        QUuid::createUuid().toRfc4122().toHex();
    if (transaction)
        *transaction = generated;
    return writePrivateStateFile(
        path,
        QByteArrayLiteral("v1 ") + phase
            + QByteArrayLiteral(" ") + generated
            + QByteArrayLiteral("\n"));
}

QByteArray credentialPhaseTransaction(
    const QString &path,
    const QByteArray &expectedPhase)
{
    const QList<QByteArray> fields =
        fileContents(path).trimmed().split(' ');
    if (fields.size() != 3
        || fields.at(0) != QByteArrayLiteral("v1")
        || fields.at(1) != expectedPhase
        || !isCredentialTestTransaction(fields.at(2))) {
        return {};
    }
    return fields.at(2);
}

bool credentialPhaseIs(
    const QString &path,
    const QByteArray &expectedPhase)
{
    return !credentialPhaseTransaction(
                path, expectedPhase).isEmpty();
}

bool readEmptySettings(QIODevice &, QSettings::SettingsMap &settings)
{
    settings.clear();
    return true;
}

bool rejectSettingsWrite(QIODevice &,
                         const QSettings::SettingsMap &)
{
    return false;
}

const QString legacySystemMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/system_state");
const QString legacyGlobalMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/global_state");
const QString legacyAthleteMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/athlete_state");
const QString legacyMigrationStarted = QStringLiteral("started");
const QString legacyMigrationComplete = QStringLiteral("complete");

struct MigrationFormatFaultState
{
    bool enabled = false;
    QString failurePoint;
    int rejectedWrites = 0;
};

MigrationFormatFaultState &migrationFormatFaultState()
{
    static MigrationFormatFaultState state;
    return state;
}

bool readPersistentSettings(
    QIODevice &device,
    QSettings::SettingsMap &settings)
{
    if (device.bytesAvailable() == 0) {
        settings.clear();
        return true;
    }

    QDataStream stream(&device);
    quint32 magic = 0;
    stream >> magic >> settings;
    return magic == 0x47435331U
        && stream.status() == QDataStream::Ok;
}

bool writePersistentSettings(
    QIODevice &device,
    const QSettings::SettingsMap &settings)
{
    QDataStream stream(&device);
    stream << quint32(0x47435331U) << settings;
    return stream.status() == QDataStream::Ok;
}

QSettings::Format scopeLockTestFormat()
{
    static const QSettings::Format format =
        QSettings::registerFormat(
            QStringLiteral("gc-scope-lock"),
            readPersistentSettings,
            [](QIODevice &device,
               const QSettings::SettingsMap &settings) {
                if (qEnvironmentVariableIsSet(
                        "GC_SCOPE_LOCK_CHILD")) {
                    const QString readyPath =
                        qEnvironmentVariable(
                            "GC_SCOPE_LOCK_READY");
                    if (!readyPath.isEmpty()) {
                        writeSignalFile(readyPath);
                    }
                    const int holdMs = qEnvironmentVariableIntValue(
                        "GC_SCOPE_LOCK_HOLD_MS");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            std::max(holdMs, 1)));
                }
                return writePersistentSettings(device, settings);
            });
    return format;
}

struct CredentialScrubFaultState
{
    bool enabled = false;
    QString requiredKey;
    QString forbiddenKey;
    int rejectedWrites = 0;
};

CredentialScrubFaultState &credentialScrubFaultState()
{
    static CredentialScrubFaultState state;
    return state;
}

bool writeCredentialScrubFaultSettings(
    QIODevice &device,
    const QSettings::SettingsMap &settings)
{
    CredentialScrubFaultState &state =
        credentialScrubFaultState();
    if (state.enabled
        && ((!state.requiredKey.isEmpty()
             && !settings.contains(state.requiredKey))
            || (!state.forbiddenKey.isEmpty()
                && settings.contains(state.forbiddenKey)))) {
        ++state.rejectedWrites;
        return false;
    }
    return writePersistentSettings(device, settings);
}

QSettings::Format credentialScrubTestFormat()
{
    static const QSettings::Format format =
        QSettings::registerFormat(
            QStringLiteral("gc-credential-scrub"),
            readPersistentSettings,
            writeCredentialScrubFaultSettings);
    return format;
}

QSettings::SettingsMap persistedSettingsMap(
    const QString &path,
    bool *readable = nullptr)
{
    QFile file(path);
    QSettings::SettingsMap settings;
    const bool success = file.open(QIODevice::ReadOnly)
        && readPersistentSettings(file, settings);
    if (readable) *readable = success;
    return settings;
}

QString migrationWritePoint(
    const QSettings::SettingsMap &settings)
{
    const QString systemState = settings
        .value(legacySystemMigrationMarkerKey).toString();
    const QString globalState = settings
        .value(legacyGlobalMigrationMarkerKey).toString();
    const QString athleteState = settings
        .value(legacyAthleteMigrationMarkerKey).toString();

    if (systemState == legacyMigrationStarted) {
        return settings.contains(
                   plainKey(GC_SETTINGS_LAST_IMPORT_PATH))
            ? QStringLiteral("system-target")
            : QStringLiteral("system-start");
    }
    if (systemState == legacyMigrationComplete) {
        if (settings.contains(plainKey(GC_START_HTTP))) {
            return QStringLiteral("global-system");
        }
        if (settings.contains(
                plainKey(GC_SETTINGS_LAST_IMPORT_PATH))) {
            return QStringLiteral("system-complete");
        }
    }

    if (globalState == legacyMigrationStarted) {
        return settings.contains(plainKey(GC_TABBAR))
            ? QStringLiteral("global-general")
            : QStringLiteral("global-start");
    }
    if (globalState == legacyMigrationComplete) {
        if (settings.contains(plainKey(GC_UNIT))) {
            return QStringLiteral("athlete-global");
        }
        if (settings.contains(plainKey(GC_TABBAR))) {
            return QStringLiteral("global-complete");
        }
    }
    if (settings.contains(plainKey(GC_DEV_COUNT))) {
        return QStringLiteral("global-train");
    }

    if (athleteState == legacyMigrationStarted) {
        return settings.contains(plainKey(GC_VERSION_USED))
            ? QStringLiteral("athlete-general")
            : QStringLiteral("athlete-start");
    }
    if (athleteState == legacyMigrationComplete
        && settings.contains(plainKey(GC_VERSION_USED))) {
        return QStringLiteral("athlete-complete");
    }
    if (settings.contains(plainKey(GC_LTM_LAST_DATE_RANGE))) {
        return QStringLiteral("athlete-layout");
    }
    if (settings.contains(plainKey(GC_NICKNAME))) {
        return QStringLiteral("athlete-preferences");
    }
    if (settings.contains(plainKey(GC_RWGPSUSER))) {
        return QStringLiteral("athlete-private");
    }
    return {};
}

bool writeFaultInjectingSettings(
    QIODevice &device,
    const QSettings::SettingsMap &settings)
{
    MigrationFormatFaultState &state =
        migrationFormatFaultState();
    const bool rejectsCredentialBinding =
        state.failurePoint
            == QStringLiteral("credential-binding")
        && settings.contains(
            QStringLiteral(
                "credential_store/binding_v2"));
    bool rejectsLocationClaim = false;
    if (state.failurePoint
            == QStringLiteral(
                "credential-location-claim")) {
        const QString prefix =
            QStringLiteral(
                "credential_store/location_claims/");
        for (auto entry = settings.cbegin();
             entry != settings.cend(); ++entry) {
            if (entry.key().startsWith(prefix)) {
                rejectsLocationClaim = true;
                break;
            }
        }
    } else if (state.failurePoint.startsWith(
                   QStringLiteral(
                       "credential-location-claim-count-"))) {
        bool validCount = false;
        const int rejectedCount = state.failurePoint.mid(
            QStringLiteral(
                "credential-location-claim-count-").size())
                                      .toInt(&validCount);
        int claimCount = 0;
        const QString prefix =
            QStringLiteral(
                "credential_store/location_claims/");
        for (auto entry = settings.cbegin();
             entry != settings.cend(); ++entry) {
            if (entry.key().startsWith(prefix))
                ++claimCount;
        }
        rejectsLocationClaim =
            validCount && claimCount == rejectedCount;
    }
    bool rejectsLegacyFallbackBlock = false;
    if (state.failurePoint
            == QStringLiteral(
                "credential-fallback-block")) {
        const QString prefix =
            QStringLiteral(
                "credential_store/legacy_fallback_blocks/");
        for (auto entry = settings.cbegin();
             entry != settings.cend(); ++entry) {
            if (entry.key().startsWith(prefix)) {
                rejectsLegacyFallbackBlock = true;
                break;
            }
        }
    }
    if (state.enabled
        && (rejectsCredentialBinding
            || rejectsLocationClaim
            || rejectsLegacyFallbackBlock
            || migrationWritePoint(settings)
                == state.failurePoint)) {
        ++state.rejectedWrites;
        return false;
    }
    return writePersistentSettings(device, settings);
}

QSettings::Format legacyMigrationTestFormat()
{
    static const QSettings::Format format =
        QSettings::registerFormat(
            QStringLiteral("gc-legacy-settings"),
            readPersistentSettings,
            writePersistentSettings);
    return format;
}

class ScopedLegacyMigrationFormat
{
public:
    explicit ScopedLegacyMigrationFormat(const QString &path)
    {
        QSettings::setPath(
            format(),
            QSettings::UserScope,
            path);
    }

    QSettings::Format format() const
    {
        return legacyMigrationTestFormat();
    }

    Q_DISABLE_COPY(ScopedLegacyMigrationFormat)
};

class LegacyMigrationQSettings : public QSettings
{
public:
    using QSettings::QSettings;

    LegacyMigrationQSettings(
        const QString &organization,
        const QString &application)
        : QSettings(
              legacyMigrationTestFormat(),
              QSettings::UserScope,
              organization,
              application)
    {
    }
};

QSettings::Format targetMigrationTestFormat()
{
    static const QSettings::Format format =
        QSettings::registerFormat(
            QStringLiteral("gc-target-settings"),
            readPersistentSettings,
            writeFaultInjectingSettings);
    return format;
}

void verifyOwnerOnlyPermissions(const QString &path)
{
#ifndef Q_OS_WIN
    const QFileDevice::Permissions permissions =
        QFileInfo(path).permissions();
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup
        | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup
        | QFileDevice::ReadOther
        | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    QCOMPARE(permissions & forbidden,
             QFileDevice::Permissions());
#else
    Q_UNUSED(path)
#endif
}

#ifdef Q_OS_WIN
bool currentWindowsUserSid(
    QByteArray *storage,
    PSID *sid)
{
    if (!storage || !sid)
        return false;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD required = 0;
    ::GetTokenInformation(
        token, TokenUser, nullptr, 0, &required);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        ::CloseHandle(token);
        return false;
    }
    storage->resize(int(required));
    const bool read = ::GetTokenInformation(
        token, TokenUser, storage->data(), required,
        &required);
    ::CloseHandle(token);
    if (!read)
        return false;
    auto *user = reinterpret_cast<TOKEN_USER *>(
        storage->data());
    if (!::IsValidSid(user->User.Sid))
        return false;
    *sid = user->User.Sid;
    return true;
}

bool setWindowsDirectoryAcl(
    const QString &path,
    bool allowEveryone,
    bool inheritable = true)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(
            &userStorage, &userSid)) {
        return false;
    }
    BYTE everyoneStorage[SECURITY_MAX_SID_SIZE];
    DWORD everyoneSize = sizeof(everyoneStorage);
    if (allowEveryone
        && !::CreateWellKnownSid(
            WinWorldSid, nullptr, everyoneStorage,
            &everyoneSize)) {
        return false;
    }

    EXPLICIT_ACCESSW access[2] = {};
    access[0].grfAccessPermissions = FILE_ALL_ACCESS;
    access[0].grfAccessMode = SET_ACCESS;
    access[0].grfInheritance = inheritable
        ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
        : NO_INHERITANCE;
    ::BuildTrusteeWithSidW(
        &access[0].Trustee, userSid);
    ULONG count = 1;
    if (allowEveryone) {
        access[1].grfAccessPermissions =
            FILE_ALL_ACCESS;
        access[1].grfAccessMode = SET_ACCESS;
        access[1].grfInheritance = inheritable
            ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
            : NO_INHERITANCE;
        ::BuildTrusteeWithSidW(
            &access[1].Trustee, everyoneStorage);
        count = 2;
    }

    PACL acl = nullptr;
    const DWORD aclResult = ::SetEntriesInAclW(
        count, access, nullptr, &acl);
    if (aclResult != ERROR_SUCCESS)
        return false;
    QString nativePath = QDir::toNativeSeparators(path);
    const DWORD result = ::SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(
            nativePath.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION
            | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    ::LocalFree(acl);
    return result == ERROR_SUCCESS;
}

bool windowsDirectoryHasOwnerOnlyAcl(
    const QString &path)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(
            &userStorage, &userSid)) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QString nativePath = QDir::toNativeSeparators(path);
    const DWORD result = ::GetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(
            nativePath.data()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD descriptorRevision = 0;
    const bool protectedDacl =
        ::GetSecurityDescriptorControl(
            descriptor, &control,
            &descriptorRevision)
        && (control & SE_DACL_PROTECTED);
    bool valid = owner && ::EqualSid(owner, userSid)
        && acl && protectedDacl
        && acl->AceCount == 1;
    bool currentUserAllowed = false;
    if (valid) {
        void *rawAce = nullptr;
        if (!::GetAce(acl, 0, &rawAce)) {
            valid = false;
        } else {
            auto *header = static_cast<ACE_HEADER *>(rawAce);
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(
                    rawAce);
            PSID trustee = &ace->SidStart;
            if (header->AceType
                    != ACCESS_ALLOWED_ACE_TYPE
                || !::IsValidSid(trustee)
                || !::EqualSid(trustee, userSid)
                || (header->AceFlags
                    & INHERITED_ACE)
                || (header->AceFlags
                    & (OBJECT_INHERIT_ACE
                       | CONTAINER_INHERIT_ACE))
                    != (OBJECT_INHERIT_ACE
                        | CONTAINER_INHERIT_ACE)
                || (header->AceFlags
                    & (INHERIT_ONLY_ACE
                       | NO_PROPAGATE_INHERIT_ACE))
                || (ace->Mask & FILE_ALL_ACCESS)
                    != FILE_ALL_ACCESS) {
                valid = false;
            } else {
                currentUserAllowed = true;
            }
        }
    }
    ::LocalFree(descriptor);
    return valid && currentUserAllowed;
}

bool windowsFileHasOwnerOnlyAcl(const QString &path)
{
    QByteArray userStorage;
    PSID userSid = nullptr;
    if (!currentWindowsUserSid(
            &userStorage, &userSid)) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QString nativePath = QDir::toNativeSeparators(path);
    const DWORD result = ::GetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(nativePath.data()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD descriptorRevision = 0;
    bool valid = owner && ::EqualSid(owner, userSid)
        && acl && acl->AceCount == 1
        && ::GetSecurityDescriptorControl(
            descriptor, &control, &descriptorRevision)
        && (control & SE_DACL_PROTECTED);
    if (valid) {
        void *rawAce = nullptr;
        if (!::GetAce(acl, 0, &rawAce)) {
            valid = false;
        } else {
            auto *header =
                static_cast<ACE_HEADER *>(rawAce);
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
            valid = header->AceType
                    == ACCESS_ALLOWED_ACE_TYPE
                && !(header->AceFlags
                     & (INHERITED_ACE
                        | OBJECT_INHERIT_ACE
                        | CONTAINER_INHERIT_ACE
                        | INHERIT_ONLY_ACE
                        | NO_PROPAGATE_INHERIT_ACE))
                && ::IsValidSid(&ace->SidStart)
                && ::EqualSid(&ace->SidStart, userSid)
                && (ace->Mask & FILE_ALL_ACCESS)
                    == FILE_ALL_ACCESS;
        }
    }
    if (descriptor)
        ::LocalFree(descriptor);
    return valid;
}
#endif

} // namespace

std::unique_ptr<CredentialStore>
createPlatformCredentialStore()
{
    const QString enrollmentVault =
        qEnvironmentVariable(
            "GC_ENROLLMENT_TEST_VAULT");
    if (!enrollmentVault.isEmpty()) {
        return std::make_unique<FileCredentialStore>(
            enrollmentVault);
    }
    if (useQtKeychainFactory())
        return createQtKeychainCredentialStore();
    return fakeStore(factoryState());
}

QStringList GCColor::getConfigKeys()
{
    return {};
}

class TestCredentialSettings : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void credentialClassification_data();
    void credentialClassification();
    void stravaClientCredentialsNeverReachPlaintextSettings();
    void keychainStatusMapping_data();
    void keychainStatusMapping();
    void linuxKeychainRuntimeStatusReport_data();
    void linuxKeychainRuntimeStatusReport();
    void bundledLinuxRuntimePathRequiresContainedRegularFile();
    void keychainJobsDisablePlaintextFallback();
    void timedOutKeychainMutationRetainsSerialization_data();
    void timedOutKeychainMutationRetainsSerialization();
    void timedOutKeychainMutationIsBounded_data();
    void timedOutKeychainMutationIsBounded();
    void timedOutKeychainReadRetainsSerialization_data();
    void timedOutKeychainReadRetainsSerialization();
    void destroyedTimedOutReadReleasesGate();
    void staleTimedOutKeychainJobCannotReleaseNewOwner();
    void destroyedTimedOutMutationRecoversAfterLeaseRelease();
    void keychainMarkerCreationFailureBlocksBackend_data();
    void keychainMarkerCreationFailureBlocksBackend();
    void keychainMarkerCleanupFailureRecoversOnNextLease_data();
    void keychainMarkerCleanupFailureRecoversOnNextLease();
    void timedOutKeychainMutationFromWorkerIsBounded();
    void timedOutKeychainMutationBlocksCredentialStateUntilTerminal();
    void timedOutKeychainMutationLeaseIsCrossProcess_data();
    void timedOutKeychainMutationLeaseIsCrossProcess();
    void invalidKeychainMutationMarkerBlocksCredentialState_data();
    void invalidKeychainMutationMarkerBlocksCredentialState();
    void platformStoreRoundTripsOrFailsClosed();
    void platformCreateIfAbsentIsBoundedAndUnsupported();
    void fileStoreCreateIfAbsentIsAtomicAcrossProcesses();
    void vaultCallsDoNotMutateFallbackState();
    void credentialOperationsAreSerialized();
    void reentrantCredentialOperationFailsFast();
    void reentrantIndependentCredentialOperationSucceeds();
    void credentialProcessLockIsExclusive();
    void contendedCredentialReadWaitsForOwner_data();
    void contendedCredentialReadWaitsForOwner();
    void activeKeychainMutationLeaseBlocksCredentialState();
    void activeCredentialProcessLockDoesNotExpire();
    void scopeCreationIsSerialized();
    void scopeProcessLockCanonicalizesAliases();
    void identityIdsAreStableAndMalformedValuesFailClosed();
    void scopeBindingsAreStableAndRootBound();
    void scopeBindingsRejectUnconfirmedLegacyScopes();
    void persistedLegacyBindingsRequireAuthorization();
    void malformedScopeBindingsFailClosed_data();
    void malformedScopeBindingsFailClosed();
    void locationClaimsRejectCopiesAndUnconfirmedMoves();
    void pendingLocationEnrollmentRejectsCompetingParent();
    void canonicalLocationClaimRejectsCompetingParent();
    void misplacedLocationClaimCannotAuthorizeBackfill();
    void malformedLocationAuthorityRecordsFailClosed_data();
    void malformedLocationAuthorityRecordsFailClosed();
    void locationEnrollmentCompletionRequiresLocalMetadata();
    void locationEnrollmentCompletionRequiresFullLocalBinding_data();
    void locationEnrollmentCompletionRequiresFullLocalBinding();
    void persistedCachesTrackOtherInstances_data();
    void persistedCachesTrackOtherInstances();
    void externalVaultMutationExpiresPersistedCache_data();
    void externalVaultMutationExpiresPersistedCache();
    void activeCredentialCacheObservesExternalMutation_data();
    void activeCredentialCacheObservesExternalMutation();
    void expiredPersistedCacheFailsClosedOnVaultError();
    void memoryOnlyCredentialCacheDoesNotExpire_data();
    void memoryOnlyCredentialCacheDoesNotExpire();
    void authoritativeReadObservesExternalVaultMutation_data();
    void authoritativeReadObservesExternalVaultMutation();
    void cachedCredentialTracksOtherInstanceRemoval();
    void memoryOnlyCredentialTracksOtherInstanceReplacement();
    void failedUpdateResolutionInvalidatesMemoryOnlyCache();
    void supersededUpdateInvalidatesMemoryOnlyCache();
    void credentialCacheTracksProcessMutations();
    void missingRevisionInvalidatesCachedCredential();
    void credentialRevisionFailureBlocksVaultMutation();
    void credentialRevisionFailureBlocksMigration();
    void credentialRevisionFailureBlocksRemoval();
    void groupedCredentialCleanupTargetsActiveGroup();
    void plaintextCleanupRequiresMatchingVaultCopy();
    void plaintextCleanupConflictsAreGroupScoped();
    void plaintextCleanupConflictsAreSourceScoped();
    void plaintextCleanupAliasesShareGeneration();
    void plaintextCleanupIdentitySeparatesNewlineTuples();
    void plaintextCleanupSurvivesLiveSettingsCache();
    void plaintextCleanupUsesFreshDiskSnapshot();
    void plaintextCleanupBypassesStaleNegativeCache();
    void plaintextRemovalBypassesStaleNegativeCache();
    void plaintextSnapshotCrashLeavesNoCopy();
    void settingsSerializationCrashLeavesPrivateCopy();
    void scratchCleanupLockRecoversAfterCrash();
    void vaultOnlyReadIgnoresSettingsCleanupLock();
    void plaintextCleanupFlushesLivePendingSettings();
    void plaintextMigratesToVault();
    void vaultValueWinsAndDistinctPlaintextIsRetained();
    void writesAndDeletesNeverTouchIni();
    void failedMigrationIsRetriedWithoutCredentialLoss();
    void failedNewCredentialWriteIsMemoryOnly();
    void checkedCredentialWriteReportsPersistence();
    void failedReplacementPreservesLegacyCredential();
    void creatingRecoveryFindsLegacyInOtherSource();
    void duplicatePlaintextDoesNotResurrectDeletedCredential();
    void failedDeleteIntentAppliesAcrossPlaintextSources();
    void deletionStatePersistenceFailurePreservesVault();
    void completedDeletionPersistsGlobalTombstone();
    void replacementRecoveryRetainsUnboundPlaintext();
    void uncommittedCleanupIntentCannotAuthorizeReplacement();
    void authorizedCleanupRequiresWrittenVaultValue();
    void authorizedCleanupCrashRecoveryAcrossProcesses();
    void incompleteReplacementBlocksPlaintextMigration();
    void stalePendingRemovalDoesNotDeleteReplacementAcrossSources();
    void stalePendingRemovalDoesNotDeleteOrdinaryWrite();
    void activeStateBlocksDuplicateMigrationAfterExternalVaultLoss();
    void uncertainActiveUpdateDoesNotMigrateDuplicatePlaintext();
    void orphanedDeletePreparationDoesNotRemoveCredential();
    void preparedDeleteWithMarkerCompletes();
    void mismatchedPreparedMarkerDoesNotDeleteCredential();
    void orphanedGeneratedRemovalFailsClosed();
    void malformedGeneratedRemovalFailsClosed();
    void credentialTransactionMetadataContainsNoSecrets();
    void plaintextCleanupMetadataContainsNoSecrets();
    void plaintextCleanupDurabilityFailureRetainsSource_data();
    void plaintextCleanupDurabilityFailureRetainsSource();
    void explicitWriteAuthorizationFailureRetainsSource_data();
    void explicitWriteAuthorizationFailureRetainsSource();
    void malformedPlaintextCleanupStateFailsClosed_data();
    void malformedPlaintextCleanupStateFailsClosed();
    void redirectedPlaintextCleanupStateFailsClosed();
    void hardLinkedPlaintextCleanupStateFailsClosed();
    void hardLinkedPlaintextSourceFailsClosed();
    void explicitWriteRejectsRedirectedCleanupState();
    void explicitMutationRepairsMalformedCleanupState_data();
    void explicitMutationRepairsMalformedCleanupState();
    void explicitWriteProtectsLaterPlaintext_data();
    void explicitWriteProtectsLaterPlaintext();
    void credentialStateDurabilityFailureFailsClosed_data();
    void credentialStateDurabilityFailureFailsClosed();
    void credentialPostMutationDurabilityFailure_data();
    void credentialPostMutationDurabilityFailure();
    void credentialCrashRecoveryAcrossProcesses();
    void plaintextCleanupCrashRecoveryAcrossProcesses();
    void canonicalCredentialStateAncestorIsAccepted();
    void credentialStateAncestorSyncFailureFailsClosed();
    void partialCredentialStateAncestryFailsClosed();
    void unsafeCredentialStateAncestorFailsClosed();
    void insecureCredentialStateRootFailsClosed();
    void symlinkedCredentialStateDirectoryFailsClosed();
    void windowsCredentialDirectoriesUseOwnerOnlyAcl();
    void windowsSettingsReplacementUsesOwnerOnlyAcl();
    void windowsCleanupMatchesIniKeysCaseInsensitively();
    void windowsWritableCredentialRootFailsClosed();
    void windowsExistingWritableCredentialDirectoryFailsClosed_data();
    void windowsExistingWritableCredentialDirectoryFailsClosed();
    void windowsNonInheritableCredentialDirectoryFailsClosed_data();
    void windowsNonInheritableCredentialDirectoryFailsClosed();
    void reissuedDeleteResumesDurableTransaction();
    void reportedMarkerFailureRecoversDurableMarker();
    void failedMarkerPersistenceLeavesPreparationState();
    void failedCreationDeletePreparationPreservesLegacyCredential();
    void failedCreationDeletePreparationFindsLegacyInOtherSource();
    void failedActiveDeletePreparationBlocksDuplicateResurrection();
    void deletionCommitFailureRetriesWithoutResurrection();
    void replacementCommitFailureRecoversWithoutRewrite();
    void finalRevisionFailureRecoversCommittedWrite_data();
    void finalRevisionFailureRecoversCommittedWrite();
    void failedReplacementResultRecoversCommittedSecret();
    void failedOrdinaryWriteResultRecoversCommittedSecret();
    void transientReplacementReadKeepsDeletionState();
    void malformedDeletionStateFailsClosed();
    void failedDeleteIsRetriedWithoutCredentialResurrection();
    void failedDeleteIsRetriedInSameSession();
    void failedDeleteMarkerWriteDefersDeletionUntilDurable();
    void externalPendingRemovalOverridesFailedMarkerState();
    void externalReplacementClearsStaleRemovalState();
    void failedReplacementAfterPendingRemovalDoesNotResurrect();
    void pendingRemovalClearsMemoryOnlyCredential();
    void pendingRemovalDiscardsMemoryOnlyCredentialAfterRestart();
    void persistedCacheScrubsDuplicatePlaintext();
    void unpersistedCachePreservesDuplicatePlaintext();
    void failedPlaintextScrubIsRetried_data();
    void failedPlaintextScrubIsRetried();
    void failedCachedPlaintextScrubIsRetried();
    void failedMigrationScrubInvalidatesNegativeCache();
    void failedVaultReadScrubInvalidatesNegativeCache();
    void failedSetValuePlaintextScrubIsRetried();
    void failedSetValueScrubInvalidatesOldCache();
    void failedRemovePlaintextScrubPreservesVault();
    void pendingRemovalPersistenceFailurePreservesVault();
    void pendingRemovalCleanupFailureIsRetried();
    void pendingRemovalCleanupRetriesInSameSettingsSession();
    void credentialOperationsRecoverFromStickyCallerStatus();
    void pendingRemovalMustClearBeforeCredentialWrite();
    void systemFallbackCredentialIsIgnored();
    void systemFallbackPendingRemovalIsIgnored();
    void systemFallbackScopeIdIsIgnored();
    void negativeCacheDoesNotHideLegacyCredential();
    void emptyPlaintextDoesNotCacheTransientVaultFailure();
    void transientReadFailureIsRetried();
    void transientReadDoesNotOverwriteNewerCredential_data();
    void transientReadDoesNotOverwriteNewerCredential();
    void migrationDoesNotOverwriteCredentialCreatedAfterMiss();
    void migrationCleanupRetainsChangedOrLastPlaintext_data();
    void migrationCleanupRetainsChangedOrLastPlaintext();
    void completedCleanupProtectsReappearedPlaintext_data();
    void completedCleanupProtectsReappearedPlaintext();
    void migrationCollisionReadFailureRetainsPlaintextAndRetries();
    void definiteMigrationOutcomeRequiresConfirmation_data();
    void definiteMigrationOutcomeRequiresConfirmation();
    void creatingMigrationDoesNotOverwriteCredentialCreatedAfterMiss();
    void creatingMigrationCollisionReadFailureRetainsTransaction();
    void creatingMigrationUnsupportedRetainsTransactionAndRetries();
    void failedMigrationDoesNotCacheOverNewerCredential();
    void transientMigrationCreateFailureRetriesInSameSession();
    void failedMigrationCreateRetainsPlaintextAndRetries();
    void unsupportedMigrationRetainsPlaintextAndRetries();
    void indeterminateMigrationCommitIsConfirmed();
    void indeterminateMigrationMissFailsClosedAndRetries();
    void indeterminateMigrationReadFailureRetainsPlaintext();
    void transientReadBeforeMissingCredentialRetriesMigration();
    void scopesAreIsolated();
    void scopeIdentifiersAreStableAndValidated();
    void scopeCreationFailsClosedWhenItCannotPersist();
    void migratePlaintextCoversConfiguredCredentials();
    void gsettingsRoutesCredentialsToVault();
    void gsettingsCheckedCredentialWriteReportsPersistence();
    void gsettingsCheckedCredentialReadDistinguishesMissingAndFailure();
    void gsettingsSyncsOnlyRequestedAthleteFile();
    void constructionDoesNotPersistMigrationState();
    void systemFallbackDoesNotSuppressUserMigration();
    void credentialMetadataDoesNotSuppressSystemMigration();
    void markerlessEstablishedSettingsAreAdoptedWithoutBackfill();
    void partialSystemMigrationResumesWithoutOverwrite();
    void partialGlobalMigrationResumesWithoutOverwrite_data();
    void partialGlobalMigrationResumesWithoutOverwrite();
    void partialAthleteMigrationResumesWithoutOverwrite_data();
    void partialAthleteMigrationResumesWithoutOverwrite();
    void dynamicLegacyKeysMigrateToTheirExactTargets();
    void unknownMigrationStateFailsClosed();
    void migrationSyncFailuresResumeAfterRestart_data();
    void migrationSyncFailuresResumeAfterRestart();
    void credentialClaimFailureDoesNotBlockGlobalMigration();
    void interruptedFreshEnrollmentRecovers_data();
    void interruptedFreshEnrollmentRecovers();
    void pendingEnrollmentIntentIsPathBound_data();
    void pendingEnrollmentIntentIsPathBound();
    void locationEnrollmentIsSerializedAcrossProcesses_data();
    void locationEnrollmentIsSerializedAcrossProcesses();
    void freshEnrollmentIsSerializedAcrossProcesses_data();
    void freshEnrollmentIsSerializedAcrossProcesses();
    void freshEnrollmentCrashRecoveryAcrossProcesses();
    void credentialEnrollmentAuthorityMustBeExternal();
    void credentialEnrollmentAuthorityAliasesFailClosed_data();
    void credentialEnrollmentAuthorityAliasesFailClosed();
    void completedLocationCannotBeReenrolledWithoutLocalMetadata_data();
    void completedLocationCannotBeReenrolledWithoutLocalMetadata();
    void legacyLocationClaimsAreBackfilled_data();
    void legacyLocationClaimsAreBackfilled();
    void newFormatRootlessCredentialIsRetained();
    void newFormatRootlessCredentialRemainsAfterStoreRecovery();
    void authorizedLegacyPlaintextRequiresAuthoritativeVaultMiss_data();
    void authorizedLegacyPlaintextRequiresAuthoritativeVaultMiss();
    void authorizedLegacyGlobalPlaintextRequiresAuthoritativeVaultMiss_data();
    void authorizedLegacyGlobalPlaintextRequiresAuthoritativeVaultMiss();
    void targetCredentialUseBlocksLegacyFallback_data();
    void targetCredentialUseBlocksLegacyFallback();
    void targetPlaintextPrecedesLegacyAfterTransientVaultFailure();
    void emptyTargetPlaintextBlocksLegacyAcrossRestart_data();
    void emptyTargetPlaintextBlocksLegacyAcrossRestart();
    void failedFallbackMarkerPersistenceFailsClosedAndRetries_data();
    void failedFallbackMarkerPersistenceFailsClosedAndRetries();
    void legacyPlaintextRequiresMatchingSourceRoot();
    void legacyPlaintextRequiresMatchingSourceScope();
    void legacyCredentialScopeUsesOneExactSnapshot();
    void authorizedLegacyFallbackUsesOneExactSnapshot();
    void targetAppearingDuringLegacyFallbackTakesPrecedence();
    void canonicalVaultReadRetainsAuthorizedLegacyDuplicate();
    void vanishedCanonicalRetainsAuthorizedLegacyDuplicate();
    void globalCredentialsInDifferentRootsHaveIsolatedScopes();
    void sameNamedAthletesInDifferentRootsHaveIsolatedScopes();
    void preUpgradeCopiedCredentialScopesFailClosed_data();
    void preUpgradeCopiedCredentialScopesFailClosed();
    void preUpgradeCopiedAthleteScopeFailsClosedWhenOpenedFirst();
    void legacyClaimsSurviveLocalBindingWriteFailure();
    void switchingCredentialRootsClearsScopedState();
    void invalidCredentialRootSwitchFailsClosed();
    void copiedCredentialRootFailsClosed();
    void copiedAthleteCredentialProfileFailsClosed();
    void existingFreshRootCannotBootstrapMissingClaims();
    void existingFreshAthleteCannotBootstrapMissingClaims();
    void unavailableClaimedRootCannotBeReboundByCopy();
    void symlinkedGlobalCredentialSettingsFailClosed();
    void localAthleteScopeIsPreservedWithoutCrossRootAdoption();
    void ambiguousLegacyAthleteScopeFailsClosed();
    void ambiguousLegacyPlaintextFailsClosed();
    void malformedRootIdentityFailsClosed();
    void malformedAthleteScopeFailsClosed();
    void escapedAthleteCredentialPathsFailClosed();
    void danglingAthletePrivateSymlinkFailsClosed();
    void invalidAthleteDoesNotDisableValidCredentials();
    void windowsAthleteJunctionFailsClosed();
    void preInitializationUsesLocalAthleteScope();
    void clearedRootDoesNotRetainAthleteScope();
    void credentialBackendWaitDoesNotHoldSettingsMutex();
    void applicationThreadCredentialBackendWaitProcessesKeychainCompletion();
    void applicationThreadKeychainReentrancyDefersSettingsReconfiguration();
    void credentialBackendBlocksSettingsReconfigurationUntilRelease();
    void credentialBackendDoesNotDropSameInstanceAthleteInitialization();
    void credentialWorkerKeychainTimeoutRespectsCallerDeadline();
    void credentialAsyncCompletionHonorsContextLifetime();
    void credentialWorkerKeepsNativeKeychainOnApplicationThread();
    void credentialWorkerRestartsAfterCleanShutdown();
    void stoppedCredentialWorkerGenerationsAreDestroyed();
    void credentialWorkerShutdownAbandonsQueuedOperations();

private:
    QString ownedCredentialStateRoot_;
};

void TestCredentialSettings::initTestCase()
{
    if (qEnvironmentVariableIsSet(
            "GC_CREDENTIAL_TEST_STATE_ROOT")
        || qEnvironmentVariableIsSet(
            "GC_CREDENTIAL_USE_PRODUCTION_STATE_ROOT")) {
        return;
    }
    ownedCredentialStateRoot_ = QDir(QDir::tempPath()).filePath(
        QStringLiteral("gc-credential-test-state-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(ownedCredentialStateRoot_));
    qputenv(
        "GC_CREDENTIAL_TEST_STATE_ROOT",
        QFile::encodeName(ownedCredentialStateRoot_));
}

void TestCredentialSettings::cleanupTestCase()
{
    if (ownedCredentialStateRoot_.isEmpty())
        return;
    QDir directory(ownedCredentialStateRoot_);
    QVERIFY(directory.removeRecursively());
    qunsetenv("GC_CREDENTIAL_TEST_STATE_ROOT");
}

void TestCredentialSettings::init()
{
    useQtKeychainFactory() = false;
    credentialScrubFaultState() = {};
#ifdef GC_CREDENTIAL_TEST_HOOKS
    GSettings::setCredentialLegacyScopeSnapshotHook({});
    GSettings::setCredentialLegacyValueSnapshotHook({});
    CredentialSettingsDetail::
        resetCredentialCacheNowForTest();
    CredentialStoreQtKeychainDetail::resetJobTestHooks();
    GSettings::setCredentialBackendWaitTimeoutForTest(-1);
#endif
    if (!ownedCredentialStateRoot_.isEmpty()) {
        QDir stateDirectory(QDir(ownedCredentialStateRoot_)
            .filePath(QStringLiteral(
                "GoldenCheetah/credential-locks")));
        if (stateDirectory.exists()) {
            QVERIFY(stateDirectory.removeRecursively());
        }
    }
}

void TestCredentialSettings::cleanup()
{
    useQtKeychainFactory() = false;
#ifdef GC_CREDENTIAL_TEST_HOOKS
    CredentialSettingsDetail::
        resetCredentialCacheNowForTest();
    CredentialStoreQtKeychainDetail::resetJobTestHooks();
    CredentialStoreQtKeychainDetail::resetJobGateForTest();
    GSettings::setCredentialBackendWaitTimeoutForTest(-1);
#endif
}

void TestCredentialSettings::credentialClassification_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<bool>("credential");

#define CREDENTIAL_ROW(symbol) \
    QTest::newRow(#symbol) << QStringLiteral(symbol) << true
    CREDENTIAL_ROW(GC_RWGPSPASS);
    CREDENTIAL_ROW(GC_RWGPS_AUTH_TOKEN);
    CREDENTIAL_ROW(GC_TTBPASS);
    CREDENTIAL_ROW(GC_SPORTPLUSHEALTHPASS);
    CREDENTIAL_ROW(GC_SELPASS);
    CREDENTIAL_ROW(GC_WIKEY);
    CREDENTIAL_ROW(GC_DVPASS);
    CREDENTIAL_ROW(GC_DROPBOX_TOKEN);
    CREDENTIAL_ROW(GC_WITHINGS_TOKEN);
    CREDENTIAL_ROW(GC_WITHINGS_SECRET);
    CREDENTIAL_ROW(GC_NOKIA_TOKEN);
    CREDENTIAL_ROW(GC_NOKIA_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_STRAVA_TOKEN);
    CREDENTIAL_ROW(GC_STRAVA_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_STRAVA_PENDING_TRANSACTION);
    CREDENTIAL_ROW(GC_STRAVA_CLIENT_CREDENTIALS);
    CREDENTIAL_ROW(GC_CYCLINGANALYTICS_TOKEN);
    CREDENTIAL_ROW(GC_SIXCYCLE_PASS);
    CREDENTIAL_ROW(GC_AZUM_ACCESS_TOKEN);
    CREDENTIAL_ROW(GC_AZUM_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_AZUM_USERKEY);
    CREDENTIAL_ROW(GC_TREDICT_TOKEN);
    CREDENTIAL_ROW(GC_TREDICT_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_POLARFLOW_TOKEN);
    CREDENTIAL_ROW(GC_SPORTTRACKS_TOKEN);
    CREDENTIAL_ROW(GC_SPORTTRACKS_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_XERTPASS);
    CREDENTIAL_ROW(GC_XERT_TOKEN);
    CREDENTIAL_ROW(GC_XERT_REFRESH_TOKEN);
    CREDENTIAL_ROW(GC_NOLIO_ACCESS_TOKEN);
    CREDENTIAL_ROW(GC_NOLIO_REFRESH_TOKEN);
#undef CREDENTIAL_ROW

#define NON_CREDENTIAL_ROW(symbol) \
    QTest::newRow(#symbol) << QStringLiteral(symbol) << false
    NON_CREDENTIAL_ROW(GC_RWGPSUSER);
    NON_CREDENTIAL_ROW(GC_WIURL);
    NON_CREDENTIAL_ROW(GC_DROPBOX_FOLDER);
    NON_CREDENTIAL_ROW(GC_STRAVA_LAST_REFRESH);
    NON_CREDENTIAL_ROW(GC_AZUM_ATHLETE_ID);
    NON_CREDENTIAL_ROW(GC_NOLIO_LAST_REFRESH);
    NON_CREDENTIAL_ROW(GC_SETTINGS_MAIN_GEOM);
#undef NON_CREDENTIAL_ROW
}

void TestCredentialSettings::credentialClassification()
{
    QFETCH(QString, key);
    QFETCH(bool, credential);
    QCOMPARE(CredentialSettings::isCredentialKey(key), credential);
}

void TestCredentialSettings::
stravaClientCredentialsNeverReachPlaintextSettings()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("athlete-private.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    const QString scope = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    const QString plaintextKey =
        plainKey(GC_STRAVA_CLIENT_CREDENTIALS);
    const QString sentinelSecret = QStringLiteral(
        "sec013-runtime-secret-must-not-reach-qsettings");
    const QString payload = QStringLiteral(
        R"({"client_id":"123456","client_secret":"%1","version":1})")
            .arg(sentinelSecret);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_CLIENT_CREDENTIALS);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_CLIENT_CREDENTIALS,
        plaintextKey, payload));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(!settings.contains(plaintextKey));
    QVERIFY(!fileContents(settingsPath).contains(
        sentinelSecret.toUtf8()));
    QCOMPARE(state->values.value(vaultKey), payload);

    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_CLIENT_CREDENTIALS,
        plaintextKey));
    QVERIFY(!state->values.contains(vaultKey));
    settings.sync();
    QVERIFY(!fileContents(settingsPath).contains(
        sentinelSecret.toUtf8()));
}

void TestCredentialSettings::keychainStatusMapping_data()
{
    QTest::addColumn<int>("error");
    QTest::addColumn<int>("status");

#define STATUS_ROW(error, status) \
    QTest::newRow(#error) << int(QKeychain::error) \
                          << int(CredentialStore::Status::status)
    STATUS_ROW(NoError, Success);
    STATUS_ROW(EntryNotFound, NotFound);
    STATUS_ROW(AccessDeniedByUser, Unavailable);
    STATUS_ROW(AccessDenied, Unavailable);
    STATUS_ROW(NoBackendAvailable, Unavailable);
    STATUS_ROW(NotImplemented, Unavailable);
    STATUS_ROW(CouldNotDeleteEntry, Failed);
    STATUS_ROW(OtherError, Failed);
#undef STATUS_ROW
}

void TestCredentialSettings::keychainStatusMapping()
{
    QFETCH(int, error);
    QFETCH(int, status);
    QCOMPARE(int(CredentialStoreQtKeychainDetail::statusForError(
                 QKeychain::Error(error))), status);
}

void TestCredentialSettings::linuxKeychainRuntimeStatusReport_data()
{
    QTest::addColumn<bool>("compileSupport");
    QTest::addColumn<bool>("runtimeAvailable");
    QTest::addColumn<QByteArray>("expected");

    const QByteArray prefix(
        "goldencheetah_linux_keychain_status=1\n"
        "application=GoldenCheetah\n");
    QTest::newRow("enabled-and-available")
        << true << true
        << prefix
            + "libsecret_compile_support=enabled\n"
              "libsecret_runtime=available\n";
    QTest::newRow("enabled-but-unavailable")
        << true << false
        << prefix
            + "libsecret_compile_support=enabled\n"
              "libsecret_runtime=unavailable\n";
    QTest::newRow("disabled")
        << false << false
        << prefix
            + "libsecret_compile_support=disabled\n"
              "libsecret_runtime=unavailable\n";
    QTest::newRow("disabled-ignores-runtime")
        << false << true
        << prefix
            + "libsecret_compile_support=disabled\n"
              "libsecret_runtime=unavailable\n";
}

void TestCredentialSettings::linuxKeychainRuntimeStatusReport()
{
    QFETCH(bool, compileSupport);
    QFETCH(bool, runtimeAvailable);
    QFETCH(QByteArray, expected);

    QCOMPARE(
        CredentialStoreQtKeychainDetail::linuxRuntimeStatusReport(
            compileSupport, runtimeAvailable),
        expected);
}

void TestCredentialSettings::
bundledLinuxRuntimePathRequiresContainedRegularFile()
{
#ifdef Q_OS_LINUX
    QTemporaryDir applicationDir;
    QTemporaryDir outsideDir;
    QVERIFY(applicationDir.isValid());
    QVERIFY(outsideDir.isValid());
    QVERIFY(QDir(applicationDir.path()).mkpath(
        QStringLiteral("lib")));

    const QString libraryPath = applicationDir.filePath(
        QStringLiteral("lib/libsecret-1.so.0"));
    QVERIFY(CredentialStoreQtKeychainDetail::
        bundledLinuxRuntimePath(applicationDir.path()).isEmpty());

    QFile library(libraryPath);
    QVERIFY(library.open(QIODevice::WriteOnly));
    QCOMPARE(library.write("fixture"), qint64(7));
    library.close();
    QCOMPARE(
        CredentialStoreQtKeychainDetail::bundledLinuxRuntimePath(
            applicationDir.path()),
        QFileInfo(libraryPath).canonicalFilePath());

    QVERIFY(QFile::remove(libraryPath));
    const QString outsidePath = outsideDir.filePath(
        QStringLiteral("libsecret-1.so.0"));
    QFile outside(outsidePath);
    QVERIFY(outside.open(QIODevice::WriteOnly));
    QCOMPARE(outside.write("outside"), qint64(7));
    outside.close();
    QVERIFY(QFile::link(outsidePath, libraryPath));
    QVERIFY(QFileInfo(libraryPath).isSymLink());
    QVERIFY(CredentialStoreQtKeychainDetail::
        bundledLinuxRuntimePath(applicationDir.path()).isEmpty());
#else
    QVERIFY(CredentialStoreQtKeychainDetail::
        bundledLinuxRuntimePath(QStringLiteral("/unused")).isEmpty());
#endif
}

void TestCredentialSettings::keychainJobsDisablePlaintextFallback()
{
    QKeychain::WritePasswordJob job(
        QStringLiteral("credential-test"));
    job.setInsecureFallback(true);
    const QString key = QStringLiteral("opaque-test-key");

    CredentialStoreQtKeychainDetail::configureJob(&job, key);

    QCOMPARE(job.key(), key);
    QVERIFY(!job.insecureFallback());
    QVERIFY(!job.autoDelete());
}

void TestCredentialSettings::
timedOutKeychainMutationRetainsSerialization_data()
{
    QTest::addColumn<bool>("firstRemove");
    QTest::addColumn<bool>("conflictRemove");
    QTest::addColumn<bool>("firstCommits");

    QTest::newRow("write-then-write")
        << false << false << true;
    QTest::newRow("write-then-remove")
        << false << true << true;
    QTest::newRow("remove-then-write")
        << true << false << true;
    QTest::newRow("failed-write-then-write")
        << false << false << false;
    QTest::newRow("failed-remove-then-write")
        << true << false << false;
}

void TestCredentialSettings::
timedOutKeychainMutationRetainsSerialization()
{
    QFETCH(bool, firstRemove);
    QFETCH(bool, conflictRemove);
    QFETCH(bool, firstCommits);

    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());

    const QString key = QStringLiteral("timeout-mutation");
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));
    const QString firstValue = QStringLiteral("first");
    const QString replacement = QStringLiteral("replacement");
    QString vaultValue =
        firstRemove ? QStringLiteral("original") : QString();
    bool vaultContainsKey = firstRemove;
    int startedJobs = 0;
    int startedAtConflict = -1;
    bool firstReturned = false;
    bool firstReturnedAtConflict = false;
    bool firstMutationWasLate = false;
    bool delayedReleased = false;
    bool timeoutObserved = false;
    bool backendLeaseHeldAtConflict = false;
    bool markerPublishedBeforeStart = true;
    CredentialStore::Status conflictStatus =
        CredentialStore::Status::Failed;
    QPointer<QKeychain::Job> delayedJob;

    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [&](QKeychain::Job *job) {
            ++startedJobs;
            markerPublishedBeforeStart =
                markerPublishedBeforeStart
                && CredentialSettingsDetail::
                       backendMutationMarkerStatus(
                           mutationLockPath)
                    == CredentialSettingsDetail::
                       BackendMutationMarkerStatus::Pending;
            if (startedJobs == 1) {
                delayedJob = job;
                return true;
            }
            if (qobject_cast<QKeychain::DeletePasswordJob *>(
                    job)) {
                vaultContainsKey = false;
                vaultValue.clear();
            } else {
                vaultContainsKey = true;
                vaultValue = replacement;
            }
            job->emitFinished();
            return true;
        });

    CredentialStoreQtKeychainDetail::
        setJobTimeoutHookForTest(
            [&](QKeychain::Job *job) {
                timeoutObserved = job == delayedJob;
                QMetaObject::invokeMethod(
                    this,
                    [&]() {
                        firstReturnedAtConflict =
                            firstReturned;
                        QLockFile backendProbe(
                            mutationLockPath);
                        backendProbe.setStaleLockTime(0);
                        backendLeaseHeldAtConflict =
                            !backendProbe.tryLock(0);
                        if (backendProbe.isLocked())
                            backendProbe.unlock();
                        QString error;
                        conflictStatus =
                            conflictRemove
                            ? store->removeCoordinated(
                                  key, &error,
                                  mutationLockPath)
                            : store->writeCoordinated(
                                  key, replacement,
                                  &error,
                                  mutationLockPath);
                        startedAtConflict = startedJobs;
                        QMetaObject::invokeMethod(
                            this,
                            [&]() {
                                delayedReleased = true;
                                firstMutationWasLate =
                                    firstReturned;
                                if (firstCommits) {
                                    if (firstRemove) {
                                        vaultContainsKey = false;
                                        vaultValue.clear();
                                    } else {
                                        vaultContainsKey = true;
                                        vaultValue = firstValue;
                                    }
                                }
                                if (delayedJob) {
                                    if (firstCommits) {
                                        delayedJob->emitFinished();
                                    } else {
                                        delayedJob
                                            ->emitFinishedWithError(
                                                QKeychain::OtherError,
                                                QStringLiteral(
                                                    "delayed failure"));
                                    }
                                }
                            },
                            Qt::QueuedConnection);
                    },
                    Qt::QueuedConnection);
            });

    QString error;
    const CredentialStore::Status firstStatus =
        firstRemove
        ? store->removeCoordinated(
              key, &error, mutationLockPath)
        : store->writeCoordinated(
              key, firstValue, &error,
              mutationLockPath);
    firstReturned = true;

    QTRY_VERIFY_WITH_TIMEOUT(delayedReleased, 1000);
    const CredentialStore::Status retryStatus =
        store->writeCoordinated(
            key, replacement, &error,
            mutationLockPath);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();

    QVERIFY(timeoutObserved);
    QVERIFY(markerPublishedBeforeStart);
    QCOMPARE(firstStatus,
             CredentialStore::Status::Indeterminate);
    QVERIFY(firstReturnedAtConflict);
    QVERIFY(firstMutationWasLate);
    QVERIFY(backendLeaseHeldAtConflict);
    QCOMPARE(conflictStatus,
             CredentialStore::Status::Unavailable);
    QCOMPARE(startedAtConflict, 1);
    QCOMPARE(retryStatus, CredentialStore::Status::Success);
    QVERIFY(vaultContainsKey);
    QCOMPARE(vaultValue, replacement);
}

void TestCredentialSettings::
timedOutKeychainMutationIsBounded_data()
{
    QTest::addColumn<bool>("remove");

    QTest::newRow("write") << false;
    QTest::newRow("remove") << true;
}

void TestCredentialSettings::
timedOutKeychainMutationIsBounded()
{
    QFETCH(bool, remove);

    if (qEnvironmentVariableIsSet(
            "GC_KEYCHAIN_TIMEOUT_CHILD")) {
        std::unique_ptr<CredentialStore> store =
            createQtKeychainCredentialStore();
        QVERIFY(store);
        QTemporaryDir leaseDirectory;
        QVERIFY(leaseDirectory.isValid());
        const QString mutationLockPath =
            leaseDirectory.filePath(
                QStringLiteral("mutation.lock"));
        int startedJobs = 0;
        QPointer<QKeychain::Job> delayedJob;
        CredentialStoreQtKeychainDetail::
            setJobTimeoutForTest(20);
        CredentialStoreQtKeychainDetail::
            setJobStartHookForTest(
                [&](QKeychain::Job *job) {
                    ++startedJobs;
                    delayedJob = job;
                    return true;
                });

        QString error;
        const CredentialStore::Status status =
            remove
            ? store->removeCoordinated(
                  QStringLiteral("bounded-remove"),
                  &error, mutationLockPath)
            : store->writeCoordinated(
                  QStringLiteral("bounded-write"),
                  QStringLiteral("value"),
                  &error, mutationLockPath);
        QLockFile activeProbe(mutationLockPath);
        activeProbe.setStaleLockTime(0);
        const bool backendLeaseHeld =
            !activeProbe.tryLock(0);
        if (activeProbe.isLocked())
            activeProbe.unlock();
        if (delayedJob) {
            delayedJob->emitFinishedWithError(
                QKeychain::OtherError,
                QStringLiteral("test completion"));
        }
        QCoreApplication::sendPostedEvents(
            nullptr, QEvent::DeferredDelete);
        CredentialStoreQtKeychainDetail::
            resetJobTestHooks();
        QLockFile releasedProbe(mutationLockPath);
        releasedProbe.setStaleLockTime(0);
        const bool backendLeaseReleased =
            releasedProbe.tryLock(0);
        if (releasedProbe.isLocked())
            releasedProbe.unlock();

        QCOMPARE(status,
                 CredentialStore::Status::Indeterminate);
        QCOMPARE(startedJobs, 1);
        QVERIFY(backendLeaseHeld);
        QVERIFY(backendLeaseReleased);
        return;
    }

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_TIMEOUT_CHILD"),
        QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.setProgram(
        QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "timedOutKeychainMutationIsBounded:%1")
            .arg(QString::fromLatin1(
                QTest::currentDataTag())),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const bool finished =
        child.waitForFinished(3000);
    if (!finished) {
        child.kill();
        child.waitForFinished();
    }
    const QByteArray output = child.readAll();
    QVERIFY2(finished, output.constData());
    QVERIFY2(child.exitStatus()
                 == QProcess::NormalExit,
             output.constData());
    QVERIFY2(child.exitCode() == 0,
             output.constData());
}

void TestCredentialSettings::
timedOutKeychainReadRetainsSerialization_data()
{
    QTest::addColumn<bool>("terminalError");

    QTest::newRow("late-success") << false;
    QTest::newRow("late-error") << true;
}

void TestCredentialSettings::
timedOutKeychainReadRetainsSerialization()
{
    QFETCH(bool, terminalError);

    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    std::unique_ptr<CredentialStore> competingStore =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QVERIFY(competingStore);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());

    const QString key = QStringLiteral("timeout-read");
    const QString replacement = QStringLiteral("replacement");
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));
    QString vaultValue;
    int startedJobs = 0;
    int startedAtConflict = -1;
    bool firstReturned = false;
    bool firstReturnedAtConflict = false;
    bool delayedReleased = false;
    CredentialStore::Status conflictStatus =
        CredentialStore::Status::Failed;
    QPointer<QKeychain::Job> delayedJob;

    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [&](QKeychain::Job *job) {
            ++startedJobs;
            if (startedJobs == 1) {
                delayedJob = job;
                return true;
            }
            vaultValue = replacement;
            job->emitFinished();
            return true;
        });

    const CredentialStore::ReadResult firstResult =
        store->read(key);
    firstReturned = true;
    store.reset();

    QString error;
    firstReturnedAtConflict = firstReturned;
    conflictStatus =
        competingStore->writeCoordinated(
            key, replacement, &error,
            mutationLockPath);
    startedAtConflict = startedJobs;
    delayedReleased = true;
    if (delayedJob) {
        if (terminalError) {
            delayedJob->emitFinishedWithError(
                QKeychain::EntryNotFound,
                QStringLiteral(
                    "delayed read completed"));
        } else {
            delayedJob->emitFinished();
        }
    }
    const CredentialStore::Status retryStatus =
        competingStore->writeCoordinated(
            key, replacement, &error,
            mutationLockPath);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();

    QCOMPARE(firstResult.status,
             CredentialStore::Status::Unavailable);
    QVERIFY(delayedReleased);
    QVERIFY(firstReturnedAtConflict);
    QCOMPARE(conflictStatus,
             CredentialStore::Status::Unavailable);
    QCOMPARE(startedAtConflict, 1);
    QCOMPARE(retryStatus, CredentialStore::Status::Success);
    QCOMPARE(vaultValue, replacement);
}

void TestCredentialSettings::
destroyedTimedOutReadReleasesGate()
{
    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);

    int startedJobs = 0;
    QPointer<QKeychain::Job> delayedJob;
    CredentialStoreQtKeychainDetail::
        setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                ++startedJobs;
                if (startedJobs == 1) {
                    delayedJob = job;
                    return true;
                }
                job->emitFinished();
                return true;
            });

    const CredentialStore::ReadResult readResult =
        store->read(QStringLiteral("destroyed-read"));
    const bool readStarted = delayedJob;
    delete delayedJob.data();
    QString error;
    const CredentialStore::Status retryStatus =
        store->write(
            QStringLiteral("after-destroyed-read"),
            QStringLiteral("value"), &error);
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();

    QCOMPARE(readResult.status,
             CredentialStore::Status::Unavailable);
    QVERIFY(readStarted);
    QVERIFY(delayedJob.isNull());
    QCOMPARE(retryStatus,
             CredentialStore::Status::Success);
    QCOMPARE(startedJobs, 2);
}

void TestCredentialSettings::
staleTimedOutKeychainJobCannotReleaseNewOwner()
{
    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);

    QPointer<QKeychain::Job> firstJob;
    QPointer<QKeychain::Job> secondJob;
    int startedJobs = 0;
    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [&](QKeychain::Job *job) {
            ++startedJobs;
            if (startedJobs == 1) {
                firstJob = job;
                return true;
            }
            if (startedJobs == 2) {
                secondJob = job;
                return true;
            }
            job->emitFinished();
            return true;
        });

    const CredentialStore::ReadResult firstResult =
        store->read(QStringLiteral("first-timeout"));
    const bool firstStarted = firstJob;
    if (firstJob) {
        firstJob->emitFinishedWithError(
            QKeychain::EntryNotFound,
            QStringLiteral("first read completed"));
    }

    const CredentialStore::ReadResult secondResult =
        store->read(QStringLiteral("second-timeout"));
    const bool secondStarted = secondJob;
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    const bool firstDestroyed = firstJob.isNull();

    QString error;
    const CredentialStore::Status blockedStatus =
        store->write(
            QStringLiteral("blocked-by-second"),
            QStringLiteral("replacement"),
            &error);
    const int startedWhileSecondActive = startedJobs;

    if (secondJob) {
        secondJob->emitFinishedWithError(
            QKeychain::EntryNotFound,
            QStringLiteral("second read completed"));
    }
    const CredentialStore::Status retryStatus =
        store->write(
            QStringLiteral("after-second"),
            QStringLiteral("replacement"),
            &error);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();

    QCOMPARE(firstResult.status,
             CredentialStore::Status::Unavailable);
    QVERIFY(firstStarted);
    QCOMPARE(secondResult.status,
             CredentialStore::Status::Unavailable);
    QVERIFY(secondStarted);
    QVERIFY(firstDestroyed);
    QCOMPARE(blockedStatus,
             CredentialStore::Status::Unavailable);
    QCOMPARE(startedWhileSecondActive, 2);
    QCOMPARE(retryStatus, CredentialStore::Status::Success);
    QCOMPARE(startedJobs, 3);
}

void TestCredentialSettings::
destroyedTimedOutMutationRecoversAfterLeaseRelease()
{
    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));

    int startedJobs = 0;
    QPointer<QKeychain::Job> delayedJob;
    CredentialStoreQtKeychainDetail::
        setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                ++startedJobs;
                if (startedJobs == 1) {
                    delayedJob = job;
                    return true;
                }
                job->emitFinished();
                return true;
            });

    QString error;
    const CredentialStore::Status firstStatus =
        store->writeCoordinated(
            QStringLiteral("destroyed-mutation"),
            QStringLiteral("value"),
            &error, mutationLockPath);
    const bool firstStarted = delayedJob;
    delete delayedJob.data();

    QLockFile releasedProbe(mutationLockPath);
    releasedProbe.setStaleLockTime(0);
    const bool backendLeaseReleased =
        releasedProbe.tryLock(0);
    if (releasedProbe.isLocked())
        releasedProbe.unlock();
    QCOMPARE(
        CredentialSettingsDetail::backendMutationMarkerStatus(
            mutationLockPath),
        CredentialSettingsDetail::BackendMutationMarkerStatus::Pending);
    const CredentialStore::Status recoveredStatus =
        store->writeCoordinated(
            QStringLiteral("recovered-after-destroy"),
            QStringLiteral("replacement"),
            &error, mutationLockPath);
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();

    QCOMPARE(firstStatus,
             CredentialStore::Status::Indeterminate);
    QVERIFY(firstStarted);
    QVERIFY(delayedJob.isNull());
    QVERIFY(backendLeaseReleased);
    QCOMPARE(recoveredStatus,
             CredentialStore::Status::Success);
    QCOMPARE(startedJobs, 2);
    QCOMPARE(
        CredentialSettingsDetail::backendMutationMarkerStatus(
            mutationLockPath),
        CredentialSettingsDetail::BackendMutationMarkerStatus::Absent);
}

void TestCredentialSettings::
keychainMarkerCreationFailureBlocksBackend_data()
{
    QTest::addColumn<QByteArray>("failureStage");

    QTest::newRow("marker-file-sync")
        << QByteArrayLiteral("backend-marker-file");
    QTest::newRow("marker-directory-sync")
        << QByteArrayLiteral("backend-marker-directory");
}

void TestCredentialSettings::
keychainMarkerCreationFailureBlocksBackend()
{
    QFETCH(QByteArray, failureStage);

    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));

    int startedJobs = 0;
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                ++startedJobs;
                job->emitFinished();
                return true;
            });
    ScopedEnvironmentVariable durabilityFailure(
        QByteArrayLiteral(
            "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
        failureStage);

    QString error;
    const CredentialStore::Status firstStatus =
        store->writeCoordinated(
            QStringLiteral("marker-creation"),
            QStringLiteral("value"),
            &error, mutationLockPath);
    const auto markerStatus =
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath);
    QLockFile backendProbe(mutationLockPath);
    backendProbe.setStaleLockTime(0);
    const bool backendLeaseReleased =
        backendProbe.tryLock(0);
    if (backendProbe.isLocked())
        backendProbe.unlock();
    const CredentialStore::Status blockedStatus =
        store->writeCoordinated(
            QStringLiteral("blocked-by-marker"),
            QStringLiteral("replacement"),
            &error, mutationLockPath);
    const int startedBeforeCleanup = startedJobs;

    durabilityFailure.reset();
    const bool markerRemoved =
        CredentialSettingsDetail::
            removeBackendMutationMarker(
                mutationLockPath);
    const CredentialStore::Status retryStatus =
        store->writeCoordinated(
            QStringLiteral("after-cleanup"),
            QStringLiteral("replacement"),
            &error, mutationLockPath);
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();

    QCOMPARE(firstStatus,
             CredentialStore::Status::Unavailable);
    QCOMPARE(
        markerStatus,
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Pending);
    QVERIFY(backendLeaseReleased);
    QCOMPARE(blockedStatus,
             CredentialStore::Status::Unavailable);
    QCOMPARE(startedBeforeCleanup, 0);
    QVERIFY(markerRemoved);
    QCOMPARE(retryStatus, CredentialStore::Status::Success);
    QCOMPARE(startedJobs, 1);
    QCOMPARE(
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath),
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Absent);
}

void TestCredentialSettings::
keychainMarkerCleanupFailureRecoversOnNextLease_data()
{
    QTest::addColumn<QByteArray>("failureStage");
    QTest::addColumn<bool>("markerRemains");

    QTest::newRow("before-unlink")
        << QByteArrayLiteral("backend-marker-remove")
        << true;
    QTest::newRow("after-unlink-before-directory-sync")
        << QByteArrayLiteral(
               "backend-marker-remove-directory")
        << false;
}

void TestCredentialSettings::
keychainMarkerCleanupFailureRecoversOnNextLease()
{
    QFETCH(QByteArray, failureStage);
    QFETCH(bool, markerRemains);

    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));

    int startedJobs = 0;
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                ++startedJobs;
                job->emitFinished();
                return true;
            });
    ScopedEnvironmentVariable durabilityFailure(
        QByteArrayLiteral(
            "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
        failureStage);

    QString error;
    const CredentialStore::Status firstStatus =
        store->writeCoordinated(
            QStringLiteral("marker-cleanup"),
            QStringLiteral("value"),
            &error, mutationLockPath);
    const auto markerStatus =
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath);
    QLockFile backendProbe(mutationLockPath);
    backendProbe.setStaleLockTime(0);
    const bool backendLeaseReleased =
        backendProbe.tryLock(0);
    if (backendProbe.isLocked())
        backendProbe.unlock();
    durabilityFailure.reset();
    const CredentialStore::Status retryStatus =
        store->writeCoordinated(
            QStringLiteral("after-cleanup-failure"),
            QStringLiteral("replacement"),
            &error, mutationLockPath);
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();
    const auto markerAfterRetry =
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath);

    QCOMPARE(firstStatus,
             CredentialStore::Status::Indeterminate);
    QCOMPARE(
        markerStatus,
        markerRemains
            ? CredentialSettingsDetail::
                  BackendMutationMarkerStatus::Pending
            : CredentialSettingsDetail::
                  BackendMutationMarkerStatus::Absent);
    QVERIFY(backendLeaseReleased);
    QCOMPARE(retryStatus, CredentialStore::Status::Success);
    QCOMPARE(startedJobs, 2);
    QCOMPARE(
        markerAfterRetry,
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Absent);
}

void TestCredentialSettings::
timedOutKeychainMutationFromWorkerIsBounded()
{
    if (!qEnvironmentVariableIsSet(
            "GC_KEYCHAIN_WORKER_TIMEOUT_CHILD")) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral(
                "GC_KEYCHAIN_WORKER_TIMEOUT_CHILD"),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "timedOutKeychainMutationFromWorkerIsBounded"),
            QStringLiteral("-silent")
        });
        child.start();
        QVERIFY2(child.waitForStarted(5000),
                 qPrintable(child.errorString()));
        const bool finished =
            child.waitForFinished(3000);
        if (!finished) {
            child.kill();
            child.waitForFinished();
        }
        const QByteArray output = child.readAll();
        QVERIFY2(finished, output.constData());
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit,
            output.constData());
        QVERIFY2(child.exitCode() == 0,
                 output.constData());
        return;
    }

    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    QTemporaryDir leaseDirectory;
    QVERIFY(leaseDirectory.isValid());
    const QString mutationLockPath =
        leaseDirectory.filePath(
            QStringLiteral("mutation.lock"));

    QPointer<QKeychain::Job> delayedJob;
    CredentialStoreQtKeychainDetail::
        setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                delayedJob = job;
                return true;
            });

    std::atomic<bool> returned{false};
    CredentialStore::Status status =
        CredentialStore::Status::Failed;
    QString error;
    std::thread worker([&]() {
        status = store->writeCoordinated(
            QStringLiteral("worker-mutation"),
            QStringLiteral("value"),
            &error, mutationLockPath);
        returned.store(true);
    });
    QElapsedTimer wait;
    wait.start();
    while (!returned.load()
           && wait.elapsed() < 1000) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    const bool returnedInTime = returned.load();
    if (!returnedInTime && delayedJob) {
        delayedJob->emitFinishedWithError(
            QKeychain::OtherError,
            QStringLiteral("worker timeout cleanup"));
    }
    worker.join();

    const bool jobStillActive = delayedJob;
    if (delayedJob) {
        delayedJob->emitFinishedWithError(
            QKeychain::OtherError,
            QStringLiteral("worker completion"));
    }
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();

    QCOMPARE(status,
             CredentialStore::Status::Indeterminate);
    QVERIFY(returnedInTime);
    QVERIFY(jobStillActive);
}

void TestCredentialSettings::
timedOutKeychainMutationBlocksCredentialStateUntilTerminal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString plaintextKey =
        plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey =
        CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath =
        credentialOperationFile(
            stateRoot, vaultKey,
            QStringLiteral(".deletion"));
    const QString firstValue =
        QStringLiteral("first-value");
    const QString replacement =
        QStringLiteral("replacement");

    int startedJobs = 0;
    QString fakeVaultValue;
    QPointer<QKeychain::Job> delayedJob;
    CredentialStoreQtKeychainDetail::
        setJobTimeoutForTest(20);
    CredentialStoreQtKeychainDetail::
        setJobStartHookForTest(
            [&](QKeychain::Job *job) {
                ++startedJobs;
                if (startedJobs == 1) {
                    delayedJob = job;
                    return true;
                }
                fakeVaultValue = replacement;
                job->emitFinished();
                return true;
            });

    CredentialSettings credentials(
        createQtKeychainCredentialStore());
    const bool firstPersisted =
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, firstValue);
    const QByteArray firstTransaction =
        credentialPhaseTransaction(
            deletionPath,
            QByteArrayLiteral("creating"));
    const QVariant valueWhilePending =
        credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey,
            QStringLiteral("operation-pending"));
    const bool replacementWhilePending =
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, replacement);
    const QByteArray transactionWhilePending =
        credentialPhaseTransaction(
            deletionPath,
            QByteArrayLiteral("creating"));
    const int jobsWhilePending = startedJobs;

    const bool firstJobActive = delayedJob;
    if (delayedJob) {
        fakeVaultValue = firstValue;
        delayedJob->emitFinished();
    }
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    const bool replacementPersisted =
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, replacement);
    const bool activeAfterReplacement =
        credentialPhaseIs(
            deletionPath, QByteArrayLiteral("active"));
    CredentialStoreQtKeychainDetail::
        resetJobTestHooks();

    QVERIFY(!firstPersisted);
    QVERIFY(!firstTransaction.isEmpty());
    QCOMPARE(
        valueWhilePending,
        QVariant(QStringLiteral("operation-pending")));
    QVERIFY(!replacementWhilePending);
    QCOMPARE(transactionWhilePending, firstTransaction);
    QCOMPARE(jobsWhilePending, 1);
    QVERIFY(firstJobActive);
    QVERIFY(replacementPersisted);
    QCOMPARE(startedJobs, 2);
    QCOMPARE(fakeVaultValue, replacement);
    QVERIFY(activeAfterReplacement);
}

void TestCredentialSettings::
timedOutKeychainMutationLeaseIsCrossProcess_data()
{
    QTest::addColumn<bool>("crash");
    QTest::addColumn<bool>("remove");

    QTest::newRow("write-terminal")
        << false << false;
    QTest::newRow("remove-terminal")
        << false << true;
    QTest::newRow("write-crash")
        << true << false;
    QTest::newRow("remove-crash")
        << true << true;
}

void TestCredentialSettings::
timedOutKeychainMutationLeaseIsCrossProcess()
{
    QFETCH(bool, crash);
    QFETCH(bool, remove);

    if (qEnvironmentVariableIsSet(
            "GC_KEYCHAIN_LEASE_CHILD")) {
        const QString settingsPath =
            qEnvironmentVariable(
                "GC_KEYCHAIN_LEASE_SETTINGS");
        const QString scope =
            qEnvironmentVariable(
                "GC_KEYCHAIN_LEASE_SCOPE");
        const QString readyPath =
            qEnvironmentVariable(
                "GC_KEYCHAIN_LEASE_READY");
        const QString releasePath =
            qEnvironmentVariable(
                "GC_KEYCHAIN_LEASE_RELEASE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!scope.isEmpty());
        QVERIFY(!readyPath.isEmpty());
        QVERIFY(!releasePath.isEmpty());

        const QString stateRoot =
            qEnvironmentVariable(
                "GC_CREDENTIAL_TEST_STATE_ROOT");
        const QString vaultKey =
            CredentialSettings::vaultKey(
                scope, GC_STRAVA_TOKEN);
        const QString mutationLockPath =
            credentialOperationFile(
                stateRoot, vaultKey,
                QStringLiteral(".backend.lock"));
        QPointer<QKeychain::Job> delayedJob;
        bool markerPublishedBeforeStart = false;
        CredentialStoreQtKeychainDetail::
            setJobTimeoutForTest(20);
        CredentialStoreQtKeychainDetail::
            setJobStartHookForTest(
                [&](QKeychain::Job *job) {
                    markerPublishedBeforeStart =
                        CredentialSettingsDetail::
                            backendMutationMarkerStatus(
                                mutationLockPath)
                        == CredentialSettingsDetail::
                            BackendMutationMarkerStatus::Pending;
                    delayedJob = job;
                    return true;
                });
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(
            createQtKeychainCredentialStore());
        const bool persisted =
            remove
            ? credentials.removeChecked(
                  &settings, scope, GC_STRAVA_TOKEN,
                  plainKey(GC_STRAVA_TOKEN))
            : credentials.setValueChecked(
                  &settings, scope, GC_STRAVA_TOKEN,
                  plainKey(GC_STRAVA_TOKEN),
                  QStringLiteral("child-value"));
        const bool jobActive = delayedJob;
        const bool readyWritten =
            !persisted && jobActive
            && markerPublishedBeforeStart
            && writeSignalFile(readyPath);
        if (!readyWritten) {
            if (delayedJob) {
                delayedJob->emitFinishedWithError(
                    QKeychain::OtherError,
                    QStringLiteral("child cleanup"));
            }
            QVERIFY(readyWritten);
            return;
        }

        if (crash)
            std::_Exit(87);

        const bool released =
            waitForFile(releasePath, 5000);
        if (delayedJob) {
            delayedJob->emitFinishedWithError(
                QKeychain::OtherError,
                QStringLiteral("child terminal"));
        }
        QCoreApplication::sendPostedEvents(
            nullptr, QEvent::DeferredDelete);
        QVERIFY(released);
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    const QString settingsPath =
        temporary.filePath(
            QStringLiteral("private.ini"));
    const QString readyPath =
        temporary.filePath(
            QStringLiteral("child-ready"));
    const QString releasePath =
        temporary.filePath(
            QStringLiteral("child-release"));
    const QString scope =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString vaultKey =
        CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
    const QString mutationLockPath =
        credentialOperationFile(
            stateRoot, vaultKey,
            QStringLiteral(".backend.lock"));

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_LEASE_CHILD"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_LEASE_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_LEASE_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_LEASE_READY"),
        readyPath);
    environment.insert(
        QStringLiteral("GC_KEYCHAIN_LEASE_RELEASE"),
        releasePath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    child.setProcessEnvironment(environment);
    child.setProgram(
        QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "timedOutKeychainMutationLeaseIsCrossProcess:%1")
            .arg(QString::fromLatin1(
                QTest::currentDataTag())),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const bool childReady =
        waitForFile(readyPath, 5000, &child);
    QByteArray childOutput = child.readAll();
    QVERIFY2(childReady, childOutput.constData());

    if (crash) {
        QVERIFY2(child.waitForFinished(5000),
                 qPrintable(child.errorString()));
        childOutput += child.readAll();
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit,
            childOutput.constData());
        QCOMPARE(child.exitCode(), 87);
    }

    QCOMPARE(
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath),
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Pending);

    QLockFile backendProbe(mutationLockPath);
    backendProbe.setStaleLockTime(0);
    const bool backendLockAcquired =
        backendProbe.tryLock(crash ? 1000 : 0);
    if (backendProbe.isLocked())
        backendProbe.unlock();
    QCOMPARE(backendLockAcquired, crash);

    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        settingsPath, QSettings::IniFormat);
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    const bool blockedWrite =
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("parent-value"));
    QCOMPARE(blockedWrite, crash);
    QCOMPARE(state->writes, crash ? 1 : 0);
    QCOMPARE(state->removes, crash && remove ? 1 : 0);

    if (crash) {
        QCOMPARE(
            CredentialSettingsDetail::
                backendMutationMarkerStatus(mutationLockPath),
            CredentialSettingsDetail::
                BackendMutationMarkerStatus::Absent);
        return;
    }

    QVERIFY(writeSignalFile(releasePath));
    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    childOutput += child.readAll();
    QVERIFY2(child.exitStatus()
                 == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());
    QCOMPARE(
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath),
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Absent);
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("parent-value")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, remove ? 1 : 0);
}

void TestCredentialSettings::
invalidKeychainMutationMarkerBlocksCredentialState_data()
{
    QTest::addColumn<int>("fixture");

    QTest::newRow("malformed") << 0;
    QTest::newRow("truncated") << 1;
    QTest::newRow("directory") << 2;
#ifdef Q_OS_UNIX
    QTest::newRow("symbolic-link") << 3;
    QTest::newRow("hard-link") << 4;
#endif
}

void TestCredentialSettings::
invalidKeychainMutationMarkerBlocksCredentialState()
{
    QFETCH(int, fixture);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString vaultKey =
        CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
    const QString mutationLockPath =
        credentialOperationFile(
            stateRoot, vaultKey,
            QStringLiteral(".backend.lock"));
    const QString markerPath =
        mutationLockPath + QStringLiteral(".pending");
    const QByteArray validMarker =
        QByteArrayLiteral(
            "goldencheetah_backend_mutation_pending=1\n");
    QVERIFY(QDir().mkpath(
        QFileInfo(markerPath).absolutePath()));

    if (fixture == 0) {
        QVERIFY(writePrivateStateFile(
            markerPath,
            QByteArrayLiteral("not-a-valid-marker\n")));
    } else if (fixture == 1) {
        QVERIFY(writePrivateStateFile(
            markerPath, validMarker.chopped(1)));
    } else if (fixture == 2) {
        QVERIFY(QDir().mkpath(markerPath));
#ifdef Q_OS_UNIX
    } else {
        const QString targetPath =
            temporary.filePath(
                fixture == 3
                    ? QStringLiteral("symlink-target")
                    : QStringLiteral("hardlink-target"));
        QVERIFY(writePrivateStateFile(
            targetPath, validMarker));
        const QByteArray encodedTarget =
            QFile::encodeName(targetPath);
        const QByteArray encodedMarker =
            QFile::encodeName(markerPath);
        const int linkResult = fixture == 3
            ? ::symlink(
                  encodedTarget.constData(),
                  encodedMarker.constData())
            : ::link(
                  encodedTarget.constData(),
                  encodedMarker.constData());
        QCOMPARE(linkResult, 0);
#endif
    }

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("existing-secret"));
    CredentialSettings credentials(fakeStore(state));
    const QVariant observed = credentials.value(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("blocked"));
    const bool writeSucceeded =
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("replacement"));
    const bool removalSucceeded =
        credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN));

    QCOMPARE(
        CredentialSettingsDetail::
            backendMutationMarkerStatus(
                mutationLockPath),
        CredentialSettingsDetail::
            BackendMutationMarkerStatus::Invalid);
    QCOMPARE(observed,
             QVariant(QStringLiteral("blocked")));
    QVERIFY(!writeSucceeded);
    QVERIFY(!removalSucceeded);
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("existing-secret"));
}

void TestCredentialSettings::platformStoreRoundTripsOrFailsClosed()
{
    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    const QString key = QStringLiteral("integration-test/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString secret = QStringLiteral("credential-test-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);

    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    const CredentialStore::Status writeStatus =
        store->write(key, secret, &error);
    QVERIFY2(elapsed.elapsed() < 17000,
             "Credential store write exceeded its timeout");

    if (writeStatus != CredentialStore::Status::Success) {
        QVERIFY(writeStatus == CredentialStore::Status::Unavailable
                || writeStatus
                    == CredentialStore::Status::Indeterminate
                || writeStatus == CredentialStore::Status::Failed);
        return;
    }

    const CredentialStore::ReadResult readResult = store->read(key);
    QCOMPARE(int(readResult.status),
             int(CredentialStore::Status::Success));
    QCOMPARE(readResult.value, secret);
    QCOMPARE(int(store->remove(key, &error)),
             int(CredentialStore::Status::Success));
    QCOMPARE(int(store->read(key).status),
             int(CredentialStore::Status::NotFound));
}

void TestCredentialSettings::
platformCreateIfAbsentIsBoundedAndUnsupported()
{
    std::unique_ptr<CredentialStore> store =
        createQtKeychainCredentialStore();
    QVERIFY(store);
    const QString key = QStringLiteral("credential-create-test/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString firstSecret = QStringLiteral("first-value-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);

    const CredentialStore::CreateResult first =
        store->createIfAbsent(key, firstSecret);
    QCOMPARE(
        int(first.status),
        int(CredentialStore::CreateStatus::Unsupported));
}

void TestCredentialSettings::
fileStoreCreateIfAbsentIsAtomicAcrossProcesses()
{
    if (qEnvironmentVariableIsSet(
            "GC_CREDENTIAL_CREATE_CHILD")) {
        const QString vaultPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_VAULT");
        const QString key = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_KEY");
        const QString value = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_VALUE");
        const QString readyPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_READY");
        const QString startPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_START");
        const QString resultPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_RESULT");
        const QString missingPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_MISSING");
        const QString releasePath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_RELEASE");
        const QString contentionPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CREATE_CONTENTION");
        QVERIFY(!vaultPath.isEmpty());
        QVERIFY(!key.isEmpty());
        QVERIFY(!value.isEmpty());
        QVERIFY(!readyPath.isEmpty());
        QVERIFY(!startPath.isEmpty());
        QVERIFY(!resultPath.isEmpty());
        QVERIFY(writeSignalFile(readyPath));
        QVERIFY(waitForFile(startPath, 5000));

        bool coordinationSucceeded = true;
        FileCredentialStore store(
            vaultPath,
            [&] {
                if (missingPath.isEmpty())
                    return;
                coordinationSucceeded =
                    writeSignalFile(missingPath)
                    && (releasePath.isEmpty()
                        || waitForFile(releasePath, 5000));
            },
            [&] {
                if (!contentionPath.isEmpty()) {
                    coordinationSucceeded =
                        coordinationSucceeded
                        && writeSignalFile(contentionPath);
                }
            });
        const CredentialStore::CreateResult result =
            store.createIfAbsent(key, value);
        QVERIFY(coordinationSucceeded);
        QVERIFY(result.status
                    == CredentialStore::CreateStatus::Created
                || result.status
                    == CredentialStore::CreateStatus::AlreadyExists);
        QVERIFY(writePrivateStateFile(
            resultPath,
            QByteArray::number(int(result.status))));
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString key = QStringLiteral("shared-key");
    const QString firstValue = QStringLiteral("first-value");
    const QString secondValue = QStringLiteral("second-value");
    const QString firstStart =
        temporary.filePath(QStringLiteral("first-start"));
    const QString secondStart =
        temporary.filePath(QStringLiteral("second-start"));
    const QString firstReady =
        temporary.filePath(QStringLiteral("first-ready"));
    const QString secondReady =
        temporary.filePath(QStringLiteral("second-ready"));
    const QString firstResult =
        temporary.filePath(QStringLiteral("first-result"));
    const QString secondResult =
        temporary.filePath(QStringLiteral("second-result"));
    const QString firstMissing =
        temporary.filePath(QStringLiteral("first-missing"));
    const QString secondMissing =
        temporary.filePath(QStringLiteral("second-missing"));
    const QString releaseFirst =
        temporary.filePath(QStringLiteral("release-first"));
    const QString secondContention =
        temporary.filePath(QStringLiteral("second-contention"));

    const auto configureChild = [&](
        QProcess *child,
        const QString &value,
        const QString &ready,
        const QString &start,
        const QString &result,
        const QString &missing,
        const QString &release,
        const QString &contention) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_CHILD"),
            QStringLiteral("1"));
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_VAULT"),
            vaultPath);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_KEY"),
            key);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_VALUE"),
            value);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_READY"),
            ready);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_START"),
            start);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_RESULT"),
            result);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_MISSING"),
            missing);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_RELEASE"),
            release);
        environment.insert(
            QStringLiteral("GC_CREDENTIAL_CREATE_CONTENTION"),
            contention);
        child->setProcessEnvironment(environment);
        child->setProgram(
            QCoreApplication::applicationFilePath());
        child->setArguments({
            QStringLiteral(
                "fileStoreCreateIfAbsentIsAtomicAcrossProcesses"),
            QStringLiteral("-silent")
        });
    };

    QProcess first;
    QProcess second;
    configureChild(
        &first, firstValue, firstReady, firstStart,
        firstResult, firstMissing, releaseFirst, QString());
    configureChild(
        &second, secondValue, secondReady, secondStart,
        secondResult, secondMissing, QString(),
        secondContention);
    first.start();
    QVERIFY2(first.waitForStarted(5000),
             qPrintable(first.errorString()));
    QVERIFY2(waitForFile(firstReady, 5000, &first),
             first.readAll().constData());
    QVERIFY(writeSignalFile(firstStart));
    QVERIFY2(waitForFile(firstMissing, 5000, &first),
             first.readAll().constData());

    second.start();
    QVERIFY2(second.waitForStarted(5000),
             qPrintable(second.errorString()));
    QVERIFY2(waitForFile(secondReady, 5000, &second),
             second.readAll().constData());
    QVERIFY(writeSignalFile(secondStart));
    QVERIFY2(waitForFile(secondContention, 5000, &second),
             second.readAll().constData());
    QCOMPARE(second.state(), QProcess::Running);
    QVERIFY(writeSignalFile(releaseFirst));
    QVERIFY2(first.waitForFinished(10000),
             qPrintable(first.errorString()));
    QVERIFY2(second.waitForFinished(10000),
             qPrintable(second.errorString()));
    const QByteArray firstOutput = first.readAll();
    const QByteArray secondOutput = second.readAll();
    QVERIFY2(first.exitStatus() == QProcess::NormalExit
                 && first.exitCode() == 0,
             firstOutput.constData());
    QVERIFY2(second.exitStatus() == QProcess::NormalExit
                 && second.exitCode() == 0,
             secondOutput.constData());

    bool firstParsed = false;
    bool secondParsed = false;
    const int firstStatus =
        fileContents(firstResult).trimmed().toInt(
            &firstParsed);
    const int secondStatus =
        fileContents(secondResult).trimmed().toInt(
            &secondParsed);
    QVERIFY(firstParsed);
    QVERIFY(secondParsed);
    const int created =
        int(CredentialStore::CreateStatus::Created);
    const int alreadyExists =
        int(CredentialStore::CreateStatus::AlreadyExists);
    QCOMPARE(firstStatus, created);
    QCOMPARE(secondStatus, alreadyExists);
    QVERIFY(!QFileInfo::exists(secondMissing));

    FileCredentialStore store(vaultPath);
    const CredentialStore::ReadResult final =
        store.read(key);
    QCOMPARE(
        int(final.status),
        int(CredentialStore::Status::Success));
    QCOMPARE(
        final.value,
        firstValue);
}

void TestCredentialSettings::vaultCallsDoNotMutateFallbackState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    QVERIFY(ini.fallbacksEnabled());
    const QString scope = QStringLiteral("scope");

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        CredentialSettings::vaultKey(scope, GC_STRAVA_TOKEN),
        QStringLiteral("read-secret"));
    bool readFallbacks = false;
    bool writeFallbacks = false;
    bool removeFallbacks = false;
    state->beforeRead = [&] {
        readFallbacks = ini.fallbacksEnabled();
    };
    state->beforeWrite = [&] {
        writeFallbacks = ini.fallbacksEnabled();
    };
    state->beforeRemove = [&] {
        removeFallbacks = ini.fallbacksEnabled();
    };

    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN),
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("read-secret")));
    QVERIFY(credentials.setValueChecked(
        &ini, scope, GC_STRAVA_REFRESH_TOKEN,
        plainKey(GC_STRAVA_REFRESH_TOKEN),
        QStringLiteral("written-secret")));
    QVERIFY(credentials.removeChecked(
        &ini, scope, GC_NOKIA_TOKEN,
        plainKey(GC_NOKIA_TOKEN)));

    QVERIFY(readFallbacks);
    QVERIFY(writeFallbacks);
    QVERIFY(removeFallbacks);
    QVERIFY(ini.fallbacksEnabled());
}

void TestCredentialSettings::credentialOperationsAreSerialized()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath =
        temporary.filePath(QStringLiteral("first.ini"));
    const QString secondPath =
        temporary.filePath(QStringLiteral("second.ini"));

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    const auto observeWrite = [&] {
        const int current = active.fetch_add(1) + 1;
        int observed = maximum.load();
        while (observed < current
               && !maximum.compare_exchange_weak(
                   observed, current)) {
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        active.fetch_sub(1);
    };

    auto firstState = std::make_shared<FakeStoreState>();
    auto secondState = std::make_shared<FakeStoreState>();
    firstState->beforeWrite = observeWrite;
    secondState->beforeWrite = observeWrite;
    CredentialSettings first(fakeStore(firstState));
    CredentialSettings second(fakeStore(secondState));
    bool firstResult = false;
    bool secondResult = false;

    std::thread firstThread([&] {
        QSettings settings(firstPath, QSettings::IniFormat);
        ready.fetch_add(1);
        while (!start.load())
            std::this_thread::yield();
        firstResult = first.setValueChecked(
            &settings, QStringLiteral("first-scope"),
            GC_STRAVA_TOKEN, plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("first-secret"));
    });
    std::thread secondThread([&] {
        QSettings settings(secondPath, QSettings::IniFormat);
        ready.fetch_add(1);
        while (!start.load())
            std::this_thread::yield();
        secondResult = second.setValueChecked(
            &settings, QStringLiteral("second-scope"),
            GC_STRAVA_TOKEN, plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("second-secret"));
    });

    while (ready.load() != 2)
        std::this_thread::yield();
    start.store(true);
    firstThread.join();
    secondThread.join();

    QVERIFY(firstResult);
    QVERIFY(secondResult);
    QCOMPARE(maximum.load(), 1);
}

void TestCredentialSettings::
reentrantCredentialOperationFailsFast()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVariant reentrantValue;
    qint64 reentrantElapsed = -1;
    state->beforeWrite = [&] {
        QElapsedTimer timer;
        timer.start();
        reentrantValue = credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("operation-busy"));
        reentrantElapsed = timer.elapsed();
    };

    QVERIFY(credentials.setValueChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("written-secret")));
    QCOMPARE(reentrantValue,
             QVariant(QStringLiteral("operation-busy")));
    QVERIFY(reentrantElapsed >= 0);
    QVERIFY(reentrantElapsed < 2000);
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 1);
}

void TestCredentialSettings::
reentrantIndependentCredentialOperationSucceeds()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings firstSettings(
        temporary.filePath(QStringLiteral("first-private.ini")),
        QSettings::IniFormat);
    QSettings secondSettings(
        temporary.filePath(QStringLiteral("second-private.ini")),
        QSettings::IniFormat);
    const QString firstScope = QStringLiteral("first-scope");
    const QString secondScope = QStringLiteral("second-scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);

    auto firstState = std::make_shared<FakeStoreState>();
    auto secondState = std::make_shared<FakeStoreState>();
    firstState->values.insert(
        CredentialSettings::vaultKey(firstScope, GC_STRAVA_TOKEN),
        QStringLiteral("first-secret"));
    secondState->values.insert(
        CredentialSettings::vaultKey(secondScope, GC_STRAVA_TOKEN),
        QStringLiteral("second-secret"));
    CredentialSettings first(fakeStore(firstState));
    CredentialSettings second(fakeStore(secondState));

    QVariant nestedValue;
    firstState->beforeRead = [&] {
        nestedValue = second.value(
            &secondSettings, secondScope,
            GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("operation-busy"));
    };

    QCOMPARE(first.value(
                 &firstSettings, firstScope,
                 GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("first-secret")));
    QCOMPARE(nestedValue,
             QVariant(QStringLiteral("second-secret")));
    QCOMPARE(firstState->reads, 1);
    QCOMPARE(secondState->reads, 1);
}

void TestCredentialSettings::credentialProcessLockIsExclusive()
{
    const bool childMode =
        qEnvironmentVariableIsSet("GC_CREDENTIAL_LOCK_CHILD");
    if (childMode) {
        const QString settingsPath = qEnvironmentVariable(
            "GC_CREDENTIAL_LOCK_SETTINGS");
        const QString readyPath = qEnvironmentVariable(
            "GC_CREDENTIAL_LOCK_READY");
        const QString scope = qEnvironmentVariable(
            "GC_CREDENTIAL_LOCK_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!readyPath.isEmpty());
        QVERIFY(!scope.isEmpty());

        QSettings settings(settingsPath, QSettings::IniFormat);
        auto state = std::make_shared<FakeStoreState>();
        state->beforeWrite = [&] {
            writeSignalFile(readyPath);
            const int configuredHold =
                qEnvironmentVariableIntValue(
                    "GC_CREDENTIAL_LOCK_HOLD_MS");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(
                    configuredHold > 0
                        ? configuredHold : 750));
        };
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("child-secret")));
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString readyPath =
        temporary.filePath(QStringLiteral("child-ready"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString dataRoot =
        temporary.filePath(QStringLiteral("shared-data"));
    const QString parentCache =
        temporary.filePath(QStringLiteral("parent-cache"));
    const QString childCache =
        temporary.filePath(QStringLiteral("child-cache"));
    const QString parentRuntime =
        temporary.filePath(QStringLiteral("parent-runtime"));
    const QString childRuntime =
        temporary.filePath(QStringLiteral("child-runtime"));
    QVERIFY(QDir().mkpath(dataRoot));
    QVERIFY(QDir().mkpath(parentCache));
    QVERIFY(QDir().mkpath(childCache));
    QVERIFY(QDir().mkpath(parentRuntime));
    QVERIFY(QDir().mkpath(childRuntime));
    QVERIFY(QFile::setPermissions(
        parentRuntime,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(
        childRuntime,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    ScopedUnsetEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"));
    ScopedEnvironmentVariable dataEnvironment(
        QByteArrayLiteral("XDG_DATA_HOME"),
        QFile::encodeName(dataRoot));
    ScopedEnvironmentVariable cacheEnvironment(
        QByteArrayLiteral("XDG_CACHE_HOME"),
        QFile::encodeName(parentCache));
    ScopedEnvironmentVariable runtimeEnvironment(
        QByteArrayLiteral("XDG_RUNTIME_DIR"),
        QFile::encodeName(parentRuntime));

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_CHILD"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_READY"),
        readyPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_SCOPE"),
        scope);
    environment.remove(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"));
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_USE_PRODUCTION_STATE_ROOT"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("XDG_DATA_HOME"),
        dataRoot);
    environment.insert(
        QStringLiteral("XDG_CACHE_HOME"),
        childCache);
    environment.insert(
        QStringLiteral("XDG_RUNTIME_DIR"),
        childRuntime);
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral("credentialProcessLockIsExclusive"),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));

    const bool childReady =
        waitForFile(readyPath, 5000, &child);
    const QByteArray startupOutput = child.readAll();
    QVERIFY2(childReady, startupOutput.constData());

    QSettings settings(settingsPath, QSettings::IniFormat);
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QElapsedTimer rejectionTimer;
    rejectionTimer.start();
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("parent-secret")));
    QVERIFY(rejectionTimer.elapsed() < 500);
    QCOMPARE(state->writes, 0);

    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    const QByteArray childOutput =
        startupOutput + child.readAll();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("parent-secret")));
    QCOMPARE(state->writes, 1);
}

void TestCredentialSettings::
contendedCredentialReadWaitsForOwner_data()
{
    QTest::addColumn<bool>("releaseOwner");
    QTest::addColumn<bool>("backendQuarantined");
    QTest::newRow("owner-releases") << true << false;
    QTest::newRow("timeout") << false << false;
    QTest::newRow("pending-backend-marker") << true << true;
}

void TestCredentialSettings::
contendedCredentialReadWaitsForOwner()
{
    QFETCH(bool, releaseOwner);
    QFETCH(bool, backendQuarantined);

    const QString settingsPath = qEnvironmentVariable(
        "GC_CREDENTIAL_READ_LOCK_SETTINGS");
    if (!settingsPath.isEmpty()) {
        const QString scope = qEnvironmentVariable(
            "GC_CREDENTIAL_READ_LOCK_SCOPE");
        QVERIFY(!scope.isEmpty());

        const QString missing = QStringLiteral("missing");
        const QString secret = QStringLiteral("stored-secret");
        const QString vaultKey = CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
        auto state = std::make_shared<FakeStoreState>();
        state->values.insert(vaultKey, secret);
        QSettings settings(settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(fakeStore(state));
        QElapsedTimer timer;
        timer.start();
        bool authoritativeMiss = true;
        bool confirmedVaultValue = true;
        const QVariant value = credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN), missing,
            CredentialSettings::ReadPolicy::RequireLiveVault,
            &authoritativeMiss, &confirmedVaultValue);

        const bool expectSecret =
            releaseOwner && !backendQuarantined;
        QCOMPARE(value, QVariant(
            expectSecret ? secret : missing));
        QVERIFY(!authoritativeMiss);
        QCOMPARE(confirmedVaultValue, expectSecret);
        if (!releaseOwner) {
            QVERIFY2(
                timer.elapsed() >= 4500,
                qPrintable(QStringLiteral(
                    "contended read returned after %1 ms")
                    .arg(timer.elapsed())));
            QVERIFY2(
                timer.elapsed() < 10000,
                qPrintable(QStringLiteral(
                    "contended read timed out after %1 ms")
                    .arg(timer.elapsed())));
        }
        QCOMPARE(state->reads, expectSecret ? 1 : 0);
        QCOMPARE(state->writes, 0);
        QCOMPARE(state->removes, 0);
        QCOMPARE(state->values.value(vaultKey), secret);
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    const QString privateSettingsPath = temporary.filePath(
        QStringLiteral("private.ini"));
    const QString scope = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString contentionPath = temporary.filePath(
        QStringLiteral("credential-lock-contended"));
    const QString acquiredPath = temporary.filePath(
        QStringLiteral("credential-lock-acquired"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));

    {
        QSettings settings(
            privateSettingsPath, QSettings::IniFormat);
        settings.setValue(QStringLiteral("seed"), true);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        auto state = std::make_shared<FakeStoreState>();
        CredentialSettings credentials(fakeStore(state));
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plainKey(GC_STRAVA_TOKEN), missing),
                 QVariant(missing));
    }

    const QString lockPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".lock"));
    QVERIFY(QFileInfo::exists(
        QFileInfo(lockPath).absolutePath()));
    QLockFile owner(lockPath);
    owner.setStaleLockTime(0);
    QVERIFY(owner.tryLock(0));

    const QString backendLockPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".backend.lock"));
    std::unique_ptr<QLockFile> backendLease;
    if (backendQuarantined) {
        backendLease = std::make_unique<QLockFile>(
            backendLockPath);
        backendLease->setStaleLockTime(0);
        QVERIFY(backendLease->tryLock(0));
        QVERIFY(CredentialSettingsDetail::
            createBackendMutationMarker(backendLockPath));
    }

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_READ_LOCK_SETTINGS"),
        privateSettingsPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_READ_LOCK_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_OPERATION_LOCK_ID"),
        vaultKey);
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_OPERATION_LOCK_CONTENDED"),
        contentionPath);
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_OPERATION_LOCK_ACQUIRED"),
        acquiredPath);
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "contendedCredentialReadWaitsForOwner:")
            + QString::fromLatin1(QTest::currentDataTag()),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const bool contended =
        waitForFile(contentionPath, 5000, &child);
    QByteArray childOutput = child.readAll();
    QVERIFY2(contended, childOutput.constData());

    bool acquired = false;
    if (releaseOwner) {
        QCOMPARE(child.state(), QProcess::Running);
        owner.unlock();
        acquired = waitForFile(
            acquiredPath, 5000, &child);
    }
    QVERIFY2(child.waitForFinished(15000),
             qPrintable(child.errorString()));
    if (owner.isLocked())
        owner.unlock();
    childOutput += child.readAll();
    QCOMPARE(acquired, releaseOwner);
    QCOMPARE(QFileInfo::exists(acquiredPath), releaseOwner);
    QVERIFY2(child.exitStatus() == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());

    if (backendQuarantined) {
        QCOMPARE(
            CredentialSettingsDetail::
                backendMutationMarkerStatus(backendLockPath),
            CredentialSettingsDetail::
                BackendMutationMarkerStatus::Pending);
        QVERIFY(CredentialSettingsDetail::
            removeBackendMutationMarker(backendLockPath));
        backendLease->unlock();

        QSettings settings(
            privateSettingsPath, QSettings::IniFormat);
        auto state = std::make_shared<FakeStoreState>();
        state->values.insert(
            vaultKey, QStringLiteral("stored-secret"));
        CredentialSettings credentials(fakeStore(state));
        bool authoritativeMiss = true;
        bool confirmedVaultValue = false;
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plainKey(GC_STRAVA_TOKEN),
                     QStringLiteral("missing"),
                     CredentialSettings::ReadPolicy::
                         RequireLiveVault,
                     &authoritativeMiss,
                     &confirmedVaultValue),
                 QVariant(QStringLiteral("stored-secret")));
        QVERIFY(!authoritativeMiss);
        QVERIFY(confirmedVaultValue);
        QCOMPARE(state->reads, 1);
    }
}

void TestCredentialSettings::
activeKeychainMutationLeaseBlocksCredentialState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString vaultKey =
        CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
    const QString lockPath = credentialOperationFile(
        stateRoot, vaultKey,
        QStringLiteral(".backend.lock"));
    const QString lockDirectory =
        QFileInfo(lockPath).absolutePath();
    QVERIFY(QDir().mkpath(lockDirectory));
#ifdef Q_OS_UNIX
    QVERIFY(QFile::setPermissions(
        QDir(stateRoot).filePath(
            QStringLiteral("GoldenCheetah")),
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(
        lockDirectory,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
#endif

    QLockFile backendLease(lockPath);
    backendLease.setStaleLockTime(0);
    QVERIFY(backendLease.tryLock(0));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("existing-secret"));
    CredentialSettings credentials(fakeStore(state));
    QElapsedTimer rejectionTimer;
    rejectionTimer.start();
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN),
                 QStringLiteral("blocked")),
             QVariant(QStringLiteral("blocked")));
    QVERIFY(rejectionTimer.elapsed() < 2000);
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("blocked-secret")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);

    backendLease.unlock();
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("persisted-secret")));
    QCOMPARE(state->writes, 1);
}

void TestCredentialSettings::
activeCredentialProcessLockDoesNotExpire()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString readyPath =
        temporary.filePath(QStringLiteral("child-ready"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    const QString runtime =
        temporary.filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkpath(stateRoot));
    QVERIFY(QDir().mkpath(runtime));
    QVERIFY(QFile::setPermissions(
        runtime,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    ScopedEnvironmentVariable runtimeEnvironment(
        QByteArrayLiteral("XDG_RUNTIME_DIR"),
        QFile::encodeName(runtime));

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_CHILD"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_READY"),
        readyPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_LOCK_HOLD_MS"),
        QStringLiteral("1500"));
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("XDG_RUNTIME_DIR"),
        runtime);
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral("credentialProcessLockIsExclusive"),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const bool childReady =
        waitForFile(readyPath, 5000, &child);
    QByteArray childOutput = child.readAll();
    QVERIFY2(childReady, childOutput.constData());

    const QString operationId = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString runtimeLock = credentialOperationFile(
        runtime, operationId, QStringLiteral(".lock"));
    const QString stateLock = credentialOperationFile(
        stateRoot, operationId, QStringLiteral(".lock"));
    const QString lockPath = QFileInfo::exists(stateLock)
        ? stateLock : runtimeLock;
    QVERIFY(QFileInfo::exists(lockPath));
    QFile lockFile(lockPath);
    QVERIFY(lockFile.open(QIODevice::ReadWrite));
    QVERIFY(lockFile.setFileTime(
        QDateTime::currentDateTimeUtc().addSecs(-60),
        QFileDevice::FileModificationTime));
    lockFile.close();

    QSettings settings(settingsPath, QSettings::IniFormat);
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("parent-secret")));
    QCOMPARE(state->writes, 0);

    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    childOutput += child.readAll();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());
}

void TestCredentialSettings::scopeCreationIsSerialized()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    static const QSettings::Format format =
        QSettings::registerFormat(
            QStringLiteral("gc-slow-settings"),
            readPersistentSettings,
            [](QIODevice &device,
               const QSettings::SettingsMap &settings) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
                return writePersistentSettings(
                    device, settings);
            });
    QVERIFY(format != QSettings::InvalidFormat);
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-slow-settings"));
    const QString storageKey =
        QStringLiteral("credential_store/id");
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    QString firstScope;
    QString secondScope;

    std::thread first([&] {
        QSettings settings(path, format);
        ready.fetch_add(1);
        while (!start.load())
            std::this_thread::yield();
        firstScope = CredentialSettings::ensureScopeId(
            &settings, storageKey);
    });
    std::thread second([&] {
        QSettings settings(path, format);
        ready.fetch_add(1);
        while (!start.load())
            std::this_thread::yield();
        secondScope = CredentialSettings::ensureScopeId(
            &settings, storageKey);
    });

    while (ready.load() != 2)
        std::this_thread::yield();
    start.store(true);
    first.join();
    second.join();

    QVERIFY(!QUuid(firstScope).isNull());
    QCOMPARE(secondScope, firstScope);

    QSettings persisted(path, format);
    QCOMPARE(CredentialSettings::ensureScopeId(
                 &persisted, storageKey),
             firstScope);
}

void TestCredentialSettings::
scopeProcessLockCanonicalizesAliases()
{
#ifndef Q_OS_UNIX
    QSKIP("Directory symlink alias coverage requires Unix semantics");
#else
    if (qEnvironmentVariableIsSet("GC_SCOPE_LOCK_CHILD")) {
        const QString settingsPath = qEnvironmentVariable(
            "GC_SCOPE_LOCK_SETTINGS");
        QVERIFY(!settingsPath.isEmpty());
        QSettings settings(
            settingsPath, scopeLockTestFormat());
        const QString scope = CredentialSettings::ensureScopeId(
            &settings,
            QStringLiteral("credential_store/id"));
        QVERIFY(!scope.isEmpty());
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString realDirectory =
        temporary.filePath(QStringLiteral("real"));
    const QString aliasDirectory =
        temporary.filePath(QStringLiteral("alias"));
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    const QString runtime =
        temporary.filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkpath(realDirectory));
    QVERIFY(QDir().mkpath(stateRoot));
    QVERIFY(QDir().mkpath(runtime));
    QVERIFY(QFile::link(realDirectory, aliasDirectory));
    QVERIFY(QFile::setPermissions(
        runtime,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    const QString settingsPath = QDir(realDirectory).filePath(
        QStringLiteral("private.gc-scope-lock"));
    const QString aliasPath = QDir(aliasDirectory).filePath(
        QStringLiteral("private.gc-scope-lock"));
    {
        QSettings seed(settingsPath, scopeLockTestFormat());
        seed.setValue(QStringLiteral("seed"), true);
        seed.sync();
        QCOMPARE(seed.status(), QSettings::NoError);
    }
    QCOMPARE(QFileInfo(aliasPath).canonicalFilePath(),
             QFileInfo(settingsPath).canonicalFilePath());

    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    ScopedEnvironmentVariable runtimeEnvironment(
        QByteArrayLiteral("XDG_RUNTIME_DIR"),
        QFile::encodeName(runtime));
    const QString readyPath =
        temporary.filePath(QStringLiteral("scope-child-ready"));
    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_SCOPE_LOCK_CHILD"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GC_SCOPE_LOCK_SETTINGS"),
        aliasPath);
    environment.insert(
        QStringLiteral("GC_SCOPE_LOCK_READY"),
        readyPath);
    environment.insert(
        QStringLiteral("GC_SCOPE_LOCK_HOLD_MS"),
        QStringLiteral("1200"));
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("XDG_RUNTIME_DIR"),
        runtime);
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral("scopeProcessLockCanonicalizesAliases"),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const bool childReady =
        waitForFile(readyPath, 5000, &child);
    QByteArray childOutput = child.readAll();
    QVERIFY2(childReady, childOutput.constData());

    QSettings competing(
        settingsPath, scopeLockTestFormat());
    QElapsedTimer rejectionTimer;
    rejectionTimer.start();
    const QString competingScope =
        CredentialSettings::ensureScopeId(
            &competing,
            QStringLiteral("credential_store/id"));
    QVERIFY(competingScope.isEmpty());
    QVERIFY(rejectionTimer.elapsed() < 500);

    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    childOutput += child.readAll();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());

    QSettings persisted(
        settingsPath, scopeLockTestFormat());
    const QString durableScope = persisted.value(
        QStringLiteral("credential_store/id")).toString();
    QVERIFY(!QUuid(durableScope).isNull());
    QCOMPARE(CredentialSettings::ensureScopeId(
                 &persisted,
                 QStringLiteral("credential_store/id")),
             durableScope);
#endif
}

void TestCredentialSettings::
identityIdsAreStableAndMalformedValuesFailClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString storageKey =
        QStringLiteral("credential_store/root_id");
    QSettings valid(
        temporary.filePath(QStringLiteral("valid.ini")),
        QSettings::IniFormat);
    const QString first =
        CredentialSettings::ensureIdentityId(
            &valid, storageKey);
    QVERIFY(!QUuid(first).isNull());
    QCOMPARE(
        CredentialSettings::ensureIdentityId(
            &valid, storageKey),
        first);
    const QString bindingKey =
        QStringLiteral("credential_store/binding_v2");
    const CredentialSettings::ScopeBindingResult binding =
        CredentialSettings::ensureScopeBinding(
            &valid, first, bindingKey,
            QStringLiteral("credential_store/id"));
    QVERIFY(binding.succeeded());
    valid.remove(storageKey);
    valid.sync();
    QCOMPARE(valid.status(), QSettings::NoError);
    QCOMPARE(
        CredentialSettings::ensureIdentityId(
            &valid, storageKey, bindingKey),
        first);
    QCOMPARE(valid.value(storageKey).toString(),
             first);

    QSettings malformed(
        temporary.filePath(QStringLiteral("malformed.ini")),
        QSettings::IniFormat);
    malformed.setValue(
        storageKey, QStringLiteral("../invalid"));
    malformed.sync();
    QCOMPARE(malformed.status(), QSettings::NoError);
    QVERIFY(CredentialSettings::ensureIdentityId(
                &malformed, storageKey).isEmpty());
    QCOMPARE(malformed.value(storageKey).toString(),
             QStringLiteral("../invalid"));
}

void TestCredentialSettings::
scopeBindingsAreStableAndRootBound()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString bindingKey =
        QStringLiteral("credential_store/binding_v2");
    const QString scopeKey =
        QStringLiteral("credential_store/id");
    const QString firstRoot =
        QStringLiteral("77777777-7777-4777-8777-777777777777");
    const QString secondRoot =
        QStringLiteral("88888888-8888-4888-8888-888888888888");
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);

    const CredentialSettings::ScopeBindingResult first =
        CredentialSettings::ensureScopeBinding(
            &settings, firstRoot, bindingKey, scopeKey);
    QVERIFY(first.succeeded());
    QVERIFY(!QUuid(first.profileId).isNull());
    QVERIFY(!QUuid(first.scopeId).isNull());

    const CredentialSettings::ScopeBindingResult stable =
        CredentialSettings::ensureScopeBinding(
            &settings, firstRoot, bindingKey, scopeKey);
    QVERIFY(stable.succeeded());
    QCOMPARE(stable.profileId, first.profileId);
    QCOMPARE(stable.scopeId, first.scopeId);

    {
        QSettings incomplete(path, QSettings::IniFormat);
        incomplete.remove(scopeKey);
        incomplete.sync();
        QCOMPARE(incomplete.status(), QSettings::NoError);
    }
    const CredentialSettings::ScopeBindingResult recovered =
        CredentialSettings::ensureScopeBinding(
            &settings, firstRoot, bindingKey, scopeKey);
    QVERIFY(recovered.succeeded());
    QCOMPARE(recovered.profileId, first.profileId);
    QCOMPARE(recovered.scopeId, first.scopeId);
    QSettings persisted(path, QSettings::IniFormat);
    QCOMPARE(persisted.value(scopeKey).toString(),
             first.scopeId);

    const CredentialSettings::ScopeBindingResult conflict =
        CredentialSettings::ensureScopeBinding(
            &settings, secondRoot, bindingKey, scopeKey);
    QCOMPARE(
        conflict.status,
        CredentialSettings::ScopeBindingStatus::Conflict);
    QVERIFY(conflict.scopeId.isEmpty());
}

void TestCredentialSettings::
scopeBindingsRejectUnconfirmedLegacyScopes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString bindingKey =
        QStringLiteral("credential_store/binding_v2");
    const QString scopeKey =
        QStringLiteral("credential_store/id");
    const QString root =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    const QString legacyScope =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    settings.setValue(scopeKey, legacyScope);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const CredentialSettings::ScopeBindingResult rejected =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey);
    QCOMPARE(
        rejected.status,
        CredentialSettings::ScopeBindingStatus::Conflict);
    QVERIFY(rejected.profileId.isEmpty());
    QVERIFY(rejected.scopeId.isEmpty());
    QVERIFY(!settings.contains(bindingKey));
    QCOMPARE(settings.value(scopeKey).toString(), legacyScope);
}

void TestCredentialSettings::
persistedLegacyBindingsRequireAuthorization()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString bindingKey =
        QStringLiteral("credential_store/binding_v2");
    const QString scopeKey =
        QStringLiteral("credential_store/id");
    const QString root =
        QStringLiteral("91919191-9191-4191-8191-919191919191");
    const QString legacyProfile =
        QStringLiteral("92929292-9292-4292-8292-929292929292");
    const QString legacyScope =
        QStringLiteral("93939393-9393-4393-8393-939393939393");
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    settings.setValue(scopeKey, legacyScope);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const CredentialSettings::ScopeBindingResult created =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey,
            legacyScope, legacyProfile);
    QVERIFY(created.succeeded());
    QVERIFY(created.legacyLocalScope);
    QCOMPARE(created.profileId, legacyProfile);
    QCOMPARE(created.scopeId, legacyScope);

    const CredentialSettings::ScopeBindingResult missing =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey);
    QCOMPARE(
        missing.status,
        CredentialSettings::ScopeBindingStatus::Conflict);

    const CredentialSettings::ScopeBindingResult wrongScope =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey,
            QStringLiteral(
                "94949494-9494-4494-8494-949494949494"),
            legacyProfile);
    QCOMPARE(
        wrongScope.status,
        CredentialSettings::ScopeBindingStatus::Conflict);

    const CredentialSettings::ScopeBindingResult wrongProfile =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey,
            legacyScope,
            QStringLiteral(
                "95959595-9595-4595-8595-959595959595"));
    QCOMPARE(
        wrongProfile.status,
        CredentialSettings::ScopeBindingStatus::Conflict);

    const CredentialSettings::ScopeBindingResult confirmed =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey,
            legacyScope, legacyProfile);
    QVERIFY(confirmed.succeeded());
    QVERIFY(confirmed.legacyLocalScope);
    QCOMPARE(confirmed.profileId, legacyProfile);
    QCOMPARE(confirmed.scopeId, legacyScope);
}

void TestCredentialSettings::
malformedScopeBindingsFailClosed_data()
{
    QTest::addColumn<bool>("bindingPresent");
    QTest::addColumn<QString>("binding");
    QTest::addColumn<bool>("scopePresent");
    QTest::addColumn<QString>("scope");

    const QString root =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString otherRoot =
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const QString profile =
        QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    const QString firstScope =
        QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const QString secondScope =
        QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff");
    const auto bindingFor = [&](
        const QString &bindingRoot,
        const QString &bindingScope) {
        return QStringLiteral(
            "{\"origin\":\"fresh\",\"profile_id\":\"%1\","
            "\"root_id\":\"%2\",\"scope_id\":\"%3\","
            "\"version\":2}")
            .arg(profile, bindingRoot, bindingScope);
    };

    QTest::newRow("malformed-binding")
        << true << QStringLiteral("{}")
        << true << firstScope;
    QTest::newRow("malformed-scope")
        << false << QString()
        << true << QStringLiteral("../invalid");
    QTest::newRow("conflicting-scope")
        << true << bindingFor(root, firstScope)
        << true << secondScope;
    QTest::newRow("different-root")
        << true << bindingFor(otherRoot, firstScope)
        << true << firstScope;
}

void TestCredentialSettings::
malformedScopeBindingsFailClosed()
{
    QFETCH(bool, bindingPresent);
    QFETCH(QString, binding);
    QFETCH(bool, scopePresent);
    QFETCH(QString, scope);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString bindingKey =
        QStringLiteral("credential_store/binding_v2");
    const QString scopeKey =
        QStringLiteral("credential_store/id");
    const QString root =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    if (bindingPresent)
        settings.setValue(bindingKey, binding);
    if (scopePresent)
        settings.setValue(scopeKey, scope);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const CredentialSettings::ScopeBindingResult result =
        CredentialSettings::ensureScopeBinding(
            &settings, root, bindingKey, scopeKey);
    QCOMPARE(
        result.status,
        CredentialSettings::ScopeBindingStatus::Conflict);
    QCOMPARE(settings.contains(bindingKey),
             bindingPresent);
    QCOMPARE(settings.contains(scopeKey),
             scopePresent);
    if (bindingPresent)
        QCOMPARE(settings.value(bindingKey).toString(),
                 binding);
    if (scopePresent)
        QCOMPARE(settings.value(scopeKey).toString(),
                 scope);
}

void TestCredentialSettings::
locationClaimsRejectCopiesAndUnconfirmedMoves()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString original =
        temporary.filePath(QStringLiteral("original"));
    const QString copied =
        temporary.filePath(QStringLiteral("copied"));
    const QString moved =
        temporary.filePath(QStringLiteral("moved"));
    QVERIFY(QDir().mkpath(original));
    QVERIFY(QDir().mkpath(copied));
    const QString identity =
        QStringLiteral("13131313-1313-4313-8313-131313131313");
    const QString claimKey =
        QStringLiteral(
            "credential_store/location_claims/test");
    QSettings registry(
        temporary.filePath(QStringLiteral("registry.ini")),
        QSettings::IniFormat);

    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &registry, claimKey, identity,
            QString(), original),
        CredentialSettings::LocationClaimStatus::Success);
    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &registry, claimKey, identity,
            QString(), copied),
        CredentialSettings::LocationClaimStatus::Conflict);

    QVERIFY(QDir().rename(original, moved));
    QVERIFY(!QFileInfo::exists(original));
    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &registry, claimKey, identity,
            QString(), moved),
        CredentialSettings::LocationClaimStatus::Conflict);
    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &registry, claimKey, identity,
            QString(), copied),
        CredentialSettings::LocationClaimStatus::Conflict);
    QVERIFY(QDir().mkpath(original));
    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &registry, claimKey, identity,
            QString(), original),
        CredentialSettings::LocationClaimStatus::Success);
}

void TestCredentialSettings::
pendingLocationEnrollmentRejectsCompetingParent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(directory));
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    const QString firstParent =
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111");
    const QString secondParent =
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222");

    const CredentialSettings::LocationEnrollmentResult
        first =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority,
                    QByteArrayLiteral("scope"),
                    QString(), firstParent,
                    directory, true);
    QVERIFY(first.succeeded());
    QVERIFY(first.pending);
    authority.sync();
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));

    const CredentialSettings::LocationEnrollmentResult
        competing =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority,
                    QByteArrayLiteral("scope"),
                    QString(), secondParent,
                    directory, true);
    QCOMPARE(
        competing.status,
        CredentialSettings::
            LocationClaimStatus::Conflict);
    QVERIFY(competing.identityId.isEmpty());
    QVERIFY(!competing.pending);
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
}

void TestCredentialSettings::
canonicalLocationClaimRejectsCompetingParent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(directory));
    const QString canonicalDirectory =
        QFileInfo(directory).canonicalFilePath();
    QVERIFY(!canonicalDirectory.isEmpty());
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    const QByteArray kind = QByteArrayLiteral("scope");
    const QString firstParent =
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111");
    const QString secondParent =
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222");
    const QString identity =
        QStringLiteral(
            "33333333-3333-4333-8333-333333333333");
    authority.setValue(
        testLocationClaimKey(kind, identity),
        testLocationClaimPayload(
            identity, firstParent, canonicalDirectory));
    authority.sync();
    QCOMPARE(authority.status(), QSettings::NoError);
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));

    const CredentialSettings::LocationEnrollmentResult
        competing =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority, kind, QString(),
                    secondParent, directory, true);
    QCOMPARE(
        competing.status,
        CredentialSettings::
            LocationClaimStatus::Conflict);
    QVERIFY(competing.identityId.isEmpty());
    QVERIFY(!competing.pending);
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
}

void TestCredentialSettings::
misplacedLocationClaimCannotAuthorizeBackfill()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(directory));
    const QString canonicalDirectory =
        QFileInfo(directory).canonicalFilePath();
    QVERIFY(!canonicalDirectory.isEmpty());
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    const QString parent =
        QStringLiteral(
            "33333333-3333-4333-8333-333333333333");
    const QString identity =
        QStringLiteral(
            "44444444-4444-4444-8444-444444444444");
    const QString wrongClaimKey =
        QStringLiteral(
            "credential_store/location_claims/")
        + QString(64, QLatin1Char('0'));
    QVERIFY(
        wrongClaimKey
        != testLocationClaimKey(
            QByteArrayLiteral("scope"), identity));
    authority.setValue(
        wrongClaimKey,
        testLocationClaimPayload(
            identity, parent, canonicalDirectory));
    authority.sync();
    QCOMPARE(
        authority.status(), QSettings::NoError);
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));

    const CredentialSettings::LocationEnrollmentResult
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority,
                    QByteArrayLiteral("scope"),
                    identity, parent,
                    directory, false);
    QCOMPARE(
        enrollment.status,
        CredentialSettings::
            LocationClaimStatus::Conflict);
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
    QVERIFY(authority.allKeys()
                .filter(QStringLiteral(
                    "credential_store/"
                    "location_bindings/"))
                .isEmpty());
}

void TestCredentialSettings::
malformedLocationAuthorityRecordsFailClosed_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("malformed-enrollment")
        << QStringLiteral("malformed-enrollment");
    QTest::newRow("conflicting-enrollment-kind")
        << QStringLiteral(
               "conflicting-enrollment-kind");
    QTest::newRow("conflicting-enrollment-identity")
        << QStringLiteral(
               "conflicting-enrollment-identity");
    QTest::newRow("malformed-binding")
        << QStringLiteral("malformed-binding");
    QTest::newRow("conflicting-binding-parent")
        << QStringLiteral(
               "conflicting-binding-parent");
    QTest::newRow("malformed-canonical-claim")
        << QStringLiteral(
               "malformed-canonical-claim");
}

void TestCredentialSettings::
malformedLocationAuthorityRecordsFailClosed()
{
    QFETCH(QString, mode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(directory));
    const QString canonicalDirectory =
        QFileInfo(directory).canonicalFilePath();
    QVERIFY(!canonicalDirectory.isEmpty());
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    const QByteArray kind = QByteArrayLiteral("scope");
    const QString parent =
        QStringLiteral(
            "99999999-9999-4999-8999-999999999999");
    const QString otherParent =
        QStringLiteral(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString identity =
        QStringLiteral(
            "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString otherIdentity =
        QStringLiteral(
            "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    QString key;
    QString payload;
    if (mode.contains(QStringLiteral("enrollment"))) {
        key = testLocationEnrollmentKey(
            kind, parent, canonicalDirectory);
        if (mode
                == QStringLiteral(
                    "malformed-enrollment")) {
            payload = QStringLiteral("{}");
        } else if (mode
                       == QStringLiteral(
                           "conflicting-enrollment-kind")) {
            payload = testLocationRecordPayload(
                QByteArrayLiteral("profile"),
                identity, parent,
                canonicalDirectory);
        } else {
            payload = testLocationRecordPayload(
                kind, otherIdentity, parent,
                canonicalDirectory);
        }
    } else if (mode.contains(
                   QStringLiteral("binding"))) {
        key = testLocationBindingKey(
            kind, canonicalDirectory);
        payload =
            mode == QStringLiteral(
                        "malformed-binding")
                ? QStringLiteral("{}")
                : testLocationRecordPayload(
                      kind, identity, otherParent,
                      canonicalDirectory);
    } else {
        key = testLocationClaimKey(kind, identity);
        payload = QStringLiteral("{}");
    }
    authority.setValue(key, payload);
    authority.sync();
    QCOMPARE(
        authority.status(), QSettings::NoError);
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));

    const CredentialSettings::LocationEnrollmentResult
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority, kind, identity,
                    parent, directory, false);
    QCOMPARE(
        enrollment.status,
        CredentialSettings::
            LocationClaimStatus::Conflict);
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
}

void TestCredentialSettings::
locationEnrollmentCompletionRequiresLocalMetadata()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(directory));
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    QSettings local(
        temporary.filePath(QStringLiteral("local.ini")),
        QSettings::IniFormat);
    const QString identityKey =
        QStringLiteral("credential_store/root_id");

    const CredentialSettings::LocationEnrollmentResult
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority,
                    QByteArrayLiteral("root"),
                    QString(), QString(),
                    directory, true);
    QVERIFY(enrollment.succeeded());
    QVERIFY(enrollment.pending);
    authority.sync();
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));
    QVERIFY(!CredentialSettings::
        completeLocationEnrollment(
            &authority,
            QByteArrayLiteral("root"),
            enrollment.identityId,
            QString(), directory,
            &local, identityKey,
            QString(), QString(),
            QString(), QString(), QString()));
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
    QVERIFY(!authority.allKeys()
                 .filter(QStringLiteral(
                     "credential_store/"
                     "location_enrollments/"))
                 .isEmpty());

    local.setValue(
        identityKey,
        QStringLiteral(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    local.sync();
    QVERIFY(!CredentialSettings::
        completeLocationEnrollment(
            &authority,
            QByteArrayLiteral("root"),
            enrollment.identityId,
            QString(), directory,
            &local, identityKey,
            QString(), QString(),
            QString(), QString(), QString()));
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);

    local.setValue(identityKey, enrollment.identityId);
    local.sync();
    QVERIFY(CredentialSettings::
        completeLocationEnrollment(
            &authority,
            QByteArrayLiteral("root"),
            enrollment.identityId,
            QString(), directory,
            &local, identityKey,
            QString(), QString(),
            QString(), QString(), QString()));
    authority.sync();
    QVERIFY(authority.allKeys()
                .filter(QStringLiteral(
                    "credential_store/"
                    "location_enrollments/"))
                .isEmpty());
    QCOMPARE(
        authority.allKeys()
            .filter(QStringLiteral(
                "credential_store/location_bindings/"))
            .size(),
        1);
}

void TestCredentialSettings::
locationEnrollmentCompletionRequiresFullLocalBinding_data()
{
    QTest::addColumn<QByteArray>("kind");
    QTest::addColumn<QString>("tamperMode");
    QTest::newRow("profile-scope-changed")
        << QByteArrayLiteral("profile")
        << QStringLiteral("scope-changed");
    QTest::newRow("profile-binding-missing")
        << QByteArrayLiteral("profile")
        << QStringLiteral("binding-missing");
    QTest::newRow("profile-scope-missing")
        << QByteArrayLiteral("profile")
        << QStringLiteral("scope-missing");
    QTest::newRow("profile-scope-mismatch")
        << QByteArrayLiteral("profile")
        << QStringLiteral("scope-mismatch");
    QTest::newRow("scope-root-changed")
        << QByteArrayLiteral("scope")
        << QStringLiteral("root-changed");
    QTest::newRow("scope-binding-missing")
        << QByteArrayLiteral("scope")
        << QStringLiteral("binding-missing");
    QTest::newRow("scope-scope-missing")
        << QByteArrayLiteral("scope")
        << QStringLiteral("scope-missing");
    QTest::newRow("scope-scope-mismatch")
        << QByteArrayLiteral("scope")
        << QStringLiteral("scope-mismatch");
}

void TestCredentialSettings::
locationEnrollmentCompletionRequiresFullLocalBinding()
{
    QFETCH(QByteArray, kind);
    QFETCH(QString, tamperMode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        temporary.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(directory));
    QSettings authority(
        temporary.filePath(
            QStringLiteral("authority.ini")),
        QSettings::IniFormat);
    QSettings local(
        temporary.filePath(QStringLiteral("local.ini")),
        QSettings::IniFormat);
    const QString rootId =
        QStringLiteral(
            "55555555-5555-4555-8555-555555555555");
    const QString fixedProfileId =
        QStringLiteral(
            "66666666-6666-4666-8666-666666666666");
    const QString fixedScopeId =
        QStringLiteral(
            "77777777-7777-4777-8777-777777777777");
    const QString changedId =
        QStringLiteral(
            "88888888-8888-4888-8888-888888888888");
    const QString parentId =
        kind == QByteArrayLiteral("profile")
            ? rootId : fixedProfileId;
    const CredentialSettings::LocationEnrollmentResult
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    &authority, kind,
                    QString(), parentId,
                    directory, true);
    QVERIFY(enrollment.succeeded());
    QVERIFY(enrollment.pending);

    const QString profileId =
        kind == QByteArrayLiteral("profile")
            ? enrollment.identityId
            : fixedProfileId;
    const QString scopeId =
        kind == QByteArrayLiteral("scope")
            ? enrollment.identityId
            : fixedScopeId;
    const QString localRootId =
        tamperMode == QStringLiteral("root-changed")
            ? changedId : rootId;
    const QString localScopeId =
        tamperMode == QStringLiteral("scope-changed")
            || tamperMode
                == QStringLiteral("scope-mismatch")
            ? changedId : scopeId;
    const QString bindingKey =
        QStringLiteral(
            "credential_store/binding_v2");
    const QString scopeKey =
        QStringLiteral("credential_store/id");
    if (tamperMode
            != QStringLiteral("binding-missing")) {
        local.setValue(
            bindingKey,
            testScopeBindingPayload(
                localRootId, profileId,
                tamperMode
                        == QStringLiteral(
                            "scope-mismatch")
                    ? scopeId : localScopeId));
    }
    if (tamperMode
            != QStringLiteral("scope-missing")) {
        local.setValue(scopeKey, localScopeId);
    }
    local.sync();
    QCOMPARE(local.status(), QSettings::NoError);
    authority.sync();
    const QByteArray authorityBefore =
        fileContents(authority.fileName());
    const QHash<QString, QVariant> valuesBefore =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/"));

    QVERIFY(!CredentialSettings::
        completeLocationEnrollment(
            &authority, kind,
            enrollment.identityId,
            parentId, directory,
            &local, QString(),
            bindingKey, scopeKey,
            rootId, profileId, scopeId));
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral("credential_store/")),
        valuesBefore);
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
}

void TestCredentialSettings::
persistedCachesTrackOtherInstances_data()
{
    QTest::addColumn<bool>("initiallyPresent");
    QTest::newRow("positive-cache") << true;
    QTest::newRow("negative-cache") << false;
}

void TestCredentialSettings::
persistedCachesTrackOtherInstances()
{
    QFETCH(bool, initiallyPresent);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");

    auto state = std::make_shared<FakeStoreState>();
    if (initiallyPresent) {
        state->values.insert(vaultKey, oldSecret);
    }
    CredentialSettings first(fakeStore(state));
    CredentialSettings second(fakeStore(state));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(initiallyPresent
                          ? oldSecret
                          : QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);

    QVERIFY(second.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(newSecret));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
externalVaultMutationExpiresPersistedCache_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::addColumn<bool>("clockRollback");
    QTest::newRow("replacement")
        << QStringLiteral("replace") << false;
    QTest::newRow("deletion")
        << QStringLiteral("delete") << false;
    QTest::newRow("insertion-after-miss")
        << QStringLiteral("insert") << false;
    QTest::newRow("clock-rollback")
        << QStringLiteral("replace") << true;
}

void TestCredentialSettings::
externalVaultMutationExpiresPersistedCache()
{
    QFETCH(QString, mutation);
    QFETCH(bool, clockRollback);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    const bool initiallyPresent =
        mutation != QStringLiteral("insert");
    const QVariant initialValue =
        initiallyPresent ? QVariant(oldSecret)
                         : QVariant(missing);
    const QVariant finalValue =
        mutation == QStringLiteral("delete")
            ? QVariant(missing) : QVariant(newSecret);
    qint64 nowMs = 1000;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);

    auto state = std::make_shared<FakeStoreState>();
    if (initiallyPresent)
        state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             initialValue);
    QCOMPARE(state->reads, 1);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    const bool revisionPresent =
        QFileInfo::exists(revisionPath);
    const QByteArray revisionBefore =
        fileContents(revisionPath);

    if (mutation == QStringLiteral("delete")) {
        state->values.remove(vaultKey);
    } else {
        state->values.insert(vaultKey, newSecret);
    }
    QCOMPARE(
        QFileInfo::exists(revisionPath),
        revisionPresent);
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    const qint64 lifetimeMs =
        CredentialSettingsDetail::
            credentialCacheLifetimeMsForTest();
    if (clockRollback) {
        --nowMs;
        CredentialSettingsDetail::
            setCredentialCacheNowForTest(nowMs);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, missing),
                 finalValue);
        QCOMPARE(state->reads, 2);
        QCOMPARE(
            QFileInfo::exists(revisionPath),
            revisionPresent);
        QCOMPARE(
            fileContents(revisionPath),
            revisionBefore);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, missing),
                 finalValue);
        QCOMPARE(state->reads, 2);
        return;
    }

    nowMs += lifetimeMs - 1;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             initialValue);
    QCOMPARE(state->reads, 1);
    QCOMPARE(
        QFileInfo::exists(revisionPath),
        revisionPresent);
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    ++nowMs;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             finalValue);
    QCOMPARE(state->reads, 2);
    QCOMPARE(
        QFileInfo::exists(revisionPath),
        revisionPresent);
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             finalValue);
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
activeCredentialCacheObservesExternalMutation_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::newRow("replacement")
        << QStringLiteral("replace");
    QTest::newRow("deletion")
        << QStringLiteral("delete");
}

void TestCredentialSettings::
activeCredentialCacheObservesExternalMutation()
{
    QFETCH(QString, mutation);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    const QVariant finalValue =
        mutation == QStringLiteral("delete")
            ? QVariant(missing) : QVariant(newSecret);
    qint64 nowMs = 1000;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, oldSecret));
    const int readsAfterWrite = state->reads;
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(oldSecret));
    QCOMPARE(state->reads, readsAfterWrite);

    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    QVERIFY(QFileInfo::exists(revisionPath));
    const QByteArray revisionBefore =
        fileContents(revisionPath);
    QVERIFY(!revisionBefore.isEmpty());
    if (mutation == QStringLiteral("delete")) {
        state->values.remove(vaultKey);
    } else {
        state->values.insert(vaultKey, newSecret);
    }
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    nowMs += CredentialSettingsDetail::
        credentialCacheLifetimeMsForTest() - 1;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(oldSecret));
    QCOMPARE(state->reads, readsAfterWrite);
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    ++nowMs;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             finalValue);
    QCOMPARE(state->reads, readsAfterWrite + 1);
    QCOMPARE(fileContents(revisionPath), revisionBefore);

    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             finalValue);
    QCOMPARE(state->reads, readsAfterWrite + 1);
}

void TestCredentialSettings::
expiredPersistedCacheFailsClosedOnVaultError()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    qint64 nowMs = 1000;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(oldSecret));
    QCOMPARE(state->reads, 1);

    state->values.insert(vaultKey, newSecret);
    state->failReads = true;
    nowMs += CredentialSettingsDetail::
        credentialCacheLifetimeMsForTest();
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(missing));
    QCOMPARE(state->reads, 2);

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(newSecret));
    QCOMPARE(state->reads, 3);
}

void TestCredentialSettings::
memoryOnlyCredentialCacheDoesNotExpire_data()
{
    QTest::addColumn<bool>("updating");
    QTest::newRow("creating") << false;
    QTest::newRow("updating") << true;
}

void TestCredentialSettings::
memoryOnlyCredentialCacheDoesNotExpire()
{
    QFETCH(bool, updating);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString memorySecret =
        QStringLiteral("memory-secret");
    qint64 nowMs = 1000;
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    if (updating) {
        QVERIFY(credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("old-secret")));
    }
    state->failWrites = true;
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, memorySecret));
    const int readsAfterWrite = state->reads;
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(memorySecret));

    nowMs += 4 * CredentialSettingsDetail::
        credentialCacheLifetimeMsForTest();
    CredentialSettingsDetail::setCredentialCacheNowForTest(
        nowMs);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(memorySecret));
    QCOMPARE(state->reads, readsAfterWrite);

    bool authoritativeMiss = true;
    bool confirmedVaultValue = true;
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing,
                 CredentialSettings::ReadPolicy::RequireLiveVault,
                 &authoritativeMiss,
                 &confirmedVaultValue),
             QVariant(memorySecret));
    QVERIFY(!authoritativeMiss);
    QVERIFY(!confirmedVaultValue);
    QCOMPARE(state->reads, readsAfterWrite);
}

void TestCredentialSettings::
authoritativeReadObservesExternalVaultMutation_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::newRow("replacement")
        << QStringLiteral("replace");
    QTest::newRow("deletion")
        << QStringLiteral("delete");
    QTest::newRow("insertion-after-miss")
        << QStringLiteral("insert");
    QTest::newRow("backend-unavailable")
        << QStringLiteral("unavailable");
}

void TestCredentialSettings::
authoritativeReadObservesExternalVaultMutation()
{
    QFETCH(QString, mutation);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString missing = QStringLiteral("missing");
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    const bool initiallyPresent =
        mutation != QStringLiteral("insert");

    auto state = std::make_shared<FakeStoreState>();
    if (initiallyPresent)
        state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing),
             QVariant(initiallyPresent ? oldSecret : missing));
    QCOMPARE(state->reads, 1);

    if (mutation == QStringLiteral("delete")) {
        state->values.remove(vaultKey);
    } else if (mutation == QStringLiteral("unavailable")) {
        state->failReads = true;
    } else {
        state->values.insert(vaultKey, newSecret);
    }

    bool authoritativeMiss = false;
    bool confirmedVaultValue = false;
    const QVariant expected =
        mutation == QStringLiteral("delete")
            || mutation == QStringLiteral("unavailable")
            ? QVariant(missing) : QVariant(newSecret);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, missing,
                 CredentialSettings::ReadPolicy::RequireLiveVault,
                 &authoritativeMiss,
                 &confirmedVaultValue),
             expected);
    QCOMPARE(
        authoritativeMiss,
        mutation == QStringLiteral("delete"));
    QCOMPARE(
        confirmedVaultValue,
        mutation != QStringLiteral("delete")
            && mutation != QStringLiteral("unavailable"));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
cachedCredentialTracksOtherInstanceRemoval()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("old-secret"));
    CredentialSettings first(fakeStore(state));
    CredentialSettings second(fakeStore(state));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("old-secret")));

    QVERIFY(second.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
}

void TestCredentialSettings::
memoryOnlyCredentialTracksOtherInstanceReplacement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings first(fakeStore(state));
    CredentialSettings second(fakeStore(state));
    QVERIFY(!first.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("memory-secret")));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("memory-secret")));

    state->failWrites = false;
    QVERIFY(second.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("persisted-secret")));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("persisted-secret")));
}

void TestCredentialSettings::
failedUpdateResolutionInvalidatesMemoryOnlyCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("persisted-credential"));
    state->failWrites = true;
    CredentialSettings writer(fakeStore(state));
    CredentialSettings resolver(fakeStore(state));
    QVERIFY(!writer.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("memory-only-credential")));
    QCOMPARE(writer.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("memory-only-credential")));

    state->failWrites = false;
    QCOMPARE(resolver.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("persisted-credential")));
    QCOMPARE(writer.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("persisted-credential")));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
supersededUpdateInvalidatesMemoryOnlyCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    const QString revisionBackup =
        revisionPath + QStringLiteral(".backup");

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings first(fakeStore(state));
    CredentialSettings second(fakeStore(state));
    QVERIFY(!first.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey,
        QStringLiteral("first-memory-credential")));
    const QByteArray firstTransaction =
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating"));
    QVERIFY(!firstTransaction.isEmpty());
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(
                 QStringLiteral("first-memory-credential")));

    QVERIFY(QFile::rename(
        revisionPath, revisionBackup));
    QVERIFY(QDir().mkdir(revisionPath));
    QVERIFY(!second.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey,
        QStringLiteral("superseding-credential")));
    const QByteArray secondTransaction =
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating"));
    QVERIFY(!secondTransaction.isEmpty());
    QVERIFY(secondTransaction != firstTransaction);
    QVERIFY(QDir(revisionPath).removeRecursively());
    QVERIFY(QFile::rename(
        revisionBackup, revisionPath));

    state->failWrites = false;
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("creating")));
}

void TestCredentialSettings::
credentialCacheTracksProcessMutations()
{
    const bool childMode =
        qEnvironmentVariableIsSet("GC_CREDENTIAL_CACHE_CHILD");
    if (childMode) {
        const QString settingsPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CACHE_SETTINGS");
        const QString vaultPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CACHE_VAULT");
        const QString signalRoot = qEnvironmentVariable(
            "GC_CREDENTIAL_CACHE_SIGNALS");
        const QString scope = qEnvironmentVariable(
            "GC_CREDENTIAL_CACHE_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!vaultPath.isEmpty());
        QVERIFY(!signalRoot.isEmpty());
        QVERIFY(!scope.isEmpty());

        QSettings settings(settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(
            std::make_unique<FileCredentialStore>(vaultPath));
        const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("old-secret")));
        QVERIFY(writeSignalFile(QDir(signalRoot).filePath(
            QStringLiteral("positive-ready"))));
        QVERIFY(waitForFile(QDir(signalRoot).filePath(
                                QStringLiteral("positive-go")),
                            5000));
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("new-secret")));

        QVERIFY(credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
        QVERIFY(writeSignalFile(QDir(signalRoot).filePath(
            QStringLiteral("negative-ready"))));
        QVERIFY(waitForFile(QDir(signalRoot).filePath(
                                QStringLiteral("negative-go")),
                            5000));
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("final-secret")));
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString signalRoot =
        temporary.filePath(QStringLiteral("signals"));
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    const QString runtime =
        temporary.filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkpath(signalRoot));
    QVERIFY(QDir().mkpath(stateRoot));
    QVERIFY(QDir().mkpath(runtime));
    QVERIFY(QFile::setPermissions(
        runtime,
        QFileDevice::ReadOwner
        | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    ScopedEnvironmentVariable runtimeEnvironment(
        QByteArrayLiteral("XDG_RUNTIME_DIR"),
        QFile::encodeName(runtime));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    QString error;
    FileCredentialStore initialVault(vaultPath);
    QCOMPARE(initialVault.write(
                 vaultKey, QStringLiteral("old-secret"), &error),
             CredentialStore::Status::Success);

    QProcess child;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_CACHE_CHILD"),
        QStringLiteral("1"));
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_CACHE_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_CACHE_VAULT"),
        vaultPath);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_CACHE_SIGNALS"),
        signalRoot);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_CACHE_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("XDG_RUNTIME_DIR"),
        runtime);
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral("credentialCacheTracksProcessMutations"),
        QStringLiteral("-silent")
    });
    child.start();
    QVERIFY2(child.waitForStarted(5000),
             qPrintable(child.errorString()));
    const QString positiveReady = QDir(signalRoot).filePath(
        QStringLiteral("positive-ready"));
    bool phaseReady = waitForFile(
        positiveReady, 5000, &child);
    QByteArray childOutput = child.readAll();
    QVERIFY2(phaseReady, childOutput.constData());

    QSettings settings(settingsPath, QSettings::IniFormat);
    CredentialSettings credentials(
        std::make_unique<FileCredentialStore>(vaultPath));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("new-secret")));
    QVERIFY(writeSignalFile(QDir(signalRoot).filePath(
        QStringLiteral("positive-go"))));

    const QString negativeReady = QDir(signalRoot).filePath(
        QStringLiteral("negative-ready"));
    phaseReady = waitForFile(
        negativeReady, 5000, &child);
    childOutput += child.readAll();
    QVERIFY2(phaseReady, childOutput.constData());

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("final-secret")));
    QVERIFY(writeSignalFile(QDir(signalRoot).filePath(
        QStringLiteral("negative-go"))));

    QVERIFY2(child.waitForFinished(10000),
             qPrintable(child.errorString()));
    childOutput += child.readAll();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit,
             childOutput.constData());
    QVERIFY2(child.exitCode() == 0,
             childOutput.constData());
}

void TestCredentialSettings::
missingRevisionInvalidatesCachedCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("old-secret"));
    CredentialSettings first(fakeStore(state));
    CredentialSettings second(fakeStore(state));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("old-secret")));
    QVERIFY(QFileInfo::exists(revisionPath));

    QVERIFY(second.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("new-secret")));
    QVERIFY(QFile::remove(revisionPath));
    QCOMPARE(first.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("new-secret")));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
credentialRevisionFailureBlocksVaultMutation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    const QString secret = QStringLiteral("revision-test-secret");

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(QFile::remove(revisionPath));
    QVERIFY(QDir().mkpath(revisionPath));

    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, secret));
    QCOMPARE(state->writes, 0);
    QVERIFY(!state->values.contains(vaultKey));

    QVERIFY(QDir(revisionPath).removeRecursively());
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, secret));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), secret);
    const QByteArray revision = fileContents(revisionPath);
    QCOMPARE(revision.trimmed().size(), 32);
    QVERIFY(!revision.contains(secret.toUtf8()));
    QVERIFY(!revision.contains(vaultKey.toUtf8()));
    verifyOwnerOnlyPermissions(
        QFileInfo(revisionPath).absolutePath());
    verifyOwnerOnlyPermissions(revisionPath);
}

void TestCredentialSettings::
credentialRevisionFailureBlocksMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    const QString secret = QStringLiteral("legacy-secret");
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(QDir().mkpath(
        QFileInfo(revisionPath).absolutePath()));
    QVERIFY(QDir().mkpath(revisionPath));

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QVERIFY(settings.contains(plaintextKey));
    QVERIFY(!state->values.contains(vaultKey));

    QVERIFY(QDir(revisionPath).removeRecursively());
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), secret);
    QVERIFY(!settings.contains(plaintextKey));
}

void TestCredentialSettings::
credentialRevisionFailureBlocksRemoval()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    QVERIFY(QDir().mkpath(
        QFileInfo(revisionPath).absolutePath()));
    QVERIFY(QDir().mkpath(revisionPath));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-secret"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("vault-secret"));
    QVERIFY(settings.value(removalKey).toBool());

    QVERIFY(QDir(revisionPath).removeRecursively());
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(!settings.contains(removalKey));
}

void TestCredentialSettings::
groupedCredentialCleanupTargetsActiveGroup()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString rootSecret = QStringLiteral("root-secret");
    const QString groupedSecret = QStringLiteral("grouped-secret");
    ini.setValue(plaintextKey, rootSecret);
    ini.beginGroup(QStringLiteral("profile"));
    ini.setValue(plaintextKey, groupedSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(groupedSecret));
    QVERIFY(!ini.contains(plaintextKey));
    QCOMPARE(state->values.value(CredentialSettings::vaultKey(
                 scope, GC_STRAVA_TOKEN)),
             groupedSecret);

    ini.endGroup();
    QCOMPARE(ini.value(plaintextKey).toString(), rootSecret);
}

void TestCredentialSettings::
plaintextCleanupRequiresMatchingVaultCopy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString sourceSecret =
        QStringLiteral("distinct-source-secret");
    const QString vaultSecret =
        QStringLiteral("distinct-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, sourceSecret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, vaultSecret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        sourceSecret);

    state->values.remove(vaultKey);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(sourceSecret));
    QCOMPARE(state->values.value(vaultKey), sourceSecret);
    QVERIFY(!settings.contains(plaintextKey));
}

void TestCredentialSettings::
plaintextCleanupConflictsAreGroupScoped()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings root(path, QSettings::IniFormat);
    const QString group = QStringLiteral("profile");
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString rootSecret =
        QStringLiteral("current-vault-secret");
    const QString groupedSecret =
        QStringLiteral("stale-grouped-secret");
    const QString newerGroupedSecret =
        QStringLiteral("newer-grouped-secret");
    const QString vaultSecret =
        QStringLiteral("current-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    root.setValue(plaintextKey, rootSecret);
    root.beginGroup(group);
    root.setValue(plaintextKey, groupedSecret);
    root.endGroup();
    root.sync();
    QCOMPARE(root.status(), QSettings::NoError);
    const QString rootCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &root, plaintextKey);
    QSettings groupedIdentity(path, QSettings::IniFormat);
    groupedIdentity.beginGroup(group);
    const QString groupedCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &groupedIdentity, plaintextKey);
    QVERIFY(!rootCleanupPath.isEmpty());
    QVERIFY(!groupedCleanupPath.isEmpty());
    QVERIFY(rootCleanupPath != groupedCleanupPath);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, vaultSecret);
    bool replaced = false;
    state->beforeRead = [&] {
        if (replaced || state->reads != 2)
            return;
        replaced = true;
        QSettings external(path, QSettings::IniFormat);
        external.beginGroup(group);
        external.setValue(plaintextKey, newerGroupedSecret);
        external.sync();
        QCOMPARE(external.status(), QSettings::NoError);
    };
    CredentialSettings credentials(fakeStore(state));

    QSettings grouped(path, QSettings::IniFormat);
    grouped.beginGroup(group);
    QCOMPARE(credentials.value(
                 &grouped, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(replaced);
    QCOMPARE(
        grouped.value(plaintextKey).toString(),
        newerGroupedSecret);
    state->beforeRead = {};

    QSettings rootReader(path, QSettings::IniFormat);
    QCOMPARE(credentials.value(
                 &rootReader, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(!rootReader.contains(plaintextKey));

    QSettings retained(path, QSettings::IniFormat);
    retained.beginGroup(group);
    QCOMPARE(
        retained.value(plaintextKey).toString(),
        newerGroupedSecret);

    state->values.remove(vaultKey);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &retained, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(newerGroupedSecret));
    QCOMPARE(
        state->values.value(vaultKey),
        newerGroupedSecret);
    QVERIFY(!retained.contains(plaintextKey));
}

void TestCredentialSettings::
plaintextCleanupConflictsAreSourceScoped()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString firstPath =
        temporary.filePath(QStringLiteral("first.ini"));
    const QString secondPath =
        temporary.filePath(QStringLiteral("second.ini"));
    QSettings first(firstPath, QSettings::IniFormat);
    QSettings second(secondPath, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString originalSecret =
        QStringLiteral("original-source-secret");
    const QString newerSecret =
        QStringLiteral("newer-source-secret");
    const QString vaultSecret =
        QStringLiteral("current-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    first.setValue(plaintextKey, originalSecret);
    first.sync();
    second.setValue(plaintextKey, vaultSecret);
    second.sync();
    QCOMPARE(first.status(), QSettings::NoError);
    QCOMPARE(second.status(), QSettings::NoError);
    const QString firstCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &first, plaintextKey);
    const QString secondCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &second, plaintextKey);
    QVERIFY(!firstCleanupPath.isEmpty());
    QVERIFY(!secondCleanupPath.isEmpty());
    QVERIFY(firstCleanupPath != secondCleanupPath);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, vaultSecret);
    bool replaced = false;
    state->beforeRead = [&] {
        if (replaced || state->reads != 2)
            return;
        replaced = true;
        QSettings external(firstPath, QSettings::IniFormat);
        external.setValue(plaintextKey, newerSecret);
        external.sync();
        QCOMPARE(external.status(), QSettings::NoError);
    };
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &first, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(replaced);
    QCOMPARE(first.value(plaintextKey).toString(), newerSecret);
    state->beforeRead = {};

    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(!second.contains(plaintextKey));
    QCOMPARE(first.value(plaintextKey).toString(), newerSecret);

    state->values.remove(vaultKey);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &first, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(newerSecret));
    QCOMPARE(state->values.value(vaultKey), newerSecret);
    QVERIFY(!first.contains(plaintextKey));
}

void TestCredentialSettings::
plaintextCleanupAliasesShareGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString group = QStringLiteral("profile");
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString absoluteKey =
        group + QLatin1Char('/') + plaintextKey;
    const QString legacySecret =
        QStringLiteral("legacy-grouped-secret");
    const QString newerSecret =
        QStringLiteral("newer-grouped-secret");
    const QString vaultSecret =
        QStringLiteral("current-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.beginGroup(group);
        settings.setValue(plaintextKey, legacySecret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }
    QSettings groupedIdentity(path, QSettings::IniFormat);
    groupedIdentity.beginGroup(group);
    QSettings rootIdentity(path, QSettings::IniFormat);
    const QString groupedCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &groupedIdentity, plaintextKey);
    const QString rootCleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &rootIdentity, absoluteKey);
    QVERIFY(!groupedCleanupPath.isEmpty());
    QCOMPARE(groupedCleanupPath, rootCleanupPath);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, vaultSecret);
    bool replaced = false;
    state->beforeRead = [&] {
        if (replaced || state->reads != 2)
            return;
        replaced = true;
        QSettings external(path, QSettings::IniFormat);
        external.setValue(absoluteKey, newerSecret);
        external.sync();
        QCOMPARE(external.status(), QSettings::NoError);
    };
    CredentialSettings credentials(fakeStore(state));
    {
        QSettings grouped(path, QSettings::IniFormat);
        grouped.beginGroup(group);
        QCOMPARE(credentials.value(
                     &grouped, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(vaultSecret));
    }
    QVERIFY(replaced);
    state->beforeRead = {};

    QSettings root(path, QSettings::IniFormat);
    QCOMPARE(credentials.value(
                 &root, scope, GC_STRAVA_TOKEN,
                 absoluteKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QCOMPARE(root.value(absoluteKey).toString(), newerSecret);

    state->values.remove(vaultKey);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &root, scope, GC_STRAVA_TOKEN,
                 absoluteKey, QStringLiteral("missing")),
             QVariant(newerSecret));
    QCOMPARE(state->values.value(vaultKey), newerSecret);
    QVERIFY(!root.contains(absoluteKey));
}

void TestCredentialSettings::
plaintextCleanupIdentitySeparatesNewlineTuples()
{
#ifdef Q_OS_WIN
    QSKIP("Windows file names cannot contain newlines");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString firstPath =
        temporary.filePath(QStringLiteral("source"));
    const QString secondDirectory =
        temporary.filePath(QStringLiteral("source\nsuffix"));
    QVERIFY(QDir().mkpath(secondDirectory));
    const QString secondPath = QDir(secondDirectory).filePath(
        QStringLiteral("settings"));
    const QString firstKey =
        QStringLiteral("suffix/settings\ncredential");
    const QString secondKey = QStringLiteral("credential");
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString secret = QStringLiteral("credential-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));

    for (const auto &source :
         {qMakePair(firstPath, firstKey),
          qMakePair(secondPath, secondKey)}) {
        QSettings settings(source.first, QSettings::IniFormat);
        settings.setValue(source.second, secret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     source.second, QStringLiteral("missing")),
                 QVariant(secret));
        QVERIFY(!settings.contains(source.second));
    }

    int cleanupFiles = 0;
    QDirIterator files(
        stateRoot,
        QStringList{QStringLiteral("*.cleanup")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        ++cleanupFiles;
    }
    QCOMPARE(cleanupFiles, 2);
#endif
}

void TestCredentialSettings::
plaintextCleanupSurvivesLiveSettingsCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings cleanupSource(path, QSettings::IniFormat);
    QSettings liveWriter(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("credential-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    cleanupSource.setValue(plaintextKey, secret);
    cleanupSource.setValue(
        QStringLiteral("normal/before"),
        QStringLiteral("keep-before"));
    cleanupSource.sync();
    QCOMPARE(cleanupSource.status(), QSettings::NoError);
    QCOMPARE(liveWriter.value(plaintextKey).toString(), secret);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &cleanupSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!cleanupSource.contains(plaintextKey));

    liveWriter.setValue(
        QStringLiteral("normal/after"),
        QStringLiteral("keep-after"));
    liveWriter.sync();
    QCOMPARE(liveWriter.status(), QSettings::NoError);

    QSettings verified(path, QSettings::IniFormat);
    verified.sync();
    QCOMPARE(verified.status(), QSettings::NoError);
    QVERIFY(!verified.contains(plaintextKey));
    QCOMPARE(
        verified.value(
            QStringLiteral("normal/before")).toString(),
        QStringLiteral("keep-before"));
    QCOMPARE(
        verified.value(
            QStringLiteral("normal/after")).toString(),
        QStringLiteral("keep-after"));
}

void TestCredentialSettings::
plaintextCleanupUsesFreshDiskSnapshot()
{
#ifdef Q_OS_UNIX
    ScopedEnvironmentVariable fixedSnapshotTime(
        QByteArrayLiteral(
            "GC_CREDENTIAL_TEST_FIXED_SNAPSHOT_TIME"),
        QByteArrayLiteral("1"));
#endif
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString originalSecret =
        QStringLiteral("original-secret");
    const QString replacementSecret =
        QStringLiteral("replacement-sec");
    QCOMPARE(originalSecret.size(), replacementSecret.size());
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, originalSecret);
    settings.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep-normal"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
#ifdef Q_OS_UNIX
    struct stat originalInformation;
    const QByteArray encodedPath = QFile::encodeName(path);
    QCOMPARE(
        ::lstat(
            encodedPath.constData(),
            &originalInformation),
        0);
#else
    const QDateTime originalModification =
        QFileInfo(path).fileTime(
            QFileDevice::FileModificationTime);
#endif

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, originalSecret);
    bool replaced = false;
    state->afterRead = [&] {
        if (replaced || state->reads != 2)
            return;
        replaced = true;
        QByteArray contents = fileContents(path);
        QCOMPARE(
            contents.count(originalSecret.toUtf8()), 1);
        QCOMPARE(
            contents.replace(
                originalSecret.toUtf8(),
                replacementSecret.toUtf8()).size(),
            fileContents(path).size());
        QFile replacement(path);
        QVERIFY(replacement.open(
            QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(
            replacement.write(contents),
            contents.size());
        QVERIFY(replacement.flush());
        replacement.close();
#ifdef Q_OS_UNIX
        struct timespec times[2];
#ifdef Q_OS_DARWIN
        times[0] = originalInformation.st_atimespec;
        times[1] = originalInformation.st_mtimespec;
#else
        times[0] = originalInformation.st_atim;
        times[1] = originalInformation.st_mtim;
#endif
        QCOMPARE(
            ::utimensat(
                AT_FDCWD, encodedPath.constData(),
                times, 0),
            0);
        struct stat replacedInformation;
        QCOMPARE(
            ::lstat(
                encodedPath.constData(),
                &replacedInformation),
            0);
        QCOMPARE(
            replacedInformation.st_dev,
            originalInformation.st_dev);
        QCOMPARE(
            replacedInformation.st_ino,
            originalInformation.st_ino);
        QCOMPARE(
            replacedInformation.st_size,
            originalInformation.st_size);
#ifdef Q_OS_DARWIN
        QCOMPARE(
            replacedInformation.st_mtimespec.tv_sec,
            originalInformation.st_mtimespec.tv_sec);
        QCOMPARE(
            replacedInformation.st_mtimespec.tv_nsec,
            originalInformation.st_mtimespec.tv_nsec);
#else
        QCOMPARE(
            replacedInformation.st_mtim.tv_sec,
            originalInformation.st_mtim.tv_sec);
        QCOMPARE(
            replacedInformation.st_mtim.tv_nsec,
            originalInformation.st_mtim.tv_nsec);
#endif
#else
        QVERIFY(replacement.open(QIODevice::ReadOnly));
        QVERIFY(replacement.setFileTime(
            originalModification,
            QFileDevice::FileModificationTime));
        replacement.close();
#endif
    };

    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(originalSecret));
    QVERIFY(replaced);
    const QByteArray finalContents = fileContents(path);
    QVERIFY(finalContents.contains(
        replacementSecret.toUtf8()));
    QVERIFY(finalContents.contains(
        QByteArrayLiteral("keep-normal")));
}

void TestCredentialSettings::
plaintextCleanupBypassesStaleNegativeCache()
{
#ifndef Q_OS_UNIX
    QSKIP("Exact timestamp restoration is required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString placeholderKey(
        plaintextKey.size(), QLatin1Char('x'));
    const QString secret =
        QStringLiteral("stale-negative-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    QVERIFY(placeholderKey != plaintextKey);
    settings.setValue(placeholderKey, secret);
    settings.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep-normal"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(!settings.contains(plaintextKey));

    struct stat originalInformation;
    const QByteArray encodedPath = QFile::encodeName(path);
    QCOMPARE(
        ::lstat(
            encodedPath.constData(),
            &originalInformation),
        0);
    QByteArray contents = fileContents(path);
    QCOMPARE(contents.count(placeholderKey.toUtf8()), 1);
    QCOMPARE(
        contents.replace(
            placeholderKey.toUtf8(),
            plaintextKey.toUtf8()).size(),
        fileContents(path).size());
    QFile replacement(path);
    QVERIFY(replacement.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(replacement.write(contents), contents.size());
    QVERIFY(replacement.flush());
    replacement.close();

    struct timespec times[2];
#ifdef Q_OS_DARWIN
    times[0] = originalInformation.st_atimespec;
    times[1] = originalInformation.st_mtimespec;
#else
    times[0] = originalInformation.st_atim;
    times[1] = originalInformation.st_mtim;
#endif
    QCOMPARE(
        ::utimensat(
            AT_FDCWD, encodedPath.constData(),
            times, 0),
        0);
    struct stat replacedInformation;
    QCOMPARE(
        ::lstat(
            encodedPath.constData(),
            &replacedInformation),
        0);
    QCOMPARE(
        replacedInformation.st_dev,
        originalInformation.st_dev);
    QCOMPARE(
        replacedInformation.st_ino,
        originalInformation.st_ino);
    QCOMPARE(
        replacedInformation.st_size,
        originalInformation.st_size);
#ifdef Q_OS_DARWIN
    QCOMPARE(
        replacedInformation.st_mtimespec.tv_sec,
        originalInformation.st_mtimespec.tv_sec);
    QCOMPARE(
        replacedInformation.st_mtimespec.tv_nsec,
        originalInformation.st_mtimespec.tv_nsec);
#else
    QCOMPARE(
        replacedInformation.st_mtim.tv_sec,
        originalInformation.st_mtim.tv_sec);
    QCOMPARE(
        replacedInformation.st_mtim.tv_nsec,
        originalInformation.st_mtim.tv_nsec);
#endif
    QVERIFY(!settings.contains(plaintextKey));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    const QByteArray finalContents = fileContents(path);
    QVERIFY(!finalContents.contains(secret.toUtf8()));
    QVERIFY(finalContents.contains(
        QByteArrayLiteral("keep-normal")));
#endif
}

void TestCredentialSettings::
plaintextRemovalBypassesStaleNegativeCache()
{
#ifndef Q_OS_UNIX
    QSKIP("Exact timestamp restoration is required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString placeholderKey(
        plaintextKey.size(), QLatin1Char('x'));
    const QString secret =
        QStringLiteral("stale-delete-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("preparing"),
        &transaction));

    settings.setValue(placeholderKey, secret);
    settings.setValue(removalKey, true);
    settings.setValue(
        generationKey, QString::fromLatin1(transaction));
    settings.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep-normal"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(!settings.contains(plaintextKey));

    struct stat originalInformation;
    const QByteArray encodedPath = QFile::encodeName(path);
    QCOMPARE(
        ::lstat(
            encodedPath.constData(),
            &originalInformation),
        0);
    QByteArray contents = fileContents(path);
    QCOMPARE(contents.count(placeholderKey.toUtf8()), 1);
    QCOMPARE(
        contents.replace(
            placeholderKey.toUtf8(),
            plaintextKey.toUtf8()).size(),
        fileContents(path).size());
    QFile replacement(path);
    QVERIFY(replacement.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(replacement.write(contents), contents.size());
    QVERIFY(replacement.flush());
    replacement.close();

    struct timespec times[2];
#ifdef Q_OS_DARWIN
    times[0] = originalInformation.st_atimespec;
    times[1] = originalInformation.st_mtimespec;
#else
    times[0] = originalInformation.st_atim;
    times[1] = originalInformation.st_mtim;
#endif
    QCOMPARE(
        ::utimensat(
            AT_FDCWD, encodedPath.constData(),
            times, 0),
        0);
    QVERIFY(!settings.contains(plaintextKey));
    QVERIFY(fileContents(path).contains(secret.toUtf8()));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    const QByteArray finalContents = fileContents(path);
    QVERIFY(!finalContents.contains(secret.toUtf8()));
    QVERIFY(finalContents.contains(
        QByteArrayLiteral("keep-normal")));
    QVERIFY(!settings.contains(removalKey));
    QVERIFY(!settings.contains(generationKey));
#endif
}

void TestCredentialSettings::
plaintextSnapshotCrashLeavesNoCopy()
{
    const QString childAction = qEnvironmentVariable(
        "GC_PLAINTEXT_SNAPSHOT_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString settingsPath = qEnvironmentVariable(
            "GC_PLAINTEXT_SNAPSHOT_CRASH_SETTINGS");
        const QString scope = qEnvironmentVariable(
            "GC_PLAINTEXT_SNAPSHOT_CRASH_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!scope.isEmpty());
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        auto state = std::make_shared<FakeStoreState>();
        CredentialSettings credentials(fakeStore(state));
        credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("missing"));
        QFAIL("Configured snapshot crash was not reached");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret =
        QStringLiteral("snapshot-plaintext-secret");
    {
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        settings.setValue(plaintextKey, secret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral(
            "GC_PLAINTEXT_SNAPSHOT_CRASH_ACTION"),
        QStringLiteral("crash"));
    environment.insert(
        QStringLiteral(
            "GC_PLAINTEXT_SNAPSHOT_CRASH_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral(
            "GC_PLAINTEXT_SNAPSHOT_CRASH_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_CRASH_POINT"),
        QStringLiteral("cleanup:snapshot-staged"));

    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral("plaintextSnapshotCrashLeavesNoCopy")
    });
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY2(
        child.waitForStarted(5000),
        qPrintable(child.errorString()));
    QVERIFY2(
        child.waitForFinished(15000),
        qPrintable(child.errorString()));
    const QByteArray childOutput = child.readAll();
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 86,
        childOutput.constData());

    const QStringList snapshots =
        QDir(temporary.path()).entryList(
            QStringList{
                QStringLiteral(
                    ".gc-credential-snapshot-*.tmp")
            },
            QDir::Files | QDir::Hidden);
    QVERIFY(snapshots.isEmpty());
    QVERIFY(fileContents(settingsPath).contains(
        secret.toUtf8()));
    QDirIterator stateArtifacts(
        stateRoot, QDir::Files,
        QDirIterator::Subdirectories);
    while (stateArtifacts.hasNext()) {
        const QString artifact = stateArtifacts.next();
        QVERIFY2(
            !fileContents(artifact).contains(
                secret.toUtf8()),
            qPrintable(artifact));
    }
}

void TestCredentialSettings::
settingsSerializationCrashLeavesPrivateCopy()
{
    const QString childAction = qEnvironmentVariable(
        "GC_SETTINGS_SERIALIZATION_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString settingsPath = qEnvironmentVariable(
            "GC_SETTINGS_SERIALIZATION_CRASH_SETTINGS");
        const QString scope = qEnvironmentVariable(
            "GC_SETTINGS_SERIALIZATION_CRASH_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!scope.isEmpty());
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        const QString plaintextKey =
            plainKey(GC_STRAVA_TOKEN);
        const QString vaultKey = CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
        auto state = std::make_shared<FakeStoreState>();
        state->values.insert(
            vaultKey,
            QStringLiteral("serialization-target-secret"));
        CredentialSettings credentials(fakeStore(state));
        credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("missing"));
        QFAIL("Configured serialization crash was not reached");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString targetSecret =
        QStringLiteral("serialization-target-secret");
    const QString retainedSecret =
        QStringLiteral("serialization-retained-secret");
    {
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        settings.setValue(plaintextKey, targetSecret);
        settings.setValue(
            plainKey(GC_STRAVA_REFRESH_TOKEN),
            retainedSecret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral(
            "GC_SETTINGS_SERIALIZATION_CRASH_ACTION"),
        QStringLiteral("crash"));
    environment.insert(
        QStringLiteral(
            "GC_SETTINGS_SERIALIZATION_CRASH_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral(
            "GC_SETTINGS_SERIALIZATION_CRASH_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_CRASH_POINT"),
        QStringLiteral("cleanup:settings-serialized"));

    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "settingsSerializationCrashLeavesPrivateCopy")
    });
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY2(
        child.waitForStarted(5000),
        qPrintable(child.errorString()));
    QVERIFY2(
        child.waitForFinished(15000),
        qPrintable(child.errorString()));
    const QByteArray childOutput = child.readAll();
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 86,
        childOutput.constData());

    QStringList copies;
    QDirIterator copyIterator(
        stateRoot,
        QStringList{
            QStringLiteral(
                ".gc-credential-settings-*.tmp")
        },
        QDir::Files | QDir::Hidden,
        QDirIterator::Subdirectories);
    while (copyIterator.hasNext())
        copies.append(copyIterator.next());
    QCOMPARE(copies.size(), 1);
    const QString copyPath = copies.constFirst();
    QVERIFY(fileContents(copyPath).contains(
        retainedSecret.toUtf8()));
#ifdef Q_OS_WIN
    QVERIFY(windowsFileHasOwnerOnlyAcl(copyPath));
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(
        QFileInfo(copyPath).absolutePath()));
#else
    verifyOwnerOnlyPermissions(copyPath);
    verifyOwnerOnlyPermissions(
        QFileInfo(copyPath).absolutePath());
#endif

    QSettings recovery(
        settingsPath, QSettings::IniFormat);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto recoveryState =
        std::make_shared<FakeStoreState>();
    recoveryState->values.insert(
        vaultKey, targetSecret);
    CredentialSettings credentials(
        fakeStore(recoveryState));
    QCOMPARE(credentials.value(
                 &recovery, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(targetSecret));
    const QStringList scratchDirectories =
        QDir(QDir(stateRoot).filePath(
                 QStringLiteral(
                     "GoldenCheetah/credential-locks")))
            .entryList(
                QStringList{
                    QStringLiteral(
                        ".gc-credential-scratch-*")
                },
                QDir::Dirs | QDir::Hidden
                    | QDir::NoDotAndDotDot);
    QVERIFY2(
        scratchDirectories.isEmpty(),
        qPrintable(scratchDirectories.join(
            QLatin1Char('\n'))));
}

void TestCredentialSettings::
scratchCleanupLockRecoversAfterCrash()
{
    const QString childAction = qEnvironmentVariable(
        "GC_SCRATCH_LOCK_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString settingsPath = qEnvironmentVariable(
            "GC_SCRATCH_LOCK_CRASH_SETTINGS");
        const QString scope = qEnvironmentVariable(
            "GC_SCRATCH_LOCK_CRASH_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!scope.isEmpty());
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        const QString plaintextKey =
            plainKey(GC_STRAVA_TOKEN);
        const QString vaultKey = CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
        auto state = std::make_shared<FakeStoreState>();
        state->values.insert(
            vaultKey,
            QStringLiteral("scratch-lock-secret"));
        CredentialSettings credentials(fakeStore(state));
        credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("missing"));
        QFAIL("Configured scratch lock crash was not reached");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret =
        QStringLiteral("scratch-lock-secret");
    {
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        settings.setValue(plaintextKey, secret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("GC_SCRATCH_LOCK_CRASH_ACTION"),
        QStringLiteral("crash"));
    environment.insert(
        QStringLiteral("GC_SCRATCH_LOCK_CRASH_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_SCRATCH_LOCK_CRASH_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_CRASH_POINT"),
        QStringLiteral(
            "cleanup:scratch-lock-acquired"));

    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "scratchCleanupLockRecoversAfterCrash")
    });
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY2(
        child.waitForStarted(5000),
        qPrintable(child.errorString()));
    QVERIFY2(
        child.waitForFinished(15000),
        qPrintable(child.errorString()));
    const QByteArray childOutput = child.readAll();
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 86,
        childOutput.constData());

    const QString lockPath = QDir(stateRoot).filePath(
        QStringLiteral(
            "GoldenCheetah/credential-locks/"
            ".gc-credential-scratch-cleanup.lock"));
    QVERIFY(QFileInfo::exists(lockPath));

    QSettings settings(
        settingsPath, QSettings::IniFormat);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!fileContents(settingsPath).contains(
        secret.toUtf8()));
    QVERIFY(!QFileInfo::exists(lockPath));
}

void TestCredentialSettings::
vaultOnlyReadIgnoresSettingsCleanupLock()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString secret =
        QStringLiteral("vault-only-secret");
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);

    QLockFile settingsLock(path + QStringLiteral(".lock"));
    settingsLock.setStaleLockTime(0);
    QVERIFY(settingsLock.tryLock());
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 1);
    settingsLock.unlock();
}

void TestCredentialSettings::
plaintextCleanupFlushesLivePendingSettings()
{
    const QString action = qEnvironmentVariable(
        "GC_PLAINTEXT_CLEANUP_PENDING_ACTION");
    if (!action.isEmpty()) {
        QCOMPARE(action, QStringLiteral("child"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ScopedEnvironmentVariable stateRootEnvironment(
            QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
            QFile::encodeName(temporary.filePath(
                QStringLiteral("credential-state"))));
        const QString path =
            temporary.filePath(QStringLiteral("private.ini"));
        QSettings cleanupSource(path, QSettings::IniFormat);
        QSettings liveWriter(path, QSettings::IniFormat);
        const QString scope =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const QString plaintextKey =
            plainKey(GC_STRAVA_TOKEN);
        const QString secret =
            QStringLiteral("credential-secret");
        const QString vaultKey =
            CredentialSettings::vaultKey(
                scope, GC_STRAVA_TOKEN);
        cleanupSource.setValue(plaintextKey, secret);
        cleanupSource.sync();
        QCOMPARE(
            cleanupSource.status(), QSettings::NoError);
        QCOMPARE(
            liveWriter.value(plaintextKey).toString(),
            secret);

        auto state = std::make_shared<FakeStoreState>();
        state->values.insert(vaultKey, secret);
        bool pendingWriteQueued = false;
        state->afterRead = [&] {
            if (pendingWriteQueued || state->reads != 2)
                return;
            pendingWriteQueued = true;
            liveWriter.setValue(
                QStringLiteral("normal/pending"),
                QStringLiteral("keep-pending"));
        };

        CredentialSettings credentials(fakeStore(state));
        QCOMPARE(credentials.value(
                     &cleanupSource, scope,
                     GC_STRAVA_TOKEN, plaintextKey,
                     QStringLiteral("missing")),
                 QVariant(secret));
        QVERIFY(pendingWriteQueued);
        QVERIFY(!cleanupSource.contains(plaintextKey));
        liveWriter.sync();
        QCOMPARE(liveWriter.status(), QSettings::NoError);

        QSettings verified(path, QSettings::IniFormat);
        verified.sync();
        QCOMPARE(verified.status(), QSettings::NoError);
        QVERIFY(!verified.contains(plaintextKey));
        QCOMPARE(
            verified.value(
                QStringLiteral("normal/pending")).toString(),
            QStringLiteral("keep-pending"));
        return;
    }

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "GC_PLAINTEXT_CLEANUP_PENDING_ACTION"),
        QStringLiteral("child"));
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "plaintextCleanupFlushesLivePendingSettings")
    });
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY(child.waitForStarted(5000));
    if (!child.waitForFinished(8000)) {
        child.kill();
        child.waitForFinished();
        const QByteArray output = child.readAll();
        QFAIL(output.isEmpty()
                  ? "Credential cleanup deadlocked"
                  : output.constData());
    }
    const QByteArray output = child.readAll();
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 0,
        output.constData());
}

void TestCredentialSettings::plaintextMigratesToVault()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    const QString sentinel = QStringLiteral("legacy-secret-sentinel");
    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(plainKey(GC_STRAVA_REFRESH_TOKEN), sentinel);
    ini.setValue(QStringLiteral("normal/value"), QStringLiteral("keep"));
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_REFRESH_TOKEN);

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_REFRESH_TOKEN,
                 plainKey(GC_STRAVA_REFRESH_TOKEN), QStringLiteral("default")),
             QVariant(sentinel));
    QCOMPARE(state->values.value(vaultKey), sentinel);
    QVERIFY(!ini.contains(plainKey(GC_STRAVA_REFRESH_TOKEN)));
    QCOMPARE(ini.value(QStringLiteral("normal/value")).toString(),
             QStringLiteral("keep"));
    ini.sync();
    QVERIFY(!fileContents(path).contains(sentinel.toUtf8()));
    QVERIFY(!QFileInfo::exists(credentialOperationFile(
        QFile::decodeName(qgetenv(
            "GC_CREDENTIAL_TEST_STATE_ROOT")),
        vaultKey, QStringLiteral(".deletion"))));
    verifyOwnerOnlyPermissions(path);
}

void TestCredentialSettings::
vaultValueWinsAndDistinctPlaintextIsRetained()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plainKey(GC_STRAVA_TOKEN),
                 QStringLiteral("stale-plaintext"));
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-value"));
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("default")),
             QVariant(QStringLiteral("vault-value")));
    QCOMPARE(
        ini.value(
            plainKey(GC_STRAVA_TOKEN)).toString(),
        QStringLiteral("stale-plaintext"));
    ini.sync();
    QVERIFY(fileContents(path).contains("stale-plaintext"));
    QVERIFY(!QFileInfo::exists(credentialOperationFile(
        QFile::decodeName(qgetenv(
            "GC_CREDENTIAL_TEST_STATE_ROOT")),
        vaultKey, QStringLiteral(".deletion"))));
}

void TestCredentialSettings::writesAndDeletesNeverTouchIni()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));

    credentials.setValue(
        &ini, scope, GC_DVPASS, plainKey(GC_DVPASS),
        QStringLiteral("new-secret"));
    QCOMPARE(state->values.value(CredentialSettings::vaultKey(
                 scope, GC_DVPASS)), QStringLiteral("new-secret"));
    QVERIFY(!ini.contains(plainKey(GC_DVPASS)));

    credentials.setValue(
        &ini, scope, GC_DVPASS, plainKey(GC_DVPASS), QString());
    QVERIFY(!state->values.contains(CredentialSettings::vaultKey(
        scope, GC_DVPASS)));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_DVPASS, plainKey(GC_DVPASS),
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    ini.sync();
    QVERIFY(!fileContents(path).contains("new-secret"));
}

void TestCredentialSettings::failedMigrationIsRetriedWithoutCredentialLoss()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString sentinel = QStringLiteral("memory-only-secret");
    ini.setValue(plainKey(GC_NOKIA_REFRESH_TOKEN), sentinel);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings currentSession(fakeStore(state));
    QCOMPARE(currentSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN,
                 plainKey(GC_NOKIA_REFRESH_TOKEN), QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(ini.contains(plainKey(GC_NOKIA_REFRESH_TOKEN)));
    ini.sync();
    QVERIFY(fileContents(path).contains(sentinel.toUtf8()));
    verifyOwnerOnlyPermissions(path);

    state->failWrites = false;
    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN,
                 plainKey(GC_NOKIA_REFRESH_TOKEN), QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(!ini.contains(plainKey(GC_NOKIA_REFRESH_TOKEN)));
    ini.sync();
    QVERIFY(!fileContents(path).contains(sentinel.toUtf8()));
    QCOMPARE(state->values.value(CredentialSettings::vaultKey(
                 scope, GC_NOKIA_REFRESH_TOKEN)), sentinel);

    CredentialSettings persistedSession(fakeStore(state));
    QCOMPARE(persistedSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN,
                 plainKey(GC_NOKIA_REFRESH_TOKEN), QStringLiteral("missing")),
             QVariant(sentinel));
}

void TestCredentialSettings::failedNewCredentialWriteIsMemoryOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString sentinel = QStringLiteral("memory-only-new-secret");

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings currentSession(fakeStore(state));
    currentSession.setValue(
        &ini, scope, GC_NOKIA_REFRESH_TOKEN,
        plainKey(GC_NOKIA_REFRESH_TOKEN), sentinel);
    QCOMPARE(currentSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN,
                 plainKey(GC_NOKIA_REFRESH_TOKEN), QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(!ini.contains(plainKey(GC_NOKIA_REFRESH_TOKEN)));
    ini.sync();
    QVERIFY(!fileContents(path).contains(sentinel.toUtf8()));

    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN,
                 plainKey(GC_NOKIA_REFRESH_TOKEN), QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
}

void TestCredentialSettings::checkedCredentialWriteReportsPersistence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString sentinel =
        QStringLiteral("checked-refresh-token");

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.setValueChecked(
        &ini, scope, GC_STRAVA_REFRESH_TOKEN,
        plainKey(GC_STRAVA_REFRESH_TOKEN), sentinel));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_REFRESH_TOKEN,
                 plainKey(GC_STRAVA_REFRESH_TOKEN),
                 QStringLiteral("missing")),
             QVariant(sentinel));

    state->failWrites = false;
    QVERIFY(credentials.setValueChecked(
        &ini, scope, GC_STRAVA_REFRESH_TOKEN,
        plainKey(GC_STRAVA_REFRESH_TOKEN), sentinel));

    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 &ini, scope, GC_STRAVA_REFRESH_TOKEN,
                 plainKey(GC_STRAVA_REFRESH_TOKEN),
                 QStringLiteral("missing")),
             QVariant(sentinel));
}

void TestCredentialSettings::failedReplacementPreservesLegacyCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_NOKIA_REFRESH_TOKEN);
    const QString oldSecret = QStringLiteral("old-legacy-secret");
    const QString newSecret = QStringLiteral("new-memory-secret");
    ini.setValue(plaintextKey, oldSecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings currentSession(fakeStore(state));
    currentSession.setValue(
        &ini, scope, GC_NOKIA_REFRESH_TOKEN, plaintextKey, newSecret);
    QCOMPARE(currentSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(newSecret));
    QVERIFY(ini.contains(plaintextKey));
    QCOMPARE(ini.value(plaintextKey).toString(), oldSecret);
    ini.sync();
    QVERIFY(fileContents(path).contains(oldSecret.toUtf8()));
    QVERIFY(!fileContents(path).contains(newSecret.toUtf8()));
    verifyOwnerOnlyPermissions(path);

    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 &ini, scope, GC_NOKIA_REFRESH_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(oldSecret));
}

void TestCredentialSettings::
creatingRecoveryFindsLegacyInOtherSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    QSettings legacySource(
        temporary.filePath(QStringLiteral("legacy.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey =
        plainKey(GC_NOKIA_REFRESH_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_NOKIA_REFRESH_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    legacySource.setValue(
        plaintextKey, QStringLiteral("legacy-credential"));
    legacySource.sync();
    QCOMPARE(legacySource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    {
        CredentialSettings writer(fakeStore(state));
        QVERIFY(!writer.setValueChecked(
            &currentSource, scope, GC_NOKIA_REFRESH_TOKEN,
            plaintextKey,
            QStringLiteral("memory-only-credential")));
        QCOMPARE(writer.value(
                     &currentSource, scope,
                     GC_NOKIA_REFRESH_TOKEN, plaintextKey,
                     QStringLiteral("missing")),
                 QVariant(QStringLiteral(
                     "memory-only-credential")));
    }
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("creating")));
    QCOMPARE(state->writes, 1);

    state->failWrites = false;
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &currentSource, scope,
                 GC_NOKIA_REFRESH_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->writes, 1);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("creating")));

    QCOMPARE(restarted.value(
                 &legacySource, scope,
                 GC_NOKIA_REFRESH_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("legacy-credential")));
    QCOMPARE(state->writes, 2);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("legacy-credential"));
    QVERIFY(!legacySource.contains(plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
duplicatePlaintextDoesNotResurrectDeletedCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings deletionSource(
        temporary.filePath(QStringLiteral("deletion.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    duplicateSource.setValue(
        plaintextKey, QStringLiteral("duplicate-secret"));
    duplicateSource.sync();
    QCOMPARE(duplicateSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-secret"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.removeChecked(
        &deletionSource, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->writes, 0);
    QVERIFY(!duplicateSource.contains(plaintextKey));
    QVERIFY(!state->values.contains(vaultKey));

    QVERIFY(restarted.setValueChecked(
        &duplicateSource, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement-secret")));
    QCOMPARE(state->writes, 1);
    CredentialSettings finalSession(fakeStore(state));
    QCOMPARE(finalSession.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("replacement-secret")));
}

void TestCredentialSettings::
failedDeleteIntentAppliesAcrossPlaintextSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings deletionSource(
        temporary.filePath(QStringLiteral("deletion.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    duplicateSource.setValue(
        plaintextKey, QStringLiteral("duplicate-secret"));
    duplicateSource.sync();
    QCOMPARE(duplicateSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-secret"));
    state->failRemoves = true;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &deletionSource, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->removes, 1);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("vault-secret"));

    state->failRemoves = false;
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QCOMPARE(state->writes, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(!duplicateSource.contains(plaintextKey));
}

void TestCredentialSettings::
deletionStatePersistenceFailurePreservesVault()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(QDir().mkpath(deletionPath));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("credential-to-preserve"));
}

void TestCredentialSettings::
completedDeletionPersistsGlobalTombstone()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-delete"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("deleted")));
    verifyOwnerOnlyPermissions(deletionPath);

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("replacement-credential")));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("replacement-credential"));

    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("deleted")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));
}

void TestCredentialSettings::
replacementRecoveryRetainsUnboundPlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("replacing"),
        &transaction));
    settings.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("committed-replacement"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-replacement")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("replacing")),
        transaction);
}

void TestCredentialSettings::
uncommittedCleanupIntentCannotAuthorizeReplacement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret =
        QStringLiteral("stale-plaintext");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    settings.setValue(plaintextKey, staleSecret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    QVERIFY(!cleanupPath.isEmpty());

    auto state = std::make_shared<FakeStoreState>();
    state->failReads = true;
    {
        CredentialSettings preparing(fakeStore(state));
        QCOMPARE(preparing.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }

    const QList<QByteArray> cleanupFields =
        fileContents(cleanupPath).trimmed().split(' ');
    QCOMPARE(cleanupFields.size(), 4);
    QCOMPARE(cleanupFields.at(0), QByteArrayLiteral("v1"));
    QCOMPARE(cleanupFields.at(1), QByteArrayLiteral("intent"));
    const QByteArray transaction = cleanupFields.at(2);
    QVERIFY(isCredentialTestTransaction(transaction));
    QVERIFY(writePrivateStateFile(
        deletionPath,
        QByteArrayLiteral("v1 updating ")
            + transaction + QByteArrayLiteral("\n")));

    state->failReads = false;
    state->values.insert(
        vaultKey, QStringLiteral("committed-replacement"));
    CredentialSettings recovering(fakeStore(state));
    QCOMPARE(recovering.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-replacement")));
    QCOMPARE(settings.value(plaintextKey).toString(), staleSecret);
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("updating")),
        transaction);
}

void TestCredentialSettings::
authorizedCleanupRequiresWrittenVaultValue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("active")));
    settings.setValue(
        plaintextKey, QStringLiteral("old-plaintext-secret"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-vault-secret"));
    FakeStoreState *const statePointer = state.get();
    state->beforeRead = [statePointer, vaultKey] {
        if (statePointer->values.value(vaultKey)
            == QStringLiteral("requested-vault-secret")) {
            statePointer->values.insert(
                vaultKey, QStringLiteral("superseding-secret"));
        }
    };

    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("requested-vault-secret")));
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        QStringLiteral("old-plaintext-secret"));
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("superseding-secret"));
}

void TestCredentialSettings::
authorizedCleanupCrashRecoveryAcrossProcesses()
{
    const QString childAction = qEnvironmentVariable(
        "GC_AUTHORIZED_CLEANUP_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString settingsPath = qEnvironmentVariable(
            "GC_AUTHORIZED_CLEANUP_CRASH_SETTINGS");
        const QString vaultPath = qEnvironmentVariable(
            "GC_AUTHORIZED_CLEANUP_CRASH_VAULT");
        const QString scope = qEnvironmentVariable(
            "GC_AUTHORIZED_CLEANUP_CRASH_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!vaultPath.isEmpty());
        QVERIFY(!scope.isEmpty());

        QSettings settings(settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(
            std::make_unique<FileCredentialStore>(vaultPath));
        credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("new-vault-secret"));
        QFAIL("Configured authorized-cleanup crash was not reached");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("active")));

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue(
        plaintextKey, QStringLiteral("old-plaintext-secret"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    QVERIFY(!cleanupPath.isEmpty());
    FileCredentialStore vault(vaultPath);
    QString error;
    QCOMPARE(
        vault.write(
            vaultKey, QStringLiteral("old-vault-secret"),
            &error),
        CredentialStore::Status::Success);

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("GC_AUTHORIZED_CLEANUP_CRASH_ACTION"),
        QStringLiteral("crash"));
    environment.insert(
        QStringLiteral("GC_AUTHORIZED_CLEANUP_CRASH_SETTINGS"),
        settingsPath);
    environment.insert(
        QStringLiteral("GC_AUTHORIZED_CLEANUP_CRASH_VAULT"),
        vaultPath);
    environment.insert(
        QStringLiteral("GC_AUTHORIZED_CLEANUP_CRASH_SCOPE"),
        scope);
    environment.insert(
        QStringLiteral("GC_CREDENTIAL_TEST_CRASH_POINT"),
        QStringLiteral("cleanup:authorized"));

    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({
        QStringLiteral(
            "authorizedCleanupCrashRecoveryAcrossProcesses")
    });
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY2(
        child.waitForStarted(5000),
        qPrintable(child.errorString()));
    QVERIFY2(
        child.waitForFinished(15000),
        qPrintable(child.errorString()));
    const QByteArray childOutput = child.readAll();
    QVERIFY2(
        child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 86,
        childOutput.constData());

    QSettings afterCrash(settingsPath, QSettings::IniFormat);
    QCOMPARE(
        afterCrash.value(plaintextKey).toString(),
        QStringLiteral("old-plaintext-secret"));
    const QList<QByteArray> cleanupFields =
        fileContents(cleanupPath).trimmed().split(' ');
    QCOMPARE(cleanupFields.size(), 4);
    QCOMPARE(cleanupFields.at(0), QByteArrayLiteral("v1"));
    QCOMPARE(
        cleanupFields.at(1),
        QByteArrayLiteral("authorized"));
    const QByteArray transaction = cleanupFields.at(2);
    const QByteArray cleanupMetadata =
        fileContents(cleanupPath);
    QVERIFY(!cleanupMetadata.contains(
        QByteArrayLiteral("old-plaintext-secret")));
    QVERIFY(!cleanupMetadata.contains(
        QByteArrayLiteral("new-vault-secret")));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("updating")),
        transaction);

    CredentialSettings recovering(
        std::make_unique<FileCredentialStore>(vaultPath));
    QCOMPARE(recovering.value(
                 &afterCrash, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("new-vault-secret")));
    QCOMPARE(
        afterCrash.value(plaintextKey).toString(),
        QStringLiteral("old-plaintext-secret"));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("updating")),
        transaction);

    QVERIFY(recovering.setValueChecked(
        &afterCrash, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("new-vault-secret")));
    QVERIFY(!afterCrash.contains(plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
incompleteReplacementBlocksPlaintextMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("replacing")));
    settings.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("replacing")));

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("replacement-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("replacement-credential"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
stalePendingRemovalDoesNotDeleteReplacementAcrossSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings staleSource(
        temporary.filePath(QStringLiteral("stale.ini")),
        QSettings::IniFormat);
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    staleSource.setValue(removalKey, true);
    staleSource.sync();
    QCOMPARE(staleSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-credential"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.removeChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QVERIFY(credentials.setValueChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement-credential")));
    QCOMPARE(state->removes, 1);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &staleSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("replacement-credential")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!staleSource.contains(removalKey));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("replacement-credential"));
}

void TestCredentialSettings::
stalePendingRemovalDoesNotDeleteOrdinaryWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings staleSource(
        temporary.filePath(QStringLiteral("stale.ini")),
        QSettings::IniFormat);
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    staleSource.setValue(removalKey, true);
    staleSource.sync();
    QCOMPARE(staleSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.setValueChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("new-credential")));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &staleSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("new-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 0);
    QVERIFY(!staleSource.contains(removalKey));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("new-credential"));
}

void TestCredentialSettings::
activeStateBlocksDuplicateMigrationAfterExternalVaultLoss()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-credential"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.removeChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QVERIFY(credentials.setValueChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement-credential")));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
    QCOMPARE(state->writes, 1);

    state->values.remove(vaultKey);
    duplicateSource.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    duplicateSource.sync();
    QCOMPARE(duplicateSource.status(), QSettings::NoError);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(
        duplicateSource.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
    QVERIFY(!state->values.contains(vaultKey));
}

void TestCredentialSettings::
uncertainActiveUpdateDoesNotMigrateDuplicatePlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    {
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(credentials.setValueChecked(
            &currentSource, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("active-credential")));
        QVERIFY(credentialPhaseIs(
            deletionPath, QByteArrayLiteral("active")));

        duplicateSource.setValue(
            plaintextKey, QStringLiteral("stale-duplicate"));
        duplicateSource.sync();
        QCOMPARE(duplicateSource.status(), QSettings::NoError);

        state->failWrites = true;
        state->beforeWrite = [state, vaultKey] {
            state->values.remove(vaultKey);
        };
        QVERIFY(!credentials.setValueChecked(
            &currentSource, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("uncertain-update")));
        QVERIFY(credentialPhaseIs(
            deletionPath, QByteArrayLiteral("updating")));
    }

    state->failWrites = false;
    state->beforeWrite = {};
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->writes, 2);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(
        duplicateSource.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
}

void TestCredentialSettings::
orphanedDeletePreparationDoesNotRemoveCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("preparing")));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("credential-to-preserve"));

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("replacement-credential"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
preparedDeleteWithMarkerCompletes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("preparing"),
        &transaction));
    settings.setValue(removalKey, true);
    settings.setValue(
        generationKey, QString::fromLatin1(transaction));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-delete"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(!settings.contains(removalKey));
    QVERIFY(!settings.contains(generationKey));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("deleted")),
        transaction);
}

void TestCredentialSettings::
mismatchedPreparedMarkerDoesNotDeleteCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("stale.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray currentTransaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("preparing"),
        &currentTransaction));
    const QByteArray staleTransaction =
        QUuid::createUuid().toRfc4122().toHex();
    QVERIFY(staleTransaction != currentTransaction);
    settings.setValue(removalKey, true);
    settings.setValue(
        generationKey,
        QString::fromLatin1(staleTransaction));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(!settings.contains(removalKey));
    QVERIFY(!settings.contains(generationKey));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("preparing")),
        currentTransaction);

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey,
        QStringLiteral("replacement-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("replacement-credential"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
orphanedGeneratedRemovalFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("orphaned.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QByteArray orphanedTransaction =
        QUuid::createUuid().toRfc4122().toHex();
    settings.setValue(removalKey, true);
    settings.setValue(
        generationKey,
        QString::fromLatin1(orphanedTransaction));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("newer-vault-credential"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("newer-vault-credential"));
    QVERIFY(settings.value(removalKey).toBool());
    QCOMPARE(
        settings.value(generationKey).toString().toLatin1(),
        orphanedTransaction);
}

void TestCredentialSettings::
malformedGeneratedRemovalFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    QSettings settings(
        temporary.filePath(QStringLiteral("malformed.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    settings.setValue(removalKey, true);
    settings.setValue(
        generationKey, QStringLiteral("invalid-generation"));
    settings.setValue(
        plaintextKey, QStringLiteral("duplicate-plaintext"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-credential"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("vault-credential"));
    QVERIFY(settings.contains(removalKey));
    QVERIFY(settings.contains(generationKey));
    QVERIFY(settings.contains(plaintextKey));
}

void TestCredentialSettings::
credentialTransactionMetadataContainsNoSecrets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    const QString scope =
        QStringLiteral("private-scope-sentinel");
    const QString secret =
        QStringLiteral("private-token-sentinel");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    state->failRemoves = true;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);

    const QByteArray transaction =
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("deleting"));
    QVERIFY(isCredentialTestTransaction(transaction));
    QCOMPARE(
        settings.value(generationKey).toString().toLatin1(),
        transaction);
    QVERIFY(settings.value(removalKey).toBool());
    verifyOwnerOnlyPermissions(deletionPath);

    const QByteArray stateMetadata =
        fileContents(deletionPath);
    const QByteArray settingsMetadata =
        fileContents(settingsPath);
    for (const QByteArray &privateValue :
         {secret.toUtf8(), scope.toUtf8(),
          vaultKey.toUtf8(),
          QStringLiteral(GC_STRAVA_TOKEN).toUtf8()}) {
        QVERIFY(!stateMetadata.contains(privateValue));
        QVERIFY(!settingsMetadata.contains(privateValue));
    }
}

void TestCredentialSettings::
plaintextCleanupMetadataContainsNoSecrets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    const QString scope =
        QStringLiteral("private-scope-sentinel");
    const QString secret =
        QStringLiteral("private-token-sentinel");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    QVERIFY(!cleanupPath.isEmpty());

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
    verifyOwnerOnlyPermissions(cleanupPath);

    const QByteArray cleanupMetadata =
        fileContents(cleanupPath);
    const QList<QByteArray> fields =
        cleanupMetadata.trimmed().split(' ');
    QCOMPARE(fields.size(), 4);
    QCOMPARE(fields.at(0), QByteArrayLiteral("v1"));
    QCOMPARE(fields.at(1), QByteArrayLiteral("complete"));
    QVERIFY(isCredentialTestTransaction(fields.at(2)));
    QCOMPARE(fields.at(3), QByteArrayLiteral("-"));
    for (const QByteArray &privateValue :
         {secret.toUtf8(), scope.toUtf8(),
          vaultKey.toUtf8(),
          QStringLiteral(GC_STRAVA_TOKEN).toUtf8(),
          QFile::encodeName(settingsPath)}) {
        QVERIFY(!cleanupMetadata.contains(privateValue));
    }
}

void TestCredentialSettings::
plaintextCleanupDurabilityFailureRetainsSource_data()
{
    QTest::addColumn<QByteArray>("failureStage");
    QTest::newRow("cleanup-file")
        << QByteArrayLiteral("cleanup-file");
    QTest::newRow("cleanup-directory")
        << QByteArrayLiteral("cleanup-directory");
}

void TestCredentialSettings::
plaintextCleanupDurabilityFailureRetainsSource()
{
    QFETCH(QByteArray, failureStage);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    {
        ScopedEnvironmentVariable durabilityFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
            failureStage);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }
    QCOMPARE(state->reads, 0);
    QCOMPARE(settings.value(plaintextKey).toString(), secret);

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
}

void TestCredentialSettings::
explicitWriteAuthorizationFailureRetainsSource_data()
{
    QTest::addColumn<QByteArray>("failure");
    QTest::newRow("file")
        << QByteArrayLiteral("cleanup-file:3");
    QTest::newRow("directory")
        << QByteArrayLiteral("cleanup-directory:3");
}

void TestCredentialSettings::
explicitWriteAuthorizationFailureRetainsSource()
{
    QFETCH(QByteArray, failure);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldPlaintext =
        QStringLiteral("old-plaintext");
    const QString newSecret =
        QStringLiteral("new-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("active")));
    settings.setValue(plaintextKey, oldPlaintext);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-vault-secret"));
    CredentialSettings credentials(fakeStore(state));
    {
        ScopedEnvironmentVariable durabilityFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
            failure);
        QVERIFY(!credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, newSecret));
    }
    QCOMPARE(state->values.value(vaultKey), newSecret);
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        oldPlaintext);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("updating")));

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QVERIFY(!settings.contains(plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
malformedPlaintextCleanupStateFailsClosed_data()
{
    QTest::addColumn<QByteArray>("metadata");
    const QByteArray transaction(32, 'a');
    const QByteArray fingerprint(64, 'b');
    QTest::newRow("unstructured")
        << QByteArrayLiteral("malformed\n");
    QTest::newRow("unknown-version")
        << QByteArrayLiteral("v2 complete ")
               + transaction + QByteArrayLiteral(" -\n");
    QTest::newRow("unknown-phase")
        << QByteArrayLiteral("v1 pending ")
               + transaction + QByteArrayLiteral(" -\n");
    QTest::newRow("invalid-transaction")
        << QByteArrayLiteral("v1 complete short -\n");
    QTest::newRow("complete-with-fingerprint")
        << QByteArrayLiteral("v1 complete ")
               + transaction + QByteArrayLiteral(" ")
               + fingerprint + QByteArrayLiteral("\n");
    QTest::newRow("intent-without-fingerprint")
        << QByteArrayLiteral("v1 intent ")
               + transaction + QByteArrayLiteral(" -\n");
    QTest::newRow("authorized-without-fingerprint")
        << QByteArrayLiteral("v1 authorized ")
               + transaction + QByteArrayLiteral(" -\n");
    QTest::newRow("uppercase-fingerprint")
        << QByteArrayLiteral("v1 intent ")
               + transaction + QByteArrayLiteral(" ")
               + fingerprint.toUpper() + QByteArrayLiteral("\n");
    QTest::newRow("trailing-field")
        << QByteArrayLiteral("v1 complete ")
               + transaction
               + QByteArrayLiteral(" - trailing\n");
    QTest::newRow("oversized")
        << QByteArray(161, 'x');
}

void TestCredentialSettings::
malformedPlaintextCleanupStateFailsClosed()
{
    QFETCH(QByteArray, metadata);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    {
        CredentialSettings bootstrap(fakeStore(state));
        QCOMPARE(bootstrap.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    QVERIFY(!cleanupPath.isEmpty());
    QVERIFY(writePrivateStateFile(
        cleanupPath, metadata));

    state->values.insert(vaultKey, secret);
    const int readsBeforeFailure = state->reads;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, readsBeforeFailure);
    QCOMPARE(settings.value(plaintextKey).toString(), secret);

    QVERIFY(QFile::remove(cleanupPath));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
}

void TestCredentialSettings::
redirectedPlaintextCleanupStateFailsClosed()
{
#ifdef Q_OS_WIN
    QSKIP("Symbolic-link creation requires platform privileges");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    {
        CredentialSettings bootstrap(fakeStore(state));
        QCOMPARE(bootstrap.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    const QString outsidePath =
        temporary.filePath(QStringLiteral("outside-state"));
    QVERIFY(writePrivateStateFile(
        outsidePath, QByteArrayLiteral("outside-sentinel\n")));
    QVERIFY(QFile::link(outsidePath, cleanupPath));

    state->values.insert(vaultKey, secret);
    const int readsBeforeFailure = state->reads;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, readsBeforeFailure);
    QCOMPARE(settings.value(plaintextKey).toString(), secret);
    QCOMPARE(
        fileContents(outsidePath),
        QByteArrayLiteral("outside-sentinel\n"));

    QVERIFY(QFile::remove(cleanupPath));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
#endif
}

void TestCredentialSettings::
hardLinkedPlaintextCleanupStateFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Hard-link state validation requires Unix");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    {
        CredentialSettings bootstrap(fakeStore(state));
        QCOMPARE(bootstrap.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    const QString outsidePath =
        temporary.filePath(QStringLiteral("outside-state"));
    const QByteArray transaction =
        QUuid::createUuid().toRfc4122().toHex();
    const QByteArray outsideContents =
        QByteArrayLiteral("v1 complete ")
        + transaction + QByteArrayLiteral(" -\n");
    QVERIFY(writePrivateStateFile(
        outsidePath, outsideContents));
    const QByteArray encodedOutside =
        QFile::encodeName(outsidePath);
    const QByteArray encodedCleanup =
        QFile::encodeName(cleanupPath);
    QCOMPARE(
        ::link(encodedOutside.constData(),
               encodedCleanup.constData()),
        0);

    state->values.insert(vaultKey, secret);
    const int readsBeforeFailure = state->reads;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, readsBeforeFailure);
    QCOMPARE(settings.value(plaintextKey).toString(), secret);
    QCOMPARE(fileContents(outsidePath), outsideContents);

    QVERIFY(QFile::remove(cleanupPath));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
#endif
}

void TestCredentialSettings::
hardLinkedPlaintextSourceFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Hard-link source validation requires Unix");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString aliasPath =
        temporary.filePath(QStringLiteral("private-alias.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QByteArray encodedPath = QFile::encodeName(path);
    const QByteArray encodedAlias = QFile::encodeName(aliasPath);
    QCOMPARE(
        ::link(encodedPath.constData(), encodedAlias.constData()),
        0);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(settings.value(plaintextKey).toString(), secret);
    {
        QSettings alias(aliasPath, QSettings::IniFormat);
        QCOMPARE(alias.value(plaintextKey).toString(), secret);
    }

    QVERIFY(QFile::remove(aliasPath));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
#endif
}

void TestCredentialSettings::
explicitWriteRejectsRedirectedCleanupState()
{
#ifdef Q_OS_WIN
    QSKIP("Symbolic-link creation requires platform privileges");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    {
        CredentialSettings bootstrap(fakeStore(state));
        QCOMPARE(bootstrap.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
    }
    settings.setValue(plaintextKey, oldSecret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    const QString outsidePath =
        temporary.filePath(QStringLiteral("outside-state"));
    const QByteArray outsideContents =
        QByteArrayLiteral("outside-sentinel\n");
    QVERIFY(writePrivateStateFile(
        outsidePath, outsideContents));
    QVERIFY(QFile::link(outsidePath, cleanupPath));

    state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->values.value(vaultKey), oldSecret);
    QCOMPARE(settings.value(plaintextKey).toString(), oldSecret);
    QCOMPARE(fileContents(outsidePath), outsideContents);

    QVERIFY(QFile::remove(cleanupPath));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QCOMPARE(state->values.value(vaultKey), newSecret);
    QVERIFY(!settings.contains(plaintextKey));
#endif
}

void TestCredentialSettings::
explicitMutationRepairsMalformedCleanupState_data()
{
    QTest::addColumn<bool>("removeCredential");
    QTest::newRow("set") << false;
    QTest::newRow("delete") << true;
}

void TestCredentialSettings::
explicitMutationRepairsMalformedCleanupState()
{
    QFETCH(bool, removeCredential);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-secret");
    const QString newSecret = QStringLiteral("new-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    {
        CredentialSettings bootstrap(fakeStore(state));
        QCOMPARE(bootstrap.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(oldSecret));
    }
    settings.setValue(plaintextKey, oldSecret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    const QString cleanupPath = credentialCleanupFile(
        stateRoot, vaultKey, &settings, plaintextKey);
    QVERIFY(writePrivateStateFile(
        cleanupPath, QByteArrayLiteral("malformed\n")));

    CredentialSettings credentials(fakeStore(state));
    if (removeCredential) {
        QVERIFY(credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QVERIFY(!state->values.contains(vaultKey));
    } else {
        QVERIFY(credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey, newSecret));
        QCOMPARE(state->values.value(vaultKey), newSecret);
    }
    QVERIFY(!settings.contains(plaintextKey));
    const QList<QByteArray> fields =
        fileContents(cleanupPath).trimmed().split(' ');
    QCOMPARE(fields.size(), 4);
    QCOMPARE(fields.at(0), QByteArrayLiteral("v1"));
    QCOMPARE(fields.at(1), QByteArrayLiteral("complete"));
}

void TestCredentialSettings::
explicitWriteProtectsLaterPlaintext_data()
{
    QTest::addColumn<bool>("seedCompleteState");
    QTest::newRow("without-prior-cleanup") << false;
    QTest::newRow("after-prior-cleanup") << true;
}

void TestCredentialSettings::
explicitWriteProtectsLaterPlaintext()
{
    QFETCH(bool, seedCompleteState);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-secret");
    const QString explicitSecret =
        QStringLiteral("explicit-secret");
    const QString laterSecret =
        QStringLiteral("later-plaintext-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));

    if (seedCompleteState) {
        settings.setValue(plaintextKey, oldSecret);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        state->values.insert(vaultKey, oldSecret);
        QCOMPARE(credentials.value(
                     &settings, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(oldSecret));
        QVERIFY(!settings.contains(plaintextKey));
    }
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, explicitSecret));

    {
        QSettings external(path, QSettings::IniFormat);
        external.setValue(plaintextKey, laterSecret);
        external.sync();
        QCOMPARE(external.status(), QSettings::NoError);
    }
    CredentialSettings restarted(fakeStore(state));
    QSettings recovery(path, QSettings::IniFormat);
    QCOMPARE(restarted.value(
                 &recovery, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(explicitSecret));
    QCOMPARE(
        recovery.value(plaintextKey).toString(),
        laterSecret);
}

void TestCredentialSettings::
credentialStateDurabilityFailureFailsClosed_data()
{
    QTest::addColumn<QByteArray>("failureStage");
    QTest::newRow("file") << QByteArrayLiteral("file");
    QTest::newRow("directory")
        << QByteArrayLiteral("directory");
}

void TestCredentialSettings::
credentialStateDurabilityFailureFailsClosed()
{
    QFETCH(QByteArray, failureStage);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    {
        ScopedEnvironmentVariable durabilityFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
            failureStage);
        QVERIFY(!credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
    }
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(credentialPhaseIs(
        deletionPath,
        QByteArrayLiteral("preparing-creation")));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->removes, 0);

    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey,
        QStringLiteral("replacement-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 0);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
credentialPostMutationDurabilityFailure_data()
{
    QTest::addColumn<QByteArray>("operation");
    QTest::addColumn<QByteArray>("failure");
    QTest::addColumn<QByteArray>("expectedPhase");

    for (const QByteArray &stage :
         {QByteArrayLiteral("file"),
          QByteArrayLiteral("directory")}) {
        const auto row = [&stage](
            const char *name,
            const QByteArray &operation,
            int ordinal,
            const QByteArray &phase) {
            QTest::newRow(
                (QByteArray(name) + '-' + stage).constData())
                << operation
                << stage + ':' + QByteArray::number(ordinal)
                << phase;
        };
        row("delete-before-vault", "delete", 3, "deleting");
        row("delete-after-vault", "delete", 4, "deleted");
        row("update-final-revision", "update", 3, "updating");
        row("update-active", "update", 4, "active");
        row("replace-final-revision", "replace", 3, "replacing");
        row("replace-active", "replace", 4, "active");
        row("migration-revision", "migration", 2, "");
    }
}

void TestCredentialSettings::
credentialPostMutationDurabilityFailure()
{
    QFETCH(QByteArray, operation);
    QFETCH(QByteArray, failure);
    QFETCH(QByteArray, expectedPhase);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));

    if (operation == QByteArrayLiteral("migration")) {
        currentSource.setValue(
            plaintextKey, QStringLiteral("legacy-credential"));
        currentSource.sync();
        QCOMPARE(currentSource.status(), QSettings::NoError);
        {
            ScopedEnvironmentVariable durabilityFailure(
                QByteArrayLiteral(
                    "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
                failure);
            QCOMPARE(credentials.value(
                         &currentSource, scope,
                         GC_STRAVA_TOKEN, plaintextKey,
                         QStringLiteral("missing")),
                     QVariant(QStringLiteral(
                         "legacy-credential")));
        }
        QCOMPARE(state->writes, 0);
        QVERIFY(currentSource.contains(plaintextKey));
        QCOMPARE(credentials.value(
                     &currentSource, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("legacy-credential")));
        QCOMPARE(state->writes, 1);
        QVERIFY(!currentSource.contains(plaintextKey));
        return;
    }

    state->values.insert(
        vaultKey, QStringLiteral("old-credential"));
    if (operation == QByteArrayLiteral("update")
        || operation == QByteArrayLiteral("replace")) {
        QVERIFY(credentials.setValueChecked(
            &currentSource, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("active-credential")));
        if (operation == QByteArrayLiteral("replace")) {
            QVERIFY(credentials.removeChecked(
                &currentSource, scope, GC_STRAVA_TOKEN,
                plaintextKey));
            QVERIFY(credentialPhaseIs(
                deletionPath, QByteArrayLiteral("deleted")));
        }
    }
    duplicateSource.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    duplicateSource.sync();
    QCOMPARE(duplicateSource.status(), QSettings::NoError);

    bool operationResult = false;
    {
        ScopedEnvironmentVariable durabilityFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
            failure);
        if (operation == QByteArrayLiteral("delete")) {
            operationResult = credentials.removeChecked(
                &currentSource, scope, GC_STRAVA_TOKEN,
                plaintextKey);
        } else {
            operationResult = credentials.setValueChecked(
                &currentSource, scope, GC_STRAVA_TOKEN,
                plaintextKey, QStringLiteral("new-credential"));
        }
    }
    QVERIFY(!operationResult);
    QVERIFY(credentialPhaseIs(deletionPath, expectedPhase));

    if (operation == QByteArrayLiteral("delete")) {
        const bool vaultWasRemoved =
            expectedPhase == QByteArrayLiteral("deleted");
        QCOMPARE(state->values.contains(vaultKey),
                 !vaultWasRemoved);
        CredentialSettings restarted(fakeStore(state));
        QCOMPARE(restarted.value(
                     &duplicateSource, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(QStringLiteral("missing")));
        QVERIFY(!duplicateSource.contains(plaintextKey));
        QVERIFY(!state->values.contains(vaultKey));
        QVERIFY(credentialPhaseIs(
            deletionPath, QByteArrayLiteral("deleted")));
        return;
    }

    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("new-credential"));
    const int writesAfterOperation = state->writes;
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("new-credential")));
    QCOMPARE(state->writes, writesAfterOperation);
    QCOMPARE(
        duplicateSource.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
    if (expectedPhase == QByteArrayLiteral("active")) {
        QVERIFY(credentialPhaseIs(
            deletionPath, QByteArrayLiteral("active")));
    } else {
        QVERIFY(credentialPhaseIs(
            deletionPath, expectedPhase));
    }
}

void TestCredentialSettings::
credentialCrashRecoveryAcrossProcesses()
{
    const QString childAction =
        qEnvironmentVariable("GC_CREDENTIAL_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        const QString settingsPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_SETTINGS");
        const QString duplicatePath = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_DUPLICATE");
        const QString vaultPath = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_VAULT");
        const QString scope = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_SCOPE");
        const QString operation = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_OPERATION");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!duplicatePath.isEmpty());
        QVERIFY(!vaultPath.isEmpty());
        QVERIFY(!scope.isEmpty());
        QVERIFY(!operation.isEmpty());
        const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);

        if (childAction == QStringLiteral("crash")) {
            QSettings settings(
                settingsPath, QSettings::IniFormat);
            CredentialSettings credentials(
                std::make_unique<FileCredentialStore>(
                    vaultPath));
            if (operation == QStringLiteral("delete")) {
                credentials.removeChecked(
                    &settings, scope, GC_STRAVA_TOKEN,
                    plaintextKey);
            } else {
                credentials.setValueChecked(
                    &settings, scope, GC_STRAVA_TOKEN,
                    plaintextKey,
                    QStringLiteral("new-credential"));
            }
            QFAIL("Configured credential crash point was not reached");
        }

        QCOMPARE(childAction, QStringLiteral("recover"));
        QSettings duplicate(
            duplicatePath, QSettings::IniFormat);
        CredentialSettings credentials(
            std::make_unique<FileCredentialStore>(
                vaultPath));
        const QString expected = qEnvironmentVariable(
            "GC_CREDENTIAL_CRASH_EXPECTED");
        const QVariant observed = credentials.value(
            &duplicate, scope, GC_STRAVA_TOKEN,
            plaintextKey, QStringLiteral("missing"));
        QCOMPARE(observed, QVariant(expected));
        if (operation == QStringLiteral("delete")) {
            QVERIFY(credentials.removeChecked(
                &duplicate, scope, GC_STRAVA_TOKEN,
                plaintextKey));
            QCOMPARE(credentials.value(
                         &duplicate, scope,
                         GC_STRAVA_TOKEN, plaintextKey,
                         QStringLiteral("missing")),
                     QVariant(QStringLiteral("missing")));
        } else if (operation == QStringLiteral("replace")
                   && expected
                       == QStringLiteral("missing")) {
            QVERIFY(credentials.setValueChecked(
                &duplicate, scope, GC_STRAVA_TOKEN,
                plaintextKey,
                QStringLiteral("new-credential")));
        }
        if (qEnvironmentVariableIsSet(
                "GC_CREDENTIAL_CRASH_RETAIN_DUPLICATE")) {
            const QString duplicateValue =
                operation == QStringLiteral("create")
                ? QStringLiteral("legacy-credential")
                : QStringLiteral("stale-duplicate");
            QCOMPARE(
                duplicate.value(plaintextKey).toString(),
                duplicateValue);
        } else {
            QVERIFY(!duplicate.contains(plaintextKey));
        }
        return;
    }

    struct CrashCase {
        const char *name;
        const char *operation;
        const char *point;
        const char *expected;
        bool dropMarker;
    };
    const QList<CrashCase> cases = {
        {"delete-preparing", "delete",
         "deletion:preparing", "missing", false},
        {"delete-marker", "delete",
         "pending-removal", "missing", false},
        {"delete-lost-marker", "delete",
         "pending-removal", "missing", true},
        {"delete-deleting", "delete",
         "deletion:deleting", "missing", false},
        {"delete-vault", "delete",
         "vault:removed", "missing", false},
        {"delete-deleted", "delete",
         "deletion:deleted", "missing", false},
        {"create-state", "create",
         "deletion:creating", "legacy-credential", false},
        {"create-vault", "create",
         "vault:written", "new-credential", false},
        {"create-revision", "create",
         "write:final-revision", "new-credential", false},
        {"create-active", "create",
         "deletion:active", "new-credential", false},
        {"update-state", "update",
         "deletion:updating", "active-credential", false},
        {"update-vault", "update",
         "vault:written", "new-credential", false},
        {"update-revision", "update",
         "write:final-revision", "new-credential", false},
        {"update-active", "update",
         "deletion:active", "new-credential", false},
        {"replace-state", "replace",
         "deletion:replacing", "missing", false},
        {"replace-vault", "replace",
         "vault:written", "new-credential", false},
        {"replace-revision", "replace",
         "write:final-revision", "new-credential", false},
        {"replace-active", "replace",
         "deletion:active", "new-credential", false}
    };

    const auto runChild = [](
        const QProcessEnvironment &environment,
        int expectedExit,
        QByteArray *output) {
        QProcess child;
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "credentialCrashRecoveryAcrossProcesses")
        });
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        child.start();
        if (!child.waitForStarted(5000)) {
            if (output) *output = child.errorString().toUtf8();
            return false;
        }
        if (!child.waitForFinished(15000)) {
            child.kill();
            child.waitForFinished();
            if (output) {
                *output = child.readAll()
                    + child.errorString().toUtf8();
            }
            return false;
        }
        if (output) *output = child.readAll();
        return child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == expectedExit;
    };

    for (const CrashCase &testCase : cases) {
        QTemporaryDir temporary;
        QVERIFY2(temporary.isValid(), testCase.name);
        const QString stateRoot = temporary.filePath(
            QStringLiteral("credential-state"));
        const QString settingsPath = temporary.filePath(
            QStringLiteral("current.ini"));
        const QString duplicatePath = temporary.filePath(
            QStringLiteral("duplicate.ini"));
        const QString vaultPath = temporary.filePath(
            QStringLiteral("vault.ini"));
        const QString scope =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const QString plaintextKey =
            plainKey(GC_STRAVA_TOKEN);
        const QString vaultKey = CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
        {
            ScopedEnvironmentVariable stateRootEnvironment(
                QByteArrayLiteral(
                    "GC_CREDENTIAL_TEST_STATE_ROOT"),
                QFile::encodeName(stateRoot));
            QSettings current(
                settingsPath, QSettings::IniFormat);
            if (QByteArray(testCase.operation)
                == QByteArrayLiteral("create")) {
                current.setValue(
                    plaintextKey,
                    QStringLiteral("legacy-credential"));
                current.sync();
                QCOMPARE(
                    current.status(),
                    QSettings::NoError);
            } else {
                CredentialSettings bootstrap(
                    std::make_unique<FileCredentialStore>(
                        vaultPath));
                QVERIFY2(bootstrap.setValueChecked(
                             &current, scope,
                             GC_STRAVA_TOKEN,
                             plaintextKey,
                             QStringLiteral(
                                 "active-credential")),
                         testCase.name);
                if (QByteArray(testCase.operation)
                    == QByteArrayLiteral("replace")) {
                    QVERIFY2(bootstrap.removeChecked(
                                 &current, scope,
                                 GC_STRAVA_TOKEN,
                                 plaintextKey),
                             testCase.name);
                }
            }
        }
        {
            QSettings duplicate(
                duplicatePath, QSettings::IniFormat);
            duplicate.setValue(
                plaintextKey,
                QByteArray(testCase.operation)
                        == QByteArrayLiteral("create")
                    ? QStringLiteral("legacy-credential")
                    : QStringLiteral("stale-duplicate"));
            duplicate.sync();
            QCOMPARE(duplicate.status(), QSettings::NoError);
        }

        QProcessEnvironment crashEnvironment =
            QProcessEnvironment::systemEnvironment();
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
            stateRoot);
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_ACTION"),
            QStringLiteral("crash"));
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_SETTINGS"),
            settingsPath);
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_DUPLICATE"),
            duplicatePath);
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_VAULT"),
            vaultPath);
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_SCOPE"),
            scope);
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_OPERATION"),
            QString::fromLatin1(testCase.operation));
        crashEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_TEST_CRASH_POINT"),
            QString::fromLatin1(testCase.point));
        QByteArray childOutput;
        QVERIFY2(runChild(
                     crashEnvironment, 86, &childOutput),
                 childOutput.constData());

        if (testCase.dropMarker) {
            QSettings current(
                settingsPath, QSettings::IniFormat);
            current.remove(pendingRemovalTestKey(
                scope, GC_STRAVA_TOKEN));
            current.remove(pendingRemovalGenerationTestKey(
                scope, GC_STRAVA_TOKEN));
            current.sync();
            QCOMPARE(current.status(), QSettings::NoError);
        }

        QProcessEnvironment recoveryEnvironment =
            crashEnvironment;
        recoveryEnvironment.remove(
            QStringLiteral(
                "GC_CREDENTIAL_TEST_CRASH_POINT"));
        recoveryEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_ACTION"),
            QStringLiteral("recover"));
        recoveryEnvironment.insert(
            QStringLiteral("GC_CREDENTIAL_CRASH_EXPECTED"),
            QString::fromLatin1(testCase.expected));
        const QString duplicateValue =
            QByteArray(testCase.operation)
                    == QByteArrayLiteral("create")
                ? QStringLiteral("legacy-credential")
                : QStringLiteral("stale-duplicate");
        if (QString::fromLatin1(testCase.expected)
                != QStringLiteral("missing")
            && QString::fromLatin1(testCase.expected)
                != duplicateValue) {
            recoveryEnvironment.insert(
                QStringLiteral(
                    "GC_CREDENTIAL_CRASH_RETAIN_DUPLICATE"),
                QStringLiteral("1"));
        } else {
            recoveryEnvironment.remove(
                QStringLiteral(
                    "GC_CREDENTIAL_CRASH_RETAIN_DUPLICATE"));
        }
        childOutput.clear();
        QVERIFY2(runChild(
                     recoveryEnvironment, 0, &childOutput),
                 childOutput.constData());

        FileCredentialStore vault(vaultPath);
        const CredentialStore::ReadResult final =
            vault.read(vaultKey);
        if (QByteArray(testCase.operation)
            == QByteArrayLiteral("delete")) {
            QCOMPARE(int(final.status),
                     int(CredentialStore::Status::NotFound));
        } else {
            QCOMPARE(int(final.status),
                     int(CredentialStore::Status::Success));
            QString expectedFinal =
                QStringLiteral("new-credential");
            if (QByteArray(testCase.point)
                == QByteArrayLiteral(
                    "deletion:updating")) {
                expectedFinal =
                    QStringLiteral("active-credential");
            } else if (QByteArray(testCase.point)
                       == QByteArrayLiteral(
                           "deletion:creating")) {
                expectedFinal =
                    QStringLiteral("legacy-credential");
            }
            QCOMPARE(final.value, expectedFinal);
        }
    }
}

void TestCredentialSettings::
plaintextCleanupCrashRecoveryAcrossProcesses()
{
    const QString childAction = qEnvironmentVariable(
        "GC_PLAINTEXT_CLEANUP_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString settingsPath = qEnvironmentVariable(
            "GC_PLAINTEXT_CLEANUP_CRASH_SETTINGS");
        const QString vaultPath = qEnvironmentVariable(
            "GC_PLAINTEXT_CLEANUP_CRASH_VAULT");
        const QString scope = qEnvironmentVariable(
            "GC_PLAINTEXT_CLEANUP_CRASH_SCOPE");
        QVERIFY(!settingsPath.isEmpty());
        QVERIFY(!vaultPath.isEmpty());
        QVERIFY(!scope.isEmpty());

        QSettings settings(
            settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(
            std::make_unique<FileCredentialStore>(
                vaultPath));
        credentials.value(
            &settings, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN),
            QStringLiteral("missing"));
        QFAIL("Configured plaintext cleanup crash was not reached");
    }

    struct CrashCase {
        const char *name;
        const char *point;
        bool conflict;
    };
    const QList<CrashCase> cases = {
        {"intent", "cleanup:intent", false},
        {"source-removed", "cleanup:source-removed", false},
        {"complete", "cleanup:complete", false},
        {"conflict", "cleanup:conflict", true}
    };

    for (const CrashCase &testCase : cases) {
        QTemporaryDir temporary;
        QVERIFY2(temporary.isValid(), testCase.name);
        const QString stateRoot = temporary.filePath(
            QStringLiteral("credential-state"));
        ScopedEnvironmentVariable stateRootEnvironment(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_STATE_ROOT"),
            QFile::encodeName(stateRoot));
        const QString settingsPath = temporary.filePath(
            QStringLiteral("private.ini"));
        const QString vaultPath = temporary.filePath(
            QStringLiteral("vault.ini"));
        const QString scope =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const QString plaintextKey =
            plainKey(GC_STRAVA_TOKEN);
        const QString vaultKey = CredentialSettings::vaultKey(
            scope, GC_STRAVA_TOKEN);
        const QString vaultSecret =
            QStringLiteral("current-vault-secret");
        const QString sourceSecret = vaultSecret;
        const QString newerSecret =
            QStringLiteral("newer-source-secret");

        FileCredentialStore vault(vaultPath);
        QString error;
        QCOMPARE(
            vault.write(vaultKey, vaultSecret, &error),
            CredentialStore::Status::Success);
        {
            QSettings settings(
                settingsPath, QSettings::IniFormat);
            settings.setValue(plaintextKey, sourceSecret);
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
            if (testCase.conflict) {
                CredentialSettings bootstrap(
                    std::make_unique<FileCredentialStore>(
                        vaultPath));
                QCOMPARE(bootstrap.value(
                             &settings, scope,
                             GC_STRAVA_TOKEN, plaintextKey,
                             QStringLiteral("missing")),
                         QVariant(vaultSecret));
                QVERIFY(!settings.contains(plaintextKey));
                settings.setValue(plaintextKey, newerSecret);
                settings.sync();
                QCOMPARE(
                    settings.status(), QSettings::NoError);
            }
        }

        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral(
                "GC_CREDENTIAL_TEST_STATE_ROOT"),
            stateRoot);
        environment.insert(
            QStringLiteral(
                "GC_PLAINTEXT_CLEANUP_CRASH_ACTION"),
            QStringLiteral("crash"));
        environment.insert(
            QStringLiteral(
                "GC_PLAINTEXT_CLEANUP_CRASH_SETTINGS"),
            settingsPath);
        environment.insert(
            QStringLiteral(
                "GC_PLAINTEXT_CLEANUP_CRASH_VAULT"),
            vaultPath);
        environment.insert(
            QStringLiteral(
                "GC_PLAINTEXT_CLEANUP_CRASH_SCOPE"),
            scope);
        environment.insert(
            QStringLiteral(
                "GC_CREDENTIAL_TEST_CRASH_POINT"),
            QString::fromLatin1(testCase.point));

        QProcess child;
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "plaintextCleanupCrashRecoveryAcrossProcesses")
        });
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        child.start();
        QVERIFY2(child.waitForStarted(5000), testCase.name);
        if (!child.waitForFinished(15000)) {
            child.kill();
            child.waitForFinished();
            QFAIL(testCase.name);
        }
        const QByteArray childOutput = child.readAll();
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit
                && child.exitCode() == 86,
            childOutput.constData());

        QSettings recovery(
            settingsPath, QSettings::IniFormat);
        CredentialSettings recovered(
            std::make_unique<FileCredentialStore>(
                vaultPath));
        QCOMPARE(recovered.value(
                     &recovery, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(vaultSecret));
        if (!testCase.conflict) {
            QVERIFY(!recovery.contains(plaintextKey));
            const QString cleanupPath = credentialCleanupFile(
                stateRoot, vaultKey, &recovery, plaintextKey);
            const QList<QByteArray> cleanupFields =
                fileContents(cleanupPath).trimmed().split(' ');
            QCOMPARE(cleanupFields.size(), 4);
            QCOMPARE(
                cleanupFields.at(0),
                QByteArrayLiteral("v1"));
            QCOMPARE(
                cleanupFields.at(1),
                QByteArrayLiteral("complete"));
            continue;
        }

        QCOMPARE(
            recovery.value(plaintextKey).toString(),
            newerSecret);
        error.clear();
        QCOMPARE(
            vault.remove(vaultKey, &error),
            CredentialStore::Status::Success);
        CredentialSettings migrator(
            std::make_unique<FileCredentialStore>(
                vaultPath));
        QCOMPARE(migrator.value(
                     &recovery, scope, GC_STRAVA_TOKEN,
                     plaintextKey, QStringLiteral("missing")),
                 QVariant(newerSecret));
        QVERIFY(!recovery.contains(plaintextKey));
    }
}

void TestCredentialSettings::
canonicalCredentialStateAncestorIsAccepted()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix canonical path aliases are required");
#else
    QTemporaryDir stateTemporary;
    QTemporaryDir settingsTemporary;
    QVERIFY(stateTemporary.isValid());
    QVERIFY(settingsTemporary.isValid());

    const QString realParent = stateTemporary.filePath(
        QStringLiteral("real-parent"));
    const QString aliasParent = stateTemporary.filePath(
        QStringLiteral("alias-parent"));
    QVERIFY(QDir().mkpath(realParent));
    QVERIFY(QFile::link(realParent, aliasParent));
    QVERIFY(QFileInfo(aliasParent).isSymLink());

    const QString stateRoot = QDir(aliasParent).filePath(
        QStringLiteral("state-root"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        settingsTemporary.filePath(
            QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("canonical-secret");

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, secret));
    QCOMPARE(state->values.value(vaultKey), secret);
    QVERIFY(QFileInfo(QDir(realParent).filePath(
        QStringLiteral(
            "state-root/GoldenCheetah/credential-locks")))
                .isDir());
#endif
}

void TestCredentialSettings::
credentialStateAncestorSyncFailureFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("missing/state-root"));
    QVERIFY(!QFileInfo::exists(stateRoot));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    {
        ScopedEnvironmentVariable durabilityFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_DURABILITY_FAILURE"),
            QByteArrayLiteral("ancestor"));
        QVERIFY(!credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QVERIFY(!credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
    }
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));

    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
}

void TestCredentialSettings::
partialCredentialStateAncestryFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    QTemporaryDir stateTemporary;
    QTemporaryDir settingsTemporary;
    QVERIFY(stateTemporary.isValid());
    QVERIFY(settingsTemporary.isValid());
    const QString partial =
        stateTemporary.filePath(QStringLiteral("partial"));
    QVERIFY(QDir().mkpath(partial));
    const QString stateRoot =
        QDir(partial).filePath(
            QStringLiteral("missing/state-root"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        settingsTemporary.filePath(
            QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(QFile::setPermissions(
        stateTemporary.path(),
        QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    const bool removed = credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey);
    QVERIFY(QFile::setPermissions(
        stateTemporary.path(),
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));

    QVERIFY(!removed);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
#endif
}

void TestCredentialSettings::
unsafeCredentialStateAncestorFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    QTemporaryDir stateTemporary;
    QTemporaryDir settingsTemporary;
    QVERIFY(stateTemporary.isValid());
    QVERIFY(settingsTemporary.isValid());
    const QString unsafeParent =
        stateTemporary.filePath(QStringLiteral("unsafe-parent"));
    const QString stateRoot =
        QDir(unsafeParent).filePath(
            QStringLiteral("owner-only-root"));
    QVERIFY(QDir().mkpath(stateRoot));
    QVERIFY(QFile::setPermissions(
        stateRoot,
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(
        unsafeParent,
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner
            | QFileDevice::WriteOther
            | QFileDevice::ExeOther));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        settingsTemporary.filePath(
            QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    const bool removed = credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey);
    QVERIFY(QFile::setPermissions(
        unsafeParent,
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));

    QVERIFY(!removed);
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
#endif
}

void TestCredentialSettings::
insecureCredentialStateRootFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions are required");
#else
    QTemporaryDir stateTemporary;
    QTemporaryDir settingsTemporary;
    QVERIFY(stateTemporary.isValid());
    QVERIFY(settingsTemporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateTemporary.path()));
    QSettings settings(
        settingsTemporary.filePath(
            QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(QFile::setPermissions(
        stateTemporary.path(),
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner
            | QFileDevice::WriteGroup));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QVERIFY(QFile::setPermissions(
        stateTemporary.path(),
        QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner));

    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
#endif
}

void TestCredentialSettings::
symlinkedCredentialStateDirectoryFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix symbolic links are required");
#else
    QTemporaryDir stateTemporary;
    QTemporaryDir targetTemporary;
    QTemporaryDir settingsTemporary;
    QVERIFY(stateTemporary.isValid());
    QVERIFY(targetTemporary.isValid());
    QVERIFY(settingsTemporary.isValid());
    const QString applicationPath =
        stateTemporary.filePath(
            QStringLiteral("GoldenCheetah"));
    QVERIFY(QFile::link(
        targetTemporary.path(), applicationPath));
    QVERIFY(QFileInfo(applicationPath).isSymLink());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateTemporary.path()));
    QSettings settings(
        settingsTemporary.filePath(
            QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
#endif
}

void TestCredentialSettings::
windowsCredentialDirectoriesUseOwnerOnlyAcl()
{
#ifndef Q_OS_WIN
    QSKIP("Windows DACLs are required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    CredentialSettings credentials(
        std::make_unique<FileCredentialStore>(
            temporary.filePath(QStringLiteral("vault.ini"))));
    QVERIFY(credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plainKey(GC_STRAVA_TOKEN),
        QStringLiteral("credential")));

    const QString applicationPath =
        QDir(stateRoot).filePath(
            QStringLiteral("GoldenCheetah"));
    const QString lockPath =
        QDir(applicationPath).filePath(
            QStringLiteral("credential-locks"));
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(
        applicationPath));
    QVERIFY(windowsDirectoryHasOwnerOnlyAcl(lockPath));
#endif
}

void TestCredentialSettings::
windowsSettingsReplacementUsesOwnerOnlyAcl()
{
#ifndef Q_OS_WIN
    QSKIP("Windows DACLs are required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(temporary.filePath(
            QStringLiteral("credential-state"))));
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, secret);
    settings.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(setWindowsDirectoryAcl(path, true, false));
    QVERIFY(!windowsFileHasOwnerOnlyAcl(path));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));
    QCOMPARE(
        settings.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    QVERIFY(windowsFileHasOwnerOnlyAcl(path));
#endif
}

void TestCredentialSettings::
windowsCleanupMatchesIniKeysCaseInsensitively()
{
#ifndef Q_OS_WIN
    QSKIP("Windows INI key semantics are required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString differentlyCasedKey =
        plaintextKey.toUpper();
    QVERIFY(differentlyCasedKey != plaintextKey);
    const QString secret = QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(differentlyCasedKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(settings.contains(plaintextKey));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));

    settings.setValue(differentlyCasedKey, secret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 differentlyCasedKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(!settings.contains(plaintextKey));

    int cleanupFiles = 0;
    QDirIterator files(
        stateRoot,
        QStringList{QStringLiteral("*.cleanup")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        ++cleanupFiles;
    }
    QCOMPARE(cleanupFiles, 1);
#endif
}

void TestCredentialSettings::
windowsWritableCredentialRootFailsClosed()
{
#ifndef Q_OS_WIN
    QSKIP("Windows DACLs are required");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    QVERIFY(QDir().mkpath(stateRoot));
    QVERIFY(setWindowsDirectoryAcl(
        stateRoot, true));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));

    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(setWindowsDirectoryAcl(
        stateRoot, false));
#endif
}

void TestCredentialSettings::
windowsExistingWritableCredentialDirectoryFailsClosed_data()
{
#ifndef Q_OS_WIN
    QTest::addColumn<bool>("writableApplication");
    QTest::newRow("Windows DACLs unavailable") << false;
#else
    QTest::addColumn<bool>("writableApplication");
    QTest::newRow("application") << true;
    QTest::newRow("credential-locks") << false;
#endif
}

void TestCredentialSettings::
windowsExistingWritableCredentialDirectoryFailsClosed()
{
#ifndef Q_OS_WIN
    QSKIP("Windows DACLs are required");
#else
    QFETCH(bool, writableApplication);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    const QString applicationPath = QDir(stateRoot).filePath(
        QStringLiteral("GoldenCheetah"));
    const QString lockPath = QDir(applicationPath).filePath(
        QStringLiteral("credential-locks"));
    QVERIFY(QDir().mkpath(lockPath));
    QVERIFY(setWindowsDirectoryAcl(stateRoot, false));
    QVERIFY(setWindowsDirectoryAcl(
        applicationPath, writableApplication));
    QVERIFY(setWindowsDirectoryAcl(
        lockPath, !writableApplication));

    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));

    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(
        writableApplication
            ? applicationPath : lockPath));
#endif
}

void TestCredentialSettings::
windowsNonInheritableCredentialDirectoryFailsClosed_data()
{
    QTest::addColumn<bool>("targetApplication");
#ifndef Q_OS_WIN
    QTest::newRow("Windows DACLs unavailable") << false;
#else
    QTest::newRow("application") << true;
    QTest::newRow("credential-locks") << false;
#endif
}

void TestCredentialSettings::
windowsNonInheritableCredentialDirectoryFailsClosed()
{
#ifndef Q_OS_WIN
    QSKIP("Windows DACLs are required");
#else
    QFETCH(bool, targetApplication);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    const QString applicationPath = QDir(stateRoot).filePath(
        QStringLiteral("GoldenCheetah"));
    const QString lockPath = QDir(applicationPath).filePath(
        QStringLiteral("credential-locks"));
    QVERIFY(QDir().mkpath(lockPath));
    QVERIFY(setWindowsDirectoryAcl(stateRoot, false));
    QVERIFY(setWindowsDirectoryAcl(
        applicationPath, false, !targetApplication));
    QVERIFY(setWindowsDirectoryAcl(
        lockPath, false, targetApplication));

    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialSettings credentials(fakeStore(state));

    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("credential-to-preserve"));
    QVERIFY(!windowsDirectoryHasOwnerOnlyAcl(
        targetApplication
            ? applicationPath : lockPath));
#endif
}

void TestCredentialSettings::
reissuedDeleteResumesDurableTransaction()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral(
            "private.gc-credential-scrub"));
    QSettings settings(
        settingsPath, credentialScrubTestFormat());
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-delete"));
    state->failRemoves = true;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    const QByteArray transaction =
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("deleting"));
    QVERIFY(!transaction.isEmpty());
    QCOMPARE(state->removes, 1);

    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = removalKey;
    state->failRemoves = false;
    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("deleted")),
        transaction);
    QVERIFY(!settings.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
reportedMarkerFailureRecoversDurableMarker()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath =
        temporary.filePath(QStringLiteral("private.ini"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-delete"));
    {
        QSettings settings(
            settingsPath, QSettings::IniFormat);
        CredentialSettings credentials(fakeStore(state));
        ScopedEnvironmentVariable reportedFailure(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_PENDING_RESULT_FAILURE"),
            QByteArrayLiteral("1"));
        QVERIFY(!credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QCOMPARE(state->removes, 0);
        QVERIFY(settings.value(removalKey).toBool());
        QVERIFY(isCredentialTestTransaction(
            settings.value(generationKey)
                .toString().toLatin1()));
        QVERIFY(credentialPhaseIs(
            deletionPath,
            QByteArrayLiteral("preparing-creation")));
    }

    QSettings restartedSettings(
        settingsPath, QSettings::IniFormat);
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &restartedSettings, scope,
                 GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(!restartedSettings.contains(removalKey));
    QVERIFY(!restartedSettings.contains(generationKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("deleted")));
}

void TestCredentialSettings::
failedMarkerPersistenceLeavesPreparationState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString path =
        temporary.filePath(QStringLiteral(
            "private.gc-credential-scrub"));
    QSettings settings(path, credentialScrubTestFormat());
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-preserve"));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = removalKey;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("credential-to-preserve"));
    QVERIFY(credentialPhaseIs(
        deletionPath,
        QByteArrayLiteral("preparing-creation")));
    fault = {};
}

void TestCredentialSettings::
failedCreationDeletePreparationPreservesLegacyCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString settingsPath = temporary.filePath(
        QStringLiteral("private.gc-credential-scrub"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString legacy =
        QStringLiteral("legacy-before-failed-delete");

    auto state = std::make_shared<FakeStoreState>();
    {
        QSettings settings(
            settingsPath, credentialScrubTestFormat());
        settings.setValue(plaintextKey, legacy);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);

        CredentialScrubFaultState &fault =
            credentialScrubFaultState();
        fault = {};
        fault.enabled = true;
        fault.forbiddenKey = removalKey;
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(!credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QCOMPARE(state->removes, 0);
        fault = {};
    }
    {
        QSettings cleanup(
            settingsPath, credentialScrubTestFormat());
        cleanup.remove(removalKey);
        cleanup.remove(generationKey);
        cleanup.sync();
        QCOMPARE(cleanup.status(), QSettings::NoError);
        QCOMPARE(cleanup.value(plaintextKey).toString(), legacy);
    }

    state->failWrites = true;
    {
        QSettings settings(
            settingsPath, credentialScrubTestFormat());
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(!credentials.setValueChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey,
            QStringLiteral("failed-replacement")));
    }
    state->failWrites = false;

    QSettings restartedSettings(
        settingsPath, credentialScrubTestFormat());
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &restartedSettings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(legacy));
    QCOMPARE(state->values.value(vaultKey), legacy);
    QVERIFY(!restartedSettings.contains(plaintextKey));
}

void TestCredentialSettings::
failedCreationDeletePreparationFindsLegacyInOtherSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString deletionPath = temporary.filePath(
        QStringLiteral("deletion.gc-credential-scrub"));
    const QString legacyPath = temporary.filePath(
        QStringLiteral("legacy.gc-credential-scrub"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);
    const QString legacy =
        QStringLiteral("legacy-in-second-source");

    auto state = std::make_shared<FakeStoreState>();
    {
        QSettings legacySettings(
            legacyPath, credentialScrubTestFormat());
        legacySettings.setValue(plaintextKey, legacy);
        legacySettings.sync();
        QCOMPARE(legacySettings.status(), QSettings::NoError);

        QSettings deletionSettings(
            deletionPath, credentialScrubTestFormat());
        CredentialScrubFaultState &fault =
            credentialScrubFaultState();
        fault = {};
        fault.enabled = true;
        fault.forbiddenKey = removalKey;
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(!credentials.removeChecked(
            &deletionSettings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QCOMPARE(state->removes, 0);
        fault = {};
    }
    {
        QSettings cleanup(
            deletionPath, credentialScrubTestFormat());
        cleanup.remove(removalKey);
        cleanup.remove(generationKey);
        cleanup.sync();
        QCOMPARE(cleanup.status(), QSettings::NoError);
    }

    state->failWrites = true;
    {
        QSettings deletionSettings(
            deletionPath, credentialScrubTestFormat());
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(!credentials.setValueChecked(
            &deletionSettings, scope, GC_STRAVA_TOKEN,
            plaintextKey,
            QStringLiteral("failed-replacement")));
    }
    state->failWrites = false;

    QSettings restartedLegacy(
        legacyPath, credentialScrubTestFormat());
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &restartedLegacy, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(legacy));
    QCOMPARE(state->values.value(vaultKey), legacy);
    QVERIFY(!restartedLegacy.contains(plaintextKey));
}

void TestCredentialSettings::
failedActiveDeletePreparationBlocksDuplicateResurrection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    const QString deletionPath = temporary.filePath(
        QStringLiteral("deletion.gc-credential-scrub"));
    const QString duplicatePath = temporary.filePath(
        QStringLiteral("duplicate.gc-credential-scrub"));
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString generationKey =
        pendingRemovalGenerationTestKey(
            scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    {
        QSettings deletionSettings(
            deletionPath, credentialScrubTestFormat());
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(credentials.setValueChecked(
            &deletionSettings, scope, GC_STRAVA_TOKEN,
            plaintextKey,
            QStringLiteral("active-credential")));

        QSettings duplicateSettings(
            duplicatePath, credentialScrubTestFormat());
        duplicateSettings.setValue(
            plaintextKey, QStringLiteral("stale-duplicate"));
        duplicateSettings.sync();
        QCOMPARE(duplicateSettings.status(), QSettings::NoError);

        CredentialScrubFaultState &fault =
            credentialScrubFaultState();
        fault = {};
        fault.enabled = true;
        fault.forbiddenKey = removalKey;
        QVERIFY(!credentials.removeChecked(
            &deletionSettings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
        QCOMPARE(state->removes, 0);
        fault = {};
    }
    {
        QSettings cleanup(
            deletionPath, credentialScrubTestFormat());
        cleanup.remove(removalKey);
        cleanup.remove(generationKey);
        cleanup.sync();
        QCOMPARE(cleanup.status(), QSettings::NoError);
    }

    state->failWrites = true;
    {
        QSettings deletionSettings(
            deletionPath, credentialScrubTestFormat());
        CredentialSettings credentials(fakeStore(state));
        QVERIFY(!credentials.setValueChecked(
            &deletionSettings, scope, GC_STRAVA_TOKEN,
            plaintextKey,
            QStringLiteral("failed-replacement")));
    }
    state->failWrites = false;
    state->values.remove(vaultKey);

    QSettings restartedDuplicate(
        duplicatePath, credentialScrubTestFormat());
    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &restartedDuplicate, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(
        restartedDuplicate.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
}

void TestCredentialSettings::
deletionCommitFailureRetriesWithoutResurrection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings deletionSource(
        temporary.filePath(QStringLiteral("deletion.ini")),
        QSettings::IniFormat);
    QSettings duplicateSource(
        temporary.filePath(QStringLiteral("duplicate.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    const QString backupPath =
        deletionPath + QStringLiteral(".fault-backup");
    duplicateSource.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    duplicateSource.sync();
    QCOMPARE(duplicateSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("credential-to-delete"));
    bool faultInjected = false;
    state->beforeRemove = [&]() {
        if (faultInjected)
            return;
        faultInjected = true;
        QVERIFY(QFile::rename(deletionPath, backupPath));
        QVERIFY(QDir().mkdir(deletionPath));
    };
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &deletionSource, scope, GC_STRAVA_TOKEN,
        plaintextKey));
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(QFileInfo(deletionPath).isDir());
    QVERIFY(QDir(deletionPath).removeRecursively());
    QVERIFY(QFile::rename(backupPath, deletionPath));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("deleting")));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &duplicateSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("deleted")));
    QVERIFY(!duplicateSource.contains(plaintextKey));
}

void TestCredentialSettings::
replacementCommitFailureRecoversWithoutRewrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    const QString backupPath =
        deletionPath + QStringLiteral(".fault-backup");

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-credential"));
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    bool faultInjected = false;
    state->beforeWrite = [&]() {
        if (faultInjected)
            return;
        faultInjected = true;
        QVERIFY(QFile::rename(deletionPath, backupPath));
        QVERIFY(QDir().mkdir(deletionPath));
    };
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("replacement-credential")));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("replacement-credential"));
    QCOMPARE(state->writes, 1);
    QVERIFY(QFileInfo(deletionPath).isDir());
    QVERIFY(QDir(deletionPath).removeRecursively());
    QVERIFY(QFile::rename(backupPath, deletionPath));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("replacing")));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("replacement-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 1);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
finalRevisionFailureRecoversCommittedWrite_data()
{
    QTest::addColumn<bool>("replacement");
    QTest::newRow("ordinary-write") << false;
    QTest::newRow("replacement") << true;
}

void TestCredentialSettings::
finalRevisionFailureRecoversCommittedWrite()
{
    QFETCH(bool, replacement);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    const QString revisionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".revision"));
    const QString revisionBackup =
        revisionPath + QStringLiteral(".fault-backup");

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    if (replacement) {
        state->values.insert(
            vaultKey, QStringLiteral("old-credential"));
        QVERIFY(credentials.removeChecked(
            &settings, scope, GC_STRAVA_TOKEN,
            plaintextKey));
    }

    bool faultInjected = false;
    state->beforeWrite = [&]() {
        if (faultInjected)
            return;
        faultInjected = true;
        QVERIFY(QFile::rename(
            revisionPath, revisionBackup));
        QVERIFY(QDir().mkdir(revisionPath));
    };
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey,
        QStringLiteral("committed-credential")));
    QCOMPARE(
        state->values.value(vaultKey),
        QStringLiteral("committed-credential"));
    QCOMPARE(state->writes, 1);
    const QByteArray transaction =
        credentialPhaseTransaction(
            deletionPath,
            replacement
                ? QByteArrayLiteral("replacing")
                : QByteArrayLiteral("creating"));
    QVERIFY(!transaction.isEmpty());
    QVERIFY(QDir(revisionPath).removeRecursively());
    QVERIFY(QFile::rename(
        revisionBackup, revisionPath));

    CredentialSettings restarted(fakeStore(state));
    QCOMPARE(restarted.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(
                 QStringLiteral("committed-credential")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 1);
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("active")),
        transaction);
}

void TestCredentialSettings::
failedReplacementResultRecoversCommittedSecret()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("old-credential"));
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QVERIFY(credentials->removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    state->failWrites = true;
    state->commitFailedWrites = true;
    QVERIFY(!credentials->setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("committed-replacement")));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("committed-replacement"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("replacing")));
    QCOMPARE(credentials->value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-replacement")));
    QCOMPARE(state->reads, 0);

    credentials.reset();
    state->failWrites = false;
    state->commitFailedWrites = false;
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-replacement")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 1);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
}

void TestCredentialSettings::
failedOrdinaryWriteResultRecoversCommittedSecret()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings currentSource(
        temporary.filePath(QStringLiteral("current.ini")),
        QSettings::IniFormat);
    QSettings staleSource(
        temporary.filePath(QStringLiteral("stale.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    staleSource.setValue(removalKey, true);
    staleSource.sync();
    QCOMPARE(staleSource.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    state->commitFailedWrites = true;
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QVERIFY(!credentials->setValueChecked(
        &currentSource, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("committed-credential")));
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("committed-credential"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("creating")));

    credentials.reset();
    state->failWrites = false;
    state->commitFailedWrites = false;
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 &currentSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-credential")));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
    QCOMPARE(credentials->value(
                 &staleSource, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-credential")));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 0);
    QVERIFY(!staleSource.contains(removalKey));
}

void TestCredentialSettings::
transientReplacementReadKeepsDeletionState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("replacing")));
    settings.setValue(
        plaintextKey, QStringLiteral("stale-duplicate"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->failReads = true;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(
        settings.value(plaintextKey).toString(),
        QStringLiteral("stale-duplicate"));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("replacing")));

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("replacing")));
}

void TestCredentialSettings::
malformedDeletionStateFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings settings(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    const QString plaintext =
        QStringLiteral("plaintext-credential");
    settings.setValue(plaintextKey, plaintext);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(writePrivateStateFile(
        deletionPath, QByteArrayLiteral("unknown-state\n")));

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        vaultKey, QStringLiteral("vault-credential"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!credentials.setValueChecked(
        &settings, scope, GC_STRAVA_TOKEN,
        plaintextKey, QStringLiteral("replacement")));
    QVERIFY(!credentials.removeChecked(
        &settings, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QCOMPARE(settings.value(plaintextKey).toString(),
             plaintext);
    QCOMPARE(state->values.value(vaultKey),
             QStringLiteral("vault-credential"));

    const QByteArray transaction =
        QUuid::createUuid().toRfc4122().toHex();
    QVERIFY(writePrivateStateFile(
        deletionPath,
        QByteArrayLiteral("v1 active ") + transaction
            + QByteArrayLiteral(" \n")));
    CredentialSettings strictParser(fakeStore(state));
    QCOMPARE(strictParser.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(settings.value(plaintextKey).toString(),
             plaintext);
}

void TestCredentialSettings::failedDeleteIsRetriedWithoutCredentialResurrection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    auto ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        ini.get(), QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini->setValue(
        plaintextKey, QStringLiteral("legacy-duplicate"));
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("removed-secret"));
    state->failRemoves = true;
    {
        CredentialSettings currentSession(fakeStore(state));
        QVERIFY(!currentSession.removeChecked(
            ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    }
    QCOMPARE(state->removes, 1);
    QVERIFY(state->values.contains(vaultKey));

    ini.reset();
    {
        QSettings persisted(path, QSettings::IniFormat);
        QVERIFY(persisted.value(removalKey, false).toBool());
        QVERIFY(!persisted.contains(plaintextKey));
    }

    state->failRemoves = false;
    ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 ini.get(), scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));

    ini.reset();
    ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    CredentialSettings persistedSession(fakeStore(state));
    QCOMPARE(persistedSession.value(
                 ini.get(), scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!ini->contains(removalKey));
    QVERIFY(!ini->contains(plaintextKey));
    QVERIFY(!fileContents(path).contains("legacy-duplicate"));
}

void TestCredentialSettings::failedDeleteIsRetriedInSameSession()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("removed-secret"));
    state->failRemoves = true;
    CredentialSettings credentials(fakeStore(state));
    QVERIFY(!credentials.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(state->values.contains(vaultKey));

    state->failRemoves = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(!ini.value(
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN),
        false).toBool());
}

void TestCredentialSettings::
failedDeleteMarkerWriteDefersDeletionUntilDurable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    QSettings ini(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("credential-to-keep");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini.setValue(
        QStringLiteral("normal/value"), QStringLiteral("keep"));
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = removalKey;

    QVERIFY(!credentials.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey), secret);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
externalPendingRemovalOverridesFailedMarkerState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    QSettings ini(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("credential-to-remove");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings first(fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = removalKey;
    QVERIFY(!first.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);

    fault.enabled = false;
    state->failRemoves = true;
    CredentialSettings second(fakeStore(state));
    QVERIFY(!second.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(persisted.value(removalKey, false).toBool());

    state->failRemoves = false;
    QCOMPARE(first.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));
    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
externalReplacementClearsStaleRemovalState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-vault-secret");
    const QString newSecret = QStringLiteral("new-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    state->failRemoves = true;
    CredentialSettings first(fakeStore(state));
    QVERIFY(!first.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);

    state->failRemoves = false;
    CredentialSettings second(fakeStore(state));
    QVERIFY(second.setValueChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey, newSecret));
    QCOMPARE(state->removes, 2);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), newSecret);

    QCOMPARE(first.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(newSecret));
    QCOMPARE(state->removes, 2);
    QCOMPARE(state->values.value(vaultKey), newSecret);
}

void TestCredentialSettings::
failedReplacementAfterPendingRemovalDoesNotResurrect()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    auto ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-vault-secret");
    const QString newSecret = QStringLiteral("new-memory-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini->setValue(removalKey, true);
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    state->failWrites = true;
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));

    QVERIFY(!credentials->setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
        newSecret));
    QCOMPARE(state->removes, 1);
    QCOMPARE(state->writes, 1);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(newSecret));

    credentials.reset();
    ini.reset();
    state->failWrites = false;
    ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!ini->contains(removalKey));
}

void TestCredentialSettings::
pendingRemovalClearsMemoryOnlyCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    auto ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString memorySecret = QStringLiteral("memory-only-secret");
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QVERIFY(!credentials->setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
        memorySecret));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(memorySecret));

    state->failWrites = false;
    state->failRemoves = true;
    QVERIFY(!credentials->removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);

    state->failRemoves = false;
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!ini->value(removalKey, false).toBool());

    credentials.reset();
    ini.reset();
    ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    QVERIFY(!ini->contains(removalKey));
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
}

void TestCredentialSettings::
pendingRemovalDiscardsMemoryOnlyCredentialAfterRestart()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    auto ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString memorySecret = QStringLiteral("memory-only-secret");
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QVERIFY(!credentials->setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
        memorySecret));

    state->failWrites = false;
    state->failRemoves = true;
    QVERIFY(!credentials->removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);

    credentials.reset();
    ini.reset();
    {
        QSettings persisted(path, QSettings::IniFormat);
        QVERIFY(persisted.value(removalKey, false).toBool());
        QVERIFY(!persisted.contains(plaintextKey));
    }

    state->failRemoves = false;
    ini = std::make_unique<QSettings>(
        path, QSettings::IniFormat);
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);

    credentials.reset();
    ini.reset();
    QSettings persisted(path, QSettings::IniFormat);
    QVERIFY(!persisted.contains(removalKey));
    QVERIFY(!persisted.contains(plaintextKey));
}

void TestCredentialSettings::persistedCacheScrubsDuplicatePlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath = temporary.filePath(
        QStringLiteral("first.ini"));
    const QString secondPath = temporary.filePath(
        QStringLiteral("second.ini"));
    QSettings first(firstPath, QSettings::IniFormat);
    QSettings second(secondPath, QSettings::IniFormat);
    const QString scope = QStringLiteral("shared-scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString sentinel = QStringLiteral("duplicate-secret");
    first.setValue(plaintextKey, sentinel);
    second.setValue(plaintextKey, sentinel);
    first.sync();
    second.sync();

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &first, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(sentinel));
    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(!first.contains(plaintextKey));
    QVERIFY(!second.contains(plaintextKey));
    first.sync();
    second.sync();
    QVERIFY(!fileContents(firstPath).contains(sentinel.toUtf8()));
    QVERIFY(!fileContents(secondPath).contains(sentinel.toUtf8()));
}

void TestCredentialSettings::unpersistedCachePreservesDuplicatePlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings first(temporary.filePath(QStringLiteral("first.ini")),
                    QSettings::IniFormat);
    QSettings second(temporary.filePath(QStringLiteral("second.ini")),
                     QSettings::IniFormat);
    const QString scope = QStringLiteral("shared-scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString sentinel = QStringLiteral("duplicate-secret");
    first.setValue(plaintextKey, sentinel);
    second.setValue(plaintextKey, sentinel);
    first.sync();
    second.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &first, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(sentinel));
    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(first.contains(plaintextKey));
    QVERIFY(second.contains(plaintextKey));
}

void TestCredentialSettings::failedPlaintextScrubIsRetried_data()
{
    QTest::addColumn<bool>("preexistingVault");
    QTest::addColumn<bool>("newSession");

    QTest::newRow("vault-same-session") << true << false;
    QTest::newRow("vault-new-session") << true << true;
    QTest::newRow("migration-same-session") << false << false;
    QTest::newRow("migration-new-session") << false << true;
}

void TestCredentialSettings::failedPlaintextScrubIsRetried()
{
    QFETCH(bool, preexistingVault);
    QFETCH(bool, newSession);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    const QSettings::Format format = credentialScrubTestFormat();
    QVERIFY(format != QSettings::InvalidFormat);
    auto ini = std::make_unique<QSettings>(path, format);
    const QString scope = CredentialSettings::ensureScopeId(
        ini.get(), QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret = QStringLiteral("legacy-secret");
    const QString vaultSecret = legacySecret;
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini->setValue(plaintextKey, legacySecret);
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    if (preexistingVault) {
        state->values.insert(vaultKey, vaultSecret);
    }
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;

    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(fault.rejectedWrites >= 1);
    QCOMPARE(state->writes, preexistingVault ? 0 : 1);
    QCOMPARE(state->values.value(vaultKey), vaultSecret);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QCOMPARE(persisted.value(plaintextKey).toString(),
             legacySecret);

    if (newSession) {
        ini.reset();
        credentials = std::make_unique<CredentialSettings>(
            fakeStore(state));
        fault.enabled = false;
        ini = std::make_unique<QSettings>(path, format);
    } else {
        fault.enabled = false;
    }
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(vaultSecret));
    QCOMPARE(state->writes, preexistingVault ? 0 : 1);
    QCOMPARE(state->values.value(vaultKey), vaultSecret);

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    fault = {};
}

void TestCredentialSettings::failedCachedPlaintextScrubIsRetried()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath =
        temporary.filePath(QStringLiteral("first.ini"));
    const QString secondPath =
        temporary.filePath(QStringLiteral("second.gc-credential-scrub"));
    QSettings first(firstPath, QSettings::IniFormat);
    QSettings second(secondPath, credentialScrubTestFormat());
    const QString scope = QStringLiteral("shared-scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &first, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 1);

    second.setValue(plaintextKey, secret);
    second.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    second.sync();
    QCOMPARE(second.status(), QSettings::NoError);
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;

    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QVERIFY(fault.rejectedWrites >= 1);
    QCOMPARE(state->reads, 3);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->writes, 0);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(secondPath, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QCOMPARE(
        persisted.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    fault = {};
}

void TestCredentialSettings::
failedMigrationScrubInvalidatesNegativeCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings empty(
        temporary.filePath(QStringLiteral("empty.ini")),
        QSettings::IniFormat);
    const QString path =
        temporary.filePath(QStringLiteral("legacy.gc-credential-scrub"));
    QSettings legacy(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("migrated-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    legacy.setValue(plaintextKey, secret);
    legacy.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    legacy.sync();
    QCOMPARE(legacy.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &empty, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);

    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;
    QCOMPARE(credentials.value(
                 &legacy, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), secret);
    QVERIFY(fault.rejectedWrites >= 1);
    bool retainedReadable = false;
    const QSettings::SettingsMap retained =
        persistedSettingsMap(path, &retainedReadable);
    QVERIFY(retainedReadable);
    QCOMPARE(
        retained.value(plaintextKey).toString(),
        secret);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &legacy, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 6);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QCOMPARE(
        persisted.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    fault = {};
}

void TestCredentialSettings::
failedVaultReadScrubInvalidatesNegativeCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings empty(
        temporary.filePath(QStringLiteral("empty.ini")),
        QSettings::IniFormat);
    const QString path =
        temporary.filePath(QStringLiteral("duplicate.gc-credential-scrub"));
    QSettings duplicate(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    duplicate.setValue(plaintextKey, secret);
    duplicate.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    duplicate.sync();
    QCOMPARE(duplicate.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &empty, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);

    state->values.insert(vaultKey, secret);
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;
    QCOMPARE(credentials.value(
                 &duplicate, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 3);
    QVERIFY(fault.rejectedWrites >= 1);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &duplicate, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->writes, 0);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QCOMPARE(
        persisted.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    fault = {};
}

void TestCredentialSettings::
failedSetValuePlaintextScrubIsRetried()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    auto ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-plaintext");
    const QString newSecret = QStringLiteral("new-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini->setValue(plaintextKey, oldSecret);
    ini->setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;

    QVERIFY(!credentials->setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey, newSecret));
    QVERIFY(fault.rejectedWrites >= 1);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), newSecret);

    credentials.reset();
    ini.reset();
    fault.enabled = false;
    ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(newSecret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QCOMPARE(
        persisted.value(plaintextKey).toString(),
        oldSecret);

    QVERIFY(credentials->setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QCOMPARE(state->writes, 2);
    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QCOMPARE(
        persisted.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    fault = {};
}

void TestCredentialSettings::
failedSetValueScrubInvalidatesOldCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings cacheSource(
        temporary.filePath(QStringLiteral("cache.ini")),
        QSettings::IniFormat);
    const QString path =
        temporary.filePath(QStringLiteral("target.gc-credential-scrub"));
    QSettings target(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-vault-secret");
    const QString newSecret = QStringLiteral("new-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    target.setValue(
        plaintextKey, QStringLiteral("stale-plaintext"));
    target.setValue(
        QStringLiteral("normal/value"),
        QStringLiteral("keep"));
    target.sync();
    QCOMPARE(target.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &cacheSource, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(oldSecret));
    QCOMPARE(state->reads, 1);

    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;
    QVERIFY(!credentials.setValueChecked(
        &target, scope, GC_STRAVA_TOKEN, plaintextKey, newSecret));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), newSecret);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &target, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(newSecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QCOMPARE(
        persisted.value(plaintextKey).toString(),
        QStringLiteral("stale-plaintext"));

    QVERIFY(credentials.setValueChecked(
        &target, scope, GC_STRAVA_TOKEN,
        plaintextKey, newSecret));
    QCOMPARE(state->writes, 2);
    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QCOMPARE(
        persisted.value(
            QStringLiteral("normal/value")).toString(),
        QStringLiteral("keep"));
    fault = {};
}

void TestCredentialSettings::
failedRemovePlaintextScrubPreservesVault()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    auto ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("credential-to-remove");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini->setValue(plaintextKey, secret);
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = plaintextKey;

    QVERIFY(!credentials->removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey), secret);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QCOMPARE(persisted.value(plaintextKey).toString(), secret);
    QVERIFY(persisted.value(removalKey, false).toBool());

    credentials.reset();
    ini.reset();
    fault.enabled = false;
    ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
pendingRemovalPersistenceFailurePreservesVault()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    auto ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("credential-to-remove");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini->setValue(
        QStringLiteral("normal/value"), QStringLiteral("keep"));
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = removalKey;

    QVERIFY(!credentials.removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 0);
    QCOMPARE(state->values.value(vaultKey), secret);

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));

    ini.reset();
    fault.enabled = false;
    ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    QVERIFY(credentials.removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
pendingRemovalCleanupFailureIsRetried()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    auto ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("credential-to-remove"));
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = removalKey;

    QVERIFY(!credentials->removeChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(persisted.value(removalKey, false).toBool());

    credentials.reset();
    ini.reset();
    fault.enabled = false;
    ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
pendingRemovalCleanupRetriesInSameSettingsSession()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    QSettings ini(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("credential-to-remove"));
    CredentialSettings credentials(fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = removalKey;

    QVERIFY(!credentials.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(persisted.value(removalKey, false).toBool());

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 1);

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::
credentialOperationsRecoverFromStickyCallerStatus()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    QSettings ini(path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString poisonKey = QStringLiteral("test/poison");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini.setValue(
        plaintextKey, QStringLiteral("stale-plaintext"));
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.forbiddenKey = poisonKey;
    ini.setValue(poisonKey, true);
    ini.sync();
    QVERIFY(ini.status() != QSettings::NoError);

    fault.enabled = false;
    ini.remove(poisonKey);
    ini.sync();
    QVERIFY(ini.status() != QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("vault-secret"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("vault-secret")));
    QCOMPARE(state->reads, 2);
    QVERIFY(credentials.removeChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey));
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
    QVERIFY(!persisted.contains(removalKey));
    QVERIFY(!persisted.contains(poisonKey));
    QVERIFY(ini.fallbacksEnabled());
    fault = {};
}

void TestCredentialSettings::
pendingRemovalMustClearBeforeCredentialWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.gc-credential-scrub"));
    auto ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString oldSecret = QStringLiteral("old-vault-secret");
    const QString newSecret = QStringLiteral("new-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    ini->setValue(removalKey, true);
    ini->sync();
    QCOMPARE(ini->status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, oldSecret);
    CredentialSettings credentials(fakeStore(state));
    CredentialScrubFaultState &fault =
        credentialScrubFaultState();
    fault = {};
    fault.enabled = true;
    fault.requiredKey = removalKey;

    QVERIFY(!credentials.setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey, newSecret));
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 1);
    QVERIFY(!state->values.contains(vaultKey));

    bool readable = false;
    QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(persisted.value(removalKey, false).toBool());

    ini.reset();
    fault.enabled = false;
    ini = std::make_unique<QSettings>(
        path, credentialScrubTestFormat());
    QVERIFY(credentials.setValueChecked(
        ini.get(), scope, GC_STRAVA_TOKEN, plaintextKey, newSecret));
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->removes, 1);
    QCOMPARE(state->values.value(vaultKey), newSecret);

    persisted = persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(removalKey));
    fault = {};
}

void TestCredentialSettings::systemFallbackCredentialIsIgnored()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString userPath =
        temporary.filePath(QStringLiteral("user"));
    const QString systemPath =
        temporary.filePath(QStringLiteral("system"));
    QVERIFY(QDir().mkpath(userPath));
    QVERIFY(QDir().mkpath(systemPath));
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, userPath);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::SystemScope, systemPath);

    const QString organization =
        QStringLiteral("CredentialFallback-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString fallbackSecret =
        QStringLiteral("system-fallback-secret");
    QSettings fallback(
        QSettings::IniFormat,
        QSettings::SystemScope,
        organization,
        application);
    fallback.setValue(plaintextKey, fallbackSecret);
    fallback.sync();
    QCOMPARE(fallback.status(), QSettings::NoError);

    QSettings user(
        QSettings::IniFormat,
        QSettings::UserScope,
        organization,
        application);
    QVERIFY(user.fallbacksEnabled());
    QVERIFY(user.contains(plaintextKey));
    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    const QString scope = QStringLiteral("scope");

    QCOMPARE(credentials.value(
                 &user, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->writes, 0);
    QVERIFY(!state->values.contains(CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN)));
    QVERIFY(user.fallbacksEnabled());
    QVERIFY(fallback.contains(plaintextKey));

    QSettings exactUser(user.fileName(), QSettings::IniFormat);
    QVERIFY(!exactUser.contains(plaintextKey));
}

void TestCredentialSettings::systemFallbackPendingRemovalIsIgnored()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString userPath =
        temporary.filePath(QStringLiteral("user"));
    const QString systemPath =
        temporary.filePath(QStringLiteral("system"));
    QVERIFY(QDir().mkpath(userPath));
    QVERIFY(QDir().mkpath(systemPath));
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, userPath);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::SystemScope, systemPath);

    const QString organization =
        QStringLiteral("CredentialRemovalFallback-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString scope = QStringLiteral("scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString removalKey =
        pendingRemovalTestKey(scope, GC_STRAVA_TOKEN);
    const QString secret = QStringLiteral("user-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    QSettings fallback(
        QSettings::IniFormat,
        QSettings::SystemScope,
        organization,
        application);
    fallback.setValue(removalKey, true);
    fallback.sync();
    QCOMPARE(fallback.status(), QSettings::NoError);

    QSettings user(
        QSettings::IniFormat,
        QSettings::UserScope,
        organization,
        application);
    QVERIFY(user.fallbacksEnabled());
    QVERIFY(user.value(removalKey, false).toBool());
    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, secret);
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &user, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->removes, 0);
    QVERIFY(user.fallbacksEnabled());
    QVERIFY(fallback.value(removalKey, false).toBool());

    QSettings exactUser(user.fileName(), QSettings::IniFormat);
    QVERIFY(!exactUser.contains(removalKey));
}

void TestCredentialSettings::systemFallbackScopeIdIsIgnored()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString userPath =
        temporary.filePath(QStringLiteral("user"));
    const QString systemPath =
        temporary.filePath(QStringLiteral("system"));
    QVERIFY(QDir().mkpath(userPath));
    QVERIFY(QDir().mkpath(systemPath));
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, userPath);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::SystemScope, systemPath);

    const QString organization =
        QStringLiteral("CredentialScopeFallback-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString storageKey =
        QStringLiteral("credential_store/id");
    const QString fallbackScope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings fallback(
        QSettings::IniFormat,
        QSettings::SystemScope,
        organization,
        application);
    fallback.setValue(storageKey, fallbackScope);
    fallback.sync();
    QCOMPARE(fallback.status(), QSettings::NoError);

    QSettings user(
        QSettings::IniFormat,
        QSettings::UserScope,
        organization,
        application);
    QCOMPARE(user.value(storageKey).toString(), fallbackScope);

    const QString userScope =
        CredentialSettings::ensureScopeId(&user, storageKey);
    QVERIFY(!userScope.isEmpty());
    QVERIFY(userScope != fallbackScope);
    QVERIFY(user.fallbacksEnabled());

    QSettings exactUser(user.fileName(), QSettings::IniFormat);
    QCOMPARE(exactUser.value(storageKey).toString(), userScope);
    QCOMPARE(fallback.value(storageKey).toString(), fallbackScope);
}

void TestCredentialSettings::negativeCacheDoesNotHideLegacyCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings empty(temporary.filePath(QStringLiteral("empty.ini")),
                    QSettings::IniFormat);
    QSettings legacy(temporary.filePath(QStringLiteral("legacy.ini")),
                     QSettings::IniFormat);
    const QString scope = QStringLiteral("shared-scope");
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString sentinel = QStringLiteral("late-legacy-secret");
    legacy.setValue(plaintextKey, sentinel);
    legacy.sync();

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &empty, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(credentials.value(
                 &legacy, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(sentinel));
    QVERIFY(!legacy.contains(plaintextKey));
    QCOMPARE(state->values.value(CredentialSettings::vaultKey(
                 scope, GC_STRAVA_TOKEN)), sentinel);
}

void TestCredentialSettings::emptyPlaintextDoesNotCacheTransientVaultFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, QString());
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        CredentialSettings::vaultKey(scope, GC_STRAVA_TOKEN),
        QStringLiteral("vault-secret"));
    state->failReads = true;
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!ini.contains(plaintextKey));

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("vault-secret")));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::transientReadFailureIsRetried()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    state->values.insert(vaultKey, QStringLiteral("vault-secret"));
    state->failReads = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("missing")),
             QVariant(QStringLiteral("vault-secret")));
    QCOMPARE(state->reads, 2);
}

void TestCredentialSettings::
transientReadDoesNotOverwriteNewerCredential_data()
{
    QTest::addColumn<int>("readStatus");

    QTest::newRow("unavailable")
        << int(CredentialStore::Status::Unavailable);
    QTest::newRow("failed")
        << int(CredentialStore::Status::Failed);
}

void TestCredentialSettings::
transientReadDoesNotOverwriteNewerCredential()
{
    QFETCH(int, readStatus);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret = QStringLiteral("stale-plaintext");
    const QString currentSecret = QStringLiteral("current-vault-secret");
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    state->values.insert(vaultKey, currentSecret);
    state->failReads = true;
    state->failedReadStatus =
        static_cast<CredentialStore::Status>(readStatus);
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QVERIFY(ini.contains(plaintextKey));
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
}

void TestCredentialSettings::
migrationDoesNotOverwriteCredentialCreatedAfterMiss()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret = QStringLiteral("stale-plaintext");
    const QString currentSecret = QStringLiteral("current-vault-secret");
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    bool published = false;
    state->beforeCreateIfAbsent = [&] {
        if (published)
            return;
        published = true;
        state->values.insert(vaultKey, currentSecret);
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
}

void TestCredentialSettings::
migrationCleanupRetainsChangedOrLastPlaintext_data()
{
    QTest::addColumn<bool>("replacePlaintext");
    QTest::addColumn<bool>("restartBeforeRecovery");
    QTest::addColumn<bool>("replaceDuringFinalConfirmation");

    QTest::newRow("changed-before-cleanup-same-process")
        << true << false << false;
    QTest::newRow("changed-before-cleanup-after-restart")
        << true << true << false;
    QTest::newRow("changed-during-confirmation-same-process")
        << true << false << true;
    QTest::newRow("changed-during-confirmation-after-restart")
        << true << true << true;
    QTest::newRow("last-copy-before-cleanup-same-process")
        << false << false << false;
    QTest::newRow("last-copy-before-cleanup-after-restart")
        << false << true << false;
}

void TestCredentialSettings::
migrationCleanupRetainsChangedOrLastPlaintext()
{
    QFETCH(bool, replacePlaintext);
    QFETCH(bool, restartBeforeRecovery);
    QFETCH(bool, replaceDuringFinalConfirmation);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString newerSecret =
        QStringLiteral("newer-plaintext-secret");
    const QString recoverableSecret =
        replacePlaintext ? newerSecret : legacySecret;
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    bool plaintextReplaced = false;
    bool replacementPersisted = false;
    if (replacePlaintext) {
        const auto persistReplacement = [&] {
            QSettings external(path, QSettings::IniFormat);
            external.setFallbacksEnabled(false);
            external.setAtomicSyncRequired(true);
            external.setValue(plaintextKey, newerSecret);
            external.sync();
            replacementPersisted =
                external.status() == QSettings::NoError
                && external.value(plaintextKey).toString()
                    == newerSecret;
        };
        state->beforeRead = [&, persistReplacement] {
            const int replacementRead =
                replaceDuringFinalConfirmation ? 3 : 2;
            if (plaintextReplaced
                || state->reads != replacementRead) {
                return;
            }
            plaintextReplaced = true;
            persistReplacement();
        };
    } else {
        state->beforeCreateIfAbsent = [&] {
            state->removeAfterReadKey = vaultKey;
        };
    }

    CredentialSettings credentials(fakeStore(state));
    credentials.value(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("missing"));
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    if (replacePlaintext) {
        QVERIFY(plaintextReplaced);
        QVERIFY(replacementPersisted);
        QCOMPARE(state->values.value(vaultKey), legacySecret);
    } else {
        QVERIFY(!state->values.contains(vaultKey));
    }

    QSettings retained(path, QSettings::IniFormat);
    retained.setFallbacksEnabled(false);
    retained.sync();
    QCOMPARE(retained.status(), QSettings::NoError);
    QVERIFY(retained.contains(plaintextKey));
    QCOMPARE(
        retained.value(plaintextKey).toString(),
        recoverableSecret);

    state->beforeRead = {};
    state->beforeCreateIfAbsent = {};
    state->removeAfterReadKey.clear();

    if (replacePlaintext) {
        if (restartBeforeRecovery) {
            CredentialSettings conflictReader(fakeStore(state));
            QSettings conflictSettings(path, QSettings::IniFormat);
            QCOMPARE(
                conflictReader.value(
                    &conflictSettings, scope, GC_STRAVA_TOKEN,
                    plaintextKey, QStringLiteral("missing")),
                QVariant(legacySecret));
        } else {
            QCOMPARE(
                credentials.value(
                    &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                    QStringLiteral("missing")),
                QVariant(legacySecret));
        }
        QSettings stillRetained(path, QSettings::IniFormat);
        stillRetained.setFallbacksEnabled(false);
        stillRetained.sync();
        QCOMPARE(stillRetained.status(), QSettings::NoError);
        QCOMPARE(
            stillRetained.value(plaintextKey).toString(),
            newerSecret);
    }

    state->values.remove(vaultKey);

    if (restartBeforeRecovery) {
        CredentialSettings restarted(fakeStore(state));
        QSettings afterRestart(path, QSettings::IniFormat);
        QCOMPARE(
            restarted.value(
                &afterRestart, scope, GC_STRAVA_TOKEN,
                plaintextKey, QStringLiteral("missing")),
            QVariant(recoverableSecret));
    } else {
        QCOMPARE(
            credentials.value(
                &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                QStringLiteral("missing")),
            QVariant(recoverableSecret));
    }

    QCOMPARE(state->values.value(vaultKey), recoverableSecret);
    QSettings cleaned(path, QSettings::IniFormat);
    cleaned.setFallbacksEnabled(false);
    cleaned.sync();
    QCOMPARE(cleaned.status(), QSettings::NoError);
    QVERIFY(!cleaned.contains(plaintextKey));
}

void TestCredentialSettings::
completedCleanupProtectsReappearedPlaintext_data()
{
    QTest::addColumn<bool>("restartAfterReappearance");
    QTest::addColumn<bool>("sameValue");
    QTest::newRow("different-same-process")
        << false << false;
    QTest::newRow("different-after-restart")
        << true << false;
    QTest::newRow("same-same-process")
        << false << true;
    QTest::newRow("same-after-restart")
        << true << true;
}

void TestCredentialSettings::
completedCleanupProtectsReappearedPlaintext()
{
    QFETCH(bool, restartAfterReappearance);
    QFETCH(bool, sameValue);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings settings(path, QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString vaultSecret =
        QStringLiteral("current-vault-secret");
    const QString newerSecret =
        QStringLiteral("newer-plaintext-secret");
    const QString reappearedSecret =
        sameValue ? vaultSecret : newerSecret;
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    settings.setValue(plaintextKey, vaultSecret);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, vaultSecret);
    auto credentials = std::make_unique<CredentialSettings>(
        fakeStore(state));
    QCOMPARE(credentials->value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    QVERIFY(!settings.contains(plaintextKey));

    {
        QSettings external(path, QSettings::IniFormat);
        external.setValue(plaintextKey, reappearedSecret);
        external.sync();
        QCOMPARE(external.status(), QSettings::NoError);
    }
    if (restartAfterReappearance) {
        credentials = std::make_unique<CredentialSettings>(
            fakeStore(state));
    }
    QSettings reappeared(path, QSettings::IniFormat);
    QCOMPARE(credentials->value(
                 &reappeared, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(vaultSecret));
    if (sameValue) {
        QVERIFY(!reappeared.contains(plaintextKey));
        return;
    }

    QSettings retained(path, QSettings::IniFormat);
    retained.sync();
    QCOMPARE(retained.status(), QSettings::NoError);
    QCOMPARE(retained.value(plaintextKey).toString(), newerSecret);

    state->values.remove(vaultKey);
    if (restartAfterReappearance) {
        credentials = std::make_unique<CredentialSettings>(
            fakeStore(state));
    }
    QSettings recovery(path, QSettings::IniFormat);
    QCOMPARE(credentials->value(
                 &recovery, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(newerSecret));
    QCOMPARE(state->values.value(vaultKey), newerSecret);

    QSettings cleaned(path, QSettings::IniFormat);
    cleaned.sync();
    QCOMPARE(cleaned.status(), QSettings::NoError);
    QVERIFY(!cleaned.contains(plaintextKey));
}

void TestCredentialSettings::
migrationCollisionReadFailureRetainsPlaintextAndRetries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path =
        temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret = QStringLiteral("stale-plaintext");
    const QString currentSecret = QStringLiteral("current-vault-secret");
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    bool published = false;
    state->beforeCreateIfAbsent = [&] {
        if (published)
            return;
        published = true;
        state->values.insert(vaultKey, currentSecret);
        state->failReads = true;
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QVERIFY(ini.contains(plaintextKey));
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
    QVERIFY(fileContents(path).contains(staleSecret.toUtf8()));

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
}

void TestCredentialSettings::
definiteMigrationOutcomeRequiresConfirmation_data()
{
    QTest::addColumn<bool>("collision");
    QTest::newRow("created-then-missing") << false;
    QTest::newRow("already-exists-then-missing") << true;
}

void TestCredentialSettings::
definiteMigrationOutcomeRequiresConfirmation()
{
    QFETCH(bool, collision);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString competingSecret =
        QStringLiteral("competing-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    if (collision) {
        state->beforeCreateIfAbsent = [
            state, vaultKey, competingSecret] {
                state->values.insert(
                    vaultKey, competingSecret);
            };
    }
    state->beforeRead = [state, vaultKey] {
        if (state->reads == 2)
            state->values.remove(vaultKey);
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(ini.value(plaintextKey).toString(),
             legacySecret);

    state->beforeRead = {};
    state->beforeCreateIfAbsent = {};
    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(legacySecret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->creates, 2);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey),
             legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
creatingMigrationDoesNotOverwriteCredentialCreatedAfterMiss()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret = QStringLiteral("stale-plaintext");
    const QString currentSecret = QStringLiteral("current-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("creating"),
        &transaction));
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    bool published = false;
    state->beforeCreateIfAbsent = [&] {
        if (published)
            return;
        published = true;
        state->values.insert(vaultKey, currentSecret);
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating")),
        transaction);
}

void TestCredentialSettings::
creatingMigrationCollisionReadFailureRetainsTransaction()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot =
        temporary.filePath(QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret =
        QStringLiteral("stale-plaintext");
    const QString currentSecret =
        QStringLiteral("current-vault-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("creating"),
        &transaction));
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    bool published = false;
    state->beforeCreateIfAbsent = [&] {
        if (published)
            return;
        published = true;
        state->values.insert(vaultKey, currentSecret);
        state->failReads = true;
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(ini.contains(plaintextKey));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating")),
        transaction);

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating")),
        transaction);
}

void TestCredentialSettings::
creatingMigrationUnsupportedRetainsTransactionAndRetries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stateRoot = temporary.filePath(
        QStringLiteral("credential-state"));
    ScopedEnvironmentVariable stateRootEnvironment(
        QByteArrayLiteral("GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    const QString deletionPath = credentialOperationFile(
        stateRoot, vaultKey, QStringLiteral(".deletion"));
    QByteArray transaction;
    QVERIFY(writeCredentialPhaseFile(
        deletionPath, QByteArrayLiteral("creating"),
        &transaction));
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    state->supportCreates = false;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(legacySecret));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(ini.value(plaintextKey).toString(),
             legacySecret);
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("creating")),
        transaction);

    state->supportCreates = true;
    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(legacySecret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->creates, 2);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey),
             legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
    QCOMPARE(
        credentialPhaseTransaction(
            deletionPath, QByteArrayLiteral("active")),
        transaction);
}

void TestCredentialSettings::
failedMigrationDoesNotCacheOverNewerCredential()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString staleSecret = QStringLiteral("stale-plaintext");
    const QString currentSecret = QStringLiteral("current-vault-secret");
    ini.setValue(plaintextKey, staleSecret);
    ini.sync();
    QCOMPARE(ini.status(), QSettings::NoError);

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    state->failWrites = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(staleSecret));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(ini.contains(plaintextKey));
    QCOMPARE(
        ini.value(plaintextKey).toString(),
        staleSecret);

    state->failWrites = false;
    state->values.insert(vaultKey, currentSecret);
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(currentSecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
    QCOMPARE(ini.value(plaintextKey).toString(), staleSecret);
}

void TestCredentialSettings::
transientMigrationCreateFailureRetriesInSameSession()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->failWrites = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(ini.contains(plaintextKey));

    state->failWrites = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->creates, 2);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
failedMigrationCreateRetainsPlaintextAndRetries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->failedCreates = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(legacySecret));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QCOMPARE(ini.value(plaintextKey).toString(),
             legacySecret);

    state->failedCreates = false;
    QCOMPARE(
        credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("missing")),
        QVariant(legacySecret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->creates, 2);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey),
             legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
unsupportedMigrationRetainsPlaintextAndRetries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->supportCreates = false;
    CredentialSettings credentials(fakeStore(state));

    for (int attempt = 1; attempt <= 2; ++attempt) {
        QCOMPARE(credentials.value(
                     &ini, scope, GC_STRAVA_TOKEN,
                     plaintextKey,
                     QStringLiteral("missing")),
                 QVariant(legacySecret));
        QCOMPARE(state->reads, attempt * 2);
        QCOMPARE(state->creates, attempt);
        QCOMPARE(state->overwrites, 0);
        QVERIFY(!state->values.contains(vaultKey));
        QCOMPARE(
            ini.value(plaintextKey).toString(),
            legacySecret);
    }

    state->supportCreates = true;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 7);
    QCOMPARE(state->creates, 3);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey),
             legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
indeterminateMigrationCommitIsConfirmed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString canonicalSecret =
        QStringLiteral("canonical-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->indeterminateCreates = true;
    state->commitIndeterminateCreates = true;
    state->indeterminateCommittedValue =
        canonicalSecret;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(canonicalSecret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), canonicalSecret);
    QCOMPARE(
        ini.value(plaintextKey).toString(),
        legacySecret);
}

void TestCredentialSettings::
indeterminateMigrationMissFailsClosedAndRetries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->indeterminateCreates = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!state->values.contains(vaultKey));
    QVERIFY(ini.contains(plaintextKey));

    state->indeterminateCreates = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 5);
    QCOMPARE(state->creates, 2);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
indeterminateMigrationReadFailureRetainsPlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(
        temporary.filePath(QStringLiteral("private.ini")),
        QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret =
        QStringLiteral("legacy-secret");
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->indeterminateCreates = true;
    state->commitIndeterminateCreates = true;
    state->beforeCreateIfAbsent = [&] {
        state->failReads = true;
    };
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QCOMPARE(state->values.value(vaultKey), legacySecret);
    QVERIFY(ini.contains(plaintextKey));

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->creates, 1);
    QCOMPARE(state->overwrites, 0);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::
transientReadBeforeMissingCredentialRetriesMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString plaintextKey = plainKey(GC_STRAVA_TOKEN);
    const QString legacySecret = QStringLiteral("legacy-secret");
    ini.setValue(plaintextKey, legacySecret);
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);
    state->failReads = true;
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QVERIFY(ini.contains(plaintextKey));

    state->failReads = false;
    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(legacySecret));
    QCOMPARE(state->reads, 4);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), legacySecret);
    QVERIFY(!ini.contains(plaintextKey));
}

void TestCredentialSettings::scopesAreIsolated()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings first(temporary.filePath(QStringLiteral("first.ini")),
                    QSettings::IniFormat);
    QSettings second(temporary.filePath(QStringLiteral("second.ini")),
                     QSettings::IniFormat);
    const QString firstScope = CredentialSettings::ensureScopeId(
        &first, QStringLiteral("credential_store/id"));
    const QString secondScope = CredentialSettings::ensureScopeId(
        &second, QStringLiteral("credential_store/id"));
    QVERIFY(firstScope != secondScope);

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    credentials.setValue(
        &first, firstScope, GC_XERT_TOKEN, plainKey(GC_XERT_TOKEN),
        QStringLiteral("first-secret"));
    credentials.setValue(
        &second, secondScope, GC_XERT_TOKEN, plainKey(GC_XERT_TOKEN),
        QStringLiteral("second-secret"));

    QCOMPARE(credentials.value(
                 &first, firstScope, GC_XERT_TOKEN,
                 plainKey(GC_XERT_TOKEN), QString()),
             QVariant(QStringLiteral("first-secret")));
    QCOMPARE(credentials.value(
                 &second, secondScope, GC_XERT_TOKEN,
                 plainKey(GC_XERT_TOKEN), QString()),
             QVariant(QStringLiteral("second-secret")));
}

void TestCredentialSettings::scopeIdentifiersAreStableAndValidated()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("settings.ini")),
                  QSettings::IniFormat);
    const QString storageKey = QStringLiteral("credential_store/id");
    ini.setValue(storageKey, QStringLiteral("../invalid"));

    const QString first = CredentialSettings::ensureScopeId(
        &ini, storageKey);
    const QString second = CredentialSettings::ensureScopeId(
        &ini, storageKey);
    QVERIFY(!QUuid(first).isNull());
    QCOMPARE(first, second);
    QCOMPARE(ini.value(storageKey).toString(), first);
}

void TestCredentialSettings::scopeCreationFailsClosedWhenItCannotPersist()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format format = QSettings::registerFormat(
        QStringLiteral("gc-reject-write"),
        readEmptySettings, rejectSettingsWrite);
    QVERIFY(format != QSettings::InvalidFormat);
    QSettings settings(
        temporary.filePath(QStringLiteral("settings.gc-reject-write")),
        format);

    QVERIFY(CredentialSettings::ensureScopeId(
                &settings,
                QStringLiteral("credential_store/id")).isEmpty());
    QVERIFY(CredentialSettings::ensureIdentityId(
                &settings,
                QStringLiteral(
                    "credential_store/root_id")).isEmpty());
    const CredentialSettings::ScopeBindingResult binding =
        CredentialSettings::ensureScopeBinding(
            &settings,
            QStringLiteral(
                "12121212-1212-4212-8212-121212121212"),
            QStringLiteral(
                "credential_store/binding_v2"),
            QStringLiteral(
                "credential_store/binding_scope"));
    QCOMPARE(
        binding.status,
        CredentialSettings::ScopeBindingStatus::Unavailable);
    QVERIFY(!settings.contains(
        QStringLiteral("credential_store/root_id")));
    QVERIFY(!settings.contains(
        QStringLiteral("credential_store/binding_v2")));
    QVERIFY(!settings.contains(
        QStringLiteral(
            "credential_store/binding_scope")));
    const QString claimDirectory =
        temporary.filePath(QStringLiteral("claim"));
    QVERIFY(QDir().mkpath(claimDirectory));
    QCOMPARE(
        CredentialSettings::ensureLocationClaim(
            &settings,
            QStringLiteral(
                "credential_store/location_claims/test"),
            QStringLiteral(
                "14141414-1414-4414-8414-141414141414"),
            QString(),
            claimDirectory),
        CredentialSettings::LocationClaimStatus::Unavailable);
    QVERIFY(!settings.contains(
        QStringLiteral(
            "credential_store/location_claims/test")));
    QCOMPARE(settings.status(), QSettings::NoError);
}

void TestCredentialSettings::migratePlaintextCoversConfiguredCredentials()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings ini(temporary.filePath(QStringLiteral("private.ini")),
                  QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QStringList keys = CredentialSettings::credentialKeysForPrefix(
        QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE));
    QVERIFY(!keys.isEmpty());
    for (const QString &key : keys) {
        ini.setValue(plainKey(key), QStringLiteral("secret-%1").arg(key));
    }
    ini.setValue(QStringLiteral("normal/value"), QStringLiteral("keep"));
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    CredentialSettings credentials(fakeStore(state));
    credentials.migratePlaintext(
        &ini, scope, QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE));

    QCOMPARE(state->values.size(), keys.size());
    for (const QString &key : keys) {
        QVERIFY(!ini.contains(plainKey(key)));
    }
    QCOMPARE(ini.value(QStringLiteral("normal/value")).toString(),
             QStringLiteral("keep"));
}

void TestCredentialSettings::gsettingsRoutesCredentialsToVault()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("legacy.ini"));
    {
        QSettings initial(path, QSettings::IniFormat);
        initial.setValue(QStringLiteral("Athlete/strava_token"),
                         QStringLiteral("legacy-athlete-secret"));
        initial.setValue(QStringLiteral("nolio_refresh_token"),
                         QStringLiteral("legacy-global-secret"));
        initial.setValue(QStringLiteral("Athlete/rwgps/user"),
                         QStringLiteral("ordinary-user"));
        initial.sync();
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(path, QSettings::IniFormat);
        QCOMPARE(settings.cvalue(
                     QStringLiteral("Athlete"), GC_STRAVA_TOKEN,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("legacy-athlete-secret"));
        QCOMPARE(settings.value(
                     nullptr, GC_NOLIO_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("legacy-global-secret"));

        settings.setCValue(
            QStringLiteral("Athlete"), GC_DVPASS,
            QStringLiteral("new-athlete-secret"));
        settings.setValue(
            GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("new-global-secret"));
        QCOMPARE(settings.cvalue(
                     QStringLiteral("Athlete"), GC_RWGPSUSER,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("ordinary-user"));
        settings.syncQSettings();
    }

    const QByteArray persisted = fileContents(path);
    QVERIFY(!persisted.contains("legacy-athlete-secret"));
    QVERIFY(!persisted.contains("legacy-global-secret"));
    QVERIFY(!persisted.contains("new-athlete-secret"));
    QVERIFY(!persisted.contains("new-global-secret"));
    QVERIFY(persisted.contains("ordinary-user"));
    QCOMPARE(factoryState()->values.size(), 4);
    verifyOwnerOnlyPermissions(path);
}

void TestCredentialSettings::
gsettingsCheckedCredentialWriteReportsPersistence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("legacy.ini"));
    const QString athlete = QStringLiteral("Athlete");
    const QString sentinel =
        QStringLiteral("checked-gsettings-refresh-token");

    factoryState() = std::make_shared<FakeStoreState>();
    factoryState()->failWrites = true;
    {
        GSettings settings(path, QSettings::IniFormat);
        QVERIFY(!settings.setCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN, sentinel));
        QCOMPARE(settings.cvalue(
                     athlete, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 sentinel);

        factoryState()->failWrites = false;
        QVERIFY(settings.setCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN, sentinel));
    }

    {
        GSettings nextSession(path, QSettings::IniFormat);
        QCOMPARE(nextSession.cvalue(
                     athlete, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 sentinel);
    }
}

void TestCredentialSettings::
gsettingsCheckedCredentialReadDistinguishesMissingAndFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("legacy.ini"));
    const QString athlete = QStringLiteral("Athlete");

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(path, QSettings::IniFormat);

    const GSettings::CredentialReadResult missing =
        settings.credentialCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN);
    QCOMPARE(
        missing.status,
        GSettings::CredentialReadStatus::NotFound);
    QVERIFY(!missing.value.isValid());

    QVERIFY(settings.setCValueChecked(
        athlete,
        GC_STRAVA_REFRESH_TOKEN,
        QStringLiteral("refresh-token")));
    const GSettings::CredentialReadResult present =
        settings.credentialCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN);
    QCOMPARE(
        present.status,
        GSettings::CredentialReadStatus::Present);
    QCOMPARE(present.value.toString(), QStringLiteral("refresh-token"));

    factoryState()->failReads = true;
    const GSettings::CredentialReadResult unavailable =
        settings.credentialCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN);
    QCOMPARE(
        unavailable.status,
        GSettings::CredentialReadStatus::Unavailable);
    QVERIFY(!unavailable.value.isValid());

    factoryState()->failReads = false;
    const GSettings::CredentialReadResult recovered =
        settings.credentialCValueChecked(
            athlete, GC_STRAVA_REFRESH_TOKEN);
    QCOMPARE(
        recovered.status,
        GSettings::CredentialReadStatus::Present);
    QCOMPARE(recovered.value.toString(), QStringLiteral("refresh-token"));
}

void TestCredentialSettings::
gsettingsSyncsOnlyRequestedAthleteFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());
    const QString organization =
        QStringLiteral("CredentialTargetedSync-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString first = QStringLiteral("First");
    const QString second = QStringLiteral("Second");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot
        + QStringLiteral("/First/config")));
    QVERIFY(QDir().mkpath(
        athleteRoot
        + QStringLiteral("/Second/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, first);
    settings.initializeQSettingsAthlete(
        athleteRoot, second);
    settings.setCValue(
        first,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("authorization_pending"));
    settings.setCValue(
        second,
        GC_STRAVA_AUTHORIZATION_STATE,
        QStringLiteral("active"));

    QVERIFY(settings.syncCValueChecked(
        first, GC_STRAVA_AUTHORIZATION_STATE));
    const QString firstPrivate = athleteRoot
        + QStringLiteral(
            "/First/config/athlete-private.ini");
    const QString secondPrivate = athleteRoot
        + QStringLiteral(
            "/Second/config/athlete-private.ini");
    QVERIFY(fileContents(firstPrivate).contains(
        "authorization_pending"));
    QVERIFY(!fileContents(secondPrivate).contains(
        "authorization_state"));
    QVERIFY(!settings.syncCValueChecked(
        QStringLiteral("Missing"),
        GC_STRAVA_AUTHORIZATION_STATE));
    QVERIFY(!settings.syncCValueChecked(
        first, GC_SETTINGS_MAIN_GEOM));
}

void TestCredentialSettings::
constructionDoesNotPersistMigrationState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("MigrationConstruction-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    QString targetPath;
    {
        QSettings target(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        targetPath = target.fileName();
    }
    QVERIFY(!QFileInfo::exists(targetPath));

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
    }

    QVERIFY(!QFileInfo::exists(targetPath));
}

void TestCredentialSettings::
systemFallbackDoesNotSuppressUserMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy-user"));
    const QString targetUserPath =
        temporary.filePath(QStringLiteral("target-user"));
    const QString targetSystemPath =
        temporary.filePath(QStringLiteral("target-system"));
    QVERIFY(QDir().mkpath(legacyPath));
    QVERIFY(QDir().mkpath(targetUserPath));
    QVERIFY(QDir().mkpath(targetSystemPath));

    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        legacyPath);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        targetUserPath);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::SystemScope,
        targetSystemPath);

    const QString organization =
        QStringLiteral("MigrationFallback-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString migratedKey =
        plainKey(GC_SETTINGS_LAST_IMPORT_PATH);
    const QString legacyValue =
        QStringLiteral("user-legacy-value");
    const QString fallbackValue =
        QStringLiteral("system-fallback-value");
    QString userFile;

    {
        QSettings legacy(
            QSettings::NativeFormat,
            QSettings::UserScope,
            organization,
            application);
        legacy.setValue(migratedKey, legacyValue);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings fallback(
            QSettings::IniFormat,
            QSettings::SystemScope,
            organization,
            application);
        fallback.setValue(migratedKey, fallbackValue);
        fallback.sync();
        QCOMPARE(fallback.status(), QSettings::NoError);
    }
    {
        QSettings target(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        userFile = target.fileName();
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        QVERIFY(settings.contains(
            GC_SETTINGS_LAST_IMPORT_PATH));
        QCOMPARE(
            settings.value(
                nullptr,
                GC_SETTINGS_LAST_IMPORT_PATH).toString(),
            fallbackValue);
        settings.migrateQSettingsSystem();
        QCOMPARE(
            settings.value(
                nullptr,
                GC_SETTINGS_LAST_IMPORT_PATH).toString(),
            legacyValue);
    }

    QSettings userTarget(userFile, QSettings::IniFormat);
    QCOMPARE(
        userTarget.value(migratedKey).toString(),
        legacyValue);
    QCOMPARE(
        userTarget.value(
            legacySystemMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::
credentialMetadataDoesNotSuppressSystemMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("SystemMigrationBootstrap-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(athleteRoot));
    const QString migratedKey =
        plainKey(GC_SETTINGS_LAST_IMPORT_PATH);
    const QString migratedValue =
        QStringLiteral("legacy-bootstrap-value");
    const QString startupFont =
        QStringLiteral("startup-font-write");
    QString systemPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(migratedKey, migratedValue);
        legacy.setValue(plainKey(GC_START_HTTP), true);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings target(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        target.setValue(
            QStringLiteral(
                "credential_store/test_metadata"),
            QStringLiteral(
                "66666666-6666-4666-8666-666666666666"));
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
        systemPath = target.fileName();
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.setValue(GC_FONT_DEFAULT, startupFont);
        settings.initializeQSettingsGlobal(athleteRoot);
        {
            QSettings metadata(systemPath, QSettings::IniFormat);
            const QStringList keys = metadata.allKeys();
            QVERIFY(std::any_of(
                keys.cbegin(),
                keys.cend(),
                [](const QString &key) {
                    return key.startsWith(
                        QStringLiteral("credential_store/"));
                }));
        }
        settings.migrateQSettingsSystem();
        QCOMPARE(
            settings.value(nullptr, GC_FONT_DEFAULT).toString(),
            startupFont);
        QCOMPARE(
            settings.value(nullptr, GC_START_HTTP).toBool(),
            true);
        QCOMPARE(
            settings.value(
                nullptr,
                GC_SETTINGS_LAST_IMPORT_PATH).toString(),
            migratedValue);
    }

    QSettings migrated(systemPath, QSettings::IniFormat);
    QCOMPARE(
        migrated.value(plainKey(GC_FONT_DEFAULT)).toString(),
        startupFont);
    QCOMPARE(migrated.value(migratedKey).toString(), migratedValue);
    QCOMPARE(
        migrated.value(legacySystemMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::
markerlessEstablishedSettingsAreAdoptedWithoutBackfill()
{
    using QSettings = LegacyMigrationQSettings;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("MigrationMarkerRollout-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString configPath = athleteRoot
        + QStringLiteral("/Athlete/config");
    QVERIFY(QDir().mkpath(configPath));

    const QString systemExistingKey = plainKey(GC_SETTINGS_LAST);
    const QString systemCompanionKey = plainKey(GC_START_HTTP);
    const QString systemMissingKey =
        plainKey(GC_SETTINGS_LAST_IMPORT_PATH);
    const QString globalExistingKey = plainKey(GC_TABBAR);
    const QString globalMissingKey = plainKey(GC_DEV_COUNT);
    const QString athleteExistingKey = plainKey(GC_VERSION_USED);
    const QString athleteMissingKey = plainKey(GC_NICKNAME);
    const QString systemExisting =
        QStringLiteral("established-system");
    const QString staleSystem =
        QStringLiteral("stale-system");
    const int staleGlobal = 4;
    const QString staleAthlete =
        QStringLiteral("stale-athlete");
    QString systemPath;

    {
        QSettings legacy(organization, application);
        legacy.setValue(systemMissingKey, staleSystem);
        legacy.setValue(globalMissingKey, staleGlobal);
        legacy.setValue(
            athleteName + QLatin1Char('/')
                + athleteMissingKey,
            staleAthlete);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(systemExistingKey, systemExisting);
        system.setValue(systemCompanionKey, false);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
        systemPath = system.fileName();
    }
    const QString globalPath = athleteRoot
        + QStringLiteral("/configglobal-general.ini");
    {
        QSettings global(globalPath, QSettings::IniFormat);
        global.setValue(globalExistingKey, false);
        global.sync();
        QCOMPARE(global.status(), QSettings::NoError);
    }
    const QString athletePath = configPath
        + QStringLiteral("/athlete-general.ini");
    {
        QSettings athlete(athletePath, QSettings::IniFormat);
        athlete.setValue(athleteExistingKey, QString());
        athlete.sync();
        QCOMPARE(athlete.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.migrateQSettingsSystem();
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
    }

    QSettings system(systemPath, QSettings::IniFormat);
    QVERIFY(system.contains(systemExistingKey));
    QCOMPARE(system.value(systemExistingKey).toString(), systemExisting);
    QCOMPARE(system.value(systemCompanionKey).toBool(), false);
    QVERIFY(!system.contains(systemMissingKey));
    QCOMPARE(
        system.value(legacySystemMigrationMarkerKey).toString(),
        legacyMigrationComplete);

    QSettings global(globalPath, QSettings::IniFormat);
    QVERIFY(global.contains(globalExistingKey));
    QCOMPARE(global.value(globalExistingKey).toBool(), false);
    QVERIFY(!global.contains(globalMissingKey));
    QCOMPARE(
        global.value(legacyGlobalMigrationMarkerKey).toString(),
        legacyMigrationComplete);

    QSettings athlete(athletePath, QSettings::IniFormat);
    QVERIFY(athlete.contains(athleteExistingKey));
    QCOMPARE(athlete.value(athleteExistingKey).toString(), QString());
    QVERIFY(!athlete.contains(athleteMissingKey));
    QCOMPARE(
        athlete.value(legacyAthleteMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::partialSystemMigrationResumesWithoutOverwrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("SystemMigrationRetry-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString existingKey = plainKey(GC_SETTINGS_LAST);
    const QString missingKey = plainKey(GC_SETTINGS_LAST_IMPORT_PATH);
    const QString existingValue = QStringLiteral("new-target-value");
    const QString staleValue = QStringLiteral("stale-legacy-value");
    const QString missingValue = QStringLiteral("legacy-missing-value");
    QString targetPath;

    {
        QSettings legacy(organization, application);
        legacy.setValue(existingKey, staleValue);
        legacy.setValue(missingKey, missingValue);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings target(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        target.setValue(existingKey, existingValue);
        target.setValue(
            legacySystemMigrationMarkerKey,
            legacyMigrationStarted);
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
        targetPath = target.fileName();
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.migrateQSettingsSystem();
        QCOMPARE(
            settings.value(nullptr, GC_SETTINGS_LAST).toString(),
            existingValue);
        QCOMPARE(
            settings.value(
                nullptr, GC_SETTINGS_LAST_IMPORT_PATH).toString(),
            missingValue);
    }

    QSettings migrated(targetPath, QSettings::IniFormat);
    QCOMPARE(migrated.value(existingKey).toString(), existingValue);
    QCOMPARE(migrated.value(missingKey).toString(), missingValue);
    QCOMPARE(
        migrated.value(legacySystemMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::
partialGlobalMigrationResumesWithoutOverwrite_data()
{
    QTest::addColumn<QString>("partialFile");
    QTest::newRow("general-target-written")
        << QStringLiteral("general");
    QTest::newRow("train-target-written")
        << QStringLiteral("train");
}

void TestCredentialSettings::
partialGlobalMigrationResumesWithoutOverwrite()
{
    QFETCH(QString, partialFile);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("GlobalMigrationRetry-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(athleteRoot));

    const QString generalKey = plainKey(GC_TABBAR);
    const QString trainKey = plainKey(GC_DEV_COUNT);
    const QString legacyGeneral =
        QStringLiteral("legacy-general-value");
    const int legacyTrain = 2;
    const QString targetGeneral =
        QStringLiteral("new-general-value");
    const int targetTrain = 0;
    {
        QSettings legacy(organization, application);
        legacy.setValue(generalKey, legacyGeneral);
        legacy.setValue(trainKey, legacyTrain);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    const QString generalPath = athleteRoot
        + QStringLiteral("/configglobal-general.ini");
    const QString trainPath = athleteRoot
        + QStringLiteral("/configglobal-trainmode.ini");
    {
        QSettings general(generalPath, QSettings::IniFormat);
        general.setValue(
            legacyGlobalMigrationMarkerKey,
            legacyMigrationStarted);
        if (partialFile == QStringLiteral("general")) {
            general.setValue(generalKey, targetGeneral);
        }
        general.sync();
        QCOMPARE(general.status(), QSettings::NoError);
    }
    if (partialFile == QStringLiteral("train")) {
        QSettings train(trainPath, QSettings::IniFormat);
        train.setValue(trainKey, targetTrain);
        train.sync();
        QCOMPARE(train.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(
            settings.value(nullptr, GC_TABBAR).toString(),
            partialFile == QStringLiteral("general")
                ? targetGeneral : legacyGeneral);
        QCOMPARE(
            settings.value(nullptr, GC_DEV_COUNT).toInt(),
            partialFile == QStringLiteral("train")
                ? targetTrain : legacyTrain);
    }

    QSettings general(generalPath, QSettings::IniFormat);
    QSettings train(trainPath, QSettings::IniFormat);
    QCOMPARE(
        general.value(generalKey).toString(),
        partialFile == QStringLiteral("general")
            ? targetGeneral : legacyGeneral);
    QCOMPARE(
        train.value(trainKey).toInt(),
        partialFile == QStringLiteral("train")
            ? targetTrain : legacyTrain);
    QVERIFY(train.contains(trainKey));
    QCOMPARE(
        general.value(legacyGlobalMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::
partialAthleteMigrationResumesWithoutOverwrite_data()
{
    QTest::addColumn<QString>("partialFile");
    QTest::newRow("general-target-written")
        << QStringLiteral("athlete-general.ini");
    QTest::newRow("layout-target-written")
        << QStringLiteral("athlete-layout.ini");
    QTest::newRow("preferences-target-written")
        << QStringLiteral("athlete-preferences.ini");
    QTest::newRow("private-target-written")
        << QStringLiteral("athlete-private.ini");
}

void TestCredentialSettings::
partialAthleteMigrationResumesWithoutOverwrite()
{
    QFETCH(QString, partialFile);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("AthleteMigrationRetry-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString configPath = athleteRoot
        + QStringLiteral("/Athlete/config");
    QVERIFY(QDir().mkpath(configPath));

    struct MigratedValue {
        QString fileName;
        QString settingKey;
        QString legacyValue;
        QString targetValue;
        QString prefixedKey;
    };
    const QList<MigratedValue> values = {
        {
            QStringLiteral("athlete-general.ini"),
            plainKey(GC_VERSION_USED),
            QStringLiteral("legacy-general"),
            QStringLiteral("new-general"),
            QStringLiteral(GC_VERSION_USED)
        },
        {
            QStringLiteral("athlete-layout.ini"),
            plainKey(GC_LTM_LAST_DATE_RANGE),
            QStringLiteral("legacy-layout"),
            QStringLiteral("new-layout"),
            QStringLiteral(GC_LTM_LAST_DATE_RANGE)
        },
        {
            QStringLiteral("athlete-preferences.ini"),
            plainKey(GC_NICKNAME),
            QStringLiteral("legacy-preferences"),
            QStringLiteral("new-preferences"),
            QStringLiteral(GC_NICKNAME)
        },
        {
            QStringLiteral("athlete-private.ini"),
            plainKey(GC_RWGPSUSER),
            QStringLiteral("legacy-private"),
            QStringLiteral("new-private"),
            QStringLiteral(GC_RWGPSUSER)
        }
    };

    {
        QSettings legacy(organization, application);
        for (const MigratedValue &value : values) {
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + value.settingKey,
                value.legacyValue);
        }
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings general(
            configPath + QStringLiteral("/athlete-general.ini"),
            QSettings::IniFormat);
        general.setValue(
            legacyAthleteMigrationMarkerKey,
            legacyMigrationStarted);
        general.sync();
        QCOMPARE(general.status(), QSettings::NoError);
    }
    {
        const auto selected = std::find_if(
            values.cbegin(),
            values.cend(),
            [&partialFile](const MigratedValue &value) {
                return value.fileName == partialFile;
            });
        QVERIFY(selected != values.cend());
        QSettings partial(
            configPath + QLatin1Char('/') + selected->fileName,
            QSettings::IniFormat);
        partial.setValue(
            selected->settingKey,
            selected->targetValue);
        partial.sync();
        QCOMPARE(partial.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        for (const MigratedValue &value : values) {
            QCOMPARE(
                settings.cvalue(
                    athleteName, value.prefixedKey).toString(),
                value.fileName == partialFile
                    ? value.targetValue : value.legacyValue);
        }
    }

    for (const MigratedValue &value : values) {
        QSettings migrated(
            configPath + QLatin1Char('/') + value.fileName,
            QSettings::IniFormat);
        QCOMPARE(
            migrated.value(value.settingKey).toString(),
            value.fileName == partialFile
                ? value.targetValue : value.legacyValue);
    }
    QSettings general(
        configPath + QStringLiteral("/athlete-general.ini"),
        QSettings::IniFormat);
    QCOMPARE(
        general.value(legacyAthleteMigrationMarkerKey).toString(),
        legacyMigrationComplete);
}

void TestCredentialSettings::
dynamicLegacyKeysMigrateToTheirExactTargets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("DynamicMigration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString configPath = athleteRoot
        + QStringLiteral("/Athlete/config");
    QVERIFY(QDir().mkpath(configPath));

    const QString dpKey = QStringLiteral("dp/custom/apply");
    const QString colmapKey = QStringLiteral("colmap/custom");
    const QString deviceNameKey =
        plainKey(GC_DEV_NAME) + QStringLiteral("1");
    const QString renamedKey =
        plainKey(GC_NAVHEADINGWIDTHS);
    const QString splitterSizeKey =
        plainKey(GC_SETTINGS_SPLITTER_SIZES);
    const QString splitterStateKey =
        QStringLiteral("splitter/analysis/hide");

    {
        QSettings legacy(organization, application);
        legacy.setValue(dpKey, QStringLiteral("dynamic-dp"));
        legacy.setValue(
            colmapKey, QStringLiteral("dynamic-column"));
        legacy.setValue(plainKey(GC_DEV_COUNT), 1);
        legacy.setValue(
            deviceNameKey, QStringLiteral("dynamic-trainer"));
        legacy.setValue(
            athleteName
                + QStringLiteral("/bavigator/headingwidths"),
            QStringLiteral("12,34"));
        legacy.setValue(
            athleteName + QLatin1Char('/') + splitterSizeKey,
            QByteArray("splitter-sizes"));
        legacy.setValue(
            athleteName + QLatin1Char('/') + splitterStateKey,
            false);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
    }

    QSettings globalGeneral(
        athleteRoot + QStringLiteral("/configglobal-general.ini"),
        QSettings::IniFormat);
    QCOMPARE(
        globalGeneral.value(dpKey).toString(),
        QStringLiteral("dynamic-dp"));
    QCOMPARE(
        globalGeneral.value(colmapKey).toString(),
        QStringLiteral("dynamic-column"));

    QSettings globalTrain(
        athleteRoot + QStringLiteral("/configglobal-trainmode.ini"),
        QSettings::IniFormat);
    QCOMPARE(globalTrain.value(plainKey(GC_DEV_COUNT)).toInt(), 1);
    QCOMPARE(
        globalTrain.value(deviceNameKey).toString(),
        QStringLiteral("dynamic-trainer"));

    QSettings athletePreferences(
        configPath + QStringLiteral("/athlete-preferences.ini"),
        QSettings::IniFormat);
    QCOMPARE(
        athletePreferences.value(renamedKey).toString(),
        QStringLiteral("12,34"));

    QSettings athleteLayout(
        configPath + QStringLiteral("/athlete-layout.ini"),
        QSettings::IniFormat);
    QCOMPARE(
        athleteLayout.value(splitterSizeKey).toByteArray(),
        QByteArray("splitter-sizes"));
    QVERIFY(athleteLayout.contains(splitterStateKey));
    QCOMPARE(
        athleteLayout.value(splitterStateKey).toBool(),
        false);
}

void TestCredentialSettings::unknownMigrationStateFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("UnknownMigrationState-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString migratedKey =
        plainKey(GC_SETTINGS_LAST_IMPORT_PATH);
    const QString unknownState = QStringLiteral("future-state");
    QString targetPath;

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            migratedKey, QStringLiteral("must-not-migrate"));
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings target(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        target.setValue(
            legacySystemMigrationMarkerKey, unknownState);
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
        targetPath = target.fileName();
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.migrateQSettingsSystem();
        QVERIFY(!settings.contains(
            GC_SETTINGS_LAST_IMPORT_PATH));
    }

    QSettings target(targetPath, QSettings::IniFormat);
    QCOMPARE(
        target.value(legacySystemMigrationMarkerKey).toString(),
        unknownState);
    QVERIFY(!target.contains(migratedKey));
}

void TestCredentialSettings::
migrationSyncFailuresResumeAfterRestart_data()
{
    QTest::addColumn<QString>("scope");
    QTest::addColumn<QString>("failurePoint");

    QTest::newRow("system-start")
        << QStringLiteral("system")
        << QStringLiteral("system-start");
    QTest::newRow("system-target")
        << QStringLiteral("system")
        << QStringLiteral("system-target");
    QTest::newRow("system-complete")
        << QStringLiteral("system")
        << QStringLiteral("system-complete");

    QTest::newRow("global-start")
        << QStringLiteral("global")
        << QStringLiteral("global-start");
    QTest::newRow("global-system")
        << QStringLiteral("global")
        << QStringLiteral("global-system");
    QTest::newRow("global-general")
        << QStringLiteral("global")
        << QStringLiteral("global-general");
    QTest::newRow("global-train")
        << QStringLiteral("global")
        << QStringLiteral("global-train");
    QTest::newRow("global-complete")
        << QStringLiteral("global")
        << QStringLiteral("global-complete");

    QTest::newRow("athlete-start")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-start");
    QTest::newRow("athlete-global")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-global");
    QTest::newRow("athlete-general")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-general");
    QTest::newRow("athlete-layout")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-layout");
    QTest::newRow("athlete-preferences")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-preferences");
    QTest::newRow("athlete-private")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-private");
    QTest::newRow("athlete-complete")
        << QStringLiteral("athlete")
        << QStringLiteral("athlete-complete");
}

void TestCredentialSettings::
migrationSyncFailuresResumeAfterRestart()
{
    QFETCH(QString, scope);
    QFETCH(QString, failurePoint);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    QSettings::setPath(
        legacyFormat,
        QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        targetFormat,
        QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("MigrationSyncRetry-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString configPath = athleteRoot
        + QStringLiteral("/Athlete/config");
    QVERIFY(QDir().mkpath(configPath));

    const QString systemValue =
        QStringLiteral("durable-system-value");
    const QString globalGeneralValue =
        QStringLiteral("durable-global-value");
    const int globalTrainValue = 2;
    const QString athleteGeneralValue =
        QStringLiteral("durable-athlete-general");
    const QString athleteLayoutValue =
        QStringLiteral("durable-athlete-layout");
    const QString athletePreferencesValue =
        QStringLiteral("durable-athlete-preferences");
    const QString athletePrivateValue =
        QStringLiteral("durable-athlete-private");

    {
        QSettings legacy(
            legacyFormat,
            QSettings::UserScope,
            organization,
            application);
        if (scope == QStringLiteral("system")) {
            legacy.setValue(
                plainKey(GC_SETTINGS_LAST_IMPORT_PATH),
                systemValue);
        } else if (scope == QStringLiteral("global")) {
            legacy.setValue(plainKey(GC_START_HTTP), true);
            legacy.setValue(
                plainKey(GC_TABBAR),
                globalGeneralValue);
            legacy.setValue(
                plainKey(GC_DEV_COUNT),
                globalTrainValue);
        } else {
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + plainKey(GC_UNIT),
                QStringLiteral(GC_UNIT_METRIC));
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + plainKey(GC_VERSION_USED),
                athleteGeneralValue);
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + plainKey(GC_LTM_LAST_DATE_RANGE),
                athleteLayoutValue);
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + plainKey(GC_NICKNAME),
                athletePreferencesValue);
            legacy.setValue(
                athleteName + QLatin1Char('/')
                    + plainKey(GC_RWGPSUSER),
                athletePrivateValue);
        }
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    QString systemPath;
    {
        QSettings system(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        systemPath = system.fileName();
        if (scope == QStringLiteral("global")) {
            system.setValue(
                legacySystemMigrationMarkerKey,
                legacyMigrationComplete);
            system.sync();
            QCOMPARE(system.status(), QSettings::NoError);
        }
    }
    const QString globalGeneralPath = athleteRoot
        + QStringLiteral("/configglobal-general.ini");
    const QString globalTrainPath = athleteRoot
        + QStringLiteral("/configglobal-trainmode.ini");
    const QString athleteGeneralPath = configPath
        + QStringLiteral("/athlete-general.ini");
    const QString athleteLayoutPath = configPath
        + QStringLiteral("/athlete-layout.ini");
    const QString athletePreferencesPath = configPath
        + QStringLiteral("/athlete-preferences.ini");
    const QString athletePrivatePath = configPath
        + QStringLiteral("/athlete-private.ini");
    if (scope == QStringLiteral("athlete")) {
        QSettings global(
            globalGeneralPath,
            targetFormat);
        global.setValue(
            legacyGlobalMigrationMarkerKey,
            legacyMigrationComplete);
        global.sync();
        QCOMPARE(global.status(), QSettings::NoError);
    }

    MigrationFormatFaultState &fault =
        migrationFormatFaultState();
    fault = {};
    fault.failurePoint = failurePoint;
    fault.enabled =
        scope == QStringLiteral("system")
        && failurePoint == QStringLiteral("system-start");
    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(
            organization,
            application,
            legacyFormat,
            targetFormat);
        if (scope == QStringLiteral("athlete")) {
            settings.initializeQSettingsGlobal(athleteRoot);
        }
        fault.enabled = true;
        if (scope == QStringLiteral("system")) {
            settings.migrateQSettingsSystem();
        } else if (scope == QStringLiteral("global")) {
            settings.initializeQSettingsGlobal(athleteRoot);
        } else {
            settings.initializeQSettingsAthlete(
                athleteRoot,
                athleteName);
        }
        if (failurePoint.endsWith(
                QStringLiteral("-start"))) {
            fault.enabled = false;
            if (scope == QStringLiteral("system")) {
                settings.setValue(
                    GC_FONT_DEFAULT,
                    QStringLiteral("post-failure-system-write"));
            } else if (scope == QStringLiteral("global")) {
                settings.setValue(
                    GC_WARNEXIT,
                    false);
            } else {
                settings.setCValue(
                    athleteName,
                    GC_DOB,
                    QDate(2000, 1, 1));
            }
            settings.syncQSettings();
        }
    }
    fault.enabled = false;
    QVERIFY2(
        fault.rejectedWrites > 0,
        qPrintable(QStringLiteral(
            "The injected migration write point was not reached: %1")
                       .arg(failurePoint)));

    QString markerPath;
    QString markerKey;
    if (scope == QStringLiteral("system")) {
        markerPath = systemPath;
        markerKey = legacySystemMigrationMarkerKey;
    } else if (scope == QStringLiteral("global")) {
        markerPath = globalGeneralPath;
        markerKey = legacyGlobalMigrationMarkerKey;
    } else {
        markerPath = athleteGeneralPath;
        markerKey = legacyAthleteMigrationMarkerKey;
    }
    {
        QSettings incomplete(markerPath, targetFormat);
        QVERIFY(
            incomplete.value(markerKey).toString()
            != legacyMigrationComplete);
    }

    {
        GSettings settings(
            organization,
            application,
            legacyFormat,
            targetFormat);
        if (scope == QStringLiteral("system")) {
            settings.migrateQSettingsSystem();
            QVERIFY(settings.contains(
                GC_SETTINGS_LAST_IMPORT_PATH));
            QCOMPARE(
                settings.value(
                    nullptr,
                    GC_SETTINGS_LAST_IMPORT_PATH).toString(),
                systemValue);
        } else if (scope == QStringLiteral("global")) {
            settings.initializeQSettingsGlobal(athleteRoot);
            QVERIFY(settings.contains(GC_START_HTTP));
            QCOMPARE(
                settings.value(
                    nullptr,
                    GC_START_HTTP).toBool(),
                true);
            QVERIFY(settings.contains(GC_TABBAR));
            QCOMPARE(
                settings.value(
                    nullptr,
                    GC_TABBAR).toString(),
                globalGeneralValue);
            QVERIFY(settings.contains(GC_DEV_COUNT));
            QCOMPARE(
                settings.value(
                    nullptr,
                    GC_DEV_COUNT).toInt(),
                globalTrainValue);
        } else {
            settings.initializeQSettingsGlobal(athleteRoot);
            settings.initializeQSettingsAthlete(
                athleteRoot,
                athleteName);
            QCOMPARE(
                settings.value(nullptr, GC_UNIT).toString(),
                QStringLiteral(GC_UNIT_METRIC));
            QCOMPARE(
                settings.cvalue(
                    athleteName,
                    GC_VERSION_USED).toString(),
                athleteGeneralValue);
            QCOMPARE(
                settings.cvalue(
                    athleteName,
                    GC_LTM_LAST_DATE_RANGE).toString(),
                athleteLayoutValue);
            QCOMPARE(
                settings.cvalue(
                    athleteName,
                    GC_NICKNAME).toString(),
                athletePreferencesValue);
            QCOMPARE(
                settings.cvalue(
                    athleteName,
                    GC_RWGPSUSER).toString(),
                athletePrivateValue);
        }
    }

    QSettings completed(markerPath, targetFormat);
    QCOMPARE(
        completed.value(markerKey).toString(),
        legacyMigrationComplete);

    if (scope == QStringLiteral("global")) {
        QSettings train(globalTrainPath, targetFormat);
        QCOMPARE(
            train.value(plainKey(GC_DEV_COUNT)).toInt(),
            globalTrainValue);
    } else if (scope == QStringLiteral("athlete")) {
        QSettings layout(athleteLayoutPath, targetFormat);
        QSettings preferences(
            athletePreferencesPath,
            targetFormat);
        QSettings privateSettings(
            athletePrivatePath,
            targetFormat);
        QCOMPARE(
            layout.value(
                plainKey(GC_LTM_LAST_DATE_RANGE)).toString(),
            athleteLayoutValue);
        QCOMPARE(
            preferences.value(
                plainKey(GC_NICKNAME)).toString(),
            athletePreferencesValue);
        QCOMPARE(
            privateSettings.value(
                plainKey(GC_RWGPSUSER)).toString(),
            athletePrivateValue);
    }
}

void TestCredentialSettings::
credentialClaimFailureDoesNotBlockGlobalMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("CredentialClaimMigration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString globalValue =
        QStringLiteral("ordinary-global-setting");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(athleteRoot));

    {
        QSettings legacy(
            legacyFormat,
            QSettings::UserScope,
            organization,
            application);
        legacy.setValue(plainKey(GC_START_HTTP), true);
        legacy.setValue(
            plainKey(GC_TABBAR), globalValue);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings system(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            legacySystemMigrationMarkerKey,
            legacyMigrationComplete);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    MigrationFormatFaultState &fault =
        migrationFormatFaultState();
    fault = {};
    fault.failurePoint =
        QStringLiteral(
            "credential-location-claim");
    fault.enabled = true;
    factoryState() =
        std::make_shared<FakeStoreState>();

    GSettings settings(
        organization, application,
        legacyFormat, targetFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    fault.enabled = false;
    QVERIFY(fault.rejectedWrites > 0);
    QCOMPARE(
        settings.value(
            nullptr, GC_TABBAR,
            QStringLiteral("missing")).toString(),
        globalValue);

    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    QSettings migrated(globalPath, targetFormat);
    QCOMPARE(
        migrated.value(
            legacyGlobalMigrationMarkerKey).toString(),
        legacyMigrationComplete);
    QCOMPARE(
        migrated.value(
            plainKey(GC_TABBAR)).toString(),
        globalValue);
    fault = {};
}

void TestCredentialSettings::
interruptedFreshEnrollmentRecovers_data()
{
    QTest::addColumn<QString>("stage");
    QTest::addColumn<int>("rejectedClaimCount");
    QTest::addColumn<bool>("athleteEnrollment");
    QTest::addColumn<bool>("sameProcessRetry");

    QTest::newRow("root-same-process")
        << QStringLiteral("root") << 1 << false << true;
    QTest::newRow("root-reconstructed")
        << QStringLiteral("root") << 1 << false << false;
    QTest::newRow("global-scope-same-process")
        << QStringLiteral("global") << 2 << false << true;
    QTest::newRow("global-scope-reconstructed")
        << QStringLiteral("global") << 2 << false << false;
    QTest::newRow("athlete-profile-same-process")
        << QStringLiteral("athlete-profile")
        << 3 << true << true;
    QTest::newRow("athlete-profile-reconstructed")
        << QStringLiteral("athlete-profile")
        << 3 << true << false;
    QTest::newRow("athlete-scope-same-process")
        << QStringLiteral("athlete-scope")
        << 4 << true << true;
    QTest::newRow("athlete-scope-reconstructed")
        << QStringLiteral("athlete-scope")
        << 4 << true << false;
}

void TestCredentialSettings::
interruptedFreshEnrollmentRecovers()
{
    QFETCH(QString, stage);
    QFETCH(int, rejectedClaimCount);
    QFETCH(bool, athleteEnrollment);
    QFETCH(bool, sameProcessRetry);

    struct FaultReset
    {
        ~FaultReset()
        {
            migrationFormatFaultState() = {};
        }
    } faultReset;
    Q_UNUSED(faultReset)

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("CredentialFreshEnrollment-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString athletePath =
        QDir(athleteRoot).filePath(athleteName);
    const QString athleteConfig =
        QDir(athletePath).filePath(
            QStringLiteral("config"));
    QVERIFY(QDir().mkpath(athleteConfig));

    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString athletePrivatePath =
        QDir(athleteConfig).filePath(
            QStringLiteral("athlete-private.ini"));
    const QString plaintextKey = athleteEnrollment
        ? plainKey(GC_STRAVA_TOKEN)
        : plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret = athleteEnrollment
        ? QStringLiteral("interrupted-athlete-secret")
        : QStringLiteral("interrupted-global-secret");
    const QString settingsPath = athleteEnrollment
        ? athletePrivatePath : globalPath;
    {
        QSettings plaintext(settingsPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(plaintext.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    auto createSettings = [&] {
        return std::make_unique<GSettings>(
            organization, application,
            legacyFormat, targetFormat);
    };
    std::unique_ptr<GSettings> settings =
        createSettings();
    if (athleteEnrollment)
        settings->initializeQSettingsGlobal(athleteRoot);

    MigrationFormatFaultState &fault =
        migrationFormatFaultState();
    fault = {};
    fault.failurePoint =
        QStringLiteral(
            "credential-location-claim-count-%1")
            .arg(rejectedClaimCount);
    fault.enabled = true;
    if (athleteEnrollment) {
        settings->initializeQSettingsAthlete(
            athleteRoot, athleteName);
    } else {
        settings->initializeQSettingsGlobal(athleteRoot);
    }
    fault.enabled = false;
    QVERIFY(fault.rejectedWrites > 0);
    QVERIFY(factoryState()->values.isEmpty());

    {
        QSettings retained(settingsPath, targetFormat);
        retained.setFallbacksEnabled(false);
        retained.sync();
        QCOMPARE(retained.status(), QSettings::NoError);
        QCOMPARE(retained.value(plaintextKey).toString(),
                 secret);
        if (stage == QStringLiteral("root")) {
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/root_id")));
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/binding_v2")));
        } else if (stage == QStringLiteral("global")) {
            QVERIFY(retained.contains(
                QStringLiteral(
                    "credential_store/root_id")));
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/binding_v2")));
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/id")));
        } else {
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/binding_v2")));
            QVERIFY(!retained.contains(
                QStringLiteral(
                    "credential_store/id")));
        }
    }
    {
        QSettings system(
            targetFormat, QSettings::UserScope,
            organization, application);
        system.setFallbacksEnabled(false);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
        const QString claimPrefix =
            QStringLiteral(
                "credential_store/location_claims/");
        const QString enrollmentPrefix =
            QStringLiteral(
                "credential_store/location_enrollments/");
        QCOMPARE(
            system.allKeys().filter(claimPrefix).size(),
            rejectedClaimCount - 1);
        QVERIFY(!system.allKeys()
                     .filter(enrollmentPrefix)
                     .isEmpty());
    }

    if (!sameProcessRetry) {
        settings.reset();
        settings = createSettings();
        if (athleteEnrollment) {
            settings->initializeQSettingsGlobal(
                athleteRoot);
        }
    }
    if (athleteEnrollment) {
        settings->initializeQSettingsAthlete(
            athleteRoot, athleteName);
    } else {
        settings->initializeQSettingsGlobal(athleteRoot);
    }

    const QVariant recovered = athleteEnrollment
        ? settings->cvalue(
              athleteName, GC_STRAVA_TOKEN,
              QStringLiteral("missing"))
        : settings->value(
              nullptr, GC_NOLIO_ACCESS_TOKEN,
              QStringLiteral("missing"));
    QCOMPARE(recovered.toString(), secret);
    QSettings recoveredLocal(
        settingsPath, targetFormat);
    recoveredLocal.setFallbacksEnabled(false);
    recoveredLocal.sync();
    const QString recoveredScopeId =
        recoveredLocal.value(
            QStringLiteral(
                "credential_store/id")).toString();
    QVERIFY(!QUuid(recoveredScopeId).isNull());
    const QString expectedVaultKey =
        CredentialSettings::vaultKey(
            recoveredScopeId,
            athleteEnrollment
                ? QStringLiteral(GC_STRAVA_TOKEN)
                : QStringLiteral(
                      GC_NOLIO_ACCESS_TOKEN));
    QCOMPARE(factoryState()->values.size(), 1);
    QCOMPARE(
        factoryState()->values.value(
            expectedVaultKey),
        secret);
    {
        QSettings migrated(settingsPath, targetFormat);
        migrated.setFallbacksEnabled(false);
        migrated.sync();
        QCOMPARE(migrated.status(), QSettings::NoError);
        QVERIFY(!migrated.contains(plaintextKey));
    }
    {
        QSettings system(
            targetFormat, QSettings::UserScope,
            organization, application);
        system.setFallbacksEnabled(false);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
        QVERIFY(system.allKeys()
                    .filter(QStringLiteral(
                        "credential_store/"
                        "location_enrollments/"))
                    .isEmpty());
        const int expectedClaims =
            athleteEnrollment ? 4 : 2;
        QCOMPARE(
            system.allKeys()
                .filter(QStringLiteral(
                    "credential_store/location_claims/"))
                .size(),
            expectedClaims);
    }
}

void TestCredentialSettings::
pendingEnrollmentIntentIsPathBound_data()
{
    QTest::addColumn<QString>("stage");
    QTest::addColumn<int>("rejectedClaimCount");
    QTest::addColumn<bool>("athleteEnrollment");

    QTest::newRow("root")
        << QStringLiteral("root") << 1 << false;
    QTest::newRow("global")
        << QStringLiteral("global") << 2 << false;
    QTest::newRow("athlete-profile")
        << QStringLiteral("athlete-profile")
        << 3 << true;
    QTest::newRow("athlete-scope")
        << QStringLiteral("athlete-scope")
        << 4 << true;
}

void TestCredentialSettings::
pendingEnrollmentIntentIsPathBound()
{
    QFETCH(QString, stage);
    QFETCH(int, rejectedClaimCount);
    QFETCH(bool, athleteEnrollment);
    struct FaultReset
    {
        ~FaultReset()
        {
            migrationFormatFaultState() = {};
        }
    } faultReset;
    Q_UNUSED(faultReset)

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy"));
    const QString authorityPath =
        temporary.filePath(QStringLiteral("authority"));
    QVERIFY(QDir().mkpath(legacyPath));
    QVERIFY(QDir().mkpath(authorityPath));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        legacyPath);
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        authorityPath);

    const QString organization =
        QStringLiteral("CredentialPendingCopy-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName =
        QStringLiteral("Athlete");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString originalConfig =
        QDir(originalRoot).filePath(
            athleteName + QStringLiteral("/config"));
    QVERIFY(QDir().mkpath(originalConfig));
    const QString globalPath =
        QDir(originalRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString athletePath =
        QDir(originalConfig).filePath(
            QStringLiteral(
                "athlete-private.ini"));
    const QString settingsPath = athleteEnrollment
        ? athletePath : globalPath;
    const QString plaintextKey = athleteEnrollment
        ? plainKey(GC_STRAVA_TOKEN)
        : plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret = athleteEnrollment
        ? QStringLiteral("pending-athlete-copy-secret")
        : QStringLiteral("pending-global-copy-secret");
    {
        QSettings plaintext(settingsPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(
            plaintext.status(),
            QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings interrupted(
            organization, application,
            legacyFormat, targetFormat);
        if (athleteEnrollment) {
            interrupted.initializeQSettingsGlobal(
                originalRoot);
        }
        MigrationFormatFaultState &fault =
            migrationFormatFaultState();
        fault = {};
        fault.failurePoint =
            QStringLiteral(
                "credential-location-claim-count-%1")
                .arg(rejectedClaimCount);
        fault.enabled = true;
        if (athleteEnrollment) {
            interrupted.initializeQSettingsAthlete(
                originalRoot, athleteName);
        } else {
            interrupted.initializeQSettingsGlobal(
                originalRoot);
        }
        fault.enabled = false;
        QVERIFY(fault.rejectedWrites > 0);
    }
    QVERIFY(factoryState()->values.isEmpty());

    const QString enrollmentPrefix =
        QStringLiteral(
            "credential_store/location_enrollments/");
    QHash<QString, QVariant> pendingBefore;
    QSet<QString> pendingIdentityIds;
    {
        QSettings authority(
            targetFormat, QSettings::UserScope,
            organization, application);
        authority.setFallbacksEnabled(false);
        authority.sync();
        pendingBefore = settingsValuesWithPrefix(
            &authority, enrollmentPrefix);
        QVERIFY(!pendingBefore.isEmpty());
        for (const QVariant &stored :
             pendingBefore) {
            QJsonParseError error;
            const QJsonDocument document =
                QJsonDocument::fromJson(
                    stored.toString().toUtf8(), &error);
            QVERIFY(error.error
                        == QJsonParseError::NoError
                    && document.isObject());
            const QString identity =
                document.object().value(
                    QStringLiteral(
                        "identity_id")).toString();
            QVERIFY(!QUuid(identity).isNull());
            pendingIdentityIds.insert(identity);
        }
    }

    QString copiedEvidence;
    if (athleteEnrollment) {
        const QString copiedName =
            QStringLiteral("Clone");
        const QString copiedConfig =
            QDir(originalRoot).filePath(
                copiedName + QStringLiteral("/config"));
        const QString copiedPrivate =
            QDir(copiedConfig).filePath(
                QStringLiteral(
                    "athlete-private.ini"));
        QVERIFY(QDir().mkpath(copiedConfig));
        QVERIFY(QFile::copy(
            athletePath, copiedPrivate));
        GSettings copied(
            organization, application,
            legacyFormat, targetFormat);
        copied.initializeQSettingsGlobal(originalRoot);
        copied.initializeQSettingsAthlete(
            originalRoot, copiedName);
        copied.cvalue(
            copiedName, GC_STRAVA_TOKEN,
            QStringLiteral("missing"));
        QSettings copiedLocal(
            copiedPrivate, targetFormat);
        copiedLocal.setFallbacksEnabled(false);
        copiedLocal.sync();
        copiedEvidence += copiedLocal.value(
            QStringLiteral(
                "credential_store/binding_v2"))
                                  .toString();
        copiedEvidence += copiedLocal.value(
            QStringLiteral(
                "credential_store/id")).toString();
    } else {
        const QString copiedRoot =
            temporary.filePath(QStringLiteral("copy"));
        QVERIFY(QDir().mkpath(copiedRoot));
        const QString copiedGlobal =
            QDir(copiedRoot).filePath(
                QStringLiteral(
                    "configglobal-general.ini"));
        QVERIFY(QFile::copy(
            globalPath, copiedGlobal));
        GSettings copied(
            organization, application,
            legacyFormat, targetFormat);
        copied.initializeQSettingsGlobal(copiedRoot);
        copied.value(
            nullptr, GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("missing"));
        QSettings copiedLocal(
            copiedGlobal, targetFormat);
        copiedLocal.setFallbacksEnabled(false);
        copiedLocal.sync();
        copiedEvidence += copiedLocal.value(
            QStringLiteral(
                "credential_store/root_id"))
                                  .toString();
        copiedEvidence += copiedLocal.value(
            QStringLiteral(
                "credential_store/binding_v2"))
                                  .toString();
        copiedEvidence += copiedLocal.value(
            QStringLiteral(
                "credential_store/id")).toString();
    }
    for (const QString &pendingIdentity :
         pendingIdentityIds) {
        QVERIFY(!copiedEvidence.contains(
            pendingIdentity));
    }
    {
        QSettings authority(
            targetFormat, QSettings::UserScope,
            organization, application);
        authority.setFallbacksEnabled(false);
        authority.sync();
        QCOMPARE(
            settingsValuesWithPrefix(
                &authority, enrollmentPrefix),
            pendingBefore);
    }

    {
        GSettings recovered(
            organization, application,
            legacyFormat, targetFormat);
        recovered.initializeQSettingsGlobal(
            originalRoot);
        if (athleteEnrollment) {
            recovered.initializeQSettingsAthlete(
                originalRoot, athleteName);
        }
        const QVariant observed = athleteEnrollment
            ? recovered.cvalue(
                  athleteName, GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : recovered.value(
                  nullptr, GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(observed.toString(), secret);
    }
    {
        QSettings authority(
            targetFormat, QSettings::UserScope,
            organization, application);
        authority.setFallbacksEnabled(false);
        authority.sync();
        QVERIFY(settingsValuesWithPrefix(
                    &authority, enrollmentPrefix)
                    .isEmpty());
    }
}

void TestCredentialSettings::
locationEnrollmentIsSerializedAcrossProcesses_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("parentId");
    QTest::addColumn<bool>("athleteDirectory");

    const QString rootId =
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111");
    const QString profileId =
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222");
    QTest::newRow("root")
        << QStringLiteral("root") << QString() << false;
    QTest::newRow("global-scope")
        << QStringLiteral("scope") << rootId << false;
    QTest::newRow("athlete-profile")
        << QStringLiteral("profile") << rootId << true;
    QTest::newRow("athlete-scope")
        << QStringLiteral("scope") << profileId << true;
}

void TestCredentialSettings::
locationEnrollmentIsSerializedAcrossProcesses()
{
    const QString childAction = qEnvironmentVariable(
        "GC_LOCATION_ENROLLMENT_SERIAL_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("child"));
        const QString authorityPath = qEnvironmentVariable(
            "GC_LOCATION_ENROLLMENT_SERIAL_AUTHORITY");
        const QString directoryPath = qEnvironmentVariable(
            "GC_LOCATION_ENROLLMENT_SERIAL_DIRECTORY");
        const QString kind = qEnvironmentVariable(
            "GC_LOCATION_ENROLLMENT_SERIAL_KIND");
        const QString parentId = qEnvironmentVariable(
            "GC_LOCATION_ENROLLMENT_SERIAL_PARENT");
        const QString resultPath = qEnvironmentVariable(
            "GC_LOCATION_ENROLLMENT_SERIAL_RESULT");
        QVERIFY(!authorityPath.isEmpty());
        QVERIFY(!directoryPath.isEmpty());
        QVERIFY(!kind.isEmpty());
        QVERIFY(!resultPath.isEmpty());

        QSettings authority(
            authorityPath, QSettings::IniFormat);
        authority.setFallbacksEnabled(false);
        const CredentialSettings::LocationEnrollmentResult
            enrollment =
                CredentialSettings::
                    ensureLocationEnrollment(
                        &authority, kind.toLatin1(),
                        QString(), parentId,
                        directoryPath, true);
        QJsonObject result;
        result.insert(
            QStringLiteral("status"),
            static_cast<int>(enrollment.status));
        result.insert(
            QStringLiteral("identity_id"),
            enrollment.identityId);
        result.insert(
            QStringLiteral("pending"),
            enrollment.pending);
        QVERIFY(writePrivateStateFile(
            resultPath,
            QJsonDocument(result).toJson(
                QJsonDocument::Compact)));
        return;
    }

    QFETCH(QString, kind);
    QFETCH(QString, parentId);
    QFETCH(bool, athleteDirectory);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library =
        temporary.filePath(QStringLiteral("library"));
    const QString athlete =
        QDir(library).filePath(QStringLiteral("Athlete"));
    QVERIFY(QDir().mkpath(athlete));
    const QString directory =
        athleteDirectory ? athlete : library;
    const QString canonicalDirectory =
        QFileInfo(directory).canonicalFilePath();
    QVERIFY(!canonicalDirectory.isEmpty());
    const QString authorityPath =
        temporary.filePath(
            QStringLiteral("authority.ini"));
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    const QString lockEntered =
        temporary.filePath(
            QStringLiteral("lock-entered"));
    const QString lockRelease =
        temporary.filePath(
            QStringLiteral("lock-release"));
    const QString lockContended =
        temporary.filePath(
            QStringLiteral("lock-contended"));
    const QString firstResult =
        temporary.filePath(
            QStringLiteral("first-result"));
    const QString secondResult =
        temporary.filePath(
            QStringLiteral("second-result"));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_ACTION"),
        QStringLiteral("child"));
    environment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_AUTHORITY"),
        authorityPath);
    environment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_DIRECTORY"),
        directory);
    environment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_KIND"),
        kind);
    environment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_PARENT"),
        parentId);
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_ENTERED"));
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_RELEASE"));
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_CONTENDED"));
    environment.remove(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_RESULT"));

    QProcess first;
    QProcess second;
    const auto configureChild =
        [&](QProcess *child,
            const QProcessEnvironment &childEnvironment) {
        child->setProgram(
            QCoreApplication::applicationFilePath());
        child->setArguments({
            QStringLiteral(
                "locationEnrollmentIsSerializedAcrossProcesses:")
                + QString::fromUtf8(
                    QTest::currentDataTag())
        });
        child->setProcessEnvironment(childEnvironment);
        child->setProcessChannelMode(
            QProcess::MergedChannels);
    };
    QProcessEnvironment firstEnvironment = environment;
    firstEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_ENTERED"),
        lockEntered);
    firstEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_RELEASE"),
        lockRelease);
    firstEnvironment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_RESULT"),
        firstResult);
    QProcessEnvironment secondEnvironment = environment;
    secondEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_CONTENDED"),
        lockContended);
    secondEnvironment.insert(
        QStringLiteral(
            "GC_LOCATION_ENROLLMENT_SERIAL_RESULT"),
        secondResult);
    configureChild(&first, firstEnvironment);
    configureChild(&second, secondEnvironment);

    first.start();
    QVERIFY2(
        first.waitForStarted(5000),
        qPrintable(first.errorString()));
    QVERIFY2(
        waitForFile(lockEntered, 10000, &first),
        first.readAll().constData());
    second.start();
    QVERIFY2(
        second.waitForStarted(5000),
        qPrintable(second.errorString()));
    QVERIFY2(
        waitForFile(lockContended, 10000, &second),
        second.readAll().constData());
    QCOMPARE(first.state(), QProcess::Running);
    QCOMPARE(second.state(), QProcess::Running);
    QVERIFY(writeSignalFile(lockRelease));

    bool childrenSucceeded = true;
    QByteArray childDiagnostics;
    for (QProcess *child : {&first, &second}) {
        if (!child->waitForFinished(30000)) {
            child->kill();
            child->waitForFinished();
            childrenSucceeded = false;
        }
        childDiagnostics += child->readAll();
        if (child->exitStatus()
                != QProcess::NormalExit
            || child->exitCode() != 0) {
            childrenSucceeded = false;
        }
    }
    if (!childrenSucceeded)
        QFAIL(childDiagnostics.constData());

    const QByteArray firstEnrollment =
        fileContents(firstResult);
    const QByteArray secondEnrollment =
        fileContents(secondResult);
    QVERIFY(!firstEnrollment.isEmpty());
    QCOMPARE(secondEnrollment, firstEnrollment);
    QJsonParseError resultError;
    const QJsonDocument resultDocument =
        QJsonDocument::fromJson(
            firstEnrollment, &resultError);
    QVERIFY(
        resultError.error == QJsonParseError::NoError
        && resultDocument.isObject());
    const QJsonObject result = resultDocument.object();
    QCOMPARE(
        result.value(QStringLiteral("status")).toInt(),
        static_cast<int>(
            CredentialSettings::
                LocationClaimStatus::Success));
    const QString identityId =
        result.value(
            QStringLiteral("identity_id")).toString();
    QVERIFY(!QUuid(identityId).isNull());
    QVERIFY(result.value(
                QStringLiteral("pending")).toBool());

    QSettings authority(
        authorityPath, QSettings::IniFormat);
    authority.setFallbacksEnabled(false);
    authority.sync();
    QCOMPARE(authority.status(), QSettings::NoError);
    const QHash<QString, QVariant> enrollments =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral(
                "credential_store/location_enrollments/"));
    const QHash<QString, QVariant> claims =
        settingsValuesWithPrefix(
            &authority,
            QStringLiteral(
                "credential_store/location_claims/"));
    QCOMPARE(enrollments.size(), 1);
    QCOMPARE(claims.size(), 1);
    QVERIFY(settingsValuesWithPrefix(
                &authority,
                QStringLiteral(
                    "credential_store/location_bindings/"))
                .isEmpty());
    const TestLocationRecord enrollment =
        parseTestLocationRecord(
            enrollments.cbegin().value());
    QVERIFY(enrollment.valid);
    QCOMPARE(enrollment.kind, kind);
    QCOMPARE(enrollment.identityId, identityId);
    QCOMPARE(enrollment.parentId, parentId);
    QCOMPARE(
        enrollment.directoryPath,
        canonicalDirectory);
    const TestLocationRecord claim =
        parseTestLocationClaim(
            claims.cbegin().value());
    QVERIFY(claim.valid);
    QCOMPARE(claim.identityId, identityId);
    QCOMPARE(claim.parentId, parentId);
    QCOMPARE(
        claim.directoryPath,
        canonicalDirectory);
}

void TestCredentialSettings::
freshEnrollmentIsSerializedAcrossProcesses_data()
{
    QTest::addColumn<bool>("athleteEnrollment");
    QTest::newRow("global") << false;
    QTest::newRow("athlete") << true;
}

void TestCredentialSettings::
freshEnrollmentIsSerializedAcrossProcesses()
{
    const QString childAction = qEnvironmentVariable(
        "GC_ENROLLMENT_SERIAL_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("child"));
        const QString legacyPath = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_LEGACY_PATH");
        const QString authorityPath = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_AUTHORITY_PATH");
        const QString athleteRoot = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_ATHLETE_ROOT");
        const QString organization = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_ORGANIZATION");
        const QString application = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_APPLICATION");
        const QString secret = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_SECRET");
        const QString resultPath = qEnvironmentVariable(
            "GC_ENROLLMENT_SERIAL_RESULT");
        const bool athleteEnrollment =
            qEnvironmentVariableIntValue(
                "GC_ENROLLMENT_SERIAL_ATHLETE") != 0;
        QVERIFY(!legacyPath.isEmpty());
        QVERIFY(!authorityPath.isEmpty());
        QVERIFY(!athleteRoot.isEmpty());
        QVERIFY(!organization.isEmpty());
        QVERIFY(!application.isEmpty());
        QVERIFY(!secret.isEmpty());
        QVERIFY(!resultPath.isEmpty());

        const QSettings::Format legacyFormat =
            legacyMigrationTestFormat();
        const QSettings::Format targetFormat =
            targetMigrationTestFormat();
        QVERIFY(legacyFormat != QSettings::InvalidFormat);
        QVERIFY(targetFormat != QSettings::InvalidFormat);
        QSettings::setPath(
            legacyFormat, QSettings::UserScope,
            legacyPath);
        QSettings::setPath(
            targetFormat, QSettings::UserScope,
            authorityPath);

        GSettings settings(
            organization, application,
            legacyFormat, targetFormat);
        settings.initializeQSettingsGlobal(
            athleteRoot);
        if (athleteEnrollment) {
            settings.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        }
        const QVariant observed = athleteEnrollment
            ? settings.cvalue(
                  QStringLiteral("Athlete"),
                  GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : settings.value(
                  nullptr,
                  GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(observed.toString(), secret);

        const QString globalPath =
            QDir(athleteRoot).filePath(
                QStringLiteral(
                    "configglobal-general.ini"));
        const QString athletePath =
            QDir(athleteRoot).filePath(
                QStringLiteral(
                    "Athlete/config/"
                    "athlete-private.ini"));
        QSettings global(
            globalPath, targetFormat);
        global.setFallbacksEnabled(false);
        global.sync();
        QJsonObject result;
        result.insert(
            QStringLiteral("root_id"),
            global.value(
                QStringLiteral(
                    "credential_store/root_id"))
                .toString());
        result.insert(
            QStringLiteral("global_binding"),
            global.value(
                QStringLiteral(
                    "credential_store/binding_v2"))
                .toString());
        result.insert(
            QStringLiteral("global_scope"),
            global.value(
                QStringLiteral(
                    "credential_store/id"))
                .toString());
        if (athleteEnrollment) {
            QSettings athlete(
                athletePath, targetFormat);
            athlete.setFallbacksEnabled(false);
            athlete.sync();
            result.insert(
                QStringLiteral("athlete_binding"),
                athlete.value(
                    QStringLiteral(
                        "credential_store/"
                        "binding_v2"))
                    .toString());
            result.insert(
                QStringLiteral("athlete_scope"),
                athlete.value(
                    QStringLiteral(
                        "credential_store/id"))
                    .toString());
        }
        QVERIFY(writePrivateStateFile(
            resultPath,
            QJsonDocument(result).toJson(
                QJsonDocument::Compact)));
        return;
    }

    QFETCH(bool, athleteEnrollment);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);

    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy"));
    const QString authorityPath =
        temporary.filePath(QStringLiteral("authority"));
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString athleteConfig =
        QDir(athleteRoot).filePath(
            QStringLiteral("Athlete/config"));
    const QString stateRoot =
        temporary.filePath(
            QStringLiteral("credential-state"));
    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString lockEntered =
        temporary.filePath(
            QStringLiteral("lock-entered"));
    const QString lockRelease =
        temporary.filePath(
            QStringLiteral("lock-release"));
    const QString lockContended =
        temporary.filePath(
            QStringLiteral("lock-contended"));
    const QString firstResult =
        temporary.filePath(
            QStringLiteral("first-result"));
    const QString secondResult =
        temporary.filePath(
            QStringLiteral("second-result"));
    QVERIFY(QDir().mkpath(legacyPath));
    QVERIFY(QDir().mkpath(authorityPath));
    QVERIFY(QDir().mkpath(athleteConfig));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        legacyPath);
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        authorityPath);

    const QString organization =
        QStringLiteral("CredentialConcurrentEnrollment-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString athletePath =
        QDir(athleteConfig).filePath(
            QStringLiteral(
                "athlete-private.ini"));
    const QString settingsPath = athleteEnrollment
        ? athletePath : globalPath;
    const QString plaintextKey = athleteEnrollment
        ? plainKey(GC_STRAVA_TOKEN)
        : plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret = athleteEnrollment
        ? QStringLiteral(
              "concurrent-athlete-secret")
        : QStringLiteral(
              "concurrent-global-secret");
    {
        QSettings plaintext(settingsPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(
            plaintext.status(),
            QSettings::NoError);
    }

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_STATE_ROOT"),
        stateRoot);
    environment.insert(
        QStringLiteral("GC_ENROLLMENT_TEST_VAULT"),
        vaultPath);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_ACTION"),
        QStringLiteral("child"));
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_LEGACY_PATH"),
        legacyPath);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_AUTHORITY_PATH"),
        authorityPath);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_ATHLETE_ROOT"),
        athleteRoot);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_ORGANIZATION"),
        organization);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_APPLICATION"),
        application);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_SECRET"),
        secret);
    environment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_ATHLETE"),
        athleteEnrollment
            ? QStringLiteral("1")
            : QStringLiteral("0"));
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_ENTERED"));
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_RELEASE"));
    environment.remove(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_CONTENDED"));
    environment.remove(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_RESULT"));

    QProcess first;
    QProcess second;
    const auto configureChild =
        [&](QProcess *child,
            const QProcessEnvironment &childEnvironment) {
        child->setProgram(
            QCoreApplication::applicationFilePath());
        child->setArguments({
            QStringLiteral(
                "freshEnrollmentIsSerializedAcrossProcesses:")
                + (athleteEnrollment
                       ? QStringLiteral("athlete")
                       : QStringLiteral("global"))
        });
        child->setProcessEnvironment(
            childEnvironment);
        child->setProcessChannelMode(
            QProcess::MergedChannels);
    };
    QProcessEnvironment firstEnvironment = environment;
    firstEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_ENTERED"),
        lockEntered);
    firstEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_RELEASE"),
        lockRelease);
    firstEnvironment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_RESULT"),
        firstResult);
    QProcessEnvironment secondEnvironment = environment;
    secondEnvironment.insert(
        QStringLiteral(
            "GC_CREDENTIAL_TEST_"
            "ENROLLMENT_LOCK_CONTENDED"),
        lockContended);
    secondEnvironment.insert(
        QStringLiteral(
            "GC_ENROLLMENT_SERIAL_RESULT"),
        secondResult);
    configureChild(&first, firstEnvironment);
    configureChild(&second, secondEnvironment);

    first.start();
    QVERIFY2(
        first.waitForStarted(5000),
        qPrintable(first.errorString()));
    QVERIFY2(
        waitForFile(lockEntered, 10000, &first),
        first.readAll().constData());
    second.start();
    QVERIFY2(
        second.waitForStarted(5000),
        qPrintable(second.errorString()));
    QVERIFY2(
        waitForFile(lockContended, 10000, &second),
        second.readAll().constData());
    QCOMPARE(first.state(), QProcess::Running);
    QCOMPARE(second.state(), QProcess::Running);
    QVERIFY(writeSignalFile(lockRelease));

    bool childrenSucceeded = true;
    QByteArray childDiagnostics;
    for (QProcess *child : {&first, &second}) {
        if (!child->waitForFinished(60000)) {
            child->kill();
            child->waitForFinished();
            childrenSucceeded = false;
        }
        const QByteArray output = child->readAll();
        childDiagnostics += output;
        if (child->exitStatus()
                != QProcess::NormalExit
            || child->exitCode() != 0) {
            childrenSucceeded = false;
        }
    }
    if (!childrenSucceeded) {
        QSettings authority(
            targetFormat, QSettings::UserScope,
            organization, application);
        authority.setFallbacksEnabled(false);
        authority.sync();
        childDiagnostics += "\nauthority:\n";
        for (const QString &key : authority.allKeys()) {
            childDiagnostics += key.toUtf8()
                + '='
                + authority.value(key).toString().toUtf8()
                + '\n';
        }
        childDiagnostics += "global:\n"
            + fileContents(globalPath)
            + "\nathlete:\n"
            + fileContents(athletePath);
        QFAIL(childDiagnostics.constData());
    }
    const QByteArray firstEnrollment =
        fileContents(firstResult);
    const QByteArray secondEnrollment =
        fileContents(secondResult);
    QVERIFY(!firstEnrollment.isEmpty());
    QCOMPARE(secondEnrollment, firstEnrollment);
    QJsonParseError enrollmentError;
    const QJsonDocument enrollmentDocument =
        QJsonDocument::fromJson(
            firstEnrollment, &enrollmentError);
    QVERIFY(
        enrollmentError.error
                == QJsonParseError::NoError
            && enrollmentDocument.isObject());

    QString scopeId;
    {
        QSettings local(settingsPath, targetFormat);
        local.setFallbacksEnabled(false);
        local.sync();
        QCOMPARE(local.status(), QSettings::NoError);
        QVERIFY(!local.contains(plaintextKey));
        scopeId = local.value(
            QStringLiteral(
                "credential_store/id")).toString();
        QVERIFY(!QUuid(scopeId).isNull());
        QVERIFY(local.contains(
            QStringLiteral(
                "credential_store/binding_v2")));
    }
    {
        QSettings authority(
            targetFormat, QSettings::UserScope,
            organization, application);
        authority.setFallbacksEnabled(false);
        authority.sync();
        QVERIFY(authority.allKeys()
                    .filter(QStringLiteral(
                        "credential_store/"
                        "location_enrollments/"))
                    .isEmpty());
        QCOMPARE(
            authority.allKeys()
                .filter(QStringLiteral(
                    "credential_store/"
                    "location_bindings/"))
                .size(),
            athleteEnrollment ? 4 : 2);
        QCOMPARE(
            authority.allKeys()
                .filter(QStringLiteral(
                    "credential_store/"
                    "location_claims/"))
                .size(),
            athleteEnrollment ? 4 : 2);
    }
    FileCredentialStore vault(vaultPath);
    const QString expectedVaultKey =
        CredentialSettings::vaultKey(
            scopeId,
            athleteEnrollment
                ? QStringLiteral(GC_STRAVA_TOKEN)
                : QStringLiteral(
                      GC_NOLIO_ACCESS_TOKEN));
    const CredentialStore::ReadResult stored =
        vault.read(expectedVaultKey);
    QCOMPARE(
        stored.status,
        CredentialStore::Status::Success);
    QCOMPARE(stored.value, secret);
    {
        QSettings rawVault(
            vaultPath, QSettings::IniFormat);
        rawVault.setFallbacksEnabled(false);
        rawVault.sync();
        QCOMPARE(
            rawVault.allKeys(),
            QStringList({expectedVaultKey}));
    }

    ScopedEnvironmentVariable stateEnvironment(
        QByteArrayLiteral(
            "GC_CREDENTIAL_TEST_STATE_ROOT"),
        QFile::encodeName(stateRoot));
    ScopedEnvironmentVariable vaultEnvironment(
        QByteArrayLiteral(
            "GC_ENROLLMENT_TEST_VAULT"),
        QFile::encodeName(vaultPath));
    GSettings restarted(
        organization, application,
        legacyFormat, targetFormat);
    restarted.initializeQSettingsGlobal(athleteRoot);
    if (athleteEnrollment) {
        restarted.initializeQSettingsAthlete(
            athleteRoot,
            QStringLiteral("Athlete"));
    }
    const QVariant observed = athleteEnrollment
        ? restarted.cvalue(
              QStringLiteral("Athlete"),
              GC_STRAVA_TOKEN,
              QStringLiteral("missing"))
        : restarted.value(
              nullptr, GC_NOLIO_ACCESS_TOKEN,
              QStringLiteral("missing"));
    QCOMPARE(observed.toString(), secret);
}

void TestCredentialSettings::
freshEnrollmentCrashRecoveryAcrossProcesses()
{
    const QString childAction = qEnvironmentVariable(
        "GC_ENROLLMENT_CRASH_ACTION");
    if (!childAction.isEmpty()) {
        QCOMPARE(childAction, QStringLiteral("crash"));
        const QString legacyPath = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_LEGACY_PATH");
        const QString authorityPath = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_AUTHORITY_PATH");
        const QString athleteRoot = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_ATHLETE_ROOT");
        const QString organization = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_ORGANIZATION");
        const QString application = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_APPLICATION");
        const QString stage = qEnvironmentVariable(
            "GC_ENROLLMENT_CRASH_STAGE");
        const QByteArray crashPoint = qgetenv(
            "GC_ENROLLMENT_CRASH_POINT");
        QVERIFY(!legacyPath.isEmpty());
        QVERIFY(!authorityPath.isEmpty());
        QVERIFY(!athleteRoot.isEmpty());
        QVERIFY(!organization.isEmpty());
        QVERIFY(!application.isEmpty());
        QVERIFY(!stage.isEmpty());
        QVERIFY(!crashPoint.isEmpty());

        const QSettings::Format legacyFormat =
            legacyMigrationTestFormat();
        const QSettings::Format targetFormat =
            targetMigrationTestFormat();
        QVERIFY(legacyFormat != QSettings::InvalidFormat);
        QVERIFY(targetFormat != QSettings::InvalidFormat);
        QSettings::setPath(
            legacyFormat, QSettings::UserScope,
            legacyPath);
        QSettings::setPath(
            targetFormat, QSettings::UserScope,
            authorityPath);

        qunsetenv("GC_CREDENTIAL_TEST_CRASH_POINT");
        GSettings settings(
            organization, application,
            legacyFormat, targetFormat);
        if (stage == QStringLiteral("athlete")) {
            settings.initializeQSettingsGlobal(
                athleteRoot);
            qputenv(
                "GC_CREDENTIAL_TEST_CRASH_POINT",
                crashPoint);
            settings.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        } else {
            qputenv(
                "GC_CREDENTIAL_TEST_CRASH_POINT",
                crashPoint);
            settings.initializeQSettingsGlobal(
                athleteRoot);
        }
        QFAIL("Configured enrollment crash was not reached");
    }

    struct CrashCase {
        const char *name;
        const char *stage;
        const char *point;
        int expectedEnrollments;
        int expectedClaims;
        int expectedBindings;
        int expectedEntities;
        bool rootPublished;
        bool globalBindingPublished;
        bool athleteBindingPublished;
    };
    const QList<CrashCase> cases = {
        {"root-intent", "root",
         "enrollment:root-intent",
         1, 0, 0, 1, false, false, false},
        {"root-claim", "root",
         "enrollment:root-claim",
         1, 1, 0, 1, false, false, false},
        {"root-identity", "root",
         "enrollment:identity-published",
         1, 1, 0, 1, true, false, false},
        {"root-complete", "root",
         "enrollment:root-complete",
         0, 1, 1, 1, true, false, false},
        {"global-intent", "global",
         "enrollment:scope-intent",
         1, 1, 1, 2, true, false, false},
        {"global-claim", "global",
         "enrollment:scope-claim",
         1, 2, 1, 2, true, false, false},
        {"global-binding", "global",
         "enrollment:binding-published",
         1, 2, 1, 2, true, true, false},
        {"global-complete", "global",
         "enrollment:scope-complete",
         0, 2, 2, 2, true, true, false},
        {"athlete-profile-intent", "athlete",
         "enrollment:profile-intent",
         1, 2, 2, 3, true, true, false},
        {"athlete-profile-claim", "athlete",
         "enrollment:profile-claim",
         1, 3, 2, 3, true, true, false},
        {"athlete-scope-intent", "athlete",
         "enrollment:scope-intent",
         2, 3, 2, 4, true, true, false},
        {"athlete-scope-claim", "athlete",
         "enrollment:scope-claim",
         2, 4, 2, 4, true, true, false},
        {"athlete-binding", "athlete",
         "enrollment:binding-published",
         2, 4, 2, 4, true, true, true},
        {"athlete-scope-complete", "athlete",
         "enrollment:scope-complete",
         1, 4, 3, 4, true, true, true},
        {"athlete-profile-complete", "athlete",
         "enrollment:profile-complete",
         0, 4, 4, 4, true, true, true}
    };

    for (const CrashCase &testCase : cases) {
        QTemporaryDir temporary;
        QVERIFY2(temporary.isValid(), testCase.name);
        const QSettings::Format legacyFormat =
            legacyMigrationTestFormat();
        const QSettings::Format targetFormat =
            targetMigrationTestFormat();
        QVERIFY2(
            legacyFormat != QSettings::InvalidFormat,
            testCase.name);
        QVERIFY2(
            targetFormat != QSettings::InvalidFormat,
            testCase.name);

        const QString legacyPath =
            temporary.filePath(
                QStringLiteral("legacy"));
        const QString authorityPath =
            temporary.filePath(
                QStringLiteral("authority"));
        const QString athleteRoot =
            temporary.filePath(
                QStringLiteral("library"));
        const QString athleteConfig =
            QDir(athleteRoot).filePath(
                QStringLiteral("Athlete/config"));
        const QString stateRoot =
            temporary.filePath(
                QStringLiteral("credential-state"));
        const QString vaultPath =
            temporary.filePath(
                QStringLiteral("vault.ini"));
        QVERIFY2(QDir().mkpath(legacyPath), testCase.name);
        QVERIFY2(QDir().mkpath(authorityPath), testCase.name);
        QVERIFY2(QDir().mkpath(athleteConfig), testCase.name);
        QSettings::setPath(
            legacyFormat, QSettings::UserScope,
            legacyPath);
        QSettings::setPath(
            targetFormat, QSettings::UserScope,
            authorityPath);

        const QString organization =
            QStringLiteral("CredentialCrashEnrollment-")
            + QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const QString application =
            QStringLiteral("GoldenCheetahTest");
        const bool athleteStage =
            QByteArray(testCase.stage)
                == QByteArrayLiteral("athlete");
        const QString globalPath =
            QDir(athleteRoot).filePath(
                QStringLiteral(
                    "configglobal-general.ini"));
        const QString athletePath =
            QDir(athleteConfig).filePath(
                QStringLiteral(
                    "athlete-private.ini"));
        const QString plaintextKey = athleteStage
            ? plainKey(GC_STRAVA_TOKEN)
            : plainKey(GC_NOLIO_ACCESS_TOKEN);
        const QString secret = athleteStage
            ? QStringLiteral(
                  "athlete-crash-recovery-secret")
            : QStringLiteral(
                  "global-crash-recovery-secret");
        const QString settingsPath = athleteStage
            ? athletePath : globalPath;
        {
            QSettings plaintext(
                settingsPath, targetFormat);
            plaintext.setFallbacksEnabled(false);
            plaintext.setValue(plaintextKey, secret);
            plaintext.sync();
            QCOMPARE(
                plaintext.status(),
                QSettings::NoError);
        }

        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.remove(
            QStringLiteral(
                "GC_CREDENTIAL_TEST_CRASH_POINT"));
        environment.insert(
            QStringLiteral(
                "GC_CREDENTIAL_TEST_STATE_ROOT"),
            stateRoot);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_TEST_VAULT"),
            vaultPath);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_ACTION"),
            QStringLiteral("crash"));
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_LEGACY_PATH"),
            legacyPath);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_AUTHORITY_PATH"),
            authorityPath);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_ATHLETE_ROOT"),
            athleteRoot);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_ORGANIZATION"),
            organization);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_APPLICATION"),
            application);
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_STAGE"),
            QString::fromLatin1(testCase.stage));
        environment.insert(
            QStringLiteral(
                "GC_ENROLLMENT_CRASH_POINT"),
            QString::fromLatin1(testCase.point));

        QProcess child;
        child.setProgram(
            QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "freshEnrollmentCrashRecoveryAcrossProcesses")
        });
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(
            QProcess::MergedChannels);
        child.start();
        QVERIFY2(
            child.waitForStarted(5000),
            testCase.name);
        if (!child.waitForFinished(30000)) {
            child.kill();
            child.waitForFinished();
            QFAIL(testCase.name);
        }
        const QByteArray childOutput = child.readAll();
        QVERIFY2(
            child.exitStatus() == QProcess::NormalExit
                && child.exitCode() == 86,
            childOutput.constData());

        const QString canonicalRoot =
            QFileInfo(athleteRoot).canonicalFilePath();
        const QString canonicalAthlete =
            QFileInfo(
                QDir(athleteRoot).filePath(
                    QStringLiteral("Athlete")))
                .canonicalFilePath();
        QVERIFY2(!canonicalRoot.isEmpty(), testCase.name);
        QVERIFY2(!canonicalAthlete.isEmpty(), testCase.name);
        const QString enrollmentPrefix =
            QStringLiteral(
                "credential_store/location_enrollments/");
        const QString claimPrefix =
            QStringLiteral(
                "credential_store/location_claims/");
        const QString bindingPrefix =
            QStringLiteral(
                "credential_store/location_bindings/");
        QHash<QString, TestLocationRecord> entities;
        QHash<QString, QString> pendingEntityIds;
        {
            QSettings authority(
                targetFormat,
                QSettings::UserScope,
                organization,
                application);
            authority.setFallbacksEnabled(false);
            authority.sync();
            QCOMPARE(
                authority.status(),
                QSettings::NoError);
            const QHash<QString, QVariant> enrollments =
                settingsValuesWithPrefix(
                    &authority, enrollmentPrefix);
            const QHash<QString, QVariant> claims =
                settingsValuesWithPrefix(
                    &authority, claimPrefix);
            const QHash<QString, QVariant> bindings =
                settingsValuesWithPrefix(
                    &authority, bindingPrefix);
            QCOMPARE(
                enrollments.size(),
                testCase.expectedEnrollments);
            QCOMPARE(
                claims.size(),
                testCase.expectedClaims);
            QCOMPARE(
                bindings.size(),
                testCase.expectedBindings);

            const auto entityName =
                [&](const TestLocationRecord &record) {
                    if (record.kind
                            == QStringLiteral("root")
                        && record.parentId.isEmpty()
                        && record.directoryPath
                            == canonicalRoot) {
                        return QStringLiteral("root");
                    }
                    if (record.kind
                            == QStringLiteral("profile")
                        && record.directoryPath
                            == canonicalAthlete) {
                        return QStringLiteral(
                            "athlete-profile");
                    }
                    if (record.kind
                            == QStringLiteral("scope")
                        && record.directoryPath
                            == canonicalRoot) {
                        return QStringLiteral(
                            "global-scope");
                    }
                    if (record.kind
                            == QStringLiteral("scope")
                        && record.directoryPath
                            == canonicalAthlete) {
                        return QStringLiteral(
                            "athlete-scope");
                    }
                    return QString();
                };
            for (const QVariant &stored : bindings) {
                QVERIFY2(
                    !stored.toString().contains(secret),
                    testCase.name);
                const TestLocationRecord record =
                    parseTestLocationRecord(stored);
                const QString entity =
                    entityName(record);
                QVERIFY2(
                    record.valid
                        && !entity.isEmpty()
                        && !entities.contains(entity),
                    testCase.name);
                entities.insert(entity, record);
            }
            for (const QVariant &stored : enrollments) {
                QVERIFY2(
                    !stored.toString().contains(secret),
                    testCase.name);
                const TestLocationRecord record =
                    parseTestLocationRecord(stored);
                const QString entity =
                    entityName(record);
                QVERIFY2(
                    record.valid
                        && !entity.isEmpty()
                        && !entities.contains(entity),
                    testCase.name);
                entities.insert(entity, record);
                pendingEntityIds.insert(
                    entity, record.identityId);
            }
            QCOMPARE(
                entities.size(),
                testCase.expectedEntities);
            QVERIFY2(
                entities.contains(
                    QStringLiteral("root")),
                testCase.name);
            if (testCase.expectedEntities >= 2) {
                QVERIFY2(
                    entities.contains(
                        QStringLiteral(
                            "global-scope")),
                    testCase.name);
                QCOMPARE(
                    entities.value(
                        QStringLiteral(
                            "global-scope"))
                        .parentId,
                    entities.value(
                        QStringLiteral("root"))
                        .identityId);
            }
            if (testCase.expectedEntities >= 3) {
                QVERIFY2(
                    entities.contains(
                        QStringLiteral(
                            "athlete-profile")),
                    testCase.name);
                QCOMPARE(
                    entities.value(
                        QStringLiteral(
                            "athlete-profile"))
                        .parentId,
                    entities.value(
                        QStringLiteral("root"))
                        .identityId);
            }
            if (testCase.expectedEntities >= 4) {
                QVERIFY2(
                    entities.contains(
                        QStringLiteral(
                            "athlete-scope")),
                    testCase.name);
                QCOMPARE(
                    entities.value(
                        QStringLiteral(
                            "athlete-scope"))
                        .parentId,
                    entities.value(
                        QStringLiteral(
                            "athlete-profile"))
                        .identityId);
            }

            QSet<QString> claimedIdentities;
            for (const QVariant &stored : claims) {
                QVERIFY2(
                    !stored.toString().contains(secret),
                    testCase.name);
                const TestLocationRecord claim =
                    parseTestLocationClaim(stored);
                QVERIFY2(claim.valid, testCase.name);
                QString matchingEntity;
                for (auto entity = entities.cbegin();
                     entity != entities.cend();
                     ++entity) {
                    if (entity.value().identityId
                            == claim.identityId
                        && entity.value().parentId
                            == claim.parentId
                        && entity.value().directoryPath
                            == claim.directoryPath) {
                        matchingEntity = entity.key();
                        break;
                    }
                }
                QVERIFY2(
                    !matchingEntity.isEmpty()
                        && !claimedIdentities.contains(
                            claim.identityId),
                    testCase.name);
                claimedIdentities.insert(
                    claim.identityId);
            }
            QCOMPARE(
                claimedIdentities.size(),
                testCase.expectedClaims);
        }
        const QString rootId =
            entities.value(
                QStringLiteral("root")).identityId;
        {
            QSettings local(globalPath, targetFormat);
            local.setFallbacksEnabled(false);
            local.sync();
            QCOMPARE(
                local.status(), QSettings::NoError);
            QCOMPARE(
                local.contains(
                    QStringLiteral(
                        "credential_store/root_id")),
                testCase.rootPublished);
            if (testCase.rootPublished) {
                QCOMPARE(
                    local.value(
                        QStringLiteral(
                            "credential_store/root_id"))
                        .toString(),
                    rootId);
            }
            QCOMPARE(
                local.contains(
                    QStringLiteral(
                        "credential_store/binding_v2")),
                testCase.globalBindingPublished);
            QCOMPARE(
                local.contains(
                    QStringLiteral(
                        "credential_store/id")),
                testCase.globalBindingPublished);
            if (testCase.globalBindingPublished) {
                const TestScopeBinding binding =
                    parseTestScopeBinding(
                        local.value(
                            QStringLiteral(
                                "credential_store/"
                                "binding_v2")));
                QVERIFY2(binding.valid, testCase.name);
                QCOMPARE(binding.rootId, rootId);
                QCOMPARE(binding.profileId, rootId);
                QCOMPARE(
                    binding.scopeId,
                    entities.value(
                        QStringLiteral(
                            "global-scope"))
                        .identityId);
                QCOMPARE(
                    binding.origin,
                    QStringLiteral("fresh"));
                QCOMPARE(
                    local.value(
                        QStringLiteral(
                            "credential_store/id"))
                        .toString(),
                    binding.scopeId);
            }
        }
        {
            QSettings local(athletePath, targetFormat);
            local.setFallbacksEnabled(false);
            local.sync();
            QCOMPARE(
                local.status(), QSettings::NoError);
            QCOMPARE(
                local.contains(
                    QStringLiteral(
                        "credential_store/binding_v2")),
                testCase.athleteBindingPublished);
            QCOMPARE(
                local.contains(
                    QStringLiteral(
                        "credential_store/id")),
                testCase.athleteBindingPublished);
            if (testCase.athleteBindingPublished) {
                const TestScopeBinding binding =
                    parseTestScopeBinding(
                        local.value(
                            QStringLiteral(
                                "credential_store/"
                                "binding_v2")));
                QVERIFY2(binding.valid, testCase.name);
                QCOMPARE(binding.rootId, rootId);
                QCOMPARE(
                    binding.profileId,
                    entities.value(
                        QStringLiteral(
                            "athlete-profile"))
                        .identityId);
                QCOMPARE(
                    binding.scopeId,
                    entities.value(
                        QStringLiteral(
                            "athlete-scope"))
                        .identityId);
                QCOMPARE(
                    binding.origin,
                    QStringLiteral("fresh"));
                QCOMPARE(
                    local.value(
                        QStringLiteral(
                            "credential_store/id"))
                        .toString(),
                    binding.scopeId);
            }
        }
        {
            QSettings plaintext(
                settingsPath, targetFormat);
            plaintext.setFallbacksEnabled(false);
            plaintext.sync();
            QCOMPARE(
                plaintext.status(), QSettings::NoError);
            QCOMPARE(
                plaintext.value(plaintextKey).toString(),
                secret);
        }
        {
            QSettings vault(
                vaultPath, QSettings::IniFormat);
            vault.setFallbacksEnabled(false);
            vault.sync();
            QCOMPARE(
                vault.status(), QSettings::NoError);
            QVERIFY2(
                vault.allKeys().isEmpty(),
                testCase.name);
        }

        ScopedEnvironmentVariable stateEnvironment(
            QByteArrayLiteral(
                "GC_CREDENTIAL_TEST_STATE_ROOT"),
            QFile::encodeName(stateRoot));
        ScopedEnvironmentVariable vaultEnvironment(
            QByteArrayLiteral(
                "GC_ENROLLMENT_TEST_VAULT"),
            QFile::encodeName(vaultPath));
        {
            GSettings recovered(
                organization, application,
                legacyFormat, targetFormat);
            recovered.initializeQSettingsGlobal(
                athleteRoot);
            if (athleteStage) {
                recovered.initializeQSettingsAthlete(
                    athleteRoot,
                    QStringLiteral("Athlete"));
            }
            const QVariant observed = athleteStage
                ? recovered.cvalue(
                      QStringLiteral("Athlete"),
                      GC_STRAVA_TOKEN,
                      QStringLiteral("missing"))
                : recovered.value(
                      nullptr,
                      GC_NOLIO_ACCESS_TOKEN,
                      QStringLiteral("missing"));
            QCOMPARE(observed.toString(), secret);
        }
        {
            GSettings restarted(
                organization, application,
                legacyFormat, targetFormat);
            restarted.initializeQSettingsGlobal(
                athleteRoot);
            if (athleteStage) {
                restarted.initializeQSettingsAthlete(
                    athleteRoot,
                    QStringLiteral("Athlete"));
            }
            const QVariant observed = athleteStage
                ? restarted.cvalue(
                      QStringLiteral("Athlete"),
                      GC_STRAVA_TOKEN,
                      QStringLiteral("missing"))
                : restarted.value(
                      nullptr,
                      GC_NOLIO_ACCESS_TOKEN,
                      QStringLiteral("missing"));
            QCOMPARE(observed.toString(), secret);
        }

        QHash<QString, TestLocationRecord>
            finalEntities;
        {
            QSettings authority(
                targetFormat,
                QSettings::UserScope,
                organization,
                application);
            authority.setFallbacksEnabled(false);
            authority.sync();
            QCOMPARE(
                authority.status(),
                QSettings::NoError);
            QVERIFY(
                settingsValuesWithPrefix(
                    &authority, enrollmentPrefix)
                    .isEmpty());
            const QHash<QString, QVariant> bindings =
                settingsValuesWithPrefix(
                    &authority, bindingPrefix);
            const QHash<QString, QVariant> claims =
                settingsValuesWithPrefix(
                    &authority, claimPrefix);
            QCOMPARE(
                bindings.size(),
                athleteStage ? 4 : 2);
            QCOMPARE(
                claims.size(),
                athleteStage ? 4 : 2);
            for (const QVariant &stored : bindings) {
                const TestLocationRecord record =
                    parseTestLocationRecord(stored);
                QVERIFY2(record.valid, testCase.name);
                QString entity;
                if (record.kind
                        == QStringLiteral("root")
                    && record.directoryPath
                        == canonicalRoot) {
                    entity = QStringLiteral("root");
                } else if (record.kind
                               == QStringLiteral("scope")
                           && record.directoryPath
                               == canonicalRoot) {
                    entity =
                        QStringLiteral("global-scope");
                } else if (record.kind
                               == QStringLiteral("profile")
                           && record.directoryPath
                               == canonicalAthlete) {
                    entity = QStringLiteral(
                        "athlete-profile");
                } else if (record.kind
                               == QStringLiteral("scope")
                           && record.directoryPath
                               == canonicalAthlete) {
                    entity =
                        QStringLiteral("athlete-scope");
                }
                QVERIFY2(
                    !entity.isEmpty()
                        && !finalEntities.contains(
                            entity),
                    testCase.name);
                finalEntities.insert(entity, record);
            }
            QVERIFY2(
                finalEntities.contains(
                    QStringLiteral("root"))
                    && finalEntities.contains(
                        QStringLiteral(
                            "global-scope")),
                testCase.name);
            QCOMPARE(
                finalEntities.value(
                    QStringLiteral("global-scope"))
                    .parentId,
                finalEntities.value(
                    QStringLiteral("root"))
                    .identityId);
            if (athleteStage) {
                QVERIFY2(
                    finalEntities.contains(
                        QStringLiteral(
                            "athlete-profile"))
                        && finalEntities.contains(
                            QStringLiteral(
                                "athlete-scope")),
                    testCase.name);
                QCOMPARE(
                    finalEntities.value(
                        QStringLiteral(
                            "athlete-profile"))
                        .parentId,
                    finalEntities.value(
                        QStringLiteral("root"))
                        .identityId);
                QCOMPARE(
                    finalEntities.value(
                        QStringLiteral(
                            "athlete-scope"))
                        .parentId,
                    finalEntities.value(
                        QStringLiteral(
                            "athlete-profile"))
                        .identityId);
            }
            QSet<QString> claimedIdentities;
            for (const QVariant &stored : claims) {
                const TestLocationRecord claim =
                    parseTestLocationClaim(stored);
                QVERIFY2(claim.valid, testCase.name);
                bool matched = false;
                for (const TestLocationRecord &record :
                     finalEntities) {
                    if (record.identityId
                            == claim.identityId
                        && record.parentId
                            == claim.parentId
                        && record.directoryPath
                            == claim.directoryPath) {
                        matched = true;
                        break;
                    }
                }
                QVERIFY2(
                    matched
                        && !claimedIdentities.contains(
                            claim.identityId),
                    testCase.name);
                claimedIdentities.insert(
                    claim.identityId);
            }
            QCOMPARE(
                claimedIdentities.size(),
                finalEntities.size());
        }
        for (auto pending =
                 pendingEntityIds.cbegin();
             pending != pendingEntityIds.cend();
             ++pending) {
            QVERIFY2(
                finalEntities.contains(pending.key()),
                testCase.name);
            QCOMPARE(
                finalEntities.value(
                    pending.key()).identityId,
                pending.value());
        }

        QString scopeId;
        {
            QSettings local(
                settingsPath, targetFormat);
            local.setFallbacksEnabled(false);
            local.sync();
            QCOMPARE(
                local.status(),
                QSettings::NoError);
            QVERIFY(!local.contains(plaintextKey));
            scopeId = local.value(
                QStringLiteral(
                    "credential_store/id")).toString();
            QVERIFY(!QUuid(scopeId).isNull());
            const TestScopeBinding binding =
                parseTestScopeBinding(
                    local.value(
                        QStringLiteral(
                            "credential_store/"
                            "binding_v2")));
            QVERIFY2(binding.valid, testCase.name);
            QCOMPARE(
                binding.rootId,
                finalEntities.value(
                    QStringLiteral("root"))
                    .identityId);
            QCOMPARE(
                binding.profileId,
                athleteStage
                    ? finalEntities.value(
                          QStringLiteral(
                              "athlete-profile"))
                          .identityId
                    : binding.rootId);
            QCOMPARE(
                binding.scopeId,
                finalEntities.value(
                    athleteStage
                        ? QStringLiteral(
                              "athlete-scope")
                        : QStringLiteral(
                              "global-scope"))
                    .identityId);
            QCOMPARE(scopeId, binding.scopeId);
        }
        {
            QSettings global(
                globalPath, targetFormat);
            global.setFallbacksEnabled(false);
            global.sync();
            QCOMPARE(
                global.value(
                    QStringLiteral(
                        "credential_store/root_id"))
                    .toString(),
                finalEntities.value(
                    QStringLiteral("root"))
                    .identityId);
            const TestScopeBinding binding =
                parseTestScopeBinding(
                    global.value(
                        QStringLiteral(
                            "credential_store/"
                            "binding_v2")));
            QVERIFY2(binding.valid, testCase.name);
            QCOMPARE(binding.rootId, binding.profileId);
            QCOMPARE(
                binding.scopeId,
                finalEntities.value(
                    QStringLiteral("global-scope"))
                    .identityId);
        }

        FileCredentialStore vault(vaultPath);
        const CredentialStore::ReadResult stored =
            vault.read(CredentialSettings::vaultKey(
                scopeId,
                athleteStage
                    ? QStringLiteral(GC_STRAVA_TOKEN)
                    : QStringLiteral(
                          GC_NOLIO_ACCESS_TOKEN)));
        QCOMPARE(
            stored.status,
            CredentialStore::Status::Success);
        QCOMPARE(stored.value, secret);
    }
}

void TestCredentialSettings::
credentialEnrollmentAuthorityMustBeExternal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);

    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(athleteRoot));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        temporary.filePath(QStringLiteral("legacy")));
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        athleteRoot);

    const QString organization =
        QStringLiteral("CredentialInternalAuthority-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString plaintextKey =
        plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret =
        QStringLiteral("internal-authority-secret");
    {
        QSettings plaintext(globalPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(plaintext.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(
            organization, application,
            legacyFormat, targetFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(
            settings.value(
                nullptr, GC_NOLIO_ACCESS_TOKEN,
                QStringLiteral("missing")).toString(),
            QStringLiteral("missing"));
        QVERIFY(!settings.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QVERIFY(factoryState()->values.isEmpty());

    {
        QSettings retained(globalPath, targetFormat);
        retained.setFallbacksEnabled(false);
        retained.sync();
        QCOMPARE(retained.status(), QSettings::NoError);
        QCOMPARE(retained.value(plaintextKey).toString(),
                 secret);
        QVERIFY(!retained.contains(
            QStringLiteral(
                "credential_store/root_id")));
        QVERIFY(!retained.contains(
            QStringLiteral(
                "credential_store/binding_v2")));
    }
    {
        QSettings internalAuthority(
            targetFormat, QSettings::UserScope,
            organization, application);
        internalAuthority.setFallbacksEnabled(false);
        internalAuthority.sync();
        QCOMPARE(
            QFileInfo(internalAuthority.fileName())
                .canonicalFilePath()
                .startsWith(
                    QFileInfo(athleteRoot)
                        .canonicalFilePath()
                    + QDir::separator()),
            true);
        QVERIFY(internalAuthority.allKeys()
                    .filter(QStringLiteral(
                        "credential_store/"
                        "location_claims/"))
                    .isEmpty());
        QVERIFY(internalAuthority.allKeys()
                    .filter(QStringLiteral(
                        "credential_store/"
                        "location_enrollments/"))
                    .isEmpty());
        QVERIFY(internalAuthority.allKeys()
                    .filter(QStringLiteral(
                        "credential_store/"
                        "location_bindings/"))
                    .isEmpty());
    }
}

void TestCredentialSettings::
credentialEnrollmentAuthorityAliasesFailClosed_data()
{
    QTest::addColumn<QString>("aliasMode");
#ifdef Q_OS_UNIX
    QTest::newRow("symlink-directory")
        << QStringLiteral("symlink-directory");
#endif
#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
    QTest::newRow("hardlink-file")
        << QStringLiteral("hardlink-file");
#endif
#ifdef Q_OS_WIN
    QTest::newRow("junction-directory")
        << QStringLiteral("junction-directory");
    QTest::newRow("case-alias")
        << QStringLiteral("case-alias");
#endif
}

void TestCredentialSettings::
credentialEnrollmentAuthorityAliasesFailClosed()
{
    QFETCH(QString, aliasMode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);

    const QString athleteRoot =
        temporary.filePath(QStringLiteral("Library"));
    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy"));
    const QString outsideAuthority =
        temporary.filePath(
            QStringLiteral("outside-authority"));
    const QString insideAuthority =
        QDir(athleteRoot).filePath(
            QStringLiteral("authority"));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(legacyPath));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        legacyPath);

    QString configuredAuthorityPath;
    if (aliasMode
            == QStringLiteral("symlink-directory")) {
        QVERIFY(QDir().mkpath(insideAuthority));
        configuredAuthorityPath =
            temporary.filePath(
                QStringLiteral("authority-alias"));
        QVERIFY(QFile::link(
            insideAuthority,
            configuredAuthorityPath));
        QVERIFY(QFileInfo(
                    configuredAuthorityPath)
                    .isSymLink());
    } else if (aliasMode
                   == QStringLiteral(
                       "junction-directory")) {
#ifdef Q_OS_WIN
        QVERIFY(QDir().mkpath(insideAuthority));
        configuredAuthorityPath =
            temporary.filePath(
                QStringLiteral("authority-junction"));
        QProcess junction;
        junction.start(
            QStringLiteral("cmd.exe"),
            {
                QStringLiteral("/c"),
                QStringLiteral("mklink"),
                QStringLiteral("/J"),
                QDir::toNativeSeparators(
                    configuredAuthorityPath),
                QDir::toNativeSeparators(
                    insideAuthority)
            });
        QVERIFY2(
            junction.waitForFinished(10000),
            qPrintable(junction.errorString()));
        const QByteArray output =
            junction.readAllStandardOutput()
            + junction.readAllStandardError();
        QVERIFY2(
            junction.exitStatus()
                    == QProcess::NormalExit
                && junction.exitCode() == 0,
            output.constData());
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(
                QDir::toNativeSeparators(
                    configuredAuthorityPath)
                    .utf16()));
        QVERIFY(
            attributes != INVALID_FILE_ATTRIBUTES);
        QVERIFY(
            attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        QFAIL("Authority junction row requires Windows");
#endif
    } else if (aliasMode
                   == QStringLiteral("case-alias")) {
        configuredAuthorityPath =
            QDir(temporary.path()).filePath(
                QStringLiteral("library"));
    } else {
        QVERIFY(QDir().mkpath(outsideAuthority));
        configuredAuthorityPath =
            outsideAuthority;
    }
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        configuredAuthorityPath);

    const QString organization =
        QStringLiteral("CredentialAuthorityAlias-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    QSettings authority(
        targetFormat, QSettings::UserScope,
        organization, application);
    authority.setFallbacksEnabled(false);
    authority.setValue(
        QStringLiteral("sentinel"),
        QStringLiteral("unchanged"));
    authority.setValue(
        legacySystemMigrationMarkerKey,
        legacyMigrationComplete);
    authority.sync();
    QCOMPARE(
        authority.status(), QSettings::NoError);

    if (aliasMode
            == QStringLiteral("hardlink-file")) {
#ifdef Q_OS_UNIX
        const QString linkedAuthority =
            QDir(insideAuthority).filePath(
                QStringLiteral("authority-link"));
        QVERIFY(QDir().mkpath(insideAuthority));
        const QByteArray source =
            QFile::encodeName(authority.fileName());
        const QByteArray target =
            QFile::encodeName(linkedAuthority);
        QCOMPARE(
            ::link(
                source.constData(),
                target.constData()),
            0);
#elif defined(Q_OS_WIN)
        const QString linkedAuthority =
            QDir(insideAuthority).filePath(
                QStringLiteral("authority-link"));
        QVERIFY(QDir().mkpath(insideAuthority));
        const BOOL linked = CreateHardLinkW(
            reinterpret_cast<LPCWSTR>(
                QDir::toNativeSeparators(
                    linkedAuthority).utf16()),
            reinterpret_cast<LPCWSTR>(
                QDir::toNativeSeparators(
                    authority.fileName()).utf16()),
            nullptr);
        const DWORD error =
            linked ? ERROR_SUCCESS : GetLastError();
        QVERIFY2(
            linked,
            qPrintable(
                QStringLiteral(
                    "CreateHardLinkW failed: %1")
                    .arg(error)));
#else
        QFAIL(
            "Hard-link alias row requires Unix or Windows");
#endif
    }
    const QByteArray authorityBefore =
        fileContents(authority.fileName());

    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString plaintextKey =
        plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret =
        QStringLiteral("authority-alias-secret");
    {
        QSettings plaintext(
            globalPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(
            plaintext.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(
        organization, application,
        legacyFormat, targetFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    QCOMPARE(
        settings.value(
            nullptr, GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("missing")).toString(),
        QStringLiteral("missing"));
    QVERIFY(!settings.setValueChecked(
        GC_NOLIO_ACCESS_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(factoryState()->values.isEmpty());

    {
        QSettings retained(globalPath, targetFormat);
        retained.setFallbacksEnabled(false);
        retained.sync();
        QCOMPARE(
            retained.value(plaintextKey).toString(),
            secret);
        QVERIFY(!retained.contains(
            QStringLiteral(
                "credential_store/root_id")));
        QVERIFY(!retained.contains(
            QStringLiteral(
                "credential_store/binding_v2")));
    }
    authority.sync();
    QCOMPARE(
        authority.value(
            QStringLiteral("sentinel")).toString(),
        QStringLiteral("unchanged"));
    for (const QString &prefix : {
             QStringLiteral(
                 "credential_store/location_claims/"),
             QStringLiteral(
                 "credential_store/location_enrollments/"),
             QStringLiteral(
                 "credential_store/location_bindings/")}) {
        QVERIFY(settingsValuesWithPrefix(
                    &authority, prefix)
                    .isEmpty());
    }
    QCOMPARE(
        fileContents(authority.fileName()),
        authorityBefore);
}

void TestCredentialSettings::
completedLocationCannotBeReenrolledWithoutLocalMetadata_data()
{
    QTest::addColumn<bool>("athleteCredential");
    QTest::addColumn<bool>("legacyClaimsOnly");
    QTest::addColumn<QString>("lossMode");
    QTest::addColumn<bool>("recoverable");

    QTest::newRow("completed-global-both")
        << false << false << QStringLiteral("both")
        << false;
    QTest::newRow("completed-global-binding")
        << false << false << QStringLiteral("binding")
        << true;
    QTest::newRow("completed-global-scope")
        << false << false << QStringLiteral("scope")
        << true;
    QTest::newRow("claim-only-global-both")
        << false << true << QStringLiteral("both")
        << false;
    QTest::newRow("claim-only-global-binding")
        << false << true << QStringLiteral("binding")
        << true;
    QTest::newRow("claim-only-global-scope")
        << false << true << QStringLiteral("scope")
        << true;
    QTest::newRow("completed-global-root-and-binding")
        << false << false
        << QStringLiteral("root-and-binding") << false;
    QTest::newRow("claim-only-global-root-and-binding")
        << false << true
        << QStringLiteral("root-and-binding") << false;
    QTest::newRow("completed-athlete-both")
        << true << false << QStringLiteral("both")
        << false;
    QTest::newRow("completed-athlete-binding")
        << true << false << QStringLiteral("binding")
        << true;
    QTest::newRow("completed-athlete-scope")
        << true << false << QStringLiteral("scope")
        << true;
    QTest::newRow("claim-only-athlete-both")
        << true << true << QStringLiteral("both")
        << false;
    QTest::newRow("claim-only-athlete-binding")
        << true << true << QStringLiteral("binding")
        << true;
    QTest::newRow("claim-only-athlete-scope")
        << true << true << QStringLiteral("scope")
        << true;
}

void TestCredentialSettings::
completedLocationCannotBeReenrolledWithoutLocalMetadata()
{
    QFETCH(bool, athleteCredential);
    QFETCH(bool, legacyClaimsOnly);
    QFETCH(QString, lossMode);
    QFETCH(bool, recoverable);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy"));
    const QString authorityPath =
        temporary.filePath(QStringLiteral("authority"));
    QVERIFY(QDir().mkpath(legacyPath));
    QVERIFY(QDir().mkpath(authorityPath));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        legacyPath);
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        authorityPath);

    const QString organization =
        QStringLiteral("CredentialLostMetadata-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString athleteConfig =
        QDir(athleteRoot).filePath(
            QStringLiteral("Athlete/config"));
    QVERIFY(QDir().mkpath(athleteConfig));
    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString athletePath =
        QDir(athleteConfig).filePath(
            QStringLiteral("athlete-private.ini"));
    const QString settingsPath = athleteCredential
        ? athletePath : globalPath;
    const QString credentialKey = athleteCredential
        ? QStringLiteral(GC_STRAVA_TOKEN)
        : QStringLiteral(GC_NOLIO_ACCESS_TOKEN);
    const QString plaintextKey =
        plainKey(credentialKey);
    const QString originalSecret =
        athleteCredential
            ? QStringLiteral(
                  "completed-athlete-location-secret")
            : QStringLiteral(
                  "completed-global-location-secret");
    const QString replacementSecret =
        QStringLiteral("must-not-be-reenrolled");
    {
        QSettings plaintext(settingsPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(
            plaintextKey, originalSecret);
        plaintext.sync();
        QCOMPARE(
            plaintext.status(),
            QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings enrolled(
            organization, application,
            legacyFormat, targetFormat);
        enrolled.initializeQSettingsGlobal(athleteRoot);
        if (athleteCredential) {
            enrolled.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        }
        const QVariant observed = athleteCredential
            ? enrolled.cvalue(
                  QStringLiteral("Athlete"),
                  GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : enrolled.value(
                  nullptr, GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(observed.toString(), originalSecret);
    }
    const QHash<QString, QString> vaultBefore =
        factoryState()->values;
    QCOMPARE(vaultBefore.size(), 1);

    const QString rootKey =
        QStringLiteral("credential_store/root_id");
    const QString localBindingKey =
        QStringLiteral(
            "credential_store/binding_v2");
    const QString localScopeKey =
        QStringLiteral("credential_store/id");
    QString originalRootId;
    {
        QSettings global(globalPath, targetFormat);
        global.setFallbacksEnabled(false);
        global.sync();
        originalRootId =
            global.value(rootKey).toString();
        QVERIFY(!QUuid(originalRootId).isNull());
    }
    QVariant originalBinding;
    QString originalScopeId;
    {
        QSettings local(settingsPath, targetFormat);
        local.setFallbacksEnabled(false);
        local.sync();
        originalBinding =
            local.value(localBindingKey);
        originalScopeId =
            local.value(localScopeKey).toString();
        const TestScopeBinding parsed =
            parseTestScopeBinding(originalBinding);
        QVERIFY(parsed.valid);
        QCOMPARE(parsed.rootId, originalRootId);
        QCOMPARE(parsed.scopeId, originalScopeId);
        QCOMPARE(parsed.origin, QStringLiteral("fresh"));
    }
    const QString expectedVaultKey =
        CredentialSettings::vaultKey(
            originalScopeId, credentialKey);
    QCOMPARE(
        vaultBefore.value(expectedVaultKey),
        originalSecret);

    QSettings authority(
        targetFormat, QSettings::UserScope,
        organization, application);
    authority.setFallbacksEnabled(false);
    authority.sync();
    const QString bindingPrefix =
        QStringLiteral(
            "credential_store/location_bindings/");
    const QString claimPrefix =
        QStringLiteral(
            "credential_store/location_claims/");
    const QString enrollmentPrefix =
        QStringLiteral(
            "credential_store/location_enrollments/");
    const bool removeRoot =
        lossMode
        == QStringLiteral("root-and-binding");
    const bool removeBinding =
        lossMode == QStringLiteral("both")
        || lossMode == QStringLiteral("binding")
        || removeRoot;
    const bool removeScope =
        lossMode == QStringLiteral("both")
        || lossMode == QStringLiteral("scope");
    const QHash<QString, QVariant> completedBindings =
        settingsValuesWithPrefix(
            &authority, bindingPrefix);
    const QHash<QString, QVariant> claimsBefore =
        settingsValuesWithPrefix(
            &authority, claimPrefix);
    QCOMPARE(
        completedBindings.size(),
        athleteCredential ? 4 : 2);
    QCOMPARE(
        claimsBefore.size(),
        athleteCredential ? 4 : 2);
    QVERIFY(settingsValuesWithPrefix(
                &authority, enrollmentPrefix)
                .isEmpty());
    QHash<QString, QVariant> legacyBindings =
        completedBindings;
    if (legacyClaimsOnly) {
        const QString canonicalRoot =
            QFileInfo(athleteRoot)
                .canonicalFilePath();
        const QString canonicalAthlete =
            QFileInfo(
                QDir(athleteRoot).filePath(
                    QStringLiteral("Athlete")))
                .canonicalFilePath();
        for (auto binding =
                 completedBindings.cbegin();
             binding != completedBindings.cend();
             ++binding) {
            const TestLocationRecord record =
                parseTestLocationRecord(
                    binding.value());
            QVERIFY(record.valid);
            const bool targetBinding =
                athleteCredential
                    ? record.directoryPath
                        == canonicalAthlete
                    : record.kind
                            == QStringLiteral("scope")
                        && record.directoryPath
                            == canonicalRoot;
            const bool rootBinding =
                removeRoot
                && record.kind
                    == QStringLiteral("root")
                && record.directoryPath
                    == canonicalRoot;
            if (targetBinding || rootBinding) {
                authority.remove(binding.key());
                legacyBindings.remove(binding.key());
            }
        }
        authority.sync();
        QCOMPARE(
            authority.status(), QSettings::NoError);
        QCOMPARE(
            settingsValuesWithPrefix(
                &authority, bindingPrefix),
            legacyBindings);
        QCOMPARE(
            legacyBindings.size(),
            athleteCredential ? 2
                              : (removeRoot ? 0 : 1));
        if (removeRoot) {
            for (auto binding =
                     legacyBindings.cbegin();
                 binding != legacyBindings.cend();
                 ++binding) {
                const TestLocationRecord record =
                    parseTestLocationRecord(
                        binding.value());
                QVERIFY(record.valid);
                QVERIFY(record.kind
                        != QStringLiteral("root"));
            }
        }
    }
    const QByteArray authorityBefore =
        fileContents(authority.fileName());

    {
        QSettings damaged(settingsPath, targetFormat);
        damaged.setFallbacksEnabled(false);
        if (removeRoot)
            damaged.remove(rootKey);
        if (removeBinding) {
            damaged.remove(localBindingKey);
        }
        if (removeScope) {
            damaged.remove(localScopeKey);
        }
        damaged.setValue(
            plaintextKey, replacementSecret);
        damaged.sync();
        QCOMPARE(
            damaged.status(),
            QSettings::NoError);
    }

    {
        GSettings rejected(
            organization, application,
            legacyFormat, targetFormat);
        rejected.initializeQSettingsGlobal(athleteRoot);
        if (athleteCredential) {
            rejected.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        }
        const QVariant observed = athleteCredential
            ? rejected.cvalue(
                  QStringLiteral("Athlete"),
                  GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : rejected.value(
                  nullptr, GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(
            observed.toString(),
            recoverable
                ? originalSecret
                : QStringLiteral("missing"));
        if (!recoverable) {
            const bool stored = athleteCredential
                ? rejected.setCValueChecked(
                      QStringLiteral("Athlete"),
                      GC_STRAVA_TOKEN,
                      QStringLiteral(
                          "must-not-be-stored"))
                : rejected.setValueChecked(
                      GC_NOLIO_ACCESS_TOKEN,
                      QStringLiteral(
                          "must-not-be-stored"));
            QVERIFY(!stored);
        }
    }
    QCOMPARE(factoryState()->values, vaultBefore);

    {
        QSettings retained(settingsPath, targetFormat);
        retained.setFallbacksEnabled(false);
        retained.sync();
        QCOMPARE(
            retained.contains(localBindingKey),
            recoverable || !removeBinding);
        QCOMPARE(
            retained.contains(localScopeKey),
            recoverable || !removeScope);
        if (recoverable) {
            QCOMPARE(
                retained.value(localBindingKey),
                originalBinding);
            QCOMPARE(
                retained.value(
                    localScopeKey).toString(),
                originalScopeId);
        } else {
            QCOMPARE(
                retained.value(
                    plaintextKey).toString(),
                replacementSecret);
            if (!removeBinding) {
                QCOMPARE(
                    retained.value(localBindingKey),
                    originalBinding);
            }
            if (!removeScope) {
                QCOMPARE(
                    retained.value(
                        localScopeKey).toString(),
                    originalScopeId);
            }
        }
    }
    {
        QSettings global(globalPath, targetFormat);
        global.setFallbacksEnabled(false);
        global.sync();
        QCOMPARE(global.contains(rootKey), !removeRoot);
        if (!removeRoot) {
            QCOMPARE(
                global.value(rootKey).toString(),
                originalRootId);
        }
    }
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority, claimPrefix),
        claimsBefore);
    QVERIFY(settingsValuesWithPrefix(
                &authority, enrollmentPrefix)
                .isEmpty());
    const QHash<QString, QVariant> expectedBindings =
        legacyClaimsOnly && !recoverable
            ? legacyBindings
            : completedBindings;
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority, bindingPrefix),
        expectedBindings);
    if (!legacyClaimsOnly || !recoverable) {
        QCOMPARE(
            fileContents(authority.fileName()),
            authorityBefore);
    }
}

void TestCredentialSettings::
legacyLocationClaimsAreBackfilled_data()
{
    QTest::addColumn<bool>("athleteCredential");
    QTest::newRow("global") << false;
    QTest::newRow("athlete") << true;
}

void TestCredentialSettings::
legacyLocationClaimsAreBackfilled()
{
    QFETCH(bool, athleteCredential);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    const QString legacyPath =
        temporary.filePath(QStringLiteral("legacy"));
    const QString authorityPath =
        temporary.filePath(QStringLiteral("authority"));
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString athleteConfig =
        QDir(athleteRoot).filePath(
            QStringLiteral("Athlete/config"));
    QVERIFY(QDir().mkpath(legacyPath));
    QVERIFY(QDir().mkpath(authorityPath));
    QVERIFY(QDir().mkpath(athleteConfig));
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        legacyPath);
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        authorityPath);

    const QString organization =
        QStringLiteral("CredentialClaimBackfill-")
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    const QString privatePath =
        QDir(athleteConfig).filePath(
            QStringLiteral(
                "athlete-private.ini"));
    const QString settingsPath = athleteCredential
        ? privatePath : globalPath;
    const QString plaintextKey = athleteCredential
        ? plainKey(GC_STRAVA_TOKEN)
        : plainKey(GC_NOLIO_ACCESS_TOKEN);
    const QString secret = athleteCredential
        ? QStringLiteral("backfilled-athlete-secret")
        : QStringLiteral("backfilled-global-secret");
    {
        QSettings plaintext(settingsPath, targetFormat);
        plaintext.setFallbacksEnabled(false);
        plaintext.setValue(plaintextKey, secret);
        plaintext.sync();
        QCOMPARE(
            plaintext.status(),
            QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings current(
            organization, application,
            legacyFormat, targetFormat);
        current.initializeQSettingsGlobal(athleteRoot);
        if (athleteCredential) {
            current.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        }
        const QVariant observed = athleteCredential
            ? current.cvalue(
                  QStringLiteral("Athlete"),
                  GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : current.value(
                  nullptr, GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(observed.toString(), secret);
    }
    const QHash<QString, QString> vaultBefore =
        factoryState()->values;

    QSettings authority(
        targetFormat, QSettings::UserScope,
        organization, application);
    authority.setFallbacksEnabled(false);
    authority.sync();
    const QString claimPrefix =
        QStringLiteral(
            "credential_store/location_claims/");
    const QString bindingPrefix =
        QStringLiteral(
            "credential_store/location_bindings/");
    const QHash<QString, QVariant> claimsBefore =
        settingsValuesWithPrefix(
            &authority, claimPrefix);
    QCOMPARE(
        claimsBefore.size(),
        athleteCredential ? 4 : 2);
    const QHash<QString, QVariant> bindingsBefore =
        settingsValuesWithPrefix(
            &authority, bindingPrefix);
    const QStringList bindingKeys =
        authority.allKeys().filter(bindingPrefix);
    QCOMPARE(
        bindingKeys.size(),
        athleteCredential ? 4 : 2);
    for (const QString &key : bindingKeys)
        authority.remove(key);
    authority.sync();
    QCOMPARE(
        authority.status(),
        QSettings::NoError);
    QVERIFY(settingsValuesWithPrefix(
                &authority, bindingPrefix)
                .isEmpty());

    {
        GSettings upgraded(
            organization, application,
            legacyFormat, targetFormat);
        upgraded.initializeQSettingsGlobal(athleteRoot);
        if (athleteCredential) {
            upgraded.initializeQSettingsAthlete(
                athleteRoot,
                QStringLiteral("Athlete"));
        }
        const QVariant observed = athleteCredential
            ? upgraded.cvalue(
                  QStringLiteral("Athlete"),
                  GC_STRAVA_TOKEN,
                  QStringLiteral("missing"))
            : upgraded.value(
                  nullptr, GC_NOLIO_ACCESS_TOKEN,
                  QStringLiteral("missing"));
        QCOMPARE(observed.toString(), secret);
    }
    QCOMPARE(factoryState()->values, vaultBefore);
    authority.sync();
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority, claimPrefix),
        claimsBefore);
    QCOMPARE(
        settingsValuesWithPrefix(
            &authority, bindingPrefix),
        bindingsBefore);
    QVERIFY(authority.allKeys()
                .filter(QStringLiteral(
                    "credential_store/"
                    "location_enrollments/"))
                .isEmpty());
}

void TestCredentialSettings::
newFormatRootlessCredentialIsRetained()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialMigration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString sentinel = QStringLiteral("legacy-password-sentinel");
    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, sentinel);
        legacy.sync();
        legacyPath = legacy.fileName();
    }

    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    const QString localScope =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    {
        QSettings privateSettings(
            athleteRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            localScope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_RWGPSPASS,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("missing"));
        settings.syncQSettings();
    }

    QSettings legacy(organization, application);
    QCOMPARE(legacy.value(legacyKey).toString(),
             sentinel);
    QVERIFY(fileContents(legacyPath).contains(
        sentinel.toUtf8()));
    const QString privatePath = athleteRoot
        + QStringLiteral("/Athlete/config/athlete-private.ini");
    QVERIFY(!fileContents(privatePath).contains(sentinel.toUtf8()));
    QVERIFY(factoryState()->values.isEmpty());
    QSettings privateSettings(
        privatePath, QSettings::IniFormat);
    QCOMPARE(privateSettings.value(
                 QStringLiteral(
                     "credential_store/id")).toString(),
             localScope);
}

void TestCredentialSettings::
newFormatRootlessCredentialRemainsAfterStoreRecovery()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialRetry-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString sentinel = QStringLiteral("retry-password-sentinel");
    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, sentinel);
        legacy.sync();
        legacyPath = legacy.fileName();
    }

    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    const QString localScope =
        QStringLiteral("55555555-5555-4555-8555-555555555555");
    {
        QSettings privateSettings(
            athleteRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            localScope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    factoryState() = std::make_shared<FakeStoreState>();
    factoryState()->failWrites = true;
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_RWGPSPASS,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("missing"));
        settings.syncQSettings();
    }

    {
        QSettings retained(organization, application);
        QVERIFY(retained.contains(legacyKey));
        QCOMPARE(retained.value(legacyKey).toString(), sentinel);
    }
    QVERIFY(fileContents(legacyPath).contains(sentinel.toUtf8()));
    verifyOwnerOnlyPermissions(legacyPath);
    QVERIFY(factoryState()->values.isEmpty());

    factoryState()->failWrites = false;
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_RWGPSPASS,
                     QStringLiteral("missing")).toString(),
                 QStringLiteral("missing"));
        settings.syncQSettings();
    }

    QSettings migrated(organization, application);
    QCOMPARE(migrated.value(legacyKey).toString(),
             sentinel);
    QVERIFY(fileContents(legacyPath).contains(
        sentinel.toUtf8()));
    QVERIFY(factoryState()->values.isEmpty());
    QSettings privateSettings(
        athleteRoot
            + QStringLiteral(
                "/Athlete/config/athlete-private.ini"),
        QSettings::IniFormat);
    QCOMPARE(privateSettings.value(
                 QStringLiteral(
                     "credential_store/id")).toString(),
                 localScope);
}

void TestCredentialSettings::
authorizedLegacyPlaintextRequiresAuthoritativeVaultMiss_data()
{
    QTest::addColumn<bool>("vaultUnavailable");
    QTest::newRow("vault-miss") << false;
    QTest::newRow("vault-unavailable") << true;
}

void TestCredentialSettings::
authorizedLegacyPlaintextRequiresAuthoritativeVaultMiss()
{
    using QSettings = LegacyMigrationQSettings;

    QFETCH(bool, vaultUnavailable);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialAuthorizedMigration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString secret =
        QStringLiteral("authorized-legacy-password");
    const QString scope =
        QStringLiteral("56565656-5656-4565-8565-565656565656");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.setValue(legacyKey, secret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(),
                 QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    factoryState()->failReads = vaultUnavailable;
    {
        GSettings settings(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(
            settings.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            vaultUnavailable
                ? QStringLiteral("missing")
                : secret);
        if (vaultUnavailable) {
            const QString updatedSecret =
                QStringLiteral(
                    "updated-authorized-legacy-password");
            QSettings legacy(organization, application);
            legacy.setValue(legacyKey, updatedSecret);
            legacy.sync();
            QCOMPARE(legacy.status(), QSettings::NoError);
            QCOMPARE(settings.cvalue(
                         athleteName, GC_RWGPSPASS,
                         QStringLiteral("missing")).toString(),
                     QStringLiteral("missing"));

            legacy.remove(legacyKey);
            legacy.sync();
            QCOMPARE(legacy.status(), QSettings::NoError);
            QCOMPARE(settings.cvalue(
                         athleteName, GC_RWGPSPASS,
                         QStringLiteral("missing")).toString(),
                     QStringLiteral("missing"));

            legacy.setValue(legacyKey, secret);
            legacy.sync();
            QCOMPARE(legacy.status(), QSettings::NoError);
            QCOMPARE(settings.cvalue(
                         athleteName, GC_RWGPSPASS,
                         QStringLiteral("missing")).toString(),
                     QStringLiteral("missing"));
        }
        settings.syncQSettings();
    }

    {
        GSettings retry(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        retry.initializeQSettingsGlobal(athleteRoot);
        retry.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(
            retry.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            vaultUnavailable
                ? QStringLiteral("missing")
                : secret);
        retry.syncQSettings();
    }

    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             secret);
    QVERIFY(!fileContents(privatePath).contains(
        secret.toUtf8()));
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
}

void TestCredentialSettings::
authorizedLegacyGlobalPlaintextRequiresAuthoritativeVaultMiss_data()
{
    QTest::addColumn<bool>("vaultUnavailable");
    QTest::newRow("vault-miss") << false;
    QTest::newRow("vault-unavailable") << true;
}

void TestCredentialSettings::
authorizedLegacyGlobalPlaintextRequiresAuthoritativeVaultMiss()
{
    using QSettings = LegacyMigrationQSettings;

    QFETCH(bool, vaultUnavailable);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialAuthorizedGlobal-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString legacyKey =
        plainKey(GC_NOLIO_REFRESH_TOKEN);
    const QString secret =
        QStringLiteral("authorized-global-refresh-token");
    const QString scope =
        QStringLiteral("96969696-9696-4696-8696-969696969696");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString globalPath =
        QDir(athleteRoot).filePath(
            QStringLiteral(
                "configglobal-general.ini"));
    QVERIFY(QDir().mkpath(athleteRoot));

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(QString()),
            scope);
        legacy.setValue(legacyKey, secret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }
    {
        QSettings global(
            globalPath, QSettings::IniFormat);
        global.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        global.sync();
        QCOMPARE(global.status(), QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(
                QString()),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    factoryState()->failReads = vaultUnavailable;
    {
        GSettings settings(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(
            settings.value(
                nullptr, GC_NOLIO_REFRESH_TOKEN,
                QStringLiteral("missing")).toString(),
            vaultUnavailable
                ? QStringLiteral("missing")
                : secret);
        settings.syncQSettings();
    }

    {
        GSettings retry(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        retry.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(
            retry.value(
                nullptr, GC_NOLIO_REFRESH_TOKEN,
                QStringLiteral("missing")).toString(),
            vaultUnavailable
                ? QStringLiteral("missing")
                : secret);
        retry.syncQSettings();
    }

    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             secret);
    QVERIFY(!fileContents(globalPath).contains(
        secret.toUtf8()));
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
}

void TestCredentialSettings::
targetCredentialUseBlocksLegacyFallback_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("canonical-read") << 0;
    QTest::newRow("explicit-write") << 1;
    QTest::newRow("explicit-delete") << 2;
    QTest::newRow("canonical-after-cached-miss") << 3;
    QTest::newRow("memory-only-read-does-not-commit-marker") << 4;
}

void TestCredentialSettings::
targetCredentialUseBlocksLegacyFallback()
{
    using QSettings = LegacyMigrationQSettings;

    QFETCH(int, action);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialTargetUse-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString legacySecret =
        QStringLiteral("older-legacy-password");
    const QString canonicalSecret =
        QStringLiteral("canonical-password");
    const QString replacementSecret =
        QStringLiteral("replacement-password");
    const QString scope =
        QStringLiteral("7e7e7e7e-7e7e-4e7e-8e7e-7e7e7e7e7e7e");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }
    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        target.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_RWGPSPASS);
    if (action != 1 && action != 3 && action != 4) {
        factoryState()->values.insert(
            vaultKey, canonicalSecret);
    }

    {
        GSettings settings(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);

        if (action == 0) {
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                canonicalSecret);
            factoryState()->values.remove(vaultKey);
        } else if (action == 1) {
            QVERIFY(settings.setCValueChecked(
                athleteName, GC_RWGPSPASS,
                replacementSecret));
            factoryState()->values.remove(vaultKey);
        } else if (action == 2) {
            QVERIFY(settings.setCValueChecked(
                athleteName, GC_RWGPSPASS, QString()));
        } else if (action == 3) {
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                legacySecret);
            factoryState()->values.insert(
                vaultKey, canonicalSecret);
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                canonicalSecret);
            factoryState()->values.remove(vaultKey);
        } else {
            factoryState()->failWrites = true;
            QVERIFY(!settings.setCValueChecked(
                athleteName, GC_RWGPSPASS,
                replacementSecret));
            factoryState()->failWrites = false;
            {
                QSettings target(
                    privatePath, QSettings::IniFormat);
                target.remove(
                    legacyFallbackBlockTestKey(
                        scope, GC_RWGPSPASS));
                target.sync();
                QCOMPARE(
                    target.status(),
                    QSettings::NoError);
            }
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                replacementSecret);
        }
    }

    QVERIFY(factoryState()->values.isEmpty());
    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        QCOMPARE(
            target.value(
                legacyFallbackBlockTestKey(
                    scope, GC_RWGPSPASS),
                false).toBool(),
            action != 4);
    }
    {
        QSettings retained(organization, application);
        QCOMPARE(retained.value(legacyKey).toString(),
                 legacySecret);
    }

    {
        GSettings restarted(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        restarted.initializeQSettingsGlobal(athleteRoot);
        restarted.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(
            restarted.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            QStringLiteral("missing"));
    }
}

void TestCredentialSettings::
targetPlaintextPrecedesLegacyAfterTransientVaultFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialTargetPrecedence-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString targetSecret =
        QStringLiteral("newer-target-password");
    const QString legacySecret =
        QStringLiteral("older-legacy-password");
    const QString scope =
        QStringLiteral("a1a1a1a1-a1a1-41a1-81a1-a1a1a1a1a1a1");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);

    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        target.setValue(plaintextKey, targetSecret);
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
    }
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    factoryState()->reads = 0;
    factoryState()->writes = 0;
    factoryState()->creates = 0;
    factoryState()->overwrites = 0;
    factoryState()->failNextReads = 1;
    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        QStringLiteral("missing"));
    QCOMPARE(factoryState()->creates, 0);
    QVERIFY(factoryState()->values.isEmpty());
    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        QCOMPARE(
            target.value(plaintextKey).toString(),
            targetSecret);
    }
    {
        QSettings legacy(organization, application);
        QCOMPARE(
            legacy.value(legacyKey).toString(),
            legacySecret);
    }

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        targetSecret);
    QCOMPARE(factoryState()->creates, 1);
    QCOMPARE(factoryState()->overwrites, 0);
    QCOMPARE(
        factoryState()->values.value(
            CredentialSettings::vaultKey(
                scope, GC_RWGPSPASS)),
        targetSecret);
    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        QVERIFY(!target.contains(plaintextKey));
    }
    {
        QSettings legacy(organization, application);
        QCOMPARE(
            legacy.value(legacyKey).toString(),
            legacySecret);
    }
}

void TestCredentialSettings::
emptyTargetPlaintextBlocksLegacyAcrossRestart_data()
{
    QTest::addColumn<bool>("preexistingTarget");
    QTest::newRow("preexisting-target") << true;
    QTest::newRow("late-target") << false;
}

void TestCredentialSettings::
emptyTargetPlaintextBlocksLegacyAcrossRestart()
{
    using QSettings = LegacyMigrationQSettings;

    QFETCH(bool, preexistingTarget);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialEmptyTarget-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString legacySecret =
        QStringLiteral("superseded-legacy-password");
    const QString scope =
        QStringLiteral("f6f6f6f6-f6f6-46f6-86f6-f6f6f6f6f6f6");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    if (preexistingTarget) {
        QSettings target(
            privatePath, QSettings::IniFormat);
        target.setValue(plaintextKey, QString());
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);

        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    factoryState()->reads = 0;
    factoryState()->writes = 0;
    factoryState()->creates = 0;
    factoryState()->overwrites = 0;
    {
        GSettings settings(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);

        if (!preexistingTarget) {
            QSettings target(
                privatePath, QSettings::IniFormat);
            target.setValue(plaintextKey, QString());
            target.sync();
            QCOMPARE(target.status(),
                     QSettings::NoError);

            QSettings legacy(organization, application);
            legacy.setValue(legacyKey, legacySecret);
            legacy.sync();
            QCOMPARE(legacy.status(),
                     QSettings::NoError);
        }

        for (int read = 0; read < 2; ++read) {
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                QStringLiteral("missing"));
        }
        QCOMPARE(factoryState()->creates, 0);
        QCOMPARE(factoryState()->overwrites, 0);
        QVERIFY(factoryState()->values.isEmpty());
    }

    {
        GSettings restarted(
            organization, application,
            legacyFormat.format(), QSettings::IniFormat);
        restarted.initializeQSettingsGlobal(athleteRoot);
        restarted.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(
            restarted.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            QStringLiteral("missing"));
    }
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
    {
        QSettings protectedTarget(
            privatePath, QSettings::IniFormat);
        QVERIFY(protectedTarget.value(
            legacyFallbackBlockTestKey(
                scope, GC_RWGPSPASS),
            false).toBool());
        QVERIFY(!protectedTarget.contains(plaintextKey));
    }
    {
        QSettings retained(organization, application);
        QCOMPARE(retained.value(legacyKey).toString(),
                 legacySecret);
    }
}

void TestCredentialSettings::
failedFallbackMarkerPersistenceFailsClosedAndRetries_data()
{
    QTest::addColumn<bool>("canonicalTarget");
    QTest::addColumn<bool>("sameProcessRetry");
    QTest::newRow("empty-target-restart")
        << false << false;
    QTest::newRow("canonical-target-restart")
        << true << false;
    QTest::newRow("canonical-target-same-process")
        << true << true;
}

void TestCredentialSettings::
failedFallbackMarkerPersistenceFailsClosedAndRetries()
{
    QFETCH(bool, canonicalTarget);
    QFETCH(bool, sameProcessRetry);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("CredentialFallbackMarker-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString legacySecret =
        QStringLiteral("superseded-legacy-password");
    const QString canonicalSecret =
        QStringLiteral("canonical-password");
    const QString scope =
        QStringLiteral("6d6d6d6d-6d6d-4d6d-8d6d-6d6d6d6d6d6d");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(
            legacyFormat,
            QSettings::UserScope,
            organization,
            application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings target(privatePath, targetFormat);
        target.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        target.sync();
        QCOMPARE(target.status(), QSettings::NoError);
    }
    {
        QSettings system(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_RWGPSPASS);
    MigrationFormatFaultState &fault =
        migrationFormatFaultState();
    fault = {};
    {
        GSettings settings(
            organization, application,
            legacyFormat, targetFormat);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        if (canonicalTarget) {
            factoryState()->values.insert(
                vaultKey, canonicalSecret);
        } else {
            QSettings target(privatePath, targetFormat);
            target.setValue(plaintextKey, QString());
            target.sync();
            QCOMPARE(target.status(), QSettings::NoError);
        }
        {
            QSettings legacy(
                legacyFormat,
                QSettings::UserScope,
                organization,
                application);
            legacy.setValue(legacyKey, legacySecret);
            legacy.sync();
            QCOMPARE(legacy.status(), QSettings::NoError);
        }

        fault.failurePoint =
            QStringLiteral("credential-fallback-block");
        fault.enabled = true;
        QCOMPARE(
            settings.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            QStringLiteral("missing"));
        QCOMPARE(factoryState()->creates, 0);
        QCOMPARE(factoryState()->overwrites, 0);
        if (canonicalTarget) {
            QCOMPARE(factoryState()->values.value(vaultKey),
                     canonicalSecret);
        } else {
            QVERIFY(factoryState()->values.isEmpty());
        }
        if (sameProcessRetry) {
            fault.enabled = false;
            QCOMPARE(
                settings.cvalue(
                    athleteName, GC_RWGPSPASS,
                    QStringLiteral("missing")).toString(),
                canonicalSecret);
        }
    }
    fault.enabled = false;
    QVERIFY(fault.rejectedWrites > 0);

    const QString blockKey = legacyFallbackBlockTestKey(
        scope, GC_RWGPSPASS);
    {
        bool readable = false;
        const QSettings::SettingsMap unprotectedTarget =
            persistedSettingsMap(privatePath, &readable);
        QVERIFY(readable);
        QCOMPARE(
            unprotectedTarget.contains(blockKey),
            sameProcessRetry);
        QCOMPARE(
            unprotectedTarget.contains(plaintextKey),
            !canonicalTarget);
        if (!canonicalTarget) {
            QCOMPARE(
                unprotectedTarget.value(
                    plaintextKey).toString(),
                QString());
        }
    }
    {
        QSettings retained(
            legacyFormat,
            QSettings::UserScope,
            organization,
            application);
        QCOMPARE(retained.value(legacyKey).toString(),
                 legacySecret);
    }

    {
        GSettings retry(
            organization, application,
            legacyFormat, targetFormat);
        retry.initializeQSettingsGlobal(athleteRoot);
        retry.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(
            retry.cvalue(
                athleteName, GC_RWGPSPASS,
                QStringLiteral("missing")).toString(),
            canonicalTarget
                ? canonicalSecret
                : QStringLiteral("missing"));
    }
    {
        bool readable = false;
        const QSettings::SettingsMap protectedTarget =
            persistedSettingsMap(privatePath, &readable);
        QVERIFY(readable);
        QVERIFY(protectedTarget.value(
            blockKey, false).toBool());
        QVERIFY(!protectedTarget.contains(plaintextKey));
    }
    {
        QSettings retained(
            legacyFormat,
            QSettings::UserScope,
            organization,
            application);
        QCOMPARE(retained.value(legacyKey).toString(),
                 legacySecret);
    }
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    if (canonicalTarget) {
        QCOMPARE(factoryState()->values.value(vaultKey),
                 canonicalSecret);
    } else {
        QVERIFY(factoryState()->values.isEmpty());
    }
    fault = {};
}

void TestCredentialSettings::
legacyPlaintextRequiresMatchingSourceRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLegacyRoot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString secret =
        QStringLiteral("different-root-password");
    const QString scope =
        QStringLiteral("b2b2b2b2-b2b2-42b2-82b2-b2b2b2b2b2b2");
    const QString activeRoot =
        temporary.filePath(QStringLiteral("active"));
    const QString otherRoot =
        temporary.filePath(QStringLiteral("other"));
    const QString privatePath = QDir(activeRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));
    QVERIFY(QDir().mkpath(otherRoot));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), otherRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), activeRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(activeRoot);
    settings.initializeQSettingsAthlete(
        activeRoot, athleteName);
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, secret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    factoryState()->reads = 0;
    factoryState()->writes = 0;
    factoryState()->creates = 0;
    factoryState()->overwrites = 0;

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        QStringLiteral("missing"));
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             secret);
}

void TestCredentialSettings::
legacyPlaintextRequiresMatchingSourceScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLegacyScope-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString secret =
        QStringLiteral("different-scope-password");
    const QString scope =
        QStringLiteral("d4d4d4d4-d4d4-44d4-84d4-d4d4d4d4d4d4");
    const QString otherScope =
        QStringLiteral("e5e5e5e5-e5e5-45e5-85e5-e5e5e5e5e5e5");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            otherScope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, secret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    factoryState()->reads = 0;
    factoryState()->writes = 0;
    factoryState()->creates = 0;
    factoryState()->overwrites = 0;

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        QStringLiteral("missing"));
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             secret);
}

void TestCredentialSettings::
legacyCredentialScopeUsesOneExactSnapshot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialScopeSnapshot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString originalScope =
        QStringLiteral("18181818-1818-4818-8818-181818181818");
    const QString changedScope =
        QStringLiteral("29292929-2929-4929-8929-292929292929");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString changedRoot =
        temporary.filePath(QStringLiteral("changed"));
    QVERIFY(QDir().mkpath(originalRoot));
    QVERIFY(QDir().mkpath(changedRoot));

    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), originalRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            originalScope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(originalRoot);

    GSettings::setCredentialLegacyScopeSnapshotHook(
        [organization, application, athleteName,
         changedRoot, changedScope] {
            QSettings changed(
                QSettings::IniFormat,
                QSettings::UserScope,
                organization,
                application);
            changed.setValue(
                plainKey(GC_HOMEDIR), changedRoot);
            changed.setValue(
                legacyCredentialScopeMappingKey(athleteName),
                changedScope);
            changed.sync();
        });

    QCOMPARE(
        settings.credentialLegacyScopeForTest(athleteName),
        originalScope);
}

void TestCredentialSettings::
authorizedLegacyFallbackUsesOneExactSnapshot()
{
    using QSettings = LegacyMigrationQSettings;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialValueSnapshot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString originalSecret =
        QStringLiteral("authorized-snapshot-password");
    const QString changedSecret =
        QStringLiteral("changed-after-authorization-password");
    const QString scope =
        QStringLiteral("3a3a3a3a-3a3a-4a3a-8a3a-3a3a3a3a3a3a");
    const QString changedScope =
        QStringLiteral("4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString changedRoot =
        temporary.filePath(QStringLiteral("changed"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));
    QVERIFY(QDir().mkpath(changedRoot));

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(
        organization, application,
        legacyFormat.format(), QSettings::IniFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, originalSecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
#ifndef Q_OS_WIN
        QVERIFY(QFile::setPermissions(
            legacyPath,
            QFileDevice::ReadOwner
            | QFileDevice::WriteOwner
            | QFileDevice::ReadGroup
            | QFileDevice::ReadOther));
#endif
    }

    GSettings::setCredentialLegacyValueSnapshotHook(
        [organization, application, athleteName,
         legacyKey, legacyPath, changedRoot, changedScope,
         changedSecret] {
            verifyOwnerOnlyPermissions(legacyPath);
            QSettings changed(organization, application);
            changed.setValue(
                plainKey(GC_HOMEDIR), changedRoot);
            changed.setValue(
                legacyCredentialScopeMappingKey(athleteName),
                changedScope);
            changed.setValue(legacyKey, changedSecret);
            changed.sync();
        });

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        originalSecret);
    verifyOwnerOnlyPermissions(legacyPath);
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());

    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             changedSecret);
}

void TestCredentialSettings::
targetAppearingDuringLegacyFallbackTakesPrecedence()
{
    using QSettings = LegacyMigrationQSettings;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLateTarget-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString plaintextKey = plainKey(GC_RWGPSPASS);
    const QString legacyKey =
        athleteName + QLatin1Char('/') + plaintextKey;
    const QString legacySecret =
        QStringLiteral("older-legacy-password");
    const QString targetSecret =
        QStringLiteral("newer-target-password");
    const QString scope =
        QStringLiteral("5c5c5c5c-5c5c-4c5c-8c5c-5c5c5c5c5c5c");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(
        organization, application,
        legacyFormat.format(), QSettings::IniFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }

    GSettings::setCredentialLegacyValueSnapshotHook(
        [privatePath, plaintextKey, targetSecret] {
            QSettings target(
                privatePath, QSettings::IniFormat);
            target.setValue(plaintextKey, targetSecret);
            target.sync();
        });

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        QStringLiteral("missing"));
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QVERIFY(factoryState()->values.isEmpty());
    {
        QSettings target(
            privatePath, QSettings::IniFormat);
        QCOMPARE(target.value(plaintextKey).toString(),
                 targetSecret);
    }
    {
        QSettings retained(organization, application);
        QCOMPARE(retained.value(legacyKey).toString(),
                 legacySecret);
    }

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        targetSecret);
    QCOMPARE(factoryState()->creates, 1);
    QCOMPARE(factoryState()->overwrites, 0);
    QCOMPARE(
        factoryState()->values.value(
            CredentialSettings::vaultKey(
                scope, GC_RWGPSPASS)),
        targetSecret);
    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             legacySecret);
}

void TestCredentialSettings::
canonicalVaultReadRetainsAuthorizedLegacyDuplicate()
{
    using QSettings = LegacyMigrationQSettings;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLegacyDuplicate-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString canonicalSecret =
        QStringLiteral("canonical-password");
    const QString duplicateSecret =
        canonicalSecret;
    const QString scope =
        QStringLiteral("c3c3c3c3-c3c3-43c3-83c3-c3c3c3c3c3c3");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    factoryState()->values.insert(
        CredentialSettings::vaultKey(
            scope, GC_RWGPSPASS),
        canonicalSecret);
    GSettings settings(
        organization, application,
        legacyFormat.format(), QSettings::IniFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, duplicateSecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        canonicalSecret);

    QSettings observed(organization, application);
    QCOMPARE(observed.value(legacyKey).toString(),
             duplicateSecret);
    QCOMPARE(
        factoryState()->values.value(
            CredentialSettings::vaultKey(
                scope, GC_RWGPSPASS)),
        canonicalSecret);
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QCOMPARE(factoryState()->removes, 0);
}

void TestCredentialSettings::
vanishedCanonicalRetainsAuthorizedLegacyDuplicate()
{
    using QSettings = LegacyMigrationQSettings;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ScopedLegacyMigrationFormat legacyFormat(temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLegacyVanished-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_RWGPSPASS);
    const QString canonicalSecret =
        QStringLiteral("vanishing-canonical-password");
    const QString duplicateSecret =
        QStringLiteral("retained-legacy-password");
    const QString scope =
        QStringLiteral("f6f6f6f6-f6f6-46f6-86f6-f6f6f6f6f6f6");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("athletes"));
    const QString privatePath = QDir(athleteRoot).filePath(
        QStringLiteral(
            "Athlete/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(privatePath).absolutePath()));

    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
    }
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        system.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        system.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            scope);
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    factoryState() =
        std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_RWGPSPASS);
    factoryState()->values.insert(
        vaultKey, canonicalSecret);
    factoryState()->removeAfterReadKey = vaultKey;
    GSettings settings(
        organization, application,
        legacyFormat.format(), QSettings::IniFormat);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);

    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, duplicateSecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_RWGPSPASS,
            QStringLiteral("missing")).toString(),
        canonicalSecret);
    QVERIFY(!factoryState()->values.contains(vaultKey));
    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             duplicateSecret);
    QCOMPARE(factoryState()->creates, 0);
    QCOMPARE(factoryState()->overwrites, 0);
    QCOMPARE(factoryState()->removes, 0);
}

void TestCredentialSettings::
globalCredentialsInDifferentRootsHaveIsolatedScopes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialGlobalRootIsolation-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString firstSecret =
        QStringLiteral("first-root-global-token");
    const QString secondSecret =
        QStringLiteral("second-root-global-token");
    const QString missing = QStringLiteral("missing");
    const QString firstRoot =
        temporary.filePath(QStringLiteral("first-library"));
    const QString secondRoot =
        temporary.filePath(QStringLiteral("second-library"));
    QVERIFY(QDir().mkpath(firstRoot));
    QVERIFY(QDir().mkpath(secondRoot));

    factoryState() = std::make_shared<FakeStoreState>();
    QString firstScope;
    {
        GSettings first(organization, application);
        first.initializeQSettingsGlobal(firstRoot);
        QVERIFY(first.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN, firstSecret));
        QSettings globalSettings(
            firstRoot
                + QStringLiteral("/configglobal-general.ini"),
            QSettings::IniFormat);
        firstScope = globalSettings.value(
            QStringLiteral("credential_store/id")).toString();
        QVERIFY(!QUuid(firstScope).isNull());
    }

    QString secondScope;
    {
        GSettings second(organization, application);
        second.initializeQSettingsGlobal(secondRoot);
        QCOMPARE(second.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 missing);
        QVERIFY(second.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN, secondSecret));
        QSettings globalSettings(
            secondRoot
                + QStringLiteral("/configglobal-general.ini"),
            QSettings::IniFormat);
        secondScope = globalSettings.value(
            QStringLiteral("credential_store/id")).toString();
        QVERIFY(!QUuid(secondScope).isNull());
        QVERIFY(secondScope != firstScope);
    }

    const QString firstVaultKey = CredentialSettings::vaultKey(
        firstScope, GC_NOLIO_ACCESS_TOKEN);
    const QString secondVaultKey = CredentialSettings::vaultKey(
        secondScope, GC_NOLIO_ACCESS_TOKEN);
    QCOMPARE(factoryState()->values.value(firstVaultKey),
             firstSecret);
    QCOMPARE(factoryState()->values.value(secondVaultKey),
             secondSecret);

    {
        GSettings firstRestart(organization, application);
        firstRestart.initializeQSettingsGlobal(firstRoot);
        QCOMPARE(firstRestart.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 firstSecret);
    }
}

void TestCredentialSettings::
sameNamedAthletesInDifferentRootsHaveIsolatedScopes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialRootIsolation-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString firstSecret =
        QStringLiteral("first-root-refresh-token");
    const QString secondSecret =
        QStringLiteral("second-root-refresh-token");
    const QString missing = QStringLiteral("missing");
    const QString firstRoot =
        temporary.filePath(QStringLiteral("first-library"));
    const QString secondRoot =
        temporary.filePath(QStringLiteral("second-library"));
    QVERIFY(QDir().mkpath(
        firstRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        secondRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    QString firstScope;
    {
        GSettings first(organization, application);
        first.initializeQSettingsGlobal(firstRoot);
        first.initializeQSettingsAthlete(
            firstRoot, athleteName);
        QVERIFY(first.setCValueChecked(
            athleteName, GC_STRAVA_REFRESH_TOKEN,
            firstSecret));
        QSettings privateSettings(
            firstRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        firstScope = privateSettings.value(
            QStringLiteral("credential_store/id")).toString();
        QVERIFY(!firstScope.isEmpty());
        QVERIFY(!QUuid(firstScope).isNull());
    }

    QString secondScope;
    {
        GSettings second(organization, application);
        second.initializeQSettingsGlobal(secondRoot);
        second.initializeQSettingsAthlete(
            secondRoot, athleteName);
        QCOMPARE(second.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
        QVERIFY(second.setCValueChecked(
            athleteName, GC_STRAVA_REFRESH_TOKEN,
            secondSecret));
        QSettings privateSettings(
            secondRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        secondScope = privateSettings.value(
            QStringLiteral("credential_store/id")).toString();
        QVERIFY(!secondScope.isEmpty());
        QVERIFY(!QUuid(secondScope).isNull());
        QVERIFY(secondScope != firstScope);
    }

    const QString firstVaultKey = CredentialSettings::vaultKey(
        firstScope, GC_STRAVA_REFRESH_TOKEN);
    const QString secondVaultKey = CredentialSettings::vaultKey(
        secondScope, GC_STRAVA_REFRESH_TOKEN);
    QVERIFY(firstVaultKey != secondVaultKey);
    QCOMPARE(factoryState()->values.value(firstVaultKey),
             firstSecret);
    QCOMPARE(factoryState()->values.value(secondVaultKey),
             secondSecret);

    {
        GSettings firstRestart(organization, application);
        firstRestart.initializeQSettingsGlobal(firstRoot);
        QCOMPARE(firstRestart.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 firstSecret);
        firstRestart.initializeQSettingsAthlete(
            firstRoot, athleteName);
        QCOMPARE(firstRestart.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 firstSecret);
    }

    {
        GSettings secondRestart(organization, application);
        secondRestart.initializeQSettingsGlobal(secondRoot);
        secondRestart.initializeQSettingsAthlete(
            secondRoot, athleteName);
        QCOMPARE(secondRestart.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 secondSecret);
    }
}

void TestCredentialSettings::
preUpgradeCopiedCredentialScopesFailClosed_data()
{
    QTest::addColumn<bool>("openCopyFirst");
    QTest::newRow("original-first") << false;
    QTest::newRow("copy-first") << true;
}

void TestCredentialSettings::
preUpgradeCopiedCredentialScopesFailClosed()
{
    QFETCH(bool, openCopyFirst);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialPreUpgradeClone-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString missing = QStringLiteral("missing");
    const QString globalSecret =
        QStringLiteral("pre-upgrade-global-token");
    const QString athleteSecret =
        QStringLiteral("pre-upgrade-athlete-token");
    const QString globalScope =
        QStringLiteral("41414141-4141-4414-8414-414141414141");
    const QString athleteScope =
        QStringLiteral("42424242-4242-4424-8424-424242424242");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString copiedRoot =
        temporary.filePath(QStringLiteral("copy"));
    const QString relativePrivate =
        QStringLiteral("/Athlete/config/athlete-private.ini");
    const QString relativeGlobal =
        QStringLiteral("/configglobal-general.ini");
    QVERIFY(QDir().mkpath(
        originalRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        copiedRoot + QStringLiteral("/Athlete/config")));

    {
        QSettings global(
            originalRoot + relativeGlobal,
            QSettings::IniFormat);
        global.setValue(
            QStringLiteral("credential_store/id"),
            globalScope);
        global.sync();
        QCOMPARE(global.status(), QSettings::NoError);

        QSettings athlete(
            originalRoot + relativePrivate,
            QSettings::IniFormat);
        athlete.setValue(
            QStringLiteral("credential_store/id"),
            athleteScope);
        athlete.sync();
        QCOMPARE(athlete.status(), QSettings::NoError);
    }
    QVERIFY(QFile::copy(
        originalRoot + relativeGlobal,
        copiedRoot + relativeGlobal));
    QVERIFY(QFile::copy(
        originalRoot + relativePrivate,
        copiedRoot + relativePrivate));
    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), originalRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(QString()),
            globalScope);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            athleteScope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    factoryState()->values.insert(
        CredentialSettings::vaultKey(
            globalScope, GC_NOLIO_ACCESS_TOKEN),
        globalSecret);
    factoryState()->values.insert(
        CredentialSettings::vaultKey(
            athleteScope, GC_STRAVA_REFRESH_TOKEN),
        athleteSecret);
    const QHash<QString, QString> before =
        factoryState()->values;

    const QString firstRoot =
        openCopyFirst ? copiedRoot : originalRoot;
    const QString secondRoot =
        openCopyFirst ? originalRoot : copiedRoot;
    {
        GSettings first(organization, application);
        first.initializeQSettingsGlobal(firstRoot);
        first.initializeQSettingsAthlete(
            firstRoot, athleteName);
        QCOMPARE(first.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 openCopyFirst ? missing : globalSecret);
        QCOMPARE(first.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 openCopyFirst ? missing : athleteSecret);
        if (openCopyFirst) {
            QVERIFY(!first.setValueChecked(
                GC_NOLIO_ACCESS_TOKEN,
                QStringLiteral("must-not-be-stored")));
            QVERIFY(!first.setCValueChecked(
                athleteName, GC_STRAVA_REFRESH_TOKEN,
                QStringLiteral("must-not-be-stored")));
        }
    }

    {
        GSettings second(organization, application);
        second.initializeQSettingsGlobal(secondRoot);
        second.initializeQSettingsAthlete(
            secondRoot, athleteName);
        QCOMPARE(second.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 openCopyFirst ? globalSecret : missing);
        QCOMPARE(second.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 openCopyFirst ? athleteSecret : missing);
        if (!openCopyFirst) {
            QVERIFY(!second.setValueChecked(
                GC_NOLIO_ACCESS_TOKEN,
                QStringLiteral("must-not-be-stored")));
            QVERIFY(!second.setCValueChecked(
                athleteName, GC_STRAVA_REFRESH_TOKEN,
                QStringLiteral("must-not-be-stored")));
        }
    }
    QCOMPARE(factoryState()->values, before);
}

void TestCredentialSettings::
preUpgradeCopiedAthleteScopeFailsClosedWhenOpenedFirst()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialPreUpgradeProfileClone-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString originalName = QStringLiteral("Athlete");
    const QString copiedName = QStringLiteral("Clone");
    const QString missing = QStringLiteral("missing");
    const QString scope =
        QStringLiteral("45454545-4545-4454-8454-454545454545");
    const QString secret =
        QStringLiteral("pre-upgrade-profile-token");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString originalPrivate = QDir(athleteRoot).filePath(
        QStringLiteral("Athlete/config/athlete-private.ini"));
    const QString copiedPrivate = QDir(athleteRoot).filePath(
        QStringLiteral("Clone/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(QFileInfo(originalPrivate).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(copiedPrivate).absolutePath()));
    {
        QSettings original(
            originalPrivate, QSettings::IniFormat);
        original.setValue(
            QStringLiteral("credential_store/id"), scope);
        original.sync();
        QCOMPARE(original.status(), QSettings::NoError);
    }
    QVERIFY(QFile::copy(originalPrivate, copiedPrivate));
    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(originalName),
            scope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_REFRESH_TOKEN);
    factoryState()->values.insert(vaultKey, secret);
    const QHash<QString, QString> before =
        factoryState()->values;

    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, copiedName);
    QCOMPARE(settings.cvalue(
                 copiedName, GC_STRAVA_REFRESH_TOKEN,
                 missing).toString(),
             missing);
    QVERIFY(!settings.setCValueChecked(
        copiedName, GC_STRAVA_REFRESH_TOKEN,
        QStringLiteral("must-not-be-stored")));

    settings.initializeQSettingsAthlete(
        athleteRoot, originalName);
    QCOMPARE(settings.cvalue(
                 originalName, GC_STRAVA_REFRESH_TOKEN,
                 missing).toString(),
             secret);
    QCOMPARE(factoryState()->values, before);
}

void TestCredentialSettings::
legacyClaimsSurviveLocalBindingWriteFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QSettings::Format legacyFormat =
        legacyMigrationTestFormat();
    const QSettings::Format targetFormat =
        targetMigrationTestFormat();
    QVERIFY(legacyFormat != QSettings::InvalidFormat);
    QVERIFY(targetFormat != QSettings::InvalidFormat);
    QSettings::setPath(
        legacyFormat, QSettings::UserScope,
        temporary.path());
    QSettings::setPath(
        targetFormat, QSettings::UserScope,
        temporary.path());

    const QString organization =
        QStringLiteral("CredentialBindingRecovery-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString originalName = QStringLiteral("Athlete");
    const QString copiedName = QStringLiteral("Clone");
    const QString missing = QStringLiteral("missing");
    const QString scope =
        QStringLiteral("57575757-5757-4575-8575-575757575757");
    const QString secret =
        QStringLiteral("binding-recovery-token");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString originalPrivate = QDir(athleteRoot).filePath(
        QStringLiteral("Athlete/config/athlete-private.ini"));
    const QString copiedPrivate = QDir(athleteRoot).filePath(
        QStringLiteral("Clone/config/athlete-private.ini"));
    QVERIFY(QDir().mkpath(
        QFileInfo(originalPrivate).absolutePath()));
    QVERIFY(QDir().mkpath(
        QFileInfo(copiedPrivate).absolutePath()));
    {
        QSettings original(
            originalPrivate, targetFormat);
        original.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        original.sync();
        QCOMPARE(original.status(), QSettings::NoError);
    }
    {
        QSettings systemSettings(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(
                originalName),
            scope);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(
                copiedName),
            scope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(),
                 QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_REFRESH_TOKEN);
    factoryState()->values.insert(vaultKey, secret);
    const QHash<QString, QString> before =
        factoryState()->values;
    MigrationFormatFaultState &fault =
        migrationFormatFaultState();
    fault = {};
    fault.failurePoint =
        QStringLiteral("credential-binding");
    {
        GSettings original(
            organization, application,
            legacyFormat, targetFormat);
        original.initializeQSettingsGlobal(athleteRoot);
        fault.enabled = true;
        original.initializeQSettingsAthlete(
            athleteRoot, originalName);
        QCOMPARE(original.cvalue(
                     originalName,
                     GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
        QSettings liveClaims(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        const QStringList liveClaimKeys =
            liveClaims.allKeys().filter(
                QStringLiteral(
                    "credential_store/location_claims/"));
        QVERIFY2(
            liveClaimKeys.size() >= 3,
            qPrintable(QStringLiteral(
                "Expected live root, profile, and scope claims; found %1")
                           .arg(liveClaimKeys.size())));
    }
    fault.enabled = false;
    QVERIFY(fault.rejectedWrites > 0);
    {
        QSettings claims(
            targetFormat,
            QSettings::UserScope,
            organization,
            application);
        QCOMPARE(
            claims.value(
                plainKey(GC_HOMEDIR)).toString(),
            athleteRoot);
        QCOMPARE(
            claims.value(
                legacyCredentialScopeMappingKey(
                    originalName)).toString(),
            scope);
        const QStringList claimKeys =
            claims.allKeys().filter(
                QStringLiteral(
                    "credential_store/location_claims/"));
        QVERIFY2(
            claimKeys.size() >= 3,
            qPrintable(QStringLiteral(
                "Expected root, profile, and scope claims; found %1")
                           .arg(claimKeys.size())));
    }
    {
        QSettings incomplete(
            originalPrivate, targetFormat);
        QCOMPARE(
            incomplete.value(
                QStringLiteral(
                    "credential_store/id")).toString(),
            scope);
        QVERIFY(!incomplete.contains(
            QStringLiteral(
                "credential_store/binding_v2")));
    }
    QVERIFY(QFile::copy(
        originalPrivate, copiedPrivate));

    {
        GSettings copied(
            organization, application,
            legacyFormat, targetFormat);
        copied.initializeQSettingsGlobal(athleteRoot);
        copied.initializeQSettingsAthlete(
            athleteRoot, copiedName);
        QCOMPARE(copied.cvalue(
                     copiedName,
                     GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
        QVERIFY(!copied.setCValueChecked(
            copiedName, GC_STRAVA_REFRESH_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QCOMPARE(factoryState()->values, before);

    {
        GSettings original(
            organization, application,
            legacyFormat, targetFormat);
        original.initializeQSettingsGlobal(athleteRoot);
        original.initializeQSettingsAthlete(
            athleteRoot, originalName);
        QCOMPARE(original.cvalue(
                     originalName,
                     GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 secret);
    }
    QCOMPARE(factoryState()->values, before);
    fault = {};
}

void TestCredentialSettings::
switchingCredentialRootsClearsScopedState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialRootSwitch-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString firstSecret =
        QStringLiteral("first-root-switch-token");
    const QString secondSecret =
        QStringLiteral("second-root-switch-token");
    const QString missing = QStringLiteral("missing");
    const QString firstRoot =
        temporary.filePath(QStringLiteral("first-library"));
    const QString secondRoot =
        temporary.filePath(QStringLiteral("second-library"));
    QVERIFY(QDir().mkpath(
        firstRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        secondRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(firstRoot);
    settings.initializeQSettingsAthlete(
        firstRoot, athleteName);
    QVERIFY(settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN, firstSecret));

    settings.clearGlobalAndAthletes();
    settings.initializeQSettingsGlobal(secondRoot);
    settings.initializeQSettingsAthlete(
        secondRoot, athleteName);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 missing).toString(),
             missing);
    QVERIFY(settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN, secondSecret));

    settings.clearGlobalAndAthletes();
    settings.initializeQSettingsGlobal(firstRoot);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 missing).toString(),
             firstSecret);
    settings.initializeQSettingsAthlete(
        firstRoot, athleteName);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 missing).toString(),
             firstSecret);
}

void TestCredentialSettings::
invalidCredentialRootSwitchFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialInvalidRootSwitch-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString globalSecret =
        QStringLiteral("active-root-global-token");
    const QString athleteSecret =
        QStringLiteral("active-root-athlete-token");
    const QString missing = QStringLiteral("missing");
    const QString validRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString invalidRoot =
        temporary.filePath(QStringLiteral("missing-library"));
    QVERIFY(QDir().mkpath(
        validRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(validRoot);
    settings.initializeQSettingsAthlete(
        validRoot, athleteName);
    QVERIFY(settings.setValueChecked(
        GC_NOLIO_ACCESS_TOKEN, globalSecret));
    QVERIFY(settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN,
        athleteSecret));
    const QHash<QString, QString> before =
        factoryState()->values;

    settings.initializeQSettingsGlobal(invalidRoot);
    QVERIFY(!settings.setValueChecked(
        GC_NOLIO_ACCESS_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(!settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QCOMPARE(settings.value(
                 nullptr, GC_NOLIO_ACCESS_TOKEN,
                 missing).toString(),
             missing);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 missing).toString(),
             missing);
    QCOMPARE(factoryState()->values, before);

    settings.initializeQSettingsGlobal(validRoot);
    settings.initializeQSettingsAthlete(
        validRoot, athleteName);
    QCOMPARE(settings.value(
                 nullptr, GC_NOLIO_ACCESS_TOKEN,
                 missing).toString(),
             globalSecret);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 missing).toString(),
             athleteSecret);
}

void TestCredentialSettings::
copiedCredentialRootFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialRootClone-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString globalSecret =
        QStringLiteral("original-root-global-token");
    const QString athleteSecret =
        QStringLiteral("original-root-athlete-token");
    const QString missing = QStringLiteral("missing");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString copiedRoot =
        temporary.filePath(QStringLiteral("copied"));
    QVERIFY(QDir().mkpath(
        originalRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(originalRoot);
        original.initializeQSettingsAthlete(
            originalRoot, athleteName);
        QVERIFY(original.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN, globalSecret));
        QVERIFY(original.setCValueChecked(
            athleteName, GC_STRAVA_TOKEN,
            athleteSecret));
        original.syncQSettings();
    }
    const QHash<QString, QString> before =
        factoryState()->values;

    QVERIFY(QDir().mkpath(
        copiedRoot + QStringLiteral("/Athlete/config")));
    const auto copyFiles = [](
        const QString &source,
        const QString &target) {
        const QDir sourceDirectory(source);
        for (const QString &fileName :
             sourceDirectory.entryList(
                 QDir::Files | QDir::Hidden
                     | QDir::System)) {
            if (!QFile::copy(
                    sourceDirectory.filePath(fileName),
                    QDir(target).filePath(fileName))) {
                return false;
            }
        }
        return true;
    };
    QVERIFY(copyFiles(originalRoot, copiedRoot));
    QVERIFY(copyFiles(
        originalRoot + QStringLiteral("/Athlete/config"),
        copiedRoot + QStringLiteral("/Athlete/config")));

    {
        GSettings copied(organization, application);
        copied.initializeQSettingsGlobal(copiedRoot);
        copied.initializeQSettingsAthlete(
            copiedRoot, athleteName);
        QCOMPARE(copied.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 missing);
        QCOMPARE(copied.cvalue(
                     athleteName, GC_STRAVA_TOKEN,
                     missing).toString(),
                 missing);
        QVERIFY(!copied.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("must-not-be-stored")));
        QVERIFY(!copied.setCValueChecked(
            athleteName, GC_STRAVA_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QCOMPARE(factoryState()->values, before);

    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(originalRoot);
        original.initializeQSettingsAthlete(
            originalRoot, athleteName);
        QCOMPARE(original.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 globalSecret);
        QCOMPARE(original.cvalue(
                     athleteName, GC_STRAVA_TOKEN,
                     missing).toString(),
                 athleteSecret);
    }
}

void TestCredentialSettings::
copiedAthleteCredentialProfileFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialProfileClone-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString originalName = QStringLiteral("Athlete");
    const QString copiedName = QStringLiteral("Clone");
    const QString secret =
        QStringLiteral("original-profile-token");
    const QString missing = QStringLiteral("missing");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString originalConfig = athleteRoot
        + QStringLiteral("/Athlete/config");
    const QString copiedConfig = athleteRoot
        + QStringLiteral("/Clone/config");
    QVERIFY(QDir().mkpath(originalConfig));
    QVERIFY(QDir().mkpath(copiedConfig));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, originalName);
    QVERIFY(settings.setCValueChecked(
        originalName, GC_STRAVA_TOKEN, secret));
    settings.syncQSettings();
    const QString privateFile =
        QStringLiteral("/athlete-private.ini");
    QVERIFY(QFile::copy(
        originalConfig + privateFile,
        copiedConfig + privateFile));
    const QHash<QString, QString> before =
        factoryState()->values;

    settings.initializeQSettingsAthlete(
        athleteRoot, copiedName);
    QCOMPARE(settings.cvalue(
                 copiedName, GC_STRAVA_TOKEN,
                 missing).toString(),
             missing);
    QVERIFY(!settings.setCValueChecked(
        copiedName, GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QCOMPARE(factoryState()->values, before);
    QCOMPARE(settings.cvalue(
                 originalName, GC_STRAVA_TOKEN,
                 missing).toString(),
             secret);
}

void TestCredentialSettings::
existingFreshRootCannotBootstrapMissingClaims()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialFreshRootClaims-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString secret =
        QStringLiteral("fresh-root-token");
    const QString missing = QStringLiteral("missing");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString copiedRoot =
        temporary.filePath(QStringLiteral("copied"));
    QVERIFY(QDir().mkpath(originalRoot));
    QVERIFY(QDir().mkpath(copiedRoot));

    factoryState() =
        std::make_shared<FakeStoreState>();
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(originalRoot);
        QVERIFY(original.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN, secret));
        original.syncQSettings();
    }
    const QHash<QString, QString> before =
        factoryState()->values;

    const QDir source(originalRoot);
    for (const QString &fileName :
         source.entryList(
             QDir::Files | QDir::Hidden
                 | QDir::System)) {
        QVERIFY(QFile::copy(
            source.filePath(fileName),
            QDir(copiedRoot).filePath(fileName)));
    }

    QHash<QString, QVariant> claims;
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        const QString prefix =
            QStringLiteral(
                "credential_store/location_claims/");
        for (const QString &key : system.allKeys()) {
            if (key.startsWith(prefix))
                claims.insert(key, system.value(key));
        }
        QVERIFY(claims.size() >= 2);
        for (auto claim = claims.cbegin();
             claim != claims.cend(); ++claim) {
            system.remove(claim.key());
        }
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    {
        GSettings copied(organization, application);
        copied.initializeQSettingsGlobal(copiedRoot);
        QCOMPARE(
            copied.value(
                nullptr, GC_NOLIO_ACCESS_TOKEN,
                missing).toString(),
            missing);
        QVERIFY(!copied.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QCOMPARE(factoryState()->values, before);

    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        const QString prefix =
            QStringLiteral(
                "credential_store/location_claims/");
        QVERIFY(system.allKeys().filter(prefix).isEmpty());
        for (auto claim = claims.cbegin();
             claim != claims.cend(); ++claim) {
            system.setValue(
                claim.key(), claim.value());
        }
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(originalRoot);
        QCOMPARE(
            original.value(
                nullptr, GC_NOLIO_ACCESS_TOKEN,
                missing).toString(),
            secret);
    }
}

void TestCredentialSettings::
existingFreshAthleteCannotBootstrapMissingClaims()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialFreshAthleteClaims-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString originalName = QStringLiteral("Athlete");
    const QString copiedName = QStringLiteral("Clone");
    const QString secret =
        QStringLiteral("fresh-athlete-token");
    const QString missing = QStringLiteral("missing");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString originalPath =
        QDir(athleteRoot).filePath(originalName);
    const QString copiedPath =
        QDir(athleteRoot).filePath(copiedName);
    const QString originalConfig =
        QDir(originalPath).filePath(
            QStringLiteral("config"));
    const QString copiedConfig =
        QDir(copiedPath).filePath(
            QStringLiteral("config"));
    QVERIFY(QDir().mkpath(originalConfig));
    QVERIFY(QDir().mkpath(copiedConfig));

    factoryState() =
        std::make_shared<FakeStoreState>();
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(athleteRoot);
        original.initializeQSettingsAthlete(
            athleteRoot, originalName);
        QVERIFY(original.setCValueChecked(
            originalName, GC_STRAVA_TOKEN, secret));
        original.syncQSettings();
    }
    const QHash<QString, QString> before =
        factoryState()->values;
    QVERIFY(QFile::copy(
        QDir(originalConfig).filePath(
            QStringLiteral("athlete-private.ini")),
        QDir(copiedConfig).filePath(
            QStringLiteral("athlete-private.ini"))));

    QHash<QString, QVariant> athleteClaims;
    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        const QString prefix =
            QStringLiteral(
                "credential_store/location_claims/");
        const QString canonicalOriginal =
            QFileInfo(originalPath).canonicalFilePath();
        for (const QString &key : system.allKeys()) {
            if (!key.startsWith(prefix))
                continue;
            const QJsonDocument document =
                QJsonDocument::fromJson(
                    system.value(key).toByteArray());
            if (document.isObject()
                && document.object().value(
                    QStringLiteral(
                        "directory_path")).toString()
                    == canonicalOriginal) {
                athleteClaims.insert(
                    key, system.value(key));
            }
        }
        QCOMPARE(athleteClaims.size(), 2);
        for (auto claim = athleteClaims.cbegin();
             claim != athleteClaims.cend(); ++claim) {
            system.remove(claim.key());
        }
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }

    {
        GSettings copied(organization, application);
        copied.initializeQSettingsGlobal(athleteRoot);
        copied.initializeQSettingsAthlete(
            athleteRoot, copiedName);
        QCOMPARE(
            copied.cvalue(
                copiedName, GC_STRAVA_TOKEN,
                missing).toString(),
            missing);
        QVERIFY(!copied.setCValueChecked(
            copiedName, GC_STRAVA_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QCOMPARE(factoryState()->values, before);

    {
        QSettings system(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        const QString canonicalCopied =
            QFileInfo(copiedPath).canonicalFilePath();
        for (const QString &key : system.allKeys()) {
            if (!key.startsWith(
                    QStringLiteral(
                        "credential_store/location_claims/"))) {
                continue;
            }
            const QJsonDocument document =
                QJsonDocument::fromJson(
                    system.value(key).toByteArray());
            QVERIFY(
                !document.isObject()
                || document.object().value(
                    QStringLiteral(
                        "directory_path")).toString()
                    != canonicalCopied);
        }
        for (auto claim = athleteClaims.cbegin();
             claim != athleteClaims.cend(); ++claim) {
            system.setValue(
                claim.key(), claim.value());
        }
        system.sync();
        QCOMPARE(system.status(), QSettings::NoError);
    }
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(athleteRoot);
        original.initializeQSettingsAthlete(
            athleteRoot, originalName);
        QCOMPARE(
            original.cvalue(
                originalName, GC_STRAVA_TOKEN,
                missing).toString(),
            secret);
    }
}

void TestCredentialSettings::
unavailableClaimedRootCannotBeReboundByCopy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialUnavailableRoot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString secret =
        QStringLiteral("unavailable-root-token");
    const QString missing = QStringLiteral("missing");
    const QString originalRoot =
        temporary.filePath(QStringLiteral("original"));
    const QString copiedRoot =
        temporary.filePath(QStringLiteral("copy"));
    const QString offlineRoot =
        temporary.filePath(QStringLiteral("offline-original"));
    const QString globalFile =
        QStringLiteral("/configglobal-general.ini");
    QVERIFY(QDir().mkpath(originalRoot));
    QVERIFY(QDir().mkpath(copiedRoot));

    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings original(organization, application);
        original.initializeQSettingsGlobal(originalRoot);
        QVERIFY(original.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN, secret));
        original.syncQSettings();
    }
    QVERIFY(QFile::copy(
        originalRoot + globalFile,
        copiedRoot + globalFile));
    const QHash<QString, QString> before =
        factoryState()->values;
    QVERIFY(QDir().rename(
        originalRoot, offlineRoot));
    QVERIFY(!QFileInfo::exists(originalRoot));

    {
        GSettings copied(organization, application);
        copied.initializeQSettingsGlobal(copiedRoot);
        QCOMPARE(copied.value(
                     nullptr, GC_NOLIO_ACCESS_TOKEN,
                     missing).toString(),
                 missing);
        QVERIFY(!copied.setValueChecked(
            GC_NOLIO_ACCESS_TOKEN,
            QStringLiteral("must-not-be-stored")));
    }
    QCOMPARE(factoryState()->values, before);
}

void TestCredentialSettings::
symlinkedGlobalCredentialSettingsFailClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("This regression requires Unix symlinks");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialGlobalSymlink-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString scope =
        QStringLiteral("43434343-4343-4434-8434-434343434343");
    const QString secret =
        QStringLiteral("outside-global-token");
    const QString missing = QStringLiteral("missing");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString outsidePath =
        temporary.filePath(QStringLiteral("outside-global.ini"));
    const QString linkedPath =
        QDir(athleteRoot).filePath(
            QStringLiteral("configglobal-general.ini"));
    QVERIFY(QDir().mkpath(athleteRoot));
    {
        QSettings outside(
            outsidePath, QSettings::IniFormat);
        outside.setValue(
            QStringLiteral("credential_store/id"),
            scope);
        outside.setValue(
            QStringLiteral("sentinel"),
            QStringLiteral("unchanged"));
        outside.sync();
        QCOMPARE(outside.status(), QSettings::NoError);
    }
    const QByteArray before =
        fileContents(outsidePath);
    QVERIFY(QFile::link(outsidePath, linkedPath));
    QVERIFY(QFileInfo(linkedPath).isSymLink());

    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey =
        CredentialSettings::vaultKey(
            scope, GC_NOLIO_ACCESS_TOKEN);
    factoryState()->values.insert(vaultKey, secret);

    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    QCOMPARE(settings.value(
                 nullptr, GC_LANG,
                 QStringLiteral("fallback-language"))
                 .toString(),
             QStringLiteral("fallback-language"));
    QVERIFY(!settings.setValueChecked(
        GC_LANG, QStringLiteral("must-not-be-stored")));
    settings.remove(GC_LANG);
    QVERIFY(!settings.contains(GC_LANG));
    QVERIFY(!settings.allKeys().contains(GC_LANG));
    QCOMPARE(settings.value(
                 nullptr, GC_NOLIO_ACCESS_TOKEN,
                 missing).toString(),
             missing);
    QVERIFY(!settings.setValueChecked(
        GC_NOLIO_ACCESS_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QCOMPARE(fileContents(outsidePath), before);
    QCOMPARE(factoryState()->values.size(), 1);
    QCOMPARE(factoryState()->values.value(vaultKey),
             secret);
#endif
}

void TestCredentialSettings::
localAthleteScopeIsPreservedWithoutCrossRootAdoption()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialLocalScopeMigration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyScope =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString sentinel =
        QStringLiteral("locally-bound-refresh-token");
    const QString missing = QStringLiteral("missing");
    const QString firstRoot =
        temporary.filePath(QStringLiteral("first-library"));
    const QString secondRoot =
        temporary.filePath(QStringLiteral("second-library"));
    QVERIFY(QDir().mkpath(
        firstRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        secondRoot + QStringLiteral("/Athlete/config")));

    {
        QSettings privateSettings(
            firstRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            legacyScope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(), QSettings::NoError);
    }
    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), firstRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            legacyScope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    factoryState()->values.insert(
        CredentialSettings::vaultKey(
            legacyScope, GC_STRAVA_REFRESH_TOKEN),
        sentinel);
    {
        GSettings first(organization, application);
        first.initializeQSettingsGlobal(firstRoot);
        QCOMPARE(first.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 sentinel);
        first.initializeQSettingsAthlete(
            firstRoot, athleteName);
        QCOMPARE(first.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 sentinel);
    }

    QString secondScope;
    {
        GSettings second(organization, application);
        second.initializeQSettingsGlobal(secondRoot);
        second.initializeQSettingsAthlete(
            secondRoot, athleteName);
        QCOMPARE(second.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
        QSettings privateSettings(
            secondRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        secondScope = privateSettings.value(
            QStringLiteral("credential_store/id")).toString();
        QVERIFY(!QUuid(secondScope).isNull());
        QVERIFY(secondScope != legacyScope);
    }

    QSettings preserved(
        firstRoot
            + QStringLiteral(
                "/Athlete/config/athlete-private.ini"),
        QSettings::IniFormat);
    QCOMPARE(preserved.value(
                 QStringLiteral("credential_store/id")).toString(),
             legacyScope);
}

void TestCredentialSettings::
ambiguousLegacyAthleteScopeFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialAmbiguousScope-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyScope =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    const QString legacySecret =
        QStringLiteral("ambiguous-legacy-refresh-token");
    const QString missing = QStringLiteral("missing");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));

    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            legacyScope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(), QSettings::NoError);
    }
    factoryState() = std::make_shared<FakeStoreState>();
    const QString legacyVaultKey = CredentialSettings::vaultKey(
        legacyScope, GC_STRAVA_REFRESH_TOKEN);
    factoryState()->values.insert(
        legacyVaultKey, legacySecret);

    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_REFRESH_TOKEN,
                 missing).toString(),
             missing);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_REFRESH_TOKEN,
                 missing).toString(),
             missing);

    QSettings privateSettings(
        athleteRoot
            + QStringLiteral(
                "/Athlete/config/athlete-private.ini"),
        QSettings::IniFormat);
    const QString freshScope = privateSettings.value(
        QStringLiteral("credential_store/id")).toString();
    QVERIFY(!QUuid(freshScope).isNull());
    QVERIFY(freshScope != legacyScope);
    QCOMPARE(factoryState()->values.value(legacyVaultKey),
             legacySecret);
}

void TestCredentialSettings::
ambiguousLegacyPlaintextFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialAmbiguousPlaintext-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_STRAVA_REFRESH_TOKEN);
    const QString legacySecret =
        QStringLiteral("ambiguous-plaintext-refresh-token");
    const QString missing = QStringLiteral("missing");
    QString legacyPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);
        legacyPath = legacy.fileName();
    }

    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    factoryState() = std::make_shared<FakeStoreState>();

    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     missing).toString(),
                 missing);
    }

    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             legacySecret);
    QVERIFY(fileContents(legacyPath).contains(
        legacySecret.toUtf8()));
    QVERIFY(factoryState()->values.isEmpty());
}

void TestCredentialSettings::
malformedRootIdentityFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialMalformedRoot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(athleteRoot));
    const QString globalPath = athleteRoot
        + QStringLiteral("/configglobal-general.ini");
    {
        QSettings global(globalPath, QSettings::IniFormat);
        global.setValue(
            QStringLiteral("credential_store/root_id"),
            QStringLiteral("../invalid"));
        global.sync();
        QCOMPARE(global.status(), QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    QVERIFY(!settings.setValueChecked(
        GC_NOLIO_ACCESS_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(factoryState()->values.isEmpty());

    QSettings global(globalPath, QSettings::IniFormat);
    QCOMPARE(global.value(
                 QStringLiteral(
                     "credential_store/root_id")).toString(),
             QStringLiteral("../invalid"));
}

void TestCredentialSettings::
malformedAthleteScopeFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialMalformedAthlete-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString privatePath = athleteRoot
        + QStringLiteral(
            "/Athlete/config/athlete-private.ini");
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    {
        QSettings privateSettings(
            privatePath, QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            QStringLiteral("../invalid"));
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    QVERIFY(!settings.setCValueChecked(
        athleteName, GC_STRAVA_REFRESH_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(factoryState()->values.isEmpty());

    QSettings privateSettings(
        privatePath, QSettings::IniFormat);
    QCOMPARE(privateSettings.value(
                 QStringLiteral(
                     "credential_store/id")).toString(),
             QStringLiteral("../invalid"));
}

void TestCredentialSettings::
escapedAthleteCredentialPathsFailClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialPathEscape-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString outsideAthlete =
        temporary.filePath(QStringLiteral("outside/Athlete"));
    QVERIFY(QDir().mkpath(athleteRoot));
    QVERIFY(QDir().mkpath(
        outsideAthlete + QStringLiteral("/config")));
    const QString outsidePrivate =
        outsideAthlete
        + QStringLiteral("/config/athlete-private.ini");

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot,
        QStringLiteral("../outside/Athlete"));
    QVERIFY(!settings.setCValueChecked(
        QStringLiteral("../outside/Athlete"),
        GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(!QFileInfo::exists(outsidePrivate));

#ifdef Q_OS_UNIX
    const QString linkPath =
        QDir(athleteRoot).filePath(
            QStringLiteral("Linked"));
    QVERIFY(QFile::link(outsideAthlete, linkPath));
    settings.initializeQSettingsAthlete(
        athleteRoot, QStringLiteral("Linked"));
    QVERIFY(!settings.setCValueChecked(
        QStringLiteral("Linked"),
        GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(!QFileInfo::exists(outsidePrivate));

    const QString configLinkedAthlete =
        QDir(athleteRoot).filePath(
            QStringLiteral("ConfigLinked"));
    QVERIFY(QDir().mkpath(configLinkedAthlete));
    QVERIFY(QFile::link(
        outsideAthlete + QStringLiteral("/config"),
        QDir(configLinkedAthlete).filePath(
            QStringLiteral("config"))));
    settings.initializeQSettingsAthlete(
        athleteRoot, QStringLiteral("ConfigLinked"));
    QVERIFY(!settings.setCValueChecked(
        QStringLiteral("ConfigLinked"),
        GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(!QFileInfo::exists(outsidePrivate));

    {
        QSettings outside(
            outsidePrivate, QSettings::IniFormat);
        outside.setValue(
            QStringLiteral("sentinel"),
            QStringLiteral("unchanged"));
        outside.sync();
        QCOMPARE(outside.status(), QSettings::NoError);
    }
    const QString privateLinkedConfig =
        QDir(athleteRoot).filePath(
            QStringLiteral("PrivateLinked/config"));
    QVERIFY(QDir().mkpath(privateLinkedConfig));
    QVERIFY(QFile::link(
        outsidePrivate,
        QDir(privateLinkedConfig).filePath(
            QStringLiteral("athlete-private.ini"))));
    settings.initializeQSettingsAthlete(
        athleteRoot, QStringLiteral("PrivateLinked"));
    QVERIFY(!settings.setCValueChecked(
        QStringLiteral("PrivateLinked"),
        GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QSettings outside(
        outsidePrivate, QSettings::IniFormat);
    QCOMPARE(outside.value(
                 QStringLiteral("sentinel")).toString(),
             QStringLiteral("unchanged"));
#endif
    QVERIFY(factoryState()->values.isEmpty());
    QCOMPARE(factoryState()->writes, 0);
}

void TestCredentialSettings::
danglingAthletePrivateSymlinkFailsClosed()
{
#ifndef Q_OS_UNIX
    QSKIP("This regression requires a dangling symlink");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialDanglingPrivate-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString configPath =
        QDir(athleteRoot).filePath(
            QStringLiteral("Athlete/config"));
    const QString privatePath =
        QDir(configPath).filePath(
            QStringLiteral("athlete-private.ini"));
    const QString outsidePath =
        temporary.filePath(
            QStringLiteral("outside/private.ini"));
    QVERIFY(QDir().mkpath(configPath));
    QVERIFY(QDir().mkpath(
        QFileInfo(outsidePath).absolutePath()));
    QVERIFY(QFile::link(outsidePath, privatePath));
    QVERIFY(QFileInfo(privatePath).isSymLink());
    QVERIFY(!QFileInfo::exists(outsidePath));

    factoryState() =
        std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);

    QVERIFY(!settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QVERIFY(!QFileInfo::exists(outsidePath));
    QVERIFY(QFileInfo(privatePath).isSymLink());
    QVERIFY(factoryState()->values.isEmpty());
    QCOMPARE(factoryState()->writes, 0);
#endif
}

void TestCredentialSettings::
invalidAthleteDoesNotDisableValidCredentials()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialInvalidAthlete-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application =
        QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString secret =
        QStringLiteral("valid-athlete-token");
    const QString replacement =
        QStringLiteral("valid-athlete-refresh-token");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(
        QDir(athleteRoot).filePath(
            QStringLiteral("Athlete/config"))));

    factoryState() =
        std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    QVERIFY(settings.setCValueChecked(
        athleteName, GC_STRAVA_TOKEN, secret));

    settings.initializeQSettingsAthlete(
        athleteRoot, QStringLiteral("../outside"));

    QCOMPARE(
        settings.cvalue(
            athleteName, GC_STRAVA_TOKEN,
            QStringLiteral("missing")).toString(),
        secret);
    QVERIFY(settings.setCValueChecked(
        athleteName, GC_STRAVA_REFRESH_TOKEN,
        replacement));
    QCOMPARE(factoryState()->values.size(), 2);
}

void TestCredentialSettings::
windowsAthleteJunctionFailsClosed()
{
#ifndef Q_OS_WIN
    QSKIP("This regression requires an NTFS junction");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization =
        QStringLiteral("CredentialJunction-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString originalName = QStringLiteral("Athlete");
    const QString copiedName = QStringLiteral("Junction");
    const QString secret =
        QStringLiteral("junction-protected-token");
    const QString missing = QStringLiteral("missing");
    const QString athleteRoot =
        temporary.filePath(QStringLiteral("library"));
    const QString originalPath =
        QDir(athleteRoot).filePath(originalName);
    const QString copiedPath =
        QDir(athleteRoot).filePath(copiedName);
    QVERIFY(QDir().mkpath(
        QDir(originalPath).filePath(
            QStringLiteral("config"))));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, originalName);
    QVERIFY(settings.setCValueChecked(
        originalName, GC_STRAVA_TOKEN, secret));
    settings.syncQSettings();
    const QHash<QString, QString> before =
        factoryState()->values;

    QProcess junction;
    junction.start(
        QStringLiteral("cmd.exe"),
        {
            QStringLiteral("/c"),
            QStringLiteral("mklink"),
            QStringLiteral("/J"),
            QDir::toNativeSeparators(copiedPath),
            QDir::toNativeSeparators(originalPath)
        });
    QVERIFY2(junction.waitForFinished(10000),
             qPrintable(junction.errorString()));
    const QByteArray output =
        junction.readAllStandardOutput()
        + junction.readAllStandardError();
    QVERIFY2(junction.exitStatus()
                 == QProcess::NormalExit
             && junction.exitCode() == 0,
             output.constData());
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(
            QDir::toNativeSeparators(copiedPath)
                .utf16()));
    QVERIFY(attributes != INVALID_FILE_ATTRIBUTES);
    QVERIFY(attributes & FILE_ATTRIBUTE_REPARSE_POINT);

    settings.initializeQSettingsAthlete(
        athleteRoot, copiedName);
    QCOMPARE(settings.cvalue(
                 copiedName, GC_STRAVA_TOKEN,
                 missing).toString(),
             missing);
    QVERIFY(!settings.setCValueChecked(
        copiedName, GC_STRAVA_TOKEN,
        QStringLiteral("must-not-be-stored")));
    QCOMPARE(factoryState()->values, before);
    QCOMPARE(settings.cvalue(
                 originalName, GC_STRAVA_TOKEN,
                 missing).toString(),
             secret);
#endif
}

void TestCredentialSettings::
preInitializationUsesLocalAthleteScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialEarly-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString legacyKey = athleteName + QLatin1Char('/')
        + plainKey(GC_STRAVA_REFRESH_TOKEN);
    const QString legacySecret =
        QStringLiteral("ambiguous-early-plaintext");
    const QString vaultSecret =
        QStringLiteral("local-scope-vault-token");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    const QString localScope =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    {
        QSettings legacy(organization, application);
        legacy.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        legacy.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            localScope);
        legacy.setValue(legacyKey, legacySecret);
        legacy.sync();
    }

    {
        QSettings privateSettings(
            athleteRoot
                + QStringLiteral(
                    "/Athlete/config/athlete-private.ini"),
            QSettings::IniFormat);
        privateSettings.setValue(
            QStringLiteral("credential_store/id"),
            localScope);
        privateSettings.sync();
        QCOMPARE(privateSettings.status(),
                 QSettings::NoError);
    }
    {
        QSettings systemSettings(
            QSettings::IniFormat,
            QSettings::UserScope,
            organization,
            application);
        systemSettings.setValue(
            plainKey(GC_HOMEDIR), athleteRoot);
        systemSettings.setValue(
            legacyCredentialScopeMappingKey(athleteName),
            localScope);
        systemSettings.sync();
        QCOMPARE(systemSettings.status(), QSettings::NoError);
    }
    factoryState() = std::make_shared<FakeStoreState>();
    const QString vaultKey = CredentialSettings::vaultKey(
        localScope, GC_STRAVA_REFRESH_TOKEN);
    factoryState()->values.insert(
        vaultKey, vaultSecret);
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 vaultSecret);
    }

    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 vaultSecret);
    }
    QCOMPARE(factoryState()->values.size(), 1);
    QCOMPARE(factoryState()->values.value(vaultKey),
             vaultSecret);
    QSettings retained(organization, application);
    QCOMPARE(retained.value(legacyKey).toString(),
             legacySecret);
}

void TestCredentialSettings::clearedRootDoesNotRetainAthleteScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialLate-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString sentinel = QStringLiteral("late-fallback-sentinel");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    GSettings settings(organization, application);
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    settings.setCValue(
        athleteName, GC_STRAVA_TOKEN, sentinel);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 QStringLiteral("missing")).toString(),
             sentinel);

    settings.clearGlobalAndAthletes();
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 QStringLiteral("missing")).toString(),
             QStringLiteral("missing"));
    settings.initializeQSettingsGlobal(athleteRoot);
    settings.initializeQSettingsAthlete(
        athleteRoot, athleteName);
    QCOMPARE(settings.cvalue(
                 athleteName, GC_STRAVA_TOKEN,
                 QStringLiteral("missing")).toString(),
             sentinel);
    QCOMPARE(factoryState()->values.size(), 1);
}

void TestCredentialSettings::
credentialBackendWaitDoesNotHoldSettingsMutex()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialReentrant-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString otherAthleteName = QStringLiteral("OtherAthlete");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    const QString otherAthleteRoot = temporary.filePath(
        QStringLiteral("other-athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        otherAthleteRoot
            + QStringLiteral("/OtherAthlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    const auto settings = std::make_shared<GSettings>(
        organization, application);
    const auto otherSettings = std::make_shared<GSettings>(
        organization + QStringLiteral("-Other"), application);
    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);
    otherSettings->initializeQSettingsGlobal(otherAthleteRoot);
    otherSettings->initializeQSettingsAthlete(
        otherAthleteRoot, otherAthleteName);
    QVERIFY(otherSettings->setCValueChecked(
        otherAthleteName, GC_STRAVA_TOKEN,
        QStringLiteral("other-secret")));
    const QString probeKey = QStringLiteral("<system>reentrant_probe");
    QVERIFY(settings->setValueChecked(probeKey, 42));

    struct ReentrantState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> hookEntered{false};
        std::atomic<bool> reentrantCompleted{false};
        std::atomic<bool> backendWaitTimedOut{false};
        std::atomic<bool> operationCompleted{false};
        std::atomic<bool> credentialFailedClosed{false};
        std::atomic<bool> independentCredentialSucceeded{false};
    };
    const auto state = std::make_shared<ReentrantState>();
    factoryState()->beforeRead = [
            settings, otherSettings, state,
            athleteName, otherAthleteName, probeKey] {
        if (state->hookEntered.exchange(true)) return;
        const QString operationId = CredentialSettingsDetail::
            currentCredentialOperationId();
        const bool invoked = QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [settings, otherSettings, state,
             athleteName, otherAthleteName,
             probeKey, operationId] {
                CredentialSettingsDetail::
                    CredentialOperationContextScope operationContext(
                        operationId);
                QCOMPARE(
                    settings->value(nullptr, probeKey).toInt(), 42);
                state->credentialFailedClosed.store(
                    settings->cvalue(
                        athleteName,
                        GC_STRAVA_TOKEN,
                        QStringLiteral("backend-pending")).toString()
                    == QStringLiteral("backend-pending"));
                state->independentCredentialSucceeded.store(
                    otherSettings->cvalue(
                        otherAthleteName,
                        GC_STRAVA_TOKEN,
                        QStringLiteral("missing")).toString()
                    == QStringLiteral("other-secret"));
                state->reentrantCompleted.store(true);
                state->condition.notify_all();
            },
            Qt::QueuedConnection);
        if (!invoked) return;

        std::unique_lock<std::mutex> lock(state->mutex);
        const bool completed = state->condition.wait_for(
            lock, std::chrono::seconds(5),
            [state] { return state->reentrantCompleted.load(); });
        state->backendWaitTimedOut.store(!completed);
    };

    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [settings, state, athleteName] {
                settings->cvalue(
                    athleteName,
                    GC_STRAVA_TOKEN,
                    QStringLiteral("missing"));
                state->operationCompleted.store(true);
                state->condition.notify_all();
            },
            10000,
            {});

    QVERIFY(dispatched.status
            != StravaSettingsCommit::DispatchStatus::NotStarted);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->operationCompleted.load(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        state->reentrantCompleted.load(), 30000);
    QVERIFY(state->hookEntered.load());
    QVERIFY2(!state->backendWaitTimedOut.load(),
             "GUI settings access waited for the credential backend timeout");
    QVERIFY(state->credentialFailedClosed.load());
    QVERIFY(state->independentCredentialSucceeded.load());
#if !defined(__SANITIZE_THREAD__)
    QCOMPARE(dispatched.status,
             StravaSettingsCommit::DispatchStatus::Completed);
#endif
    factoryState()->beforeRead = {};
}

void TestCredentialSettings::
applicationThreadCredentialBackendWaitProcessesKeychainCompletion()
{
    struct WaitState
    {
        std::atomic<bool> jobStarted{false};
        std::atomic<bool> workerFinished{false};
        std::atomic<int> readStatus{
            int(CredentialStore::Status::Failed)};
        QPointer<QKeychain::Job> job;
    };
    const auto state = std::make_shared<WaitState>();
    const auto settingsMutex =
        std::make_shared<SettingsAccessMutex>();

    constexpr int KeychainTimeoutMs = 250;
    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(
        KeychainTimeoutMs);
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [state](QKeychain::Job *job) {
            state->job = job;
            state->jobStarted.store(
                true, std::memory_order_release);
            return true;
        });

    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [state, settingsMutex] {
            settingsMutex->lock();
            const unsigned int depth =
                settingsMutex->suspendForCredentialBackend();
            std::unique_ptr<CredentialStore> store =
                createQtKeychainCredentialStore();
            const CredentialStore::ReadResult read = store->read(
                QStringLiteral(
                    "application-thread-settings-wait"));
            settingsMutex->resumeAfterCredentialBackend(depth);
            settingsMutex->unlock();
            state->readStatus.store(
                int(read.status), std::memory_order_release);
            state->workerFinished.store(
                true, std::memory_order_release);
        }, {}));
    QTRY_VERIFY_WITH_TIMEOUT(
        state->jobStarted.load(std::memory_order_acquire), 1000);

    QTimer::singleShot(
        0,
        QCoreApplication::instance(),
        [state] {
            if (!state->job) return;
            state->job->emitFinishedWithError(
                QKeychain::EntryNotFound,
                QStringLiteral("synthetic completion"));
        });

    settingsMutex->lock();
    QElapsedTimer elapsed;
    elapsed.start();
    const bool completed =
        settingsMutex->waitForCredentialBackends(1000);
    const qint64 waitElapsed = elapsed.elapsed();
    settingsMutex->unlock();

    QTRY_VERIFY_WITH_TIMEOUT(
        state->workerFinished.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(state->job.isNull(), 1000);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();

    QVERIFY(completed);
    QVERIFY2(
        waitElapsed < KeychainTimeoutMs,
        "Application-thread settings wait blocked keychain completion");
    QCOMPARE(
        state->readStatus.load(std::memory_order_acquire),
        int(CredentialStore::Status::NotFound));
}

void TestCredentialSettings::
applicationThreadKeychainReentrancyDefersSettingsReconfiguration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialReentrant-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString reentrantAthleteName =
        QStringLiteral("ReentrantAthlete");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        athleteRoot
        + QStringLiteral("/ReentrantAthlete/config")));

    struct ReentrantState
    {
        bool exerciseArmed = false;
        bool exerciseStarted = false;
        bool reconfigurationReturned = false;
    };
    const auto state = std::make_shared<ReentrantState>();
    const auto finishJob = [](QKeychain::Job *job) {
        if (!job) return;
        if (qobject_cast<QKeychain::ReadPasswordJob *>(job)) {
            job->emitFinishedWithError(
                QKeychain::EntryNotFound,
                QStringLiteral("synthetic missing credential"));
        } else {
            job->emitFinished();
        }
    };

    useQtKeychainFactory() = true;
    CredentialSettingsDetail::setCredentialCacheNowForTest(1000);
    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(1000);
    GSettings::setCredentialBackendWaitTimeoutForTest(50);

    std::shared_ptr<GSettings> settings;
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [state, &settings, athleteRoot,
         reentrantAthleteName, finishJob](QKeychain::Job *job) {
            const QPointer<QKeychain::Job> guardedJob(job);
            if (!state->exerciseArmed
                || state->exerciseStarted) {
                QTimer::singleShot(
                    0,
                    QCoreApplication::instance(),
                    [guardedJob, finishJob] {
                        finishJob(guardedJob.data());
                    });
                return true;
            }

            state->exerciseStarted = true;
            QTimer::singleShot(
                0,
                QCoreApplication::instance(),
                [state, &settings, athleteRoot,
                 reentrantAthleteName] {
                    settings->initializeQSettingsAthlete(
                        athleteRoot, reentrantAthleteName);
                    state->reconfigurationReturned = true;
                });
            QTimer::singleShot(
                10,
                QCoreApplication::instance(),
                [guardedJob, finishJob] {
                    finishJob(guardedJob.data());
                });
            return true;
        });

    settings = std::make_shared<GSettings>(organization, application);
    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);
    QVERIFY(!settings->athleteConfigDirectory(athleteName).isEmpty());

    CredentialSettingsDetail::setCredentialCacheNowForTest(
        1000
        + CredentialSettingsDetail::
              credentialCacheLifetimeMsForTest()
        + 1);
    state->exerciseArmed = true;
    const GSettings::CredentialReadResult read =
        settings->credentialCValueChecked(
            athleteName, GC_STRAVA_TOKEN);

    QVERIFY(read.readable());
    QVERIFY(state->exerciseStarted);
    QVERIFY(state->reconfigurationReturned);
    QVERIFY2(
        settings->athleteConfigDirectory(
            reentrantAthleteName).isEmpty(),
        "Reentrant settings initialization accessed QSettings before the keychain suspension cleared");

    settings->initializeQSettingsAthlete(
        athleteRoot, reentrantAthleteName);
    QVERIFY(!settings->athleteConfigDirectory(
        reentrantAthleteName).isEmpty());

    QVERIFY(settings->clearGlobalAndAthletes(1000));
    settings.reset();
    useQtKeychainFactory() = false;
    GSettings::setCredentialBackendWaitTimeoutForTest(-1);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();
}

void TestCredentialSettings::
credentialBackendBlocksSettingsReconfigurationUntilRelease()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialLifecycle-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    const auto settings = std::make_shared<GSettings>(
        organization, application);
    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);

    struct BlockingState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> entered{false};
        std::atomic<bool> released{false};
        std::atomic<bool> readFinished{false};
        std::atomic<bool> reconfigured{false};
    };
    const auto state = std::make_shared<BlockingState>();
    factoryState()->beforeRead = [state] {
        if (state->entered.exchange(true)) return;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->condition.wait(lock, [state] {
            return state->released.load();
        });
    };

    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [settings, state, athleteName] {
            settings->cvalue(
                athleteName, GC_STRAVA_TOKEN,
                QStringLiteral("missing"));
            state->readFinished.store(true);
        }, {}));
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 5000);

    QElapsedTimer boundedClearElapsed;
    boundedClearElapsed.start();
    QVERIFY(!settings->clearGlobalAndAthletes(25));
    QVERIFY2(boundedClearElapsed.elapsed() < 500,
             "Bounded settings shutdown exceeded its deadline");

    std::future<void> reconfiguration = std::async(
        std::launch::async,
        [settings, state] {
            settings->clearGlobalAndAthletes();
            state->reconfigured.store(true);
        });
    QTest::qWait(50);
    QVERIFY2(!state->reconfigured.load(),
             "Settings were destroyed while a credential backend retained them");

    state->released.store(true);
    state->condition.notify_all();
    QVERIFY(reconfiguration.wait_for(std::chrono::seconds(15))
            == std::future_status::ready);
    reconfiguration.get();
    QTRY_VERIFY_WITH_TIMEOUT(state->readFinished.load(), 1000);

    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);
    QVERIFY(settings->setCValueChecked(
        athleteName, GC_STRAVA_TOKEN,
        QStringLiteral("replacement-secret")));
    factoryState()->beforeRead = {};
}

void TestCredentialSettings::
credentialBackendDoesNotDropSameInstanceAthleteInitialization()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());

    const QString organization = QStringLiteral("CredentialInitWait-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString newAthleteName = QStringLiteral("NewAthlete");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/NewAthlete/config")));

    factoryState() = std::make_shared<FakeStoreState>();
    const auto settings = std::make_shared<GSettings>(
        organization, application);
    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);

    struct BlockingState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> entered{false};
        std::atomic<bool> released{false};
        std::atomic<int> initializationCompletions{0};
    };
    const auto state = std::make_shared<BlockingState>();
    factoryState()->beforeRead = [state] {
        if (state->entered.exchange(true)) return;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->condition.wait(lock, [state] {
            return state->released.load();
        });
    };

    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [settings, athleteName] {
            settings->cvalue(
                athleteName, GC_STRAVA_TOKEN,
                QStringLiteral("missing"));
        }, {}));
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 5000);

    std::future<void> initialization = std::async(
        std::launch::async,
        [settings, state, athleteRoot, newAthleteName] {
            settings->initializeQSettingsAthlete(
                athleteRoot, newAthleteName);
            state->initializationCompletions.fetch_add(1);
        });
    QTest::qWait(50);
    QCOMPARE(state->initializationCompletions.load(), 0);

    state->released.store(true);
    state->condition.notify_all();
    QVERIFY(initialization.wait_for(std::chrono::seconds(15))
            == std::future_status::ready);
    initialization.get();
    QCOMPARE(state->initializationCompletions.load(), 1);
    QVERIFY(settings->setCValueChecked(
        newAthleteName, GC_STRAVA_TOKEN,
        QStringLiteral("new-athlete-secret")));
    factoryState()->beforeRead = {};
}

void TestCredentialSettings::
credentialWorkerKeepsNativeKeychainOnApplicationThread()
{
    std::atomic<bool> hookCalled{false};
    std::atomic<bool> hookOnApplicationThread{false};
    std::atomic<bool> coordinationOnApplicationThread{true};
    std::atomic<int> readStatus{
        int(CredentialStore::Status::Failed)};
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [&](QKeychain::Job *job) {
            hookCalled.store(true);
            hookOnApplicationThread.store(
                QThread::currentThread()
                == QCoreApplication::instance()->thread());
            job->emitFinishedWithError(
                QKeychain::EntryNotFound,
                QStringLiteral("synthetic missing credential"));
            return true;
        });

    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [&] {
                coordinationOnApplicationThread.store(
                    QThread::currentThread()
                    == QCoreApplication::instance()->thread());
                std::unique_ptr<CredentialStore> store =
                    createQtKeychainCredentialStore();
                const CredentialStore::ReadResult read = store->read(
                    QStringLiteral("synthetic-strava-affinity-probe"));
                readStatus.store(int(read.status));
            },
            1000,
            {});
    QCOMPARE(dispatched.status,
             StravaSettingsCommit::DispatchStatus::Completed);
    QVERIFY(hookCalled.load());
    QVERIFY(hookOnApplicationThread.load());
    QVERIFY(!coordinationOnApplicationThread.load());
    QCOMPARE(readStatus.load(),
             int(CredentialStore::Status::NotFound));

    CredentialStoreQtKeychainDetail::resetJobTestHooks();
}

void TestCredentialSettings::
credentialWorkerKeychainTimeoutRespectsCallerDeadline()
{
    std::atomic<bool> jobStarted{false};
    QPointer<QKeychain::Job> delayedJob;
    CredentialStoreQtKeychainDetail::setJobTimeoutForTest(500);
    CredentialStoreQtKeychainDetail::setJobStartHookForTest(
        [&](QKeychain::Job *job) {
            delayedJob = job;
            jobStarted.store(true);
            return true;
        });

    QElapsedTimer elapsed;
    elapsed.start();
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [] {
                std::unique_ptr<CredentialStore> store =
                    createQtKeychainCredentialStore();
                store->read(QStringLiteral("deadline-probe"));
            },
            25,
            {});
    const qint64 callerElapsed = elapsed.elapsed();

    QCOMPARE(dispatched.status,
             StravaSettingsCommit::DispatchStatus::Pending);
    QVERIFY(jobStarted.load());
    QVERIFY2(callerElapsed < 200,
             "GUI keychain dispatch exceeded the caller deadline");
    QVERIFY(CredentialSettingsDetail::
        credentialOperationContextActive());

    if (delayedJob) {
        delayedJob->emitFinishedWithError(
            QKeychain::EntryNotFound,
            QStringLiteral("late deadline completion"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(delayedJob.isNull(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !CredentialSettingsDetail::
            credentialOperationContextActive(),
        1000);
    CredentialStoreQtKeychainDetail::resetJobTestHooks();
}

void TestCredentialSettings::
credentialAsyncCompletionHonorsContextLifetime()
{
    std::promise<void> releaseOperation;
    const std::shared_future<void> released =
        releaseOperation.get_future().share();
    std::atomic<bool> operationStarted{false};
    std::atomic<bool> completionCalled{false};
    QObject *context = new QObject(this);

    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [&] {
            operationStarted.store(true);
            released.wait();
        },
        context,
        [&] { completionCalled.store(true); }));
    QTRY_VERIFY_WITH_TIMEOUT(operationStarted.load(), 1000);
    delete context;
    releaseOperation.set_value();
    QTest::qWait(100);
    QVERIFY(!completionCalled.load());
}

void TestCredentialSettings::credentialWorkerRestartsAfterCleanShutdown()
{
    QVERIFY(StravaSettingsCommit::shutdownCredentialThread());
    QVERIFY(StravaSettingsCommit::credentialThreadStopped());
    QVERIFY(StravaSettingsCommit::restartCredentialThread());

    std::atomic<bool> ran{false};
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [&ran] { ran.store(true); }, 1000, {});
    QCOMPARE(dispatched.status,
             StravaSettingsCommit::DispatchStatus::Completed);
    QVERIFY(ran.load());
}

void TestCredentialSettings::
stoppedCredentialWorkerGenerationsAreDestroyed()
{
    QVERIFY(StravaSettingsCommit::shutdownCredentialThread());
    QVERIFY(StravaSettingsCommit::restartCredentialThread());
    QCOMPARE(
        StravaSettingsCommit::credentialWorkerLiveInstancesForTest(),
        0);

    for (int generation = 0; generation < 3; ++generation) {
        const StravaSettingsCommit::DispatchResult dispatched =
            StravaSettingsCommit::runOnCredentialThread(
                [] {}, 1000, {});
        QCOMPARE(dispatched.status,
                 StravaSettingsCommit::DispatchStatus::Completed);
        QCOMPARE(
            StravaSettingsCommit::credentialWorkerLiveInstancesForTest(),
            1);
        QVERIFY(StravaSettingsCommit::shutdownCredentialThread());
        QVERIFY(StravaSettingsCommit::restartCredentialThread());
        QCOMPARE(
            StravaSettingsCommit::credentialWorkerLiveInstancesForTest(),
            0);
    }
}

void TestCredentialSettings::
credentialWorkerShutdownAbandonsQueuedOperations()
{
    if (!qEnvironmentVariableIsSet(
            "GC_CREDENTIAL_WORKER_SHUTDOWN_CHILD")) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral(
                "GC_CREDENTIAL_WORKER_SHUTDOWN_CHILD"),
            QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral(
                "credentialWorkerShutdownAbandonsQueuedOperations"),
            QStringLiteral("-silent")
        });
        child.start();
        QVERIFY2(child.waitForStarted(5000),
                 qPrintable(child.errorString()));
        const bool finished = child.waitForFinished(30000);
        if (!finished) {
            child.kill();
            child.waitForFinished();
        }
        const QByteArray output = child.readAll();
        QVERIFY2(finished, output.constData());
        QVERIFY2(child.exitStatus() == QProcess::NormalExit,
                 output.constData());
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QSettings::setPath(QSettings::NativeFormat,
                       QSettings::UserScope, temporary.path());
    QSettings::setPath(QSettings::IniFormat,
                       QSettings::UserScope, temporary.path());
    const QString organization = QStringLiteral("CredentialShutdown-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString application = QStringLiteral("GoldenCheetahTest");
    const QString athleteName = QStringLiteral("Athlete");
    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    factoryState() = std::make_shared<FakeStoreState>();
    GSettings *settings = new GSettings(organization, application);
    settings->initializeQSettingsGlobal(athleteRoot);
    settings->initializeQSettingsAthlete(athleteRoot, athleteName);

    std::atomic<bool> firstStarted{false};
    std::atomic<bool> firstFinished{false};
    std::atomic<bool> backendEntered{false};
    std::atomic<bool> destructionFinished{false};
    std::atomic<bool> completionCalled{false};
    std::atomic<bool> queuedRan{false};
    const auto release = std::make_shared<std::promise<void>>();
    const std::shared_future<void> released =
        release->get_future().share();
    QObject *completionContext = new QObject(this);
    factoryState()->beforeRead = [&, released] {
        if (backendEntered.exchange(true)) return;
        released.wait();
    };

    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [&, settings, athleteName] {
            firstStarted.store(true);
            settings->cvalue(
                athleteName, GC_STRAVA_TOKEN,
                QStringLiteral("missing"));
            firstFinished.store(true);
        },
        completionContext,
        [&] { completionCalled.store(true); }));
    QTRY_VERIFY_WITH_TIMEOUT(firstStarted.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(backendEntered.load(), 5000);
    QVERIFY(StravaSettingsCommit::runOnCredentialThreadAsync(
        [&] { queuedRan.store(true); },
        {}));

    QElapsedTimer shutdownElapsed;
    shutdownElapsed.start();
    const bool stopped =
        StravaSettingsCommit::shutdownCredentialThread();
    QVERIFY(!stopped);
    QVERIFY2(shutdownElapsed.elapsed() < 1000,
             "Credential shutdown waited for noncooperative work");
    delete completionContext;

    std::future<void> destruction = std::async(
        std::launch::async,
        [settings, &destructionFinished] {
            delete settings;
            destructionFinished.store(true);
        });
    QTest::qWait(50);
    QVERIFY2(!destructionFinished.load(),
             "GSettings was destroyed while credential work retained it");

    release->set_value();
    QTRY_VERIFY_WITH_TIMEOUT(firstFinished.load(), 1000);
    QVERIFY(destruction.wait_for(std::chrono::seconds(5))
            == std::future_status::ready);
    destruction.get();
    QVERIFY(destructionFinished.load());
    QTRY_VERIFY_WITH_TIMEOUT(
        StravaSettingsCommit::credentialThreadStopped(), 1000);
    QTest::qWait(50);
    QVERIFY(!completionCalled.load());
    QVERIFY(!queuedRan.load());
}

QTEST_MAIN(TestCredentialSettings)
#include "testCredentialSettings.moc"
