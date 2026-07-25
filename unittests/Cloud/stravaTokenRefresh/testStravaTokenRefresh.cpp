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
    return {true, accessToken, refreshToken, QString()};
}

StravaTokenRefreshResult failedResult(const QString &error)
{
    return {false, QString(), QString(), error};
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
    gate.condition.wait(lock, [&gate] {
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
    void followerCancellationIsPrompt();
    void cachedResultExpires();
    void invalidationClearsCompletedCache();
    void invalidationSupersedesActiveLeader();
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
    QVERIFY2(!replacementStarted.load(),
             "The same account started two refresh operations concurrently.");
    release(gate);

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
