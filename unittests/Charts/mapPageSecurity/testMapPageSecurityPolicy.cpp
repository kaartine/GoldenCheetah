/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Charts/MapPageSecurityPolicy.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTest>

#include <limits>

using MapPageSecurityPolicy::ResourceType;
using MapPageSecurityPolicy::TileEndpoint;

Q_DECLARE_METATYPE(ResourceType)

namespace {

const QString DefaultTileTemplate =
    QStringLiteral(
        "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png");

QString scriptDirective(const QString &policy)
{
    const QStringList directives =
        policy.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &directive : directives) {
        const QString trimmed = directive.trimmed();
        if (trimmed.startsWith(
                QStringLiteral("script-src "))) {
            return trimmed;
        }
    }
    return QString();
}

QByteArray resourceContents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

} // namespace

class TestMapPageSecurityPolicy : public QObject
{
    Q_OBJECT

private slots:
    void unsafeMapTypesFallBackToOpenStreetMap_data();
    void unsafeMapTypesFallBackToOpenStreetMap();
    void tileTemplatesAreValidated_data();
    void tileTemplatesAreValidated();
    void tileEndpointRestrictsImageOrigins_data();
    void tileEndpointRestrictsImageOrigins();
    void requestPolicyAllowsOnlyLocalAssetsAndTiles_data();
    void requestPolicyAllowsOnlyLocalAssetsAndTiles();
    void javaScriptStringsAreEncoded_data();
    void javaScriptStringsAreEncoded();
    void contentSecurityPolicyIsNarrow();
    void invalidCspInputsFailClosed_data();
    void invalidCspInputsFailClosed();
    void leafletAssetsArePackagedAndPinned();
    void mapPageIntegrationKeepsScriptsLocal();
};

void
TestMapPageSecurityPolicy::
unsafeMapTypesFallBackToOpenStreetMap_data()
{
    QTest::addColumn<int>("requested");
    QTest::addColumn<int>("expected");

    QTest::newRow("open-street-map") << 0 << 0;
    QTest::newRow("legacy-google") << 1 << 0;
    QTest::newRow("negative") << -1 << 0;
    QTest::newRow("large")
        << std::numeric_limits<int>::max() << 0;
}

void
TestMapPageSecurityPolicy::
unsafeMapTypesFallBackToOpenStreetMap()
{
    QFETCH(int, requested);
    QFETCH(int, expected);

    QCOMPARE(
        MapPageSecurityPolicy::safeMapType(requested),
        expected);
}

void
TestMapPageSecurityPolicy::tileTemplatesAreValidated_data()
{
    QTest::addColumn<QString>("tileTemplate");
    QTest::addColumn<bool>("valid");

    QTest::newRow("default-https")
        << DefaultTileTemplate << true;
    QTest::newRow("exact-https-with-key")
        << QStringLiteral(
               "https://tiles.example.test:8443/"
               "{z}/{x}/{y}.png?key=value")
        << true;
    QTest::newRow("loopback-localhost")
        << QStringLiteral(
               "http://localhost:8080/{z}/{x}/{y}.png")
        << true;
    QTest::newRow("loopback-ipv4")
        << QStringLiteral(
               "http://127.0.0.1:8080/{z}/{x}/{y}.png")
        << true;
    QTest::newRow("loopback-ipv6")
        << QStringLiteral(
               "http://[::1]:8080/{z}/{x}/{y}.png")
        << true;
    QTest::newRow("remote-http")
        << QStringLiteral(
               "http://tiles.example.test/{z}/{x}/{y}.png")
        << false;
    QTest::newRow("loopback-lookalike")
        << QStringLiteral(
               "http://localhost.attacker.invalid/{z}.png")
        << false;
    QTest::newRow("userinfo")
        << QStringLiteral(
               "https://user@tiles.example.test/{z}.png")
        << false;
    QTest::newRow("relative")
        << QStringLiteral("tiles/{z}/{x}/{y}.png")
        << false;
    QTest::newRow("file")
        << QStringLiteral("file:///tmp/{z}.png")
        << false;
    QTest::newRow("javascript")
        << QStringLiteral("javascript:alert(1)")
        << false;
    QTest::newRow("unknown-placeholder")
        << QStringLiteral(
               "https://tiles.example.test/{quadkey}.png")
        << false;
    QTest::newRow("subdomain-placeholder-in-path")
        << QStringLiteral(
               "https://tiles.example.test/{s}/{z}.png")
        << false;
    QTest::newRow("subdomain-placeholder-mid-label")
        << QStringLiteral(
               "https://prefix{s}.example.test/{z}.png")
        << false;
    QTest::newRow("fragment")
        << QStringLiteral(
               "https://tiles.example.test/{z}.png#fragment")
        << false;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("oversized")
        << (QStringLiteral("https://tiles.example.test/")
            + QString(5000, QLatin1Char('a')))
        << false;
}

void
TestMapPageSecurityPolicy::tileTemplatesAreValidated()
{
    QFETCH(QString, tileTemplate);
    QFETCH(bool, valid);

    const TileEndpoint endpoint =
        MapPageSecurityPolicy::tileEndpoint(tileTemplate);

    QCOMPARE(endpoint.isValid(), valid);
    if (!valid) {
        QVERIFY(endpoint.cspSource().isEmpty());
    }
}

void
TestMapPageSecurityPolicy::
tileEndpointRestrictsImageOrigins_data()
{
    QTest::addColumn<QString>("tileTemplate");
    QTest::addColumn<QUrl>("requestUrl");
    QTest::addColumn<bool>("allowed");

    QTest::newRow("subdomain-a")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://a.tile.openstreetmap.org/1/2/3.png"))
        << true;
    QTest::newRow("subdomain-b")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://b.tile.openstreetmap.org/1/2/3.png"))
        << true;
    QTest::newRow("subdomain-c")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://c.tile.openstreetmap.org/1/2/3.png"))
        << true;
    QTest::newRow("unexpected-subdomain")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://d.tile.openstreetmap.org/1/2/3.png"))
        << false;
    QTest::newRow("bare-domain")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://tile.openstreetmap.org/1/2/3.png"))
        << false;
    QTest::newRow("suffix-confusion")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://a.tile.openstreetmap.org.attacker.invalid/1.png"))
        << false;
    QTest::newRow("downgrade")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "http://a.tile.openstreetmap.org/1.png"))
        << false;
    QTest::newRow("userinfo")
        << DefaultTileTemplate
        << QUrl(QStringLiteral(
               "https://user@a.tile.openstreetmap.org/1.png"))
        << false;
    QTest::newRow("exact-port")
        << QStringLiteral(
               "https://tiles.example.test:8443/{z}.png")
        << QUrl(QStringLiteral(
               "https://tiles.example.test:8443/1.png"))
        << true;
    QTest::newRow("wrong-port")
        << QStringLiteral(
               "https://tiles.example.test:8443/{z}.png")
        << QUrl(QStringLiteral(
               "https://tiles.example.test/1.png"))
        << false;
    QTest::newRow("loopback")
        << QStringLiteral(
               "http://127.0.0.1:8080/{z}.png")
        << QUrl(QStringLiteral(
               "http://127.0.0.1:8080/1.png"))
        << true;
    QTest::newRow("loopback-wrong-host")
        << QStringLiteral(
               "http://127.0.0.1:8080/{z}.png")
        << QUrl(QStringLiteral(
               "http://localhost:8080/1.png"))
        << false;
}

void
TestMapPageSecurityPolicy::
tileEndpointRestrictsImageOrigins()
{
    QFETCH(QString, tileTemplate);
    QFETCH(QUrl, requestUrl);
    QFETCH(bool, allowed);
    const TileEndpoint endpoint =
        MapPageSecurityPolicy::tileEndpoint(tileTemplate);
    QVERIFY(endpoint.isValid());

    QCOMPARE(endpoint.allowsImage(requestUrl), allowed);
}

void
TestMapPageSecurityPolicy::
requestPolicyAllowsOnlyLocalAssetsAndTiles_data()
{
    QTest::addColumn<ResourceType>("resourceType");
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<bool>("allowed");

    QTest::newRow("qrc-script")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "qrc:/web/leaflet/leaflet.js"))
        << true;
    QTest::newRow("qrc-webchannel-script")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "qrc:/qtwebchannel/qwebchannel.js"))
        << true;
    QTest::newRow("qrc-style")
        << ResourceType::StyleSheet
        << QUrl(QStringLiteral(
               "qrc:/web/leaflet/leaflet.css"))
        << true;
    QTest::newRow("qrc-image")
        << ResourceType::Image
        << QUrl(QStringLiteral(
               "qrc:/images/maps/finish.png"))
        << true;
    QTest::newRow("data-main-frame")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral("data:text/html,"))
        << false;
    QTest::newRow("qrc-map-main-frame")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral("qrc:/web/ride-map"))
        << true;
    QTest::newRow("qrc-other-main-frame")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral("qrc:/web/other"))
        << false;
    QTest::newRow("qrc-main-frame-with-query")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral(
               "qrc:/web/ride-map?unexpected=true"))
        << false;
    QTest::newRow("about-blank")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral("about:blank"))
        << false;
    QTest::newRow("tile-image")
        << ResourceType::Image
        << QUrl(QStringLiteral(
               "https://a.tile.openstreetmap.org/1/2/3.png"))
        << true;
    QTest::newRow("tile-script")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "https://a.tile.openstreetmap.org/script.js"))
        << false;
    QTest::newRow("remote-script")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "https://unpkg.com/leaflet.js"))
        << false;
    QTest::newRow("qrc-script-with-authority")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "qrc://attacker.invalid/web/leaflet/leaflet.js"))
        << false;
    QTest::newRow("qrc-script-with-query")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "qrc:/web/leaflet/leaflet.js?unexpected=true"))
        << false;
    QTest::newRow("qrc-script-with-fragment")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "qrc:/web/leaflet/leaflet.js#unexpected"))
        << false;
    QTest::newRow("unrelated-image")
        << ResourceType::Image
        << QUrl(QStringLiteral(
               "https://attacker.invalid/image.png"))
        << false;
    QTest::newRow("data-image")
        << ResourceType::Image
        << QUrl(QStringLiteral(
               "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP"))
        << true;
    QTest::newRow("data-script")
        << ResourceType::Script
        << QUrl(QStringLiteral(
               "data:text/javascript,alert(1)"))
        << false;
    QTest::newRow("external-navigation")
        << ResourceType::MainFrame
        << QUrl(QStringLiteral(
               "https://a.tile.openstreetmap.org/"))
        << false;
    QTest::newRow("local-file")
        << ResourceType::Image
        << QUrl::fromLocalFile(
               QStringLiteral("/tmp/private.png"))
        << false;
}

void
TestMapPageSecurityPolicy::
requestPolicyAllowsOnlyLocalAssetsAndTiles()
{
    QFETCH(ResourceType, resourceType);
    QFETCH(QUrl, url);
    QFETCH(bool, allowed);
    const TileEndpoint endpoint =
        MapPageSecurityPolicy::tileEndpoint(
            DefaultTileTemplate);
    QVERIFY(endpoint.isValid());

    QCOMPARE(
        MapPageSecurityPolicy::allowsRequest(
            resourceType, url, endpoint),
        allowed);
}

void
TestMapPageSecurityPolicy::javaScriptStringsAreEncoded_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("plain")
        << QStringLiteral("Interval 1");
    QTest::newRow("quotes-and-slashes")
        << QStringLiteral("'\"\\/\r\n\t");
    QTest::newRow("script-breakout")
        << QStringLiteral(
               "</script><script>alert('x')</script>");
    QTest::newRow("html-special")
        << QStringLiteral("<>&");
    QTest::newRow("line-separators")
        << (QStringLiteral("before")
            + QChar(0x2028)
            + QChar(0x2029)
            + QStringLiteral("after"));
    QTest::newRow("nul")
        << (QStringLiteral("before")
            + QChar(0)
            + QStringLiteral("after"));
}

void
TestMapPageSecurityPolicy::javaScriptStringsAreEncoded()
{
    QFETCH(QString, input);

    const QString encoded =
        MapPageSecurityPolicy::javaScriptStringLiteral(input);
    QVERIFY(encoded.startsWith(QLatin1Char('"')));
    QVERIFY(encoded.endsWith(QLatin1Char('"')));
    QVERIFY(!encoded.contains(QLatin1Char('<')));
    QVERIFY(!encoded.contains(QLatin1Char('>')));
    QVERIFY(!encoded.contains(QLatin1Char('&')));
    QVERIFY(!encoded.contains(QChar(0x2028)));
    QVERIFY(!encoded.contains(QChar(0x2029)));

    QJsonParseError error;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(
            (QByteArrayLiteral("[")
             + encoded.toUtf8()
             + QByteArrayLiteral("]")),
            &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(parsed.isArray());
    QCOMPARE(parsed.array().size(), 1);
    QCOMPARE(parsed.array().at(0).toString(), input);
}

void
TestMapPageSecurityPolicy::contentSecurityPolicyIsNarrow()
{
    const TileEndpoint endpoint =
        MapPageSecurityPolicy::tileEndpoint(
            DefaultTileTemplate);
    QVERIFY(endpoint.isValid());
    const QString nonce =
        QStringLiteral("0123456789abcdef0123456789abcdef");

    const QString policy =
        MapPageSecurityPolicy::contentSecurityPolicy(
            nonce, endpoint);

    QVERIFY(!policy.isEmpty());
    QVERIFY(policy.contains(
        QStringLiteral("default-src 'none'")));
    QCOMPARE(
        scriptDirective(policy),
        QStringLiteral(
            "script-src qrc: "
            "'nonce-0123456789abcdef0123456789abcdef'"));
    QVERIFY(!scriptDirective(policy).contains(
        QStringLiteral("'unsafe-inline'")));
    QVERIFY(policy.contains(
        QStringLiteral(
            "img-src qrc: data: "
            "https://*.tile.openstreetmap.org")));
    QVERIFY(policy.contains(
        QStringLiteral("object-src 'none'")));
    QVERIFY(policy.contains(
        QStringLiteral("frame-src 'none'")));
    QVERIFY(policy.contains(
        QStringLiteral("worker-src 'none'")));
    QVERIFY(policy.contains(
        QStringLiteral("base-uri 'none'")));
    QVERIFY(policy.contains(
        QStringLiteral("form-action 'none'")));
    QVERIFY(!policy.contains(
        QStringLiteral("unpkg.com")));
    QVERIFY(!policy.contains(
        QStringLiteral("maps.googleapis.com")));
}

void
TestMapPageSecurityPolicy::invalidCspInputsFailClosed_data()
{
    QTest::addColumn<QString>("nonce");
    QTest::addColumn<QString>("tileTemplate");

    QTest::newRow("empty-nonce")
        << QString() << DefaultTileTemplate;
    QTest::newRow("short-nonce")
        << QStringLiteral("short") << DefaultTileTemplate;
    QTest::newRow("nonce-injection")
        << QStringLiteral(
               "0123456789abcdef'; script-src *")
        << DefaultTileTemplate;
    QTest::newRow("invalid-endpoint")
        << QStringLiteral(
               "0123456789abcdef0123456789abcdef")
        << QStringLiteral(
               "http://tiles.example.test/{z}.png");
}

void
TestMapPageSecurityPolicy::invalidCspInputsFailClosed()
{
    QFETCH(QString, nonce);
    QFETCH(QString, tileTemplate);
    const TileEndpoint endpoint =
        MapPageSecurityPolicy::tileEndpoint(tileTemplate);

    QVERIFY(
        MapPageSecurityPolicy::contentSecurityPolicy(
            nonce, endpoint).isEmpty());
}

void
TestMapPageSecurityPolicy::leafletAssetsArePackagedAndPinned()
{
    QCOMPARE(
        MapPageSecurityPolicy::leafletScriptUrl(),
        QStringLiteral("qrc:/web/leaflet/leaflet.js"));
    QCOMPARE(
        MapPageSecurityPolicy::leafletStyleSheetUrl(),
        QStringLiteral("qrc:/web/leaflet/leaflet.css"));

    const QByteArray script =
        resourceContents(
            QStringLiteral(":/web/leaflet/leaflet.js"));
    const QByteArray style =
        resourceContents(
            QStringLiteral(":/web/leaflet/leaflet.css"));
    const QByteArray license =
        resourceContents(
            QStringLiteral(":/web/leaflet/LICENSE"));

    QVERIFY(!script.isEmpty());
    QVERIFY(!style.isEmpty());
    QVERIFY(license.contains(
        "Copyright (c) 2010-2023, Volodymyr Agafonkin"));
    QCOMPARE(
        QCryptographicHash::hash(
            script, QCryptographicHash::Sha256).toBase64(),
        QByteArrayLiteral(
            "20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo="));
    QCOMPARE(
        QCryptographicHash::hash(
            style, QCryptographicHash::Sha256).toBase64(),
        QByteArrayLiteral(
            "p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY="));
}

void
TestMapPageSecurityPolicy::mapPageIntegrationKeepsScriptsLocal()
{
    const QString sourcePath = QFINDTESTDATA(
        "../../../src/Charts/RideMapWindow.cpp");
    QVERIFY2(!sourcePath.isEmpty(),
             "RideMapWindow.cpp test data was not found");
    const QByteArray source = resourceContents(sourcePath);
    QVERIFY(!source.isEmpty());

    QVERIFY(!source.contains("http://maps.googleapis.com"));
    QVERIFY(!source.contains("https://unpkg.com"));
    QVERIFY(!source.contains(
        "mapCombo->addItem(tr(\"Google\"))"));
    QVERIFY(!source.contains(".arg(x->name)"));
    QVERIFY(source.contains("new QWebEngineProfile(this)"));
    QVERIFY(source.contains("NoPersistentCookies"));
    QVERIFY(source.contains("setUrlRequestInterceptor"));
    QVERIFY(source.contains("leafletScriptUrl"));
    QVERIFY(source.contains("nonce=\\\"%1\\\""));
    QVERIFY(source.count("javaScriptStringLiteral") >= 3);
    QVERIFY(source.contains("currentPage, MapDocumentUrl"));
    QVERIFY(source.contains(
        "if (view) delete view->page();"));
}

QTEST_MAIN(TestMapPageSecurityPolicy)
#include "testMapPageSecurityPolicy.moc"
