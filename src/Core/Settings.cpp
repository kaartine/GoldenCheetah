/*
 * Copyright (c) 2006 Sean C. Rhea (srhea@srhea.net)
 * Copyright (c) 2015 Joern Rischmueller (joern.rm@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <QDir>
#include "Settings.h"
#include "CredentialSettings.h"
#include "MainWindow.h"
#include "Colors.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QtAlgorithms>

#include <QFontDatabase>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QUuid>

#include <memory>
#include <chrono>
#include <unordered_map>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

thread_local std::unordered_map<const SettingsAccessMutex *, unsigned int>
    settingsAccessDepth;
thread_local std::unordered_map<const SettingsAccessMutex *, unsigned int>
    credentialBackendSuspensionOwners;

#ifdef GC_CREDENTIAL_TEST_HOOKS
std::atomic<int> credentialBackendWaitTimeoutForTest{-1};
#endif

int credentialBackendWaitTimeout()
{
#ifdef GC_CREDENTIAL_TEST_HOOKS
    return credentialBackendWaitTimeoutForTest.load(
        std::memory_order_acquire);
#else
    return -1;
#endif
}

class CredentialBackendSettingsUnlock final
{
public:
    explicit CredentialBackendSettingsUnlock(SettingsAccessMutex &mutex)
        : mutex_(mutex),
          credentialDepth_(CredentialSettingsDetail::
              suspendCredentialOperationMutexForBackend()),
          settingsDepth_(mutex_.suspendForCredentialBackend())
    {
    }

    ~CredentialBackendSettingsUnlock()
    {
        mutex_.resumeAfterCredentialBackend(settingsDepth_);
        CredentialSettingsDetail::
            resumeCredentialOperationMutexAfterBackend(
                credentialDepth_);
    }

private:
    SettingsAccessMutex &mutex_;
    unsigned int credentialDepth_;
    unsigned int settingsDepth_;
};

class SettingsUnlockingCredentialStore final : public CredentialStore
{
public:
    SettingsUnlockingCredentialStore(
        std::unique_ptr<CredentialStore> store,
        SettingsAccessMutex &mutex)
        : store_(std::move(store)), mutex_(mutex)
    {
    }

    ReadResult read(const QString &key) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->read(key);
    }

    CreateResult createIfAbsent(
        const QString &key,
        const QString &value) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->createIfAbsent(key, value);
    }

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->write(key, value, error);
    }

    Status writeCoordinated(
        const QString &key,
        const QString &value,
        QString *error,
        const QString &mutationLockPath) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->writeCoordinated(
            key, value, error, mutationLockPath);
    }

    Status remove(const QString &key,
                  QString *error) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->remove(key, error);
    }

    Status removeCoordinated(
        const QString &key,
        QString *error,
        const QString &mutationLockPath) override
    {
        CredentialBackendSettingsUnlock unlock(mutex_);
        return store_->removeCoordinated(
            key, error, mutationLockPath);
    }

private:
    std::unique_ptr<CredentialStore> store_;
    SettingsAccessMutex &mutex_;
};

std::unique_ptr<CredentialStore> settingsCredentialStore(
    SettingsAccessMutex &mutex)
{
    return std::make_unique<SettingsUnlockingCredentialStore>(
        createPlatformCredentialStore(), mutex);
}

} // namespace

void SettingsAccessMutex::lock()
{
    mutex.lock();
    ++settingsAccessDepth[this];
}

void SettingsAccessMutex::unlock()
{
    auto found = settingsAccessDepth.find(this);
    Q_ASSERT(found != settingsAccessDepth.end() && found->second > 0);
    if (--found->second == 0)
        settingsAccessDepth.erase(found);
    mutex.unlock();
}

unsigned int SettingsAccessMutex::suspendForCredentialBackend()
{
    const auto found = settingsAccessDepth.find(this);
    if (found == settingsAccessDepth.end()) return 0;

    const unsigned int depth = found->second;
    credentialBackendSuspensions.fetch_add(
        1, std::memory_order_release);
    ++credentialBackendSuspensionOwners[this];
    settingsAccessDepth.erase(found);
    for (unsigned int index = 0; index < depth; ++index)
        mutex.unlock();
    return depth;
}

void SettingsAccessMutex::resumeAfterCredentialBackend(
    unsigned int depth)
{
    for (unsigned int index = 0; index < depth; ++index)
        mutex.lock();
    if (depth > 0) {
        settingsAccessDepth[this] = depth;
        const auto owner =
            credentialBackendSuspensionOwners.find(this);
        Q_ASSERT(owner != credentialBackendSuspensionOwners.end()
                 && owner->second > 0);
        if (--owner->second == 0)
            credentialBackendSuspensionOwners.erase(owner);
        const unsigned int previous =
            credentialBackendSuspensions.fetch_sub(
                1, std::memory_order_acq_rel);
        Q_ASSERT(previous > 0);
        if (previous == 1)
            credentialBackendCondition.notify_all();
    }
}

bool SettingsAccessMutex::credentialBackendSuspended() const
{
    return credentialBackendSuspensions.load(
               std::memory_order_acquire) > 0;
}

bool SettingsAccessMutex::waitForCredentialBackends(
    int timeoutMilliseconds)
{
    if (!credentialBackendSuspended()) return true;

    const auto owner = credentialBackendSuspensionOwners.find(this);
    if (owner != credentialBackendSuspensionOwners.end()
        && owner->second > 0) {
        return false;
    }

    const auto found = settingsAccessDepth.find(this);
    Q_ASSERT(found != settingsAccessDepth.end()
             && found->second == 1);
    settingsAccessDepth.erase(found);
    std::unique_lock<std::recursive_mutex> lock(
        mutex, std::adopt_lock);
    const auto complete = [this] {
        return !credentialBackendSuspended();
    };
    bool completed = true;
    QCoreApplication *application =
        QCoreApplication::instance();
    const bool applicationThread = application
        && QThread::currentThread() == application->thread();
    if (applicationThread) {
        // Native keychain dispatch and completion are application-thread
        // events, so reconfiguration must keep that event path alive.
        constexpr int EventWaitSliceMilliseconds = 10;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(
                timeoutMilliseconds < 0
                    ? 0 : timeoutMilliseconds);
        while (!complete()) {
            int waitMilliseconds = EventWaitSliceMilliseconds;
            if (timeoutMilliseconds >= 0) {
                const auto remaining =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            deadline
                            - std::chrono::steady_clock::now())
                        .count();
                if (remaining <= 0) break;
                if (remaining < waitMilliseconds)
                    waitMilliseconds = int(remaining);
            }

            lock.unlock();
            QEventLoop eventLoop;
            QTimer wakeup;
            wakeup.setSingleShot(true);
            QObject::connect(
                &wakeup, &QTimer::timeout,
                &eventLoop, &QEventLoop::quit);
            wakeup.start(waitMilliseconds);
            eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
            lock.lock();
        }
        completed = complete();
    } else if (timeoutMilliseconds < 0) {
        credentialBackendCondition.wait(lock, complete);
    } else {
        completed = credentialBackendCondition.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMilliseconds),
            complete);
    }
    lock.release();
    settingsAccessDepth[this] = 1;
    return completed;
}

#ifdef Q_OS_MAC
int OperatingSystem = OSX;
#elif defined Q_OS_WIN32
int OperatingSystem = WINDOWS;
#elif defined Q_OS_LINUX
int OperatingSystem = LINUX;
#elif defined Q_OS_OPENBSD
int OperatingSystem = OPENBSD;
#endif

double scalefactors[13] = { 0.5f, 0.6f, 0.8, 0.9, 1.0f, 1.1f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 5.0f, 0 };

// -------------- Initializer for the "extern" variable "appsettings" ----------------//

#ifndef GC_SETTINGS_NO_GLOBAL_INSTANCE
static GSettings *GetApplicationSettings()
{
  GSettings *settings;
  QDir home = QDir();
    //First check to see if the Library folder exists where the executable is (for USB sticks)
  if(!home.exists("Library/GoldenCheetah"))
    settings = new GSettings(GC_SETTINGS_CO, GC_SETTINGS_APP);
  else
    settings = new GSettings(home.absolutePath()+"/gc", QSettings::IniFormat);
  return settings;
}
#endif

// local static helper routines

// define the sections and the filenames
enum SettingsType {SETTINGS_SYSTEM = 0,
                   SETTINGS_GLOBAL = 1,
                   SETTINGS_ATHLETE = 2 };

enum SettingsFilesIndexGlobal {GLOBAL_GENERAL = 0,
                               GLOBAL_TRAINMODE = 1};

enum SettingsFilesIndexAthlete { ATHLETE_GENERAL = 0,
                                 ATHLETE_LAYOUT = 1,
                                 ATHLETE_PREFERENCES = 2,
                                 ATHLETE_PRIVATE = 3};

static const QString settingFileNamesGlobal[2] = {"configglobal-general.ini","configglobal-trainmode.ini"};
static const QString settingFileNamesAthlete[4] = {"athlete-general.ini","athlete-layout.ini","athlete-preferences.ini","athlete-private.ini"};
static const QString credentialScopeStorageKey =
    QStringLiteral("credential_store/id");
static const QString credentialRootIdentityStorageKey =
    QStringLiteral("credential_store/root_id");
static const QString credentialScopeBindingStorageKey =
    QStringLiteral("credential_store/binding_v2");
static const QString legacySystemMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/system_state");
static const QString legacyGlobalMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/global_state");
static const QString legacyAthleteMigrationMarkerKey =
    QStringLiteral("migration/legacy_qsettings_v1/athlete_state");
static const QString legacyMigrationStarted =
    QStringLiteral("started");
static const QString legacyMigrationComplete =
    QStringLiteral("complete");

class ScopedQSettingsFallbackDisabler
{
public:
    explicit ScopedQSettingsFallbackDisabler(QSettings *settings)
        : settings(settings),
          fallbacksEnabled(
              settings && settings->fallbacksEnabled())
    {
        if (settings)
            settings->setFallbacksEnabled(false);
    }

    ~ScopedQSettingsFallbackDisabler()
    {
        if (settings)
            settings->setFallbacksEnabled(fallbacksEnabled);
    }

private:
    QSettings *settings;
    bool fallbacksEnabled;
    Q_DISABLE_COPY(ScopedQSettingsFallbackDisabler)
};

static bool isMigrationMetadataKey(const QString &key)
{
    return key.startsWith(QStringLiteral("credential_store/"))
        || key.startsWith(QStringLiteral("migration/"));
}

static bool hasApplicationSettings(
    const QList<QSettings *> &settingsFiles,
    const QStringList &ignoredKeys = {},
    bool preserveMacSingleKeyBehavior = false)
{
    int applicationKeys = 0;
    for (QSettings *settings : settingsFiles) {
        if (!settings) continue;
        for (const QString &key : settings->allKeys()) {
            if (!isMigrationMetadataKey(key)
                && !ignoredKeys.contains(key)) {
                ++applicationKeys;
            }
        }
    }
#ifdef Q_OS_MAC
    if (preserveMacSingleKeyBehavior && applicationKeys <= 1)
        return false;
#else
    Q_UNUSED(preserveMacSingleKeyBehavior)
#endif
    return applicationKeys > 0;
}

static bool syncMigrationTargets(
    const QList<QSettings *> &settingsFiles)
{
    bool success = true;
    for (QSettings *settings : settingsFiles) {
        if (!settings) {
            success = false;
            continue;
        }
        settings->sync();
        if (settings->status() != QSettings::NoError)
            success = false;
    }
    return success;
}

static bool persistMigrationState(
    QSettings *settings,
    const QString &key,
    const QString &state)
{
    if (!settings || key.isEmpty() || state.isEmpty())
        return false;

    const bool hadPrevious = settings->contains(key);
    const QVariant previous = settings->value(key);
    settings->setValue(key, state);
    settings->sync();
    if (settings->status() == QSettings::NoError)
        return true;

    // A later application sync must not persist a markerless partial target.
    if (state == legacyMigrationStarted)
        return false;

    if (hadPrevious) {
        settings->setValue(key, previous);
    } else {
        settings->remove(key);
    }
    return false;
}

template <typename CopyValues>
static bool runLegacyMigration(
    QSettings *markerSettings,
    const QString &markerKey,
    const QList<QSettings *> &scopeSettings,
    const QList<QSettings *> &syncTargets,
    const CopyValues &copyValues,
    const QStringList &ignoredApplicationKeys = {},
    bool preserveMacSingleKeyBehavior = false)
{
    if (!markerSettings)
        return false;

    const QString state =
        markerSettings->value(markerKey).toString();
    if (state == legacyMigrationComplete)
        return true;
    if (!state.isEmpty()
        && state != legacyMigrationStarted) {
        qWarning()
            << "Unsupported legacy settings migration state in"
            << markerSettings->fileName();
        return false;
    }

    if (state.isEmpty()
        && hasApplicationSettings(
            scopeSettings,
            ignoredApplicationKeys,
            preserveMacSingleKeyBehavior)) {
        return syncMigrationTargets(syncTargets)
            && persistMigrationState(
                markerSettings,
                markerKey,
                legacyMigrationComplete);
    }

    if (state.isEmpty()
        && !persistMigrationState(
            markerSettings,
            markerKey,
            legacyMigrationStarted)) {
        return false;
    }

    copyValues();
    if (!syncMigrationTargets(syncTargets))
        return false;
    return persistMigrationState(
        markerSettings,
        markerKey,
        legacyMigrationComplete);
}

static QString legacyCredentialScopeStorageKey(
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

static QString legacyCredentialFallbackBlockStorageKey(
    const QString &scopeId,
    const QString &credentialKey)
{
    const QByteArray digest = QCryptographicHash::hash(
        scopeId.toUtf8() + '\n' + credentialKey.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral(
        "credential_store/legacy_fallback_blocks/")
        + QString::fromLatin1(digest);
}

static QString credentialLocationClaimStorageKey(
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

static QString canonicalExistingDirectory(
    const QString &path)
{
    const QFileInfo information(path);
    if (!information.isDir())
        return {};
    return information.canonicalFilePath();
}

static QString normalizedFileSystemPath(
    const QString &path)
{
    QString normalized = QDir::fromNativeSeparators(
        QDir::cleanPath(path));
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    return normalized;
}

static bool credentialAuthorityIsExternal(
    QSettings *authority,
    const QString &athletesRoot)
{
    if (!authority || authority->fileName().isEmpty())
        return false;
    const QString canonicalRoot =
        canonicalExistingDirectory(athletesRoot);
    const QFileInfo authorityInformation(
        authority->fileName());
    const QString canonicalAuthority =
        authorityInformation.isFile()
        ? authorityInformation.canonicalFilePath()
        : QString();
    if (canonicalRoot.isEmpty()
        || canonicalAuthority.isEmpty()) {
        return false;
    }

    const QString root =
        normalizedFileSystemPath(canonicalRoot);
    const QString candidate =
        normalizedFileSystemPath(canonicalAuthority);
    return candidate != root
        && !candidate.startsWith(
            root + QLatin1Char('/'));
}

static bool fileSystemPathIsRedirected(
    const QFileInfo &information)
{
    if (information.isSymLink())
        return true;
#ifdef Q_OS_WIN
    const QString nativePath =
        QDir::toNativeSeparators(
            information.absoluteFilePath());
    const DWORD attributes = ::GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(
            nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return error != ERROR_FILE_NOT_FOUND
            && error != ERROR_PATH_NOT_FOUND;
    }
    return attributes
        & FILE_ATTRIBUTE_REPARSE_POINT;
#else
    return false;
#endif
}

static std::unique_ptr<QSettings> freshExactSettings(
    QSettings *source)
{
    if (!source || source->fileName().isEmpty())
        return {};
    auto exact = std::make_unique<QSettings>(
        source->fileName(), source->format());
    exact->setFallbacksEnabled(false);
    if (!source->group().isEmpty())
        exact->beginGroup(source->group());
    return exact;
}

struct ExactSettingPresence
{
    bool readable = false;
    bool present = false;
    QVariant value;
};

enum class LegacyCredentialFallbackDisposition
{
    Allowed,
    Blocked,
    Unavailable
};

static ExactSettingPresence exactSettingPresence(
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
    return {
        true,
        present,
        present ? exact->value(key) : QVariant()
    };
}

struct ExactCredentialMetadata
{
    bool readable = false;
    bool bindingPresent = false;
    QVariant binding;
    bool scopePresent = false;
    QVariant scope;
};

static ExactCredentialMetadata exactCredentialMetadata(
    QSettings *settings)
{
    const CredentialSettings::LocalMetadataSnapshot
        snapshot =
            CredentialSettings::readLocalMetadata(
                settings, QString(),
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey);
    if (!snapshot.readable)
        return {};
    return {
        true,
        snapshot.bindingPresent,
        snapshot.binding,
        snapshot.scopePresent,
        snapshot.scope
    };
}

static bool persistLegacyCredentialFallbackBlock(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey)
{
    if (!settings || scopeId.isEmpty()
        || credentialKey.isEmpty()) {
        return false;
    }

    const QString blockKey =
        legacyCredentialFallbackBlockStorageKey(
            scopeId, credentialKey);
    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return false;
    exact->sync();
    if (exact->status() != QSettings::NoError)
        return false;
    if (!exact->contains(blockKey)) {
        exact->setValue(blockKey, true);
        exact->sync();
        if (exact->status() != QSettings::NoError
            || !exact->contains(blockKey)) {
            return false;
        }
    }
    if (!CredentialSettings::hardenSettingsFile(
            exact.get())) {
        return false;
    }

    const ExactSettingPresence persisted =
        exactSettingPresence(settings, blockKey);
    return persisted.readable && persisted.present;
}

static LegacyCredentialFallbackDisposition
legacyCredentialFallbackDisposition(
    QSettings *settings,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey)
{
    if (!settings || scopeId.isEmpty()
        || credentialKey.isEmpty()
        || plaintextKey.isEmpty()) {
        return LegacyCredentialFallbackDisposition::
            Unavailable;
    }

    std::unique_ptr<QSettings> exact =
        freshExactSettings(settings);
    if (!exact)
        return LegacyCredentialFallbackDisposition::
            Unavailable;
    exact->sync();
    if (exact->status() != QSettings::NoError)
        return LegacyCredentialFallbackDisposition::
            Unavailable;

    const QString blockKey =
        legacyCredentialFallbackBlockStorageKey(
            scopeId, credentialKey);
    if (exact->contains(blockKey)) {
        return LegacyCredentialFallbackDisposition::
            Blocked;
    }
    if (!exact->contains(plaintextKey)) {
        return LegacyCredentialFallbackDisposition::
            Allowed;
    }
    return persistLegacyCredentialFallbackBlock(
               settings, scopeId, credentialKey)
        ? LegacyCredentialFallbackDisposition::Blocked
        : LegacyCredentialFallbackDisposition::
              Unavailable;
}

static bool protectPresentCredentialTargets(
    QSettings *settings,
    const QString &scopeId,
    const QString &prefix)
{
    if (!settings || scopeId.isEmpty())
        return false;

    bool available = true;
    for (const QString &credentialKey :
         CredentialSettings::credentialKeysForPrefix(
             prefix)) {
        const qsizetype marker =
            credentialKey.indexOf(QLatin1Char('>'));
        if (marker < 0) {
            available = false;
            continue;
        }
        const QString plaintextKey =
            credentialKey.mid(marker + 1);
        if (legacyCredentialFallbackDisposition(
                settings, scopeId, credentialKey,
                plaintextKey)
            == LegacyCredentialFallbackDisposition::
                Unavailable) {
            available = false;
        }
    }
    return available;
}

static QSettings *globalSettingsAt(
    const QVector<QSettings *> *settings,
    int index)
{
    if (!settings || index < 0
        || index >= settings->size()) {
        return nullptr;
    }
    return settings->at(index);
}

static QString unboundLocalCredentialScope(
    QSettings *settings)
{
    const ExactCredentialMetadata metadata =
        exactCredentialMetadata(settings);
    if (!metadata.readable
        || metadata.bindingPresent
        || !metadata.scopePresent) {
        return {};
    }
    const QUuid identifier(
        metadata.scope.toString());
    return identifier.isNull()
        ? QString()
        : identifier.toString(
              QUuid::WithoutBraces);
}

static QString selectedLegacyAthletesRootFromExact(
    QSettings *exact)
{
    if (!exact)
        return {};

    QString configuredKey =
        QStringLiteral(GC_HOMEDIR);
    configuredKey.remove(
        QRegularExpression(QStringLiteral("^<.*>")));
    const QString configuredRoot =
        exact->value(configuredKey).toString();

    if (!configuredRoot.isEmpty()) {
        if (!QFileInfo(configuredRoot).isAbsolute())
            return {};
        return canonicalExistingDirectory(
            configuredRoot);
    }

    const QString oldRoot =
        canonicalExistingDirectory(
            QDir::home().filePath(
                QStringLiteral(
                    "Library/GoldenCheetah")));
    if (!oldRoot.isEmpty())
        return oldRoot;

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QString platformPath =
        QStringLiteral("Library/GoldenCheetah");
#elif defined(Q_OS_WIN)
    const QString platformPath =
        QStandardPaths::standardLocations(
            QStandardPaths::AppLocalDataLocation)
            .value(0);
#else
    const QString platformPath =
        QStringLiteral(".goldencheetah");
#endif
    if (platformPath.isEmpty())
        return {};
    return canonicalExistingDirectory(
        QFileInfo(platformPath).isAbsolute()
        ? platformPath
        : QDir::home().filePath(platformPath));
}

#ifdef GC_CREDENTIAL_TEST_HOOKS
static std::function<void()> credentialLegacyScopeSnapshotHook;
static std::function<void()> credentialLegacyValueSnapshotHook;

static void runCredentialLegacyScopeSnapshotHook()
{
    std::function<void()> hook =
        std::move(credentialLegacyScopeSnapshotHook);
    credentialLegacyScopeSnapshotHook = {};
    if (hook)
        hook();
}

static void runCredentialLegacyValueSnapshotHook()
{
    std::function<void()> hook =
        std::move(credentialLegacyValueSnapshotHook);
    credentialLegacyValueSnapshotHook = {};
    if (hook)
        hook();
}
#endif

struct AuthorizedLegacyCredentialSnapshot
{
    bool authorized = false;
    QString value;
};

static AuthorizedLegacyCredentialSnapshot
authorizedLegacyCredentialSnapshot(
    QSettings *source,
    const QString &activeAthletesRoot,
    const QString &scopeKey,
    const QString &expectedScopeId,
    const QString &storedKey)
{
    if (!source || activeAthletesRoot.isEmpty()
        || scopeKey.isEmpty()
        || expectedScopeId.isEmpty()
        || storedKey.isEmpty()) {
        return {};
    }

    if (!CredentialSettings::hardenSettingsFile(source))
        return {};

    std::unique_ptr<QSettings> exact =
        freshExactSettings(source);
    if (!exact)
        return {};
    exact->sync();
    if (exact->status() != QSettings::NoError
        || selectedLegacyAthletesRootFromExact(
               exact.get()) != activeAthletesRoot
        || !exact->contains(scopeKey)
        || !exact->contains(storedKey)) {
        return {};
    }
    const QUuid expectedScope(expectedScopeId);
    const QUuid storedScope(
        exact->value(scopeKey).toString());
    if (expectedScope.isNull()
        || storedScope != expectedScope) {
        return {};
    }
    const QString value =
        exact->value(storedKey).toString();
#ifdef GC_CREDENTIAL_TEST_HOOKS
    runCredentialLegacyValueSnapshotHook();
#endif
    return {true, value};
}

static QString exactGlobalSettingsPath(
    const QString &athletesRoot,
    const QString &fileName)
{
    const QString root =
        canonicalExistingDirectory(athletesRoot);
    if (root.isEmpty() || fileName.isEmpty())
        return {};

    const QString expectedPath =
        QDir(root).filePath(fileName);
    const QFileInfo information(expectedPath);
    if (fileSystemPathIsRedirected(information))
        return {};
    if (information.exists()) {
        const QString canonical =
            information.canonicalFilePath();
        if (!information.isFile()
            || canonical.isEmpty()
            || QFileInfo(canonical).absolutePath()
                != root) {
            return {};
        }
    }
    return QDir::cleanPath(
        QFileInfo(expectedPath).absoluteFilePath());
}

static bool settingsTargetsExactPath(
    QSettings *settings,
    const QString &expectedPath)
{
    return settings
        && !expectedPath.isEmpty()
        && QDir::cleanPath(
               QFileInfo(settings->fileName())
                   .absoluteFilePath())
            == expectedPath;
}

static bool isSafeAthleteDirectoryName(
    const QString &athleteName)
{
    return !athleteName.isEmpty()
        && athleteName != QStringLiteral(".")
        && athleteName != QStringLiteral("..")
        && !athleteName.contains(QLatin1Char('/'))
        && !athleteName.contains(QLatin1Char('\\'));
}

static QString exactAthletePrivateSettingsPath(
    const QString &athletesRoot,
    const QString &athleteName)
{
    if (athletesRoot.isEmpty()
        || !isSafeAthleteDirectoryName(athleteName)) {
        return {};
    }
    const QString root =
        canonicalExistingDirectory(athletesRoot);
    if (root.isEmpty())
        return {};

    const QFileInfo athleteLocation(
        QDir(root).filePath(athleteName));
    if (fileSystemPathIsRedirected(
            athleteLocation)) {
        return {};
    }
    const QString athlete =
        canonicalExistingDirectory(
            athleteLocation.absoluteFilePath());
    if (athlete.isEmpty()
        || QFileInfo(athlete).absolutePath() != root) {
        return {};
    }
    const QFileInfo configLocation(
        QDir(athlete).filePath(
            QStringLiteral("config")));
    if (fileSystemPathIsRedirected(
            configLocation)) {
        return {};
    }
    const QString config =
        canonicalExistingDirectory(
            configLocation.absoluteFilePath());
    if (config.isEmpty()
        || QFileInfo(config).absolutePath() != athlete) {
        return {};
    }

    const QString privatePath = QDir(config).filePath(
        settingFileNamesAthlete[ATHLETE_PRIVATE]);
    const QFileInfo privateInformation(privatePath);
    if (fileSystemPathIsRedirected(
            privateInformation)) {
        return {};
    }
    if (privateInformation.exists()) {
        const QString canonicalPrivate =
            privateInformation.canonicalFilePath();
        if (!privateInformation.isFile()
            || canonicalPrivate.isEmpty()
            || QFileInfo(canonicalPrivate).absolutePath()
                != config) {
            return {};
        }
    }
    return privatePath;
}


static QString DetermineKey(QString & key, int& store, int& fileIndex) {

    store = SETTINGS_SYSTEM; // default to systemsettings
    fileIndex = 0;
    if (key.startsWith(GC_QSETTINGS_GLOBAL_GENERAL)) {
        store = SETTINGS_GLOBAL;
        fileIndex = GLOBAL_GENERAL;
    } else if (key.startsWith(GC_QSETTINGS_GLOBAL_TRAIN)) {
        store = SETTINGS_GLOBAL;
        fileIndex = GLOBAL_TRAINMODE;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_GENERAL)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_GENERAL;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_LAYOUT)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_LAYOUT;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_PREFERENCES)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_PREFERENCES;
    } else if (key.startsWith(GC_QSETTINGS_ATHLETE_PRIVATE)) {
        store = SETTINGS_ATHLETE;
        fileIndex = ATHLETE_PRIVATE;
    }

    // and make sure <> text is removed
    return key.remove(QRegularExpression("^<.*>"));

}


// -----------------------------constructor and public instance methods ------------------------//

GSettings::GSettings(QString org, QString app)
    : GSettings(
          org,
          app,
          QSettings::NativeFormat,
          QSettings::IniFormat)
{
}

GSettings::GSettings(
    QString org,
    QString app,
    QSettings::Format legacyFormat,
    QSettings::Format targetFormat)
    : newFormat(true),
      newSettingsFormat(targetFormat)
{
    credentialSettings = new CredentialSettings(
        settingsCredentialStore(accessMutex));
    oldsystemsettings = new QSettings(
        legacyFormat, QSettings::UserScope, org, app);
    systemsettings = new QSettings(
        targetFormat, QSettings::UserScope, org, app);
    {
        ScopedQSettingsFallbackDisabler exactTarget(systemsettings);
        const bool hasMarker = systemsettings->contains(
            legacySystemMigrationMarkerKey);
        preparedSystemMigrationState =
            systemsettings->value(
                legacySystemMigrationMarkerKey).toString();
        systemMigrationStateDurable =
            systemsettings->status() == QSettings::NoError
            && hasMarker
            && (preparedSystemMigrationState
                    == legacyMigrationStarted
                || preparedSystemMigrationState
                    == legacyMigrationComplete);
        if (systemsettings->status() == QSettings::NoError
            && !hasMarker) {
            preparedSystemMigrationState =
                hasApplicationSettings(
                    {systemsettings}, {}, true)
                    ? legacyMigrationComplete
                    : legacyMigrationStarted;
        }
    }
    global = new QVector<QSettings*>();
}

GSettings::GSettings(QString file, QSettings::Format format)
    : newFormat(false),
      newSettingsFormat(format)
{
    credentialSettings = new CredentialSettings(
        settingsCredentialStore(accessMutex));
    systemsettings = new QSettings(file,format);
}

GSettings::~GSettings() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    accessMutex.waitForCredentialBackends();
    syncQSettings();
    if (global) {
        qDeleteAll(*global);
        delete global;
    }
    qDeleteAll(athlete);
    delete oldsystemsettings;
    delete systemsettings;
    delete credentialSettings;
}

bool GSettings::ensureSystemMigrationStateDurable()
{
    if (!newFormat)
        return true;
    if (preparedSystemMigrationState
            != legacyMigrationStarted
        && preparedSystemMigrationState
            != legacyMigrationComplete) {
        return false;
    }
    if (systemMigrationStateDurable)
        return true;

    ScopedQSettingsFallbackDisabler exactTarget(systemsettings);
    systemsettings->setValue(
        legacySystemMigrationMarkerKey,
        preparedSystemMigrationState);
    systemsettings->sync();
    systemMigrationStateDurable =
        systemsettings->status() == QSettings::NoError;
    return systemMigrationStateDurable;
}

bool GSettings::ensureCredentialRootReady()
{
    QSettings *globalGeneral =
        globalSettingsAt(global, GLOBAL_GENERAL);
    if (!newFormat || !credentialSettings
        || !systemsettings || !globalGeneral
        || activeAthletesRoot.isEmpty()
        || credentialRootBlocked) {
        return false;
    }
    if (!ensureSystemMigrationStateDurable())
        return false;
    if (!credentialAuthorityIsExternal(
            systemsettings, activeAthletesRoot)
        || !CredentialSettings::hardenSettingsFile(
            systemsettings)) {
        credentialRootBlocked = true;
        credentialSettings->clearCache();
        qWarning()
            << "Credential enrollment authority is not"
            << "external to the active library root";
        return false;
    }

    const QString expectedGlobalPath =
        exactGlobalSettingsPath(
            activeAthletesRoot,
            settingFileNamesGlobal[GLOBAL_GENERAL]);
    if (!settingsTargetsExactPath(
            globalGeneral,
            expectedGlobalPath)) {
        credentialRootBlocked = true;
        credentialSettings->clearCache();
        qWarning()
            << "Global credential settings escape"
            << "the active library root";
        return false;
    }

    const CredentialSettings::LocalMetadataSnapshot
        rootMetadata =
            CredentialSettings::readLocalMetadata(
                globalGeneral,
                credentialRootIdentityStorageKey,
                credentialScopeBindingStorageKey,
                QString());
    if (!rootMetadata.readable) {
        return false;
    }
    const bool freshRoot =
        !rootMetadata.identityPresent
        && !rootMetadata.bindingPresent;
    CredentialSettings::LocationEnrollmentResult
        enrollment;
    QString currentRootId;
    if (freshRoot) {
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("root"),
                    QString(), QString(),
                    activeAthletesRoot, true);
        if (!enrollment.succeeded())
            return false;
        currentRootId =
            CredentialSettings::ensureIdentityId(
                globalGeneral,
                credentialRootIdentityStorageKey,
                credentialScopeBindingStorageKey,
                nullptr,
                enrollment.identityId);
    } else {
        currentRootId =
            CredentialSettings::ensureIdentityId(
                globalGeneral,
                credentialRootIdentityStorageKey,
                credentialScopeBindingStorageKey);
        if (currentRootId.isEmpty())
            return false;
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("root"),
                    currentRootId, QString(),
                    activeAthletesRoot, false);
    }
    if (currentRootId.isEmpty())
        return false;
    if (!credentialRootIdentityId.isEmpty()
        && currentRootId
            != credentialRootIdentityId) {
        credentialRootBlocked = true;
        qWarning()
            << "Credential library identity changed";
        return false;
    }
    if (enrollment.status
        == CredentialSettings::
            LocationClaimStatus::Conflict) {
        credentialRootBlocked = true;
        qWarning()
            << "Credential library identity is already"
            << "bound to another location";
        return false;
    }
    if (enrollment.status
        != CredentialSettings::
            LocationClaimStatus::Success) {
        return false;
    }
    if (enrollment.identityId != currentRootId
        || (enrollment.pending
            && !CredentialSettings::
                completeLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("root"),
                    currentRootId, QString(),
                    activeAthletesRoot,
                    globalGeneral,
                    credentialRootIdentityStorageKey,
                    QString(), QString(),
                    QString(), QString(), QString()))) {
        return false;
    }
    credentialRootIdentityId = currentRootId;
    return true;
}

bool GSettings::claimCredentialProfile(
    const QString &athleteName,
    const QString &profileId,
    bool allowCreate)
{
    if (!ensureCredentialRootReady())
        return false;
    const QString privatePath =
        athletePrivateSettingsPath(athleteName);
    if (privatePath.isEmpty())
        return false;
    const QString configPath =
        QFileInfo(privatePath).absolutePath();
    const QString athletePath =
        QFileInfo(configPath).absolutePath();
    const CredentialSettings::LocationClaimStatus claim =
        CredentialSettings::ensureLocationClaim(
            systemsettings,
            credentialLocationClaimStorageKey(
                QByteArrayLiteral("profile"),
                profileId),
            profileId,
            credentialRootIdentityId,
            athletePath,
            allowCreate);
    if (claim
        != CredentialSettings::
            LocationClaimStatus::Success) {
        if (claim
            == CredentialSettings::
                LocationClaimStatus::Conflict) {
            qWarning()
                << "Credential profile identity is already"
                << "bound to another location";
        }
        return false;
    }
    return true;
}

bool GSettings::claimCredentialScope(
    const QString &scopeId,
    const QString &ownerId,
    const QString &directoryPath,
    bool allowCreate)
{
    if (!systemsettings)
        return false;
    const CredentialSettings::LocationClaimStatus claim =
        CredentialSettings::ensureLocationClaim(
            systemsettings,
            credentialLocationClaimStorageKey(
                QByteArrayLiteral("scope"),
                scopeId),
            scopeId,
            ownerId,
            directoryPath,
            allowCreate);
    if (claim
        != CredentialSettings::
            LocationClaimStatus::Success) {
        if (claim
            == CredentialSettings::
                LocationClaimStatus::Conflict) {
            qWarning()
                << "Credential scope is already"
                << "bound to another owner or location";
        }
        return false;
    }
    return true;
}

QString GSettings::authorizedLegacyCredentialScope(
    const QString &athleteName)
{
    if (!systemsettings
        || activeAthletesRoot.isEmpty()) {
        return {};
    }

    std::unique_ptr<QSettings> exact =
        freshExactSettings(systemsettings);
    if (!exact)
        return {};
    exact->sync();
    const QString storageKey =
        legacyCredentialScopeStorageKey(athleteName);
    if (exact->status()
            != QSettings::NoError
        || selectedLegacyAthletesRootFromExact(
               exact.get()) != activeAthletesRoot
        || !exact->contains(storageKey)) {
        return {};
    }
    const QString stored =
        exact->value(storageKey).toString();
    const QUuid identifier(stored);
    if (identifier.isNull())
        return {};
#ifdef GC_CREDENTIAL_TEST_HOOKS
    runCredentialLegacyScopeSnapshotHook();
#endif
    return identifier.toString(QUuid::WithoutBraces);
}

#ifdef GC_CREDENTIAL_TEST_HOOKS
void GSettings::setCredentialLegacyScopeSnapshotHook(
    std::function<void()> hook)
{
    credentialLegacyScopeSnapshotHook = std::move(hook);
}

void GSettings::setCredentialLegacyValueSnapshotHook(
    std::function<void()> hook)
{
    credentialLegacyValueSnapshotHook = std::move(hook);
}

void GSettings::setCredentialBackendWaitTimeoutForTest(
    int timeoutMilliseconds)
{
    credentialBackendWaitTimeoutForTest.store(
        timeoutMilliseconds, std::memory_order_release);
}

QString GSettings::credentialLegacyScopeForTest(
    const QString &athleteName)
{
    if (accessMutex.credentialBackendSuspended())
        return {};
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    return authorizedLegacyCredentialScope(athleteName);
}
#endif

QString GSettings::credentialScopeForGlobal(
    bool *authorizedLegacy)
{
    if (authorizedLegacy)
        *authorizedLegacy = false;
    if (!credentialSettings) return QString();
    if (!newFormat) return credentialScopeForLegacy(QString());
    QSettings *globalGeneral =
        globalSettingsAt(global, GLOBAL_GENERAL);
    if (!globalGeneral) return QString();
    if (!ensureCredentialRootReady()) {
        return QString();
    }
    const QString authorizedLegacyScope =
        authorizedLegacyCredentialScope(QString());
    const QString localLegacyScope =
        unboundLocalCredentialScope(globalGeneral);
    const QString authorizedLegacyProfile =
        authorizedLegacyScope.isEmpty()
            ? QString()
            : credentialRootIdentityId;
    const ExactCredentialMetadata metadata =
        exactCredentialMetadata(globalGeneral);
    if (!metadata.readable)
        return {};
    const bool authorizedUnboundLegacy =
        !localLegacyScope.isEmpty()
        && localLegacyScope
            == authorizedLegacyScope
        && !authorizedLegacyProfile.isEmpty();
    CredentialSettings::LocationEnrollmentResult
        enrollment;
    CredentialSettings::ScopeBindingResult binding;
    if (authorizedUnboundLegacy) {
        if (!claimCredentialScope(
                localLegacyScope,
                credentialRootIdentityId,
                activeAthletesRoot,
                true)) {
            return {};
        }
        binding =
            CredentialSettings::ensureScopeBinding(
                globalGeneral,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile);
    } else if (metadata.bindingPresent) {
        binding =
            CredentialSettings::ensureScopeBinding(
                globalGeneral,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile);
        if (binding.succeeded()
            && binding.legacyLocalScope) {
            if (!claimCredentialScope(
                    binding.scopeId,
                    credentialRootIdentityId,
                    activeAthletesRoot,
                    true)) {
                return {};
            }
        } else if (binding.succeeded()) {
            enrollment =
                CredentialSettings::
                    ensureLocationEnrollment(
                        systemsettings,
                        QByteArrayLiteral("scope"),
                        binding.scopeId,
                        credentialRootIdentityId,
                        activeAthletesRoot, false);
        }
    } else {
        const bool fresh =
            !metadata.bindingPresent
            && !metadata.scopePresent;
        enrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("scope"),
                    metadata.scopePresent
                        ? metadata.scope.toString()
                        : QString(),
                    credentialRootIdentityId,
                    activeAthletesRoot, fresh);
        if (!enrollment.succeeded())
            return {};
        binding =
            CredentialSettings::ensureScopeBinding(
                globalGeneral,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile,
                credentialRootIdentityId,
                enrollment.identityId);
    }
    if (!binding.succeeded()) {
        qWarning()
            << "Cannot resolve global credential binding";
        return {};
    }
    if (!binding.legacyLocalScope
        && (!enrollment.succeeded()
            || enrollment.identityId
                != binding.scopeId
            || (enrollment.pending
                && !CredentialSettings::
                    completeLocationEnrollment(
                        systemsettings,
                        QByteArrayLiteral("scope"),
                        binding.scopeId,
                        credentialRootIdentityId,
                        activeAthletesRoot,
                        globalGeneral, QString(),
                        credentialScopeBindingStorageKey,
                        credentialScopeStorageKey,
                        credentialRootIdentityId,
                        credentialRootIdentityId,
                        binding.scopeId)))) {
        return {};
    }
    if (authorizedLegacy)
        *authorizedLegacy =
            binding.legacyLocalScope;
    return binding.scopeId;
}

QString GSettings::athletePrivateSettingsPath(
    const QString &athleteName) const
{
    return exactAthletePrivateSettingsPath(
        activeAthletesRoot, athleteName);
}

QString GSettings::athleteConfigDirectory(
    const QString &athleteName) const
{
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!newFormat || athlete.constFind(athleteName) == athlete.cend())
        return {};
    const QString privatePath =
        athletePrivateSettingsPath(athleteName);
    if (privatePath.isEmpty()) return {};
    const QFileInfo privateFile(privatePath);
    const QString configPath = QDir::cleanPath(
        privateFile.absolutePath());
    const QFileInfo config(configPath);
    return config.exists() && config.isDir()
            && !config.isSymLink()
        ? config.canonicalFilePath()
        : QString();
}

QString GSettings::credentialScopeForAthlete(
    const QString &athleteName,
    bool *authorizedLegacy)
{
    if (authorizedLegacy)
        *authorizedLegacy = false;
    if (!credentialSettings || athleteName.isEmpty()) return QString();
    if (!newFormat) return credentialScopeForLegacy(athleteName);
    if (!ensureCredentialRootReady())
        return {};
    const auto found = athlete.constFind(athleteName);
    if (found == athlete.cend()) return QString();
    QSettings *privateSettings =
        found.value()->getQSettings(ATHLETE_PRIVATE);
    const QString expectedPath =
        athletePrivateSettingsPath(athleteName);
    return credentialScopeForAthleteSettings(
        athleteName, privateSettings,
        expectedPath, authorizedLegacy);
}

QString GSettings::credentialScopeForAthleteSettings(
    const QString &athleteName,
    QSettings *privateSettings,
    const QString &expectedPath,
    bool *authorizedLegacy)
{
    if (authorizedLegacy)
        *authorizedLegacy = false;
    if (!privateSettings || expectedPath.isEmpty()
        || QDir::cleanPath(
               QFileInfo(
                   privateSettings->fileName())
                   .absoluteFilePath())
            != QDir::cleanPath(
                QFileInfo(expectedPath)
                    .absoluteFilePath())) {
        qWarning()
            << "Athlete credential settings escape"
            << "the active library root";
        return {};
    }
    if (!ensureCredentialRootReady())
        return {};
    const QString configPath =
        QFileInfo(expectedPath).absolutePath();
    const QString athletePath =
        QFileInfo(configPath).absolutePath();
    const QString authorizedLegacyScope =
        authorizedLegacyCredentialScope(athleteName);
    const QString localLegacyScope =
        unboundLocalCredentialScope(privateSettings);
    const QString authorizedLegacyProfile =
        authorizedLegacyScope;
    const ExactCredentialMetadata metadata =
        exactCredentialMetadata(privateSettings);
    if (!metadata.readable)
        return {};
    const bool authorizedUnboundLegacy =
        !localLegacyScope.isEmpty()
        && localLegacyScope
            == authorizedLegacyScope
        && !authorizedLegacyProfile.isEmpty();
    CredentialSettings::LocationEnrollmentResult
        profileEnrollment;
    CredentialSettings::LocationEnrollmentResult
        scopeEnrollment;
    CredentialSettings::ScopeBindingResult binding;
    if (authorizedUnboundLegacy) {
        if (!claimCredentialProfile(
                athleteName,
                authorizedLegacyProfile,
                true)
            || !claimCredentialScope(
                localLegacyScope,
                authorizedLegacyProfile,
                athletePath,
                true)) {
            return {};
        }
        binding =
            CredentialSettings::ensureScopeBinding(
                privateSettings,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile);
    } else if (metadata.bindingPresent) {
        binding =
            CredentialSettings::ensureScopeBinding(
                privateSettings,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile);
        if (binding.succeeded()
            && binding.legacyLocalScope) {
            if (!claimCredentialProfile(
                    athleteName,
                    binding.profileId, true)
                || !claimCredentialScope(
                    binding.scopeId,
                    binding.profileId,
                    athletePath, true)) {
                return {};
            }
        } else if (binding.succeeded()) {
            profileEnrollment =
                CredentialSettings::
                    ensureLocationEnrollment(
                        systemsettings,
                        QByteArrayLiteral("profile"),
                        binding.profileId,
                        credentialRootIdentityId,
                        athletePath, false);
            if (profileEnrollment.succeeded()) {
                scopeEnrollment =
                    CredentialSettings::
                        ensureLocationEnrollment(
                            systemsettings,
                            QByteArrayLiteral("scope"),
                            binding.scopeId,
                            binding.profileId,
                            athletePath, false);
            }
        }
    } else {
        const bool fresh =
            !metadata.bindingPresent
            && !metadata.scopePresent;
        profileEnrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("profile"),
                    QString(),
                    credentialRootIdentityId,
                    athletePath, fresh);
        if (!profileEnrollment.succeeded())
            return {};
        scopeEnrollment =
            CredentialSettings::
                ensureLocationEnrollment(
                    systemsettings,
                    QByteArrayLiteral("scope"),
                    metadata.scopePresent
                        ? metadata.scope.toString()
                        : QString(),
                    profileEnrollment.identityId,
                    athletePath, fresh);
        if (!scopeEnrollment.succeeded())
            return {};
        binding =
            CredentialSettings::ensureScopeBinding(
                privateSettings,
                credentialRootIdentityId,
                credentialScopeBindingStorageKey,
                credentialScopeStorageKey,
                authorizedLegacyScope,
                authorizedLegacyProfile,
                profileEnrollment.identityId,
                scopeEnrollment.identityId);
    }
    if (!binding.succeeded()) {
        qWarning()
            << "Cannot resolve athlete credential binding";
        return {};
    }
    if (!binding.legacyLocalScope
        && (!profileEnrollment.succeeded()
            || !scopeEnrollment.succeeded()
            || profileEnrollment.identityId
                != binding.profileId
            || scopeEnrollment.identityId
                != binding.scopeId
            || (scopeEnrollment.pending
                && !CredentialSettings::
                    completeLocationEnrollment(
                        systemsettings,
                        QByteArrayLiteral("scope"),
                        binding.scopeId,
                        binding.profileId,
                        athletePath,
                        privateSettings, QString(),
                        credentialScopeBindingStorageKey,
                        credentialScopeStorageKey,
                        credentialRootIdentityId,
                        binding.profileId,
                        binding.scopeId))
            || (profileEnrollment.pending
                && !CredentialSettings::
                    completeLocationEnrollment(
                        systemsettings,
                        QByteArrayLiteral("profile"),
                        binding.profileId,
                        credentialRootIdentityId,
                        athletePath,
                        privateSettings, QString(),
                        credentialScopeBindingStorageKey,
                        credentialScopeStorageKey,
                        credentialRootIdentityId,
                        binding.profileId,
                        binding.scopeId)))) {
        return {};
    }
    if (authorizedLegacy)
        *authorizedLegacy =
            binding.legacyLocalScope;
    return binding.scopeId;
}

QString GSettings::credentialScopeForLegacy(
    const QString &athleteName)
{
    if (!credentialSettings || !systemsettings) return QString();
    if (newFormat)
        return {};
    return CredentialSettings::ensureScopeId(
        systemsettings,
        legacyCredentialScopeStorageKey(athleteName));
}

bool GSettings::suppressAutomaticLegacyCredentialMigration(
    const QString &credentialKey)
{
    if (!CredentialSettings::isCredentialKey(credentialKey)) {
        return false;
    }
    if (oldsystemsettings) {
        CredentialSettings::hardenSettingsFile(
            oldsystemsettings);
    }
    return true;
}

QVariant GSettings::credentialValueWithLegacyFallback(
    QSettings *target,
    const QString &scopeId,
    const QString &credentialKey,
    const QString &plaintextKey,
    const QString &legacyStoredKey,
    const QString &legacyScopeKey,
    bool authorizedLegacy,
    const QVariant &defaultValue,
    bool requireLiveVault,
    bool *authoritativeMissResult,
    bool *confirmedVaultValueResult)
{
    if (authoritativeMissResult)
        *authoritativeMissResult = false;
    if (confirmedVaultValueResult)
        *confirmedVaultValueResult = false;
    if (!credentialSettings || !target
        || scopeId.isEmpty()) {
        return defaultValue;
    }

    const ExactSettingPresence before =
        exactSettingPresence(target, plaintextKey);
    if (!before.readable)
        return defaultValue;
    const LegacyCredentialFallbackDisposition
        fallbackDisposition =
            legacyCredentialFallbackDisposition(
                target, scopeId, credentialKey,
                plaintextKey);
    if (fallbackDisposition
        == LegacyCredentialFallbackDisposition::
            Unavailable) {
        return defaultValue;
    }

    const bool legacyCandidate =
        authorizedLegacy && oldsystemsettings
        && !legacyStoredKey.isEmpty()
        && !legacyScopeKey.isEmpty()
        && fallbackDisposition
            == LegacyCredentialFallbackDisposition::Allowed
        && !before.present;
    bool authoritativeMiss = false;
    bool confirmedVaultValue = false;
    const QVariant current = credentialSettings->value(
        target, scopeId, credentialKey, plaintextKey,
        QVariant(),
        legacyCandidate || requireLiveVault
            ? CredentialSettings::ReadPolicy::RequireLiveVault
            : CredentialSettings::ReadPolicy::AllowFreshCache,
        &authoritativeMiss,
        &confirmedVaultValue);
    const ExactSettingPresence after =
        exactSettingPresence(target, plaintextKey);

    if (current.isValid()) {
        if (!confirmedVaultValue) {
            if (requireLiveVault)
                return defaultValue;
            return current;
        }
        if (!persistLegacyCredentialFallbackBlock(
                target, scopeId, credentialKey)) {
            qWarning()
                << "Cannot persist legacy credential"
                << "fallback protection";
            return defaultValue;
        }
        if (confirmedVaultValueResult)
            *confirmedVaultValueResult = true;
        return current;
    }

    if (!legacyCandidate) {
        if (authoritativeMissResult)
            *authoritativeMissResult = authoritativeMiss;
        return defaultValue;
    }

    if (fallbackDisposition
            != LegacyCredentialFallbackDisposition::
                Allowed
        || !authoritativeMiss
        || !after.readable || before.present
        || after.present) {
        return defaultValue;
    }

    const AuthorizedLegacyCredentialSnapshot legacy =
        authorizedLegacyCredentialSnapshot(
            oldsystemsettings, activeAthletesRoot,
            legacyScopeKey, scopeId,
            legacyStoredKey);
    if (!legacy.authorized || legacy.value.isEmpty()) {
        return defaultValue;
    }

    if (legacyCredentialFallbackDisposition(
            target, scopeId, credentialKey,
            plaintextKey)
        != LegacyCredentialFallbackDisposition::Allowed) {
        return defaultValue;
    }
    if (confirmedVaultValueResult)
        *confirmedVaultValueResult = true;
    return legacy.value;
}

void GSettings::migrateGlobalCredentials()
{
    QSettings *settings =
        globalSettingsAt(global, GLOBAL_GENERAL);
    if (!credentialSettings || !newFormat
        || !settings) return;
    const QString scopeId =
        credentialScopeForGlobal();
    if (!protectPresentCredentialTargets(
            settings, scopeId,
            QStringLiteral(
                GC_QSETTINGS_GLOBAL_GENERAL))) {
        return;
    }
    credentialSettings->migratePlaintext(
        settings, scopeId,
        QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL));
    for (const QString &key :
         CredentialSettings::credentialKeysForPrefix(
             QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
        migrateValue(key);
    }
}

void GSettings::migrateAthleteCredentials(
    const QString &athleteName)
{
    if (!credentialSettings || !newFormat) return;
    const auto found = athlete.constFind(athleteName);
    if (found == athlete.cend()) return;
    QSettings *settings =
        found.value()->getQSettings(ATHLETE_PRIVATE);
    const QString scopeId =
        credentialScopeForAthlete(athleteName);
    if (!protectPresentCredentialTargets(
            settings, scopeId,
            QStringLiteral(
                GC_QSETTINGS_ATHLETE_PRIVATE))) {
        return;
    }
    credentialSettings->migratePlaintext(
        settings, scopeId,
        QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE));
    for (const QString &key :
         CredentialSettings::credentialKeysForPrefix(
             QStringLiteral(GC_QSETTINGS_ATHLETE_PRIVATE))) {
        migrateCValue(athleteName, key);
    }
}


QVariant
GSettings::value(const QObject * /*me*/, const QString key, const QVariant def) {
    const bool credential =
        CredentialSettings::isCredentialKey(key);
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (credentialSettings
        && credential) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            QSettings *target =
                globalSettingsAt(global, file);
            if (store != SETTINGS_GLOBAL
                || !target) {
                return def;
            }
            bool authorizedLegacy = false;
            const QString scopeId =
                credentialScopeForGlobal(
                    &authorizedLegacy);
            return credentialValueWithLegacyFallback(
                target, scopeId, key, plaintextKey,
                plaintextKey,
                legacyCredentialScopeStorageKey(
                    QString()),
                authorizedLegacy, def);
        }
        if (!key.startsWith(
                QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            return def;
        }
        return credentialSettings->value(
            systemsettings, credentialScopeForLegacy(QString()),
            key, plaintextKey, def);
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            return systemsettings->value(keyVar, def);
            break;
        case SETTINGS_GLOBAL: {
            QSettings *target =
                globalSettingsAt(global, file);
            return target
                ? target->value(keyVar, def)
                : def;
        }
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "GetValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;
        }

    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->value(keyVar, def);
    }
    return QVariant();
}

void
GSettings::setValue(QString key, QVariant value)
{
    setValueChecked(key, value);
}

bool
GSettings::setValueChecked(QString key, QVariant value)
{
    const bool credential =
        CredentialSettings::isCredentialKey(key);
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (credentialSettings
        && credential) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            QSettings *target =
                globalSettingsAt(global, file);
            if (store == SETTINGS_GLOBAL
                && target) {
                const QString scopeId =
                    credentialScopeForGlobal();
                if (scopeId.isEmpty()
                    || !persistLegacyCredentialFallbackBlock(
                        target, scopeId, key)) {
                    return false;
                }
                return credentialSettings->setValueChecked(
                    target, scopeId,
                    key, plaintextKey, value);
            }
        } else if (key.startsWith(
                       QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            return credentialSettings->setValueChecked(
                systemsettings, credentialScopeForLegacy(QString()),
                key, plaintextKey, value);
        }
        return false;
    }

    bool written = false;
    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            if (!ensureSystemMigrationStateDurable())
                break;
            systemsettings->setValue(keyVar,value);
            written = true;
            break;
        case SETTINGS_GLOBAL: {
            QSettings *target =
                globalSettingsAt(global, file);
            if (target) {
                target->setValue(keyVar, value);
                written = true;
            }
        }
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "SetValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;

        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->setValue(keyVar, value);
        written = true;
    }

    return written;
}

void
GSettings::remove(const QString &key)
{
    const bool credential =
        CredentialSettings::isCredentialKey(key);
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (credentialSettings
        && credential) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            QSettings *target =
                globalSettingsAt(global, file);
            if (store == SETTINGS_GLOBAL
                && target) {
                const QString scopeId =
                    credentialScopeForGlobal();
                if (scopeId.isEmpty()
                    || !persistLegacyCredentialFallbackBlock(
                        target, scopeId, key)) {
                    return;
                }
                credentialSettings->remove(
                    target, scopeId,
                    key, plaintextKey);
            }
        } else if (key.startsWith(
                       QStringLiteral(GC_QSETTINGS_GLOBAL_GENERAL))) {
            credentialSettings->remove(
                systemsettings, credentialScopeForLegacy(QString()),
                key, plaintextKey);
        }
        return;
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            if (!ensureSystemMigrationStateDurable())
                return;
            systemsettings->remove(keyVar);
            break;
        case SETTINGS_GLOBAL: {
            QSettings *target =
                globalSettingsAt(global, file);
            if (target)
                target->remove(keyVar);
        }
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "remove key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
            break;
        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->remove(keyVar);
    }
}

// access to athlete specific config
QVariant
GSettings::cvalue(QString athleteName, QString key, QVariant def) {
    const bool credential =
        CredentialSettings::isCredentialKey(key);
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (athleteName.isNull() || athleteName.isEmpty()) return def;

    if (credentialSettings
        && credential) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            if (store == SETTINGS_GLOBAL) {
                QSettings *target =
                    globalSettingsAt(global, file);
                if (!target) return def;
                bool authorizedLegacy = false;
                const QString scopeId =
                    credentialScopeForGlobal(
                        &authorizedLegacy);
                return credentialValueWithLegacyFallback(
                    target, scopeId, key, plaintextKey,
                    plaintextKey,
                    legacyCredentialScopeStorageKey(
                        QString()),
                    authorizedLegacy, def);
            }
            if (store == SETTINGS_ATHLETE) {
                const auto found = athlete.constFind(athleteName);
                if (found != athlete.cend()) {
                    bool authorizedLegacy = false;
                    const QString scopeId =
                        credentialScopeForAthlete(
                            athleteName,
                            &authorizedLegacy);
                    return credentialValueWithLegacyFallback(
                        found.value()->getQSettings(file),
                        scopeId, key, plaintextKey,
                        athleteName + QLatin1Char('/')
                            + plaintextKey,
                        legacyCredentialScopeStorageKey(
                            athleteName),
                        authorizedLegacy, def);
                }
                const QString privatePath =
                    athletePrivateSettingsPath(athleteName);
                if (!privatePath.isEmpty()
                    && !credentialRootBlocked) {
                    QSettings privateSettings(
                        privatePath, newSettingsFormat);
                    bool authorizedLegacy = false;
                    const QString scopeId =
                        credentialScopeForAthleteSettings(
                            athleteName,
                            &privateSettings,
                            privatePath,
                            &authorizedLegacy);
                    return credentialValueWithLegacyFallback(
                        &privateSettings,
                        scopeId,
                        key, plaintextKey,
                        athleteName + QLatin1Char('/')
                            + plaintextKey,
                        legacyCredentialScopeStorageKey(
                            athleteName),
                        authorizedLegacy, def);
                }
            }
            return def;
        }

        const bool globalCredential =
            store == SETTINGS_GLOBAL;
        const QString storedKey = globalCredential
            ? plaintextKey
            : athleteName + QLatin1Char('/') + plaintextKey;
        return credentialSettings->value(
            systemsettings,
            credentialScopeForLegacy(
                globalCredential ? QString() : athleteName),
            key, storedKey, def);
    }

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);

        QHash<QString, AthleteQSettings*>::const_iterator i = athlete.find(athleteName);
        if (i != athlete.end()) {
            switch (store) {
            case SETTINGS_SYSTEM:
            case SETTINGS_GLOBAL:
                qDebug() << "GetCValue key, keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
                break;
            case SETTINGS_ATHLETE:
                return i.value()->getQSettings(file)->value(keyVar, def);
                break;
            }
        } else {
            // fall back to old settings - assuming that this can only happen during the upgrade of an athlete
            // and before the new /config folder exists
            return oldsystemsettings->value(athleteName+"/"+keyVar, def);
        }

    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->value(athleteName+"/"+keyVar, def);
    }
    return QVariant();

}

GSettings::CredentialReadResult
GSettings::credentialCValueChecked(
    const QString &athleteName,
    const QString &key)
{
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (athleteName.isEmpty() || !credentialSettings
        || !CredentialSettings::isCredentialKey(key)) {
        return {};
    }

    const auto classified = [](
        const QVariant &value,
        bool authoritativeMiss,
        bool confirmedVaultValue) {
        if (confirmedVaultValue) {
            return CredentialReadResult{
                CredentialReadStatus::Present, value};
        }
        if (authoritativeMiss) {
            return CredentialReadResult{
                CredentialReadStatus::NotFound, QVariant()};
        }
        return CredentialReadResult{};
    };

    QString plaintextKey = key;
    int store;
    int file;
    plaintextKey = DetermineKey(plaintextKey, store, file);
    if (newFormat) {
        QSettings *target = nullptr;
        QString scopeId;
        QString legacyStoredKey;
        QString legacyScopeKey;
        bool authorizedLegacy = false;
        if (store == SETTINGS_GLOBAL) {
            target = globalSettingsAt(global, file);
            scopeId = credentialScopeForGlobal(
                &authorizedLegacy);
            legacyStoredKey = plaintextKey;
            legacyScopeKey = legacyCredentialScopeStorageKey(
                QString());
        } else if (store == SETTINGS_ATHLETE) {
            const auto found = athlete.constFind(athleteName);
            if (found != athlete.cend()) {
                target = found.value()->getQSettings(file);
                scopeId = credentialScopeForAthlete(
                    athleteName, &authorizedLegacy);
            } else {
                const QString privatePath =
                    athletePrivateSettingsPath(athleteName);
                if (privatePath.isEmpty() || credentialRootBlocked)
                    return {};
                QSettings privateSettings(
                    privatePath, newSettingsFormat);
                scopeId = credentialScopeForAthleteSettings(
                    athleteName, &privateSettings,
                    privatePath, &authorizedLegacy);
                bool authoritativeMiss = false;
                bool confirmedVaultValue = false;
                const QVariant value =
                    credentialValueWithLegacyFallback(
                        &privateSettings, scopeId, key,
                        plaintextKey,
                        athleteName + QLatin1Char('/')
                            + plaintextKey,
                        legacyCredentialScopeStorageKey(
                            athleteName),
                        authorizedLegacy, QVariant(), true,
                        &authoritativeMiss,
                        &confirmedVaultValue);
                return classified(
                    value, authoritativeMiss,
                    confirmedVaultValue);
            }
            legacyStoredKey = athleteName + QLatin1Char('/')
                + plaintextKey;
            legacyScopeKey = legacyCredentialScopeStorageKey(
                athleteName);
        } else {
            return {};
        }
        if (!target || scopeId.isEmpty()) return {};
        bool authoritativeMiss = false;
        bool confirmedVaultValue = false;
        const QVariant value = credentialValueWithLegacyFallback(
            target, scopeId, key, plaintextKey,
            legacyStoredKey, legacyScopeKey,
            authorizedLegacy, QVariant(), true,
            &authoritativeMiss, &confirmedVaultValue);
        return classified(
            value, authoritativeMiss, confirmedVaultValue);
    }

    const bool globalCredential = store == SETTINGS_GLOBAL;
    const QString storedKey = globalCredential
        ? plaintextKey
        : athleteName + QLatin1Char('/') + plaintextKey;
    bool authoritativeMiss = false;
    bool confirmedVaultValue = false;
    const QVariant value = credentialSettings->value(
        systemsettings,
        credentialScopeForLegacy(
            globalCredential ? QString() : athleteName),
        key, storedKey, QVariant(),
        CredentialSettings::ReadPolicy::RequireLiveVault,
        &authoritativeMiss, &confirmedVaultValue);
    return classified(
        value, authoritativeMiss, confirmedVaultValue);
}

void
GSettings::setCValue(QString athleteName, QString key, QVariant value)
{
    setCValueChecked(athleteName, key, value);
}

bool
GSettings::setCValueChecked(
    QString athleteName, QString key, QVariant value)
{
    const bool credential =
        CredentialSettings::isCredentialKey(key);
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (credentialSettings
        && credential) {
        QString plaintextKey = key;
        int store;
        int file;
        plaintextKey = DetermineKey(plaintextKey, store, file);
        if (newFormat) {
            QSettings *target =
                globalSettingsAt(global, file);
            if (store == SETTINGS_GLOBAL
                && target) {
                const QString scopeId =
                    credentialScopeForGlobal();
                if (scopeId.isEmpty()
                    || !persistLegacyCredentialFallbackBlock(
                        target, scopeId, key)) {
                    return false;
                }
                return credentialSettings->setValueChecked(
                    target, scopeId,
                    key, plaintextKey, value);
            } else if (store == SETTINGS_ATHLETE) {
                const auto found = athlete.constFind(athleteName);
                if (found != athlete.cend()) {
                    const QString scopeId =
                        credentialScopeForAthlete(athleteName);
                    QSettings *target =
                        found.value()->getQSettings(file);
                    if (!target || scopeId.isEmpty()
                        || !persistLegacyCredentialFallbackBlock(
                            target, scopeId, key)) {
                        return false;
                    }
                    return credentialSettings->setValueChecked(
                        target, scopeId,
                        key, plaintextKey, value);
                }
            }
        } else {
            const bool globalCredential =
                store == SETTINGS_GLOBAL;
            const QString storedKey = globalCredential
                ? plaintextKey
                : athleteName + QLatin1Char('/') + plaintextKey;
            return credentialSettings->setValueChecked(
                systemsettings,
                credentialScopeForLegacy(
                    globalCredential ? QString() : athleteName),
                key, storedKey, value);
        }
        return false;
    }

    bool written = false;
    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        QHash<QString, AthleteQSettings*>::const_iterator i = athlete.find(athleteName);
        if (i != athlete.end()) {
            switch (store) {
            case SETTINGS_SYSTEM:
            case SETTINGS_GLOBAL:
                qDebug() << "SetCValue keyVar, store:" << key << ":" << keyVar  << ": " << store; // error cases on code configuration
                break;
            case SETTINGS_ATHLETE:
                i.value()->getQSettings(file)->setValue(keyVar, value);
                written = true;
                break;
            }
        } // if we do have have the athlete - then we do not store anything
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        systemsettings->setValue(athleteName + "/" + keyVar,value);
        written = true;

    }
    return written;
}

// other functions unsed from QSettings which GSettings needs to implement
QStringList
GSettings::allKeys() const {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (newFormat) {
        QStringList allKeys, tempKeys;
        tempKeys = systemsettings->allKeys();
        foreach (QString key, tempKeys) {
           allKeys.append(GC_QSETTINGS_SYSTEM+key);
        }
        if (QSettings *settings =
                globalSettingsAt(
                    global, GLOBAL_GENERAL)) {
            tempKeys = settings->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(
                   GC_QSETTINGS_GLOBAL_GENERAL+key);
            }
        }
        if (QSettings *settings =
                globalSettingsAt(
                    global, GLOBAL_TRAINMODE)) {
            tempKeys = settings->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(
                   GC_QSETTINGS_GLOBAL_TRAIN+key);
            }
        }
        QHashIterator<QString, AthleteQSettings*> i(athlete);
        i.toFront();
        while (i.hasNext())
        { i.next();
            tempKeys = i.value()->getQSettings(ATHLETE_GENERAL)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_GENERAL+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_LAYOUT)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_LAYOUT+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_PREFERENCES)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_PREFERENCES+key);
            }
            tempKeys = i.value()->getQSettings(ATHLETE_PRIVATE)->allKeys();
            foreach (QString key, tempKeys) {
               allKeys.append(GC_QSETTINGS_ATHLETE_PRIVATE+key);
            }
        }
        allKeys.removeDuplicates();  // remove duplicate keys from the Athlete Settings
        return allKeys;
    } else {
        return systemsettings->allKeys();
    }
    return QStringList();
}

bool
GSettings::contains(const QString & key) const {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    QString keyVar = QString(key);
    if (newFormat) {
        int store;
        int file;
        keyVar = DetermineKey(keyVar, store, file);
        switch (store) {
        case SETTINGS_SYSTEM:
            return systemsettings->contains(keyVar);
        case SETTINGS_GLOBAL: {
            QSettings *target =
                globalSettingsAt(global, file);
            return target
                && target->contains(keyVar);
        }
            break;
        case SETTINGS_ATHLETE:
            qDebug() << "Contains Value key:" << key << "keyVar:" << keyVar; // error cases on code configuration
            return false;
            break;
        }
    } else {
        keyVar.remove(QRegularExpression("^<.*>"));
        return systemsettings->contains(keyVar);
    }
    return false;
}

bool GSettings::containsValueTarget(QString key) const
{
    if (!newFormat) {
        key.remove(QRegularExpression("^<.*>"));
        return systemsettings->contains(key);
    }

    int store;
    int file;
    key = DetermineKey(key, store, file);
    switch (store) {
    case SETTINGS_SYSTEM: {
        ScopedQSettingsFallbackDisabler exactTarget(
            systemsettings);
        return systemsettings->contains(key);
    }
    case SETTINGS_GLOBAL: {
        QSettings *target =
            globalSettingsAt(global, file);
        return target && target->contains(key);
    }
    case SETTINGS_ATHLETE:
        return false;
    }
    return false;
}

bool GSettings::containsCValueTarget(
    const QString &athleteName,
    QString key) const
{
    if (!newFormat || athleteName.isEmpty())
        return false;

    int store;
    int file;
    key = DetermineKey(key, store, file);
    if (store != SETTINGS_ATHLETE)
        return false;

    const auto found = athlete.constFind(athleteName);
    return found != athlete.cend()
        && found.value()->getQSettings(file)
        && found.value()->getQSettings(file)->contains(key);
}

void
GSettings::migrateQSettingsSystem() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!accessMutex.waitForCredentialBackends(
            credentialBackendWaitTimeout())) {
        return;
    }

    if (!newFormat) return;

    if (!ensureSystemMigrationStateDurable()) {
        qWarning()
            << "Cannot persist legacy system migration state";
        CredentialSettings::hardenSettingsFile(systemsettings);
        return;
    }

    ScopedQSettingsFallbackDisabler exactTarget(systemsettings);
    const QList<QSettings *> systemFiles = {systemsettings};
    if (!runLegacyMigration(
            systemsettings,
            legacySystemMigrationMarkerKey,
            systemFiles,
            systemFiles,
            [this] { upgradeSystem(); })) {
        qWarning() << "Legacy system settings migration is incomplete";
    }
    CredentialSettings::hardenSettingsFile(systemsettings);
}


void
GSettings::initializeQSettingsGlobal(QString athletesRootDir) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!accessMutex.waitForCredentialBackends(
            credentialBackendWaitTimeout())) {
        return;
    }

    if (!newFormat) return;

    const QString canonicalRoot =
        canonicalExistingDirectory(athletesRootDir);
    if (canonicalRoot.isEmpty()) {
        if (!activeAthletesRoot.isEmpty()
            || !global->isEmpty()) {
            credentialRootBlocked = true;
            if (credentialSettings)
                credentialSettings->clearCache();
        }
        qWarning() << "Cannot initialize settings for invalid"
                   << "athlete library root";
        return;
    }
    if ((!activeAthletesRoot.isEmpty()
         && activeAthletesRoot != canonicalRoot)
        || (activeAthletesRoot.isEmpty()
            && !global->isEmpty())) {
        credentialRootBlocked = true;
        if (credentialSettings)
            credentialSettings->clearCache();
        qWarning()
            << "Athlete library root changed without"
            << "clearing active settings";
        return;
    }

    const QString globalGeneralPath =
        exactGlobalSettingsPath(
            canonicalRoot,
            settingFileNamesGlobal[GLOBAL_GENERAL]);
    const QString globalTrainModePath =
        exactGlobalSettingsPath(
            canonicalRoot,
            settingFileNamesGlobal[GLOBAL_TRAINMODE]);
    if (globalGeneralPath.isEmpty()
        || globalTrainModePath.isEmpty()) {
        credentialRootBlocked = true;
        if (credentialSettings)
            credentialSettings->clearCache();
        qWarning()
            << "Global settings path escapes"
            << "the athlete library root";
        return;
    }
    activeAthletesRoot = canonicalRoot;

    if (global->isEmpty()) {

        global->append(new QSettings(
            globalGeneralPath,
            newSettingsFormat));
        global->append(new QSettings(
            globalTrainModePath,
            newSettingsFormat));

    }
    if (global->size() != 2
        || !settingsTargetsExactPath(
            global->at(GLOBAL_GENERAL),
            globalGeneralPath)
        || !settingsTargetsExactPath(
            global->at(GLOBAL_TRAINMODE),
            globalTrainModePath)) {
        credentialRootBlocked = true;
        if (credentialSettings)
            credentialSettings->clearCache();
        qWarning()
            << "Active global settings do not belong"
            << "to the athlete library root";
        return;
    }

    if (!ensureSystemMigrationStateDurable()) {
        qWarning()
            << "Cannot persist legacy system migration state";
        return;
    }

    credentialRootBlocked = false;

    const QList<QSettings *> globalFiles = {
        global->at(GLOBAL_GENERAL),
        global->at(GLOBAL_TRAINMODE)
    };
    const QList<QSettings *> syncTargets = {
        systemsettings,
        global->at(GLOBAL_GENERAL),
        global->at(GLOBAL_TRAINMODE)
    };
    if (!runLegacyMigration(
            global->at(GLOBAL_GENERAL),
            legacyGlobalMigrationMarkerKey,
            globalFiles,
            syncTargets,
            [this] { upgradeGlobal(); })) {
        qWarning() << "Legacy global settings migration is incomplete";
    }
    syncQSettingsGlobal();
    if (!ensureCredentialRootReady()) {
        qWarning()
            << "Cannot resolve credential library identity";
        return;
    }
    migrateGlobalCredentials();

}

void
GSettings::initializeQSettingsAthlete(QString athletesRootDir, QString athleteName) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!accessMutex.waitForCredentialBackends(
            credentialBackendWaitTimeout())) {
        return;
    }

    // assumption is that the directory "<athletesRootDir>/athleteName" exists //

    if (!newFormat) return;
    const QString canonicalRoot =
        canonicalExistingDirectory(athletesRootDir);
    if (canonicalRoot.isEmpty()
        || !isSafeAthleteDirectoryName(athleteName)) {
        qWarning() << "Cannot initialize invalid athlete"
                   << "settings path";
        return;
    }
    if (activeAthletesRoot != canonicalRoot) {
        qWarning()
            << "Athlete settings root differs from"
            << "the active library";
        return;
    }

    // handle not yet upgraded athlete folders without causing problems
    // initializing of the QSettings would work anyway - but upgrade would fail,
    // since the /config folder does not exist - so leave the upgrade for the next
    // initialization after successfull upgrade (the case should be rare anyway)
    const QString privatePath =
        athletePrivateSettingsPath(athleteName);
    if (privatePath.isEmpty()) {
        return; // athlete has not yet been migrated and /config does not exist - so wait until next time
    }

    bool initialized = false;
    if (athlete.constFind(athleteName) == athlete.cend()) {
        initializeQSettingsNewAthlete(
            activeAthletesRoot, athleteName);
        initialized = true;
    }

    const auto found = athlete.constFind(athleteName);
    if (found != athlete.cend()) {
        AthleteQSettings *athleteSettings = found.value();
        const QList<QSettings *> athleteFiles = {
            athleteSettings->getQSettings(ATHLETE_GENERAL),
            athleteSettings->getQSettings(ATHLETE_LAYOUT),
            athleteSettings->getQSettings(ATHLETE_PREFERENCES),
            athleteSettings->getQSettings(ATHLETE_PRIVATE)
        };
        if (global && global->size() == 2) {
            const QList<QSettings *> syncTargets = {
                global->at(GLOBAL_GENERAL),
                athleteSettings->getQSettings(ATHLETE_GENERAL),
                athleteSettings->getQSettings(ATHLETE_LAYOUT),
                athleteSettings->getQSettings(ATHLETE_PREFERENCES),
                athleteSettings->getQSettings(ATHLETE_PRIVATE)
            };
            if (!runLegacyMigration(
                    athleteSettings->getQSettings(ATHLETE_GENERAL),
                    legacyAthleteMigrationMarkerKey,
                    athleteFiles,
                    syncTargets,
                    [this, athleteName] {
                        upgradeAthlete(athleteName);
                    })) {
                qWarning()
                    << "Legacy athlete settings migration is incomplete";
            }
        } else {
            qWarning()
                << "Cannot migrate athlete settings before global settings";
        }
        CredentialSettings::hardenSettingsFile(
            athleteSettings->getQSettings(ATHLETE_PRIVATE));
    }
    if (initialized)
        syncQSettingsAllAthletes();
    migrateAthleteCredentials(athleteName);
}

void
GSettings::initializeQSettingsNewAthlete(QString athletesRootDir, QString athleteName) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!accessMutex.waitForCredentialBackends(
            credentialBackendWaitTimeout())) {
        return;
    }

    if (!newFormat) return;
    const QString canonicalRoot =
        canonicalExistingDirectory(athletesRootDir);
    if (canonicalRoot.isEmpty()
        || !isSafeAthleteDirectoryName(athleteName)) {
        return;
    }
    if (activeAthletesRoot != canonicalRoot) {
        return;
    }
    const QString privatePath =
        athletePrivateSettingsPath(athleteName);
    if (privatePath.isEmpty())
        return;

    // create the Athlete QSettings - they MUST not exist yet
    AthleteQSettings* athleteSettings = new AthleteQSettings();
    const QString baseName =
        QFileInfo(privatePath).absolutePath()
        + QLatin1Char('/');
    athleteSettings->setQSettings(
        new QSettings(
            baseName + settingFileNamesAthlete[ATHLETE_GENERAL],
            newSettingsFormat),
        ATHLETE_GENERAL);
    athleteSettings->setQSettings(
        new QSettings(
            baseName + settingFileNamesAthlete[ATHLETE_LAYOUT],
            newSettingsFormat),
        ATHLETE_LAYOUT);
    athleteSettings->setQSettings(
        new QSettings(
            baseName + settingFileNamesAthlete[ATHLETE_PREFERENCES],
            newSettingsFormat),
        ATHLETE_PREFERENCES);
    athleteSettings->setQSettings(
        new QSettings(
            baseName + settingFileNamesAthlete[ATHLETE_PRIVATE],
            newSettingsFormat),
        ATHLETE_PRIVATE);
    athlete.insert(athleteName, athleteSettings);

}


void
GSettings::syncQSettingsAllAthletes() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    syncQSettingsAllAthletesChecked();
}

bool
GSettings::syncQSettingsAllAthletesChecked() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (!newFormat) {
        if (!systemsettings) return false;
        systemsettings->sync();
        CredentialSettings::hardenSettingsFile(systemsettings);
        return systemsettings->status() == QSettings::NoError;
    }

    bool synced = true;
    QHashIterator<QString, AthleteQSettings*> i(athlete);
    i.toFront();
    while (i.hasNext())
    { i.next();
        for (int file = ATHLETE_GENERAL;
             file <= ATHLETE_PRIVATE;
             ++file) {
            QSettings *settings =
                i.value()->getQSettings(file);
            settings->sync();
            if (settings->status() != QSettings::NoError)
                synced = false;
        }
        CredentialSettings::hardenSettingsFile(
            i.value()->getQSettings(ATHLETE_PRIVATE));
    }
    return synced;
}

bool
GSettings::syncCValueChecked(
    const QString &athleteName,
    const QString &key)
{
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!newFormat) {
        if (!systemsettings) return false;
        systemsettings->sync();
        CredentialSettings::hardenSettingsFile(
            systemsettings);
        return systemsettings->status()
            == QSettings::NoError;
    }

    int store;
    int file;
    QString keyVar = key;
    keyVar = DetermineKey(keyVar, store, file);
    Q_UNUSED(keyVar)
    if (store != SETTINGS_ATHLETE)
        return false;

    const auto found = athlete.constFind(athleteName);
    if (found == athlete.cend())
        return false;

    QSettings *settings =
        found.value()->getQSettings(file);
    if (!settings) return false;
    settings->sync();
    if (file == ATHLETE_PRIVATE) {
        CredentialSettings::hardenSettingsFile(
            settings);
    }
    return settings->status() == QSettings::NoError;
}

void
GSettings::syncQSettingsGlobal() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (!newFormat) return;

    QSettings *general =
        globalSettingsAt(global, GLOBAL_GENERAL);
    QSettings *train =
        globalSettingsAt(global, GLOBAL_TRAINMODE);
    if (general && train) {
        general->sync();
        train->sync();
        CredentialSettings::hardenSettingsFile(
            general);
    };
}

void
GSettings::syncQSettings() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    systemsettings->sync();
    CredentialSettings::hardenSettingsFile(systemsettings);
    syncQSettingsGlobal();
    syncQSettingsAllAthletes();

}

bool
GSettings::clearGlobalAndAthletes(
    int credentialBackendTimeoutMilliseconds) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);
    if (!accessMutex.waitForCredentialBackends(
            credentialBackendTimeoutMilliseconds)) {
        return false;
    }

    if (!newFormat) return true;
    syncQSettings();
    qDeleteAll(*global);
    qDeleteAll(athlete);
    global->clear();
    athlete.clear();
    activeAthletesRoot.clear();
    credentialRootIdentityId.clear();
    credentialRootBlocked = false;
    if (credentialSettings) credentialSettings->clearCache();
    return true;
}


/*-------------------------------- special methods for Upgrade/Migration --------------------------
 *
 * The .INI based storage of Settings has been introduced with GoldenCheetah v3.3.0
 *
 * To transition existing settings (in PLISTs (OSX) and Registry (WINDOWS) from the
 * propriety storage to the common .INI files an automatic migration of Settings takes
 * place when no Settings are found. The methods executing the migration are implemented here
 *
 * Any development starting starting after v3.3 (so v4.0 and onwards) does not need
 * to take the migration into account, since any newly defined settings are only stored
 * using the new .INI based technique.
 *
 -----------------------------------------------------------------------------------------------*/

void
GSettings::migrateValue(QString key) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    if (!oldsystemsettings->contains(oldKey))
        return;
    if (suppressAutomaticLegacyCredentialMigration(key))
        return;
    if (!containsValueTarget(key)) {
        setValue(key, oldsystemsettings->value(oldKey));
    }
}

void
GSettings::migrateCValue(QString athlete, QString key) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    const QString storedKey = athlete + QLatin1Char('/') + oldKey;
    if (!oldsystemsettings->contains(storedKey))
        return;
    if (suppressAutomaticLegacyCredentialMigration(key))
        return;
    if (!containsCValueTarget(athlete, key)) {
        setCValue(athlete, key, oldsystemsettings->value(storedKey));
    }
}

void
GSettings::migrateAndRenameCValue(QString athlete, QString wrongKey, QString key) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    wrongKey.remove(QRegularExpression("^<.*>"));
    const QString storedKey = athlete + QLatin1Char('/') + wrongKey;
    if (!oldsystemsettings->contains(storedKey))
        return;
    if (suppressAutomaticLegacyCredentialMigration(key))
        return;
    if (!containsCValueTarget(athlete, key)) {
        setCValue(athlete, key, oldsystemsettings->value(storedKey));
    }
}

void
GSettings::migrateValueToCValue(QString athlete, QString key) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    if (!oldsystemsettings->contains(oldKey))
        return;
    if (suppressAutomaticLegacyCredentialMigration(key))
        return;
    if (!containsCValueTarget(athlete, key)) {
        setCValue(athlete, key, oldsystemsettings->value(oldKey));
    }
}

void
GSettings::migrateCValueToValue(QString athlete, QString key) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    if (containsValueTarget(key))
        return;

    QString oldKey = key;
    oldKey.remove(QRegularExpression("^<.*>"));
    oldKey = athlete + QLatin1Char('/') + oldKey;
    if (!oldsystemsettings->contains(oldKey))
        return;
    if (suppressAutomaticLegacyCredentialMigration(key))
        return;
    setValue(key, oldsystemsettings->value(oldKey));
}


void
GSettings::upgradeSystem() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3

    migrateValue(GC_HOMEDIR);
    migrateValue(GC_SETTINGS_LAST);
    migrateValue(GC_SETTINGS_MAIN_GEOM);
    migrateValue(GC_SETTINGS_MAIN_STATE);
    migrateValue(GC_SETTINGS_LAST_IMPORT_PATH);
    migrateValue(GC_SETTINGS_LAST_WORKOUT_PATH);
    migrateValue(GC_LAST_DOWNLOAD_DEVICE);
    migrateValue(GC_LAST_DOWNLOAD_PORT);
    migrateValue(GC_BE_LASTDIR);
    migrateValue(GC_BE_LASTFMT);
    migrateValue(GC_FONT_DEFAULT);
    migrateValue(GC_FONT_DEFAULT_SIZE);
    migrateValue(GC_FONT_CHARTLABELS);
    migrateValue(GC_FONT_CHARTLABELS_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_TITLES);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CHARTMARKERS);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CALENDAR);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_TITLES_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CHARTMARKERS_SIZE);
    //DEPRECATED IN V3.5 migrateValue(GC_FONT_CALENDAR_SIZE);

    QStringList colorProperties = GCColor::getConfigKeys();
    QStringListIterator colorIterator(colorProperties);
    while (colorIterator.hasNext()) {
        QString key = QString(colorIterator.next().data());
        migrateValue(key);
    }
}

void
GSettings::upgradeGlobal() {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3
    migrateValue(GC_SETTINGS_FAVOURITE_METRICS);
    migrateValue(GC_TABBAR);
    migrateValue(GC_WBALFORM);
    migrateValue(GC_BIKESCOREDAYS);
    migrateValue(GC_BIKESCOREMODE);
    migrateValue(GC_WARNCONVERT);
    migrateValue(GC_WARNEXIT);
    migrateValue(GC_HIST_BIN_WIDTH);
    migrateValue(GC_WORKOUTDIR);
    migrateValue(GC_LINEWIDTH);
    migrateValue(GC_ANTIALIAS);
    migrateValue(GC_RIDESCROLL);
    migrateValue(GC_RIDEHEAD);
    migrateValue(GC_SHADEZONES);
    migrateValue(GC_GARMIN_SMARTRECORD);
    migrateValue(GC_GARMIN_HWMARK);
    migrateValue(GC_DPFG_TOLERANCE);
    migrateValue(GC_DPFG_STOP);
    migrateValue(GC_DPFS_MAX);
    migrateValue(GC_DPFS_VARIANCE);
    migrateValue(GC_DPTA);
    migrateValue(GC_DPPA);
    migrateValue(GC_DPFHRS_MAX);
    migrateValue(GC_DPDP_BIKEWEIGHT);
    migrateValue(GC_DPDP_CRR);
    migrateValue(GC_LANG);
    migrateValue(GC_PACE);
    migrateValue(GC_SWIMPACE);
    migrateValue(GC_ELEVATION_HYSTERESIS);
    migrateValue(GC_START_HTTP);

    // Handle the Dataprocessor dp/%1/apply keys
    // Handle the RideEditor colmap/%1 keys
    QStringList dpKeys = oldsystemsettings->allKeys();
    QStringListIterator dpKeysIterator(dpKeys);
    while (dpKeysIterator.hasNext()) {
        QString key = QString(dpKeysIterator.next().data());
        if (key.startsWith("dp/") || key.startsWith("colmap/")) {
            migrateValue(GC_QSETTINGS_GLOBAL_GENERAL+key);
        }
    }

    // handle the Device configuration
    migrateValue(GC_DEV_COUNT);
    QString devCountKey = GC_DEV_COUNT;
    devCountKey.remove(QRegularExpression("^<.*>"));
    QVariant configVal = oldsystemsettings->value(devCountKey);
    int devicecount;
    if (configVal.isNull()) {
        devicecount=0;
    } else {
        devicecount = configVal.toInt();
    }
    for (int i = 0; i < devicecount; i++ ) {
        QString configStr = QString("%1%2").arg(GC_DEV_NAME).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_TYPE).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_WHEEL).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_SPEC).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_PROF).arg(i+1);
        migrateValue(configStr);
        configStr = QString("%1%2").arg(GC_DEV_VIRTUAL).arg(i+1);
        migrateValue(configStr);
    }

    migrateValue(FORTIUS_FIRMWARE);
    migrateValue(TRAIN_MULTI);

}


void
GSettings::upgradeAthlete(QString athlete) {
    const std::lock_guard<SettingsAccessMutex> lock(accessMutex);

    // by explicitely naming all the properties, and not choosing the "allKeys()" function,
    // only the properties still in use are migrated - and not any orphans for previous releases

    // NOTE: Migrating values is only required for settings introduced in GC version until v3.3

    migrateCValue(athlete, GC_VERSION_USED);
    migrateCValue(athlete, GC_SAFEEXIT);
    migrateCValue(athlete, GC_UPGRADE_FOLDER_SUCCESS);
    migrateCValue(athlete, GC_LTM_LAST_DATE_RANGE);
    migrateCValue(athlete, GC_LTM_AUTOFILTERS);
    migrateCValue(athlete, GC_BLANK_ANALYSIS);
    migrateCValue(athlete, GC_BLANK_TRAIN);
    migrateCValue(athlete, GC_BLANK_HOME);
    migrateCValue(athlete, GC_BLANK_PLAN);
    migrateCValue(athlete, GC_NICKNAME);
    migrateCValue(athlete, GC_DOB);
    migrateCValue(athlete, GC_WEIGHT);
    migrateCValue(athlete, GC_HEIGHT);
    migrateCValue(athlete, GC_WBALTAU);
    migrateCValue(athlete, GC_SEX);
    migrateCValue(athlete, GC_BIO);
    migrateCValue(athlete, GC_AVATAR);
    migrateCValue(athlete, GC_DISCOVERY);
    migrateCValue(athlete, GC_SB_TODAY);
    migrateCValue(athlete, GC_LTS_DAYS);
    migrateCValue(athlete, GC_STS_DAYS);
    migrateCValue(athlete, GC_NAVHEADINGS);
    migrateCValue(athlete, GC_NAVGROUPBY);
    migrateCValue(athlete, GC_SORTBY);
    migrateCValue(athlete, GC_WEBCAL_URL);
    migrateCValue(athlete, GC_USE_CP_FOR_FTP);

    migrateAndRenameCValue(athlete, "bavigator/headingwidths", GC_NAVHEADINGWIDTHS);
    migrateCValueToValue(athlete, GC_UNIT);

    // Handle the splittersizes keys
    QStringList splitterKeys = oldsystemsettings->allKeys();
    QStringListIterator splitterKeysIterator(splitterKeys);
    while (splitterKeysIterator.hasNext()) {
        QString key = QString(splitterKeysIterator.next().data());
        if (key.startsWith(athlete + "/mainwindow/splitterSizes") || key.startsWith(athlete+"/splitter")) {
            key.remove(0, athlete.size()+1); // remove the Athlete name and / from the old setting !
            migrateCValue(athlete, GC_QSETTINGS_ATHLETE_LAYOUT+key);
        }
    }

    // --- private --- //
    migrateCValue(athlete, GC_RWGPSUSER);
    migrateCValue(athlete, GC_RWGPSPASS);
    migrateCValue(athlete, GC_WIURL);
    migrateCValue(athlete, GC_WIUSER);
    migrateCValue(athlete, GC_WIKEY);
    migrateCValue(athlete, GC_DVURL);
    migrateCValue(athlete, GC_DVUSER);
    migrateCValue(athlete, GC_DVPASS);
    migrateCValue(athlete, GC_DVCALDAVTYPE);
    migrateCValue(athlete, GC_STRAVA_TOKEN);
    migrateCValue(athlete, GC_CYCLINGANALYTICS_TOKEN);

    // migrate from system/global to athlete specific settings
    migrateValueToCValue(athlete, GC_CRANKLENGTH);
    migrateValueToCValue(athlete, GC_WHEELSIZE);

}

static QString fontfamilyfallback[] = {
#ifdef Q_OS_LINUX
    // try pretty fonts first (you never know)
    "Noto Sans Display", // google free font
    "Clear Sans", // intel free font
    "DejaVu Sans", // gnome free font
    "Liberation Sans", // red hat free font

    // then distro specific ones
    "Ubuntu",
    "Red Hat Display",

#endif
#ifdef Q_OS_WIN
    "Segoe UI",
    "Calibri",
    "Microsoft Sans Serif",
#endif
#ifdef Q_OS_MAC
    "SF Pro Display",
    "PT Sans",
    "Helvetica Neue",
#endif

    // common fonts
    "Trebuchet MS",
    "Helvetica",

    // on all OS these two should exist at a minimum
    "Verdana",
    "Arial",
    NULL
};


// font selection and scaling uses slightly smaller fonts on MacOS
#ifdef Q_OS_MAC
#define FONTROWS 48
#else
#define FONTROWS 43
#endif

AppearanceSettings
GSettings::defaultAppearanceSettings()
{
    AppearanceSettings returning;

    // lets get the geometry of the window next
    // since its used to scale and set other
    // appearance settings
    QRect screensize = QApplication::primaryScreen()->availableGeometry();

    // leave 12% of the screen free to the left and right of the main window
    // and same number of pixels above and below
    double width = screensize.width() * 0.88;
    double margin = (screensize.width() - width) / 2.0;
    returning.windowsize.setWidth(screensize.width() - margin);
    returning.windowsize.setHeight(screensize.height() - margin);
    returning.windowsize.setX(margin);
    returning.windowsize.setY(margin);

    // sidebars should be about 20% of width and no more
    returning.sidebarwidth = returning.windowsize.width() / 5;

    // lets find an appropriate font
    returning.fontfamily = QFont().toString(); // ultimately fall back to QT default
    QFontDatabase fontdb;
    for(int i=0; !fontfamilyfallback[i].isEmpty(); i++) {

        foreach(QString family, fontdb.families()) {

            // is it installed ?
            if (family == fontfamilyfallback[i]) {
                returning.fontfamily = fontfamilyfallback[i];
                goto breakout;
            }
        }
    }

breakout:

    returning.fontpointsize = 11; // default

    // scaling only applies on hidpi displays
    returning.fontscale = 1.0;
    returning.fontscaleindex = 4;
    returning.xfactor = 1.0;
    returning.yfactor = 1.0;

    // dpiXFactor and dpiYFactor are used to scale across the code
    // typically to increase the size of widgets but also some other
    // graphical elements
    if (QApplication::primaryScreen()->devicePixelRatio() <= 1 && screensize.width() > 2160) {
       // we're on a hidpi screen - lets create a multiplier - always use smallest
       returning.xfactor = screensize.width() / 1280.0;
       returning.yfactor = screensize.height() / 1024.0;

        // always make the same, use smallest scaling when x and y differ
       if (returning.yfactor < returning.xfactor) returning.xfactor = returning.yfactor;
       else if (returning.xfactor < returning.yfactor) returning.yfactor = returning.xfactor;

    }

    // we also need to make sure fonts are scaled to be large/small enough
    // to use the screen estate reasonably- whilst some users will prefer
    // small fonts, we scale to a size that looks the same on all resolutions
    // and avoid overly small fonts. Users can of course adjust the scaling
    // to their own preferences later
    for (int i=0; scalefactors[i] != 0; i++) {

        QFont font(returning.fontfamily);
        font.setPointSizeF(returning.fontpointsize * scalefactors[i]);
        QFontMetricsF metrics(font);
        double height = metrics.boundingRect("TEST").height();

        if (returning.windowsize.height() / height < FONTROWS) {
            returning.fontscale = scalefactors[i];
            returning.fontscaleindex = i;
            break;
        }
    }

    // best settings for UI as now designed
    returning.theme = 5; // team purple colors
    returning.antialias = true;
    returning.macForms = true;
    returning.scrollbar = true;
    returning.head = false;
    returning.sideanalysis = false;
    returning.sidetrend = false;
    returning.sideplan = false;
    returning.sidetrain = true;

    // linewidth must be wholly divisible by 0.5
    // default is historically 2px but 4px is too thick
    // on hidpi displays typically, so we adjust to 3px
    returning.linewidth = dpiXFactor > 1 ? 1.5 * dpiXFactor : 2.0;
    double factor = returning.linewidth / 0.5;
    factor=qRound(factor);
    returning.linewidth = 0.5 * factor;

    return returning;
}

//----------------------------------------------------------------------------------------------//

// initialise with no athlete
#ifdef GC_SETTINGS_NO_GLOBAL_INSTANCE
GSettings *appsettings = nullptr;
#else
GSettings *appsettings = GetApplicationSettings();
#endif
