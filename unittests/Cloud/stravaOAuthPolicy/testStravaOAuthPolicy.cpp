#include "Cloud/StravaOAuthPolicy.h"
#include "Cloud/OAuthDialog.h"
#include "Cloud/OAuthDialogMessageGuard.h"

#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

const QString ClientId = QStringLiteral("83");
const QString ClientSecret = QStringLiteral("synthetic-client-secret");
const QString AccessToken =
    QStringLiteral("synthetic-access-token");
const QString RefreshToken =
    QStringLiteral("synthetic-refresh-token");

class OAuthTestReply final : public QNetworkReply
{
public:
    OAuthTestReply(
        QByteArray payload,
        QNetworkReply::NetworkError error,
        QObject *parent)
        : QNetworkReply(parent), payload_(std::move(payload))
    {
        setRequest(QNetworkRequest(
            QUrl(QStringLiteral("https://www.strava.com/oauth/token"))));
        setUrl(request().url());
        setAttribute(
            QNetworkRequest::HttpStatusCodeAttribute,
            error == QNetworkReply::NoError ? 200 : 503);
        if (error != QNetworkReply::NoError)
            setError(error, QStringLiteral("Synthetic network failure"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void abort() override {}
    qint64 bytesAvailable() const override
    {
        return payload_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        if (offset_ >= payload_.size()) return -1;
        const qint64 available = payload_.size() - offset_;
        const qint64 count = std::min(maximum, available);
        std::memcpy(data, payload_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    QByteArray payload_;
    qint64 offset_ = 0;
};

QByteArray authorizationPayload(const QJsonValue &scope)
{
    QJsonObject object{
        {QStringLiteral("access_token"), AccessToken},
        {QStringLiteral("refresh_token"), RefreshToken}
    };
    if (!scope.isUndefined()) {
        object.insert(QStringLiteral("scope"), scope);
    }
    return QJsonDocument(object).toJson(
        QJsonDocument::Compact);
}

QByteArray sourceContents(const char *relativePath)
{
    const QString path = QDir(QStringLiteral(GC_TEST_SOURCE_ROOT)).filePath(
        QString::fromLatin1(relativePath));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QByteArray sourceSection(
    const QByteArray &source,
    const QByteArray &startMarker,
    const QByteArray &endMarker)
{
    const qsizetype start = source.indexOf(startMarker);
    if (start < 0) return {};

    const qsizetype end = source.indexOf(
        endMarker, start + startMarker.size());
    if (end < 0) return {};
    return source.mid(start, end - start);
}

QUrlQuery formBody(const StravaOAuthPolicy::TokenRequest &request)
{
    return QUrlQuery(QString::fromUtf8(request.body));
}

QUrlQuery formBody(
    const StravaOAuthPolicy::RevocationRequest &request)
{
    return QUrlQuery(QString::fromUtf8(request.body));
}

void verifyOnlyKeys(const QUrlQuery &query,
                    const QStringList &expected)
{
    QStringList actual;
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        actual.append(item.first);
    }
    actual.sort();

    QStringList sortedExpected = expected;
    sortedExpected.sort();
    QCOMPARE(actual, sortedExpected);
}

} // namespace

class TestStravaOAuthPolicy : public QObject
{
    Q_OBJECT

private slots:
    void rejectsUnavailableCredentials_data();
    void rejectsUnavailableCredentials();
    void acceptsConfiguredCredentials();
    void reportsMachineReadableBuildStatus_data();
    void reportsMachineReadableBuildStatus();
    void mainExposesCredentialFreeBuildStatus();
    void buildsAuthorizationCodeRequest();
    void encodesAuthorizationCodeRequestValues();
    void rejectsInvalidAuthorizationCodeRequest_data();
    void rejectsInvalidAuthorizationCodeRequest();
    void buildsRefreshTokenRequest();
    void rejectsInvalidRefreshTokenRequest_data();
    void rejectsInvalidRefreshTokenRequest();
    void buildsRefreshTokenRevocationRequest();
    void encodesRevocationRequestValues();
    void rejectsInvalidRevocationRequest_data();
    void rejectsInvalidRevocationRequest();
    void reportsBoundedRedactedRevocationFailures();
    void reportsHttpStatusInsteadOfQtEnum();
    void redactsSensitiveProviderText();
    void redactsSensitiveTextAcrossInputBoundary();
    void handlesMalformedProviderResponse();
    void reportsTransportFailureWithoutHttpStatus();
    void boundsProviderErrorOutput();
    void parsesSuccessfulTokenResponse();
    void rejectsInvalidTokenResponse_data();
    void rejectsInvalidTokenResponse();
    void parsesGrantedAuthorizationScopes();
    void rejectsInvalidAuthorizationScopes_data();
    void rejectsInvalidAuthorizationScopes();
    void serviceOpenUsesCoordinatedDurableRefresh();
    void guardedFailureMessageSurvivesDialogDestruction_data();
    void guardedFailureMessageSurvivesDialogDestruction();
    void guardedSuccessMessageAcceptsLiveDialog();
    void guardedSuccessMessageSurvivesDialogDestruction();
    void detachedConstructorMessageSurvivesMessageDeletion();
    void storageReconciliationIsCachedOnlyAfterAdoption();
    void oauthGrantSupersedesRefreshAndPublishesDurably();
    void serviceDisconnectRevokesBeforeCredentialRemoval();
    void serviceWriteFileDisconnectCompilesWithoutErrorParameter();
    void credentialsPageOffersExplicitStravaDisconnectModes();
};

void TestStravaOAuthPolicy::
serviceWriteFileDisconnectCompilesWithoutErrorParameter()
{
    const QByteArray service = sourceContents(
        "src/Cloud/Strava.cpp");
    const QByteArray writeFile = sourceSection(
        service,
        "Strava::writeFile(QByteArray &data",
        "Strava::writeFileCompleted()");

    QVERIFY(!writeFile.isEmpty());
    QVERIFY(writeFile.contains(
        "if (!reconcileSharedAuthorizationStatus())"));
    QVERIFY(writeFile.contains("return false;"));
    QVERIFY(!writeFile.contains("error ="));
}

void TestStravaOAuthPolicy::
guardedFailureMessageSurvivesDialogDestruction_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<int>("networkError");

    QTest::newRow("token-network-failure")
        << QByteArrayLiteral("provider failure")
        << int(QNetworkReply::ServiceUnavailableError);
    QTest::newRow("malformed-token-json")
        << QByteArrayLiteral("not-json")
        << int(QNetworkReply::NoError);
    QTest::newRow("invalid-strava-token-response")
        << QByteArrayLiteral("{}")
        << int(QNetworkReply::NoError);
}

void TestStravaOAuthPolicy::
guardedFailureMessageSurvivesDialogDestruction()
{
    QFETCH(QByteArray, payload);
    QFETCH(int, networkError);

    QPointer<OAuthDialog> dialog = new OAuthDialog(
        OAuthDialog::STRAVA,
        OAuthDialog::TestConstruction{});
    bool rejected = false;
    QObject::connect(
        dialog, &QDialog::rejected,
        this, [&rejected] { rejected = true; });
    QPointer<QNetworkReply> reply = new OAuthTestReply(
        payload,
        static_cast<QNetworkReply::NetworkError>(networkError),
        dialog);
    QVERIFY(dialog->trackTokenReplyForTest(reply));
    dialog->finishTokenReplyForTest(reply);
    QPointer<QMessageBox> guardedMessage =
        dialog->findChild<QMessageBox *>();
    QVERIFY(guardedMessage);
    delete dialog.data();

    QVERIFY(dialog.isNull());
    QVERIFY(guardedMessage.isNull());
    QVERIFY(!rejected);
}

void TestStravaOAuthPolicy::guardedSuccessMessageAcceptsLiveDialog()
{
    QPointer<OAuthDialog> dialog = new OAuthDialog(
        OAuthDialog::NOLIO,
        OAuthDialog::TestConstruction{});
    bool accepted = false;
    QObject::connect(
        dialog, &QDialog::accepted,
        this, [&accepted] { accepted = true; });
    const QByteArray payload = QJsonDocument(QJsonObject{
        {QStringLiteral("access_token"), AccessToken},
        {QStringLiteral("refresh_token"), RefreshToken}
    }).toJson(QJsonDocument::Compact);
    QPointer<QNetworkReply> reply = new OAuthTestReply(
        payload, QNetworkReply::NoError, dialog);

    QVERIFY(dialog->trackTokenReplyForTest(reply));
    dialog->finishTokenReplyForTest(reply);
    QPointer<QMessageBox> guardedMessage =
        dialog->findChild<QMessageBox *>();
    QVERIFY(guardedMessage);
    guardedMessage->done(QMessageBox::Ok);
    QVERIFY(accepted);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.isNull(), 1000);
    QVERIFY(guardedMessage.isNull());
    QVERIFY(reply.isNull());
}

void TestStravaOAuthPolicy::
guardedSuccessMessageSurvivesDialogDestruction()
{
    QPointer<QDialog> dialog = new QDialog;
    bool accepted = false;
    QObject::connect(
        dialog, &QDialog::accepted,
        this, [&accepted] { accepted = true; });
    QPointer<QMessageBox> guardedMessage;

    const bool survived = OAuthDialogMessageGuard::showAndAccept(
        dialog,
        QStringLiteral("Information"),
        QStringLiteral("Synthetic success"),
        [&dialog, &guardedMessage](QMessageBox &message) {
            guardedMessage = &message;
            delete dialog.data();
        });

    QVERIFY(!survived);
    QVERIFY(dialog.isNull());
    QVERIFY(guardedMessage.isNull());
    QVERIFY(!accepted);
}

void TestStravaOAuthPolicy::
detachedConstructorMessageSurvivesMessageDeletion()
{
    const bool survived = OAuthDialogMessageGuard::showDetached(
        QStringLiteral("Authorization Error"),
        QStringLiteral("Synthetic constructor failure"),
        QString(),
        [](QMessageBox &message) {
            delete &message;
        });
    QVERIFY(!survived);
}

void TestStravaOAuthPolicy::
storageReconciliationIsCachedOnlyAfterAdoption()
{
    const QByteArray source = sourceContents(
        "src/Cloud/Strava.cpp");
    const QByteArray initialization = sourceSection(
        source,
        "Strava::initializeSharedAuthorizationStatus() const",
        "Strava::reconcileSharedAuthorizationStatus() const");

    QVERIFY(!initialization.isEmpty());
    QVERIFY(initialization.contains("const bool reconciled ="));
    const qsizetype guard = initialization.indexOf("if (reconciled)");
    const qsizetype cachedRevision = initialization.indexOf(
        "sharedAuthorizationRevision = authorization.revision");
    const qsizetype initialized = initialization.indexOf(
        "sharedAuthorizationInitialized.store(");
    QVERIFY(guard >= 0);
    QVERIFY(cachedRevision > guard);
    QVERIFY(initialized > cachedRevision);
    QVERIFY(initialization.contains(
        "storageAuthoritative && reconciled"));

    const QByteArray reconciliation = sourceSection(
        source,
        "Strava::reconcileSharedAuthorizationStatus() const",
        "Strava::accountDisconnectOperation(");
    QVERIFY(!reconciliation.isEmpty());
    QVERIFY(reconciliation.contains(
        "reconcileAuthoritativeAuthorizationFromStorage("));
    QVERIFY(!reconciliation.contains(
        "stored.revision == sharedAuthorizationRevision"));
    QVERIFY(!reconciliation.contains(
        "stored.state == sharedAuthorizationState"));
}

void TestStravaOAuthPolicy::rejectsUnavailableCredentials_data()
{
    QTest::addColumn<QString>("clientId");
    QTest::addColumn<QString>("clientSecret");

    QTest::newRow("missing-id")
        << QString() << ClientSecret;
    QTest::newRow("non-numeric-id")
        << QStringLiteral("client-83") << ClientSecret;
    QTest::newRow("missing-secret")
        << ClientId << QString();
    QTest::newRow("blank-secret")
        << ClientId << QStringLiteral("   ");
    QTest::newRow("goldencheetah-placeholder")
        << ClientId
        << QStringLiteral("__GC_STRAVA_CLIENT_SECRET__");
    QTest::newRow("generic-build-placeholder")
        << ClientId << QStringLiteral("__MISSING_SECRET__");
    QTest::newRow("documented-config-placeholder")
        << ClientId << QStringLiteral("your_client_secret");
}

void TestStravaOAuthPolicy::rejectsUnavailableCredentials()
{
    QFETCH(QString, clientId);
    QFETCH(QString, clientSecret);

    QVERIFY(!StravaOAuthPolicy::hasUsableCredentials(
        clientId, clientSecret));
}

void TestStravaOAuthPolicy::acceptsConfiguredCredentials()
{
    QVERIFY(StravaOAuthPolicy::hasUsableCredentials(
        ClientId, ClientSecret));
}

void TestStravaOAuthPolicy::reportsMachineReadableBuildStatus_data()
{
    QTest::addColumn<bool>("compileTimeFallbackConfigured");
    QTest::addColumn<QByteArray>("fallbackStatus");

    QTest::newRow("configured")
        << true
        << QByteArray("configured");
    QTest::newRow("runtime-only")
        << false
        << QByteArray("unavailable");
}

void TestStravaOAuthPolicy::reportsMachineReadableBuildStatus()
{
    QFETCH(bool, compileTimeFallbackConfigured);
    QFETCH(QByteArray, fallbackStatus);

    const QByteArray expected =
        QByteArrayLiteral(
            "goldencheetah_build_status=1\n"
            "application=GoldenCheetah\n"
            "strava_support=enabled\n"
            "strava_oauth=runtime_credentials\n"
            "strava_compile_fallback=")
        + fallbackStatus + '\n';
    QCOMPARE(
        StravaOAuthPolicy::buildStatusReport(
            compileTimeFallbackConfigured),
        expected);
}

void TestStravaOAuthPolicy::mainExposesCredentialFreeBuildStatus()
{
    const QByteArray source = sourceContents(
        "src/Core/main.cpp");
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(
        "\"--goldencheetah-build-status\""));
    QVERIFY(source.contains(
        "StravaOAuthPolicy::buildStatusReport("));
    QVERIFY(source.contains(
        "compileTimeFallbackIsConfigured()"));
    QVERIFY(!source.contains("GC_STRAVA_CLIENT_ID"));
    QVERIFY(!source.contains("GC_STRAVA_CLIENT_SECRET"));
    const qsizetype shutdown = source.indexOf(
        "StravaSettingsCommit::shutdownCredentialThread()");
    const qsizetype settingsDeletion = source.indexOf(
        "delete appsettings", shutdown);
    QVERIFY(shutdown >= 0);
    QVERIFY(settingsDeletion > shutdown);

    const qsizetype eventLoop = source.indexOf(
        "ret=application->exec()");
    const qsizetype normalShutdown = source.indexOf(
        "StravaSettingsCommit::shutdownCredentialThread()",
        eventLoop);
    const qsizetype settingsClear = source.indexOf(
        "appsettings->clearGlobalAndAthletes(100)", eventLoop);
    const qsizetype workerRestart = source.indexOf(
        "StravaSettingsCommit::restartCredentialThread()",
        eventLoop);
    const qsizetype applicationDeletion = source.indexOf(
        "delete application", eventLoop);
    QVERIFY(eventLoop >= 0);
    QVERIFY(normalShutdown > eventLoop);
    QVERIFY(settingsClear > normalShutdown);
    QVERIFY(workerRestart > settingsClear);
    QVERIFY(applicationDeletion > settingsClear);
    QVERIFY(source.contains(
        "if (!credentialWorkerStopped)"));
    QVERIFY(source.contains("_Exit(ret)"));
}

void TestStravaOAuthPolicy::buildsAuthorizationCodeRequest()
{
    const auto request =
        StravaOAuthPolicy::authorizationCodeRequest(
            ClientId, ClientSecret,
            QStringLiteral("authorization-code"));

    QVERIFY2(request.isValid(), qPrintable(request.error));
    QCOMPARE(request.endpoint,
             QUrl(QStringLiteral(
                 "https://www.strava.com/oauth/token")));
    QVERIFY(!request.endpoint.hasQuery());

    const QUrlQuery query = formBody(request);
    verifyOnlyKeys(query,
                   {QStringLiteral("client_id"),
                    QStringLiteral("client_secret"),
                    QStringLiteral("code"),
                    QStringLiteral("grant_type")});
    QCOMPARE(query.queryItemValue(QStringLiteral("client_id")),
             ClientId);
    QCOMPARE(query.queryItemValue(QStringLiteral("client_secret")),
             ClientSecret);
    QCOMPARE(query.queryItemValue(QStringLiteral("code")),
             QStringLiteral("authorization-code"));
    QCOMPARE(query.queryItemValue(QStringLiteral("grant_type")),
             QStringLiteral("authorization_code"));
}

void TestStravaOAuthPolicy::encodesAuthorizationCodeRequestValues()
{
    const QString secret = QStringLiteral("secret+with&delimiters=");
    const QString code = QStringLiteral("code/+?&=");
    const auto request =
        StravaOAuthPolicy::authorizationCodeRequest(
            ClientId, secret, code);

    QVERIFY2(request.isValid(), qPrintable(request.error));
    QVERIFY(request.body.contains(
        "client_secret=secret%2Bwith%26delimiters%3D"));
    QVERIFY(request.body.contains(
        "code=code%2F%2B%3F%26%3D"));
    QVERIFY(!request.body.contains("secret+with"));
    const QUrlQuery query = formBody(request);
    QCOMPARE(query.queryItemValue(
                 QStringLiteral("client_secret"),
                 QUrl::FullyDecoded),
             secret);
    QCOMPARE(query.queryItemValue(
                 QStringLiteral("code"),
                 QUrl::FullyDecoded),
             code);
}

void TestStravaOAuthPolicy::rejectsInvalidAuthorizationCodeRequest_data()
{
    QTest::addColumn<QString>("clientId");
    QTest::addColumn<QString>("clientSecret");
    QTest::addColumn<QString>("code");

    QTest::newRow("placeholder-secret")
        << ClientId
        << QStringLiteral("__GC_STRAVA_CLIENT_SECRET__")
        << QStringLiteral("code");
    QTest::newRow("missing-code")
        << ClientId << ClientSecret << QString();
    QTest::newRow("blank-code")
        << ClientId << ClientSecret << QStringLiteral("  ");
}

void TestStravaOAuthPolicy::rejectsInvalidAuthorizationCodeRequest()
{
    QFETCH(QString, clientId);
    QFETCH(QString, clientSecret);
    QFETCH(QString, code);

    const auto request =
        StravaOAuthPolicy::authorizationCodeRequest(
            clientId, clientSecret, code);

    QVERIFY(!request.isValid());
    QVERIFY(request.endpoint.isEmpty());
    QVERIFY(request.body.isEmpty());
    QVERIFY(!request.error.isEmpty());
}

void TestStravaOAuthPolicy::buildsRefreshTokenRequest()
{
    const auto request =
        StravaOAuthPolicy::refreshTokenRequest(
            ClientId, ClientSecret,
            QStringLiteral("refresh-token"));

    QVERIFY2(request.isValid(), qPrintable(request.error));
    QCOMPARE(request.endpoint,
             QUrl(QStringLiteral(
                 "https://www.strava.com/oauth/token")));
    const QUrlQuery query = formBody(request);
    verifyOnlyKeys(query,
                   {QStringLiteral("client_id"),
                    QStringLiteral("client_secret"),
                    QStringLiteral("grant_type"),
                    QStringLiteral("refresh_token")});
    QCOMPARE(query.queryItemValue(QStringLiteral("grant_type")),
             QStringLiteral("refresh_token"));
    QCOMPARE(query.queryItemValue(QStringLiteral("refresh_token")),
             QStringLiteral("refresh-token"));
}

void TestStravaOAuthPolicy::rejectsInvalidRefreshTokenRequest_data()
{
    QTest::addColumn<QString>("clientId");
    QTest::addColumn<QString>("clientSecret");
    QTest::addColumn<QString>("refreshToken");

    QTest::newRow("placeholder-secret")
        << ClientId
        << QStringLiteral("__GC_STRAVA_CLIENT_SECRET__")
        << QStringLiteral("refresh-token");
    QTest::newRow("missing-refresh-token")
        << ClientId << ClientSecret << QString();
    QTest::newRow("blank-refresh-token")
        << ClientId << ClientSecret << QStringLiteral("  ");
}

void TestStravaOAuthPolicy::rejectsInvalidRefreshTokenRequest()
{
    QFETCH(QString, clientId);
    QFETCH(QString, clientSecret);
    QFETCH(QString, refreshToken);

    const auto request =
        StravaOAuthPolicy::refreshTokenRequest(
            clientId, clientSecret, refreshToken);

    QVERIFY(!request.isValid());
    QVERIFY(request.endpoint.isEmpty());
    QVERIFY(request.body.isEmpty());
    QVERIFY(!request.error.isEmpty());
}

void TestStravaOAuthPolicy::buildsRefreshTokenRevocationRequest()
{
    const auto request =
        StravaOAuthPolicy::revocationRequest(
            ClientId,
            ClientSecret,
            RefreshToken,
            StravaOAuthPolicy::RevocationTokenType::RefreshToken);

    QVERIFY2(request.isValid(), qPrintable(request.error));
    QCOMPARE(
        request.endpoint,
        QUrl(QStringLiteral(
            "https://www.strava.com/oauth/revoke")));
    QVERIFY(!request.endpoint.hasQuery());
    QCOMPARE(
        request.authorizationHeader,
        QByteArrayLiteral("Basic ")
            + QStringLiteral("%1:%2")
                  .arg(ClientId, ClientSecret)
                  .toUtf8()
                  .toBase64());

    const QUrlQuery query = formBody(request);
    verifyOnlyKeys(
        query,
        {QStringLiteral("token"),
         QStringLiteral("token_type_hint")});
    QCOMPARE(
        query.queryItemValue(QStringLiteral("token")),
        RefreshToken);
    QCOMPARE(
        query.queryItemValue(QStringLiteral("token_type_hint")),
        QStringLiteral("refresh_token"));
    QVERIFY(!request.body.contains(ClientId.toUtf8()));
    QVERIFY(!request.body.contains(ClientSecret.toUtf8()));
}

void TestStravaOAuthPolicy::encodesRevocationRequestValues()
{
    const QString token =
        QStringLiteral("refresh/+?&= token");
    const auto request =
        StravaOAuthPolicy::revocationRequest(
            ClientId,
            ClientSecret,
            token,
            StravaOAuthPolicy::RevocationTokenType::RefreshToken);

    QVERIFY2(request.isValid(), qPrintable(request.error));
    QCOMPARE(
        formBody(request).queryItemValue(
            QStringLiteral("token"),
            QUrl::FullyDecoded),
        token);
    QVERIFY(request.body.contains(
        QByteArrayLiteral("refresh%2F%2B%3F%26%3D%20token")));
}

void TestStravaOAuthPolicy::rejectsInvalidRevocationRequest_data()
{
    QTest::addColumn<QString>("clientId");
    QTest::addColumn<QString>("clientSecret");
    QTest::addColumn<QString>("token");

    QTest::newRow("missing-client-id")
        << QString() << ClientSecret << RefreshToken;
    QTest::newRow("placeholder-secret")
        << ClientId
        << QStringLiteral("__GC_STRAVA_CLIENT_SECRET__")
        << RefreshToken;
    QTest::newRow("missing-token")
        << ClientId << ClientSecret << QString();
    QTest::newRow("control-character")
        << ClientId << ClientSecret
        << QStringLiteral("token\nheader");
}

void TestStravaOAuthPolicy::rejectsInvalidRevocationRequest()
{
    QFETCH(QString, clientId);
    QFETCH(QString, clientSecret);
    QFETCH(QString, token);

    const auto request =
        StravaOAuthPolicy::revocationRequest(
            clientId,
            clientSecret,
            token,
            StravaOAuthPolicy::RevocationTokenType::RefreshToken);

    QVERIFY(!request.isValid());
    QVERIFY(request.endpoint.isEmpty());
    QVERIFY(request.authorizationHeader.isEmpty());
    QVERIFY(request.body.isEmpty());
    QVERIFY(!request.error.isEmpty());
}

void TestStravaOAuthPolicy::
reportsBoundedRedactedRevocationFailures()
{
    const QString token =
        QStringLiteral("revocation-token-sentinel");
    const QString secret =
        QStringLiteral("revocation-secret-sentinel");
    const QByteArray payload = QStringLiteral(
        R"json({"message":"failed %1 %2"})json")
            .arg(token, secret)
            .toUtf8();

    const QString httpFailure =
        StravaOAuthPolicy::revocationFailureMessage(
            503,
            QNetworkReply::ServiceUnavailableError,
            QStringLiteral("provider rejected %1").arg(token),
            payload,
            {token, secret});
    QVERIFY(httpFailure.contains(QStringLiteral("HTTP 503")));
    QVERIFY(!httpFailure.contains(token));
    QVERIFY(!httpFailure.contains(secret));
    QVERIFY(httpFailure.size() <= 1024);

    const QString transportFailure =
        StravaOAuthPolicy::revocationFailureMessage(
            0,
            QNetworkReply::TimeoutError,
            QStringLiteral("timeout %1").arg(token),
            QByteArray(),
            {token, secret});
    QVERIFY(transportFailure.contains(
        QStringLiteral("Network error")));
    QVERIFY(!transportFailure.contains(token));
    QVERIFY(transportFailure.size() <= 1024);
}

void TestStravaOAuthPolicy::reportsHttpStatusInsteadOfQtEnum()
{
    const QByteArray payload = R"json({
        "message": "Authorization Error",
        "errors": [{
            "resource": "Application",
            "field": "client_secret",
            "code": "invalid"
        }]
    })json";

    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            401,
            QNetworkReply::AuthenticationRequiredError,
            QStringLiteral("Host requires authentication"),
            payload, {});

    QVERIFY(message.contains(QStringLiteral("HTTP 401")));
    QVERIFY(message.contains(QStringLiteral("Authorization Error")));
    QVERIFY(message.contains(
        QStringLiteral("Application.client_secret: invalid")));
    QVERIFY(!message.contains(QStringLiteral("HTTP 204")));
    QVERIFY(!message.contains(QStringLiteral("(204)")));
}

void TestStravaOAuthPolicy::redactsSensitiveProviderText()
{
    const QString secret = QStringLiteral("secret-sentinel-17");
    const QString code = QStringLiteral("authorization-code-sentinel-29");
    const QString token = QStringLiteral("token-sentinel-41");
    const QByteArray payload = QStringLiteral(
        "{\"message\":\"Rejected %1 %2\","
        "\"error_description\":\"Never echo %3\","
        "\"access_token\":\"%3\"}")
        .arg(secret, code, token)
        .toUtf8();

    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            401,
            QNetworkReply::AuthenticationRequiredError,
            QStringLiteral("Host requires authentication"),
            payload, {secret, code, token});

    QVERIFY(!message.contains(secret));
    QVERIFY(!message.contains(code));
    QVERIFY(!message.contains(token));
    QVERIFY(message.contains(QStringLiteral("[redacted]")));
}

void TestStravaOAuthPolicy::redactsSensitiveTextAcrossInputBoundary()
{
    const QString marker = QStringLiteral("LEAKME-");
    const QString secret = marker
        + QString(5000, QLatin1Char('S'));
    const QString reflected = QString(248, QLatin1Char('A'))
        + secret;
    const QByteArray payload = QJsonDocument(
        QJsonObject{{QStringLiteral("message"), reflected}})
        .toJson(QJsonDocument::Compact);

    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            401,
            QNetworkReply::AuthenticationRequiredError,
            QStringLiteral("Host requires authentication"),
            payload, {secret});

    QVERIFY(!message.contains(QStringLiteral("LEAKME")));
}

void TestStravaOAuthPolicy::handlesMalformedProviderResponse()
{
    const QByteArray malformed("not-json secret-sentinel");
    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            503,
            QNetworkReply::ServiceUnavailableError,
            QStringLiteral("Service unavailable"),
            malformed,
            {QStringLiteral("secret-sentinel")});

    QVERIFY(message.contains(QStringLiteral("HTTP 503")));
    QVERIFY(!message.contains(QString::fromUtf8(malformed)));
    QVERIFY(!message.contains(QStringLiteral("secret-sentinel")));
}

void TestStravaOAuthPolicy::reportsTransportFailureWithoutHttpStatus()
{
    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            0,
            QNetworkReply::TimeoutError,
            QStringLiteral("Connection timed out"),
            QByteArray(), {});

    QVERIFY(message.contains(QStringLiteral("Network error")));
    QVERIFY(message.contains(QStringLiteral("Connection timed out")));
    QVERIFY(!message.contains(QStringLiteral("HTTP")));
}

void TestStravaOAuthPolicy::boundsProviderErrorOutput()
{
    const QByteArray payload =
        QStringLiteral("{\"message\":\"%1\"}")
            .arg(QString(100000, QLatin1Char('A')))
            .toUtf8();
    const QString message =
        StravaOAuthPolicy::tokenFailureMessage(
            400,
            QNetworkReply::ProtocolInvalidOperationError,
            QStringLiteral("Protocol error"),
            payload, {});

    QVERIFY(message.size() <= 1024);
}

void TestStravaOAuthPolicy::parsesSuccessfulTokenResponse()
{
    const auto response =
        StravaOAuthPolicy::parseTokenResponse(
            QByteArrayLiteral(
                "{\"access_token\":\"synthetic-access-token\","
                "\"refresh_token\":\"synthetic-refresh-token\"}"));

    QVERIFY2(response.isValid(), qPrintable(response.error));
    QCOMPARE(response.accessToken,
             QStringLiteral("synthetic-access-token"));
    QCOMPARE(response.refreshToken,
             QStringLiteral("synthetic-refresh-token"));
}

void TestStravaOAuthPolicy::rejectsInvalidTokenResponse_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("malformed") << QByteArray("not-json");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("missing-access")
        << QByteArray(
            "{\"refresh_token\":\"synthetic-refresh-token\"}");
    QTest::newRow("missing-refresh")
        << QByteArray(
            "{\"access_token\":\"synthetic-access-token\"}");
    QTest::newRow("non-string-access")
        << QByteArray(
            "{\"access_token\":17,"
            "\"refresh_token\":\"synthetic-refresh-token\"}");
    QTest::newRow("oversized")
        << QByteArray(100000, 'A');
}

void TestStravaOAuthPolicy::rejectsInvalidTokenResponse()
{
    QFETCH(QByteArray, payload);

    const auto response =
        StravaOAuthPolicy::parseTokenResponse(payload);

    QVERIFY(!response.isValid());
    QVERIFY(response.accessToken.isEmpty());
    QVERIFY(response.refreshToken.isEmpty());
    QVERIFY(!response.error.isEmpty());
    QVERIFY(!response.error.contains(
        QStringLiteral("synthetic-access-token")));
    QVERIFY(!response.error.contains(
        QStringLiteral("synthetic-refresh-token")));
}

void TestStravaOAuthPolicy::parsesGrantedAuthorizationScopes()
{
    const auto response =
        StravaOAuthPolicy::parseAuthorizationResponse(
            authorizationPayload(QStringLiteral(
                "activity:write future:read read_all "
                "activity:read_all read_all")));

    QVERIFY2(response.isValid(), qPrintable(response.error));
    QCOMPARE(response.accessToken, AccessToken);
    QCOMPARE(response.refreshToken, RefreshToken);
    QCOMPARE(
        response.grantedScopes,
        QStringList({
            QStringLiteral("activity:read_all"),
            QStringLiteral("activity:write"),
            QStringLiteral("future:read"),
            QStringLiteral("read_all")
        }));
}

void TestStravaOAuthPolicy::
rejectsInvalidAuthorizationScopes_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("missing")
        << authorizationPayload(QJsonValue::Undefined);
    QTest::newRow("null")
        << authorizationPayload(QJsonValue::Null);
    QTest::newRow("array")
        << authorizationPayload(QJsonArray{
               QStringLiteral("read_all"),
               QStringLiteral("activity:read_all"),
               QStringLiteral("activity:write")
           });
    QTest::newRow("empty")
        << authorizationPayload(QString());
    QTest::newRow("blank")
        << authorizationPayload(QStringLiteral("   "));
    QTest::newRow("missing-route-read")
        << authorizationPayload(QStringLiteral(
               "activity:read_all activity:write"));
    QTest::newRow("missing-activity-read")
        << authorizationPayload(QStringLiteral(
               "read_all activity:write"));
    QTest::newRow("missing-upload")
        << authorizationPayload(QStringLiteral(
               "read_all activity:read_all"));
    QTest::newRow("case-sensitive")
        << authorizationPayload(QStringLiteral(
               "READ_ALL activity:read_all activity:write"));
    QTest::newRow("tab-delimited")
        << authorizationPayload(QStringLiteral(
               "read_all\tactivity:read_all\tactivity:write"));
    QTest::newRow("oversized")
        << authorizationPayload(
               QString(4096, QLatin1Char('A')));

    QStringList excessiveScopes{
        QStringLiteral("read_all"),
        QStringLiteral("activity:read_all"),
        QStringLiteral("activity:write")
    };
    for (int index = 0; index < 65; ++index) {
        excessiveScopes.append(
            QStringLiteral("future:%1").arg(index));
    }
    QTest::newRow("too-many")
        << authorizationPayload(
               excessiveScopes.join(QLatin1Char(' ')));
}

void TestStravaOAuthPolicy::rejectsInvalidAuthorizationScopes()
{
    QFETCH(QByteArray, payload);

    const auto response =
        StravaOAuthPolicy::parseAuthorizationResponse(payload);

    QVERIFY(!response.isValid());
    QVERIFY(response.accessToken.isEmpty());
    QVERIFY(response.refreshToken.isEmpty());
    QVERIFY(response.grantedScopes.isEmpty());
    QVERIFY(!response.error.isEmpty());
    QVERIFY(!response.error.contains(AccessToken));
    QVERIFY(!response.error.contains(RefreshToken));
    QVERIFY(response.error.size() <= 1024);
}

void TestStravaOAuthPolicy::
serviceOpenUsesCoordinatedDurableRefresh()
{
    const QByteArray source = sourceContents(
        "src/Cloud/Strava.cpp");
    QVERIFY(!source.isEmpty());
    QCOMPARE(
        source.count(
            "QNetworkRequest::SameOriginRedirectPolicy"),
        8);

    const QByteArray descriptionRead = sourceSection(
        source,
        "Strava::updateActivityDescription(",
        "Strava::dispatchDescriptionUpdate(");
    QVERIFY(!descriptionRead.isEmpty());
    QVERIFY(descriptionRead.contains(
        "QNetworkRequest::SameOriginRedirectPolicy"));
    QVERIFY(descriptionRead.contains(
        "StravaTokenRefreshCoordinator::beginAuthorizedRequest("));
    QVERIFY(descriptionRead.contains(".authorizeDispatch()"));

    const QByteArray descriptionWrite = sourceSection(
        source,
        "Strava::dispatchDescriptionUpdate(",
        "Strava::descriptionReadCompleted()");
    QVERIFY(!descriptionWrite.isEmpty());
    QVERIFY(descriptionWrite.contains(
        "QNetworkRequest::SameOriginRedirectPolicy"));
    QVERIFY(descriptionWrite.contains(
        "StravaTokenRefreshCoordinator::beginAuthorizedRequest("));
    QVERIFY(descriptionWrite.contains(".authorizeDispatch()"));

    const QByteArray open = sourceSection(
        source,
        "Strava::open(QStringList &errors)",
        "Strava::close()");
    QVERIFY(!open.isEmpty());
    QVERIFY(open.contains("_gcAthleteName"));
    QVERIFY(open.contains(
        "StravaTokenRefreshCoordinator::refresh("));
    QVERIFY(open.contains("waitForNetworkReply("));
    QVERIFY(open.contains(
        "StravaCredentialPublisher::publish("));
    QVERIFY(source.contains(
        "StravaCredentialPublisher::CredentialSnapshotTimeoutMs"));
    QVERIFY(open.contains(
        "StravaCredentialPublisher::beginMutation("));
    QVERIFY(open.contains(
        "MutationKind::Refresh"));
    QVERIFY(open.contains("mutation->isCurrent("));
    QVERIFY(open.contains("publication.mutation = mutation"));
    QVERIFY(open.contains(
        "PublicationMode::"));
    QVERIFY(open.contains("CompareAndSwap"));
    QVERIFY(open.contains(
        "result.sourceRefreshToken"));
    QVERIFY(open.contains("tokenReply->deleteLater()"));
    QVERIFY(open.contains(
        "NetworkReplyWaitResult::Destroyed"));
    QVERIFY(!open.contains("QEventLoop"));
    QVERIFY(!open.contains(
        "CloudServiceFactory::instance().saveSettings"));

    const qsizetype refreshOperation =
        open.indexOf("refreshOperation =");
    const qsizetype durablePublication =
        open.indexOf(
            "StravaCredentialPublisher::publish(");
    const qsizetype coordinatedRefresh =
        open.indexOf(
            "StravaTokenRefreshCoordinator::refresh(");
    QVERIFY(refreshOperation >= 0);
    QVERIFY(durablePublication > refreshOperation);
    QVERIFY(coordinatedRefresh > durablePublication);
    QCOMPARE(
        open.count(
            "StravaCredentialPublisher::publish("),
        1);
}

void TestStravaOAuthPolicy::
oauthGrantSupersedesRefreshAndPublishesDurably()
{
    const QByteArray source = sourceContents(
        "src/Cloud/OAuthDialog.cpp");
    const QByteArray header = sourceContents(
        "src/Cloud/OAuthDialog.h");
    const QByteArray settingsCommit = sourceContents(
        "src/Cloud/StravaSettingsCommit.cpp");
    QVERIFY(!source.isEmpty());
    QVERIFY(!header.isEmpty());
    QVERIFY(!settingsCommit.isEmpty());
    const QByteArray constructor = sourceSection(
        source,
        "OAuthDialog::OAuthDialog(",
        "OAuthDialog::~OAuthDialog()");
    QVERIFY(!constructor.isEmpty());
    QVERIFY(!constructor.contains(
        "StravaCredentialPublisher::recover("));
    QVERIFY(!constructor.contains(
        "StravaCredentialPublisher::readStoredAuthorization("));
    QVERIFY(source.contains(
        "StravaOAuthPolicy::parseAuthorizationResponse(payload)"));
    QVERIFY(source.contains(
        "QNetworkRequest::SameOriginRedirectPolicy"));
    QVERIFY(source.contains(
        "tokenReplyController->start(reply, 30000)"));
    QVERIFY(source.contains(
        "tokenReplyController->complete(reply)"));
    QVERIFY(source.contains(
        "OAuthTokenReplyController::Completion::TimedOut"));
    QVERIFY(source.contains("Qt::QueuedConnection"));
    QVERIFY(header.contains(
        "std::uint64_t stravaAuthorizationEpoch"));
    QVERIFY(source.contains(
        "StravaTokenRefreshCoordinator::authorizationEpoch("));
    QVERIFY(source.contains(
        "StravaCredentialPublisher::recover(accountKey, 30000)"));
    QVERIFY(source.contains(
        "StravaCredentialPublisher::beginMutation("));
    QVERIFY(source.contains(
        "MutationKind::Authorization"));
    QVERIFY(source.contains(
        "publication.mutation = stravaCredentialMutation"));
    const qsizetype oauthMutation = source.indexOf(
        "StravaCredentialPublisher::beginMutation(");
    const qsizetype oauthRecovery = source.indexOf(
        "StravaCredentialPublisher::recover(accountKey, 30000)");
    const qsizetype oauthStoredState = source.indexOf(
        "StravaCredentialPublisher::readStoredAuthorization(",
        oauthMutation);
    const qsizetype oauthCommitUnknown = source.indexOf(
        "mutation->markCommitUnknown(error)",
        oauthMutation);
    const qsizetype oauthTokenPost = source.indexOf(
        "reply = manager->post(request, data);",
        oauthCommitUnknown);
    QVERIFY(oauthRecovery >= 0);
    QVERIFY(oauthMutation > oauthRecovery);
    QVERIFY(oauthStoredState > oauthMutation);
    QVERIFY(oauthCommitUnknown > oauthMutation);
    QVERIFY(oauthTokenPost > oauthCommitUnknown);
    QVERIFY(source.contains(
        "stravaCredentialMutation->abortBeforeRemoteDispatch("));
    QVERIFY(source.contains("attempt->cancelled"));
    QVERIFY(source.contains(
        "OAuthDialogMessageGuard::showAndAccept("));
    QVERIFY(source.contains(
        "mutation->abortBeforeRemoteDispatch(abortError)"));
    QVERIFY(!source.contains("runOnCredentialThread("));
    QVERIFY(settingsCommit.contains(
        "completionContext, &QObject::destroyed"));
    QVERIFY(settingsCommit.contains("Qt::QueuedConnection"));

    const qsizetype start = source.lastIndexOf(
        "} else if (site == STRAVA) {");
    QVERIFY(start >= 0);
    const qsizetype end = source.indexOf(
        "} else if (site == CYCLING_ANALYTICS) {", start);
    QVERIFY(end > start);
    const QByteArray branch = source.mid(start, end - start);

    const qsizetype install = branch.indexOf(
        "installAuthorizationDurably(");
    const qsizetype publish = branch.indexOf(
        "publish(publication)");
    const qsizetype updateClone = branch.indexOf(
        "service->setSetting(GC_STRAVA_TOKEN");

    QVERIFY(branch.contains("_gcAthleteName"));
    QVERIFY(branch.contains(
        "PublicationMode::Authoritative"));
    QVERIFY(branch.contains(
        "publication.activatesAuthorization = true"));
    QVERIFY(branch.contains(
        "clearsRemoteGrantUncertainty"));
    QVERIFY(branch.contains(
        "authorizationSnapshot("));
    QVERIFY(branch.contains(
        "stravaAuthorizationEpoch"));
    QVERIFY(branch.contains(
        "runOnCredentialThreadAsync("));
    QVERIFY(source.contains("stravaCredentialAttempt"));
    QVERIFY(install >= 0);
    QVERIFY(publish > install);
    QCOMPARE(updateClone, qsizetype(-1));
}

void TestStravaOAuthPolicy::
serviceDisconnectRevokesBeforeCredentialRemoval()
{
    const QByteArray removal = sourceContents(
        "src/Cloud/StravaAccountRemoval.cpp");
    QVERIFY(!removal.isEmpty());
    QVERIFY(removal.contains(
        "removeAuthorizationTransaction("));
    QVERIFY(removal.contains(
        "markRevocationPendingTracked("));
    QVERIFY(removal.contains(
        "StravaCredentialPublisher::beginMutation("));
    QVERIFY(removal.contains(
        "MutationKind::Revocation"));
    QVERIFY(removal.contains("mutation->isCurrent("));
    QVERIFY(removal.contains("removal.mutation = mutation"));
    const qsizetype removalCommitUnknown = removal.indexOf(
        "mutation->markCommitUnknown(");
    const qsizetype removalFence = removal.indexOf(
        "readStoredAuthorization(mutation)");
    const qsizetype remoteRevocation = removal.indexOf(
        "revokeRemote(", removalCommitUnknown);
    const qsizetype localCommit = removal.indexOf(
        "mutation->markLocalCommitStarted(", remoteRevocation);
    const qsizetype localRemoval = removal.indexOf(
        "StravaCredentialPublisher::remove(", localCommit);
    QVERIFY(removalFence >= 0);
    QVERIFY(removalCommitUnknown >= 0);
    QVERIFY(removalFence < removalCommitUnknown);
    QVERIFY(remoteRevocation > removalCommitUnknown);
    QVERIFY(localCommit > remoteRevocation);
    QVERIFY(localRemoval > localCommit);
    QVERIFY(removal.contains(
        "StravaCredentialPublisher::remove("));
    QVERIFY(!removal.contains("Bearer "));

    const QByteArray revocation = sourceContents(
        "src/Cloud/StravaRevocationClient.cpp");
    QVERIFY(!revocation.isEmpty());
    QVERIFY(revocation.contains(
        "StravaOAuthPolicy::revocationRequest("));
    QVERIFY(revocation.contains(
        "response.httpStatus != 200"));

    const QByteArray service = sourceContents(
        "src/Cloud/Strava.cpp");
    QVERIFY(!service.isEmpty());
    const QByteArray disconnect = sourceSection(
        service,
        "Strava::accountDisconnectOperation(",
        "Strava::open(QStringList &errors)");
    QVERIFY(!disconnect.isEmpty());
    QVERIFY(disconnect.contains(
        "authorizationSnapshot("));
    QVERIFY(!disconnect.contains(
        "getSetting(GC_STRAVA_TOKEN"));
    QVERIFY(!disconnect.contains(
        "getSetting(GC_STRAVA_REFRESH_TOKEN"));

    const QByteArray refresh = sourceSection(
        service,
        "Strava::refreshAccessGrant(",
        "Strava::performAuthenticatedGet(");
    const qsizetype pending = refresh.indexOf(
        "markAuthorizationPendingTracked(");
    const qsizetype tokenPost = refresh.indexOf(
        "nam->post(request, tokenRequest.body)");
    const qsizetype commitUnknown = refresh.indexOf(
        "mutation->markCommitUnknown(");
    const qsizetype vaultFence = refresh.indexOf(
        "readStoredAuthorization(mutation)");
    const qsizetype cancellationCheck =
        refresh.indexOf("if (interrupted())", pending);
    QVERIFY(pending >= 0);
    QVERIFY(vaultFence > pending);
    QVERIFY(cancellationCheck > pending);
    QVERIFY(cancellationCheck < tokenPost);
    QVERIFY(commitUnknown > cancellationCheck);
    QVERIFY(commitUnknown < tokenPost);
    QVERIFY(tokenPost > pending);
    QVERIFY(vaultFence < tokenPost);
    QVERIFY(refresh.contains(
        "mutation->abortBeforeRemoteDispatch("));
    QVERIFY(refresh.contains(
        "publication.activatesAuthorization = true"));
    QVERIFY(refresh.contains(
        "publication.clearsRemoteGrantUncertainty = true"));
    QVERIFY(!refresh.contains("setSetting(GC_STRAVA_TOKEN"));
    QVERIFY(!refresh.contains(
        "setSetting(GC_STRAVA_REFRESH_TOKEN"));

    const QByteArray close = sourceSection(
        service,
        "Strava::close()",
        "Strava::readdir(");
    QVERIFY(!close.isEmpty());
    QVERIFY(!close.contains("/oauth/revoke"));
    QVERIFY(!close.contains("removeAuthorization("));

    const QByteArray publisher = sourceContents(
        "src/Cloud/StravaCredentialPublisher.cpp");
    QVERIFY(!publisher.isEmpty());
    QVERIFY(publisher.contains(
        "RemovalStatus::CleanupPending"));
    QVERIFY(publisher.contains(
        "result.status == RemovalStatus::Cleared"));

    QCOMPARE(
        service.count(
            "StravaTokenRefreshCoordinator::beginAuthorizedRequest("),
        7);
    QCOMPARE(
        service.count(".authorizeDispatch()"),
        7);
    QCOMPARE(
        service.count(".setAbortOperation("),
        7);
    QVERIFY(service.contains(
        "reconcileSharedAuthorizationStatus()"));
    QVERIFY(service.contains(
        "const StravaCredentialPublisher::StoredAuthorization stored ="));
    QVERIFY(!service.contains(
        "stored->revision != metadata.revision"));
    const qsizetype reconcile = service.indexOf(
        "reconcileSharedAuthorizationStatus()");
    const qsizetype permit = service.indexOf(
        "StravaTokenRefreshCoordinator::beginAuthorizedRequest(",
        reconcile);
    QVERIFY(reconcile >= 0);
    QVERIFY(permit > reconcile);
    const QByteArray readdir = sourceSection(
        service, "Strava::readdir(", "Strava::readFile(");
    const qsizetype pageLoop = readdir.indexOf(
        "while (offset < resultCount)");
    const qsizetype pageReconcile = readdir.indexOf(
        "reconcileSharedAuthorizationStatus()", pageLoop);
    const qsizetype pagePermit = readdir.indexOf(
        "beginAuthorizedRequest(", pageLoop);
    QVERIFY(pageLoop >= 0);
    QVERIFY(pageReconcile > pageLoop);
    QVERIFY(pagePermit > pageReconcile);
    QVERIFY(!service.contains(
        "StravaTokenRefreshCoordinator::authorizationUsable("));
}

void TestStravaOAuthPolicy::
credentialsPageOffersExplicitStravaDisconnectModes()
{
    const QByteArray source = sourceContents(
        "src/Gui/AthletePages.cpp");
    QVERIFY(!source.isEmpty());
    const QByteArray deletion = sourceSection(
        source,
        "CredentialsPage::deleteClicked()",
        "CredentialsPage::editClicked()");
    QVERIFY(!deletion.isEmpty());
    QVERIFY(deletion.contains(
        "supportsAccountDisconnect()"));
    QVERIFY(deletion.contains(
        "accountDisconnectOperation("));
    QVERIFY(deletion.contains("QtConcurrent::run("));
    QVERIFY(deletion.contains("QProgressDialog"));
    QVERIFY(deletion.contains("Disconnect from Strava"));
    QVERIFY(deletion.contains("Remove locally"));
    QVERIFY(deletion.contains("QMessageBox::Cancel"));

    const qsizetype operation = deletion.indexOf(
        "accountDisconnectOperation(");
    const qsizetype operationSucceeded = deletion.indexOf(
        "if (!result.isSuccess())", operation);
    const qsizetype deactivate = deletion.indexOf(
        "appsettings->setCValue(",
        operationSucceeded);
    QVERIFY(operation >= 0);
    QVERIFY(operationSucceeded > operation);
    QVERIFY(deactivate > operationSucceeded);
    QVERIFY(!deletion.contains("QEventLoop"));
    QVERIFY(deletion.contains(
        "result.cleanupPending"
        " && result.remoteAuthorizationMayRemain"));
    QVERIFY(deletion.contains(
        "The local Strava credentials were removed"));
    QVERIFY(deletion.contains("setEnabled(false)"));
    QVERIFY(deletion.contains("setEnabled(true)"));
    QVERIFY(deletion.contains("setCancelButton(nullptr)"));
    QVERIFY(deletion.contains("Qt::QueuedConnection"));
    QVERIFY(deletion.contains("irreversible"));
    QVERIFY(deletion.contains(
        "std::shared_ptr<QObject>"));
    QVERIFY(deletion.contains("irreversibleContext.get()"));
    QVERIFY(deletion.contains("irreversibleStarted->load("));
    QVERIFY(!deletion.contains("guardedProgress.data(),"));
    QVERIFY(!deletion.contains("account was disabled"));

    const qsizetype phaseContext =
        deletion.indexOf("irreversibleContext.get()");
    const qsizetype phaseQueued =
        deletion.indexOf("Qt::QueuedConnection", phaseContext);
    QVERIFY(phaseContext >= 0);
    QVERIFY(phaseQueued > phaseContext);
    const QByteArray phaseUpdate = deletion.mid(
        phaseContext, phaseQueued - phaseContext);
    QVERIFY(phaseUpdate.contains("guardedProgress->show()"));

    const qsizetype resultCall =
        deletion.indexOf("watcher->result()");
    const qsizetype resultTry =
        deletion.lastIndexOf("try {", resultCall);
    const qsizetype resultCatch =
        deletion.indexOf("catch (...)", resultCall);
    QVERIFY(resultCall >= 0);
    QVERIFY(resultTry >= 0);
    QVERIFY(resultCatch > resultCall);
}

QTEST_MAIN(TestStravaOAuthPolicy)
#include "testStravaOAuthPolicy.moc"
