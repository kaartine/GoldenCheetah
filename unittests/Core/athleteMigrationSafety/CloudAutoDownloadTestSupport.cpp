/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "CloudAutoDownloadTestSupport.h"

#include "Cloud/CloudService.h"

#include <QEventLoop>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <utility>

namespace {

constexpr auto BlockingServiceId = "LifecycleBlockingCloud";
constexpr auto ControlledServiceId = "ControlledLifecycleCloud";
constexpr auto SuccessfulFollowUpServiceId = "ZSuccessfulFollowUpCloud";

struct BufferProbeState
{
    QMutex mutex;
    QSet<QByteArray *> outstanding;
    int allocated = 0;
    int released = 0;
};

struct BlockingCloudState
{
    QMutex mutex;
    QSemaphore readStarted;
    CloudService *provider = nullptr;
    QByteArray *data = nullptr;
};

struct ControlledCloudState
{
    explicit ControlledCloudState(
            TestCloudCompletionMode mode,
            int entryCount)
        : mode(mode), entryCount(entryCount)
    {
    }

    QMutex mutex;
    QSemaphore reads;
    QSemaphore completions;
    QSemaphore providerDestroyed;
    QSemaphore blockedCallStarted;
    TestCloudCompletionMode mode;
    int entryCount;
    CloudService *providerAddress = nullptr;
    QThread *guiThread = nullptr;
    QThread *workerThread = nullptr;
    QEventLoop *blockedLoop = nullptr;
    bool destroyed = false;
    int callDepth = 0;
    int destroyedDuringCall = 0;
    int abortCalls = 0;
    int baseReplyAbortCalls = 0;
    int providerOperations = 0;
    int crossThreadProviderAccesses = 0;
    int destroyedProviderAccesses = 0;
    int reentrantReadCalls = 0;
};

BufferProbeState bufferProbe;
std::shared_ptr<BlockingCloudState> blockingState;
std::shared_ptr<ControlledCloudState> controlledState;
std::atomic<int> configuredTimeoutMs{30000};

void resetBufferProbe()
{
    QMutexLocker locker(&bufferProbe.mutex);
    bufferProbe.outstanding.clear();
    bufferProbe.allocated = 0;
    bufferProbe.released = 0;
}

void recordControlledProviderAccess(
        const std::shared_ptr<ControlledCloudState> &state)
{
    if (!state) return;

    QMutexLocker locker(&state->mutex);
    QThread *current = QThread::currentThread();
    ++state->providerOperations;
    if (!state->workerThread) state->workerThread = current;
    if (current == state->guiThread || current != state->workerThread) {
        ++state->crossThreadProviderAccesses;
    }
    if (state->destroyed) ++state->destroyedProviderAccesses;
}

void blockControlledProviderCall(
        const std::shared_ptr<ControlledCloudState> &state)
{
    QEventLoop loop;
    {
        QMutexLocker locker(&state->mutex);
        ++state->callDepth;
        state->blockedLoop = &loop;
    }
    state->blockedCallStarted.release();
    loop.exec();
    QMutexLocker locker(&state->mutex);
    state->blockedLoop = nullptr;
    --state->callDepth;
}

class BaseAbortReply final : public QNetworkReply
{
public:
    BaseAbortReply(
            QObject *parent,
            std::shared_ptr<ControlledCloudState> state)
        : QNetworkReply(parent), state(std::move(state))
    {
        setOpenMode(QIODevice::ReadOnly);
    }

    void abort() override
    {
        if (isFinished()) return;
        {
            QMutexLocker locker(&state->mutex);
            ++state->baseReplyAbortCalls;
        }
        setFinished(true);
        emit finished();
    }

protected:
    qint64 readData(char *, qint64) override { return -1; }

private:
    std::shared_ptr<ControlledCloudState> state;
};

class BlockingCloudService final : public CloudService
{
public:
    explicit BlockingCloudService(
            Context *context,
            std::shared_ptr<BlockingCloudState> state = {})
        : CloudService(context), state(std::move(state))
    {
        downloadCompression = CloudService::none;
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

class ControlledCloudService final : public CloudService
{
public:
    explicit ControlledCloudService(
            Context *context,
            std::shared_ptr<ControlledCloudState> state = {})
        : CloudService(context), state(std::move(state))
    {
        downloadCompression = CloudService::none;
        settings.insert(
            CloudService::Local1,
            QStringLiteral("<athlete-private>controlled/thread"));
        settings.insert(
            CloudService::Local2,
            QStringLiteral("<global-general>controlled/thread"));
        settings.insert(
            CloudService::URL,
            QStringLiteral("<athlete-private>controlled/url"));
        settings.insert(
            CloudService::DefaultURL, QStringLiteral("https://default.invalid"));
        if (this->state) {
            QMutexLocker locker(&this->state->mutex);
            this->state->providerAddress = this;
            this->state->destroyed = false;
        }
    }

    ~ControlledCloudService() override
    {
        const std::shared_ptr<ControlledCloudState> currentState = state;
        if (!currentState) return;
        recordControlledProviderAccess(currentState);
        QEventLoop *blockedLoop = nullptr;
        {
            QMutexLocker locker(&currentState->mutex);
            currentState->providerAddress = this;
            currentState->destroyed = true;
            if (currentState->callDepth > 0) {
                ++currentState->destroyedDuringCall;
            }
            blockedLoop = currentState->blockedLoop;
        }
        if (blockedLoop) blockedLoop->quit();
        currentState->providerDestroyed.release();
    }

    CloudService *clone(Context *context) override
    {
        return new ControlledCloudService(context, controlledState);
    }

    QString id() const override
    {
        return QString::fromLatin1(ControlledServiceId);
    }

    QString uiName() const override
    {
        recordControlledProviderAccess(state);
        return QStringLiteral("Controlled lifecycle cloud");
    }

    QImage logo() const override
    {
        return {};
    }

    QString home() override
    {
        recordControlledProviderAccess(state);
        return QStringLiteral("/");
    }

    bool open(QStringList &) override
    {
        recordControlledProviderAccess(state);
        if (state && state->mode == TestCloudCompletionMode::BlockInOpen) {
            blockControlledProviderCall(state);
        }
        if (state
            && state->mode
                    == TestCloudCompletionMode::QueuedSettingsTwice) {
            setSetting(
                QStringLiteral("<athlete-private>controlled/thread"),
                QStringLiteral("worker-value-1"));
            setSetting(
                QStringLiteral("<global-general>controlled/thread"),
                QStringLiteral("global-worker-value-1"));
            CloudServiceFactory::instance().saveSettings(this, context);
            setSetting(
                QStringLiteral("<athlete-private>controlled/thread"),
                QStringLiteral("worker-value-2"));
            setSetting(
                QStringLiteral("<global-general>controlled/thread"),
                QStringLiteral("global-worker-value-2"));
            CloudServiceFactory::instance().saveSettings(this, context);
            return true;
        }
        if (state
            && state->mode
                    == TestCloudCompletionMode::QueuedClearsUrlTwice) {
            setSetting(
                QStringLiteral("<athlete-private>controlled/url"),
                QString());
            CloudServiceFactory::instance().saveSettings(this, context);
            setSetting(
                QStringLiteral("<athlete-private>controlled/thread"),
                QStringLiteral("worker-value-after-url-clear"));
            CloudServiceFactory::instance().saveSettings(this, context);
            return true;
        }
        setSetting(
            QStringLiteral("<athlete-private>controlled/thread"),
            QStringLiteral("worker-value"));
        setSetting(
            QStringLiteral("<global-general>controlled/thread"),
            QStringLiteral("global-worker-value"));
        CloudServiceFactory::instance().saveSettings(this, context);
        return true;
    }

    bool close() override
    {
        recordControlledProviderAccess(state);
        return true;
    }

    void abortRequests() override
    {
        const std::shared_ptr<ControlledCloudState> currentState = state;
        if (!currentState) return;

        recordControlledProviderAccess(currentState);
        QEventLoop *blockedLoop = nullptr;
        bool useBaseAbort = false;
        {
            QMutexLocker locker(&currentState->mutex);
            ++currentState->abortCalls;
            blockedLoop = currentState->blockedLoop;
            useBaseAbort =
                    currentState->mode
                    == TestCloudCompletionMode::BlockInBaseAbort;
        }
        if (useBaseAbort) {
            CloudService::abortRequests();
            return;
        }
        if (blockedLoop) blockedLoop->quit();
    }

    QList<CloudServiceEntry *> readdir(
            QString, QStringList &, QDateTime, QDateTime) override
    {
        recordControlledProviderAccess(state);
        QList<CloudServiceEntry *> entries;
        if (!state) return entries;
        if (state->mode == TestCloudCompletionMode::BlockInDirectory) {
            blockControlledProviderCall(state);
        }

        const QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < state->entryCount; ++i) {
            CloudServiceEntry *entry = newCloudServiceEntry();
            entry->name =
                    now.addSecs(-i).toString(
                            QStringLiteral("yyyy_MM_dd_HH_mm_ss")) +
                    QStringLiteral(".fit");
            entry->id = QStringLiteral("controlled-%1").arg(i);
            entry->isDir = false;
            entries.append(entry);
        }
        return entries;
    }

    bool readFile(
            QByteArray *data,
            QString remoteName,
            QString) override
    {
        recordControlledProviderAccess(state);
        if (!state) return false;

        {
            QMutexLocker locker(&state->mutex);
            state->providerAddress = this;
            state->workerThread = QThread::currentThread();
        }
        state->reads.release();
        if (state->mode == TestCloudCompletionMode::BlockInBaseAbort) {
            auto *reply = new BaseAbortReply(this, state);
            QEventLoop loop;
            connect(
                reply, &QNetworkReply::finished,
                &loop, &QEventLoop::quit);
            {
                QMutexLocker locker(&state->mutex);
                ++state->callDepth;
                state->blockedLoop = &loop;
            }
            state->blockedCallStarted.release();
            loop.exec();
            {
                QMutexLocker locker(&state->mutex);
                state->blockedLoop = nullptr;
                --state->callDepth;
            }
            reply->deleteLater();
            return true;
        }
        data->append("not-a-valid-fit");
        if (state->mode == TestCloudCompletionMode::BlockInRead) {
            blockControlledProviderCall(state);
            return true;
        }

        const auto complete = [this, data, remoteName, state = state]() {
            notifyReadComplete(
                    data, remoteName, QStringLiteral("Completed."));
            state->completions.release();
        };

        switch (state->mode) {
        case TestCloudCompletionMode::Inline:
            complete();
            break;
        case TestCloudCompletionMode::InlineWithNestedEvents: {
            {
                QMutexLocker locker(&state->mutex);
                if (state->callDepth > 0)
                    ++state->reentrantReadCalls;
                ++state->callDepth;
            }
            complete();
            QEventLoop loop;
            QTimer::singleShot(0, &loop, &QEventLoop::quit);
            loop.exec();
            {
                QMutexLocker locker(&state->mutex);
                --state->callDepth;
            }
            break;
        }
        case TestCloudCompletionMode::Queued:
        case TestCloudCompletionMode::QueuedSettingsTwice:
        case TestCloudCompletionMode::QueuedClearsUrlTwice:
            QTimer::singleShot(0, this, complete);
            break;
        case TestCloudCompletionMode::Never:
            break;
        case TestCloudCompletionMode::Reject:
            return false;
        case TestCloudCompletionMode::Duplicate:
            complete();
            complete();
            break;
        case TestCloudCompletionMode::BlockInCompletion:
            QTimer::singleShot(
                0, this, [complete, state = state]() {
                    blockControlledProviderCall(state);
                    complete();
                });
            break;
        case TestCloudCompletionMode::BlockInOpen:
        case TestCloudCompletionMode::BlockInDirectory:
        case TestCloudCompletionMode::BlockInRead:
        case TestCloudCompletionMode::BlockInBaseAbort:
            break;
        }
        return true;
    }

private:
    std::shared_ptr<ControlledCloudState> state;
};

class SuccessfulFollowUpCloudService final : public CloudService
{
public:
    explicit SuccessfulFollowUpCloudService(Context *context)
        : CloudService(context)
    {
        downloadCompression = CloudService::none;
    }

    CloudService *clone(Context *context) override
    {
        return new SuccessfulFollowUpCloudService(context);
    }

    QString id() const override
    {
        return QString::fromLatin1(SuccessfulFollowUpServiceId);
    }

    QString uiName() const override
    {
        return QStringLiteral("Successful follow-up cloud");
    }

    QImage logo() const override { return {}; }
    bool open(QStringList &) override { return true; }
    bool close() override { return true; }

    QList<CloudServiceEntry *> readdir(
            QString, QStringList &, QDateTime, QDateTime) override
    {
        CloudServiceEntry *entry = newCloudServiceEntry();
        entry->name =
                QDateTime::currentDateTime().toString(
                        QStringLiteral("yyyy_MM_dd_HH_mm_ss")) +
                QStringLiteral(".fit");
        entry->id = QStringLiteral("successful-follow-up");
        entry->isDir = false;
        return {entry};
    }

    bool readFile(QByteArray *data, QString remoteName, QString) override
    {
        data->append("follow-up-payload");
        notifyReadComplete(
            data, remoteName, QStringLiteral("Completed."));
        return true;
    }
};

void registerBlockingService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId = QString::fromLatin1(BlockingServiceId);
    if (!factory.service(serviceId)) {
        factory.addService(new BlockingCloudService(nullptr));
    }
}

void registerControlledService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId = QString::fromLatin1(ControlledServiceId);
    if (!factory.service(serviceId)) {
        factory.addService(new ControlledCloudService(nullptr));
    }
}

void registerSuccessfulFollowUpService()
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString serviceId =
            QString::fromLatin1(SuccessfulFollowUpServiceId);
    if (!factory.service(serviceId)) {
        factory.addService(new SuccessfulFollowUpCloudService(nullptr));
    }
}

} // namespace

int cloudAutoDownloadRequestTimeoutMs()
{
    return configuredTimeoutMs.load(std::memory_order_relaxed);
}

void cloudAutoDownloadBufferAllocated(QByteArray *data)
{
    QMutexLocker locker(&bufferProbe.mutex);
    if (!bufferProbe.outstanding.contains(data)) {
        bufferProbe.outstanding.insert(data);
        ++bufferProbe.allocated;
    }
}

void cloudAutoDownloadBufferReleased(QByteArray *data)
{
    QMutexLocker locker(&bufferProbe.mutex);
    if (bufferProbe.outstanding.remove(data)) {
        ++bufferProbe.released;
    }
}

bool cloudAutoDownloadProviderAccessed(CloudService *provider)
{
    const std::shared_ptr<ControlledCloudState> state = controlledState;
    if (!state) return false;

    QMutexLocker locker(&state->mutex);
    if (state->providerAddress != provider) return false;

    if (QThread::currentThread() != state->workerThread) {
        ++state->crossThreadProviderAccesses;
    }
    if (state->destroyed) {
        ++state->destroyedProviderAccesses;
        return true;
    }
    return false;
}

void configureBlockingCloudAutoDownload(const QString &athlete)
{
    resetBufferProbe();
    configuredTimeoutMs.store(30000, std::memory_order_relaxed);
    blockingState = std::make_shared<BlockingCloudState>();
    registerBlockingService();

    const CloudService *service = CloudServiceFactory::instance().service(
            QString::fromLatin1(BlockingServiceId));
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
    {
        QMutexLocker locker(&blockingState->mutex);
        blockingState->data = nullptr;
    }
    blockingState.reset();
}

void configureControlledCloudAutoDownload(
        const QString &athlete,
        TestCloudCompletionMode mode,
        int entryCount,
        int timeoutMs)
{
    resetBufferProbe();
    configuredTimeoutMs.store(timeoutMs, std::memory_order_relaxed);
    controlledState =
            std::make_shared<ControlledCloudState>(mode, entryCount);
    controlledState->guiThread = QThread::currentThread();
    registerControlledService();

    const CloudService *service = CloudServiceFactory::instance().service(
            QString::fromLatin1(ControlledServiceId));
    appsettings->setCValue(
            athlete, service->syncOnStartupSettingName(),
            QStringLiteral("true"));
}

void disableControlledCloudAutoDownload(const QString &athlete)
{
    const CloudService *service =
            CloudServiceFactory::instance().service(
                QString::fromLatin1(ControlledServiceId));
    if (!service) return;
    appsettings->setCValue(
        athlete, service->syncOnStartupSettingName(),
        QStringLiteral("false"));
}

void enableSuccessfulFollowUpCloudAutoDownload(const QString &athlete)
{
    registerSuccessfulFollowUpService();
    const CloudService *service = CloudServiceFactory::instance().service(
            QString::fromLatin1(SuccessfulFollowUpServiceId));
    appsettings->setCValue(
            athlete, service->syncOnStartupSettingName(),
            QStringLiteral("true"));
}

bool waitForControlledCloudReads(int count, int timeoutMs)
{
    return controlledState &&
           controlledState->reads.tryAcquire(count, timeoutMs);
}

bool waitForControlledCloudCompletions(int count, int timeoutMs)
{
    return controlledState &&
           controlledState->completions.tryAcquire(count, timeoutMs);
}

bool waitForControlledCloudProviderDestroyed(int timeoutMs)
{
    return controlledState &&
           controlledState->providerDestroyed.tryAcquire(1, timeoutMs);
}

bool waitForControlledCloudBlockedCall(int timeoutMs)
{
    return controlledState &&
           controlledState->blockedCallStarted.tryAcquire(1, timeoutMs);
}

int controlledCloudCrossThreadProviderAccesses()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->crossThreadProviderAccesses;
}

int controlledCloudDestroyedProviderAccesses()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->destroyedProviderAccesses;
}

int controlledCloudDestroyedDuringCall()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->destroyedDuringCall;
}

int controlledCloudAbortCalls()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->abortCalls;
}

int controlledCloudBaseReplyAbortCalls()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->baseReplyAbortCalls;
}

int controlledCloudProviderOperations()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->providerOperations;
}

int controlledCloudReentrantReadCalls()
{
    if (!controlledState) return 0;
    QMutexLocker locker(&controlledState->mutex);
    return controlledState->reentrantReadCalls;
}

void cleanupControlledCloudAutoDownload()
{
    controlledState.reset();
    configuredTimeoutMs.store(30000, std::memory_order_relaxed);
}

int cloudAutoDownloadTestBuffersAllocated()
{
    QMutexLocker locker(&bufferProbe.mutex);
    return bufferProbe.allocated;
}

int cloudAutoDownloadTestBuffersReleased()
{
    QMutexLocker locker(&bufferProbe.mutex);
    return bufferProbe.released;
}

int cloudAutoDownloadTestBuffersOutstanding()
{
    QMutexLocker locker(&bufferProbe.mutex);
    return bufferProbe.outstanding.size();
}
