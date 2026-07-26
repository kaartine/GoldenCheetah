#include "Cloud/StravaAccountRemoval.h"
#include "Cloud/StravaCredentialPublisher.h"
#include "Cloud/StravaTokenRefresh.h"

#include <QTest>
#include <QUuid>

#include <atomic>
#include <functional>
#include <stdexcept>

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
    std::function<void()> markHook;
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
    if (probe.markHook)
        probe.markHook();
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
    void accessTokenSuccessSurvivesLocalCleanupFailure();
    void remoteTokenDoesNotReplaceLocalCasToken();
    void staleDisconnectCannotRemoveNewGrant();
    void failedPendingPersistenceDoesNotRevoke();
    void throwingRemoteRevocationReportsUncertainty();
    void cancellationBeforeStartDoesNotMutateStorage();
    void cancellationAfterPendingCannotAbortRemoteRevocation();
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
accessTokenSuccessSurvivesLocalCleanupFailure()
{
    const QString account =
        uniqueAccount(QStringLiteral(
            "access-cleanup-failure"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.refreshToken.clear();
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    publisherProbe().removalResult = {
        StravaTokenPublication::RemovalStatus::StorageFailure,
        QStringLiteral("Synthetic local cleanup failure.")
    };
    StravaAccountRemoval::RevocationToken revokedToken =
        StravaAccountRemoval::RevocationToken::RefreshToken;

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [&revokedToken](
                const QString &,
                StravaAccountRemoval::RevocationToken tokenType,
                const StravaAccountRemoval::CancellationCheck &) {
                revokedToken = tokenType;
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
        revokedToken,
        StravaAccountRemoval::RevocationToken::AccessToken);
}

void TestStravaAccountRemoval::
remoteTokenDoesNotReplaceLocalCasToken()
{
    const QString account =
        uniqueAccount(QStringLiteral("separate-cas-token"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");

    const StravaTokenRefreshResult durable{
        true,
        request.accessToken,
        request.refreshToken,
        QString(),
        request.refreshToken,
        QString()
    };
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, durable));

    StravaTokenRefreshResult unpublished;
    unpublished.accessToken =
        QStringLiteral("synthetic-rotated-access-token");
    unpublished.refreshToken =
        QStringLiteral("synthetic-rotated-refresh-token");
    unpublished.error =
        QStringLiteral("Synthetic publication failure.");
    unpublished.remoteGrantMayHaveRotated = true;
    const StravaTokenRefreshResult failed =
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                request.refreshToken,
                request.accessToken,
                [&](const QString &) {
                    return unpublished;
                });
    QVERIFY(!failed.isValid());
    request.expectedAuthorizationEpoch =
        StravaTokenRefreshCoordinator::authorizationEpoch(
            account);

    QString remoteToken;
    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [&](const QString &token,
                StravaAccountRemoval::RevocationToken,
                const StravaAccountRemoval::CancellationCheck &) {
                remoteToken = token;
                return StravaAccountRemoval::
                    RemoteRevocationResult{
                        true, QString()
                    };
            });

    QVERIFY(result.isSuccess());
    QCOMPARE(remoteToken, unpublished.refreshToken);
    QCOMPARE(
        publisherProbe().removalRequest.expectedRefreshToken,
        request.refreshToken);
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
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(publisherProbe().markCalls, 1);
    QCOMPARE(publisherProbe().removalCalls, 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
}

void TestStravaAccountRemoval::
throwingRemoteRevocationReportsUncertainty()
{
    const QString account =
        uniqueAccount(QStringLiteral("throwing-revocation"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [](const QString &,
               StravaAccountRemoval::RevocationToken,
               const StravaAccountRemoval::CancellationCheck &)
                -> StravaAccountRemoval::RemoteRevocationResult {
                throw std::runtime_error(
                    "Synthetic revocation exception.");
            });

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(publisherProbe().markCalls, 1);
    QCOMPARE(publisherProbe().removalCalls, 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);
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

void TestStravaAccountRemoval::
cancellationAfterPendingCannotAbortRemoteRevocation()
{
    const QString account =
        uniqueAccount(QStringLiteral(
            "cancel-after-pending"));
    StravaAccountRemoval::Request request =
        localRequest(account);
    request.mode =
        StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    std::atomic<bool> cancelled{false};
    std::atomic<int> remoteCalls{0};
    std::atomic<bool> remoteSawCancellation{true};
    std::atomic<int> irreversibleCalls{0};
    std::atomic<bool> irreversibleSawPending{false};
    publisherProbe().markHook = [&] {
        cancelled = true;
    };

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            [&] { return cancelled.load(); },
            [&](const QString &,
                StravaAccountRemoval::RevocationToken,
                const StravaAccountRemoval::CancellationCheck
                    &operationCancelled) {
                ++remoteCalls;
                remoteSawCancellation =
                    operationCancelled
                    && operationCancelled();
                return StravaAccountRemoval::
                    RemoteRevocationResult{
                        true, QString()
                    };
            },
            [&] {
                ++irreversibleCalls;
                irreversibleSawPending =
                    publisherProbe().markCalls == 1
                    && publisherProbe().removalCalls == 0;
            });

    QVERIFY2(
        result.isSuccess(),
        "A disconnect committed as pending must finish even "
        "if cancellation arrives at that boundary.");
    QCOMPARE(remoteCalls.load(), 1);
    QVERIFY(!remoteSawCancellation.load());
    QCOMPARE(irreversibleCalls.load(), 1);
    QVERIFY(irreversibleSawPending.load());
    QCOMPARE(publisherProbe().removalCalls, 1);
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
