#include "Cloud/StravaTokenPublication.h"

#include <QTest>

#include <stdexcept>

namespace {

using namespace StravaTokenPublication;

TokenPair pair(const QString &label)
{
    return {
        QStringLiteral("access-") + label,
        QStringLiteral("refresh-") + label
    };
}

struct FakeStore
{
    TokenPair stored;
    QStringList calls;
    bool failRefresh = false;
    bool failAccess = false;
    bool failTimestamp = false;
    bool clearRefreshOnFailure = false;
    bool clearAccessOnFailure = false;
    bool timestampPresent = true;

    PublicationCallbacks callbacks()
    {
        return {
            [this] {
                calls.append(QStringLiteral("read"));
                return stored;
            },
            [this](const QString &value) {
                calls.append(QStringLiteral("refresh"));
                if (failRefresh) {
                    if (value.isEmpty() && clearRefreshOnFailure)
                        stored.refreshToken.clear();
                    return false;
                }
                stored.refreshToken = value;
                return true;
            },
            [this](const QString &value) {
                calls.append(QStringLiteral("access"));
                if (failAccess) {
                    if (value.isEmpty() && clearAccessOnFailure)
                        stored.accessToken.clear();
                    return false;
                }
                stored.accessToken = value;
                return true;
            },
            [this](const QString &value) {
                calls.append(QStringLiteral("timestamp"));
                if (failTimestamp) return false;
                timestampPresent = !value.isEmpty();
                return true;
            }
        };
    }
};

} // namespace

class TestStravaTokenPublication : public QObject
{
    Q_OBJECT

private slots:
    void compareAndSwapWritesRefreshTokenFirst();
    void conflictDoesNotWrite();
    void currentPairIsRewrittenToConfirmDurability();
    void refreshFailureStopsPublication();
    void accessFailureCanBeRetriedWithoutConflict();
    void timestampFailureCanBeRetriedWithoutConflict();
    void authoritativePublicationReplacesUnrelatedGrant();
    void removalClearsCredentialsAndTimestamp();
    void alreadyClearedRemovalCanBeRetried();
    void removalConflictDoesNotWrite();
    void pendingVaultCleanupStillRemovesLogicalCredentials();
    void accessibleCredentialAfterRemovalFailureFailsClosed();
    void invalidInputsDoNotInvokeCallbacks();
    void callbackExceptionFailsClosed();
};

void TestStravaTokenPublication::
compareAndSwapWritesRefreshTokenFirst()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    const TokenPair replacement = pair(QStringLiteral("new"));

    const PublicationResult result = publish(
        store.stored.refreshToken,
        replacement,
        QStringLiteral("timestamp-new"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, PublicationStatus::Saved);
    QCOMPARE(store.stored.accessToken, replacement.accessToken);
    QCOMPARE(store.stored.refreshToken, replacement.refreshToken);
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh"),
            QStringLiteral("access"),
            QStringLiteral("timestamp")
        }));
}

void TestStravaTokenPublication::conflictDoesNotWrite()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("current"));
    const TokenPair replacement = pair(QStringLiteral("replacement"));

    const PublicationResult result = publish(
        QStringLiteral("refresh-stale"),
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QCOMPARE(result.status, PublicationStatus::Conflict);
    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(store.stored.accessToken, QStringLiteral("access-current"));
    QCOMPARE(store.stored.refreshToken, QStringLiteral("refresh-current"));
    QCOMPARE(store.calls, QStringList({QStringLiteral("read")}));
}

void TestStravaTokenPublication::
currentPairIsRewrittenToConfirmDurability()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("current"));

    const PublicationResult result = publish(
        QStringLiteral("refresh-previous"),
        store.stored,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, PublicationStatus::AlreadyCurrent);
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh"),
            QStringLiteral("access"),
            QStringLiteral("timestamp")
        }));
}

void TestStravaTokenPublication::refreshFailureStopsPublication()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    store.failRefresh = true;

    const PublicationResult result = publish(
        store.stored.refreshToken,
        pair(QStringLiteral("new")),
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QCOMPARE(result.status, PublicationStatus::StorageFailure);
    QVERIFY(!result.isSuccess());
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh")
        }));
}

void TestStravaTokenPublication::
accessFailureCanBeRetriedWithoutConflict()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    const QString expectedRefreshToken = store.stored.refreshToken;
    const TokenPair replacement = pair(QStringLiteral("new"));
    store.failAccess = true;

    const PublicationResult first = publish(
        expectedRefreshToken,
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());
    QCOMPARE(first.status, PublicationStatus::StorageFailure);
    QCOMPARE(store.stored.refreshToken, replacement.refreshToken);
    QCOMPARE(store.stored.accessToken, QStringLiteral("access-old"));

    store.failAccess = false;
    store.calls.clear();
    const PublicationResult retry = publish(
        expectedRefreshToken,
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(retry.isSuccess());
    QCOMPARE(retry.status, PublicationStatus::Saved);
    QCOMPARE(store.stored.accessToken, replacement.accessToken);
    QCOMPARE(store.stored.refreshToken, replacement.refreshToken);
}

void TestStravaTokenPublication::
timestampFailureCanBeRetriedWithoutConflict()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    const QString expectedRefreshToken = store.stored.refreshToken;
    const TokenPair replacement = pair(QStringLiteral("new"));
    store.failTimestamp = true;

    const PublicationResult first = publish(
        expectedRefreshToken,
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());
    QCOMPARE(first.status, PublicationStatus::StorageFailure);
    QCOMPARE(store.stored.accessToken, replacement.accessToken);
    QCOMPARE(store.stored.refreshToken, replacement.refreshToken);

    store.failTimestamp = false;
    store.calls.clear();
    const PublicationResult retry = publish(
        expectedRefreshToken,
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(retry.isSuccess());
    QCOMPARE(retry.status, PublicationStatus::AlreadyCurrent);
}

void TestStravaTokenPublication::
authoritativePublicationReplacesUnrelatedGrant()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("unrelated"));
    const TokenPair replacement = pair(QStringLiteral("authorized"));

    const PublicationResult result = publish(
        QStringLiteral("refresh-stale"),
        replacement,
        QStringLiteral("timestamp"),
        PublicationMode::Authoritative,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, PublicationStatus::Saved);
    QCOMPARE(store.stored.accessToken, replacement.accessToken);
    QCOMPARE(store.stored.refreshToken, replacement.refreshToken);
}

void TestStravaTokenPublication::
removalClearsCredentialsAndTimestamp()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("current"));

    const RemovalResult result = remove(
        store.stored.refreshToken,
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, RemovalStatus::Cleared);
    QVERIFY(store.stored.accessToken.isEmpty());
    QVERIFY(store.stored.refreshToken.isEmpty());
    QVERIFY(!store.timestampPresent);
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh"),
            QStringLiteral("access"),
            QStringLiteral("timestamp"),
            QStringLiteral("read")
        }));
}

void TestStravaTokenPublication::
alreadyClearedRemovalCanBeRetried()
{
    FakeStore store;
    store.stored = {};

    const RemovalResult result = remove(
        QStringLiteral("refresh-previous"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, RemovalStatus::Cleared);
    QVERIFY(store.stored.accessToken.isEmpty());
    QVERIFY(store.stored.refreshToken.isEmpty());
    QVERIFY(!store.timestampPresent);
}

void TestStravaTokenPublication::removalConflictDoesNotWrite()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("newer"));

    const RemovalResult result = remove(
        QStringLiteral("refresh-stale"),
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(!result.isSuccess());
    QCOMPARE(result.status, RemovalStatus::Conflict);
    QCOMPARE(
        store.stored.accessToken,
        QStringLiteral("access-newer"));
    QCOMPARE(
        store.stored.refreshToken,
        QStringLiteral("refresh-newer"));
    QVERIFY(store.timestampPresent);
    QCOMPARE(
        store.calls,
        QStringList({QStringLiteral("read")}));
}

void TestStravaTokenPublication::
pendingVaultCleanupStillRemovesLogicalCredentials()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("current"));
    store.failRefresh = true;
    store.failAccess = true;
    store.clearRefreshOnFailure = true;
    store.clearAccessOnFailure = true;

    const RemovalResult result = remove(
        store.stored.refreshToken,
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(result.isSuccess());
    QCOMPARE(result.status, RemovalStatus::CleanupPending);
    QVERIFY(store.stored.accessToken.isEmpty());
    QVERIFY(store.stored.refreshToken.isEmpty());
    QVERIFY(!store.timestampPresent);
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh"),
            QStringLiteral("access"),
            QStringLiteral("timestamp"),
            QStringLiteral("read")
        }));
}

void TestStravaTokenPublication::
accessibleCredentialAfterRemovalFailureFailsClosed()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("current"));
    store.failRefresh = true;

    const RemovalResult result = remove(
        store.stored.refreshToken,
        PublicationMode::CompareAndSwap,
        store.callbacks());

    QVERIFY(!result.isSuccess());
    QCOMPARE(result.status, RemovalStatus::StorageFailure);
    QVERIFY(!store.stored.refreshToken.isEmpty());
    QVERIFY(store.stored.accessToken.isEmpty());
    QVERIFY(!store.timestampPresent);
    QCOMPARE(
        store.calls,
        QStringList({
            QStringLiteral("read"),
            QStringLiteral("refresh"),
            QStringLiteral("access"),
            QStringLiteral("timestamp"),
            QStringLiteral("read")
        }));
}

void TestStravaTokenPublication::
invalidInputsDoNotInvokeCallbacks()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    PublicationCallbacks callbacks = store.callbacks();

    const PublicationResult missingExpected = publish(
        QString(),
        pair(QStringLiteral("new")),
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        callbacks);
    const PublicationResult missingAccess = publish(
        store.stored.refreshToken,
        {QString(), QStringLiteral("refresh-new")},
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        callbacks);
    const PublicationResult missingCallback = publish(
        store.stored.refreshToken,
        pair(QStringLiteral("new")),
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        {});

    QCOMPARE(missingExpected.status, PublicationStatus::InvalidInput);
    QCOMPARE(missingAccess.status, PublicationStatus::InvalidInput);
    QCOMPARE(missingCallback.status, PublicationStatus::InvalidInput);
    QVERIFY(store.calls.isEmpty());
}

void TestStravaTokenPublication::callbackExceptionFailsClosed()
{
    FakeStore store;
    store.stored = pair(QStringLiteral("old"));
    PublicationCallbacks callbacks = store.callbacks();
    callbacks.readCurrent = []() -> TokenPair {
        throw std::runtime_error("synthetic publication exception");
    };

    const PublicationResult result = publish(
        store.stored.refreshToken,
        pair(QStringLiteral("new")),
        QStringLiteral("timestamp"),
        PublicationMode::CompareAndSwap,
        callbacks);

    QCOMPARE(result.status, PublicationStatus::StorageFailure);
    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(store.calls.isEmpty());
}

QTEST_APPLESS_MAIN(TestStravaTokenPublication)

#include "testStravaTokenPublication.moc"
