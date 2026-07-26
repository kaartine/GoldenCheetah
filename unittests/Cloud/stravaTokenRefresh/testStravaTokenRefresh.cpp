#include "Cloud/StravaTokenRefresh.h"

#include <QTest>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct OperationGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
    int entered = 0;
};

QString uniqueValue(const QString &prefix)
{
    return prefix + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

StravaTokenRefreshResult successfulResult(
    const QString &accessToken,
    const QString &refreshToken)
{
    return {
        true, accessToken, refreshToken, QString(), QString(),
        QString()
    };
}

StravaTokenRefreshResult failedResult(const QString &error)
{
    return {
        false, QString(), QString(), error, QString(), QString()
    };
}

StravaAuthorizationRemovalResult successfulRemoval(
    bool cleanupPending = false)
{
    return {
        true, cleanupPending, QString()
    };
}

StravaAuthorizationRemovalResult failedRemoval(
    const QString &error)
{
    return {
        false, false, error
    };
}

bool waitForEntered(OperationGate &gate,
                    int expected,
                    std::chrono::milliseconds timeout = 1s)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.condition.wait_for(
        lock, timeout,
        [&gate, expected] {
            return gate.entered >= expected;
        });
}

void release(OperationGate &gate)
{
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();
}

void enterAndWait(OperationGate &gate)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    ++gate.entered;
    gate.condition.notify_all();
    gate.condition.wait_for(lock, 5s, [&gate] {
        return gate.released;
    });
}

void compareResult(const StravaTokenRefreshResult &actual,
                   const StravaTokenRefreshResult &expected)
{
    QCOMPARE(actual.success, expected.success);
    QCOMPARE(actual.accessToken, expected.accessToken);
    QCOMPARE(actual.refreshToken, expected.refreshToken);
    QCOMPARE(actual.error, expected.error);
    QCOMPARE(actual.isValid(), expected.isValid());
}

} // namespace

class TestStravaTokenRefresh : public QObject
{
    Q_OBJECT

private slots:
    void sameAccountConcurrentCallersExecuteOperationOnce();
    void followersReceiveIdenticalRotatingTokenPair();
    void cacheMatchesOriginalAndRotatedRefreshToken();
    void unrelatedReplacementTokenForcesNewOperation();
    void sameAccountReplacementTokenWaitsForActiveRefresh();
    void differentAccountsRefreshConcurrently();
    void failedResultsAreNotCached();
    void failedPublicationBlocksRequestGrant();
    void followerCancellationIsPrompt();
    void cachedResultExpires();
    void expiredCacheUsesLatestRotatedToken();
    void invalidationClearsCompletedCache();
    void invalidationSupersedesActiveLeader();
    void authorizationInstallSupersedesActiveLeader();
    void durableAuthorizationInstallBlocksOtherGrantChanges();
    void failedDurableInstallKeepsPendingState();
    void rejectedAccessConcurrentCallersRefreshOnce();
    void lateRejectedAccessCallerReusesNewerGrant();
    void rejectingCurrentRotatedAccessRefreshesAgain();
    void rejectedAccessRefreshesDifferentAccountsConcurrently();
    void authorizationRemovalWaitsForRotatingRefresh();
    void authorizationRemovalPersistsPendingAfterRefreshDrain();
    void authorizationRemovalBlocksNewRefreshes();
    void failedPendingPersistenceRestoresActiveState();
    void failedPendingPersistenceKeepsObservedRotation();
    void authorizationRemovalDrainsActiveRequests();
    void authorizationRemovalRejectsUndispatchedRequest();
    void failedAuthorizationRemovalStaysFailClosed();
    void successfulAuthorizationRemovalInvalidatesGrant();
    void removalWaitCanBeCancelledPromptly();
    void explicitAuthorizationReopensPendingAccount();
    void persistedNonActiveStateCannotBeReopenedByStaleClone();
    void installedGrantSupersedesStaleCloneState();
    void authorizationSnapshotCannotTearTokenPairAndEpoch();
    void oauthCompletionCannotCrossRemovalEpoch();
    void ambiguousRefreshKeepsRemoteWarning();
    void persistedPendingAuthorizationRetainsRevocationEvidence();
    void storedAuthorizationStateFailsClosed_data();
    void storedAuthorizationStateFailsClosed();
    void thrownOperationFailsClosed();
    void invalidInputAndIncompleteSuccessDoNotPolluteCache();
};

void TestStravaTokenRefresh::
sameAccountConcurrentCallersExecuteOperationOnce()
{
    const QString account = uniqueValue(QStringLiteral("singleflight"));
    const QString inputToken = uniqueValue(QStringLiteral("refresh-in"));
    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-out")),
        uniqueValue(QStringLiteral("refresh-out")));
    OperationGate gate;
    std::atomic<int> calls{0};

    auto operation = [&] {
        ++calls;
        enterAndWait(gate);
        return expected;
    };

    std::vector<std::future<StravaTokenRefreshResult>> callers;
    callers.emplace_back(std::async(
        std::launch::async,
        [&] {
            return StravaTokenRefreshCoordinator::refresh(
                account, inputToken, operation);
        }));
    QVERIFY2(waitForEntered(gate, 1),
             "The leader refresh operation did not start.");

    for (int index = 0; index < 7; ++index) {
        callers.emplace_back(std::async(
            std::launch::async,
            [&] {
                return StravaTokenRefreshCoordinator::refresh(
                    account, inputToken, operation);
            }));
    }

    QThread::msleep(80);
    release(gate);

    for (auto &caller : callers) {
        compareResult(caller.get(), expected);
    }
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::
followersReceiveIdenticalRotatingTokenPair()
{
    const QString account = uniqueValue(QStringLiteral("followers"));
    const QString inputToken = uniqueValue(QStringLiteral("refresh-in"));
    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-rotated")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    OperationGate gate;
    std::atomic<int> calls{0};

    auto operation = [&] {
        ++calls;
        enterAndWait(gate);
        return expected;
    };

    auto leader = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, inputToken, operation);
    });
    QVERIFY2(waitForEntered(gate, 1),
             "The leader refresh operation did not start.");

    auto follower = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, inputToken, operation);
    });

    QThread::msleep(50);
    release(gate);

    const StravaTokenRefreshResult leaderResult = leader.get();
    const StravaTokenRefreshResult followerResult = follower.get();
    compareResult(leaderResult, expected);
    compareResult(followerResult, leaderResult);
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::
cacheMatchesOriginalAndRotatedRefreshToken()
{
    const QString account = uniqueValue(QStringLiteral("cache-alias"));
    const QString originalToken =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-cached")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    std::atomic<int> calls{0};
    auto operation = [&] {
        ++calls;
        return expected;
    };

    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, originalToken, operation),
        expected);

    auto mustNotRun = [&] {
        ++calls;
        return failedResult(QStringLiteral(
            "A matching cached result was not reused."));
    };
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, originalToken, mustNotRun),
        expected);
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, expected.refreshToken, mustNotRun),
        expected);
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::
unrelatedReplacementTokenForcesNewOperation()
{
    const QString account =
        uniqueValue(QStringLiteral("replacement-token"));
    const QString originalToken =
        uniqueValue(QStringLiteral("refresh-original"));
    const QString unrelatedToken =
        uniqueValue(QStringLiteral("refresh-replacement"));
    std::atomic<int> calls{0};

    const auto first = StravaTokenRefreshCoordinator::refresh(
        account, originalToken, [&] {
            ++calls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-first")),
                uniqueValue(QStringLiteral("refresh-first")));
        });
    const auto second = StravaTokenRefreshCoordinator::refresh(
        account, unrelatedToken, [&] {
            ++calls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-second")),
                uniqueValue(QStringLiteral("refresh-second")));
        });

    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(first.accessToken != second.accessToken);
    QVERIFY(first.refreshToken != second.refreshToken);
    QCOMPARE(calls.load(), 2);
}

void TestStravaTokenRefresh::
sameAccountReplacementTokenWaitsForActiveRefresh()
{
    const QString account =
        uniqueValue(QStringLiteral("serialized-replacement"));
    const QString firstToken =
        uniqueValue(QStringLiteral("refresh-first"));
    const QString replacementToken =
        uniqueValue(QStringLiteral("refresh-replacement"));
    OperationGate gate;
    std::atomic<int> calls{0};
    std::atomic<bool> replacementStarted{false};

    auto first = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, firstToken, [&] {
                ++calls;
                enterAndWait(gate);
                return successfulResult(
                    uniqueValue(QStringLiteral("access-first")),
                    uniqueValue(QStringLiteral("refresh-first-out")));
            });
    });
    QVERIFY2(waitForEntered(gate, 1),
             "The first refresh operation did not start.");

    auto replacement = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, replacementToken, [&] {
                ++calls;
                replacementStarted = true;
                return successfulResult(
                    uniqueValue(QStringLiteral("access-replacement")),
                    uniqueValue(QStringLiteral("refresh-replacement-out")));
            });
    });

    QThread::msleep(80);
    const bool serialized = !replacementStarted.load();
    release(gate);

    QVERIFY2(serialized,
             "The same account started two refresh operations concurrently.");
    QVERIFY(first.get().isValid());
    QVERIFY(replacement.get().isValid());
    QVERIFY(replacementStarted.load());
    QCOMPARE(calls.load(), 2);
}

void TestStravaTokenRefresh::
differentAccountsRefreshConcurrently()
{
    const QString firstAccount =
        uniqueValue(QStringLiteral("parallel-account-a"));
    const QString secondAccount =
        uniqueValue(QStringLiteral("parallel-account-b"));
    const QString inputToken =
        uniqueValue(QStringLiteral("parallel-refresh"));
    OperationGate gate;
    std::atomic<bool> overlapped{false};

    auto operation = [&](const QString &label) {
        return [&, label] {
            {
                std::unique_lock<std::mutex> lock(gate.mutex);
                ++gate.entered;
                gate.condition.notify_all();
                const bool bothEntered = gate.condition.wait_for(
                    lock, 1s,
                    [&gate] {
                        return gate.entered >= 2;
                    });
                if (bothEntered) {
                    overlapped = true;
                }
            }
            return successfulResult(
                uniqueValue(QStringLiteral("access-") + label),
                uniqueValue(QStringLiteral("refresh-") + label));
        };
    };

    auto first = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            firstAccount, inputToken, operation(QStringLiteral("a")));
    });
    auto second = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            secondAccount, inputToken, operation(QStringLiteral("b")));
    });

    QVERIFY(first.get().isValid());
    QVERIFY(second.get().isValid());
    QVERIFY2(overlapped.load(),
             "Refresh operations for different accounts were serialized.");
}

void TestStravaTokenRefresh::failedResultsAreNotCached()
{
    const QString account =
        uniqueValue(QStringLiteral("failure-cache"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    std::atomic<int> calls{0};

    const auto failure = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return failedResult(QStringLiteral("Synthetic failure."));
        });
    QVERIFY(!failure.isValid());

    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-retry")),
        uniqueValue(QStringLiteral("refresh-retry")));
    const auto retry = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return expected;
        });

    compareResult(retry, expected);
    QCOMPARE(calls.load(), 2);
}

void TestStravaTokenRefresh::
failedPublicationBlocksRequestGrant()
{
    const QString account =
        uniqueValue(QStringLiteral("failed-publication-grant"));
    const StravaTokenRefreshResult durable = successfulResult(
        uniqueValue(QStringLiteral("access-durable")),
        uniqueValue(QStringLiteral("refresh-durable")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, durable));

    StravaTokenRefreshResult unpublished = failedResult(
        QStringLiteral("Synthetic publication conflict."));
    unpublished.accessToken =
        uniqueValue(QStringLiteral("access-unpublished"));
    unpublished.refreshToken =
        uniqueValue(QStringLiteral("refresh-unpublished"));
    unpublished.remoteGrantMayHaveRotated = true;
    const StravaTokenRefreshResult failed =
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                durable.refreshToken,
                durable.accessToken,
                [&](const QString &) {
                    return unpublished;
                });
    QVERIFY(!failed.isValid());

    auto request =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(!request.isValid());
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);

    QString removedToken;
    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            durable.refreshToken,
            [] { return true; },
            [&](const QString &effectiveRefreshToken) {
                removedToken = effectiveRefreshToken;
                return successfulRemoval();
            });
    QVERIFY(removed.isSuccess());
    QCOMPARE(removedToken, unpublished.refreshToken);
    QVERIFY(removed.remoteAuthorizationMayRemain);
}

void TestStravaTokenRefresh::followerCancellationIsPrompt()
{
    const QString account =
        uniqueValue(QStringLiteral("cancel-follower"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-out")),
        uniqueValue(QStringLiteral("refresh-out")));
    OperationGate gate;
    std::atomic<int> calls{0};

    auto operation = [&] {
        ++calls;
        enterAndWait(gate);
        return expected;
    };
    auto leader = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, inputToken, operation);
    });
    QVERIFY2(waitForEntered(gate, 1),
             "The leader refresh operation did not start.");

    const auto started = std::chrono::steady_clock::now();
    auto follower = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            inputToken,
            operation,
            [] {
                return true;
            });
    });
    const auto status = follower.wait_for(250ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    release(gate);
    const StravaTokenRefreshResult followerResult = follower.get();
    const StravaTokenRefreshResult leaderResult = leader.get();

    QCOMPARE(status, std::future_status::ready);
    QVERIFY2(elapsed < 250ms,
             "A cancelled follower waited for the leader operation.");
    QVERIFY(!followerResult.isValid());
    QVERIFY(!followerResult.error.isEmpty());
    compareResult(leaderResult, expected);
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::cachedResultExpires()
{
    const QString account =
        uniqueValue(QStringLiteral("cache-expiry"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    std::atomic<int> calls{0};
    auto operation = [&] {
        const int call = ++calls;
        return successfulResult(
            QStringLiteral("access-%1").arg(call),
            QStringLiteral("refresh-%1").arg(call));
    };
    const auto neverCancelled = [] {
        return false;
    };

    const auto first = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, operation, neverCancelled, 30ms);
    QVERIFY(first.isValid());
    QThread::msleep(80);
    const auto second = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, operation, neverCancelled, 30ms);

    QVERIFY(second.isValid());
    QCOMPARE(calls.load(), 2);
    QVERIFY(first.accessToken != second.accessToken);
    QVERIFY(first.refreshToken != second.refreshToken);
}

void TestStravaTokenRefresh::expiredCacheUsesLatestRotatedToken()
{
    const QString account =
        uniqueValue(QStringLiteral("canonical-token"));
    const QString originalToken =
        uniqueValue(QStringLiteral("refresh-original"));
    QStringList submittedTokens;
    int calls = 0;
    auto operation = [&](const QString &effectiveToken) {
        submittedTokens.append(effectiveToken);
        ++calls;
        return successfulResult(
            QStringLiteral("access-%1").arg(calls),
            QStringLiteral("refresh-%1").arg(calls));
    };
    const auto neverCancelled = [] {
        return false;
    };

    const auto first = StravaTokenRefreshCoordinator::refresh(
        account, originalToken, operation, neverCancelled, 30ms);
    QVERIFY(first.isValid());
    QThread::msleep(80);
    const auto second = StravaTokenRefreshCoordinator::refresh(
        account, originalToken, operation, neverCancelled, 30ms);

    QVERIFY(second.isValid());
    QCOMPARE(calls, 2);
    QCOMPARE(submittedTokens.size(), 2);
    QCOMPARE(submittedTokens.at(0), originalToken);
    QCOMPARE(submittedTokens.at(1), first.refreshToken);
    QCOMPARE(first.sourceRefreshToken, originalToken);
    QCOMPARE(second.sourceRefreshToken, first.refreshToken);
}

void TestStravaTokenRefresh::invalidationClearsCompletedCache()
{
    const QString account =
        uniqueValue(QStringLiteral("invalidate-cache"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    std::atomic<int> calls{0};
    auto operation = [&] {
        const int call = ++calls;
        return successfulResult(
            QStringLiteral("access-%1").arg(call),
            QStringLiteral("refresh-%1").arg(call));
    };

    const auto first = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, operation);
    QVERIFY(first.isValid());
    QCOMPARE(calls.load(), 1);

    StravaTokenRefreshCoordinator::invalidate(account);

    const auto second = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, operation);
    QVERIFY(second.isValid());
    QCOMPARE(calls.load(), 2);
    QVERIFY(first.accessToken != second.accessToken);
    QVERIFY(first.refreshToken != second.refreshToken);
}

void TestStravaTokenRefresh::invalidationSupersedesActiveLeader()
{
    const QString account =
        uniqueValue(QStringLiteral("invalidate-flight"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    const StravaTokenRefreshResult supersededResult = successfulResult(
        uniqueValue(QStringLiteral("access-superseded")),
        uniqueValue(QStringLiteral("refresh-superseded")));
    OperationGate gate;
    std::atomic<int> calls{0};

    auto leader = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, inputToken, [&] {
                ++calls;
                enterAndWait(gate);
                return supersededResult;
            });
    });
    QVERIFY2(waitForEntered(gate, 1),
             "The leader refresh operation did not start.");

    auto invalidator = std::async(std::launch::async, [&] {
        StravaTokenRefreshCoordinator::invalidate(account);
    });
    const auto invalidateStatus = invalidator.wait_for(250ms);
    release(gate);
    invalidator.get();

    const StravaTokenRefreshResult invalidatedLeader = leader.get();
    QCOMPARE(invalidateStatus, std::future_status::ready);
    QVERIFY(!invalidatedLeader.success);
    QVERIFY(!invalidatedLeader.isValid());
    QVERIFY(!invalidatedLeader.error.isEmpty());
    QVERIFY(invalidatedLeader.accessToken.isEmpty());
    QVERIFY(invalidatedLeader.refreshToken.isEmpty());

    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-retry")),
        uniqueValue(QStringLiteral("refresh-retry")));
    const auto retry = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return expected;
        });
    compareResult(retry, expected);
    QCOMPARE(calls.load(), 2);
}

void TestStravaTokenRefresh::
authorizationInstallSupersedesActiveLeader()
{
    const QString account =
        uniqueValue(QStringLiteral("authorization-install"));
    const QString staleToken =
        uniqueValue(QStringLiteral("refresh-stale"));
    const StravaTokenRefreshResult staleResult = successfulResult(
        uniqueValue(QStringLiteral("access-stale")),
        uniqueValue(QStringLiteral("refresh-stale-out")));
    StravaTokenRefreshResult ambiguousStaleResult =
        staleResult;
    ambiguousStaleResult.remoteGrantMayHaveRotated = true;
    const StravaTokenRefreshResult authorizedResult = successfulResult(
        uniqueValue(QStringLiteral("access-authorized")),
        uniqueValue(QStringLiteral("refresh-authorized")));
    OperationGate gate;
    std::atomic<int> calls{0};

    auto leader = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account, staleToken, [&] {
                ++calls;
                enterAndWait(gate);
                return ambiguousStaleResult;
            });
    });
    QVERIFY2(waitForEntered(gate, 1),
             "The stale refresh operation did not start.");

    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, authorizedResult));
    release(gate);

    const StravaTokenRefreshResult staleLeader = leader.get();
    QVERIFY(!staleLeader.isValid());
    QVERIFY(!staleLeader.error.isEmpty());

    auto mustNotRun = [&] {
        ++calls;
        return failedResult(QStringLiteral(
            "The installed authorization was not reused."));
    };
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, staleToken, mustNotRun),
        authorizedResult);
    const StravaTokenRefreshResult installed =
        StravaTokenRefreshCoordinator::refresh(
            account, staleToken, mustNotRun);
    QCOMPARE(
        installed.sourceRefreshToken,
        authorizedResult.refreshToken);
    QCOMPARE(calls.load(), 1);

    QString removedToken;
    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            authorizedResult.refreshToken,
            [&](const QString &effectiveRefreshToken) {
                removedToken = effectiveRefreshToken;
                return successfulRemoval();
            });
    QVERIFY(removed.isSuccess());
    QVERIFY(removed.remoteAuthorizationMayRemain);
    QCOMPARE(
        removedToken,
        authorizedResult.refreshToken);
}

void TestStravaTokenRefresh::
durableAuthorizationInstallBlocksOtherGrantChanges()
{
    const QString account =
        uniqueValue(QStringLiteral("durable-install"));
    const StravaTokenRefreshResult replacement =
        successfulResult(
            uniqueValue(QStringLiteral("access-new")),
            uniqueValue(QStringLiteral("refresh-new")));
    OperationGate installGate;

    auto installation = std::async(
        std::launch::async,
        [&] {
            return StravaTokenRefreshCoordinator::
                installAuthorizationDurably(
                    account,
                    replacement,
                    [&] {
                        enterAndWait(installGate);
                        return true;
                    });
        });
    QVERIFY2(waitForEntered(installGate, 1),
             "The durable authorization install did not start.");
    QVERIFY(!StravaTokenRefreshCoordinator::
        authorizationUsable(account));

    std::atomic<int> refreshCalls{0};
    const StravaTokenRefreshResult blockedRefresh =
        StravaTokenRefreshCoordinator::refresh(
            account,
            replacement.refreshToken,
            [&] {
                ++refreshCalls;
                return replacement;
            });
    std::atomic<int> removalCalls{0};
    const StravaAuthorizationRemovalResult blockedRemoval =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            replacement.refreshToken,
            [&](const QString &) {
                ++removalCalls;
                return successfulRemoval();
            });

    QVERIFY(!blockedRefresh.isValid());
    QCOMPARE(refreshCalls.load(), 0);
    QVERIFY(!blockedRemoval.isSuccess());
    QCOMPARE(removalCalls.load(), 0);

    release(installGate);
    QVERIFY(installation.get());
    QVERIFY(StravaTokenRefreshCoordinator::
        authorizationUsable(account));
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account,
            replacement.refreshToken,
            [] {
                return failedResult(QStringLiteral(
                    "The installed grant was not reused."));
            }),
        replacement);
}

void TestStravaTokenRefresh::
failedDurableInstallKeepsPendingState()
{
    const QString account =
        uniqueValue(QStringLiteral("failed-durable-install"));
    StravaTokenRefreshCoordinator::initializeAuthorizationStatus(
        account,
        StravaAuthorizationStatus::RevocationPending);
    std::atomic<int> publicationCalls{0};

    const bool installed =
        StravaTokenRefreshCoordinator::
            installAuthorizationDurably(
                account,
                successfulResult(
                    uniqueValue(QStringLiteral("access-new")),
                    uniqueValue(QStringLiteral("refresh-new"))),
                [&] {
                    ++publicationCalls;
                    return false;
                });

    QVERIFY(!installed);
    QCOMPARE(publicationCalls.load(), 1);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);
    QVERIFY(!StravaTokenRefreshCoordinator::
        authorizationUsable(account));
}

void TestStravaTokenRefresh::
rejectedAccessConcurrentCallersRefreshOnce()
{
    const QString account =
        uniqueValue(QStringLiteral("rejected-concurrent"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult initial = successfulResult(
        uniqueValue(QStringLiteral("access-rejected")),
        uniqueValue(QStringLiteral("refresh-current")));
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, originalRefresh, [&] {
                return initial;
            }),
        initial);

    const StravaTokenRefreshResult expected = successfulResult(
        uniqueValue(QStringLiteral("access-replacement")),
        uniqueValue(QStringLiteral("refresh-replacement")));
    OperationGate gate;
    std::atomic<int> calls{0};
    auto operation = [&](const QString &) {
        ++calls;
        enterAndWait(gate);
        return expected;
    };

    std::vector<std::future<StravaTokenRefreshResult>> callers;
    callers.emplace_back(std::async(
        std::launch::async,
        [&] {
            return StravaTokenRefreshCoordinator::
                refreshAfterRejectedAccessToken(
                    account,
                    initial.refreshToken,
                    initial.accessToken,
                    operation);
        }));
    QVERIFY2(waitForEntered(gate, 1),
             "The rejected-token refresh did not start.");

    for (int index = 0; index < 7; ++index) {
        callers.emplace_back(std::async(
            std::launch::async,
            [&] {
                return StravaTokenRefreshCoordinator::
                    refreshAfterRejectedAccessToken(
                        account,
                        originalRefresh,
                        initial.accessToken,
                        operation);
            }));
    }

    QThread::msleep(80);
    release(gate);
    for (auto &caller : callers)
        compareResult(caller.get(), expected);
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::
lateRejectedAccessCallerReusesNewerGrant()
{
    const QString account =
        uniqueValue(QStringLiteral("rejected-late"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult initial = successfulResult(
        uniqueValue(QStringLiteral("access-rejected")),
        uniqueValue(QStringLiteral("refresh-current")));
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, originalRefresh, [&] {
                return initial;
            }),
        initial);

    const StravaTokenRefreshResult replacement = successfulResult(
        uniqueValue(QStringLiteral("access-replacement")),
        uniqueValue(QStringLiteral("refresh-replacement")));
    std::atomic<int> calls{0};
    compareResult(
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                initial.refreshToken,
                initial.accessToken,
                [&](const QString &) {
                    ++calls;
                    return replacement;
                }),
        replacement);

    const auto mustNotRun = [&](const QString &) {
        ++calls;
        return failedResult(QStringLiteral(
            "A late 401 started a second token rotation."));
    };
    compareResult(
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                originalRefresh,
                initial.accessToken,
                mustNotRun),
        replacement);
    QCOMPARE(calls.load(), 1);
}

void TestStravaTokenRefresh::
rejectingCurrentRotatedAccessRefreshesAgain()
{
    const QString account =
        uniqueValue(QStringLiteral("rejected-current"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult first = successfulResult(
        uniqueValue(QStringLiteral("access-first")),
        uniqueValue(QStringLiteral("refresh-first")));
    const StravaTokenRefreshResult second = successfulResult(
        uniqueValue(QStringLiteral("access-second")),
        uniqueValue(QStringLiteral("refresh-second")));
    const StravaTokenRefreshResult third = successfulResult(
        uniqueValue(QStringLiteral("access-third")),
        uniqueValue(QStringLiteral("refresh-third")));
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account, originalRefresh, [&] {
                return first;
            }),
        first);

    int calls = 0;
    const auto operation = [&](const QString &) {
        ++calls;
        return calls == 1 ? second : third;
    };
    compareResult(
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                first.refreshToken,
                first.accessToken,
                operation),
        second);
    compareResult(
        StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                account,
                second.refreshToken,
                second.accessToken,
                operation),
        third);
    QCOMPARE(calls, 2);
}

void TestStravaTokenRefresh::
rejectedAccessRefreshesDifferentAccountsConcurrently()
{
    const QString firstAccount =
        uniqueValue(QStringLiteral("rejected-account-a"));
    const QString secondAccount =
        uniqueValue(QStringLiteral("rejected-account-b"));
    const QString firstRefresh =
        uniqueValue(QStringLiteral("refresh-a"));
    const QString secondRefresh =
        uniqueValue(QStringLiteral("refresh-b"));
    const StravaTokenRefreshResult firstInitial = successfulResult(
        uniqueValue(QStringLiteral("access-a")),
        uniqueValue(QStringLiteral("refresh-a-current")));
    const StravaTokenRefreshResult secondInitial = successfulResult(
        uniqueValue(QStringLiteral("access-b")),
        uniqueValue(QStringLiteral("refresh-b-current")));
    QVERIFY(StravaTokenRefreshCoordinator::refresh(
        firstAccount, firstRefresh,
        [&] { return firstInitial; }).isValid());
    QVERIFY(StravaTokenRefreshCoordinator::refresh(
        secondAccount, secondRefresh,
        [&] { return secondInitial; }).isValid());

    OperationGate gate;
    std::atomic<bool> overlapped{false};
    const auto operation = [&](const QString &label) {
        return [&, label](const QString &) {
            {
                std::unique_lock<std::mutex> lock(gate.mutex);
                ++gate.entered;
                gate.condition.notify_all();
                if (gate.condition.wait_for(
                        lock, 1s,
                        [&] { return gate.entered >= 2; })) {
                    overlapped = true;
                }
            }
            return successfulResult(
                uniqueValue(
                    QStringLiteral("replacement-access-")
                    + label),
                uniqueValue(
                    QStringLiteral("replacement-refresh-")
                    + label));
        };
    };

    auto first = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                firstAccount,
                firstInitial.refreshToken,
                firstInitial.accessToken,
                operation(QStringLiteral("a")));
    });
    auto second = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            refreshAfterRejectedAccessToken(
                secondAccount,
                secondInitial.refreshToken,
                secondInitial.accessToken,
                operation(QStringLiteral("b")));
    });

    QVERIFY(first.get().isValid());
    QVERIFY(second.get().isValid());
    QVERIFY2(overlapped.load(),
             "Rejected-token refreshes for different accounts "
             "were serialized.");
}

void TestStravaTokenRefresh::
authorizationRemovalWaitsForRotatingRefresh()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-after-refresh"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult rotated = successfulResult(
        uniqueValue(QStringLiteral("access-rotated")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    OperationGate refreshGate;
    std::atomic<bool> removalStarted{false};
    QString removedToken;

    auto refresh = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            originalRefresh,
            [&](const QString &) {
                enterAndWait(refreshGate);
                return rotated;
            });
    });
    QVERIFY2(waitForEntered(refreshGate, 1),
             "The rotating refresh did not start.");

    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                originalRefresh,
                [&](const QString &effectiveRefreshToken) {
                    removalStarted = true;
                    removedToken = effectiveRefreshToken;
                    return successfulRemoval();
                });
    });

    QThread::msleep(80);
    const bool waitedForRefresh = !removalStarted.load();
    release(refreshGate);

    const StravaTokenRefreshResult blockedRefresh =
        refresh.get();
    QVERIFY(!blockedRefresh.isValid());
    QVERIFY(!blockedRefresh.error.isEmpty());
    const StravaAuthorizationRemovalResult result =
        removal.get();
    QVERIFY2(waitedForRefresh,
             "Authorization removal raced the active refresh.");
    QVERIFY(result.isSuccess());
    QCOMPARE(removedToken, rotated.refreshToken);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaTokenRefresh::
authorizationRemovalPersistsPendingAfterRefreshDrain()
{
    const QString account =
        uniqueValue(QStringLiteral("pending-after-refresh"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult rotated = successfulResult(
        uniqueValue(QStringLiteral("access-rotated")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    OperationGate refreshGate;
    std::atomic<bool> refreshOperationComplete{false};
    std::atomic<bool> pendingSawCompletedRefresh{false};
    std::atomic<int> pendingCalls{0};

    auto refresh = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            originalRefresh,
            [&](const QString &) {
                enterAndWait(refreshGate);
                refreshOperationComplete = true;
                return rotated;
            });
    });
    QVERIFY2(waitForEntered(refreshGate, 1),
             "The refresh operation did not start.");

    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                originalRefresh,
                [&] {
                    ++pendingCalls;
                    pendingSawCompletedRefresh =
                        refreshOperationComplete.load();
                    return true;
                },
                [](const QString &) {
                    return successfulRemoval();
                },
                {},
                5s);
    });

    QThread::msleep(80);
    const bool pendingStartedBeforeDrain =
        pendingCalls.load() != 0;
    release(refreshGate);
    const StravaTokenRefreshResult refreshResult =
        refresh.get();
    const StravaAuthorizationRemovalResult removalResult =
        removal.get();

    QVERIFY(!pendingStartedBeforeDrain);
    QVERIFY(pendingSawCompletedRefresh.load());
    QCOMPARE(pendingCalls.load(), 1);
    QVERIFY(!refreshResult.isValid());
    QVERIFY(removalResult.isSuccess());
}

void TestStravaTokenRefresh::
authorizationRemovalBlocksNewRefreshes()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-blocks-refresh"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        refreshToken);
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account,
            refreshToken,
            [&] { return current; }),
        current);

    OperationGate removalGate;
    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                refreshToken,
                [&](const QString &) {
                    enterAndWait(removalGate);
                    return successfulRemoval();
                });
    });
    QVERIFY2(waitForEntered(removalGate, 1),
             "Authorization removal did not start.");

    std::atomic<int> refreshCalls{0};
    const auto blocked = StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] {
            ++refreshCalls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-forbidden")),
                uniqueValue(QStringLiteral("refresh-forbidden")));
        });
    const bool installBlocked =
        !StravaTokenRefreshCoordinator::installAuthorization(
            account,
            successfulResult(
                uniqueValue(QStringLiteral("access-new")),
                uniqueValue(QStringLiteral("refresh-new"))));

    release(removalGate);
    QVERIFY(removal.get().isSuccess());
    QVERIFY(!blocked.isValid());
    QVERIFY(!blocked.error.isEmpty());
    QCOMPARE(refreshCalls.load(), 0);
    QVERIFY2(installBlocked,
             "A concurrent OAuth grant reopened an account mid-removal.");
}

void TestStravaTokenRefresh::
failedPendingPersistenceRestoresActiveState()
{
    const QString account =
        uniqueValue(QStringLiteral("pending-storage-failure"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        uniqueValue(QStringLiteral("refresh-current")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, current));
    std::atomic<int> pendingCalls{0};
    std::atomic<int> removalCalls{0};

    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            current.refreshToken,
            [&] {
                ++pendingCalls;
                return false;
            },
            [&](const QString &) {
                ++removalCalls;
                return successfulRemoval();
            });

    QVERIFY(!removed.isSuccess());
    QCOMPARE(pendingCalls.load(), 1);
    QCOMPARE(removalCalls.load(), 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
    QVERIFY(StravaTokenRefreshCoordinator::
        authorizationUsable(account));
    auto request =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(request.isValid());
    QCOMPARE(request.accessToken(), current.accessToken);
}

void TestStravaTokenRefresh::
failedPendingPersistenceKeepsObservedRotation()
{
    const QString account =
        uniqueValue(QStringLiteral("pending-rotation"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult rotated = successfulResult(
        uniqueValue(QStringLiteral("access-rotated")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    OperationGate refreshGate;
    OperationGate pendingGate;

    auto refresh = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            originalRefresh,
            [&](const QString &) {
                enterAndWait(refreshGate);
                return rotated;
            });
    });
    QVERIFY2(waitForEntered(refreshGate, 1),
             "The rotating refresh did not start.");

    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                originalRefresh,
                [&] {
                    enterAndWait(pendingGate);
                    return false;
                },
                [](const QString &) {
                    return successfulRemoval();
                });
    });
    QVERIFY2(waitForEntered(pendingGate, 1),
             "The pending-state write did not start.");

    release(refreshGate);
    const StravaTokenRefreshResult interruptedRefresh =
        refresh.get();
    QVERIFY(!interruptedRefresh.isValid());
    release(pendingGate);
    QVERIFY(!removal.get().isSuccess());

    QString submittedToken;
    const StravaTokenRefreshResult replacement =
        successfulResult(
            uniqueValue(QStringLiteral("access-replacement")),
            uniqueValue(QStringLiteral("refresh-replacement")));
    const StravaTokenRefreshResult retry =
        StravaTokenRefreshCoordinator::refresh(
            account,
            originalRefresh,
            [&](const QString &effectiveToken) {
                submittedToken = effectiveToken;
                return replacement;
            });

    QVERIFY(retry.isValid());
    QCOMPARE(submittedToken, rotated.refreshToken);
}

void TestStravaTokenRefresh::
authorizationRemovalDrainsActiveRequests()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-active-request"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        uniqueValue(QStringLiteral("refresh-current")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, current));

    auto request =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(request.isValid());
    QCOMPARE(request.accessToken(), current.accessToken);
    QVERIFY(request.authorizeDispatch());
    std::atomic<bool> abortCalled{false};
    request.setAbortOperation([&] {
        abortCalled = true;
    });
    std::atomic<bool> removalOperationEntered{false};

    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                current.refreshToken,
                [] { return true; },
                [&](const QString &) {
                    removalOperationEntered = true;
                    return successfulRemoval();
                });
    });

    QTRY_VERIFY_WITH_TIMEOUT(abortCalled.load(), 1000);
    QVERIFY(!removalOperationEntered.load());
    QVERIFY(!StravaTokenRefreshCoordinator::
        beginAuthorizedRequest(account).isValid());

    request.release();
    const StravaAuthorizationRemovalResult result =
        removal.get();
    QVERIFY(result.isSuccess());
    QVERIFY(removalOperationEntered.load());
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaTokenRefresh::
authorizationRemovalRejectsUndispatchedRequest()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-prepared-request"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        uniqueValue(QStringLiteral("refresh-current")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, current));

    auto request =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(request.isValid());
    std::atomic<int> removalCalls{0};
    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                current.refreshToken,
                [] { return true; },
                [&](const QString &) {
                    ++removalCalls;
                    return successfulRemoval();
                });
    });

    QTRY_COMPARE_WITH_TIMEOUT(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending,
        1000);
    QVERIFY(!request.authorizeDispatch());
    QVERIFY(removal.get().isSuccess());
    QCOMPARE(removalCalls.load(), 1);
}

void TestStravaTokenRefresh::
failedAuthorizationRemovalStaysFailClosed()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-failure"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        refreshToken);
    QVERIFY(StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] { return current; }).isValid());

    const auto failed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            refreshToken,
            [](const QString &) {
                return failedRemoval(
                    QStringLiteral("Synthetic revoke failure."));
            });
    QVERIFY(!failed.isSuccess());
    QVERIFY(!failed.error.isEmpty());
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);

    std::atomic<int> refreshCalls{0};
    const auto blocked = StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] {
            ++refreshCalls;
            return current;
        });
    QVERIFY(!blocked.isValid());
    QCOMPARE(refreshCalls.load(), 0);

    QString retriedToken;
    const auto retried =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            refreshToken,
            [&](const QString &effectiveRefreshToken) {
                retriedToken = effectiveRefreshToken;
                return successfulRemoval();
            });
    QVERIFY(retried.isSuccess());
    QCOMPARE(retriedToken, current.refreshToken);
}

void TestStravaTokenRefresh::
successfulAuthorizationRemovalInvalidatesGrant()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-success"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        refreshToken);
    QVERIFY(StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] { return current; }).isValid());

    QVERIFY(StravaTokenRefreshCoordinator::removeAuthorization(
        account,
        refreshToken,
        [](const QString &) {
            return successfulRemoval(true);
        }).isSuccess());

    std::atomic<int> calls{0};
    const auto blocked = StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] {
            ++calls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-stale")),
                uniqueValue(QStringLiteral("refresh-stale")));
        });
    QVERIFY(!blocked.isValid());
    QCOMPARE(calls.load(), 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Revoked);
}

void TestStravaTokenRefresh::
removalWaitCanBeCancelledPromptly()
{
    const QString account =
        uniqueValue(QStringLiteral("remove-cancel"));
    const QString originalRefresh =
        uniqueValue(QStringLiteral("refresh-original"));
    const StravaTokenRefreshResult rotated = successfulResult(
        uniqueValue(QStringLiteral("access-rotated")),
        uniqueValue(QStringLiteral("refresh-rotated")));
    OperationGate refreshGate;
    std::atomic<bool> cancelled{false};
    std::atomic<int> pendingCalls{0};
    std::atomic<int> removalCalls{0};

    auto refresh = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::refresh(
            account,
            originalRefresh,
            [&](const QString &) {
                enterAndWait(refreshGate);
                return rotated;
            });
    });
    QVERIFY2(waitForEntered(refreshGate, 1),
             "The refresh operation did not start.");

    const auto started = std::chrono::steady_clock::now();
    auto removal = std::async(std::launch::async, [&] {
        return StravaTokenRefreshCoordinator::
            removeAuthorization(
                account,
                originalRefresh,
                [&] {
                    ++pendingCalls;
                    return true;
                },
                [&](const QString &) {
                    ++removalCalls;
                    return successfulRemoval();
                },
                [&] { return cancelled.load(); },
                5s);
    });
    QThread::msleep(80);
    cancelled = true;
    const StravaAuthorizationRemovalResult result =
        removal.get();
    const auto elapsed =
        std::chrono::steady_clock::now() - started;

    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error.isEmpty());
    QVERIFY(elapsed < 1s);
    QCOMPARE(pendingCalls.load(), 0);
    QCOMPARE(removalCalls.load(), 0);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);

    release(refreshGate);
    const StravaTokenRefreshResult completedRefresh =
        refresh.get();
    QVERIFY(completedRefresh.isValid());
    compareResult(completedRefresh, rotated);
}

void TestStravaTokenRefresh::
explicitAuthorizationReopensPendingAccount()
{
    const QString account =
        uniqueValue(QStringLiteral("reauthorize-pending"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    const StravaTokenRefreshResult current = successfulResult(
        uniqueValue(QStringLiteral("access-current")),
        refreshToken);
    QVERIFY(StravaTokenRefreshCoordinator::refresh(
        account,
        refreshToken,
        [&] { return current; }).isValid());
    QVERIFY(!StravaTokenRefreshCoordinator::removeAuthorization(
        account,
        refreshToken,
        [](const QString &) {
            return failedRemoval(
                QStringLiteral("Synthetic revoke failure."));
        }).isSuccess());

    const StravaTokenRefreshResult replacement = successfulResult(
        uniqueValue(QStringLiteral("access-replacement")),
        uniqueValue(QStringLiteral("refresh-replacement")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, replacement));
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);

    std::atomic<int> calls{0};
    compareResult(
        StravaTokenRefreshCoordinator::refresh(
            account,
            replacement.refreshToken,
            [&] {
                ++calls;
                return failedResult(QStringLiteral(
                    "The installed grant was not reused."));
            }),
        replacement);
    QCOMPARE(calls.load(), 0);
}

void TestStravaTokenRefresh::
persistedNonActiveStateCannotBeReopenedByStaleClone()
{
    const QString account =
        uniqueValue(QStringLiteral("persisted-pending"));
    StravaTokenRefreshCoordinator::initializeAuthorizationStatus(
        account,
        StravaAuthorizationStatus::RevocationPending);
    StravaTokenRefreshCoordinator::initializeAuthorizationStatus(
        account,
        StravaAuthorizationStatus::Active);

    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::RevocationPending);
    QVERIFY(!StravaTokenRefreshCoordinator::authorizationUsable(
        account));

    std::atomic<int> calls{0};
    const auto blocked = StravaTokenRefreshCoordinator::refresh(
        account,
        uniqueValue(QStringLiteral("refresh-stale")),
        [&] {
            ++calls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-stale")),
                uniqueValue(QStringLiteral("refresh-stale")));
        });
    QVERIFY(!blocked.isValid());
    QCOMPARE(calls.load(), 0);
}

void TestStravaTokenRefresh::
installedGrantSupersedesStaleCloneState()
{
    const QString account =
        uniqueValue(QStringLiteral("stale-clone"));
    const StravaTokenRefreshResult oldGrant = successfulResult(
        uniqueValue(QStringLiteral("access-old")),
        uniqueValue(QStringLiteral("refresh-old")));
    const StravaTokenRefreshResult newGrant = successfulResult(
        uniqueValue(QStringLiteral("access-new")),
        uniqueValue(QStringLiteral("refresh-new")));
    StravaTokenRefreshCoordinator::initializeAuthorization(
        account,
        StravaAuthorizationStatus::Active,
        oldGrant.accessToken,
        oldGrant.refreshToken);

    auto initial =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(initial.isValid());
    QCOMPARE(initial.accessToken(), oldGrant.accessToken);
    initial.release();

    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, newGrant));
    StravaTokenRefreshCoordinator::initializeAuthorization(
        account,
        StravaAuthorizationStatus::Revoked,
        oldGrant.accessToken,
        oldGrant.refreshToken);

    auto current =
        StravaTokenRefreshCoordinator::
            beginAuthorizedRequest(account);
    QVERIFY(current.isValid());
    QCOMPARE(current.accessToken(), newGrant.accessToken);
    QCOMPARE(
        StravaTokenRefreshCoordinator::authorizationStatus(
            account),
        StravaAuthorizationStatus::Active);
}

void TestStravaTokenRefresh::
authorizationSnapshotCannotTearTokenPairAndEpoch()
{
    const QString account =
        uniqueValue(QStringLiteral("authorization-snapshot"));
    const StravaTokenRefreshResult initial =
        successfulResult(
            QStringLiteral("access-0"),
            QStringLiteral("refresh-0"));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, initial));

    std::atomic<bool> writerDone{false};
    auto writer = std::async(std::launch::async, [&] {
        bool installed = true;
        for (int index = 1; index <= 500; ++index) {
            const QString suffix = QString::number(index);
            const StravaTokenRefreshResult replacement =
                successfulResult(
                    QStringLiteral("access-") + suffix,
                    QStringLiteral("refresh-") + suffix);
            if (!StravaTokenRefreshCoordinator::
                    installAuthorization(
                        account, replacement)) {
                installed = false;
                break;
            }
        }
        writerDone = true;
        return installed;
    });

    std::uint64_t previousEpoch = 0;
    do {
        const StravaAuthorizationSnapshot snapshot =
            StravaTokenRefreshCoordinator::
                authorizationSnapshot(account);
        QCOMPARE(
            snapshot.status,
            StravaAuthorizationStatus::Active);
        QCOMPARE(
            snapshot.accessToken.section(
                QLatin1Char('-'), -1),
            snapshot.refreshToken.section(
                QLatin1Char('-'), -1));
        QVERIFY(snapshot.epoch >= previousEpoch);
        previousEpoch = snapshot.epoch;
    } while (!writerDone.load());

    QVERIFY(writer.get());
}

void TestStravaTokenRefresh::
oauthCompletionCannotCrossRemovalEpoch()
{
    const QString account =
        uniqueValue(QStringLiteral("oauth-removal-epoch"));
    const StravaTokenRefreshResult oldGrant = successfulResult(
        uniqueValue(QStringLiteral("access-old")),
        uniqueValue(QStringLiteral("refresh-old")));
    const StravaTokenRefreshResult newGrant = successfulResult(
        uniqueValue(QStringLiteral("access-new")),
        uniqueValue(QStringLiteral("refresh-new")));
    QVERIFY(StravaTokenRefreshCoordinator::installAuthorization(
        account, oldGrant));
    const std::uint64_t oldEpoch =
        StravaTokenRefreshCoordinator::authorizationEpoch(
            account);
    QVERIFY(StravaTokenRefreshCoordinator::removeAuthorization(
        account,
        oldGrant.refreshToken,
        [] { return true; },
        [](const QString &) {
            return successfulRemoval();
        }).isSuccess());

    std::atomic<int> stalePublicationCalls{0};
    QVERIFY(!StravaTokenRefreshCoordinator::
        installAuthorizationDurably(
            account,
            newGrant,
            oldEpoch,
            [&] {
                ++stalePublicationCalls;
                return true;
            }));
    QCOMPARE(stalePublicationCalls.load(), 0);

    const std::uint64_t newEpoch =
        StravaTokenRefreshCoordinator::authorizationEpoch(
            account);
    std::atomic<int> currentPublicationCalls{0};
    QVERIFY(StravaTokenRefreshCoordinator::
        installAuthorizationDurably(
            account,
            newGrant,
            newEpoch,
            [&] {
                ++currentPublicationCalls;
                return true;
            }));
    QCOMPARE(currentPublicationCalls.load(), 1);
}

void TestStravaTokenRefresh::
ambiguousRefreshKeepsRemoteWarning()
{
    const QString account =
        uniqueValue(QStringLiteral("ambiguous-rotation"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    StravaTokenRefreshResult ambiguous = failedResult(
        QStringLiteral("The refresh reply was lost."));
    ambiguous.remoteGrantMayHaveRotated = true;

    const StravaTokenRefreshResult refresh =
        StravaTokenRefreshCoordinator::refresh(
            account,
            refreshToken,
            [&] { return ambiguous; });
    QVERIFY(!refresh.isValid());

    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            refreshToken,
            [] { return true; },
            [](const QString &) {
                return successfulRemoval();
            });
    QVERIFY(removed.isSuccess());
    QVERIFY(removed.remoteAuthorizationMayRemain);
}

void TestStravaTokenRefresh::
persistedPendingAuthorizationRetainsRevocationEvidence()
{
    const QString account =
        uniqueValue(QStringLiteral("persisted-uncertain"));
    const QString accessToken =
        uniqueValue(QStringLiteral("access-current"));
    const QString refreshToken =
        uniqueValue(QStringLiteral("refresh-current"));
    StravaTokenRefreshCoordinator::initializeAuthorization(
        account,
        StravaAuthorizationStatus::RevocationPending,
        accessToken,
        refreshToken,
        true);

    const StravaAuthorizationSnapshot snapshot =
        StravaTokenRefreshCoordinator::authorizationSnapshot(
            account);
    QCOMPARE(
        snapshot.status,
        StravaAuthorizationStatus::RevocationPending);
    QCOMPARE(snapshot.accessToken, accessToken);
    QCOMPARE(snapshot.refreshToken, refreshToken);
    QVERIFY(snapshot.remoteGrantMayHaveRotated);
    QVERIFY(!StravaTokenRefreshCoordinator::
        beginAuthorizedRequest(account).isValid());

    QString revokedToken;
    const StravaAuthorizationRemovalResult removed =
        StravaTokenRefreshCoordinator::removeAuthorization(
            account,
            snapshot.refreshToken,
            [] { return true; },
            [&revokedToken](const QString &effectiveRefreshToken) {
                revokedToken = effectiveRefreshToken;
                return successfulRemoval();
            });
    QVERIFY(removed.isSuccess());
    QCOMPARE(revokedToken, refreshToken);
    QVERIFY(removed.remoteAuthorizationMayRemain);
}

void TestStravaTokenRefresh::
storedAuthorizationStateFailsClosed_data()
{
    QTest::addColumn<QString>("stored");
    QTest::addColumn<StravaAuthorizationStatus>("expected");

    QTest::newRow("legacy-empty")
        << QString()
        << StravaAuthorizationStatus::Active;
    QTest::newRow("active")
        << QStringLiteral("active")
        << StravaAuthorizationStatus::Active;
    QTest::newRow("pending")
        << QStringLiteral("revocation_pending")
        << StravaAuthorizationStatus::RevocationPending;
    QTest::newRow("revoked")
        << QStringLiteral("revoked")
        << StravaAuthorizationStatus::Revoked;
    QTest::newRow("unknown")
        << QStringLiteral("partially_written")
        << StravaAuthorizationStatus::RevocationPending;
    QTest::newRow("whitespace")
        << QStringLiteral(" ")
        << StravaAuthorizationStatus::RevocationPending;
}

void TestStravaTokenRefresh::
storedAuthorizationStateFailsClosed()
{
    QFETCH(QString, stored);
    QFETCH(StravaAuthorizationStatus, expected);

    QCOMPARE(
        StravaTokenRefreshCoordinator::
            authorizationStatusFromStorage(stored),
        expected);
}

void TestStravaTokenRefresh::thrownOperationFailsClosed()
{
    const QString account =
        uniqueValue(QStringLiteral("throwing-operation"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    std::atomic<int> calls{0};

    const auto failure = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&]() -> StravaTokenRefreshResult {
            ++calls;
            throw std::runtime_error("synthetic refresh exception");
        });

    QVERIFY(!failure.success);
    QVERIFY(!failure.isValid());
    QVERIFY(!failure.error.isEmpty());
    QVERIFY(failure.accessToken.isEmpty());
    QVERIFY(failure.refreshToken.isEmpty());

    const auto retry = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return successfulResult(
                uniqueValue(QStringLiteral("access-retry")),
                uniqueValue(QStringLiteral("refresh-retry")));
        });
    QVERIFY(retry.isValid());
    QCOMPARE(calls.load(), 2);
}

void TestStravaTokenRefresh::
invalidInputAndIncompleteSuccessDoNotPolluteCache()
{
    const QString account =
        uniqueValue(QStringLiteral("invalid-input"));
    const QString inputToken =
        uniqueValue(QStringLiteral("refresh-in"));
    std::atomic<int> calls{0};
    auto operation = [&] {
        ++calls;
        return successfulResult(
            uniqueValue(QStringLiteral("access-out")),
            uniqueValue(QStringLiteral("refresh-out")));
    };

    const auto emptyAccount = StravaTokenRefreshCoordinator::refresh(
        QString(), inputToken, operation);
    const auto emptyToken = StravaTokenRefreshCoordinator::refresh(
        account, QString(), operation);
    QVERIFY(!emptyAccount.isValid());
    QVERIFY(!emptyToken.isValid());
    QVERIFY(!emptyAccount.error.isEmpty());
    QVERIFY(!emptyToken.error.isEmpty());
    QCOMPARE(calls.load(), 0);

    const auto missingAccess = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return StravaTokenRefreshResult{
                true,
                QString(),
                uniqueValue(QStringLiteral("refresh-incomplete")),
                QString(),
                QString(),
                QString()};
        });
    QVERIFY(!missingAccess.isValid());

    const auto missingRefresh = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, [&] {
            ++calls;
            return StravaTokenRefreshResult{
                true,
                uniqueValue(QStringLiteral("access-incomplete")),
                QString(),
                QString(),
                QString(),
                QString()};
        });
    QVERIFY(!missingRefresh.isValid());

    const auto valid = StravaTokenRefreshCoordinator::refresh(
        account, inputToken, operation);
    QVERIFY(valid.isValid());
    QCOMPARE(calls.load(), 3);
}

QTEST_APPLESS_MAIN(TestStravaTokenRefresh)

#include "testStravaTokenRefresh.moc"
