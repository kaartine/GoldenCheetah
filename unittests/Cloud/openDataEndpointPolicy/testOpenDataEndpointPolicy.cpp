#include "Cloud/OpenDataEndpointPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTest>

class TestOpenDataEndpointPolicy : public QObject
{
    Q_OBJECT

private slots:
    void discoveryUsesPinnedHttpsUrl();
    void acceptsPinnedServerRoot();
    void rejectsUntrustedServerRoots_data();
    void rejectsUntrustedServerRoots();
    void filtersTamperedDiscoveryResponse();
    void attackerOnlyDiscoveryAuthorizesNoServer();
    void rejectsInvalidDiscoveryShapes_data();
    void rejectsInvalidDiscoveryShapes();
    void rejectsOversizedDiscoveryResponse();
    void rejectsExcessiveServerCount();
    void rejectsMalformedDiscoveryResponse();
    void buildsOnlyPinnedMetricsUrl();
    void requestsAreBoundedAndNeverFollowRedirects();
    void redirectsAreNeverSuccessful();
};

void TestOpenDataEndpointPolicy::discoveryUsesPinnedHttpsUrl()
{
    QCOMPARE(OpenDataEndpointPolicy::discoveryUrl(),
             QUrl(QStringLiteral("https://www.goldencheetah.org/opendata.json")));
}

void TestOpenDataEndpointPolicy::acceptsPinnedServerRoot()
{
    QVERIFY(OpenDataEndpointPolicy::isAllowedServerRoot(
        QUrl(QStringLiteral("https://liversedge.ddns.net:9090/"))));
}

void TestOpenDataEndpointPolicy::rejectsUntrustedServerRoots_data()
{
    QTest::addColumn<QUrl>("url");

    QTest::newRow("cleartext")
        << QUrl(QStringLiteral("http://liversedge.ddns.net:9090/"));
    QTest::newRow("attacker-host")
        << QUrl(QStringLiteral("https://attacker.invalid:9090/"));
    QTest::newRow("lookalike-subdomain")
        << QUrl(QStringLiteral("https://metrics.liversedge.ddns.net:9090/"));
    QTest::newRow("trailing-dot-host")
        << QUrl(QStringLiteral("https://liversedge.ddns.net.:9090/"));
    QTest::newRow("default-port")
        << QUrl(QStringLiteral("https://liversedge.ddns.net/"));
    QTest::newRow("wrong-port")
        << QUrl(QStringLiteral("https://liversedge.ddns.net:9091/"));
    QTest::newRow("userinfo")
        << QUrl(QStringLiteral("https://user@liversedge.ddns.net:9090/"));
    QTest::newRow("password")
        << QUrl(QStringLiteral("https://user:secret@liversedge.ddns.net:9090/"));
    QTest::newRow("path")
        << QUrl(QStringLiteral("https://liversedge.ddns.net:9090/base/"));
    QTest::newRow("query")
        << QUrl(QStringLiteral("https://liversedge.ddns.net:9090/?next=attacker"));
    QTest::newRow("fragment")
        << QUrl(QStringLiteral("https://liversedge.ddns.net:9090/#attacker"));
}

void TestOpenDataEndpointPolicy::rejectsUntrustedServerRoots()
{
    QFETCH(QUrl, url);
    QVERIFY(!OpenDataEndpointPolicy::isAllowedServerRoot(url));
}

void TestOpenDataEndpointPolicy::filtersTamperedDiscoveryResponse()
{
    const QByteArray response = R"json({
        "SERVERS": [
            { "url": "https://attacker.invalid:9090/" },
            { "url": "http://liversedge.ddns.net:9090/" },
            { "url": "https://liversedge.ddns.net:9090/" },
            { "url": "https://liversedge.ddns.net:9090/" }
        ]
    })json";
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(response, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(servers,
             QList<QUrl>{QUrl(QStringLiteral("https://liversedge.ddns.net:9090/"))});
}

void TestOpenDataEndpointPolicy::attackerOnlyDiscoveryAuthorizesNoServer()
{
    const QByteArray response = R"json({
        "SERVERS": [
            { "url": "https://attacker.invalid:9090/" }
        ]
    })json";
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(response, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(servers.isEmpty());
}

void TestOpenDataEndpointPolicy::rejectsInvalidDiscoveryShapes_data()
{
    QTest::addColumn<QByteArray>("response");

    QTest::newRow("array-root") << QByteArrayLiteral("[]");
    QTest::newRow("missing-servers") << QByteArrayLiteral("{}");
    QTest::newRow("servers-not-array")
        << QByteArrayLiteral(R"json({"SERVERS": {}})json");
}

void TestOpenDataEndpointPolicy::rejectsInvalidDiscoveryShapes()
{
    QFETCH(QByteArray, response);
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(response, &error);

    QVERIFY(servers.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestOpenDataEndpointPolicy::rejectsOversizedDiscoveryResponse()
{
    const QByteArray response(64 * 1024 + 1, ' ');
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(response, &error);

    QVERIFY(servers.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestOpenDataEndpointPolicy::rejectsExcessiveServerCount()
{
    QJsonArray serversArray;
    for (int index = 0; index < 33; ++index) {
        QJsonObject server;
        server.insert(QStringLiteral("url"),
                      QStringLiteral("https://liversedge.ddns.net:9090/"));
        serversArray.append(server);
    }
    QJsonObject root;
    root.insert(QStringLiteral("SERVERS"), serversArray);
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(
            QJsonDocument(root).toJson(QJsonDocument::Compact), &error);

    QVERIFY(servers.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestOpenDataEndpointPolicy::rejectsMalformedDiscoveryResponse()
{
    QString error;

    const QList<QUrl> servers =
        OpenDataEndpointPolicy::allowedServerRootsFromDiscovery(
            QByteArrayLiteral("{not-json"), &error);

    QVERIFY(servers.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestOpenDataEndpointPolicy::buildsOnlyPinnedMetricsUrl()
{
    QCOMPARE(OpenDataEndpointPolicy::metricsUrl(
                 QUrl(QStringLiteral("https://liversedge.ddns.net:9090/"))),
             QUrl(QStringLiteral("https://liversedge.ddns.net:9090/metrics")));
    QVERIFY(OpenDataEndpointPolicy::metricsUrl(
                QUrl(QStringLiteral("https://attacker.invalid:9090/"))).isEmpty());
}

void TestOpenDataEndpointPolicy::requestsAreBoundedAndNeverFollowRedirects()
{
    const QNetworkRequest request = OpenDataEndpointPolicy::makeRequest(
        QUrl(QStringLiteral("https://www.goldencheetah.org/opendata.json")));

    QCOMPARE(request.transferTimeout(), 30 * 1000);
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
             static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
}

void TestOpenDataEndpointPolicy::redirectsAreNeverSuccessful()
{
    QVERIFY(OpenDataEndpointPolicy::isSuccessfulResponse(
        QNetworkReply::NoError, 200, QUrl()));
    QVERIFY(!OpenDataEndpointPolicy::isSuccessfulResponse(
        QNetworkReply::NoError, 302,
        QUrl(QStringLiteral("https://attacker.invalid/metrics"))));
    QVERIFY(!OpenDataEndpointPolicy::isSuccessfulResponse(
        QNetworkReply::NoError, 200,
        QUrl(QStringLiteral("https://attacker.invalid/metrics"))));
    QVERIFY(!OpenDataEndpointPolicy::isSuccessfulResponse(
        QNetworkReply::ConnectionRefusedError, 0, QUrl()));
}

QTEST_MAIN(TestOpenDataEndpointPolicy)
#include "testOpenDataEndpointPolicy.moc"
