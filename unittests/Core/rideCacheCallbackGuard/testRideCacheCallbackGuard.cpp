#include <QtTest>

#include "RideCacheCallbackGuard.h"

class TestRideCacheCallbackGuard : public QObject
{
    Q_OBJECT

private slots:
    void callbackOwnerDestructionStopsContinuation();
};

void TestRideCacheCallbackGuard::
callbackOwnerDestructionStopsContinuation()
{
    auto *cache = new QObject;
    QObject context;
    QObject ride;
    QObject estimator;
    const RideCacheCallbackGuard guard(
        cache, &context, &ride, &estimator);

    delete cache;

    int continuationCount = 0;
    if (guard.allAlive())
        ++continuationCount;
    QCOMPARE(continuationCount, 0);
    QVERIFY(!guard.allAlive());
}

QTEST_GUILESS_MAIN(TestRideCacheCallbackGuard)

#include "testRideCacheCallbackGuard.moc"
