#include <QtTest>

#include "AthleteRefreshLifecycle.h"

#include <QThread>
#include <QDir>
#include <QFile>
#include <QSemaphore>

#define GC_STRINGIFY_IMPL(value) #value
#define GC_STRINGIFY(value) GC_STRINGIFY_IMPL(value)

class TestAthleteRefreshLifecycle : public QObject
{
    Q_OBJECT

private slots:
    void transitionQuiescesBeforeClosingAndResumesOnce();
    void transitionSynchronouslyJoinsParticipantWorker();
    void shutdownIsPermanent();
    void wrongThreadCannotChangeLifecycle();
    void productionWiringStartsBarrierBeforeWrites();
};

namespace {

class LatchedWorker final : public QThread
{
public:
    QSemaphore entered;
    QSemaphore release;

protected:
    void run() override
    {
        entered.release();
        release.acquire();
    }
};

} // namespace

void TestAthleteRefreshLifecycle::
transitionQuiescesBeforeClosingAndResumesOnce()
{
    AthleteRefreshLifecycle lifecycle;
    int quiesceCount = 0;
    int resumeCount = 0;
    bool admittedInsideQuiesce = true;
    int identity = 0;

    QVERIFY(lifecycle.registerParticipant({
        &identity,
        [&]() {
            admittedInsideQuiesce = lifecycle.admitsWork();
            ++quiesceCount;
        },
        [&]() { ++resumeCount; }
    }));

    QVERIFY(lifecycle.beginConfigTransition());
    QVERIFY(!admittedInsideQuiesce);
    QVERIFY(!lifecycle.admitsWork());
    QCOMPARE(quiesceCount, 1);
    QVERIFY(!lifecycle.registerParticipant({
        reinterpret_cast<void *>(quintptr(1)), []() {}, {}}));

    QVERIFY(lifecycle.beginConfigTransition());
    QCOMPARE(quiesceCount, 1);
    QVERIFY(lifecycle.finishConfigTransition());
    QCOMPARE(resumeCount, 0);
    QVERIFY(!lifecycle.admitsWork());
    QVERIFY(lifecycle.finishConfigTransition());
    QCOMPARE(resumeCount, 1);
    QVERIFY(lifecycle.admitsWork());
    QVERIFY(!lifecycle.finishConfigTransition());
}

void TestAthleteRefreshLifecycle::shutdownIsPermanent()
{
    AthleteRefreshLifecycle lifecycle;
    int quiesceCount = 0;
    int identity = 0;
    QVERIFY(lifecycle.registerParticipant(
        {&identity, [&]() { ++quiesceCount; }, {}}));

    QVERIFY(lifecycle.beginShutdown());
    QCOMPARE(quiesceCount, 1);
    QVERIFY(lifecycle.shutdownStarted());
    QVERIFY(!lifecycle.admitsWork());
    QVERIFY(!lifecycle.beginConfigTransition());
    QVERIFY(!lifecycle.finishConfigTransition());
    QVERIFY(lifecycle.beginShutdown());
    QCOMPARE(quiesceCount, 1);
    QVERIFY(lifecycle.unregisterParticipant(&identity));
}

void TestAthleteRefreshLifecycle::
transitionSynchronouslyJoinsParticipantWorker()
{
    AthleteRefreshLifecycle lifecycle;
    LatchedWorker worker;
    int identity = 0;
    bool joined = false;
    QVERIFY(lifecycle.registerParticipant({
        &identity,
        [&]() {
            worker.requestInterruption();
            worker.release.release();
            worker.wait();
            joined = true;
        },
        {}
    }));

    worker.start();
    QVERIFY(worker.entered.tryAcquire(1, 5000));
    QVERIFY(worker.isRunning());
    QVERIFY(lifecycle.beginConfigTransition());
    QVERIFY(joined);
    QVERIFY(!worker.isRunning());
    QVERIFY(!lifecycle.admitsWork());
    QVERIFY(lifecycle.finishConfigTransition());
}

void TestAthleteRefreshLifecycle::wrongThreadCannotChangeLifecycle()
{
    AthleteRefreshLifecycle lifecycle;
    bool transitionResult = true;
    bool shutdownResult = true;
    QThread worker;
    QObject probe;
    probe.moveToThread(&worker);
    connect(&worker, &QThread::started, &probe, [&]() {
        transitionResult = lifecycle.beginConfigTransition();
        shutdownResult = lifecycle.beginShutdown();
        worker.quit();
    });

    worker.start();
    QVERIFY(worker.wait(5000));
    QVERIFY(!transitionResult);
    QVERIFY(!shutdownResult);
    QVERIFY(lifecycle.admitsWork());
}

void TestAthleteRefreshLifecycle::
productionWiringStartsBarrierBeforeWrites()
{
    const auto source = [](const QString &relativePath) {
        QFile file(QDir(QString::fromUtf8(GC_STRINGIFY(GC_TEST_SOURCE_ROOT)))
                       .filePath(relativePath));
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return file.readAll();
    };
    const QByteArray athleteDialog = source(
        QStringLiteral("src/Gui/AthleteConfigDialog.cpp"));
    const QByteArray globalDialog = source(
        QStringLiteral("src/Gui/ConfigDialog.cpp"));
    const QByteArray context = source(
        QStringLiteral("src/Core/Context.cpp"));
    const QByteArray cache = source(
        QStringLiteral("src/Core/RideCache.cpp"));
    QVERIFY(!athleteDialog.isEmpty());
    QVERIFY(!globalDialog.isEmpty());
    QVERIFY(!context.isEmpty());
    QVERIFY(!cache.isEmpty());

    const qsizetype athleteBegin = athleteDialog.indexOf(
        "context->beginConfigTransition()");
    const qsizetype athleteWrite = athleteDialog.indexOf(
        "changed |= athlete->saveClicked();");
    QVERIFY(athleteBegin >= 0);
    QVERIFY(athleteWrite > athleteBegin);

    const qsizetype globalBegin = globalDialog.indexOf(
        "GlobalContext::context()->beginConfigTransition()");
    const qsizetype globalWrite = globalDialog.indexOf(
        "changed |= general->saveClicked();");
    QVERIFY(globalBegin >= 0);
    QVERIFY(globalWrite > globalBegin);
    const qsizetype restartSurvivorCheck = globalDialog.indexOf(
        "if (mainwindows.isEmpty())");
    const qsizetype restartCancelled = globalDialog.indexOf(
        "restarting = false;", restartSurvivorCheck);
    const qsizetype notifyAfterRestart = globalDialog.indexOf(
        "notifyConfigChanged(changed);", restartSurvivorCheck);
    const qsizetype finishAfterRestart = globalDialog.indexOf(
        "finishConfigTransition();", notifyAfterRestart);
    QVERIFY(restartSurvivorCheck > globalWrite);
    QVERIFY(restartCancelled > restartSurvivorCheck);
    QVERIFY(notifyAfterRestart > restartCancelled);
    QVERIFY(finishAfterRestart > notifyAfterRestart);

    const qsizetype globalNotify = context.indexOf(
        "notifyConfigChanged(qint32 state)");
    const qsizetype fallbackBegin = context.indexOf(
        "beginConfigTransition()", globalNotify);
    const qsizetype globalReplacement = context.indexOf(
        "readConfig(state);", globalNotify);
    QVERIFY(globalNotify >= 0);
    QVERIFY(fallbackBegin >= 0);
    QVERIFY(globalReplacement > fallbackBegin);

    QVERIFY(cache.contains("quiesceForConfigTransition();"));
    QVERIFY(cache.contains("if (estimator) estimator->stop();"));
    QVERIFY(cache.contains("cancel();"));
    QVERIFY(!cache.contains("estimator->terminate()"));

    const QByteArray estimator = source(
        QStringLiteral("src/Metrics/Estimator.cpp"));
    QVERIFY(estimator.count(
        "refreshLifecycle().admitsWork()") >= 2);
    QVERIFY(estimator.contains("DeferredRequest::Lazy"));
    QVERIFY(estimator.contains("DeferredRequest::Immediate"));
    QVERIFY(cache.contains("takeDeferredRequest()"));

    const qsizetype broadcast = context.indexOf(
        "p->notifyConfigChanged(state);");
    const qsizetype globalFinish = context.indexOf(
        "finishConfigTransition();", broadcast);
    QVERIFY(broadcast >= 0);
    QVERIFY(globalFinish > broadcast);

    const QByteArray aggregate = source(
        QStringLiteral("unittests/unittests.pro"));
    QVERIFY(aggregate.contains("Core/athleteRefreshLifecycle"));
}

QTEST_GUILESS_MAIN(TestAthleteRefreshLifecycle)

#include "testAthleteRefreshLifecycle.moc"
