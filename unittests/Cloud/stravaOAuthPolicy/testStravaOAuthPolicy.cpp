#include "Cloud/StravaOAuthPolicy.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTest>
#include <QUrlQuery>

namespace {

const QString ClientId = QStringLiteral("83");
const QString ClientSecret = QStringLiteral("synthetic-client-secret");
const QString AccessToken =
    QStringLiteral("synthetic-access-token");
const QString RefreshToken =
    QStringLiteral("synthetic-refresh-token");

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
    const QString path = QFINDTESTDATA(relativePath);
    if (path.isEmpty()) return {};

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
    void buildsAuthorizationCodeRequest();
    void encodesAuthorizationCodeRequestValues();
    void rejectsInvalidAuthorizationCodeRequest_data();
    void rejectsInvalidAuthorizationCodeRequest();
    void buildsRefreshTokenRequest();
    void rejectsInvalidRefreshTokenRequest_data();
    void rejectsInvalidRefreshTokenRequest();
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
    void oauthGrantSupersedesRefreshAndPublishesDurably();
};

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
        "../../../src/Cloud/Strava.cpp");
    QVERIFY(!source.isEmpty());
    QCOMPARE(
        source.count(
            "QNetworkRequest::SameOriginRedirectPolicy"),
        6);

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
    QVERIFY(open.contains(
        "PublicationMode::CompareAndSwap"));
    QVERIFY(open.contains(
        "refreshResult.sourceRefreshToken"));
    QVERIFY(open.contains("tokenReply->deleteLater()"));
    QVERIFY(open.contains(
        "NetworkReplyWaitResult::Destroyed"));
    QVERIFY(!open.contains("QEventLoop"));
    QVERIFY(!open.contains(
        "CloudServiceFactory::instance().saveSettings"));
}

void TestStravaOAuthPolicy::
oauthGrantSupersedesRefreshAndPublishesDurably()
{
    const QByteArray source = sourceContents(
        "../../../src/Cloud/OAuthDialog.cpp");
    QVERIFY(!source.isEmpty());
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

    const qsizetype start = source.lastIndexOf(
        "} else if (site == STRAVA) {");
    QVERIFY(start >= 0);
    const qsizetype end = source.indexOf(
        "} else if (site == CYCLING_ANALYTICS) {", start);
    QVERIFY(end > start);
    const QByteArray branch = source.mid(start, end - start);

    const qsizetype invalidate = branch.indexOf(
        "StravaTokenRefreshCoordinator::invalidate(");
    const qsizetype publish = branch.indexOf(
        "StravaCredentialPublisher::publish(");
    const qsizetype install = branch.indexOf(
        "installAuthorization(");
    const qsizetype updateClone = branch.indexOf(
        "service->setSetting(GC_STRAVA_TOKEN");

    QVERIFY(branch.contains("_gcAthleteName"));
    QVERIFY(branch.contains(
        "PublicationMode::Authoritative"));
    QVERIFY(invalidate >= 0);
    QVERIFY(publish > invalidate);
    QVERIFY(install > publish);
    QVERIFY(updateClone > install);
}

QTEST_MAIN(TestStravaOAuthPolicy)
#include "testStravaOAuthPolicy.moc"
