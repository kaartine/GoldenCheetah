#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>
#include <utility>

#include "Core/CredentialSettings.h"
#include "Core/CredentialStoreQtKeychain.h"
#include "Core/Settings.h"

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

class TestCredentialSettings : public QObject
{
    Q_OBJECT

private slots:
    void credentialClassification_data();
    void credentialClassification();
    void keychainStatusMapping_data();
    void keychainStatusMapping();
    void keychainJobsDisablePlaintextFallback();
    void platformStoreRoundTripsOrFailsClosed();
    void plaintextMigratesToVault();
    void vaultValueWinsAndPlaintextIsRemoved();
    void writesAndDeletesNeverTouchIni();
    void failedMigrationIsRetriedWithoutCredentialLoss();
    void failedNewCredentialWriteIsMemoryOnly();
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
