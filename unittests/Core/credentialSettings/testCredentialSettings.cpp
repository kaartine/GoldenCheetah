#include <QtTest>

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>

#include "Core/CredentialSettings.h"
#include "Core/CredentialStoreQtKeychain.h"
#include "Core/Settings.h"
#include "Gui/Colors.h"

namespace {

struct FakeStoreState
{
    QHash<QString, QString> values;
    bool failReads = false;
    bool failWrites = false;
    bool failRemoves = false;
    int reads = 0;
    int writes = 0;
    int removes = 0;
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
        if (state_->failReads) {
            return {Status::Unavailable, QString(),
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
        if (state_->failWrites) {
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

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
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
    void credentialClassification_data();
    void credentialClassification();
    void keychainStatusMapping_data();
    void keychainStatusMapping();
    void linuxKeychainRuntimeStatusReport_data();
    void linuxKeychainRuntimeStatusReport();
    void bundledLinuxRuntimePathRequiresContainedRegularFile();
    void keychainJobsDisablePlaintextFallback();
    void platformStoreRoundTripsOrFailsClosed();
    void plaintextMigratesToVault();
    void vaultValueWinsAndPlaintextIsRemoved();
    void writesAndDeletesNeverTouchIni();
    void failedMigrationIsRetriedWithoutCredentialLoss();
    void failedNewCredentialWriteIsMemoryOnly();
    void checkedCredentialWriteReportsPersistence();
    void failedReplacementPreservesLegacyCredential();
    void failedDeleteIsRetriedWithoutCredentialResurrection();
    void persistedCacheScrubsDuplicatePlaintext();
    void unpersistedCachePreservesDuplicatePlaintext();
    void negativeCacheDoesNotHideLegacyCredential();
    void emptyPlaintextDoesNotCacheTransientVaultFailure();
    void transientReadFailureIsRetried();
    void scopesAreIsolated();
    void scopeIdentifiersAreStableAndValidated();
    void scopeCreationFailsClosedWhenItCannotPersist();
    void migratePlaintextCoversConfiguredCredentials();
    void gsettingsRoutesCredentialsToVault();
    void gsettingsCheckedCredentialWriteReportsPersistence();
    void gsettingsSyncsOnlyRequestedAthleteFile();
    void credentialMetadataDoesNotSuppressSystemMigration();
    void markerlessEstablishedSettingsAreAdoptedWithoutBackfill();
    void partialSystemMigrationResumesWithoutOverwrite();
    void partialGlobalMigrationResumesWithoutOverwrite_data();
    void partialGlobalMigrationResumesWithoutOverwrite();
    void partialAthleteMigrationResumesWithoutOverwrite_data();
    void partialAthleteMigrationResumesWithoutOverwrite();
    void migrationSyncFailuresResumeAfterRestart_data();
    void migrationSyncFailuresResumeAfterRestart();
    void newFormatMigrationScrubsLegacyCredential();
    void newFormatFailedMigrationIsRetriedWithoutCredentialLoss();
    void preInitializationMigrationKeepsAthleteScope();
    void postInitializationFallbackKeepsAthleteScope();
};

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

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_REFRESH_TOKEN,
                 plainKey(GC_STRAVA_REFRESH_TOKEN), QStringLiteral("default")),
             QVariant(sentinel));
    QCOMPARE(state->values.value(CredentialSettings::vaultKey(
                 scope, GC_STRAVA_REFRESH_TOKEN)), sentinel);
    QVERIFY(!ini.contains(plainKey(GC_STRAVA_REFRESH_TOKEN)));
    QCOMPARE(ini.value(QStringLiteral("normal/value")).toString(),
             QStringLiteral("keep"));
    ini.sync();
    QVERIFY(!fileContents(path).contains(sentinel.toUtf8()));
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
    ini.setValue(plainKey(GC_STRAVA_TOKEN),
                 QStringLiteral("stale-plaintext"));
    ini.sync();

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(
        CredentialSettings::vaultKey(scope, GC_STRAVA_TOKEN),
        QStringLiteral("vault-value"));
    CredentialSettings credentials(fakeStore(state));

    QCOMPARE(credentials.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("default")),
             QVariant(QStringLiteral("vault-value")));
    QVERIFY(!ini.contains(plainKey(GC_STRAVA_TOKEN)));
    ini.sync();
    QVERIFY(!fileContents(path).contains("stale-plaintext"));
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

void TestCredentialSettings::failedDeleteIsRetriedWithoutCredentialResurrection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("private.ini"));
    QSettings ini(path, QSettings::IniFormat);
    const QString scope = CredentialSettings::ensureScopeId(
        &ini, QStringLiteral("credential_store/id"));
    const QString vaultKey = CredentialSettings::vaultKey(
        scope, GC_STRAVA_TOKEN);

    auto state = std::make_shared<FakeStoreState>();
    state->values.insert(vaultKey, QStringLiteral("removed-secret"));
    state->failRemoves = true;
    {
        CredentialSettings currentSession(fakeStore(state));
        currentSession.remove(
            &ini, scope, GC_STRAVA_TOKEN,
            plainKey(GC_STRAVA_TOKEN));
    }
    QCOMPARE(state->removes, 1);
    QVERIFY(state->values.contains(vaultKey));

    state->failRemoves = false;
    CredentialSettings nextSession(fakeStore(state));
    QCOMPARE(nextSession.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QCOMPARE(state->removes, 2);
    QVERIFY(!state->values.contains(vaultKey));

    CredentialSettings persistedSession(fakeStore(state));
    QCOMPARE(persistedSession.value(
                 &ini, scope, GC_STRAVA_TOKEN,
                 plainKey(GC_STRAVA_TOKEN), QStringLiteral("missing")),
             QVariant(QStringLiteral("missing")));
    QVERIFY(!fileContents(path).contains("removed-secret"));
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
    QVERIFY(settings.status() != QSettings::NoError);
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
    QString systemPath;
    {
        QSettings legacy(organization, application);
        legacy.setValue(migratedKey, migratedValue);
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
            settings.value(
                nullptr,
                GC_SETTINGS_LAST_IMPORT_PATH).toString(),
            migratedValue);
    }

    QSettings migrated(systemPath, QSettings::IniFormat);
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
    const int targetTrain = 7;
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
