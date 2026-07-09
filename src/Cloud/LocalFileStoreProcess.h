/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_LOCALFILESTOREPROCESS_H
#define GC_LOCALFILESTOREPROCESS_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

class QThread;

struct LocalFileStoreEntryValue
{
    QString name;
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
};

struct LocalFileStoreProcessResult
{
    enum class Status : quint8 {
        Succeeded,
        Failed,
        Cancelled,
        TimedOut
    };

    Status status = Status::Failed;
    QString error;
    QList<LocalFileStoreEntryValue> entries;
    QByteArray data;

    bool succeeded() const { return status == Status::Succeeded; }
};

class LocalFileStoreProcess final
{
public:
    enum class Operation : quint8 {
        Open,
        List,
        Read
    };

    static constexpr int DefaultTimeoutMs = 30000;

    static bool isHelperInvocation(int argc, char *argv[]);
    static int runHelper(const QStringList &arguments);

    static bool initializeReaper();
    static bool shutdownReaper();

    static LocalFileStoreProcessResult run(
        Operation operation,
        const QString &root,
        const QString &path = QString(),
        int timeoutMs = DefaultTimeoutMs);

    static LocalFileStoreProcessResult readFileInProcess(
        const QString &root, const QString &name);
    static LocalFileStoreProcessResult writeFileInProcess(
        const QString &root, const QString &name,
        const QByteArray &data);

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    static qint64 lastHelperProcessIdForTest();
    static int helperProcessStartCountForTest();
    static bool helperProcessIsRunningForTest(qint64 processId);
    static int reaperRegistrationAttemptCountForTest();
    static int reaperProcessCountForTest();
    static bool drainReaperForTest(int timeoutMs);
    static bool reaperIsStoppedForTest();
    static QThread *reaperOwnerThreadForTest();
    static void prepareReaperAdoptPauseForTest();
    static bool waitForReaperAdoptPauseForTest(int timeoutMs);
    static void releaseReaperAdoptForTest();
    static void prepareReaperDispatchPauseForTest();
    static bool waitForReaperDispatchPauseForTest(int timeoutMs);
    static void stopReaperEventLoopForTest();
    static bool waitForReaperTeardownProbeForTest(int timeoutMs);
    static void releaseReaperDispatchForTest();
    static bool waitForReaperThreadForTest(int timeoutMs);
    static bool reaperDispatchLostTargetForTest();
    static bool parseResponseForTest(
        const QByteArray &frame,
        Operation expectedOperation,
        LocalFileStoreProcessResult &result);
#endif
};

#endif
