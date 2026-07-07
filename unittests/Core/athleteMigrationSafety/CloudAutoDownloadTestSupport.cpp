/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Cloud/CloudService.h"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>

#include <memory>
#include <utility>

namespace {

constexpr auto BlockingServiceId = "LifecycleBlockingCloud";

struct BlockingCloudState
{
    QMutex mutex;
    QSemaphore readStarted;
    CloudService *provider = nullptr;
    QByteArray *data = nullptr;
};

std::shared_ptr<BlockingCloudState> blockingState;

class BlockingCloudService final : public CloudService
{
public:
    explicit BlockingCloudService(
            Context *context,
            std::shared_ptr<BlockingCloudState> state = {})
        : CloudService(context), state(std::move(state))
    {
    }

    ~BlockingCloudService() override
    {
        if (!state) return;
        QMutexLocker locker(&state->mutex);
        if (state->provider == this) state->provider = nullptr;
    }

    CloudService *clone(Context *context) override
    {
        return new BlockingCloudService(context, blockingState);
    }

    QString id() const override
    {
        return QString::fromLatin1(BlockingServiceId);
    }

    QString uiName() const override
    {
        return QStringLiteral("Lifecycle blocking cloud");
    }

    QImage logo() const override
    {
        return {};
    }

    bool open(QStringList &) override
    {
        return true;
    }

    bool close() override
    {
        return true;
    }

    QList<CloudServiceEntry *> readdir(
            QString, QStringList &, QDateTime, QDateTime) override
    {
        CloudServiceEntry *entry = newCloudServiceEntry();
        entry->name =
                QDateTime::currentDateTime().toString(
                        QStringLiteral("yyyy_MM_dd_HH_mm_ss")) +
                QStringLiteral(".fit");
        entry->id = QStringLiteral("blocked-download");
        entry->isDir = false;
        return {entry};
    }

    bool readFile(QByteArray *data, QString, QString) override
    {
        if (!state) return false;
        {
            QMutexLocker locker(&state->mutex);
            state->provider = this;
            state->data = data;
        }
        state->readStarted.release();
        return true;
    }

private:
    std::shared_ptr<BlockingCloudState> state;
};

} // namespace

void configureBlockingCloudAutoDownload(const QString &athlete)
{
    blockingState = std::make_shared<BlockingCloudState>();

    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId = QString::fromLatin1(BlockingServiceId);
    if (!factory.service(serviceId)) {
        factory.addService(new BlockingCloudService(nullptr));
    }

    const CloudService *service = factory.service(serviceId);
    appsettings->setCValue(
            athlete, service->syncOnStartupSettingName(),
            QStringLiteral("true"));
}

bool waitForBlockingCloudRead(int timeoutMs)
{
    return blockingState &&
           blockingState->readStarted.tryAcquire(1, timeoutMs);
}

bool releaseBlockingCloudRead(CloudServiceAutoDownload *receiver)
{
    if (!blockingState || !receiver) return false;

    CloudService *provider = nullptr;
    QByteArray *data = nullptr;
    {
        QMutexLocker locker(&blockingState->mutex);
        provider = blockingState->provider;
        data = blockingState->data;
    }
    if (!provider || !data) return false;

    QObject::disconnect(
            provider, &CloudService::readComplete,
            receiver, &CloudServiceAutoDownload::readComplete);
    return QMetaObject::invokeMethod(
            provider,
            [provider, data]() {
                provider->notifyReadComplete(
                        data, QStringLiteral("blocked.fit"), QString());
            },
            Qt::BlockingQueuedConnection);
}

void cleanupBlockingCloudRead()
{
    if (!blockingState) return;

    QByteArray *data = nullptr;
    {
        QMutexLocker locker(&blockingState->mutex);
        data = blockingState->data;
        blockingState->data = nullptr;
    }
    delete data;
    blockingState.reset();
}
