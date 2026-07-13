#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>

#include "Cloud/CloudCredentialTransport.h"

namespace {

QByteArray sourceContents(const char *relativePath)
{
    const QString path = QFINDTESTDATA(relativePath);
    if (path.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

class TestCredentialTransportSafety : public QObject
{
    Q_OBJECT

private slots:
    void withingsRequestKeepsTokenOutOfUrl();
    void rideWithGpsRequestKeepsCredentialsOutOfUrl();
    void rideWithGpsTokenParser_data();
    void rideWithGpsTokenParser();
    void productionSourcesDoNotExposeCredentials();
};

void TestCredentialTransportSafety::withingsRequestKeepsTokenOutOfUrl()
{
    const QUrl endpoint(
        QStringLiteral("https://wbsapi.withings.net/measure"));
    const QString accessToken =
        QStringLiteral("sec012-withings-access-token");
    const QDateTime from(
        QDate(2026, 7, 1), QTime(1, 2, 3), QTimeZone::UTC);
    const QDateTime to(
        QDate(2026, 7, 2), QTime(4, 5, 6), QTimeZone::UTC);

    const CloudCredentialTransport::Request transport =
        CloudCredentialTransport::makeWithingsMeasuresRequest(
            endpoint, accessToken, from, to);

    QCOMPARE(transport.request.url(), endpoint);
    QVERIFY(!transport.request.url().toEncoded().contains(
        accessToken.toUtf8()));
    QCOMPARE(
        transport.request.rawHeader("Authorization"),
        QByteArrayLiteral("Bearer ") + accessToken.toUtf8());
    QCOMPARE(
        transport.request.rawHeader("Content-Type"),
        QByteArrayLiteral("application/x-www-form-urlencoded"));

    const QUrlQuery body(QString::fromUtf8(transport.body));
    QCOMPARE(
        body.queryItemValue(QStringLiteral("action")),
        QStringLiteral("getmeas"));
    QCOMPARE(
        body.queryItemValue(QStringLiteral("startdate")),
        QString::number(from.toSecsSinceEpoch()));
    QCOMPARE(
        body.queryItemValue(QStringLiteral("enddate")),
        QString::number(to.toSecsSinceEpoch()));
}

void TestCredentialTransportSafety::
rideWithGpsRequestKeepsCredentialsOutOfUrl()
{
    const QUrl endpoint(
        QStringLiteral(
            "https://ridewithgps.com/api/v1/auth_tokens"));
    const QString apiKey =
        QStringLiteral("sec012-rwgps-api-key");
    const QString email =
        QStringLiteral("sec012+user@example.invalid");
    const QString password =
        QStringLiteral("sec012-password+&?");

    const CloudCredentialTransport::Request transport =
        CloudCredentialTransport::makeRideWithGpsAuthTokenRequest(
            endpoint, apiKey, email, password);

    QCOMPARE(transport.request.url(), endpoint);
    const QByteArray encodedUrl =
        transport.request.url().toEncoded();
    QVERIFY(!encodedUrl.contains(apiKey.toUtf8()));
    QVERIFY(!encodedUrl.contains(email.toUtf8()));
    QVERIFY(!encodedUrl.contains(password.toUtf8()));
    QCOMPARE(
        transport.request.rawHeader("x-rwgps-api-key"),
        apiKey.toUtf8());
    QCOMPARE(
        transport.request.rawHeader("Content-Type"),
        QByteArrayLiteral("application/json"));

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(transport.body, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject user =
        document.object().value(QStringLiteral("user")).toObject();
    QCOMPARE(
        user.value(QStringLiteral("email")).toString(),
        email);
    QCOMPARE(
        user.value(QStringLiteral("password")).toString(),
        password);
}

void TestCredentialTransportSafety::rideWithGpsTokenParser_data()
{
    QTest::addColumn<QJsonObject>("response");
    QTest::addColumn<QString>("expected");

    QTest::newRow("v1")
        << QJsonObject{
               {QStringLiteral("auth_token"),
                QJsonObject{
                    {QStringLiteral("auth_token"),
                     QStringLiteral("sec012-v1-token")}}}}
        << QStringLiteral("sec012-v1-token");
    QTest::newRow("legacy")
        << QJsonObject{
               {QStringLiteral("user"),
                QJsonObject{
                    {QStringLiteral("auth_token"),
                     QStringLiteral("sec012-legacy-token")}}}}
        << QStringLiteral("sec012-legacy-token");
    QTest::newRow("missing")
        << QJsonObject{
               {QStringLiteral("auth_token"),
                QJsonObject{
                    {QStringLiteral("api_key"),
                     QStringLiteral("not-a-user-token")}}}}
        << QString();
}

void TestCredentialTransportSafety::rideWithGpsTokenParser()
{
    QFETCH(QJsonObject, response);
    QFETCH(QString, expected);

    QCOMPARE(
        CloudCredentialTransport::rideWithGpsAuthToken(
            response),
        expected);
}

void TestCredentialTransportSafety::
productionSourcesDoNotExposeCredentials()
{
    const QByteArray azum = sourceContents(
        "../../../src/Cloud/Azum.cpp");
    const QByteArray oauth = sourceContents(
        "../../../src/Cloud/OAuthDialog.cpp");
    const QByteArray rideWithGps = sourceContents(
        "../../../src/Cloud/RideWithGPS.cpp");
    const QByteArray withings = sourceContents(
        "../../../src/Cloud/WithingsDownload.cpp");

    QVERIFY(!azum.isEmpty());
    QVERIFY(!oauth.isEmpty());
    QVERIFY(!rideWithGps.isEmpty());
    QVERIFY(!withings.isEmpty());

    QVERIFY(!azum.contains("<< token"));
    QVERIFY(!azum.contains("Got response: %s"));

    QVERIFY(!oauth.contains(
        "https://ridewithgps.com/users/current.json"));
    QVERIFY(!oauth.contains("tokenUrl.setQuery(data)"));
    QVERIFY(oauth.contains(
        "makeRideWithGpsAuthTokenRequest"));

    QVERIFY(!rideWithGps.contains("printd(\"out:%s"));
    QVERIFY(!rideWithGps.contains("printd(\"reply:%s"));

    QVERIFY(!withings.contains(
        "params.addQueryItem(\"access_token\""));
    QVERIFY(!withings.contains(
        "postData.toString().toStdString().c_str()"));
    QVERIFY(!withings.contains(
        "printd(\"response: %s"));
    QVERIFY(withings.contains(
        "makeWithingsMeasuresRequest"));
}

QTEST_APPLESS_MAIN(TestCredentialTransportSafety)

#include "testCredentialTransportSafety.moc"
