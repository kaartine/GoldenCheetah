/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/TrainingRecordingIo.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class ShortWriteDevice : public QIODevice
{
public:
    QByteArray bytes;
    qint64 writeLimit = 0;

protected:
    qint64 readData(char *, qint64) override { return -1; }

    qint64 writeData(const char *data, qint64 size) override
    {
        const qint64 accepted = qMin(writeLimit, size);
        bytes.append(data, accepted);
        return accepted;
    }
};

class FailingWriteDevice : public QIODevice
{
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *, qint64) override { return -1; }
};

class TestTrainingRecordingIo : public QObject
{
    Q_OBJECT

private slots:
    void openFailureIsReported()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile target(directory.path());

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::open(target, QIODevice::WriteOnly);

        QVERIFY(!result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::Open);
        QVERIFY(QFileInfo::exists(directory.path()));
    }

    void shortWriteFailsAndKeepsPartialBytes()
    {
        ShortWriteDevice device;
        device.writeLimit = 4;
        QVERIFY(device.open(QIODevice::WriteOnly));
        int flushCalls = 0;

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::writeAndFlush(
                        device, QByteArray("abcdefgh"), [&flushCalls]() {
                            ++flushCalls;
                            return true;
                        });

        QVERIFY(!result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::Write);
        QCOMPARE(result.bytesWritten, qint64(4));
        QCOMPARE(device.bytes, QByteArray("abcd"));
        QCOMPARE(flushCalls, 0);
    }

    void writeFailureIsReportedWithoutFlush()
    {
        FailingWriteDevice device;
        QVERIFY(device.open(QIODevice::WriteOnly));
        int flushCalls = 0;

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::writeAndFlush(
                        device, QByteArray("sample"), [&flushCalls]() {
                            ++flushCalls;
                            return true;
                        });

        QVERIFY(!result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::Write);
        QCOMPARE(result.bytesWritten, qint64(-1));
        QCOMPARE(flushCalls, 0);
    }

    void flushFailureIsReportedAfterCompleteWrite()
    {
        QBuffer device;
        QVERIFY(device.open(QIODevice::WriteOnly));

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::writeAndFlush(
                        device, QByteArray("complete"), []() { return false; });

        QVERIFY(!result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::Flush);
        QCOMPARE(result.bytesWritten, qint64(8));
        QCOMPARE(device.data(), QByteArray("complete"));
    }

    void successfulWriteIsExactAndFlushedOnce()
    {
        QBuffer device;
        QVERIFY(device.open(QIODevice::WriteOnly));
        int flushCalls = 0;

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::writeAndFlush(
                        device, QByteArray("sample\n"), [&flushCalls]() {
                            ++flushCalls;
                            return true;
                        });

        QVERIFY(result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::None);
        QCOMPARE(result.bytesWritten, qint64(7));
        QCOMPARE(device.data(), QByteArray("sample\n"));
        QCOMPARE(flushCalls, 1);
    }

    void emptyWriteStillChecksFlushFailure()
    {
        QBuffer device;
        QVERIFY(device.open(QIODevice::WriteOnly));

        const TrainingRecordingIo::Result result =
                TrainingRecordingIo::writeAndFlush(
                        device, QByteArray(), []() { return false; });

        QVERIFY(!result.ok());
        QCOMPARE(result.failure, TrainingRecordingIo::Failure::Flush);
        QCOMPARE(result.bytesWritten, qint64(0));
        QVERIFY(device.data().isEmpty());
    }

    void healthLatchesFirstFailureUntilReset()
    {
        TrainingRecordingIo::Health health;
        QVERIFY(health.isHealthy());
        QVERIFY(!health.markFailure(TrainingRecordingIo::Failure::None));
        QVERIFY(health.isHealthy());
        QVERIFY(health.markFailure(TrainingRecordingIo::Failure::Write));
        QVERIFY(!health.isHealthy());
        QCOMPARE(health.failure(), TrainingRecordingIo::Failure::Write);
        QVERIFY(!health.markFailure(TrainingRecordingIo::Failure::Flush));
        QCOMPARE(health.failure(), TrainingRecordingIo::Failure::Write);

        health.reset();
        QVERIFY(health.isHealthy());
        QCOMPARE(health.failure(), TrainingRecordingIo::Failure::None);
    }
};

QTEST_MAIN(TestTrainingRecordingIo)
#include "testTrainingRecordingIo.moc"
