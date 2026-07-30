/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideFile.h"
#include "RideFileCache.h"
#include "RideFileCacheWriteError.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int FixedZoneFloatCount =
    10 + 4 + 10 + 4 + 10 + 4 + 4;

class FailOnThirdReadDevice : public QIODevice
{
public:
    explicit FailOnThirdReadDevice(QByteArray bytes)
        : bytes_(std::move(bytes))
    {
        open(
            QIODevice::ReadOnly
            | QIODevice::Unbuffered);
    }

    qint64 size() const override
    {
        return bytes_.size();
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > bytes_.size())
            return false;
        position_ = position;
        return QIODevice::seek(position);
    }

protected:
    qint64 readData(
        char *data,
        qint64 maximumSize) override
    {
        ++readCalls_;
        if (readCalls_ >= 3)
            return -1;
        const qint64 available =
            static_cast<qint64>(bytes_.size())
            - position_;
        const qint64 count =
            std::min(maximumSize, available);
        if (count <= 0)
            return 0;
        std::memcpy(
            data,
            bytes_.constData() + position_,
            static_cast<size_t>(count));
        position_ += count;
        return count;
    }

    qint64 writeData(
        const char *,
        qint64) override
    {
        return -1;
    }

private:
    QByteArray bytes_;
    qint64 position_ = 0;
    int readCalls_ = 0;
};

void writeCacheFixture(
    const QString &path,
    float best,
    float timeInZone)
{
    QVERIFY(QDir().mkpath(
        QFileInfo(path).absolutePath()));
    RideFileCacheHeader header {};
    header.version = RideFileCacheVersion;
    header.wattsMeanMaxCount = 2;
    QVector<float> payload(
        2 + FixedZoneFloatCount, 0.0f);
    payload[1] = best;
    payload[2] = timeInZone;

    QFile file(path);
    QVERIFY(file.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        file.write(
            reinterpret_cast<const char *>(&header),
            sizeof(header)),
        static_cast<qint64>(sizeof(header)));
    QCOMPARE(
        file.write(
            reinterpret_cast<const char *>(
                payload.constData()),
            static_cast<qint64>(
                payload.size() * sizeof(float))),
        static_cast<qint64>(
            payload.size() * sizeof(float)));
    file.close();
}

} // namespace

class TestRideFileCacheRefresh : public QObject
{
    Q_OBJECT

private slots:
    void refreshRestoresFixedZoneStorage();
    void repeatedRefreshClearsZoneValues();
    void temporaryActivityComputesWithoutPersistentCache();
    void plannedAndCompletedActivitiesUseSeparateCaches();
    void batchReadDiscardsRowAfterMidReadFailure();
    void crcReadFailureSkipsPersistence();
    void concurrentPersistenceFailuresKeepComputedResults();
};

void TestRideFileCacheRefresh::refreshRestoresFixedZoneStorage()
{
    RideFile ride;
    RideFileCache cache(
        &ride, RideFileCache::SkipInitialComputeForTest {});
    cache.wattsZoneArray().clear();
    cache.wattsCPZoneArray().clear();
    cache.hrZoneArray().clear();
    cache.hrCPZoneArray().clear();
    cache.paceZoneArray().clear();
    cache.paceCPZoneArray().clear();
    cache.wbalZoneArray().clear();
    cache.incomplete = true;

    cache.refresh(&ride);

    QVERIFY(!cache.incomplete);
    QCOMPARE(cache.wattsZoneArray().size(), 10);
    QCOMPARE(cache.wattsCPZoneArray().size(), 4);
    QCOMPARE(cache.hrZoneArray().size(), 10);
    QCOMPARE(cache.hrCPZoneArray().size(), 4);
    QCOMPARE(cache.paceZoneArray().size(), 10);
    QCOMPARE(cache.paceCPZoneArray().size(), 4);
    QCOMPARE(cache.wbalZoneArray().size(), 4);
}

void TestRideFileCacheRefresh::repeatedRefreshClearsZoneValues()
{
    RideFile ride;
    RideFileCache cache(
        &ride, RideFileCache::SkipInitialComputeForTest {});
    cache.wattsZoneArray().fill(10.0f);
    cache.wattsCPZoneArray().fill(10.0f);
    cache.hrZoneArray().fill(10.0f);
    cache.hrCPZoneArray().fill(10.0f);
    cache.paceZoneArray().fill(10.0f);
    cache.paceCPZoneArray().fill(10.0f);
    cache.wbalZoneArray().fill(10.0f);

    cache.refresh(&ride);

    const auto allZero = [](const QVector<float> &values) {
        return std::all_of(
            values.cbegin(), values.cend(),
            [](float value) { return value == 0.0f; });
    };
    QVERIFY(allZero(cache.wattsZoneArray()));
    QVERIFY(allZero(cache.wattsCPZoneArray()));
    QVERIFY(allZero(cache.hrZoneArray()));
    QVERIFY(allZero(cache.hrCPZoneArray()));
    QVERIFY(allZero(cache.paceZoneArray()));
    QVERIFY(allZero(cache.paceCPZoneArray()));
    QVERIFY(allZero(cache.wbalZoneArray()));
}

void
TestRideFileCacheRefresh::temporaryActivityComputesWithoutPersistentCache()
{
    RideFile ride;
    RideFileCache cache(
        &ride,
        RideFileCache::NoPersistentTargetForTest {});

    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::plannedAndCompletedActivitiesUseSeparateCaches()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString fileName =
        QStringLiteral("2026_07_29_12_00_00.json");
    const QString completedSource =
        QDir(completedRoot).filePath(fileName);
    const QString plannedSource =
        QDir(plannedRoot).filePath(fileName);
    QFile completedFile(completedSource);
    QVERIFY(completedFile.open(QIODevice::WriteOnly));
    completedFile.close();
    QFile plannedFile(plannedSource);
    QVERIFY(plannedFile.open(QIODevice::WriteOnly));
    plannedFile.close();

    writeCacheFixture(
        QDir(cacheRoot).filePath(
            QStringLiteral("2026_07_29_12_00_00.cpx")),
        210.0f,
        11.0f);
    writeCacheFixture(
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "planned/2026_07_29_12_00_00.cpx")),
        320.0f,
        22.0f);

    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            completedSource,
            RideFile::watts,
            1),
        210.0);
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            plannedSource,
            RideFile::watts,
            1),
        320.0);
    QCOMPARE(
        RideFileCache::tizForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            completedSource,
            RideFile::watts,
            1),
        11);
    QCOMPARE(
        RideFileCache::tizForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            plannedSource,
            RideFile::watts,
            1),
        22);
}

void
TestRideFileCacheRefresh::batchReadDiscardsRowAfterMidReadFailure()
{
    RideFileCacheHeader header {};
    header.version = RideFileCacheVersion;
    header.wattsMeanMaxCount = 2;
    QVector<float> payload(
        2 + FixedZoneFloatCount, 0.0f);
    payload[0] = 210.0f;
    payload[1] = 205.0f;
    QByteArray bytes(
        reinterpret_cast<const char *>(&header),
        sizeof(header));
    bytes.append(
        reinterpret_cast<const char *>(
            payload.constData()),
        static_cast<qsizetype>(
            payload.size() * sizeof(float)));
    FailOnThirdReadDevice input(std::move(bytes));
    const QVector<
        QPair<RideFile::SeriesType, int>>
        requests = {
            {RideFile::watts, 0},
            {RideFile::watts, 1}
        };
    QVector<double> values({999.0});

    QVERIFY(
        !RideFileCache::readBestRowForTest(
            input, requests, values));
    QVERIFY(values.isEmpty());
}

void
TestRideFileCacheRefresh::crcReadFailureSkipsPersistence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RideFile ride;
    RideFileCache cache(
        &ride,
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    int reportCalls = 0;

    const bool persisted = cache.refreshCacheForTest(
        directory.filePath(QStringLiteral("missing.fit")),
        directory.filePath(QStringLiteral("cache/missing.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *error) {
            ++writeCalls;
            if (error)
                *error = QStringLiteral("unexpected write");
            return false;
        },
        [&](const QString &, const QString &) {
            ++reportCalls;
        });

    QVERIFY(!persisted);
    QCOMPARE(writeCalls, 0);
    QCOMPARE(reportCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::concurrentPersistenceFailuresKeepComputedResults()
{
    constexpr int WorkerCount = 4;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVector<QString> sourcePaths;
    QVector<QString> cachePaths;
    for (int index = 0; index < WorkerCount; ++index) {
        const QString sourcePath =
            directory.filePath(
                QStringLiteral("source-%1.fit").arg(index));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.close();
        sourcePaths.append(sourcePath);
        cachePaths.append(
            directory.filePath(
                QStringLiteral("cache/%1.cpx").arg(index)));
    }

    RideFileCacheWriteErrorCoordinator coordinator;
    QMutex deliveryMutex;
    QVector<RideFileCacheWriteErrorCoordinator::Delivery> deliveries;
    const auto dispatch =
        [&](RideFileCacheWriteErrorCoordinator::Delivery delivery) {
            QMutexLocker locker(&deliveryMutex);
            deliveries.append(std::move(delivery));
            return true;
        };
    int notificationCount = 0;
    QThread *notificationThread = nullptr;
    const auto notify = [&](const QString &) {
        ++notificationCount;
        notificationThread = QThread::currentThread();
    };

    std::atomic<int> writeCalls {0};
    std::atomic<int> reportCalls {0};
    std::atomic<int> completeResults {0};
    std::atomic<int> ready {0};
    std::atomic<bool> start {false};
    std::mutex writerGateMutex;
    std::condition_variable writerGateChanged;
    int writersReady = 0;
    bool releaseWriters = false;
    bool writerGateTimedOut = false;
    std::vector<std::thread> workers;
    workers.reserve(WorkerCount);
    for (int index = 0; index < WorkerCount; ++index) {
        workers.emplace_back([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            RideFile ride;
            RideFileCache cache(
                &ride,
                RideFileCache::SkipInitialComputeForTest {});
            const bool persisted = cache.refreshCacheForTest(
                sourcePaths.at(index),
                cachePaths.at(index),
                [&](const QString &,
                    const RideFileCacheIntegrity::CacheWriteOperation &,
                    QString *error) {
                    writeCalls.fetch_add(
                        1, std::memory_order_relaxed);
                    {
                        std::unique_lock<std::mutex> lock(
                            writerGateMutex);
                        ++writersReady;
                        if (writersReady == WorkerCount) {
                            releaseWriters = true;
                            writerGateChanged.notify_all();
                        } else if (!writerGateChanged.wait_for(
                                       lock,
                                       std::chrono::seconds(5),
                                       [&]() { return releaseWriters; })) {
                            writerGateTimedOut = true;
                            releaseWriters = true;
                            lock.unlock();
                            writerGateChanged.notify_all();
                        }
                    }
                    if (error)
                        *error = QStringLiteral("injected write failure");
                    return false;
                },
                [&](const QString &path, const QString &error) {
                    reportCalls.fetch_add(
                        1, std::memory_order_relaxed);
                    coordinator.report(
                        path, error, dispatch, notify);
                });
            if (!persisted && !cache.incomplete)
                completeResults.fetch_add(
                    1, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) != WorkerCount)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers)
        worker.join();

    QCOMPARE(writeCalls.load(), WorkerCount);
    QCOMPARE(reportCalls.load(), WorkerCount);
    QCOMPARE(completeResults.load(), WorkerCount);
    QCOMPARE(notificationCount, 0);
    QVERIFY(!writerGateTimedOut);

    RideFileCacheWriteErrorCoordinator::Delivery delivery;
    {
        QMutexLocker locker(&deliveryMutex);
        QCOMPARE(deliveries.size(), 1);
        delivery = std::move(deliveries.front());
    }
    delivery();
    QCOMPARE(notificationCount, 1);
    QCOMPARE(notificationThread, QThread::currentThread());
}

QTEST_GUILESS_MAIN(TestRideFileCacheRefresh)

#include "testRideFileCacheRefresh.moc"
