#include "Cloud/StravaAccountRemoval.h"
#include "Cloud/StravaCredentialPublisher.h"
#include "Cloud/StravaTokenRefresh.h"

#include <QTest>
#include <QUuid>

#include <atomic>

namespace {

struct PublisherProbe
{
    bool markResult = true;
    StravaTokenPublication::RemovalResult removalResult{
        StravaTokenPublication::RemovalStatus::Cleared,
        QString()
    };
    int markCalls = 0;
    int removalCalls = 0;
    QString accountKey;
    StravaCredentialPublisher::RemovalRequest removalRequest;
};

PublisherProbe &publisherProbe()
{
    static PublisherProbe probe;
    return probe;
}

void resetPublisherProbe()
{
    publisherProbe() = {};
}

QString uniqueAccount(const QString &label)
{
    return label + QLatin1Char('-')
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
}

StravaAccountRemoval::Request localRequest(
    const QString &account)
{
    StravaAccountRemoval::Request request;
    request.accountKey = account;
    request.accessToken =
        QStringLiteral("synthetic-access-token");
    request.refreshToken =
        QStringLiteral("synthetic-refresh-token");
    request.mode = StravaAccountRemoval::Mode::LocalOnly;
    return request;
}

} // namespace

namespace StravaCredentialPublisher {

bool markRevocationPending(
    const QString &accountKey,
    int,
    const CancellationCheck &)
{
    PublisherProbe &probe = publisherProbe();
    ++probe.markCalls;
    probe.accountKey = accountKey;
    return probe.markResult;
}

StravaTokenPublication::RemovalResult remove(
    const RemovalRequest &request,
    int,
    const CancellationCheck &)
{
    PublisherProbe &probe = publisherProbe();
    ++probe.removalCalls;
    probe.removalRequest = request;
    return probe.removalResult;
}

} // namespace StravaCredentialPublisher

class TestStravaAccountRemoval : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void localOnlyRemovalClearsCredentialsAndClosesState();
    void cleanupPendingRemainsACompletedDisconnect();
    void invalidRemoteCredentialsDoNotMutateState();
    void accessTokenRevocationClearsRemoteWarning();
    void refreshTokenRevocationClearsRemoteWarning();
    void remoteSuccessSurvivesLocalCleanupFailure();
    void staleDisconnectCannotRemoveNewGrant();
    void failedPendingPersistenceDoesNotRevoke();
    void cancellationBeforeStartDoesNotMutateStorage();
    void invalidModeFailsClosed();
};

void TestStravaAccountRemoval::init()
{
    resetPublisherProbe();
}

void TestStravaAccountRemoval::
localOnlyRemovalClearsCredentialsAndClosesState()
{
    const QString account =
        uniqueAccount(QStringLiteral("local-only"));
    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            localRequest(account));

    QVERIFY(result.isSuccess());
    QVERIFY(result.disconnected);
    QVERIFY(!result.cleanupPending);
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(publisherProbe().markCalls, 1);
    QCOMPARE(publisherProbe().removalCalls, 1);
    QCOMPARE(publisherProbe().accountKey, account);
    QCOMPARE(
        publisherProbe().removalRequest.accountKey,
        account);
    QCOMPARE(
        publisherProbe().removalRequest.expectedRefreshToken,
        QStringLiteral("synthetic-refresh-token"));
    QCOMPARE(
        publisherProbe().removalRequest.mode,
        StravaTokenPublication::PublicationMode::
            Authoritative);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaAccountRemoval::
cleanupPendingRemainsACompletedDisconnect()
{
    const QString account =
        uniqueAccount(QStringLiteral("cleanup-pending"));
    publisherProbe().removalResult = {
        StravaTokenPublication::RemovalStatus::CleanupPending,
        QStringLiteral("Synthetic cleanup warning.")
    };

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            localRequest(account));

    QVERIFY(result.isSuccess());
    QVERIFY(result.disconnected);
    QVERIFY(result.cleanupPending);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaAccountRemoval::
invalidRemoteCredentialsDoNotMutateState()
{
    const QString account =
        uniqueAccount(QStringLiteral("invalid-remote"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("__GC_STRAVA_CLIENT_SECRET__");

    QVERIFY(!request.isValid());
    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(request);

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(publisherProbe().markCalls, 0);
    QCOMPARE(publisherProbe().removalCalls, 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
}

void TestStravaAccountRemoval::
accessTokenRevocationClearsRemoteWarning()
{
    const QString account =
        uniqueAccount(QStringLiteral("access-fallback"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.refreshToken.clear();
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    StravaAccountRemoval::RevocationToken revokedToken =
        StravaAccountRemoval::RevocationToken::RefreshToken;
    QString submittedToken;

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [&](const QString &token,
                StravaAccountRemoval::RevocationToken tokenType,
                const StravaAccountRemoval::CancellationCheck &) {
                submittedToken = token;
                revokedToken = tokenType;
                return StravaAccountRemoval::RemoteRevocationResult{
                    true, QString()
                };
            });

    QVERIFY(result.isSuccess());
    QVERIFY(!result.remoteAuthorizationMayRemain);
    QCOMPARE(
        submittedToken,
        QStringLiteral("synthetic-access-token"));
    QCOMPARE(
        revokedToken,
        StravaAccountRemoval::RevocationToken::AccessToken);
}

void TestStravaAccountRemoval::
refreshTokenRevocationClearsRemoteWarning()
{
    const QString account =
        uniqueAccount(QStringLiteral("refresh-revoke"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    StravaAccountRemoval::RevocationToken revokedToken =
        StravaAccountRemoval::RevocationToken::AccessToken;
    QString submittedToken;

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [&](const QString &token,
                StravaAccountRemoval::RevocationToken tokenType,
                const StravaAccountRemoval::CancellationCheck &) {
                submittedToken = token;
                revokedToken = tokenType;
                return StravaAccountRemoval::RemoteRevocationResult{
                    true, QString()
                };
            });

    QVERIFY(result.isSuccess());
    QVERIFY(!result.remoteAuthorizationMayRemain);
    QCOMPARE(
        submittedToken,
        QStringLiteral("synthetic-refresh-token"));
    QCOMPARE(
        revokedToken,
        StravaAccountRemoval::RevocationToken::RefreshToken);
}

void TestStravaAccountRemoval::
remoteSuccessSurvivesLocalCleanupFailure()
{
    const QString account =
        uniqueAccount(QStringLiteral("remote-cleanup-failure"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    publisherProbe().removalResult = {
        StravaTokenPublication::RemovalStatus::StorageFailure,
        QStringLiteral("Synthetic local cleanup failure.")
    };

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [](const QString &,
               StravaAccountRemoval::RevocationToken,
               const StravaAccountRemoval::CancellationCheck &) {
                return StravaAccountRemoval::
                    RemoteRevocationResult{
                        true, QString()
                    };
            });

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.remoteAuthorizationMayRemain);
    QCOMPARE(publisherProbe().markCalls, 1);
    QCOMPARE(publisherProbe().removalCalls, 1);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);
}

void TestStravaAccountRemoval::
staleDisconnectCannotRemoveNewGrant()
{
    const QString account =
        uniqueAccount(QStringLiteral("stale-disconnect"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.expectedAuthorizationEpoch =
        StravaTokenRefreshCoordinator::authorizationEpoch(
            account);
    const StravaTokenRefreshResult replacement{
        true,
        QStringLiteral("access-replacement"),
        QStringLiteral("refresh-replacement"),
        QString(),
        QStringLiteral("refresh-replacement"),
        QString()
    };
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, replacement));

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(request);

    QVERIFY(!result.isSuccess());
    QCOMPARE(publisherProbe().markCalls, 0);
    QCOMPARE(publisherProbe().removalCalls, 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
    auto permit =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(permit.isValid());
    QCOMPARE(
        permit.accessToken(),
        replacement.accessToken);
}

void TestStravaAccountRemoval::
failedPendingPersistenceDoesNotRevoke()
{
    const QString account =
        uniqueAccount(QStringLiteral("pending-failure"));
    publisherProbe().markResult = false;

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            localRequest(account));

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(publisherProbe().markCalls, 1);
    QCOMPARE(publisherProbe().removalCalls, 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
}

void TestStravaAccountRemoval::
cancellationBeforeStartDoesNotMutateStorage()
{
    const QString account =
        uniqueAccount(QStringLiteral("cancelled"));
    std::atomic<int> cancellationCalls{0};

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            localRequest(account),
            [&] {
                ++cancellationCalls;
                return true;
            });

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(cancellationCalls.load() > 0);
    QCOMPARE(publisherProbe().markCalls, 0);
    QCOMPARE(publisherProbe().removalCalls, 0);
}

void TestStravaAccountRemoval::invalidModeFailsClosed()
{
    StravaAccountRemoval::Request request =
        localRequest(uniqueAccount(
            QStringLiteral("invalid-mode")));
    request.mode =
        static_cast<StravaAccountRemoval::Mode>(99);
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");

    QVERIFY(!request.isValid());
    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(request);
    QVERIFY(!result.isSuccess());
    QCOMPARE(publisherProbe().markCalls, 0);
    QCOMPARE(publisherProbe().removalCalls, 0);
}

QTEST_MAIN(TestStravaAccountRemoval)
#include "testStravaAccountRemoval.moc"
