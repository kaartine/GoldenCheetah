#include "Train/StravaRoutesClient.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUrlQuery>

namespace {

using GetResult = StravaAuthenticatedSession::Result;

GetResult success(
    const QByteArray &payload,
    const QString &contentType = QStringLiteral("application/json"))
{
    GetResult result;
    result.payload = payload;
    result.contentType = contentType;
    return result;
}

GetResult failure(const QString &error)
{
    GetResult result;
    result.error = error;
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
    return QJsonDocument(routes).toJson(
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

} // namespace

class TestStravaRoutesClient : public QObject
{
    Q_OBJECT

private slots:
    void usesAuthenticatedGetForListAndGpx();
    void paginatesAndPreserves64BitRouteIds();
    void requestsEmptyPageAfterFullFinalPage();
    void discardsPartialRoutesAfterPageFailure();
    void propagatesAuthenticatedFailure_data();
    void propagatesAuthenticatedFailure();
    void observesCancellationBetweenPages();
    void rejectsMalformedResponses_data();
    void rejectsMalformedResponses();
    void rejectsOversizedResponses();
    void limitsPagination();
    void validatesRouteIdBeforeGpxRequest_data();
    void validatesRouteIdBeforeGpxRequest();
    void validatesGpxResponse_data();
    void validatesGpxResponse();
    void buildsLeafOnlyWorkoutFileNames();
    void productionWiringHidesCredentialsAndBoundsWork();
};

void TestStravaRoutesClient::usesAuthenticatedGetForListAndGpx()
{
    int requestCalls = 0;
    bool invalidMaximum = false;
    bool unexpectedUrl = false;
    StravaRoutesClient client(
        [&](const QUrl &url,
            qsizetype maximumBytes,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            invalidMaximum |= maximumBytes <= 0;
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            if (url.path().contains(QStringLiteral("/athletes/")))
                return success(routesPayload(1));
            unexpectedUrl |= url.path()
                != QStringLiteral(
                    "/api/v3/routes/1000/export_gpx");
            return success(
                QByteArrayLiteral(
                    R"xml(<gpx version="1.1"></gpx>)xml"),
                QStringLiteral("application/gpx+xml"));
        });

    const auto routes = client.listRoutes();
    QVERIFY2(routes.isValid(), qPrintable(routes.error));
    QCOMPARE(routes.routes.size(), 1);
    QCOMPARE(
        routes.routes.first().routeId,
        QStringLiteral("1000"));

    const auto gpx = client.downloadGpx(
        QStringLiteral("1000"));
    QVERIFY2(gpx.isValid(), qPrintable(gpx.error));
    QCOMPARE(requestCalls, 3);
    QVERIFY(!invalidMaximum);
    QVERIFY(!unexpectedUrl);
}

void TestStravaRoutesClient::
paginatesAndPreserves64BitRouteIds()
{
    constexpr auto LargeRouteId = "9223372036854775700";
    int pageRequests = 0;
    StravaRoutesClient client(
        [&](const QUrl &url,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());

            ++pageRequests;
            const int page = QUrlQuery(url)
                .queryItemValue(QStringLiteral("page"))
                .toInt();
            if (page == 1) {
                return success(routesPayload(
                    StravaRoutesClient::PageSize));
            }

            QJsonArray finalPage;
            finalPage.append(routeObject(
                QString::fromLatin1(LargeRouteId)));
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
        QString::fromLatin1(LargeRouteId));
    QCOMPARE(pageRequests, 2);
}

void TestStravaRoutesClient::
requestsEmptyPageAfterFullFinalPage()
{
    int pageRequests = 0;
    StravaRoutesClient client(
        [&](const QUrl &url,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            ++pageRequests;
            return success(pageRequests == 1
                ? routesPayload(StravaRoutesClient::PageSize)
                : QByteArrayLiteral("[]"));
        });

    const auto result = client.listRoutes();

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(
        result.routes.size(),
        StravaRoutesClient::PageSize);
    QCOMPARE(pageRequests, 2);
}

void TestStravaRoutesClient::
discardsPartialRoutesAfterPageFailure()
{
    int pageRequests = 0;
    StravaRoutesClient client(
        [&](const QUrl &url,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            ++pageRequests;
            if (pageRequests == 1) {
                return success(routesPayload(
                    StravaRoutesClient::PageSize));
            }
            return failure(QStringLiteral(
                "Synthetic authenticated page failure."));
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.routes.isEmpty());
    QVERIFY(result.error.contains(
        QStringLiteral("page failure")));
    QCOMPARE(pageRequests, 2);
}

void TestStravaRoutesClient::
propagatesAuthenticatedFailure_data()
{
    QTest::addColumn<QString>("error");

    QTest::newRow("forbidden")
        << QStringLiteral(
               "Strava request failed (HTTP 403).");
    QTest::newRow("timeout")
        << QStringLiteral(
               "Strava request timed out.");
    QTest::newRow("cancelled")
        << QStringLiteral(
               "Strava request was cancelled.");
}

void TestStravaRoutesClient::propagatesAuthenticatedFailure()
{
    QFETCH(QString, error);
    StravaRoutesClient client(
        [error](
            const QUrl &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            return failure(error);
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QCOMPARE(result.error, error);
}

void TestStravaRoutesClient::
observesCancellationBetweenPages()
{
    int requestCalls = 0;
    bool cancelled = false;
    StravaRoutesClient client(
        [&](const QUrl &url,
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
    QVERIFY(result.routes.isEmpty());
    QVERIFY(result.error.contains(
        QStringLiteral("cancelled"), Qt::CaseInsensitive));
    QCOMPARE(requestCalls, 2);
}

void TestStravaRoutesClient::rejectsMalformedResponses_data()
{
    QTest::addColumn<QByteArray>("athlete");
    QTest::addColumn<QByteArray>("routes");

    QTest::newRow("empty-athlete")
        << QByteArray()
        << QByteArrayLiteral("[]");
    QTest::newRow("athlete-json")
        << QByteArrayLiteral("{")
        << QByteArrayLiteral("[]");
    QTest::newRow("athlete-id")
        << QByteArrayLiteral(R"json({"id":"not-a-number"})json")
        << QByteArrayLiteral("[]");
    QTest::newRow("empty-routes")
        << athletePayload()
        << QByteArray();
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
        [athlete, routes](
            const QUrl &url,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            return success(
                url.path().endsWith(QStringLiteral("/athlete"))
                    ? athlete : routes);
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.routes.isEmpty());
    QVERIFY(!result.error.isEmpty());
}

void TestStravaRoutesClient::rejectsOversizedResponses()
{
    StravaRoutesClient client(
        [](const QUrl &,
           qsizetype maximumBytes,
           const StravaRoutesClient::CancellationCheck &) {
            return success(
                QByteArray(maximumBytes + 1, 'x'));
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(
        QStringLiteral("oversized"), Qt::CaseInsensitive));
}

void TestStravaRoutesClient::limitsPagination()
{
    int pageRequests = 0;
    StravaRoutesClient client(
        [&](const QUrl &url,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            if (url.path().endsWith(QStringLiteral("/athlete")))
                return success(athletePayload());
            ++pageRequests;
            return success(routesPayload(
                StravaRoutesClient::PageSize,
                quint64(pageRequests) * 100000));
        });

    const auto result = client.listRoutes();

    QVERIFY(!result.isValid());
    QVERIFY(result.routes.isEmpty());
    QVERIFY(result.error.contains(
        QStringLiteral("safety limit")));
    QCOMPARE(
        pageRequests,
        StravaRoutesClient::MaximumPages);
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

void TestStravaRoutesClient::
validatesRouteIdBeforeGpxRequest()
{
    QFETCH(QString, routeId);
    int requestCalls = 0;
    StravaRoutesClient client(
        [&](const QUrl &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            ++requestCalls;
            return success(
                QByteArrayLiteral("<gpx/>"),
                QStringLiteral("application/gpx+xml"));
        });

    const auto result = client.downloadGpx(routeId);

    QVERIFY(!result.isValid());
    QCOMPARE(requestCalls, 0);
}

void TestStravaRoutesClient::validatesGpxResponse_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QString>("contentType");
    QTest::addColumn<bool>("valid");

    QTest::newRow("valid")
        << QByteArrayLiteral(
               R"xml(<?xml version="1.0"?><gpx version="1.1"></gpx>)xml")
        << QStringLiteral("application/gpx+xml")
        << true;
    QTest::newRow("wrong-type")
        << QByteArrayLiteral("<gpx/>")
        << QStringLiteral("text/plain")
        << false;
    QTest::newRow("malformed")
        << QByteArrayLiteral("<gpx>")
        << QStringLiteral("application/gpx+xml")
        << false;
    QTest::newRow("wrong-root")
        << QByteArrayLiteral("<TrainingCenterDatabase/>")
        << QStringLiteral("application/xml")
        << false;
    QTest::newRow("doctype")
        << QByteArrayLiteral(
               "<!DOCTYPE gpx [<!ENTITY value \"expanded\">]>"
               "<gpx>&value;</gpx>")
        << QStringLiteral("application/gpx+xml")
        << false;
}

void TestStravaRoutesClient::validatesGpxResponse()
{
    QFETCH(QByteArray, payload);
    QFETCH(QString, contentType);
    QFETCH(bool, valid);
    StravaRoutesClient client(
        [payload, contentType](
            const QUrl &,
            qsizetype,
            const StravaRoutesClient::CancellationCheck &) {
            return success(payload, contentType);
        });

    const auto result =
        client.downloadGpx(QStringLiteral("42"));

    QCOMPARE(result.isValid(), valid);
    if (!valid)
        QVERIFY(!result.error.isEmpty());
}

void TestStravaRoutesClient::buildsLeafOnlyWorkoutFileNames()
{
    const QString name =
        StravaRoutesClient::workoutFileName(
            QStringLiteral("9223372036854775700"));

    QCOMPARE(
        name,
        QStringLiteral(
            "Strava-Route-9223372036854775700.gpx"));
    QCOMPARE(QFileInfo(name).fileName(), name);
    QVERIFY(StravaRoutesClient::workoutFileName(
        QStringLiteral("../route")).isEmpty());
}

void TestStravaRoutesClient::
productionWiringHidesCredentialsAndBoundsWork()
{
    const QByteArray routes = sourceContents(
        "../../../src/Train/StravaRoutesDownload.cpp");
    QVERIFY(!routes.isEmpty());

    QVERIFY(!routes.contains("QEventLoop"));
    QVERIFY(!routes.contains("GC_STRAVA_TOKEN"));
    QVERIFY(!routes.contains("Authorization"));
    QVERIFY(!routes.contains("waitForNetworkReply("));
    QVERIFY(!routes.contains(
        "StravaTokenRefreshCoordinator"));
    QVERIFY(routes.contains("authenticatedGet("));
    QVERIFY(routes.contains("Qt::UserRole"));
    QVERIFY(!routes.contains("setText(4"));
    QVERIFY(routes.contains("QTemporaryFile temporaryFile("));
    QVERIFY(!routes.contains(
        "temporaryDirectory.filePath(fileBasename)"));
    QVERIFY(routes.contains("QTimer::singleShot("));
    QVERIFY(!routes.contains("\n    refreshClicked();"));

    const qsizetype readPosition =
        routes.indexOf("readFile(");
    const qsizetype transactionPosition =
        routes.indexOf("trainDB->startLUW()", readPosition);
    QVERIFY(readPosition >= 0);
    QVERIFY(transactionPosition > readPosition);

    const QByteArray strava = sourceContents(
        "../../../src/Cloud/Strava.cpp");
    QVERIFY(!strava.isEmpty());
    QVERIFY(strava.contains(
        "StravaAuthenticatedSession"));
    QVERIFY(strava.contains(
        "refreshAfterRejectedAccessToken("));

    const QByteArray stravaHeader = sourceContents(
        "../../../src/Cloud/Strava.h");
    QVERIFY(!stravaHeader.isEmpty());
    QVERIFY(stravaHeader.contains(
        "QNetworkAccessManager *nam = nullptr;"));
    QVERIFY(stravaHeader.contains(
        "QNetworkReply *reply = nullptr;"));
}

QTEST_MAIN(TestStravaRoutesClient)
#include "testStravaRoutesClient.moc"
