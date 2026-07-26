#include "Cloud/StravaAuthenticatedSession.h"
#include "Cloud/StravaNetworkReply.h"

#include <QCoreApplication>
#include <QEvent>
#include <QNetworkRequest>
#include <QPointer>
#include <QTest>
#include <QTimer>

#include <cstring>
#include <memory>
#include <utility>

namespace {

using HttpResult = StravaNetworkReply::Result;
using Grant = StravaAuthenticatedSession::Grant;

Grant grant(const QString &accessToken)
{
    Grant result;
    result.accessToken = accessToken;
    return result;
}

HttpResult response(
    int status = 200,
    const QByteArray &payload = QByteArrayLiteral("{}"))
{
    HttpResult result;
    result.httpStatus = status;
    result.payload = payload;
    result.contentType = QStringLiteral("application/json");
    if (status == 401) {
        result.networkError =
            QNetworkReply::AuthenticationRequiredError;
    } else if (status == 403) {
        result.networkError =
            QNetworkReply::ContentAccessDenied;
    }
    return result;
}

struct ReplyState {
    int aborts = 0;
};

class ControlledReply final : public QNetworkReply
{
public:
    ControlledReply(
        QByteArray payload,
        std::shared_ptr<ReplyState> state,
        QObject *parent = nullptr)
        : QNetworkReply(parent),
          payload_(std::move(payload)),
          state_(std::move(state))
    {
        const QUrl url(QStringLiteral(
            "https://www.strava.com/api/v3/athlete"));
        setRequest(QNetworkRequest(url));
        setUrl(url);
        open(QIODevice::ReadOnly);
    }

    void complete(
        int status = 200,
        QNetworkReply::NetworkError error =
            QNetworkReply::NoError)
    {
        if (isFinished()) return;
        setAttribute(
            QNetworkRequest::HttpStatusCodeAttribute,
            status);
        setHeader(
            QNetworkRequest::ContentTypeHeader,
            QStringLiteral("application/json"));
        if (error != QNetworkReply::NoError) {
            setError(error, QStringLiteral(
                "Synthetic network error"));
        }
        if (!payload_.isEmpty())
            emit readyRead();
        if (isFinished()) return;
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        ++state_->aborts;
        if (isFinished()) return;
        setError(
            QNetworkReply::OperationCanceledError,
            QStringLiteral("Synthetic abort"));
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return payload_.size() - offset_
            + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        if (offset_ >= payload_.size())
            return -1;
        const qint64 count = qMin(
            maximumSize,
            qint64(payload_.size() - offset_));
        std::memcpy(
            data, payload_.constData() + offset_, count);
        offset_ += count;
        return count;
    }

private:
    QByteArray payload_;
    qint64 offset_ = 0;
    std::shared_ptr<ReplyState> state_;
};

void deliverDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
}

} // namespace

class TestStravaAuthenticatedSession : public QObject
{
    Q_OBJECT

private slots:
    void obtainsGrantBeforeFirstRequestAndReusesIt();
    void grantPublicationFailurePreventsRequest();
    void unauthorizedRefreshesAndRetriesExactlyOnce();
    void reentrantGrantInstallationUsesReplacementToken();
    void secondUnauthorizedStops();
    void forbiddenDoesNotRefresh();
    void rejectsUntrustedOrigins_data();
    void rejectsUntrustedOrigins();
    void propagatesBoundedFailures_data();
    void propagatesBoundedFailures();
    void redactsAccessTokenFromNetworkErrors();
    void redactsCandidateAccessTokenFromGrantErrors();
    void transportCollectsAndDeletesSuccessfulReply();
    void transportTimeoutAbortsAndDeletesReply();
    void transportCancellationAbortsAndDeletesReply();
    void transportThrowingCancellationAbortsAndDeletesReply();
    void transportDestroyedPendingReplyReturnsInvalid();
    void transportOversizeAbortsAndDeletesReply();
};

void TestStravaAuthenticatedSession::
obtainsGrantBeforeFirstRequestAndReusesIt()
{
    int grantCalls = 0;
    int requestCalls = 0;
    QStringList observedTokens;
    StravaAuthenticatedSession session(
        [&](const QString &rejected,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++grantCalls;
            if (!rejected.isEmpty()) {
                Grant failure;
                failure.error =
                    QStringLiteral("Unexpected rejected token.");
                return failure;
            }
            return grant(QStringLiteral("fresh-access"));
        },
        [&](const QUrl &,
            const QString &accessToken,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            observedTokens.append(accessToken);
            return response();
        });

    const QUrl url(QStringLiteral(
        "https://www.strava.com/api/v3/athlete"));
    const auto first = session.get(url, 4096);
    const auto second = session.get(url, 4096);

    QVERIFY2(first.isValid(), qPrintable(first.error));
    QVERIFY2(second.isValid(), qPrintable(second.error));
    QCOMPARE(grantCalls, 1);
    QCOMPARE(requestCalls, 2);
    QCOMPARE(
        observedTokens,
        QStringList({
            QStringLiteral("fresh-access"),
            QStringLiteral("fresh-access")
        }));
}

void TestStravaAuthenticatedSession::
grantPublicationFailurePreventsRequest()
{
    int requestCalls = 0;
    StravaAuthenticatedSession session(
        [](const QString &,
           const StravaAuthenticatedSession::CancellationCheck &) {
            Grant result;
            result.error = QStringLiteral(
                "Credentials could not be stored securely.");
            return result;
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            return response();
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(
        QStringLiteral("stored securely")));
    QCOMPARE(requestCalls, 0);
}

void TestStravaAuthenticatedSession::
unauthorizedRefreshesAndRetriesExactlyOnce()
{
    QStringList rejectedTokens;
    QStringList requestTokens;
    StravaAuthenticatedSession session(
        [&](const QString &rejected,
            const StravaAuthenticatedSession::CancellationCheck &) {
            rejectedTokens.append(rejected);
            return grant(rejected.isEmpty()
                ? QStringLiteral("access-a")
                : QStringLiteral("access-b"));
        },
        [&](const QUrl &,
            const QString &accessToken,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            requestTokens.append(accessToken);
            return response(
                requestTokens.size() == 1 ? 401 : 200);
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(
        rejectedTokens,
        QStringList({
            QString(),
            QStringLiteral("access-a")
        }));
    QCOMPARE(
        requestTokens,
        QStringList({
            QStringLiteral("access-a"),
            QStringLiteral("access-b")
        }));
}

void TestStravaAuthenticatedSession::
reentrantGrantInstallationUsesReplacementToken()
{
    int grantCalls = 0;
    bool replacementInstalled = false;
    QStringList requestTokens;
    std::unique_ptr<StravaAuthenticatedSession> session;
    session = std::make_unique<StravaAuthenticatedSession>(
        [&](const QString &rejected,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++grantCalls;
            if (!rejected.isEmpty()) {
                Grant result;
                result.error = QStringLiteral(
                    "The replacement token must not be refreshed.");
                return result;
            }
            return grant(QStringLiteral("access-a"));
        },
        [&](const QUrl &,
            const QString &accessToken,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            requestTokens.append(accessToken);
            if (requestTokens.size() == 1) {
                replacementInstalled =
                    session->installGrant(
                        grant(QStringLiteral("access-b")));
                return response(401);
            }
            return response();
        });

    const auto result = session->get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY2(result.isValid(), qPrintable(result.error));
    QVERIFY(replacementInstalled);
    QCOMPARE(grantCalls, 1);
    QCOMPARE(
        requestTokens,
        QStringList({
            QStringLiteral("access-a"),
            QStringLiteral("access-b")
        }));
}

void TestStravaAuthenticatedSession::secondUnauthorizedStops()
{
    int grantCalls = 0;
    int requestCalls = 0;
    StravaAuthenticatedSession session(
        [&](const QString &,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++grantCalls;
            return grant(QStringLiteral(
                "access-%1").arg(grantCalls));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            return response(401);
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("HTTP 401")));
    QCOMPARE(grantCalls, 2);
    QCOMPARE(requestCalls, 2);
}

void TestStravaAuthenticatedSession::forbiddenDoesNotRefresh()
{
    int grantCalls = 0;
    int requestCalls = 0;
    StravaAuthenticatedSession session(
        [&](const QString &,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++grantCalls;
            return grant(QStringLiteral("access"));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            return response(403);
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("HTTP 403")));
    QCOMPARE(grantCalls, 1);
    QCOMPARE(requestCalls, 1);
}

void TestStravaAuthenticatedSession::rejectsUntrustedOrigins_data()
{
    QTest::addColumn<QUrl>("url");

    QTest::newRow("cleartext")
        << QUrl(QStringLiteral(
               "http://www.strava.com/api/v3/athlete"));
    QTest::newRow("foreign-host")
        << QUrl(QStringLiteral(
               "https://example.test/api/v3/athlete"));
    QTest::newRow("foreign-port")
        << QUrl(QStringLiteral(
               "https://www.strava.com:444/api/v3/athlete"));
    QTest::newRow("userinfo")
        << QUrl(QStringLiteral(
               "https://user@www.strava.com/api/v3/athlete"));
    QTest::newRow("oauth-path")
        << QUrl(QStringLiteral(
               "https://www.strava.com/oauth/token"));
    QTest::newRow("api-prefix-lookalike")
        << QUrl(QStringLiteral(
               "https://www.strava.com/api/v30/athlete"));
}

void TestStravaAuthenticatedSession::rejectsUntrustedOrigins()
{
    QFETCH(QUrl, url);
    int grantCalls = 0;
    int requestCalls = 0;
    StravaAuthenticatedSession session(
        [&](const QString &,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++grantCalls;
            return grant(QStringLiteral("access"));
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            return response();
        });

    const auto result = session.get(url, 4096);

    QVERIFY(!result.isValid());
    QCOMPARE(grantCalls, 0);
    QCOMPARE(requestCalls, 0);
}

void TestStravaAuthenticatedSession::
propagatesBoundedFailures_data()
{
    QTest::addColumn<StravaNetworkReply::Failure>("failure");
    QTest::addColumn<QString>("message");

    QTest::newRow("timeout")
        << StravaNetworkReply::Failure::TimedOut
        << QStringLiteral("timed out");
    QTest::newRow("cancelled")
        << StravaNetworkReply::Failure::Cancelled
        << QStringLiteral("cancelled");
    QTest::newRow("oversized")
        << StravaNetworkReply::Failure::Oversized
        << QStringLiteral("oversized");
}

void TestStravaAuthenticatedSession::propagatesBoundedFailures()
{
    QFETCH(StravaNetworkReply::Failure, failure);
    QFETCH(QString, message);
    StravaAuthenticatedSession session(
        [](const QString &,
           const StravaAuthenticatedSession::CancellationCheck &) {
            return grant(QStringLiteral("access"));
        },
        [failure](
            const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            HttpResult result;
            result.failure = failure;
            return result;
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY2(
        result.error.contains(message, Qt::CaseInsensitive),
        qPrintable(result.error));
}

void TestStravaAuthenticatedSession::
redactsAccessTokenFromNetworkErrors()
{
    const QString accessToken =
        QStringLiteral("sensitive-access-token");
    StravaAuthenticatedSession session(
        [accessToken](
            const QString &,
            const StravaAuthenticatedSession::CancellationCheck &) {
            return grant(accessToken);
        },
        [accessToken](
            const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            HttpResult result;
            result.networkError =
                QNetworkReply::RemoteHostClosedError;
            result.networkErrorString =
                QStringLiteral("Failure for %1")
                    .arg(accessToken);
            return result;
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY(!result.error.contains(accessToken));
    QVERIFY(result.error.contains(QStringLiteral("[redacted]")));
}

void TestStravaAuthenticatedSession::
redactsCandidateAccessTokenFromGrantErrors()
{
    const QString candidateToken =
        QStringLiteral("sensitive-candidate-token");
    int requestCalls = 0;
    StravaAuthenticatedSession session(
        [candidateToken](
            const QString &,
            const StravaAuthenticatedSession::CancellationCheck &) {
            Grant result;
            result.accessToken = candidateToken;
            result.error = QStringLiteral(
                "Credential provider rejected %1.")
                    .arg(candidateToken);
            return result;
        },
        [&](const QUrl &,
            const QString &,
            qsizetype,
            const StravaAuthenticatedSession::CancellationCheck &) {
            ++requestCalls;
            return response();
        });

    const auto result = session.get(
        QUrl(QStringLiteral(
            "https://www.strava.com/api/v3/athlete")),
        4096);

    QVERIFY(!result.isValid());
    QVERIFY(!result.error.contains(candidateToken));
    QVERIFY(result.error.contains(QStringLiteral("[redacted]")));
    QCOMPARE(requestCalls, 0);
}

void TestStravaAuthenticatedSession::
transportCollectsAndDeletesSuccessfulReply()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(
        QByteArrayLiteral("{\"id\":42}"), state);
    QPointer<ControlledReply> guard(reply);
    QTimer::singleShot(0, reply, [reply] {
        reply->complete();
    });

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 4096, 1000);

    QCOMPARE(result.failure, StravaNetworkReply::Failure::None);
    QCOMPARE(result.httpStatus, 200);
    QCOMPARE(result.payload, QByteArrayLiteral("{\"id\":42}"));
    QCOMPARE(state->aborts, 0);
    deliverDeferredDeletes();
    QVERIFY(guard.isNull());
}

void TestStravaAuthenticatedSession::
transportTimeoutAbortsAndDeletesReply()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(QByteArray(), state);
    QPointer<ControlledReply> guard(reply);

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 4096, 20);

    QCOMPARE(
        result.failure,
        StravaNetworkReply::Failure::TimedOut);
    QCOMPARE(state->aborts, 1);
    deliverDeferredDeletes();
    QVERIFY(guard.isNull());
}

void TestStravaAuthenticatedSession::
transportCancellationAbortsAndDeletesReply()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(QByteArray(), state);
    QPointer<ControlledReply> guard(reply);

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 4096, 1000, [] {
                return true;
            });

    QCOMPARE(
        result.failure,
        StravaNetworkReply::Failure::Cancelled);
    QCOMPARE(state->aborts, 1);
    deliverDeferredDeletes();
    QVERIFY(guard.isNull());
}

void TestStravaAuthenticatedSession::
transportThrowingCancellationAbortsAndDeletesReply()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(QByteArray(), state);
    QPointer<ControlledReply> guard(reply);

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 4096, 1000, []() -> bool {
                throw 1;
            });

    QCOMPARE(
        result.failure,
        StravaNetworkReply::Failure::Cancelled);
    QCOMPARE(state->aborts, 1);
    deliverDeferredDeletes();
    QVERIFY(guard.isNull());
}

void TestStravaAuthenticatedSession::
transportDestroyedPendingReplyReturnsInvalid()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(QByteArray(), state);
    QPointer<ControlledReply> guard(reply);
    QTimer::singleShot(0, [reply] {
        delete reply;
    });

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 4096, 1000);

    QCOMPARE(
        result.failure,
        StravaNetworkReply::Failure::Invalid);
    QCOMPARE(state->aborts, 0);
    QVERIFY(guard.isNull());
}

void TestStravaAuthenticatedSession::
transportOversizeAbortsAndDeletesReply()
{
    const auto state = std::make_shared<ReplyState>();
    auto *reply = new ControlledReply(
        QByteArrayLiteral("123456789"), state);
    QPointer<ControlledReply> guard(reply);
    QTimer::singleShot(0, reply, [reply] {
        reply->complete();
    });

    const HttpResult result =
        StravaNetworkReply::collect(
            reply, 8, 1000);

    QCOMPARE(
        result.failure,
        StravaNetworkReply::Failure::Oversized);
    QCOMPARE(state->aborts, 1);
    QVERIFY(result.payload.isEmpty());
    deliverDeferredDeletes();
    QVERIFY(guard.isNull());
}

QTEST_GUILESS_MAIN(TestStravaAuthenticatedSession)
#include "testStravaAuthenticatedSession.moc"
