/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "ANT.h"
#include "ANTlocalController.h"
#include "LibUsb.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

namespace {

void finishWorker(ANT *worker)
{
    if (!worker) return;
    worker->stop();
    if (worker->isRunning()) QVERIFY(worker->wait(2000));
}

}

class TestAntLifecycle : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        FakeAntTransport::reset();
    }

    void controllerOwnsIdleWorker()
    {
        ANTlocalController *controller =
                new ANTlocalController(nullptr, nullptr);
        QPointer<ANT> worker = controller->myANTlocal;

        delete controller;

        const bool destroyedWithController = worker.isNull();
        if (!worker.isNull()) delete worker.data();

        QVERIFY2(destroyedWithController,
                 "ANT worker outlived its owning controller");
        QCOMPARE(FakeAntTransport::snapshot().liveInstances, 0);
    }

    void workerOwnsChannels()
    {
        ANT *worker = new ANT;
        QList<QPointer<ANTChannel>> channels;
        for (int index = 0; index < ANT_MAX_CHANNELS; ++index)
            channels.append(worker->antChannel[index]);

        delete worker;

        bool everyChannelDestroyed = true;
        for (const QPointer<ANTChannel> &channel : channels)
            everyChannelDestroyed &= channel.isNull();

        // Keep the RED path leak-free before reporting the ownership failure.
        for (const QPointer<ANTChannel> &channel : channels) {
            if (!channel.isNull()) delete channel.data();
        }

        QVERIFY2(everyChannelDestroyed,
                 "ANT channels outlived their worker");
    }

    void stopJoinsBlockedWorkerAndReleasesTransport()
    {
        ANT worker;
        QCOMPARE(worker.start(), 0);
        QVERIFY(FakeAntTransport::waitForBlockedReader());

        QCOMPARE(worker.stop(), 0);
        const FakeAntTransport::Snapshot atReturn =
                FakeAntTransport::snapshot();
        const bool joinedAtReturn = !worker.isRunning();

        if (!joinedAtReturn) QVERIFY(worker.wait(2000));

        QVERIFY2(joinedAtReturn,
                 "ANT::stop() returned while its worker thread was running");
        QCOMPARE(atReturn.blockedReaders, 0);
        QVERIFY(!atReturn.leased);
        QCOMPARE(atReturn.closeCount, 1);
    }

    void controllerDestructionStopsAndJoinsWorker()
    {
        ANTlocalController *controller =
                new ANTlocalController(nullptr, nullptr);
        QPointer<ANT> worker = controller->myANTlocal;
        QCOMPARE(worker->start(), 0);
        QVERIFY(FakeAntTransport::waitForBlockedReader());

        delete controller;

        const bool destroyedWithController = worker.isNull();
        const FakeAntTransport::Snapshot atReturn =
                FakeAntTransport::snapshot();

        if (!worker.isNull()) {
            finishWorker(worker.data());
            delete worker.data();
        }

        QVERIFY2(destroyedWithController,
                 "Controller destruction left an ANT worker alive");
        QCOMPARE(atReturn.blockedReaders, 0);
        QVERIFY(!atReturn.leased);
        QCOMPARE(atReturn.closeCount, 1);
        QCOMPARE(FakeAntTransport::snapshot().liveInstances, 0);
    }

    void workerDestructionStopsAndJoinsWorker()
    {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("GC_ANT_DESTRUCTOR_CHILD"),
                           QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), QStringList());

        QVERIFY(child.waitForStarted(2000));
        QVERIFY2(child.waitForFinished(5000),
                 qPrintable(child.errorString()));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    void portIsReusableImmediatelyAfterStop()
    {
        ANT first;
        QCOMPARE(first.start(), 0);
        QVERIFY(FakeAntTransport::waitForBlockedReader());

        QCOMPARE(first.stop(), 0);
        const bool firstJoined = !first.isRunning();

        ANT second;
        QCOMPARE(second.start(), 0);
        const FakeAntTransport::Snapshot afterSecondStart =
                FakeAntTransport::snapshot();
        const bool reopened = afterSecondStart.openCount == 2 &&
                afterSecondStart.leased;

        if (reopened) {
            QVERIFY(FakeAntTransport::waitForBlockedReader());
            finishWorker(&second);
        }
        if (!firstJoined) QVERIFY(first.wait(2000));

        QVERIFY2(firstJoined,
                 "First worker was still running when the port was reused");
        QVERIFY2(reopened,
                 "ANT transport was not reusable immediately after stop()");
    }

    void repeatedLifecycleLeavesNoWorkersOrLeases()
    {
        bool everyStopJoined = true;
        bool everyPortReleased = true;

        for (int attempt = 0; attempt < 8; ++attempt) {
            ANT *worker = new ANT;
            QCOMPARE(worker->start(), 0);
            QVERIFY(FakeAntTransport::waitForBlockedReader());

            QCOMPARE(worker->stop(), 0);
            everyStopJoined &= !worker->isRunning();
            everyPortReleased &= !FakeAntTransport::snapshot().leased;

            if (worker->isRunning()) QVERIFY(worker->wait(2000));
            if ((attempt % 2) == 0) QCOMPARE(worker->stop(), 0);
            delete worker;
        }

        const FakeAntTransport::Snapshot finalState =
                FakeAntTransport::snapshot();
        QVERIFY(everyStopJoined);
        QVERIFY(everyPortReleased);
        QCOMPARE(finalState.liveInstances, 0);
        QCOMPARE(finalState.blockedReaders, 0);
        QVERIFY(!finalState.leased);
        QCOMPARE(finalState.openCount, 8);
        QCOMPARE(finalState.closeCount, 8);
    }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);

    if (qEnvironmentVariableIsSet("GC_ANT_DESTRUCTOR_CHILD")) {
        FakeAntTransport::reset();
        ANT *worker = new ANT;
        if (worker->start() != 0 ||
            !FakeAntTransport::waitForBlockedReader()) {
            return 2;
        }

        delete worker;
        const FakeAntTransport::Snapshot state =
                FakeAntTransport::snapshot();
        return state.liveInstances == 0 &&
                state.blockedReaders == 0 &&
                !state.leased ? 0 : 3;
    }

    TestAntLifecycle test;
    return QTest::qExec(&test, argc, argv);
}

#include "testAntLifecycle.moc"
