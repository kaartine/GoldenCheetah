/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>

#include "Cloud/StravaClientCredentials.h"
#include "Cloud/StravaRevocationClient.h"

namespace {

const QString RuntimeClientId = QStringLiteral("123456");
const QString RuntimeClientSecret = QStringLiteral(
    "runtime-secret-sec013");
const QString FallbackClientId = QStringLiteral("654321");
const QString FallbackClientSecret = QStringLiteral(
    "private-build-fallback-secret");

class FakeVault final : public StravaClientCredentials::Vault
{
public:
    StravaClientCredentials::VaultReadResult read() override
    {
        ++reads;
        return {status, value};
    }

    bool write(const QString &newValue) override
    {
        ++writes;
        if (failWrites) return false;
        status = StravaClientCredentials::VaultStatus::Present;
        value = newValue;
        return true;
    }

    bool remove() override
    {
        ++removes;
        if (failRemoves) return false;
        status = StravaClientCredentials::VaultStatus::NotFound;
        value.clear();
        return true;
    }

    StravaClientCredentials::VaultStatus status =
        StravaClientCredentials::VaultStatus::NotFound;
    QString value;
    bool failWrites = false;
    bool failRemoves = false;
    int reads = 0;
    int writes = 0;
    int removes = 0;
};

QString sourceFixture(const char *relativePath)
{
    return QDir(QStringLiteral(GC_TEST_SOURCE_ROOT)).filePath(
        QString::fromLatin1(relativePath));
}

QByteArray readSource(const QString &path)
{
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) return {};
    return source.readAll();
}

QString withoutWhitespace(const QByteArray &source)
{
    QString compact = QString::fromUtf8(source);
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    return compact;
}

QStringList *capturedMessages = nullptr;

void captureMessage(
    QtMsgType,
    const QMessageLogContext &,
    const QString &message)
{
    if (capturedMessages) capturedMessages->append(message);
}

} // namespace

class TestStravaClientCredentials : public QObject
{
    Q_OBJECT

private slots:
    void runtimeCredentialsTakePrecedence();
    void compileTimeFallbackRequiresAuthoritativeVaultMiss();
    void missingAndIncompleteCredentialsFailClosed();
    void vaultFailureRejectsCompileTimeFallback();
    void credentialsCanBeRemoved();
    void removalFailureRetainsCredential();
    void blankInputCannotDeleteInvalidRecord();
    void failuresAndDiagnosticsDoNotLeakSecrets();
    void debugFailureLogRedactsSensitiveValues();
    void runtimeSecretsAreRedactedForEveryOAuthRequest();
    void wizardRequiresExplicitCredentialRemoval();
    void oauthCallSitesUseTheRuntimeResolver();
};

void TestStravaClientCredentials::runtimeCredentialsTakePrecedence()
{
    FakeVault vault;
    const StravaClientCredentials::MutationResult stored =
        StravaClientCredentials::store(
            vault, RuntimeClientId, RuntimeClientSecret);
    QVERIFY2(stored.succeeded, qPrintable(stored.error));

    const StravaClientCredentials::Resolution resolved =
        StravaClientCredentials::resolve(
            vault, FallbackClientId, FallbackClientSecret);
    QCOMPARE(resolved.status,
             StravaClientCredentials::Status::Available);
    QCOMPARE(resolved.credentials.source,
             StravaClientCredentials::Source::RuntimeVault);
    QCOMPARE(resolved.credentials.clientId, RuntimeClientId);
    QCOMPARE(resolved.credentials.clientSecret,
             RuntimeClientSecret);
}

void TestStravaClientCredentials::
compileTimeFallbackRequiresAuthoritativeVaultMiss()
{
    FakeVault vault;
    const StravaClientCredentials::Resolution resolved =
        StravaClientCredentials::resolve(
            vault, FallbackClientId, FallbackClientSecret);

    QCOMPARE(resolved.status,
             StravaClientCredentials::Status::Available);
    QCOMPARE(resolved.credentials.source,
             StravaClientCredentials::Source::CompileTimeFallback);
    QCOMPARE(resolved.credentials.clientId, FallbackClientId);
    QCOMPARE(resolved.credentials.clientSecret,
             FallbackClientSecret);
}

void TestStravaClientCredentials::missingAndIncompleteCredentialsFailClosed()
{
    FakeVault missing;
    const StravaClientCredentials::Resolution absent =
        StravaClientCredentials::resolve(
            missing, QString(), QString());
    QCOMPARE(absent.status,
             StravaClientCredentials::Status::Missing);
    QVERIFY(!absent.isAvailable());

    FakeVault incomplete;
    incomplete.status =
        StravaClientCredentials::VaultStatus::Present;
    incomplete.value = QStringLiteral(
        R"({"client_id":"123456","version":1})");
    const StravaClientCredentials::Resolution rejected =
        StravaClientCredentials::resolve(
            incomplete, FallbackClientId, FallbackClientSecret);
    QCOMPARE(rejected.status,
             StravaClientCredentials::Status::InvalidRuntimeCredentials);
    QVERIFY(!rejected.isAvailable());
    QVERIFY(rejected.credentials.clientId.isEmpty());
    QVERIFY(rejected.credentials.clientSecret.isEmpty());
}

void TestStravaClientCredentials::vaultFailureRejectsCompileTimeFallback()
{
    FakeVault vault;
    vault.status =
        StravaClientCredentials::VaultStatus::Unavailable;

    const StravaClientCredentials::Resolution resolved =
        StravaClientCredentials::resolve(
            vault, FallbackClientId, FallbackClientSecret);
    QCOMPARE(resolved.status,
             StravaClientCredentials::Status::VaultUnavailable);
    QVERIFY(!resolved.isAvailable());
    QVERIFY(resolved.credentials.clientId.isEmpty());
    QVERIFY(resolved.credentials.clientSecret.isEmpty());
}

void TestStravaClientCredentials::credentialsCanBeRemoved()
{
    FakeVault vault;
    QVERIFY(StravaClientCredentials::store(
        vault, RuntimeClientId, RuntimeClientSecret).succeeded);

    const StravaClientCredentials::MutationResult removed =
        StravaClientCredentials::clear(vault);
    QVERIFY2(removed.succeeded, qPrintable(removed.error));
    QCOMPARE(vault.removes, 1);
    QCOMPARE(vault.status,
             StravaClientCredentials::VaultStatus::NotFound);
    QVERIFY(vault.value.isEmpty());
}

void TestStravaClientCredentials::removalFailureRetainsCredential()
{
    FakeVault vault;
    QVERIFY(StravaClientCredentials::store(
        vault, RuntimeClientId, RuntimeClientSecret).succeeded);
    const QString storedValue = vault.value;
    vault.failRemoves = true;

    const StravaClientCredentials::MutationResult removed =
        StravaClientCredentials::clear(vault);
    QVERIFY(!removed.succeeded);
    QCOMPARE(vault.status,
             StravaClientCredentials::VaultStatus::Present);
    QCOMPARE(vault.value, storedValue);
}

void TestStravaClientCredentials::blankInputCannotDeleteInvalidRecord()
{
    FakeVault vault;
    vault.status = StravaClientCredentials::VaultStatus::Present;
    vault.value = QStringLiteral(
        R"({"client_id":"123456","version":1})");

    const StravaClientCredentials::MutationResult unchanged =
        StravaClientCredentials::applyUserEdit(
            vault,
            QString(),
            QString(),
            StravaClientCredentials::UserAction::SaveIfProvided);
    QVERIFY(!unchanged.succeeded);
    QCOMPARE(vault.writes, 0);
    QCOMPARE(vault.removes, 0);
    QCOMPARE(vault.status,
             StravaClientCredentials::VaultStatus::Present);
    QCOMPARE(
        StravaClientCredentials::resolve(
            vault, FallbackClientId, FallbackClientSecret).status,
        StravaClientCredentials::Status::InvalidRuntimeCredentials);

    const StravaClientCredentials::MutationResult removed =
        StravaClientCredentials::applyUserEdit(
            vault,
            QString(),
            QString(),
            StravaClientCredentials::UserAction::Remove);
    QVERIFY2(removed.succeeded, qPrintable(removed.error));
    QCOMPARE(vault.removes, 1);
    QCOMPARE(vault.status,
             StravaClientCredentials::VaultStatus::NotFound);
}

void TestStravaClientCredentials::
failuresAndDiagnosticsDoNotLeakSecrets()
{
    FakeVault vault;
    vault.failWrites = true;
    QStringList messages;
    capturedMessages = &messages;
    const QtMessageHandler previous =
        qInstallMessageHandler(captureMessage);
    const StravaClientCredentials::MutationResult result =
        StravaClientCredentials::store(
            vault, RuntimeClientId, RuntimeClientSecret);
    qInstallMessageHandler(previous);
    capturedMessages = nullptr;

    QVERIFY(!result.succeeded);
    QVERIFY(!result.error.contains(RuntimeClientSecret));
    QVERIFY(!result.error.contains(RuntimeClientId));
    for (const QString &message : messages) {
        QVERIFY(!message.contains(RuntimeClientSecret));
        QVERIFY(!message.contains(RuntimeClientId));
    }
}

void TestStravaClientCredentials::
debugFailureLogRedactsSensitiveValues()
{
    QStringList messages;
    capturedMessages = &messages;
    const QtMessageHandler previous =
        qInstallMessageHandler(captureMessage);
    const QString failure =
        StravaOAuthPolicy::tokenFailureMessage(
            0,
            QNetworkReply::UnknownNetworkError,
            QStringLiteral("transport %1")
                .arg(RuntimeClientSecret),
            QStringLiteral(R"({"message":"%1"})")
                .arg(RuntimeClientSecret).toUtf8(),
            {RuntimeClientSecret});
    qInstallMessageHandler(previous);
    capturedMessages = nullptr;

    QVERIFY(!messages.isEmpty());
    QVERIFY(!failure.contains(RuntimeClientSecret));
    for (const QString &message : messages) {
        QVERIFY(!message.contains(RuntimeClientSecret));
    }
}

void TestStravaClientCredentials::
runtimeSecretsAreRedactedForEveryOAuthRequest()
{
    FakeVault vault;
    QVERIFY(StravaClientCredentials::store(
        vault, RuntimeClientId, RuntimeClientSecret).succeeded);
    const StravaClientCredentials::Resolution resolved =
        StravaClientCredentials::resolve(
            vault, FallbackClientId, FallbackClientSecret);
    QVERIFY(resolved.isAvailable());
    const QString authorizationCode = QStringLiteral(
        "runtime-authorization-code");
    const QString refreshToken = QStringLiteral(
        "runtime-refresh-token");

    const auto authorizationRequest =
        StravaOAuthPolicy::authorizationCodeRequest(
            resolved.credentials.clientId,
            resolved.credentials.clientSecret,
            authorizationCode);
    const auto refreshRequest =
        StravaOAuthPolicy::refreshTokenRequest(
            resolved.credentials.clientId,
            resolved.credentials.clientSecret,
            refreshToken);
    QVERIFY(authorizationRequest.isValid());
    QVERIFY(refreshRequest.isValid());
    for (const QString &grant : {
             authorizationCode, refreshToken}) {
        const QString failure =
            StravaOAuthPolicy::tokenFailureMessage(
                401,
                QNetworkReply::AuthenticationRequiredError,
                QStringLiteral("rejected %1 %2")
                    .arg(RuntimeClientSecret, grant),
                QStringLiteral(
                    R"({"message":"%1 %2"})")
                    .arg(RuntimeClientSecret, grant)
                    .toUtf8(),
                {RuntimeClientSecret, grant});
        QVERIFY(!failure.contains(RuntimeClientSecret));
        QVERIFY(!failure.contains(grant));
    }

    StravaRevocationClient revocation(
        [](const StravaOAuthPolicy::RevocationRequest &request,
           qsizetype,
           const StravaRevocationClient::CancellationCheck &) {
            StravaNetworkReply::Result response;
            response.httpStatus = 401;
            response.networkError =
                QNetworkReply::AuthenticationRequiredError;
            response.networkErrorString = QStringLiteral(
                "rejected %1 %2")
                    .arg(
                        RuntimeClientSecret,
                        QString::fromLatin1(
                            request.authorizationHeader));
            response.payload = QStringLiteral(
                R"({"message":"%1"})")
                .arg(RuntimeClientSecret).toUtf8();
            return response;
        });
    const StravaRevocationClient::Result revoked =
        revocation.revoke(
            resolved.credentials.clientId,
            resolved.credentials.clientSecret,
            refreshToken,
            StravaOAuthPolicy::RevocationTokenType::RefreshToken);
    QVERIFY(!revoked.isSuccess());
    QVERIFY(!revoked.error.contains(RuntimeClientSecret));
    QVERIFY(!revoked.error.contains(refreshToken));
    QVERIFY(!revoked.error.contains(QStringLiteral("Basic ")));
}

void TestStravaClientCredentials::
wizardRequiresExplicitCredentialRemoval()
{
    const QString wizardPath = sourceFixture(
        "src/Cloud/AddCloudWizard.cpp");
    QVERIFY2(!wizardPath.isEmpty(),
             "Cannot locate AddCloudWizard.cpp test fixture");
    const QString wizard = withoutWhitespace(
        readSource(wizardPath));
    QVERIFY(!wizard.isEmpty());
    QVERIFY(wizard.contains(
        QStringLiteral("removeStravaClientCredentials")));
    QVERIFY(wizard.contains(
        QStringLiteral("applyUserEditForAccount(")));
    QVERIFY(!wizard.contains(QStringLiteral(
        "if(clientId.isEmpty()&&clientSecret.isEmpty()){"
        "if(!stravaCredentialRecordPresent)returntrue;"
        "result=StravaClientCredentials::clearForAccount(")));
}

void TestStravaClientCredentials::oauthCallSitesUseTheRuntimeResolver()
{
    const QString dialogPath = sourceFixture(
        "src/Cloud/OAuthDialog.cpp");
    const QString servicePath = sourceFixture(
        "src/Cloud/Strava.cpp");
    const QString mainPath = sourceFixture(
        "src/Core/main.cpp");
    const QString adapterPath = sourceFixture(
        "src/Cloud/StravaClientCredentialsSettings.cpp");
    const QString revocationPath = sourceFixture(
        "src/Cloud/StravaRevocationClient.cpp");
    QVERIFY2(!dialogPath.isEmpty(),
             "Cannot locate OAuthDialog.cpp test fixture");
    QVERIFY2(!servicePath.isEmpty(),
             "Cannot locate Strava.cpp test fixture");
    QVERIFY2(!mainPath.isEmpty(),
             "Cannot locate main.cpp test fixture");
    QVERIFY2(!adapterPath.isEmpty(),
             "Cannot locate credential adapter test fixture");
    QVERIFY2(!revocationPath.isEmpty(),
             "Cannot locate revocation client test fixture");
    const QByteArray dialog = readSource(dialogPath);
    const QByteArray service = readSource(servicePath);
    const QByteArray main = readSource(mainPath);
    const QByteArray adapter = readSource(adapterPath);
    const QByteArray revocation = readSource(revocationPath);
    QVERIFY(!dialog.isEmpty());
    QVERIFY(!service.isEmpty());
    QVERIFY(!main.isEmpty());
    QVERIFY(!adapter.isEmpty());
    QVERIFY(!revocation.isEmpty());

    QVERIFY(dialog.contains(
        "StravaClientCredentials::resolveForAccount("));
    QVERIFY(service.contains(
        "StravaClientCredentials::resolveForAccount("));
    QVERIFY(withoutWhitespace(main).contains(
        QStringLiteral(
            "StravaClientCredentials::compileTimeFallbackIsConfigured()")));
    QVERIFY(!dialog.contains("GC_STRAVA_CLIENT_SECRET"));
    QVERIFY(!service.contains("GC_STRAVA_CLIENT_SECRET"));
    QVERIFY(!main.contains("GC_STRAVA_CLIENT_SECRET"));
    QVERIFY(adapter.contains("GC_STRAVA_CLIENT_SECRET"));
    QVERIFY(withoutWhitespace(dialog).contains(
        QStringLiteral(
            "tokenRequestSensitiveValues={stravaClientSecret,code}")));
    QVERIFY(withoutWhitespace(service).contains(
        QStringLiteral(
            "{clientCredentials.credentials.clientSecret,effectiveRefreshToken}")));
    QVERIFY(!withoutWhitespace(service).contains(
        QStringLiteral(
            "printd(\"Goterror%s\\n\",tokenReply->errorString()")));
    QVERIFY(withoutWhitespace(revocation).contains(
        QStringLiteral(
            "return{token,clientSecret,basicCredentials,basicPayload,")));
}

QTEST_GUILESS_MAIN(TestStravaClientCredentials)
#include "testStravaClientCredentials.moc"
