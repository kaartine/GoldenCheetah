#include <QtTest>

#include "EstimatorThreadControl.h"

#include <atomic>

namespace {

class CooperativeWorker final : public QThread
{
public:
    explicit CooperativeWorker(EstimatorThreadControl &control)
        : control_(control)
    {
    }

    std::atomic_bool entered{false};
    std::atomic_bool exited{false};
    std::atomic_bool requestStayedSet{false};

protected:
    void run() override
    {
        entered.store(true, std::memory_order_release);
        while (!control_.stopRequested()) msleep(1);
        msleep(20);
        requestStayedSet.store(
            control_.stopRequested(), std::memory_order_release);
        exited.store(true, std::memory_order_release);
    }

private:
    EstimatorThreadControl &control_;
};

class NaturalWorker final : public QThread
{
protected:
    void run() override { msleep(20); }
};

} // namespace

class TestEstimatorThreadControl : public QObject
{
    Q_OBJECT

private slots:
    void stopWaitsForWorkerExit();
    void naturalCompletionDoesNotPoisonRestart();
};

void TestEstimatorThreadControl::stopWaitsForWorkerExit()
{
    EstimatorThreadControl control;
    CooperativeWorker worker(control);
    QVERIFY(control.prepareForStart(worker));
    worker.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        worker.entered.load(std::memory_order_acquire), 1000);
    QVERIFY(!control.prepareForStart(worker));

    QVERIFY(control.stopAndWait(worker));

    QVERIFY(!worker.isRunning());
    QVERIFY(worker.exited.load(std::memory_order_acquire));
    QVERIFY(worker.requestStayedSet.load(std::memory_order_acquire));
    QVERIFY(!control.stopRequested());
}

void TestEstimatorThreadControl::naturalCompletionDoesNotPoisonRestart()
{
    EstimatorThreadControl control;
    NaturalWorker firstWorker;
    QVERIFY(control.prepareForStart(firstWorker));
    firstWorker.start();

    QVERIFY(control.stopAndWait(firstWorker));
    QVERIFY(!control.stopRequested());

    CooperativeWorker secondWorker(control);
    QVERIFY(control.prepareForStart(secondWorker));
    secondWorker.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        secondWorker.entered.load(std::memory_order_acquire), 1000);
    QTest::qWait(20);
    QVERIFY(secondWorker.isRunning());
    QVERIFY(control.stopAndWait(secondWorker));
    QVERIFY(secondWorker.exited.load(std::memory_order_acquire));
}

QTEST_GUILESS_MAIN(TestEstimatorThreadControl)
#include "testEstimatorThreadControl.moc"
