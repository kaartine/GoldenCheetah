#include "Train/StravaRoutesClient.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTest>
#include <QUrlQuery>

#include <functional>

namespace {

using HttpResult = StravaRoutesClient::HttpResult;
using TokenResult = StravaRoutesClient::TokenResult;

HttpResult success(
    const QByteArray &payload,
    const QString &contentType = QStringLiteral("application/json"))
{
    HttpResult result;
    result.httpStatus = 200;
    result.payload = payload;
    result.contentType = contentType;
    return result;
}

HttpResult httpFailure(
    int status,
    QNetworkReply::NetworkError networkError)
{
    HttpResult result;
    result.httpStatus = status;
    result.networkError = networkError;
    result.networkErrorString =
        QStringLiteral("Synthetic provider failure");
    result.payload = QByteArrayLiteral(
        R"json({"message":"Authorization Error","errors":[]})json");
    result.contentType = QStringLiteral("application/json");
    return result;
}

QByteArray athletePayload()
{
    return QByteArrayLiteral(R"json({"id":424242})json");
}

QJsonObject routeObject(
    const QString &id,
    const QString &name = QStringLiteral("Test route"))
{
    return {
        {QStringLiteral("id_str"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("description"),
         QStringLiteral("Synthetic route")}
    };
}

QByteArray routesPayload(
    int count,
    quint64 firstId = 1000)
{
    QJsonArray routes;
    for (int index = 0; index < count; ++index) {
        routes.append(routeObject(
            QString::number(firstId + quint64(index)),
            QStringLiteral("Route %1").arg(index)));
    }
    return QJsonDocument(routes).toJson(QJsonDocument::Compact);
}

TokenResult token(const QString &value)
{
    TokenResult result;
    result.accessToken = value;
    return result;
}

} // namespace

class TestStravaRoutesClient : public QObject
{
    Q_OBJECT

private slots:
    void refreshesBeforeRequestsAndReusesAccessToken();
    void paginatesAndPreserves64BitRouteIds();
    void refreshesOnceAfterMidPaginationUnauthorized();
    void doesNotRetryForbidden();
    void doesNotRetryASecondUnauthorized();
    void reportsTimeoutAndCancellation_data();
    void reportsTimeoutAndCancellation();
    void observesCancellationBetweenPages();
    void rejectsMalformedResponses_data();
    void rejectsMalformedResponses();
    void rejectsOversizedResponses();
    void validatesRouteIdBeforeGpxRequest_data();
    void validatesRouteIdBeforeGpxRequest();
    void redactsAccessTokenFromErrors();
    void productionWiringUsesSharedRefreshAndBoundedWait();
};

void TestStravaRoutesClient::
refreshesBeforeRequestsAndReusesAccessToken()
{
    int tokenCalls = 0;
    int requestCalls = 0;
    StravaRoutesClient client(
        [&](bool forceRefresh,
            const StravaRoutesClient::CancellationCheck &) {
            ++tokenCalls;
            QVERIFY(!forceRefresh);
            return token(QStringLiteral("fresh-access"));
        },
        [&](const QUrl &url,
            const QString &accessToken,
            qsizetype maximumBytes,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            QCOMPARE(accessToken, QStringLiteral("fresh-access"));
            QVERIFY(maximumBytes > 0);
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            if (url.path().contains(QStringLiteral("/athletes/")))
                return success(routesPayload(1));
            QCOMPARE(
                url.path(),
                QStringLiteral("/api/v3/routes/1000/export_gpx"));
            return success(
                QByteArrayLiteral("<gpx/>"),
                QStringLiteral("application/gpx+xml"));
        });

    const auto routes = client.listRoutes();
    QVERIFY2(routes.isValid(), qPrintable(routes.error));
    QCOMPARE(routes.routes.size(), 1);
    QCOMPARE(routes.routes.first().routeId, QStringLiteral("1000"));

    const auto gpx = client.downloadGpx(QStringLiteral("1000"));
    QVERIFY2(gpx.isValid(), qPrintable(gpx.error));
    QCOMPARE(gpx.payload, QByteArrayLiteral("<gpx/>"));
    QCOMPARE(tokenCalls, 1);
    QCOMPARE(requestCalls, 3);
}

void TestStravaRoutesClient::paginatesAndPreserves64BitRouteIds()
{
    constexpr auto LargeRouteId = "9223372036854775700";
    int pageRequests = 0;
    StravaRoutesClient client(
        [](bool,
           const StravaRoutesClient::CancellationCheck &) {
            return token(QStringLiteral("access"));
        },
        [&](const QUrl &url,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());

            ++pageRequests;
            const int page =
                QUrlQuery(url).queryItemValue(
                    QStringLiteral("page")).toInt();
            if (page == 1)
                return success(routesPayload(
                    StravaRoutesClient::PageSize));

            QJsonArray finalPage;
            finalPage.append(routeObject(
                QStringLiteral(LargeRouteId)));
            return success(
                QJsonDocument(finalPage)
                    .toJson(QJsonDocument::Compact));
        });

    const auto result = client.listRoutes();

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(
        result.routes.size(),
        StravaRoutesClient::PageSize + 1);
    QCOMPARE(
        result.routes.last().routeId,
        QStringLiteral(LargeRouteId));
    QCOMPARE(pageRequests, 2);
}

void TestStravaRoutesClient::
refreshesOnceAfterMidPaginationUnauthorized()
{
    QList<bool> refreshRequests;
    int pageTwoAttempts = 0;
    StravaRoutesClient client(
        [&](bool forceRefresh,
            const StravaRoutesClient::CancellationCheck &) {
            refreshRequests.append(forceRefresh);
            return token(forceRefresh
                ? QStringLiteral("rotated-access")
                : QStringLiteral("initial-access"));
        },
        [&](const QUrl &url,
            const QString &accessToken,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());

            const int page =
                QUrlQuery(url).queryItemValue(
                    QStringLiteral("page")).toInt();
            if (page == 1)
                return success(routesPayload(
                    StravaRoutesClient::PageSize));

            ++pageTwoAttempts;
            if (pageTwoAttempts == 1) {
                QCOMPARE(
                    accessToken,
                    QStringLiteral("initial-access"));
                return httpFailure(
                    401,
                    QNetworkReply::AuthenticationRequiredError);
            }
            QCOMPARE(
                accessToken,
                QStringLiteral("rotated-access"));
            return success(routesPayload(1, 9000));
        });

    const auto routes = client.listRoutes();

    QVERIFY2(routes.isValid(), qPrintable(routes.error));
    QCOMPARE(
        routes.routes.size(),
        StravaRoutesClient::PageSize + 1);
    QCOMPARE(refreshRequests, QList<bool>({false, true}));
    QCOMPARE(pageTwoAttempts, 2);
}

void TestStravaRoutesClient::doesNotRetryForbidden()
{
    int tokenCalls = 0;
    int requestCalls = 0;
    StravaRoutesClient client(
        [&](bool,
            const StravaRoutesClient::CancellationCheck &) {
            ++tokenCalls;
            return token(QStringLiteral("access"));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            return httpFailure(
                403, QNetworkReply::ContentAccessDenied);
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("HTTP 403")));
    QCOMPARE(tokenCalls, 1);
    QCOMPARE(requestCalls, 1);
}

void TestStravaRoutesClient::doesNotRetryASecondUnauthorized()
{
    int tokenCalls = 0;
    int requestCalls = 0;
    StravaRoutesClient client(
        [&](bool forceRefresh,
            const StravaRoutesClient::CancellationCheck &) {
            QCOMPARE(forceRefresh, tokenCalls == 1);
            ++tokenCalls;
            return token(QStringLiteral("access-%1").arg(tokenCalls));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            return httpFailure(
                401,
                QNetworkReply::AuthenticationRequiredError);
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("HTTP 401")));
    QCOMPARE(tokenCalls, 2);
    QCOMPARE(requestCalls, 2);
}

void TestStravaRoutesClient::
reportsTimeoutAndCancellation_data()
{
    QTest::addColumn<StravaRoutesClient::RequestFailure>(
        "failure");
    QTest::addColumn<QString>("message");

    QTest::newRow("timeout")
        << StravaRoutesClient::RequestFailure::TimedOut
        << QStringLiteral("timed out");
    QTest::newRow("cancelled")
        << StravaRoutesClient::RequestFailure::Cancelled
        << QStringLiteral("cancelled");
}

void TestStravaRoutesClient::reportsTimeoutAndCancellation()
{
    QFETCH(StravaRoutesClient::RequestFailure, failure);
    QFETCH(QString, message);

    StravaRoutesClient client(
        [](bool,
           const StravaRoutesClient::CancellationCheck &) {
            return token(QStringLiteral("access"));
        },
        [failure](const QUrl &,
                  const QString &,
                  qsizetype,
                  const StravaRoutesClient::CancellationCheck &) {
            HttpResult result;
            result.failure = failure;
            return result;
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY2(
        result.error.contains(message, Qt::CaseInsensitive),
        qPrintable(result.error));
}

void TestStravaRoutesClient::observesCancellationBetweenPages()
{
    int requestCalls = 0;
    bool cancelled = false;
    StravaRoutesClient client(
        [](bool,
           const StravaRoutesClient::CancellationCheck &) {
            return token(QStringLiteral("access"));
        },
        [&](const QUrl &url,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            cancelled = true;
            return success(routesPayload(
                StravaRoutesClient::PageSize));
        });

    const auto result =
        client.listRoutes([&] { return cancelled; });

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
    QCOMPARE(requestCalls, 2);
}

void TestStravaRoutesClient::rejectsMalformedResponses_data()
{
    QTest::addColumn<QByteArray>("athlete");
    QTest::addColumn<QByteArray>("routes");

    QTest::newRow("athlete-json")
        << QByteArrayLiteral("{")
        << QByteArrayLiteral("[]");
    QTest::newRow("athlete-id")
        << QByteArrayLiteral(R"json({"id":"not-a-number"})json")
        << QByteArrayLiteral("[]");
    QTest::newRow("routes-object")
        << athletePayload()
        << QByteArrayLiteral("{}");
    QTest::newRow("route-id")
        << athletePayload()
        << QByteArrayLiteral(
               R"json([{"id_str":"-1","name":"x","description":""}])json");
    QTest::newRow("route-name")
        << athletePayload()
        << QByteArrayLiteral(
               R"json([{"id_str":"42","name":7,"description":""}])json");
}

void TestStravaRoutesClient::rejectsMalformedResponses()
{
    QFETCH(QByteArray, athlete);
    QFETCH(QByteArray, routes);
    StravaRoutesClient client(
        [](bool,
           const StravaRoutesClient::CancellationCheck &) {
            return token(QStringLiteral("access"));
        },
        [athlete, routes](
            const QUrl &url,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            return success(
                url.path().endsWith(QStringLiteral("/athlete"))
                    ? athlete : routes);
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(!result.error.isEmpty());
}

void TestStravaRoutesClient::rejectsOversizedResponses()
{
    StravaRoutesClient client(
        [](bool,
           const StravaRoutesClient::CancellationCheck &) {
            return token(QStringLiteral("access"));
        },
        [](const QUrl &,
           const QString &,
           qsizetype maximumBytes,
           const StravaRoutesClient::CancellationCheck &) {
            HttpResult result = success(
                QByteArray(maximumBytes + 1, 'x'));
            return result;
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(
        QStringLiteral("oversized"), Qt::CaseInsensitive));
}

void TestStravaRoutesClient::
validatesRouteIdBeforeGpxRequest_data()
{
    QTest::addColumn<QString>("routeId");

    QTest::newRow("empty") << QString();
    QTest::newRow("negative") << QStringLiteral("-1");
    QTest::newRow("letters") << QStringLiteral("route-1");
    QTest::newRow("zero") << QStringLiteral("0");
    QTest::newRow("overflow")
        << QStringLiteral("9223372036854775808");
}

void TestStravaRoutesClient::validatesRouteIdBeforeGpxRequest()
{
    QFETCH(QString, routeId);
    int tokenCalls = 0;
    int requestCalls = 0;
    StravaRoutesClient client(
        [&](bool,
            const StravaRoutesClient::CancellationCheck &) {
            ++tokenCalls;
            return token(QStringLiteral("access"));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            return success(QByteArrayLiteral("<gpx/>"));
        });

    const auto result = client.downloadGpx(routeId);

    QVERIFY(!result.isValid());
    QCOMPARE(tokenCalls, 0);
    QCOMPARE(requestCalls, 0);
}

void TestStravaRoutesClient::redactsAccessTokenFromErrors()
{
    const QString secret = QStringLiteral("sensitive-access-token");
    StravaRoutesClient client(
        [secret](bool,
                 const StravaRoutesClient::CancellationCheck &) {
            return token(secret);
        },
        [secret](const QUrl &,
                 const QString &,
                 qsizetype,
                 const StravaRoutesClient::CancellationCheck &) {
            HttpResult result;
            result.networkError =
                QNetworkReply::RemoteHostClosedError;
            result.networkErrorString =
                QStringLiteral("connection failed for %1").arg(secret);
            return result;
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(!result.error.contains(secret));
    QVERIFY(result.error.contains(QStringLiteral("[redacted]")));
}

void TestStravaRoutesClient::
productionWiringUsesSharedRefreshAndBoundedWait()
{
    const QString sourceRoot =
        QString::fromUtf8(SRCDIR) + QStringLiteral("../../../src/");
    QFile routesSource(
        sourceRoot + QStringLiteral("Train/StravaRoutesDownload.cpp"));
    QVERIFY(routesSource.open(QIODevice::ReadOnly));
    const QByteArray routes = routesSource.readAll();

    QVERIFY(!routes.contains("QEventLoop"));
    QVERIFY(!routes.contains("GC_STRAVA_TOKEN"));
    QVERIFY(routes.contains("waitForNetworkReply("));
    QVERIFY(routes.contains("30000"));
    QVERIFY(routes.contains("deleteLater()"));
    QVERIFY(routes.contains(
        "StravaTokenRefreshCoordinator::invalidate("));
    QVERIFY(routes.contains("open(errors, cancellation)"));

    QFile stravaSource(
        sourceRoot + QStringLiteral("Cloud/Strava.cpp"));
    QVERIFY(stravaSource.open(QIODevice::ReadOnly));
    const QByteArray strava = stravaSource.readAll();
    QVERIFY(strava.contains(
        "bool\nStrava::open(\n    QStringList &errors,\n"
        "    const CancellationCheck &cancelled)"));
}

QTEST_MAIN(TestStravaRoutesClient)
#include "testStravaRoutesClient.moc"
