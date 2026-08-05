/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/Daum.h"
#include "Train/TrainSidebarRuntime.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <atomic>
#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace {

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void writeFixture(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture\n"), qint64(8));
}

class StartObserver : public QObject
{
    Q_OBJECT

signals:
    void started();
};

class FakeController
{
public:
    void setLoad(int target)
    {
        targets.append(target);
    }

    QList<int> targets;
};

class TestDaum : public Daum
{
public:
    using Daum::Daum;

protected:
    void run() override
    {
        exec();
    }
};

#ifdef Q_OS_UNIX
class PseudoTerminal
{
public:
    PseudoTerminal()
        : masterFd_(::posix_openpt(O_RDWR | O_NOCTTY))
    {
        if (masterFd_ < 0 || ::grantpt(masterFd_) != 0
                || ::unlockpt(masterFd_) != 0) {
            closeMaster();
            return;
        }

        const char *name = ::ptsname(masterFd_);
        if (name == nullptr) {
            closeMaster();
            return;
        }
        slavePath_ = QString::fromLocal8Bit(name);
    }

    ~PseudoTerminal()
    {
        closeMaster();
    }

    bool isValid() const { return masterFd_ >= 0 && !slavePath_.isEmpty(); }
    QString slavePath() const { return slavePath_; }

private:
    void closeMaster()
    {
        if (masterFd_ >= 0) {
            ::close(masterFd_);
            masterFd_ = -1;
        }
    }

    int masterFd_;
    QString slavePath_;
};
#endif

}

class TestTrainRuntime : public QObject
{
    Q_OBJECT

private slots:
    void coreAndRrHeadersRoundTripToTheirOwnFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rrPath = directory.filePath(QStringLiteral("ride.rr"));
        const QString corePath = directory.filePath(QStringLiteral("ride.tcr"));
        QFile rrFile(rrPath);
        QFile coreFile(corePath);
        QVERIFY(rrFile.open(QIODevice::WriteOnly));
        QVERIFY(coreFile.open(QIODevice::WriteOnly));

        QTextStream rrHeader(
                TrainSidebarRuntime::auxiliaryHeaderDevice(
                        TrainSidebarRuntime::AuxiliaryHeader::Rr,
                        &rrFile, &coreFile));
        rrHeader << "secs, hr, msecs\n";
        rrHeader.flush();
        QTextStream coreHeader(
                TrainSidebarRuntime::auxiliaryHeaderDevice(
                        TrainSidebarRuntime::AuxiliaryHeader::CoreTemperature,
                        &rrFile, &coreFile));
        coreHeader << "secs, core, skin, hsi, qual\n";
        coreHeader.flush();
        rrFile.close();
        coreFile.close();

        QCOMPARE(readAll(rrPath), QByteArray("secs, hr, msecs\n"));
        QCOMPARE(readAll(corePath),
                 QByteArray("secs, core, skin, hsi, qual\n"));
    }

    void coreHeaderTargetsTcrWithoutAnRrFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile coreFile(directory.filePath(QStringLiteral("ride.tcr")));
        QVERIFY(coreFile.open(QIODevice::WriteOnly));

        QCOMPARE(
                TrainSidebarRuntime::auxiliaryHeaderDevice(
                        TrainSidebarRuntime::AuxiliaryHeader::CoreTemperature,
                        nullptr, &coreFile),
                static_cast<QIODevice *>(&coreFile));
    }

    void rrHeaderTargetsRrWithoutACoreFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile rrFile(directory.filePath(QStringLiteral("ride.rr")));
        QVERIFY(rrFile.open(QIODevice::WriteOnly));

        QCOMPARE(
                TrainSidebarRuntime::auxiliaryHeaderDevice(
                        TrainSidebarRuntime::AuxiliaryHeader::Rr,
                        &rrFile, nullptr),
                static_cast<QIODevice *>(&rrFile));
    }

    void discardRemovesEveryRecordingArtifact()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString base = directory.filePath(QStringLiteral("ride"));
        const QStringList paths = {
            base + QStringLiteral(".csv"),
            base + QStringLiteral(".rr"),
            base + QStringLiteral(".pos.csv"),
            base + QStringLiteral(".vo2"),
            base + QStringLiteral(".tcr")
        };
        for (const QString &path : paths) writeFixture(path);

        QVERIFY(TrainSidebarRuntime::discardRecordingArtifacts(
                base + QStringLiteral(".csv")));

        for (const QString &path : paths) {
            QVERIFY2(!QFileInfo::exists(path), qPrintable(path));
        }
    }

    void startEmitsOnceAfterCompleteInitialization()
    {
        StartObserver observer;
        QSignalSpy startSpy(&observer, &StartObserver::started);
        bool initialized = false;
        bool targetApplied = false;
        bool observerSawReadyState = true;

        QVERIFY(TrainSidebarRuntime::completeStart(
                [&]() { initialized = true; },
                [&]() {
                    targetApplied = true;
                    return true;
                },
                [&]() {
                    observerSawReadyState = observerSawReadyState
                            && initialized && targetApplied;
                    emit observer.started();
                }));

        QCOMPARE(startSpy.count(), 1);
        QVERIFY(observerSawReadyState);
    }

    void firstTargetIsAppliedSynchronously()
    {
        FakeController controller;

        QVERIFY(TrainSidebarRuntime::completeStart(
                []() {},
                [&]() {
                    controller.setLoad(235);
                    return true;
                },
                []() {}));

        QCOMPARE(controller.targets, QList<int>() << 235);
    }

    void daumRestartReleasesPausedState()
    {
        TestDaum daum(nullptr, QString(), QString());
        QCOMPARE(daum.start(), 0);
        QVERIFY(!daum.pausedForTest());
        QCOMPARE(daum.pause(), 0);
        QVERIFY(daum.pausedForTest());
        QCOMPARE(daum.restart(), 0);
        QVERIFY(!daum.pausedForTest());
        QCOMPARE(daum.stop(), 0);
        QVERIFY(daum.wait(1000));
    }

    void daumFailedOpenStopJoinAndDestructionReleasesResources()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Daum daum(nullptr, directory.filePath(QStringLiteral("missing-device")),
                  QString());
        daum.start();
        QVERIFY(daum.wait(10000));

        QCOMPARE(daum.stop(), 0);
        QVERIFY(daum.wait(1000));
        QVERIFY(daum.workerResourcesReleasedForTest());
    }

    void daumPartialStartStopJoinsAndReleasesResources()
    {
#ifndef Q_OS_UNIX
        QSKIP("A pseudo-terminal is required for the partial-start regression");
#else
        PseudoTerminal terminal;
        QVERIFY(terminal.isValid());

        Daum daum(nullptr, terminal.slavePath(), QStringLiteral("0"));
        std::atomic_bool portOpened(false);
        QObject observer;
        connect(&daum, &Daum::workerPortOpenedForTest, &observer,
                [&portOpened]() { portOpened.store(true); }, Qt::DirectConnection);
        daum.start();
        QTRY_VERIFY_WITH_TIMEOUT(portOpened.load(), 5000);

        QCOMPARE(daum.stop(), 0);
        QVERIFY(daum.wait(15000));
        QVERIFY(daum.workerResourcesReleasedForTest());
#endif
    }

    void daumDestructorJoinsPartialStartWorker()
    {
#ifndef Q_OS_UNIX
        QSKIP("A pseudo-terminal is required for the partial-start regression");
#else
        PseudoTerminal terminal;
        QVERIFY(terminal.isValid());

        std::atomic_bool finished(false);
        std::atomic_bool portOpened(false);
        QObject observer;
        auto daum = std::make_unique<Daum>(
                nullptr, terminal.slavePath(), QStringLiteral("0"));
        connect(daum.get(), &Daum::workerPortOpenedForTest, &observer,
                [&portOpened]() { portOpened.store(true); }, Qt::DirectConnection);
        connect(daum.get(), &QThread::finished, &observer,
                [&finished]() { finished.store(true); }, Qt::DirectConnection);
        daum->start();
        QTRY_VERIFY_WITH_TIMEOUT(portOpened.load(), 5000);

        daum.reset();
        QVERIFY(finished.load());
#endif
    }
};

QTEST_GUILESS_MAIN(TestTrainRuntime)
#include "testTrainRuntime.moc"
