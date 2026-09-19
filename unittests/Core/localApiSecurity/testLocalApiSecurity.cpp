#include "Core/LocalApiSecurityPolicy.h"
#include "httprequest.h"

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

namespace {

QByteArray validToken()
{
    return QByteArray(43, 'A');
}

QMultiMap<QByteArray, QByteArray> requestHeaders(
    const QByteArray &host,
    const QByteArray &origin,
    const QByteArray &authorization,
    const QByteArray &hostName = QByteArrayLiteral("Host"),
    const QByteArray &originName = QByteArrayLiteral("Origin"),
    const QByteArray &authorizationName = QByteArrayLiteral("Authorization"))
{
    QMultiMap<QByteArray, QByteArray> headers;
    if (!host.isNull()) {
        headers.insert(hostName, host);
    }
    if (!origin.isNull()) {
        headers.insert(originName, origin);
    }
    if (!authorization.isNull()) {
        headers.insert(authorizationName, authorization);
    }
    return headers;
}

QByteArray bearer(const QByteArray &token)
{
    return QByteArrayLiteral("Bearer ") + token;
}

} // namespace

class TestLocalApiSecurity : public QObject
{
    Q_OBJECT

private slots:
    void generatedTokensAreStrongAndUnique();
    void secureConfigurationOverridesHostAndPersistsToken();
    void invalidStoredTokenIsRotated();
    void legacyMalformedCommentStillMigratesSecurely();
    void rejectsDanglingSettingsSymlinkWithoutWritingTarget();
    void rejectsInvalidPorts_data();
    void rejectsInvalidPorts();
    void acceptsAuthenticatedLoopbackRequests_data();
    void acceptsAuthenticatedLoopbackRequests();
    void rejectsUntrustedHosts_data();
    void rejectsUntrustedHosts();
    void rejectsUntrustedOrigins_data();
    void rejectsUntrustedOrigins();
    void rejectsMissingOrInvalidAuthorization_data();
    void rejectsMissingOrInvalidAuthorization();
    void rejectsDuplicateSecurityHeaders();
    void invalidExpectedTokenFailsClosed();
    void rejectsUnsafePathComponents_data();
    void rejectsUnsafePathComponents();
    void acceptsPortablePathComponents_data();
    void acceptsPortablePathComponents();
    void rejectsUnsafeDecodedPaths_data();
    void rejectsUnsafeDecodedPaths();
    void splitsPortableDecodedPaths();
    void httpParserDecodesPathSeparatorsAndNul();
};

void TestLocalApiSecurity::generatedTokensAreStrongAndUnique()
{
    const QByteArray first =
        LocalApiSecurityPolicy::generateBearerToken();
    const QByteArray second =
        LocalApiSecurityPolicy::generateBearerToken();

    QCOMPARE(first.size(), 43);
    QCOMPARE(second.size(), 43);
    QVERIFY(LocalApiSecurityPolicy::isWellFormedBearerToken(first));
    QVERIFY(LocalApiSecurityPolicy::isWellFormedBearerToken(second));
    QVERIFY(first != second);
}

void TestLocalApiSecurity::secureConfigurationOverridesHostAndPersistsToken()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("httpserver.ini"));
    const QByteArray defaultSettings =
        QByteArrayLiteral(
            "port=12021\n"
            "host=0.0.0.0\n");
    QString initializationError;
    QVERIFY2(
        LocalApiSecurityPolicy::ensureSettingsFile(
            path, defaultSettings, &initializationError),
        qPrintable(initializationError));

    QSettings settings(path, QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("host")).toString(),
             QStringLiteral("0.0.0.0"));
    QCOMPARE(settings.value(QStringLiteral("port")).toInt(), 12021);
    QCOMPARE(settings.status(), QSettings::NoError);

    QString error;
    const LocalApiSecurityPolicy::Configuration first =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);

    QVERIFY2(first.isValid(), qPrintable(error));
    QCOMPARE(first.port, quint16(12021));
    QVERIFY(LocalApiSecurityPolicy::isWellFormedBearerToken(
        first.bearerToken));
    QCOMPARE(settings.value(QStringLiteral("host")).toString(),
             QStringLiteral("127.0.0.1"));
    QCOMPARE(settings.value(
                 QStringLiteral("security/bearerToken")).toByteArray(),
             first.bearerToken);

    const LocalApiSecurityPolicy::Configuration second =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);
    QVERIFY2(second.isValid(), qPrintable(error));
    QCOMPARE(second.bearerToken, first.bearerToken);

#ifdef Q_OS_UNIX
    const QFileDevice::Permissions permissions =
        QFileInfo(path).permissions();
    const QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup
        | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup
        | QFileDevice::ReadOther
        | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    QCOMPARE(permissions & nonOwnerPermissions,
             QFileDevice::Permissions());
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
#endif
}

void TestLocalApiSecurity::invalidStoredTokenIsRotated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(
        directory.filePath(QStringLiteral("httpserver.ini")),
        QSettings::IniFormat);
    settings.setValue(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    settings.setValue(QStringLiteral("port"), 12021);
    settings.setValue(
        QStringLiteral("security/bearerToken"),
        QByteArrayLiteral("short-and-predictable"));

    QString error;
    const LocalApiSecurityPolicy::Configuration configuration =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);

    QVERIFY2(configuration.isValid(), qPrintable(error));
    QVERIFY(LocalApiSecurityPolicy::isWellFormedBearerToken(
        configuration.bearerToken));
    QVERIFY(configuration.bearerToken
            != QByteArrayLiteral("short-and-predictable"));
    QCOMPARE(settings.value(
                 QStringLiteral("security/bearerToken")).toByteArray(),
             configuration.bearerToken);
}

void TestLocalApiSecurity::legacyMalformedCommentStillMigratesSecurely()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("httpserver.ini"));
    const QByteArray legacyIni =
        QByteArrayLiteral(
            "//configfile.ini\n"
            "port=12021\n"
            "host=0.0.0.0\n");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(legacyIni), qint64(legacyIni.size()));
    file.close();

    QSettings settings(path, QSettings::IniFormat);
    QString error;
    const LocalApiSecurityPolicy::Configuration configuration =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);

    QVERIFY2(configuration.isValid(), qPrintable(error));
    QCOMPARE(configuration.port, quint16(12021));
    QVERIFY(LocalApiSecurityPolicy::isWellFormedBearerToken(
        configuration.bearerToken));
    QCOMPARE(settings.value(QStringLiteral("host")).toString(),
             QStringLiteral("127.0.0.1"));
}

void TestLocalApiSecurity::rejectsDanglingSettingsSymlinkWithoutWritingTarget()
{
#ifndef Q_OS_UNIX
    QSKIP("Symbolic-link semantics are platform-specific.");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString targetPath =
        directory.filePath(QStringLiteral("target.ini"));
    const QString linkPath =
        directory.filePath(QStringLiteral("httpserver.ini"));
    const QByteArray initialSettings =
        QByteArrayLiteral("port=12021\n");

    QFile target(targetPath);
    QVERIFY(target.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(target.write(initialSettings),
             qint64(initialSettings.size()));
    target.close();
    QVERIFY(QFile::link(targetPath, linkPath));

    QSettings settings(linkPath, QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("port")).toInt(), 12021);
    QVERIFY(QFile::remove(targetPath));

    const QFileInfo danglingLink(linkPath);
    QVERIFY(danglingLink.isSymLink());
    QVERIFY(!danglingLink.exists());

    QString initializationError;
    QVERIFY(!LocalApiSecurityPolicy::ensureSettingsFile(
        linkPath,
        initialSettings,
        &initializationError));
    QVERIFY(!initializationError.isEmpty());
    QVERIFY(!QFileInfo(targetPath).exists());

    QString error;
    const LocalApiSecurityPolicy::Configuration configuration =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);

    QVERIFY(!configuration.isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo(targetPath).exists());
#endif
}

void TestLocalApiSecurity::rejectsInvalidPorts_data()
{
    QTest::addColumn<QVariant>("port");

    QTest::newRow("missing") << QVariant();
    QTest::newRow("zero") << QVariant(0);
    QTest::newRow("negative") << QVariant(-1);
    QTest::newRow("too-large") << QVariant(65536);
    QTest::newRow("not-numeric")
        << QVariant(QStringLiteral("not-a-port"));
    QTest::newRow("numeric-suffix")
        << QVariant(QStringLiteral("12021suffix"));
}

void TestLocalApiSecurity::rejectsInvalidPorts()
{
    QFETCH(QVariant, port);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(
        directory.filePath(QStringLiteral("httpserver.ini")),
        QSettings::IniFormat);
    if (port.isValid()) {
        settings.setValue(QStringLiteral("port"), port);
    }

    QString error;
    const LocalApiSecurityPolicy::Configuration configuration =
        LocalApiSecurityPolicy::prepareServerConfiguration(settings, &error);

    QVERIFY(!configuration.isValid());
    QVERIFY(!error.isEmpty());
}

void TestLocalApiSecurity::acceptsAuthenticatedLoopbackRequests_data()
{
    QTest::addColumn<QByteArray>("host");
    QTest::addColumn<QByteArray>("origin");
    QTest::addColumn<QByteArray>("hostName");
    QTest::addColumn<QByteArray>("originName");
    QTest::addColumn<QByteArray>("authorizationName");
    QTest::addColumn<QByteArray>("scheme");

    QTest::newRow("localhost-cli")
        << QByteArrayLiteral("localhost:12021")
        << QByteArray()
        << QByteArrayLiteral("Host")
        << QByteArrayLiteral("Origin")
        << QByteArrayLiteral("Authorization")
        << QByteArrayLiteral("Bearer ");
    QTest::newRow("ipv4-browser")
        << QByteArrayLiteral("127.0.0.1:12021")
        << QByteArrayLiteral("http://127.0.0.1:12021")
        << QByteArrayLiteral("Host")
        << QByteArrayLiteral("Origin")
        << QByteArrayLiteral("Authorization")
        << QByteArrayLiteral("Bearer ");
    QTest::newRow("ipv6-browser")
        << QByteArrayLiteral("[::1]:12021")
        << QByteArrayLiteral("http://[::1]:12021")
        << QByteArrayLiteral("Host")
        << QByteArrayLiteral("Origin")
        << QByteArrayLiteral("Authorization")
        << QByteArrayLiteral("Bearer ");
    QTest::newRow("case-insensitive-headers-and-scheme")
        << QByteArrayLiteral("LOCALHOST:12021")
        << QByteArrayLiteral("HTTP://LOCALHOST:12021")
        << QByteArrayLiteral("hOsT")
        << QByteArrayLiteral("oRiGiN")
        << QByteArrayLiteral("aUtHoRiZaTiOn")
        << QByteArrayLiteral("bearer ");
}

void TestLocalApiSecurity::acceptsAuthenticatedLoopbackRequests()
{
    QFETCH(QByteArray, host);
    QFETCH(QByteArray, origin);
    QFETCH(QByteArray, hostName);
    QFETCH(QByteArray, originName);
    QFETCH(QByteArray, authorizationName);
    QFETCH(QByteArray, scheme);
    const QByteArray token = validToken();
    const QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(
            host, origin, scheme + token,
            hostName, originName, authorizationName);

    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::Allow);
}

void TestLocalApiSecurity::rejectsUntrustedHosts_data()
{
    QTest::addColumn<QByteArray>("host");

    QTest::newRow("missing") << QByteArray();
    QTest::newRow("empty") << QByteArrayLiteral("");
    QTest::newRow("attacker")
        << QByteArrayLiteral("attacker.invalid:12021");
    QTest::newRow("lookalike")
        << QByteArrayLiteral("localhost.attacker.invalid:12021");
    QTest::newRow("trailing-dot")
        << QByteArrayLiteral("localhost.:12021");
    QTest::newRow("external-address")
        << QByteArrayLiteral("192.0.2.10:12021");
    QTest::newRow("wrong-port")
        << QByteArrayLiteral("localhost:12022");
    QTest::newRow("missing-port")
        << QByteArrayLiteral("localhost");
    QTest::newRow("userinfo")
        << QByteArrayLiteral("attacker@localhost:12021");
    QTest::newRow("path")
        << QByteArrayLiteral("localhost:12021/athlete");
}

void TestLocalApiSecurity::rejectsUntrustedHosts()
{
    QFETCH(QByteArray, host);
    const QByteArray token = validToken();
    const QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(host, QByteArray(), bearer(token));

    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectHost);
}

void TestLocalApiSecurity::rejectsUntrustedOrigins_data()
{
    QTest::addColumn<QByteArray>("origin");

    QTest::newRow("attacker")
        << QByteArrayLiteral("https://attacker.invalid");
    QTest::newRow("null") << QByteArrayLiteral("null");
    QTest::newRow("https-loopback")
        << QByteArrayLiteral("https://localhost:12021");
    QTest::newRow("wrong-port")
        << QByteArrayLiteral("http://localhost:12022");
    QTest::newRow("trailing-slash")
        << QByteArrayLiteral("http://localhost:12021/");
    QTest::newRow("userinfo")
        << QByteArrayLiteral("http://attacker@localhost:12021");
    QTest::newRow("path")
        << QByteArrayLiteral("http://localhost:12021/athlete");
    QTest::newRow("multiple")
        << QByteArrayLiteral(
            "http://localhost:12021 https://attacker.invalid");
}

void TestLocalApiSecurity::rejectsUntrustedOrigins()
{
    QFETCH(QByteArray, origin);
    const QByteArray token = validToken();
    const QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(
            QByteArrayLiteral("localhost:12021"),
            origin,
            bearer(token));

    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectOrigin);
}

void TestLocalApiSecurity::rejectsMissingOrInvalidAuthorization_data()
{
    QTest::addColumn<QByteArray>("authorization");

    QTest::newRow("missing") << QByteArray();
    QTest::newRow("empty") << QByteArrayLiteral("");
    QTest::newRow("basic")
        << QByteArrayLiteral("Basic dXNlcjpwYXNz");
    QTest::newRow("wrong-token")
        << bearer(QByteArray(43, 'B'));
    QTest::newRow("double-space")
        << QByteArrayLiteral("Bearer  ") + validToken();
    QTest::newRow("missing-space")
        << QByteArrayLiteral("Bearer") + validToken();
    QTest::newRow("token-suffix")
        << bearer(validToken() + QByteArrayLiteral("A"));
}

void TestLocalApiSecurity::rejectsMissingOrInvalidAuthorization()
{
    QFETCH(QByteArray, authorization);
    const QByteArray token = validToken();
    const QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(
            QByteArrayLiteral("localhost:12021"),
            QByteArray(),
            authorization);

    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectAuthorization);
}

void TestLocalApiSecurity::rejectsDuplicateSecurityHeaders()
{
    const QByteArray token = validToken();

    QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(
            QByteArrayLiteral("localhost:12021"),
            QByteArray(),
            bearer(token));
    headers.insert(
        QByteArrayLiteral("host"),
        QByteArrayLiteral("localhost:12021"));
    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectHost);

    headers = requestHeaders(
        QByteArrayLiteral("localhost:12021"),
        QByteArrayLiteral("http://localhost:12021"),
        bearer(token));
    headers.insert(
        QByteArrayLiteral("origin"),
        QByteArrayLiteral("http://localhost:12021"));
    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectOrigin);

    headers = requestHeaders(
        QByteArrayLiteral("localhost:12021"),
        QByteArray(),
        bearer(token));
    headers.insert(
        QByteArrayLiteral("authorization"),
        bearer(token));
    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, token, quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectAuthorization);
}

void TestLocalApiSecurity::invalidExpectedTokenFailsClosed()
{
    const QMultiMap<QByteArray, QByteArray> headers =
        requestHeaders(
            QByteArrayLiteral("localhost:12021"),
            QByteArray(),
            bearer(validToken()));

    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, QByteArray(), quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectAuthorization);
    QCOMPARE(
        LocalApiSecurityPolicy::evaluateRequest(
            headers, QByteArrayLiteral("short"), quint16(12021)),
        LocalApiSecurityPolicy::Decision::RejectAuthorization);
}

void TestLocalApiSecurity::rejectsUnsafePathComponents_data()
{
    QTest::addColumn<QString>("component");

    QTest::newRow("empty") << QString();
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("dot-dot") << QStringLiteral("..");
    QTest::newRow("slash") << QStringLiteral("ride/next");
    QTest::newRow("backslash") << QStringLiteral("ride\\next");
    QTest::newRow("absolute") << QStringLiteral("/etc/passwd");
    QTest::newRow("drive-or-stream") << QStringLiteral("C:ride.fit");
    QTest::newRow("trailing-dot") << QStringLiteral("ride.fit.");
    QTest::newRow("trailing-space") << QStringLiteral("ride.fit ");
    QTest::newRow("nul") << QString(QChar(u'r')) + QChar(0) + QStringLiteral("ide");
    QTest::newRow("control") << QString(QChar(0x1f));
    QTest::newRow("reserved-con") << QStringLiteral("CON");
    QTest::newRow("reserved-nul-suffix") << QStringLiteral("nul.json");
    QTest::newRow("reserved-lpt") << QStringLiteral("LPT9.fit");
    QTest::newRow("overlong") << QString(241, QLatin1Char('a'));
    QTest::newRow("encoded-slash") << QStringLiteral("ride%2fsecret");
    QTest::newRow("encoded-backslash") << QStringLiteral("ride%5Csecret");
    QTest::newRow("encoded-dot") << QStringLiteral("%2e%2e");
    QTest::newRow("encoded-nul") << QStringLiteral("ride%00name");
}

void TestLocalApiSecurity::rejectsUnsafePathComponents()
{
    QFETCH(QString, component);
    QVERIFY(!LocalApiSecurityPolicy::isSafePathComponent(component));
}

void TestLocalApiSecurity::acceptsPortablePathComponents_data()
{
    QTest::addColumn<QString>("component");

    QTest::newRow("athlete") << QStringLiteral("athlete-one");
    QTest::newRow("activity") << QStringLiteral("2026_09_19_12_00_00.fit");
    QTest::newRow("unicode") << QStringLiteral("Järvinen");
    QTest::newRow("literal-percent") << QStringLiteral("100% effort");
}

void TestLocalApiSecurity::acceptsPortablePathComponents()
{
    QFETCH(QString, component);
    QVERIFY(LocalApiSecurityPolicy::isSafePathComponent(component));
}

void TestLocalApiSecurity::rejectsUnsafeDecodedPaths_data()
{
    QTest::addColumn<QString>("path");

    QTest::newRow("internal-empty") << QStringLiteral("/athlete//activity/file.fit");
    QTest::newRow("dot-athlete") << QStringLiteral("/../activity/file.fit");
    QTest::newRow("windows-traversal") << QStringLiteral("/athlete/activity/..\\secret.fit");
    QTest::newRow("decoded-slash") << QStringLiteral("/athlete/activity/../secret.fit");
    QTest::newRow("double-encoded-slash") << QStringLiteral("/athlete/activity/%2fsecret.fit");
    QTest::newRow("embedded-nul")
        << (QStringLiteral("/athlete/activity/ride")
            + QChar(0) + QStringLiteral(".fit"));
}

void TestLocalApiSecurity::rejectsUnsafeDecodedPaths()
{
    QFETCH(QString, path);
    QStringList components;
    QVERIFY(!LocalApiSecurityPolicy::splitAndValidateDecodedPath(
        path, components));
    QVERIFY(components.isEmpty());
}

void TestLocalApiSecurity::splitsPortableDecodedPaths()
{
    QStringList components;
    QVERIFY(LocalApiSecurityPolicy::splitAndValidateDecodedPath(
        QStringLiteral("/athlete/activity/2026_09_19.fit/"),
        components));
    QCOMPARE(components, QStringList({
        QStringLiteral("athlete"),
        QStringLiteral("activity"),
        QStringLiteral("2026_09_19.fit")}));

    QVERIFY(LocalApiSecurityPolicy::splitAndValidateDecodedPath(
        QStringLiteral("/"), components));
    QVERIFY(components.isEmpty());
}

void TestLocalApiSecurity::httpParserDecodesPathSeparatorsAndNul()
{
    QCOMPARE(HttpRequest::urlDecode(
                 QByteArrayLiteral("/athlete/activity/..%5csecret.fit")),
             QByteArray("/athlete/activity/..\\secret.fit"));
    QCOMPARE(HttpRequest::urlDecode(
                 QByteArrayLiteral("/athlete/activity/a%2fb.fit")),
             QByteArray("/athlete/activity/a/b.fit"));
    QCOMPARE(HttpRequest::urlDecode(
                 QByteArrayLiteral("/athlete/activity/a%00b.fit")),
             QByteArray("/athlete/activity/a\0b.fit", 25));
}

QTEST_MAIN(TestLocalApiSecurity)
#include "testLocalApiSecurity.moc"
