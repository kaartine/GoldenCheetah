/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OPEN_DATA_UPLOAD_WORKER_H
#define GC_OPEN_DATA_UPLOAD_WORKER_H

#include "OpenDataExport.h"

#include <QObject>

#include <memory>
#include <mutex>
#include <thread>

class OpenDataUploadWorker final : public QObject
{
    Q_OBJECT

public:
    OpenDataUploadWorker(
        std::shared_ptr<const OpenDataExport::Request> request,
        OpenDataExport::UploadTask task,
        QObject *parent = nullptr);
    ~OpenDataUploadWorker() override;

    void start();
    bool wait(unsigned long timeoutMs = 5000);
    bool cancelAndWait(unsigned long timeoutMs = 5000);
    void cancelAndJoin();
    void requestCancellation();
    bool isRunning() const;
    void startManaged();

signals:
    void progress(int step, int lastStep, const QString &message);
    void succeeded(
        const QString &cyclist,
        int rideCount,
        int formatVersion);
    void failed(const QString &message);
    void finished();

private:
    struct State;

    static void execute(const std::shared_ptr<State> &state);
    bool waitForCompletion(unsigned long timeoutMs, bool deliver);
    void deliverCompletion();
    void joinThread();
    bool currentThreadIsWorker() const;

    std::shared_ptr<State> state_;
    mutable std::mutex joinMutex_;
    std::thread thread_;
    bool managed_ = false;
};

#endif
