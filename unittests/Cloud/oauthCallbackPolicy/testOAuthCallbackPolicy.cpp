#include "Cloud/OAuthCallbackPolicy.h"

#include <QNetworkReply>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>

namespace {

QString validState()
{
    return QString(43, QLatin1Char('A'));
}

QUrl remoteRedirect()
{
    return QUrl(QStringLiteral(
        "https://www.goldencheetah.org/callback"));
}

QUrl validRemoteCallback(const QString &code = QStringLiteral("code-123"))
{
    QUrl callback = remoteRedirect();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("code"), code);
    query.addQueryItem(QStringLiteral("state"), validState());
    callback.setQuery(query);
    return callback;
}

} // namespace

class TestOAuthCallbackPolicy : public QObject
{
    Q_OBJECT

private slots:
    void generatedStatesAreStrongAndUnique();
    void authorizationUrlHasExactlyOneState();
    void acceptsSecureRedirectUris_data();
    void acceptsSecureRedirectUris();
    void rejectsUnsafeRedirectUris_data();
    void rejectsUnsafeRedirectUris();
    void rejectsUnsafeAuthorizationEndpoints_data();
    void rejectsUnsafeAuthorizationEndpoints();
    void rejectsInvalidExpectedSessions_data();
    void rejectsInvalidExpectedSessions();
    void acceptsExactHttpsCallback();
    void acceptsExactLoopbackCallback_data();
    void acceptsExactLoopbackCallback();
    void rejectsMismatchedCallbacks_data();
    void rejectsMismatchedCallbacks();
    void acceptsAuthorizationErrorOnce();
    void rejectsCallbackReplay();
    void tokenRepliesRequireVerifiedTls();
};

void TestOAuthCallbackPolicy::generatedStatesAreStrongAndUnique()
{
    const QString first = OAuthCallbackPolicy::generateState();
    const QString second = OAuthCallbackPolicy::generateState();

    QCOMPARE(first.size(), 43);
    QCOMPARE(second.size(), 43);
    QVERIFY(OAuthCallbackPolicy::isWellFormedState(first));
    QVERIFY(OAuthCallbackPolicy::isWellFormedState(second));
    QVERIFY(first != second);
}

void TestOAuthCallbackPolicy::authorizationUrlHasExactlyOneState()
{
    const QString state = validState();
    const QUrl input(QStringLiteral(
        "https://provider.example/authorize?"
        "client_id=client-1&state=fixed&scope=read"));
    QString error;

    const QUrl output =
        OAuthCallbackPolicy::authorizationUrlWithState(
            input, state, &error);

    QVERIFY2(output.isValid() && !output.isEmpty(), qPrintable(error));
    QCOMPARE(output.scheme(), QStringLiteral("https"));
    QCOMPARE(output.host(), QStringLiteral("provider.example"));
    const QUrlQuery query(output);
    QCOMPARE(query.allQueryItemValues(QStringLiteral("state")),
             QStringList{state});
    QCOMPARE(query.queryItemValue(QStringLiteral("client_id")),
             QStringLiteral("client-1"));
    QCOMPARE(query.queryItemValue(QStringLiteral("scope")),
             QStringLiteral("read"));
}

void TestOAuthCallbackPolicy::acceptsSecureRedirectUris_data()
{
    QTest::addColumn<QUrl>("redirectUri");

    QTest::newRow("remote-https")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org/callback"));
    QTest::newRow("localhost-default-port")
        << QUrl(QStringLiteral("http://localhost/callback"));
    QTest::newRow("ipv4-loopback")
        << QUrl(QStringLiteral("http://127.0.0.1:49152/callback"));
    QTest::newRow("ipv6-loopback")
        << QUrl(QStringLiteral("http://[::1]:49152/callback"));
}

void TestOAuthCallbackPolicy::acceptsSecureRedirectUris()
{
    QFETCH(QUrl, redirectUri);
    QVERIFY(OAuthCallbackPolicy::isSecureRedirectUri(redirectUri));
}

void TestOAuthCallbackPolicy::rejectsUnsafeRedirectUris_data()
{
    QTest::addColumn<QUrl>("redirectUri");

    QTest::newRow("remote-http")
        << QUrl(QStringLiteral(
            "http://www.goldencheetah.org/callback"));
    QTest::newRow("loopback-lookalike")
        << QUrl(QStringLiteral(
            "http://localhost.attacker.invalid/callback"));
    QTest::newRow("userinfo")
        << QUrl(QStringLiteral(
            "https://attacker@www.goldencheetah.org/callback"));
    QTest::newRow("password")
        << QUrl(QStringLiteral(
            "https://user:secret@www.goldencheetah.org/callback"));
    QTest::newRow("fragment")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org/callback#code=stolen"));
    QTest::newRow("preconfigured-query")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org/callback?code=fixed"));
    QTest::newRow("relative")
        << QUrl(QStringLiteral("/callback"));
    QTest::newRow("unsupported-scheme")
        << QUrl(QStringLiteral(
            "ftp://www.goldencheetah.org/callback"));
}

void TestOAuthCallbackPolicy::rejectsUnsafeRedirectUris()
{
    QFETCH(QUrl, redirectUri);
    QVERIFY(!OAuthCallbackPolicy::isSecureRedirectUri(redirectUri));
}

void TestOAuthCallbackPolicy::rejectsUnsafeAuthorizationEndpoints_data()
{
    QTest::addColumn<QUrl>("authorizationUrl");

    QTest::newRow("http")
        << QUrl(QStringLiteral(
            "http://provider.example/authorize"));
    QTest::newRow("userinfo")
        << QUrl(QStringLiteral(
            "https://user@provider.example/authorize"));
    QTest::newRow("fragment")
        << QUrl(QStringLiteral(
            "https://provider.example/authorize#callback"));
    QTest::newRow("relative")
        << QUrl(QStringLiteral("/authorize"));
}

void TestOAuthCallbackPolicy::rejectsUnsafeAuthorizationEndpoints()
{
    QFETCH(QUrl, authorizationUrl);
    QString error;

    const QUrl output =
        OAuthCallbackPolicy::authorizationUrlWithState(
            authorizationUrl, validState(), &error);

    QVERIFY(output.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestOAuthCallbackPolicy::rejectsInvalidExpectedSessions_data()
{
    QTest::addColumn<QUrl>("redirectUri");
    QTest::addColumn<QString>("state");

    QTest::newRow("unsafe-redirect")
        << QUrl(QStringLiteral(
               "http://www.goldencheetah.org/callback"))
        << validState();
    QTest::newRow("missing-state")
        << remoteRedirect()
        << QString();
    QTest::newRow("short-state")
        << remoteRedirect()
        << QStringLiteral("predictable");
}

void TestOAuthCallbackPolicy::rejectsInvalidExpectedSessions()
{
    QFETCH(QUrl, redirectUri);
    QFETCH(QString, state);

    OAuthCallbackPolicy::Session session(redirectUri, state);

    QVERIFY(!session.isValid());
    QVERIFY(!session.isConsumed());
    const OAuthCallbackPolicy::Outcome outcome =
        session.consume(validRemoteCallback());
    QVERIFY(outcome.type
            == OAuthCallbackPolicy::OutcomeType::Reject);
    QVERIFY(!session.isConsumed());
}

void TestOAuthCallbackPolicy::acceptsExactHttpsCallback()
{
    OAuthCallbackPolicy::Session session(
        remoteRedirect(), validState());

    QVERIFY(session.isValid());
    const OAuthCallbackPolicy::Outcome outcome =
        session.consume(validRemoteCallback());

    QVERIFY(outcome.type
            == OAuthCallbackPolicy::OutcomeType::AuthorizationCode);
    QCOMPARE(outcome.code, QStringLiteral("code-123"));
    QVERIFY(outcome.error.isEmpty());
    QVERIFY(session.isConsumed());
}

void TestOAuthCallbackPolicy::acceptsExactLoopbackCallback_data()
{
    QTest::addColumn<QUrl>("redirectUri");

    QTest::newRow("localhost-default-port")
        << QUrl(QStringLiteral("http://localhost/callback"));
    QTest::newRow("localhost-ephemeral-port")
        << QUrl(QStringLiteral(
            "http://localhost:49152/callback"));
    QTest::newRow("ipv4-ephemeral-port")
        << QUrl(QStringLiteral(
            "http://127.0.0.1:49152/callback"));
    QTest::newRow("ipv6-ephemeral-port")
        << QUrl(QStringLiteral(
            "http://[::1]:49152/callback"));
}

void TestOAuthCallbackPolicy::acceptsExactLoopbackCallback()
{
    QFETCH(QUrl, redirectUri);
    OAuthCallbackPolicy::Session session(
        redirectUri, validState());
    QUrl callback = redirectUri;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("code"),
                       QStringLiteral("loopback-code"));
    query.addQueryItem(QStringLiteral("state"), validState());
    callback.setQuery(query);

    const OAuthCallbackPolicy::Outcome outcome =
        session.consume(callback);

    QVERIFY(outcome.type
            == OAuthCallbackPolicy::OutcomeType::AuthorizationCode);
    QCOMPARE(outcome.code, QStringLiteral("loopback-code"));
    QVERIFY(session.isConsumed());
}

void TestOAuthCallbackPolicy::rejectsMismatchedCallbacks_data()
{
    QTest::addColumn<QUrl>("callback");
    const QString state = validState();
    const QString base =
        QStringLiteral(
            "https://www.goldencheetah.org/callback");

    QTest::newRow("wrong-state")
        << QUrl(base + QStringLiteral("?code=code-123&state=")
                + QString(43, QLatin1Char('B')));
    QTest::newRow("case-changed-state")
        << QUrl(base + QStringLiteral("?code=code-123&state=")
                + QString(43, QLatin1Char('a')));
    QTest::newRow("missing-state")
        << QUrl(base + QStringLiteral("?code=code-123"));
    QTest::newRow("duplicate-state")
        << QUrl(base + QStringLiteral("?code=code-123&state=")
                + state + QStringLiteral("&state=") + state);
    QTest::newRow("missing-code")
        << QUrl(base + QStringLiteral("?state=") + state);
    QTest::newRow("empty-code")
        << QUrl(base + QStringLiteral("?code=&state=") + state);
    QTest::newRow("duplicate-code")
        << QUrl(base + QStringLiteral(
            "?code=first&code=second&state=") + state);
    QTest::newRow("code-and-error")
        << QUrl(base + QStringLiteral(
            "?code=code-123&error=denied&state=") + state);
    QTest::newRow("http-downgrade")
        << QUrl(QStringLiteral(
            "http://www.goldencheetah.org/callback?"
            "code=code-123&state=") + state);
    QTest::newRow("attacker-host")
        << QUrl(QStringLiteral(
            "https://attacker.invalid/callback?"
            "code=code-123&state=") + state);
    QTest::newRow("lookalike-host")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org.attacker.invalid/callback?"
            "code=code-123&state=") + state);
    QTest::newRow("wrong-path")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org/other?"
            "code=code-123&state=") + state);
    QTest::newRow("wrong-port")
        << QUrl(QStringLiteral(
            "https://www.goldencheetah.org:444/callback?"
            "code=code-123&state=") + state);
    QTest::newRow("userinfo")
        << QUrl(QStringLiteral(
            "https://attacker@www.goldencheetah.org/callback?"
            "code=code-123&state=") + state);
    QTest::newRow("fragment")
        << QUrl(base + QStringLiteral("?code=code-123&state=")
                + state + QStringLiteral("#ignored"));
}

void TestOAuthCallbackPolicy::rejectsMismatchedCallbacks()
{
    QFETCH(QUrl, callback);
    OAuthCallbackPolicy::Session session(
        remoteRedirect(), validState());

    const OAuthCallbackPolicy::Outcome rejected =
        session.consume(callback);

    QVERIFY(rejected.type
            == OAuthCallbackPolicy::OutcomeType::Reject);
    QVERIFY(rejected.code.isEmpty());
    QVERIFY(!session.isConsumed());

    const OAuthCallbackPolicy::Outcome accepted =
        session.consume(validRemoteCallback());
    QVERIFY(accepted.type
            == OAuthCallbackPolicy::OutcomeType::AuthorizationCode);
    QVERIFY(session.isConsumed());
}

void TestOAuthCallbackPolicy::acceptsAuthorizationErrorOnce()
{
    OAuthCallbackPolicy::Session session(
        remoteRedirect(), validState());
    QUrl callback = remoteRedirect();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("error"),
                       QStringLiteral("access_denied"));
    query.addQueryItem(QStringLiteral("error_description"),
                       QStringLiteral("The user denied access."));
    query.addQueryItem(QStringLiteral("state"), validState());
    callback.setQuery(query);

    const OAuthCallbackPolicy::Outcome outcome =
        session.consume(callback);

    QVERIFY(outcome.type
            == OAuthCallbackPolicy::OutcomeType::AuthorizationError);
    QVERIFY(outcome.code.isEmpty());
    QCOMPARE(outcome.error,
             QStringLiteral("The user denied access."));
    QVERIFY(session.isConsumed());

    const OAuthCallbackPolicy::Outcome replay =
        session.consume(callback);
    QVERIFY(replay.type
            == OAuthCallbackPolicy::OutcomeType::Reject);
}

void TestOAuthCallbackPolicy::rejectsCallbackReplay()
{
    OAuthCallbackPolicy::Session session(
        remoteRedirect(), validState());

    const OAuthCallbackPolicy::Outcome first =
        session.consume(validRemoteCallback(
            QStringLiteral("first-code")));
    const OAuthCallbackPolicy::Outcome replay =
        session.consume(validRemoteCallback(
            QStringLiteral("second-code")));

    QVERIFY(first.type
            == OAuthCallbackPolicy::OutcomeType::AuthorizationCode);
    QCOMPARE(first.code, QStringLiteral("first-code"));
    QVERIFY(replay.type
            == OAuthCallbackPolicy::OutcomeType::Reject);
    QVERIFY(replay.code.isEmpty());
    QVERIFY(session.isConsumed());
}

void TestOAuthCallbackPolicy::tokenRepliesRequireVerifiedTls()
{
    QVERIFY(OAuthCallbackPolicy::isSuccessfulTokenReply(
        QNetworkReply::NoError));
    QVERIFY(!OAuthCallbackPolicy::isSuccessfulTokenReply(
        QNetworkReply::SslHandshakeFailedError));
    QVERIFY(!OAuthCallbackPolicy::isSuccessfulTokenReply(
        QNetworkReply::ConnectionRefusedError));
}

QTEST_MAIN(TestOAuthCallbackPolicy)
#include "testOAuthCallbackPolicy.moc"
