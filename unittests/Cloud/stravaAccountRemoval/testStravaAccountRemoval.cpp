#include "Cloud/StravaAccountRemoval.h"
#include "Cloud/StravaCredentialPublisher.h"
#include "Cloud/StravaTokenRefresh.h"
#include "Core/Settings.h"
#include "StravaPublisherTestSettings.h"

#include <QDir>
#include <QTest>
#include <QTemporaryDir>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

QString uniqueAccount(const QString &label)
{
    return label + QLatin1Char('-')
        + QUuid::createUuid().toString(
            QUuid::WithoutBraces);
}

StravaAccountRemoval::Request localRequest(
    const QString &account)
{
    StravaPublisherTestSettings::seedAuthorization(
        account,
        QStringLiteral("synthetic-access-token"),
        QStringLiteral("synthetic-refresh-token"));
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
    void remoteSuccessWithVaultConflictReportsUncertainty();
    void accessTokenSuccessSurvivesLocalCleanupFailure();
    void remoteTokenDoesNotReplaceLocalCasToken();
    void staleDisconnectCannotRemoveNewGrant();
    void failedPendingPersistenceDoesNotRevoke();
    void throwingRemoteRevocationReportsUncertainty();
    void cancellationBeforeStartDoesNotMutateStorage();
    void cancellationAfterPendingCannotAbortRemoteRevocation();
    void invalidModeFailsClosed();
    void unavailableVaultReadCannotReportLocalDisconnect();
    void requestCreationFailureRestoresRetryableAuthorization();
    void metadataSnapshotWaitsForDurabilityLease();
    void snapshotDeadlineCoversNativeKeychainTimeout();
};

void TestStravaAccountRemoval::
snapshotDeadlineCoversNativeKeychainTimeout()
{
    QVERIFY(
        StravaCredentialPublisher::CredentialSnapshotTimeoutMs
        >= 30000);
}

void TestStravaAccountRemoval::
metadataSnapshotWaitsForDurabilityLease()
{
    const QString account =
        uniqueAccount(QStringLiteral("metadata-lease"));
    localRequest(account);
    QString error;
    const auto mutation = StravaCredentialPublisher::beginMutation(
        account,
        StravaCredentialDurability::MutationKind::Refresh,
        2000,
        error);
    QVERIFY2(mutation, qPrintable(error));

    auto snapshot = std::async(std::launch::async, [account] {
        return StravaCredentialPublisher::
            readStoredAuthorizationMetadata(account, 2000);
    });
    QCOMPARE(
        snapshot.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);

    QVERIFY(mutation->finishNoChange(error));
    const auto metadata = snapshot.get();
    QVERIFY(metadata.readable);
    QCOMPARE(metadata.state, QStringLiteral("active"));
    QVERIFY(!metadata.revision.isEmpty());
}

void TestStravaAccountRemoval::
unavailableVaultReadCannotReportLocalDisconnect()
{
    const QString account =
        uniqueAccount(QStringLiteral("local-vault-unavailable"));
    StravaPublisherTestSettings::setCredentialReadsAvailable(false);

    const StravaAccountRemoval::Result unavailable =
        StravaAccountRemoval::execute(localRequest(account));

    QVERIFY(!unavailable.isSuccess());
    QVERIFY(!unavailable.disconnected);
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 0);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(account),
        StravaAuthorizationStatus::Active);

    StravaPublisherTestSettings::setCredentialReadsAvailable(true);
    const StravaAccountRemoval::Result retried =
        StravaAccountRemoval::execute(localRequest(account));
    QVERIFY(retried.isSuccess());
    QVERIFY(retried.disconnected);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 2);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaAccountRemoval::
requestCreationFailureRestoresRetryableAuthorization()
{
    const QString account =
        uniqueAccount(QStringLiteral("request-not-created"));
    StravaAccountRemoval::Request request = localRequest(account);
    request.mode = StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret = QStringLiteral("synthetic-client-secret");

    const StravaAccountRemoval::Result notCreated =
        StravaAccountRemoval::execute(
            request,
            {},
            [](const QString &,
               StravaAccountRemoval::RevocationToken,
               const StravaAccountRemoval::CancellationCheck &) {
                return StravaAccountRemoval::RemoteRevocationResult{
                    false,
                    QStringLiteral("Synthetic request creation failure."),
                    false
                };
            });
    QVERIFY(!notCreated.isSuccess());
    QVERIFY(!notCreated.disconnected);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(account),
        StravaAuthorizationStatus::Active);

    request.expectedAuthorizationEpoch =
        StravaTokenRefreshCoordinator::authorizationEpoch(account);
    const StravaAccountRemoval::Result retried =
        StravaAccountRemoval::execute(
            request,
            {},
            [](const QString &,
               StravaAccountRemoval::RevocationToken,
               const StravaAccountRemoval::CancellationCheck &) {
                return StravaAccountRemoval::RemoteRevocationResult{
                    true, QString(), true
                };
            });
    QVERIFY(retried.isSuccess());
    QVERIFY(retried.disconnected);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 2);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaAccountRemoval::init()
{
    StravaPublisherTestSettings::reset();
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 1);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 2);
    QVERIFY(StravaPublisherTestSettings::value(
        account, GC_STRAVA_TOKEN).toString().isEmpty());
    QVERIFY(StravaPublisherTestSettings::value(
        account, GC_STRAVA_REFRESH_TOKEN).toString().isEmpty());
    QCOMPARE(
        StravaPublisherTestSettings::value(
            account, GC_STRAVA_AUTHORIZATION_STATE).toString(),
        QStringLiteral("revoked"));
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
    StravaPublisherTestSettings::setRevokedStateWriteFailure(true);

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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 0);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    StravaPublisherTestSettings::setValue(
        account, GC_STRAVA_REFRESH_TOKEN, QVariant());
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
    StravaPublisherTestSettings::setTokenRemovalFailure(true);

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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 1);
    QVERIFY(StravaPublisherTestSettings::tokenRemovalWrites() > 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);
}

void TestStravaAccountRemoval::
remoteSuccessWithVaultConflictReportsUncertainty()
{
    const QString account =
        uniqueAccount(QStringLiteral("vault-conflict"));
    StravaAccountRemoval::Request request = localRequest(account);
    request.mode = StravaAccountRemoval::Mode::RevokeRemote;
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            request,
            {},
            [account](const QString &,
               StravaAccountRemoval::RevocationToken,
               const StravaAccountRemoval::CancellationCheck &) {
                StravaPublisherTestSettings::setValue(
                    account,
                    GC_STRAVA_REFRESH_TOKEN,
                    QStringLiteral("synthetic-newer-refresh-token"));
                return StravaAccountRemoval::RemoteRevocationResult{
                    true, QString()
                };
            });

    QVERIFY(!result.isSuccess());
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    StravaPublisherTestSettings::setValue(
        account, GC_STRAVA_REFRESH_TOKEN, QVariant());
    request.clientId = QStringLiteral("83");
    request.clientSecret =
        QStringLiteral("synthetic-client-secret");
    StravaPublisherTestSettings::setTokenRemovalFailure(true);
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 1);
    QVERIFY(StravaPublisherTestSettings::tokenRemovalWrites() > 0);
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
    const QString fencedRefresh =
        QStringLiteral("synthetic-vault-refresh-token");
    StravaPublisherTestSettings::setValue(
        account, GC_STRAVA_REFRESH_TOKEN, fencedRefresh);

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
    QCOMPARE(remoteToken, fencedRefresh);
    QVERIFY(StravaPublisherTestSettings::value(
        account, GC_STRAVA_REFRESH_TOKEN).toString().isEmpty());
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 0);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    StravaPublisherTestSettings::setPendingStateWriteFailure(true);

    const StravaAccountRemoval::Result result =
        StravaAccountRemoval::execute(
            localRequest(account));

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.remoteAuthorizationMayRemain);
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 1);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 1);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 0);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
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
    StravaPublisherTestSettings::setPendingStateHook(
        [&] { cancelled = true; });

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
                    StravaPublisherTestSettings::pendingStateWrites() == 1
                    && StravaPublisherTestSettings::tokenRemovalWrites() == 0;
            });

    QVERIFY2(
        result.isSuccess(),
        "A disconnect committed as pending must finish even "
        "if cancellation arrives at that boundary.");
    QCOMPARE(remoteCalls.load(), 1);
    QVERIFY(!remoteSawCancellation.load());
    QCOMPARE(irreversibleCalls.load(), 1);
    QVERIFY(irreversibleSawPending.load());
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 2);
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
    QCOMPARE(StravaPublisherTestSettings::pendingStateWrites(), 0);
    QCOMPARE(StravaPublisherTestSettings::tokenRemovalWrites(), 0);
}

QTEST_MAIN(TestStravaAccountRemoval)
#include "testStravaAccountRemoval.moc"
