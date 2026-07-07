/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "FakeSiUSBXp.h"
#include "LibUsb.h"
#include "ANT.h"
#include "USBXpress.h"

#include <QTest>

#include <algorithm>

namespace {

const char GarminVid[] = "0fcf";
const char GarminPid[] = "1004";

enum EntryPoint
{
    FindEntryPoint,
    OpenEntryPoint
};

void addGarminDevice()
{
    FakeSiUSBXp::addDevice(GarminVid, GarminPid);
}

int callCount(const FakeSiUSBXp::Snapshot &snapshot,
              const FakeSiUSBXp::Operation operation)
{
    return static_cast<int>(std::count(snapshot.calls.begin(),
                                       snapshot.calls.end(), operation));
}

QString callTrace(const FakeSiUSBXp::Snapshot &snapshot)
{
    QStringList names;
    for (const FakeSiUSBXp::Operation operation : snapshot.calls)
        names.append(QString::fromLatin1(FakeSiUSBXp::operationName(operation)));
    return names.join(QStringLiteral(" -> "));
}

}

class TestUSBXpressSafety : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        FakeSiUSBXp::reset();
        FakeAntTransport::reset();
    }

    void constantsUseCanonicalUsbIdentity()
    {
        QCOMPARE(GARMIN_USB1_VID, 0x0fcf);
        QCOMPARE(GARMIN_USB1_PID, 0x1004);
    }

    void selectsRealGarminUsb1Identity()
    {
        FakeSiUSBXp::addDevice("0fcf", "0001");
        FakeSiUSBXp::addDevice("1004", "0fcf");
        addGarminDevice();

        QVERIFY(USBXpress::find());
        QCOMPARE(callCount(FakeSiUSBXp::snapshot(),
                           FakeSiUSBXp::Operation::Open), 0);

        FakeSiUSBXp::reset();
        FakeSiUSBXp::addDevice("0fcf", "0001");
        FakeSiUSBXp::addDevice("1004", "0fcf");
        addGarminDevice();

        HANDLE handle = nullptr;
        QCOMPARE(USBXpress::open(&handle), 0);
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        QCOMPARE(snapshot.openedDevice, static_cast<DWORD>(2));
        QCOMPARE(handle, FakeSiUSBXp::testHandle());
        QCOMPARE(USBXpress::close(&handle), 0);
    }

    void antCloseUsesUsbXpressSdk()
    {
        FakeAntTransport::setOpenResult(-1);
        addGarminDevice();

        {
            ANT ant;
            QCOMPARE(ant.openPort(), 0);
            QCOMPARE(ant.channelCount(), 4);
            QCOMPARE(ant.closePort(), 0);
        }

        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        QCOMPARE(snapshot.closeCalls, 1);
        QCOMPARE(callCount(snapshot,
                           FakeSiUSBXp::Operation::WindowsCloseHandle), 0);
    }

    void rejectsNonTargetUsbIdentities_data()
    {
        QTest::addColumn<QString>("vid");
        QTest::addColumn<QString>("pid");

        QTest::newRow("swapped") << QStringLiteral("1004")
                                  << QStringLiteral("0fcf");
        QTest::newRow("wrong-vendor") << QStringLiteral("1234")
                                       << QStringLiteral("1004");
        QTest::newRow("wrong-product") << QStringLiteral("0fcf")
                                        << QStringLiteral("1234");
    }

    void rejectsNonTargetUsbIdentities()
    {
        QFETCH(QString, vid);
        QFETCH(QString, pid);

        FakeSiUSBXp::addDevice(vid.toStdString(), pid.toStdString());
        QVERIFY(!USBXpress::find());

        HANDLE handle = nullptr;
        QCOMPARE(USBXpress::open(&handle), -1);
        QCOMPARE(callCount(FakeSiUSBXp::snapshot(),
                           FakeSiUSBXp::Operation::Open), 0);
    }

    void rejectsEnumerationFailures_data()
    {
        QTest::addColumn<int>("entryPoint");
        QTest::addColumn<int>("failedOperation");

        const FakeSiUSBXp::Operation operations[] = {
            FakeSiUSBXp::Operation::GetNumDevices,
            FakeSiUSBXp::Operation::GetProductStringVid,
            FakeSiUSBXp::Operation::GetProductStringPid
        };

        for (const int entryPoint : {FindEntryPoint, OpenEntryPoint}) {
            const QString prefix = entryPoint == FindEntryPoint
                    ? QStringLiteral("find-") : QStringLiteral("open-");
            for (const FakeSiUSBXp::Operation operation : operations) {
                const QString rowName = prefix +
                        QString::fromLatin1(FakeSiUSBXp::operationName(operation));
                QTest::newRow(qPrintable(rowName))
                        << entryPoint << static_cast<int>(operation);
            }
        }
    }

    void rejectsEnumerationFailures()
    {
        QFETCH(int, entryPoint);
        QFETCH(int, failedOperation);

        addGarminDevice();
        const FakeSiUSBXp::Operation operation =
                static_cast<FakeSiUSBXp::Operation>(failedOperation);
        FakeSiUSBXp::setResult(operation, SI_DEVICE_IO_FAILED);

        HANDLE handle = nullptr;
        const bool rejected = entryPoint == FindEntryPoint
                ? !USBXpress::find()
                : USBXpress::open(&handle) == -1;
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        const bool didNotOpen = callCount(
                snapshot, FakeSiUSBXp::Operation::Open) == 0;

        const QString detail = QStringLiteral(
                "A failed enumeration API must reject the candidate without "
                "opening it. Calls: %1").arg(callTrace(snapshot));
        QVERIFY2(rejected && didNotOpen, qPrintable(detail));
    }

    void sdkOpenFailureDoesNotCloseAnUnopenedHandle()
    {
        addGarminDevice();
        FakeSiUSBXp::setResult(FakeSiUSBXp::Operation::Open,
                               SI_DEVICE_IO_FAILED);

        HANDLE handle = nullptr;
        QCOMPARE(USBXpress::open(&handle), -1);
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        QCOMPARE(callCount(snapshot, FakeSiUSBXp::Operation::Open), 1);
        QCOMPARE(snapshot.closeCalls, 0);
    }

    void setupFailureRollsBackOpen_data()
    {
        QTest::addColumn<int>("failedOperation");

        const FakeSiUSBXp::Operation operations[] = {
            FakeSiUSBXp::Operation::FlushBuffers,
            FakeSiUSBXp::Operation::SetTimeouts,
            FakeSiUSBXp::Operation::SetBaudRate,
            FakeSiUSBXp::Operation::SetLineControl,
            FakeSiUSBXp::Operation::SetFlowControl
        };

        for (const FakeSiUSBXp::Operation operation : operations) {
            QTest::newRow(FakeSiUSBXp::operationName(operation))
                    << static_cast<int>(operation);
        }
    }

    void setupFailureRollsBackOpen()
    {
        QFETCH(int, failedOperation);

        addGarminDevice();
        FakeSiUSBXp::setResult(
                static_cast<FakeSiUSBXp::Operation>(failedOperation),
                SI_DEVICE_IO_FAILED);

        HANDLE handle = nullptr;
        const int result = USBXpress::open(&handle);
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        const bool closeWasLast = !snapshot.calls.empty() &&
                snapshot.calls.back() == FakeSiUSBXp::Operation::Close;

        const QString detail = QStringLiteral(
                "Setup failure must return -1 and close the opened handle "
                "exactly once. result=%1 closeCalls=%2 calls=%3")
                .arg(result)
                .arg(snapshot.closeCalls)
                .arg(callTrace(snapshot));
        QVERIFY2(result == -1 && snapshot.closeCalls == 1 && closeWasLast &&
                 snapshot.closedHandle == snapshot.openedHandle,
                 qPrintable(detail));
    }

    void successfulOpenAppliesUsb1Configuration()
    {
        addGarminDevice();

        HANDLE handle = nullptr;
        QCOMPARE(USBXpress::open(&handle), 0);
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();

        QCOMPARE(snapshot.flushTransmit, static_cast<BYTE>(1));
        QCOMPARE(snapshot.flushReceive, static_cast<BYTE>(1));
        QCOMPARE(snapshot.readTimeout, static_cast<DWORD>(5));
        QCOMPARE(snapshot.writeTimeout, static_cast<DWORD>(5));
        QCOMPARE(snapshot.baudRate, static_cast<DWORD>(115200));
        QCOMPARE(snapshot.lineControl, static_cast<WORD>(0x0800));
        QCOMPARE(snapshot.ctsMaskCode, static_cast<BYTE>(SI_STATUS_INPUT));
        QCOMPARE(snapshot.rtsMaskCode, static_cast<BYTE>(SI_HELD_INACTIVE));
        QCOMPARE(snapshot.dtrMaskCode, static_cast<BYTE>(SI_HELD_INACTIVE));
        QCOMPARE(snapshot.dsrMaskCode, static_cast<BYTE>(SI_STATUS_INPUT));
        QCOMPARE(snapshot.dcdMaskCode, static_cast<BYTE>(SI_STATUS_INPUT));
        QCOMPARE(snapshot.flowXonXoff, static_cast<BOOL>(FALSE));
        QCOMPARE(snapshot.closeCalls, 0);

        QCOMPARE(USBXpress::close(&handle), 0);
    }

    void readReportsSdkResult_data()
    {
        QTest::addColumn<int>("sdkResult");
        QTest::addColumn<int>("reportedBytes");
        QTest::addColumn<int>("expectedResult");

        QTest::newRow("complete") << static_cast<int>(SI_SUCCESS) << 8 << 8;
        QTest::newRow("short") << static_cast<int>(SI_SUCCESS) << 3 << 3;
        QTest::newRow("error") << static_cast<int>(SI_READ_ERROR) << 0 << -1;
    }

    void readReportsSdkResult()
    {
        QFETCH(int, sdkResult);
        QFETCH(int, reportedBytes);
        QFETCH(int, expectedResult);

        FakeSiUSBXp::setReadResult(static_cast<SI_STATUS>(sdkResult),
                                   static_cast<DWORD>(reportedBytes));
        HANDLE handle = FakeSiUSBXp::testHandle();
        unsigned char buffer[8] = {};

        QCOMPARE(USBXpress::read(&handle, buffer, 8), expectedResult);
        QCOMPARE(FakeSiUSBXp::snapshot().requestedRead,
                 static_cast<DWORD>(8));
    }

    void writeReportsSdkResult_data()
    {
        QTest::addColumn<int>("sdkResult");
        QTest::addColumn<int>("reportedBytes");
        QTest::addColumn<int>("expectedResult");

        QTest::newRow("complete") << static_cast<int>(SI_SUCCESS) << 8 << 8;
        QTest::newRow("short") << static_cast<int>(SI_SUCCESS) << 3 << 3;
        QTest::newRow("error") << static_cast<int>(SI_WRITE_ERROR) << 0 << -1;
    }

    void writeReportsSdkResult()
    {
        QFETCH(int, sdkResult);
        QFETCH(int, reportedBytes);
        QFETCH(int, expectedResult);

        FakeSiUSBXp::setWriteResult(static_cast<SI_STATUS>(sdkResult),
                                    static_cast<DWORD>(reportedBytes));
        HANDLE handle = FakeSiUSBXp::testHandle();
        unsigned char buffer[8] = {};

        QCOMPARE(USBXpress::write(&handle, buffer, 8), expectedResult);
        QCOMPARE(FakeSiUSBXp::snapshot().requestedWrite,
                 static_cast<DWORD>(8));
    }

    void closeReportsSdkResult_data()
    {
        QTest::addColumn<int>("sdkResult");
        QTest::addColumn<int>("expectedResult");

        QTest::newRow("success") << static_cast<int>(SI_SUCCESS) << 0;
        QTest::newRow("error") << static_cast<int>(SI_INVALID_HANDLE) << -1;
    }

    void closeReportsSdkResult()
    {
        QFETCH(int, sdkResult);
        QFETCH(int, expectedResult);

        FakeSiUSBXp::setResult(FakeSiUSBXp::Operation::Close,
                               static_cast<SI_STATUS>(sdkResult));
        HANDLE handle = FakeSiUSBXp::testHandle();

        QCOMPARE(USBXpress::close(&handle), expectedResult);
        const FakeSiUSBXp::Snapshot snapshot = FakeSiUSBXp::snapshot();
        QCOMPARE(snapshot.closeCalls, 1);
        QCOMPARE(snapshot.closedHandle, handle);
    }
};

QTEST_APPLESS_MAIN(TestUSBXpressSafety)

#include "testUSBXpressSafety.moc"
