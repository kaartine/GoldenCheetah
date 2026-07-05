/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "PowerTapDevice.h"

#include <QTest>

#include <cstring>

class FakeCommPort : public CommPort
{
public:
    enum class EndState {
        Timeout,
        ReadError
    };

    explicit FakeCommPort(const QByteArray &input,
                          EndState endState = EndState::Timeout,
                          const QString &terminalError = QString())
        : CommPort("fake"),
          input(input),
          endState(endState),
          terminalError(terminalError)
    {
    }

    bool isOpen() override { return openState; }

    bool open(QString &) override
    {
        openState = true;
        return true;
    }

    void close() override { openState = false; }

    int read(void *buffer, size_t byteCount, QString &error) override
    {
        ++readCalls;
        const qsizetype remaining = input.size() - offset;
        const qsizetype count = qMin(
            remaining, static_cast<qsizetype>(byteCount));
        if (count <= 0) {
            if (endState == EndState::ReadError) {
                error = terminalError;
                return -1;
            }
            return 0;
        }

        std::memcpy(buffer, input.constData() + offset,
                    static_cast<size_t>(count));
        offset += count;
        return static_cast<int>(count);
    }

    int write(void *, size_t byteCount, QString &) override
    {
        return static_cast<int>(byteCount);
    }

    QString name() const override { return "fake"; }
    bool setBaudRate(int, QString &) override { return true; }

    int bytesRead() const { return static_cast<int>(offset); }
    int reads() const { return readCalls; }

private:
    QByteArray input;
    qsizetype offset = 0;
    int readCalls = 0;
    bool openState = false;
    EndState endState;
    QString terminalError;
};

class TestPowerTapBounds : public QObject
{
    Q_OBJECT

private slots:
    void rejectsZeroLengthBufferWithoutReading()
    {
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(QByteArray()));
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, nullptr, 0, error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 0);
        QCOMPARE(fake->reads(), 0);
        QCOMPARE(error, QStringLiteral(
            "read buffer full after 0 bytes without newline"));
    }

    void rejectsNullBufferWithoutReading()
    {
        QSharedPointer<FakeCommPort> fake(
            new FakeCommPort(QByteArray("A", 1)));
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, nullptr, 1, error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 0);
        QCOMPARE(fake->reads(), 0);
        QCOMPARE(error, QStringLiteral("read buffer is null"));
    }

    void rejectsMoreThan256BytesWithoutNewline()
    {
        QSharedPointer<FakeCommPort> fake(
            new FakeCommPort(QByteArray(257, 'A')));
        char storage[257];
        std::memset(storage, 0, sizeof(storage));
        storage[256] = '!';
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, 256, error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 256);
        QCOMPARE(fake->reads(), 256);
        QCOMPARE(storage[256], '!');
        QVERIFY2(error.contains("buffer", Qt::CaseInsensitive),
                 qPrintable(error));
    }

    void rejectsTrailingCrAtCapacity()
    {
        QByteArray input(255, 'A');
        input.append('\r');
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(input));
        char storage[256];
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, sizeof(storage), error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 256);
        QCOMPARE(fake->reads(), 256);
        QVERIFY2(error.contains("buffer", Qt::CaseInsensitive),
                 qPrintable(error));
    }

    void acceptsCrLfAtCapacity()
    {
        QByteArray input(254, 'A');
        input.append("\r\n", 2);
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(input));
        char storage[256];
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, sizeof(storage), error);

        QCOMPARE(result, 256);
        QCOMPARE(fake->bytesRead(), 256);
        QCOMPARE(fake->reads(), 256);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void reportsTimeoutAfterPartialRead()
    {
        QSharedPointer<FakeCommPort> fake(
            new FakeCommPort(QByteArray("ABC", 3)));
        char storage[8] = {};
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, sizeof(storage), error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 3);
        QCOMPARE(fake->reads(), 4);
        QCOMPARE(error, QStringLiteral(
            "read timeout, read 3 bytes so far: \"ABC\""));
    }

    void reportsImmediateTimeoutWithoutInspectingStorage()
    {
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(QByteArray()));
        char storage = '!';
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, &storage, 1, error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 0);
        QCOMPARE(fake->reads(), 1);
        QCOMPARE(storage, '!');
        QCOMPARE(error, QStringLiteral(
            "read timeout, read 0 bytes so far: \"\""));
    }

    void escapesPrintableAndNonprintableBytesInErrors()
    {
        QByteArray input;
        input.append('A');
        input.append('"');
        input.append('~');
        input.append('\0');
        input.append(char(0x1f));
        input.append(char(0x7f));
        input.append(char(0xff));
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(input));
        char storage[8] = {};
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, sizeof(storage), error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 7);
        QCOMPARE(fake->reads(), 8);
        QCOMPARE(error, QStringLiteral(
            "read timeout, read 7 bytes so far: "
            "\"A\\\"~\\x00\\x1f\\x7f\\xff\""));
    }

    void propagatesReadErrorWithBoundedContext()
    {
        QSharedPointer<FakeCommPort> fake(new FakeCommPort(
            QByteArray("OK", 2), FakeCommPort::EndState::ReadError,
            QStringLiteral("serial failure %2")));
        char storage[8] = {};
        QString error;

        const int result = PowerTapDevice::readUntilNewline(
            fake, storage, sizeof(storage), error);

        QCOMPARE(result, -1);
        QCOMPARE(fake->bytesRead(), 2);
        QCOMPARE(fake->reads(), 3);
        QCOMPARE(error, QStringLiteral(
            "read error: serial failure %2, read 2 bytes so far: \"OK\""));
    }
};

QTEST_APPLESS_MAIN(TestPowerTapBounds)
#include "testPowerTapBounds.moc"
