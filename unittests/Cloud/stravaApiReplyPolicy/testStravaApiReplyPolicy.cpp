#include "Cloud/StravaApiReplyPolicy.h"

#include <QNetworkReply>
#include <QTest>

namespace {

constexpr int NoHttpStatus = 0;

StravaApiReplyPolicy::Result validate(
    StravaApiReplyPolicy::PayloadKind kind,
    const QByteArray &payload,
    int httpStatus = 200,
    QNetworkReply::NetworkError networkError =
        QNetworkReply::NoError,
    const QString &networkErrorString = QString(),
    const QStringList &sensitiveValues = {},
    const QString &contentType = QString())
{
    return StravaApiReplyPolicy::validate(
        kind,
        httpStatus,
        networkError,
        networkErrorString,
        payload,
        sensitiveValues,
        contentType);
}

void verifyRejected(const StravaApiReplyPolicy::Result &result)
{
    QVERIFY(!result.isValid());
    QVERIFY(!result.error.isEmpty());
}

} // namespace

class TestStravaApiReplyPolicy : public QObject
{
    Q_OBJECT

private slots:
    void acceptsDetailedActivity();
    void acceptsEmptyStreams();
    void acceptsHeartRateStream();
    void rejectsStravaFault_data();
    void rejectsStravaFault();
    void reportsTransportErrorWithoutHttpStatus();
    void rejectsMalformedOrNonObjectActivity_data();
    void rejectsMalformedOrNonObjectActivity();
    void rejectsInvalidActivityFields_data();
    void rejectsInvalidActivityFields();
    void rejectsFaultObjectReturnedWithSuccessStatus();
    void rejectsHttpFailureWithValidActivityShape();
    void validatesContentType();
    void rejectsMalformedStreams_data();
    void rejectsMalformedStreams();
    void rejectsOversizedPayload();
    void redactsSensitiveValues();
};

void TestStravaApiReplyPolicy::acceptsDetailedActivity()
{
    const QByteArray payload = R"json({
        "id": 14567890123,
        "name": "Synthetic interval session",
        "start_date_local": "2026-07-25T18:42:00Z",
        "type": "VirtualRide"
    })json";

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity, payload);

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.error, QString());
}

void TestStravaApiReplyPolicy::acceptsEmptyStreams()
{
    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Streams,
        QByteArrayLiteral("[]"));

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.error, QString());
}

void TestStravaApiReplyPolicy::acceptsHeartRateStream()
{
    const QByteArray payload = R"json([
        {
            "type": "heartrate",
            "data": [121, 134, 149, 142],
            "series_type": "time",
            "original_size": 4,
            "resolution": "high"
        }
    ])json";

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Streams, payload);

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.error, QString());
}

void TestStravaApiReplyPolicy::rejectsStravaFault_data()
{
    QTest::addColumn<int>("httpStatus");
    QTest::addColumn<QNetworkReply::NetworkError>("networkError");

    QTest::newRow("unauthorized")
        << 401 << QNetworkReply::AuthenticationRequiredError;
    QTest::newRow("forbidden")
        << 403 << QNetworkReply::ContentAccessDenied;
}

void TestStravaApiReplyPolicy::rejectsStravaFault()
{
    QFETCH(int, httpStatus);
    QFETCH(QNetworkReply::NetworkError, networkError);

    const QByteArray payload = R"json({
        "message": "Authorization Error",
        "errors": [{
            "resource": "AccessToken",
            "field": "activity:read_all",
            "code": "missing"
        }]
    })json";

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        httpStatus,
        networkError,
        QStringLiteral("Host requires authentication"));

    verifyRejected(result);
    QVERIFY2(result.error.contains(QString::number(httpStatus)),
             qPrintable(result.error));
    QVERIFY2(result.error.contains(
                 QStringLiteral("Authorization Error"),
                 Qt::CaseInsensitive),
             qPrintable(result.error));
}

void TestStravaApiReplyPolicy::reportsTransportErrorWithoutHttpStatus()
{
    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        QByteArray(),
        NoHttpStatus,
        QNetworkReply::TimeoutError,
        QStringLiteral("Connection timed out"));

    verifyRejected(result);
    QVERIFY2(result.error.contains(
                 QStringLiteral("timed out"),
                 Qt::CaseInsensitive),
             qPrintable(result.error));
    QVERIFY2(!result.error.contains(QStringLiteral("HTTP"),
                                    Qt::CaseInsensitive),
             qPrintable(result.error));
}

void TestStravaApiReplyPolicy::
rejectsMalformedOrNonObjectActivity_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("malformed-json")
        << QByteArrayLiteral("{\"id\":");
    QTest::newRow("array")
        << QByteArrayLiteral("[]");
    QTest::newRow("string")
        << QByteArrayLiteral("\"activity\"");
    QTest::newRow("null")
        << QByteArrayLiteral("null");
}

void TestStravaApiReplyPolicy::
rejectsMalformedOrNonObjectActivity()
{
    QFETCH(QByteArray, payload);

    verifyRejected(validate(
        StravaApiReplyPolicy::PayloadKind::Activity, payload));
}

void TestStravaApiReplyPolicy::rejectsInvalidActivityFields_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("missing-id")
        << QByteArrayLiteral(
               R"json({"start_date_local":"2026-07-25T18:42:00Z"})json");
    QTest::newRow("zero-id")
        << QByteArrayLiteral(
               R"json({"id":0,"start_date_local":"2026-07-25T18:42:00Z"})json");
    QTest::newRow("negative-id")
        << QByteArrayLiteral(
               R"json({"id":-1,"start_date_local":"2026-07-25T18:42:00Z"})json");
    QTest::newRow("string-id")
        << QByteArrayLiteral(
               R"json({"id":"14567890123","start_date_local":"2026-07-25T18:42:00Z"})json");
    QTest::newRow("missing-date")
        << QByteArrayLiteral(
               R"json({"id":14567890123})json");
    QTest::newRow("malformed-date")
        << QByteArrayLiteral(
               R"json({"id":14567890123,"start_date_local":"yesterday"})json");
    QTest::newRow("numeric-date")
        << QByteArrayLiteral(
               R"json({"id":14567890123,"start_date_local":1721932920})json");
}

void TestStravaApiReplyPolicy::rejectsInvalidActivityFields()
{
    QFETCH(QByteArray, payload);

    verifyRejected(validate(
        StravaApiReplyPolicy::PayloadKind::Activity, payload));
}

void TestStravaApiReplyPolicy::
rejectsFaultObjectReturnedWithSuccessStatus()
{
    const QByteArray payload = R"json({
        "message": "Rate Limit Exceeded",
        "errors": [{
            "resource": "Application",
            "field": "rate limit",
            "code": "exceeded"
        }]
    })json";

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        200,
        QNetworkReply::NoError);

    verifyRejected(result);
    QVERIFY2(result.error.contains(
                 QStringLiteral("Rate Limit Exceeded"),
                 Qt::CaseInsensitive),
             qPrintable(result.error));
}

void TestStravaApiReplyPolicy::
rejectsHttpFailureWithValidActivityShape()
{
    const QByteArray payload = R"json({
        "id": 14567890123,
        "start_date_local": "2026-07-25T18:42:00Z"
    })json";

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        503,
        QNetworkReply::NoError);

    verifyRejected(result);
    QVERIFY2(result.error.contains(QStringLiteral("HTTP 503")),
             qPrintable(result.error));
}

void TestStravaApiReplyPolicy::validatesContentType()
{
    const QByteArray payload = R"json({
        "id": 14567890123,
        "start_date_local": "2026-07-25T18:42:00Z"
    })json";

    const auto jsonResult = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        200,
        QNetworkReply::NoError,
        QString(),
        {},
        QStringLiteral("application/json; charset=utf-8"));
    QVERIFY2(jsonResult.isValid(), qPrintable(jsonResult.error));

    const auto htmlResult = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        200,
        QNetworkReply::NoError,
        QString(),
        {},
        QStringLiteral("text/html"));
    verifyRejected(htmlResult);
    QVERIFY2(htmlResult.error.contains(
                 QStringLiteral("content type"),
                 Qt::CaseInsensitive),
             qPrintable(htmlResult.error));
}

void TestStravaApiReplyPolicy::rejectsMalformedStreams_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("object-root")
        << QByteArrayLiteral("{}");
    QTest::newRow("scalar-entry")
        << QByteArrayLiteral("[17]");
    QTest::newRow("missing-type")
        << QByteArrayLiteral(R"json([{"data":[120]}])json");
    QTest::newRow("non-string-type")
        << QByteArrayLiteral(
               R"json([{"type":17,"data":[120]}])json");
    QTest::newRow("missing-data")
        << QByteArrayLiteral(
               R"json([{"type":"heartrate"}])json");
    QTest::newRow("non-array-data")
        << QByteArrayLiteral(
               R"json([{"type":"heartrate","data":"120"}])json");
}

void TestStravaApiReplyPolicy::rejectsMalformedStreams()
{
    QFETCH(QByteArray, payload);

    verifyRejected(validate(
        StravaApiReplyPolicy::PayloadKind::Streams, payload));
}

void TestStravaApiReplyPolicy::rejectsOversizedPayload()
{
    QByteArray payload(4 * 1024 * 1024, ' ');
    payload[0] = '[';
    payload[payload.size() - 1] = ']';

    verifyRejected(validate(
        StravaApiReplyPolicy::PayloadKind::Streams, payload));
}

void TestStravaApiReplyPolicy::redactsSensitiveValues()
{
    const QString sensitive =
        QStringLiteral("synthetic-secret-marker+/%");
    const QByteArray payload = QStringLiteral(R"json({
        "message": "Authorization Error: %1",
        "errors": [{
            "resource": "AccessToken",
            "field": "%1",
            "code": "invalid"
        }]
    })json")
                                   .arg(sensitive)
                                   .toUtf8();

    const auto result = validate(
        StravaApiReplyPolicy::PayloadKind::Activity,
        payload,
        401,
        QNetworkReply::AuthenticationRequiredError,
        QStringLiteral("Authentication failed for %1")
            .arg(sensitive),
        {sensitive});

    verifyRejected(result);
    QVERIFY2(!result.error.contains(sensitive),
             qPrintable(result.error));
    QVERIFY2(result.error.contains(
                 QStringLiteral("[redacted]"),
                 Qt::CaseInsensitive),
             qPrintable(result.error));
}

QTEST_APPLESS_MAIN(TestStravaApiReplyPolicy)

#include "testStravaApiReplyPolicy.moc"
