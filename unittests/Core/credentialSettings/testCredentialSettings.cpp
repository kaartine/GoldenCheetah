#include <QtTest>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <qt_windows.h>
#endif

#include "Core/CredentialSettings.h"
#include "Core/CredentialStoreQtKeychain.h"
#include "Core/Settings.h"
#include "Gui/Colors.h"

namespace {

struct FakeStoreState
{
    QHash<QString, QString> values;
    bool failReads = false;
    CredentialStore::Status failedReadStatus =
        CredentialStore::Status::Unavailable;
    bool failWrites = false;
    bool commitFailedWrites = false;
    bool failRemoves = false;
    int reads = 0;
    int writes = 0;
    int removes = 0;
    std::function<void()> beforeRead;
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
        if (state_->failReads) {
            return {state_->failedReadStatus, QString(),
                    QStringLiteral("unavailable")};
        }
        if (!state_->values.contains(key)) {
            return {Status::NotFound, QString(), QString()};
        }
        return {Status::Success, state_->values.value(key),
                QString()};
    }

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
        ++state_->writes;
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
    explicit FileCredentialStore(QString path)
        : path_(std::move(path))
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

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
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
        if (existed_) {
            qputenv(name_.constData(), previous_);
        } else {
            qunsetenv(name_.constData());
        }
    }

    Q_DISABLE_COPY(ScopedEnvironmentVariable)

private:
    QByteArray name_;
    bool existed_;
    QByteArray previous_;
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
    if (state.enabled
        && migrationWritePoint(settings)
            == state.failurePoint) {
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
    bool allowEveryone)
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
    access[0].grfInheritance =
        SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ::BuildTrusteeWithSidW(
        &access[0].Trustee, userSid);
    ULONG count = 1;
    if (allowEveryone) {
        access[1].grfAccessPermissions =
            FILE_ALL_ACCESS;
        access[1].grfAccessMode = SET_ACCESS;
        access[1].grfInheritance =
            SUB_CONTAINERS_AND_OBJECTS_INHERIT;
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
        && acl && protectedDacl;
    bool currentUserAllowed = false;
    if (valid) {
        for (DWORD index = 0;
             index < acl->AceCount; ++index) {
            void *rawAce = nullptr;
            if (!::GetAce(acl, index, &rawAce)) {
                valid = false;
                break;
            }
            auto *header =
                static_cast<ACE_HEADER *>(rawAce);
            if (header->AceType
                != ACCESS_ALLOWED_ACE_TYPE) {
                continue;
            }
            auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(
                    rawAce);
            PSID trustee = &ace->SidStart;
            if (!::IsValidSid(trustee)
                || !::EqualSid(trustee, userSid)
                || (header->AceFlags
                    & INHERITED_ACE)
                || (ace->Mask & FILE_ALL_ACCESS)
                    != FILE_ALL_ACCESS) {
                valid = false;
                break;
            }
            currentUserAllowed = true;
        }
    }
    ::LocalFree(descriptor);
    return valid && currentUserAllowed;
}
#endif

} // namespace

std::unique_ptr<CredentialStore>
createPlatformCredentialStore()
{
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
    void credentialClassification_data();
    void credentialClassification();
    void keychainStatusMapping_data();
    void keychainStatusMapping();
    void linuxKeychainRuntimeStatusReport_data();
    void linuxKeychainRuntimeStatusReport();
    void bundledLinuxRuntimePathRequiresContainedRegularFile();
    void keychainJobsDisablePlaintextFallback();
    void platformStoreRoundTripsOrFailsClosed();
    void vaultCallsDoNotMutateFallbackState();
    void credentialOperationsAreSerialized();
    void reentrantCredentialOperationFailsFast();
    void credentialProcessLockIsExclusive();
    void activeCredentialProcessLockDoesNotExpire();
    void scopeCreationIsSerialized();
    void scopeProcessLockCanonicalizesAliases();
    void persistedCachesTrackOtherInstances_data();
    void persistedCachesTrackOtherInstances();
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
    void plaintextMigratesToVault();
    void vaultValueWinsAndPlaintextIsRemoved();
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
    void replacementRecoveryFinalizesCommittedVaultWrite();
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
    void credentialStateDurabilityFailureFailsClosed_data();
    void credentialStateDurabilityFailureFailsClosed();
    void credentialPostMutationDurabilityFailure_data();
    void credentialPostMutationDurabilityFailure();
    void credentialCrashRecoveryAcrossProcesses();
    void credentialStateAncestorSyncFailureFailsClosed();
    void partialCredentialStateAncestryFailsClosed();
    void unsafeCredentialStateAncestorFailsClosed();
    void insecureCredentialStateRootFailsClosed();
    void symlinkedCredentialStateDirectoryFailsClosed();
    void windowsCredentialDirectoriesUseOwnerOnlyAcl();
    void windowsWritableCredentialRootFailsClosed();
    void reissuedDeleteResumesDurableTransaction();
    void reportedMarkerFailureRecoversDurableMarker();
    void failedMarkerPersistenceLeavesPreparationState();
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
    void transientReadBeforeMissingCredentialRetriesMigration();
    void scopesAreIsolated();
    void scopeIdentifiersAreStableAndValidated();
    void scopeCreationFailsClosedWhenItCannotPersist();
    void migratePlaintextCoversConfiguredCredentials();
    void gsettingsRoutesCredentialsToVault();
    void gsettingsCheckedCredentialWriteReportsPersistence();
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
    void newFormatMigrationScrubsLegacyCredential();
    void newFormatFailedMigrationIsRetriedWithoutCredentialLoss();
    void preInitializationMigrationKeepsAthleteScope();
    void postInitializationFallbackKeepsAthleteScope();

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
    credentialScrubFaultState() = {};
    if (!ownedCredentialStateRoot_.isEmpty()) {
        QDir stateDirectory(QDir(ownedCredentialStateRoot_)
            .filePath(QStringLiteral(
                "GoldenCheetah/credential-locks")));
        if (stateDirectory.exists()) {
            QVERIFY(stateDirectory.removeRecursively());
        }
    }
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

    QVERIFY(firstResult != secondResult);
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
    state->beforeWrite = [&] {
        reentrantValue = credentials.value(
            &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
            QStringLiteral("operation-busy"));
    };

    QVERIFY(credentials.setValueChecked(
        &ini, scope, GC_STRAVA_TOKEN, plaintextKey,
        QStringLiteral("written-secret")));
    QCOMPARE(reentrantValue,
             QVariant(QStringLiteral("operation-busy")));
    QCOMPARE(state->reads, 0);
    QCOMPARE(state->writes, 1);
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

    QVERIFY(firstScope.isEmpty() != secondScope.isEmpty());
    const QString created = firstScope.isEmpty()
        ? secondScope : firstScope;
    QVERIFY(!QUuid(created).isNull());

    QSettings persisted(path, format);
    QCOMPARE(CredentialSettings::ensureScopeId(
                 &persisted, storageKey),
             created);
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
    QCOMPARE(state->reads, 2);
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

void TestCredentialSettings::vaultValueWinsAndPlaintextIsRemoved()
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
    QVERIFY(!ini.contains(plainKey(GC_STRAVA_TOKEN)));
    ini.sync();
    QVERIFY(!fileContents(path).contains("stale-plaintext"));
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
replacementRecoveryFinalizesCommittedVaultWrite()
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
    state->values.insert(
        vaultKey, QStringLiteral("committed-replacement"));
    CredentialSettings credentials(fakeStore(state));
    QCOMPARE(credentials.value(
                 &settings, scope, GC_STRAVA_TOKEN,
                 plaintextKey, QStringLiteral("missing")),
             QVariant(QStringLiteral("committed-replacement")));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->removes, 0);
    QVERIFY(!settings.contains(plaintextKey));
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
    QVERIFY(!settings.contains(plaintextKey));
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
    QVERIFY(!duplicateSource.contains(plaintextKey));
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
    QVERIFY(!duplicateSource.contains(plaintextKey));
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
        deletionPath, QByteArrayLiteral("preparing")));
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
    QVERIFY(!duplicateSource.contains(plaintextKey));
    QVERIFY(credentialPhaseIs(
        deletionPath, QByteArrayLiteral("active")));
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
        QVERIFY(!duplicate.contains(plaintextKey));
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
            deletionPath, QByteArrayLiteral("preparing")));
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
        deletionPath, QByteArrayLiteral("preparing")));
    fault = {};
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
    QVERIFY(!settings.contains(plaintextKey));
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
    const QString vaultSecret = preexistingVault
        ? QStringLiteral("current-vault-secret")
        : legacySecret;
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

    second.setValue(plaintextKey, QStringLiteral("duplicate-secret"));
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
    QCOMPARE(state->reads, 1);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &second, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 0);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(secondPath, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
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
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->writes, 1);
    QCOMPARE(state->values.value(vaultKey), secret);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &legacy, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
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
    duplicate.setValue(
        plaintextKey, QStringLiteral("stale-plaintext"));
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
    QCOMPARE(state->reads, 2);

    fault.enabled = false;
    QCOMPARE(credentials.value(
                 &duplicate, scope, GC_STRAVA_TOKEN, plaintextKey,
                 QStringLiteral("missing")),
             QVariant(secret));
    QCOMPARE(state->reads, 3);
    QCOMPARE(state->writes, 0);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
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
    QCOMPARE(state->reads, 1);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
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
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->writes, 1);

    bool readable = false;
    const QSettings::SettingsMap persisted =
        persistedSettingsMap(path, &readable);
    QVERIFY(readable);
    QVERIFY(!persisted.contains(plaintextKey));
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
    QCOMPARE(state->reads, 1);
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
    QCOMPARE(state->reads, 2);
    QCOMPARE(state->writes, 0);
    QCOMPARE(state->values.value(vaultKey), currentSecret);
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
    QCOMPARE(state->reads, 2);
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
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.migrateQSettingsSystem();
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
    }

    QSettings system(systemPath, QSettings::IniFormat);
    QVERIFY(system.contains(systemExistingKey));
    QCOMPARE(system.value(systemExistingKey).toString(), systemExisting);
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

void TestCredentialSettings::newFormatMigrationScrubsLegacyCredential()
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
    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_RWGPSPASS,
                     QStringLiteral("missing")).toString(),
                 sentinel);
        settings.syncQSettings();
    }

    QSettings legacy(organization, application);
    QVERIFY(!legacy.contains(legacyKey));
    QVERIFY(!fileContents(legacyPath).contains(sentinel.toUtf8()));
    const QString privatePath = athleteRoot
        + QStringLiteral("/Athlete/config/athlete-private.ini");
    QVERIFY(!fileContents(privatePath).contains(sentinel.toUtf8()));
    QCOMPARE(factoryState()->values.size(), 1);
}

void TestCredentialSettings::newFormatFailedMigrationIsRetriedWithoutCredentialLoss()
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
                 sentinel);
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
                 sentinel);
        settings.syncQSettings();
    }

    QSettings migrated(organization, application);
    QVERIFY(!migrated.contains(legacyKey));
    QVERIFY(!fileContents(legacyPath).contains(sentinel.toUtf8()));
    QCOMPARE(factoryState()->values.size(), 1);
}

void TestCredentialSettings::preInitializationMigrationKeepsAthleteScope()
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
    const QString sentinel = QStringLiteral("early-migration-sentinel");
    {
        QSettings legacy(organization, application);
        legacy.setValue(legacyKey, sentinel);
        legacy.sync();
    }

    const QString athleteRoot = temporary.filePath(
        QStringLiteral("athletes"));
    QVERIFY(QDir().mkpath(
        athleteRoot + QStringLiteral("/Athlete/config")));
    factoryState() = std::make_shared<FakeStoreState>();
    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 sentinel);
    }

    {
        GSettings settings(organization, application);
        settings.initializeQSettingsGlobal(athleteRoot);
        settings.initializeQSettingsAthlete(
            athleteRoot, athleteName);
        QCOMPARE(settings.cvalue(
                     athleteName, GC_STRAVA_REFRESH_TOKEN,
                     QStringLiteral("missing")).toString(),
                 sentinel);
    }
    QCOMPARE(factoryState()->values.size(), 1);
}

void TestCredentialSettings::postInitializationFallbackKeepsAthleteScope()
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
             sentinel);
    QCOMPARE(factoryState()->values.size(), 1);
}

QTEST_MAIN(TestCredentialSettings)
#include "testCredentialSettings.moc"
