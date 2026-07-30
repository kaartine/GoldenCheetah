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
#include "WPrime.h"

#include <QByteArrayView>
#include <QBuffer>
#include <QCryptographicHash>
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
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int FixedZoneFloatCount =
    10 + 4 + 10 + 4 + 10 + 4 + 4;

QByteArray bytesForValue(quint32 value)
{
    QByteArray bytes(4, '\0');
    bytes[0] = static_cast<char>(value & 0xff);
    bytes[1] = static_cast<char>((value >> 8) & 0xff);
    bytes[2] = static_cast<char>((value >> 16) & 0xff);
    bytes[3] = static_cast<char>((value >> 24) & 0xff);
    return bytes;
}

QPair<QByteArray, QByteArray> crc16Collision()
{
    std::vector<int> firstSeen(1 << 16, -1);
    for (quint32 value = 0; value <= (1U << 16); ++value) {
        const QByteArray candidate = bytesForValue(value);
        const quint16 checksum =
            qChecksum(QByteArrayView(candidate));
        if (firstSeen[checksum] >= 0) {
            return {
                bytesForValue(
                    static_cast<quint32>(
                        firstSeen[checksum])),
                candidate
            };
        }
        firstSeen[checksum] =
            static_cast<int>(value);
    }
    return {};
}

void makeSourceOlderThanCache(
    const QString &sourcePath,
    const QString &cachePath)
{
    const QDateTime oldTime =
        QFileInfo(cachePath).lastModified().addSecs(-60);
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadWrite));
    QVERIFY(source.setFileTime(
        oldTime,
        QFileDevice::FileModificationTime));
    source.close();
    QVERIFY(
        QFileInfo(sourcePath).lastModified()
        <= QFileInfo(cachePath).lastModified());
}

void setModificationTime(
    const QString &path,
    const QDateTime &time)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(
        time,
        QFileDevice::FileModificationTime));
}

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void writeFileBytes(
    const QString &path,
    const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(
        QIODevice::WriteOnly
        | QIODevice::Truncate));
    QCOMPARE(
        file.write(bytes),
        static_cast<qint64>(
            bytes.size()));
}

void resealCacheBytes(QByteArray &bytes)
{
    QVERIFY(
        bytes.size()
        >= RideFileCacheIntegrity::
            CacheFooterBytes);
    bytes.chop(
        RideFileCacheIntegrity::
            CacheFooterBytes);
    bytes.append(
        QCryptographicHash::hash(
            bytes,
            QCryptographicHash::Sha256));
}

class ProvenanceTestReader final : public RideFileReader
{
public:
    RideFile *openRideFile(
        QFile &file,
        QStringList &errors,
        QList<RideFile *> *) const override
    {
        QString currentPath = file.fileName();
        currentPath.detach();
        {
            QMutexLocker locker(&openedPathMutex);
            openedPath = std::move(currentPath);
        }
        if (!file.isOpen()
            && !file.open(QIODevice::ReadOnly)) {
            errors.append(file.errorString());
            return nullptr;
        }
        if (!file.seek(0)) {
            errors.append(file.errorString());
            return nullptr;
        }
        const QByteArray contents = file.readAll();
        file.close();

        auto *ride = new RideFile;
        ride->setRecIntSecs(1.0);
        if (!contents.isEmpty()) {
            RideFilePoint point;
            point.secs = 1.0;
            point.watts =
                static_cast<unsigned char>(contents.at(0));
            ride->appendPoint(point);
        }
        return ride;
    }

    bool requiresOriginalSourcePath() const override
    {
        return false;
    }

    QString openedPathForTest() const
    {
        QMutexLocker locker(&openedPathMutex);
        QString snapshot = openedPath;
        snapshot.detach();
        return snapshot;
    }

private:
    mutable QMutex openedPathMutex;
    mutable QString openedPath;
};

class PathDependentTestReader final : public RideFileReader
{
public:
    RideFile *openRideFile(
        QFile &file,
        QStringList &,
        QList<RideFile *> *) const override
    {
        openedPath = file.fileName();
        return new RideFile;
    }

    bool requiresOriginalSourcePath() const override
    {
        return true;
    }

    mutable QString openedPath;
};

class UnauditedTestReader final : public RideFileReader
{
public:
    RideFile *openRideFile(
        QFile &file,
        QStringList &,
        QList<RideFile *> *) const override
    {
        openedPath = file.fileName();
        return new RideFile;
    }

    mutable QString openedPath;
};

class MutatingTestReader final : public RideFileReader
{
public:
    RideFile *openRideFile(
        QFile &file,
        QStringList &errors,
        QList<RideFile *> *) const override
    {
        if (!file.open(QIODevice::ReadWrite)
            || file.write(QByteArrayLiteral("changed"))
                != qint64(7)) {
            errors.append(file.errorString());
            return nullptr;
        }
        file.close();
        return new RideFile;
    }

    bool requiresOriginalSourcePath() const override
    {
        return false;
    }
};

ProvenanceTestReader &provenanceTestReader()
{
    static ProvenanceTestReader reader;
    return reader;
}

void registerProvenanceTestReader()
{
    RideFileFactory::instance().registerReader(
        QStringLiteral("provenance"),
        QStringLiteral("provenance test"),
        &provenanceTestReader());
}

class FailOnFifthReadDevice : public QIODevice
{
public:
    explicit FailOnFifthReadDevice(QByteArray bytes)
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
        if (readCalls_ >= 5)
            return -1;
        const qint64 available =
            static_cast<qint64>(bytes_.size())
            - position_;
        const qint64 count =
            std::min({
                maximumSize,
                available,
                qint64(32)
            });
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

class ThreadJoiner
{
public:
    explicit ThreadJoiner(
        std::vector<std::thread> &threads)
        : threads_(threads)
    {
    }

    ~ThreadJoiner()
    {
        for (std::thread &thread : threads_) {
            if (thread.joinable())
                thread.join();
        }
    }

private:
    std::vector<std::thread> &threads_;
};

void writeCacheFixture(
    const QString &path,
    float best,
    float timeInZone,
    const QByteArray &sourceBytes = {},
    double weight = 0.0)
{
    QVERIFY(QDir().mkpath(
        QFileInfo(path).absolutePath()));
    RideFileCacheHeader header {};
    header.version = RideFileCacheVersion;
    header.crc = qChecksum(
        QByteArrayView(sourceBytes));
    header.WEIGHT = weight;
    header.wattsMeanMaxCount = 2;
    QVector<float> payload(
        2 + FixedZoneFloatCount, 0.0f);
    payload[1] = best;
    payload[2] = timeInZone;

    QByteArray cacheBytes(
        reinterpret_cast<const char *>(&header),
        sizeof(header));
    const qint64 sourceByteSize =
        sourceBytes.size();
    cacheBytes.append(
        reinterpret_cast<const char *>(
            &sourceByteSize),
        sizeof(sourceByteSize));
    const QByteArray sourceSha256 =
        QCryptographicHash::hash(
            sourceBytes,
            QCryptographicHash::Sha256);
    cacheBytes.append(sourceSha256);
    cacheBytes.append(
        reinterpret_cast<const char *>(
            payload.constData()),
        static_cast<qsizetype>(
            payload.size() * sizeof(float)));
    cacheBytes.append(
        QCryptographicHash::hash(
            cacheBytes,
            QCryptographicHash::Sha256));

    QFile file(path);
    QVERIFY(file.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        file.write(cacheBytes),
        static_cast<qint64>(
            cacheBytes.size()));
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
    void standalonePowerActivityComputesWithoutContext();
    void standaloneWPrimeWithoutZonesStaysEmpty();
    void plannedAndCompletedActivitiesUseSeparateCaches();
    void batchReadDiscardsRowAfterMidReadFailure();
    void crcReadFailureSkipsPersistence();
    void restoredMtimeSourceChangeRejectsCache();
    void missingSourceRejectsCache();
    void crc16CollisionRejectsCache();
    void matchingSourceAcceptedRegardlessOfMtime();
    void sourceChangeDuringReadRejectsResults();
    void failedReadConsumesSourceMutationHook();
    void sourceFingerprintReadsAreNotAmplified();
    void corruptPayloadRejectsFastReaders();
    void sourceFingerprintMismatchRejectsFastReaders();
    void apiReadersRejectChangedSource();
    void batchReadersRejectChangedSource();
    void aggregateBindingsRejectChangedSource();
    void factoryCapturesSourceProvenance();
    void factoryLeavesUnauditedReaderUnprovenanced();
    void factoryLeavesPathDependentReaderUnprovenanced();
    void factoryRejectsParserMutatedStage();
    void unprovenancedRideSkipsPersistence();
    void savedRideRebindsAndPersistsAtomically();
    void matchingSourceProvenanceAllowsPersistenceAttempt();
    void sourceChangeInvalidatesPersistence();
    void rideMutationInvalidatesPersistence();
    void directPointMutationSkipsPersistence();
    void sourceChangeBeforeCommitSkipsPersistence();
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
TestRideFileCacheRefresh::standalonePowerActivityComputesWithoutContext()
{
    RideFile ride;
    ride.setRecIntSecs(1.0);
    RideFilePoint point;
    point.secs = 1.0;
    point.watts = 200.0;
    ride.appendPoint(point);

    RideFileCache cache(&ride);

    QVERIFY(!cache.incomplete);
    const QVector<double> &distribution =
        cache.distributionArray(RideFile::watts);
    QVERIFY(distribution.size() > 200);
    QCOMPARE(distribution.at(200), 1.0);
}

void
TestRideFileCacheRefresh::standaloneWPrimeWithoutZonesStaysEmpty()
{
    RideFile ride;
    ride.setRecIntSecs(1.0);
    RideFilePoint point;
    point.secs = 1.0;
    point.watts = 250.0;
    ride.appendPoint(point);
    ride.wprimeData()->ydata().append(1000.0);

    RideFileCache cache(
        &ride,
        RideFileCache::NoPersistentTargetForTest {});

    QVERIFY(!cache.incomplete);
    const QVector<double> distribution =
        cache.distributionArray(RideFile::wbal);
    QVERIFY(std::all_of(
        distribution.cbegin(),
        distribution.cend(),
        [](double value) { return value == 0.0; }));
    QVERIFY(std::all_of(
        cache.wbalZoneArray().cbegin(),
        cache.wbalZoneArray().cend(),
        [](float value) { return value == 0.0f; }));
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

    const QString completedCache =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    QVERIFY(completedFile.open(QIODevice::WriteOnly));
    QCOMPARE(
        completedFile.write(
            QByteArrayLiteral("changed")),
        qint64(7));
    completedFile.close();
    makeSourceOlderThanCache(
        completedSource, completedCache);
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            completedSource,
            RideFile::watts,
            1),
        0.0);
    QCOMPARE(
        RideFileCache::tizForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            completedSource,
            RideFile::watts,
            1),
        0);
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
    const qint64 sourceByteSize = 0;
    bytes.append(
        reinterpret_cast<const char *>(
            &sourceByteSize),
        sizeof(sourceByteSize));
    bytes.append(
        RideFileCRC::Sha256Size,
        '\0');
    bytes.append(
        reinterpret_cast<const char *>(
            payload.constData()),
        static_cast<qsizetype>(
            payload.size() * sizeof(float)));
    bytes.append(
        QCryptographicHash::hash(
            bytes,
            QCryptographicHash::Sha256));
    FailOnFifthReadDevice input(std::move(bytes));
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
TestRideFileCacheRefresh::
restoredMtimeSourceChangeRejectsCache()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.fit"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/source.cpx"));
    const QByteArray original =
        QByteArrayLiteral("source-a");
    const QByteArray replacement =
        QByteArrayLiteral("source-b");
    QCOMPARE(original.size(), replacement.size());

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), qint64(original.size()));
    source.close();

    constexpr double Weight = 75.0;
    writeCacheFixture(
        cachePath, 100.0f, 30.0f,
        original, Weight);
    QVERIFY(RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));

    QVERIFY(source.open(
        QIODevice::WriteOnly
        | QIODevice::Truncate));
    QCOMPARE(
        source.write(replacement),
        qint64(replacement.size()));
    source.close();

    const QDateTime cacheTime =
        QFileInfo(cachePath).lastModified();
    setModificationTime(
        sourcePath,
        cacheTime.addSecs(60));
    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
    setModificationTime(
        sourcePath, cacheTime);
    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
    makeSourceOlderThanCache(
        sourcePath, cachePath);

    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
}

void
TestRideFileCacheRefresh::missingSourceRejectsCache()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("missing.fit"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/missing.cpx"));
    constexpr double Weight = 75.0;
    writeCacheFixture(
        cachePath, 100.0f, 30.0f,
        QByteArrayLiteral("unavailable"),
        Weight);

    QVERIFY(!QFileInfo::exists(sourcePath));
    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
}

void
TestRideFileCacheRefresh::crc16CollisionRejectsCache()
{
    const auto collision = crc16Collision();
    const QByteArray &original = collision.first;
    const QByteArray &replacement = collision.second;
    QVERIFY(!original.isEmpty());
    QVERIFY(original != replacement);
    QCOMPARE(original.size(), replacement.size());
    QCOMPARE(
        qChecksum(QByteArrayView(original)),
        qChecksum(QByteArrayView(replacement)));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.fit"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/source.cpx"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(original),
        qint64(original.size()));
    source.close();

    constexpr double Weight = 75.0;
    writeCacheFixture(
        cachePath, 100.0f, 30.0f,
        original, Weight);

    QVERIFY(source.open(
        QIODevice::WriteOnly
        | QIODevice::Truncate));
    QCOMPARE(
        source.write(replacement),
        qint64(replacement.size()));
    source.close();
    makeSourceOlderThanCache(
        sourcePath, cachePath);

    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
}

void
TestRideFileCacheRefresh::
matchingSourceAcceptedRegardlessOfMtime()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.fit"));
    const QString cachePath =
        directory.filePath(
            QStringLiteral("cache/source.cpx"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    writeFileBytes(sourcePath, sourceBytes);
    constexpr double Weight = 75.0;
    writeCacheFixture(
        cachePath,
        100.0f,
        30.0f,
        sourceBytes,
        Weight);

    const QDateTime baseline =
        QDateTime::currentDateTimeUtc()
            .addSecs(-120);
    setModificationTime(
        cachePath, baseline);
    setModificationTime(
        sourcePath,
        baseline.addSecs(60));
    QVERIFY(RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));

    setModificationTime(
        sourcePath, baseline);
    QVERIFY(RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));

    setModificationTime(
        sourcePath,
        baseline.addSecs(-60));
    QVERIFY(RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath, cachePath, Weight));
    QVERIFY(!RideFileCache::
        cacheIsCurrentForSourceForTest(
            sourcePath,
            cachePath,
            Weight + 1.0));
}

void
TestRideFileCacheRefresh::
sourceChangeDuringReadRejectsResults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(
            QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(
            QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(
            QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString fileName =
        QStringLiteral(
            "2026_07_29_12_00_00.fit");
    const QString sourcePath =
        QDir(completedRoot).filePath(
            fileName);
    const QString cachePath =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray original =
        QByteArrayLiteral("source-a");
    const QByteArray replacement =
        QByteArrayLiteral("source-b");
    writeFileBytes(sourcePath, original);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        original);

    RideFileCache::
        setSourceBoundReadHookForTest(
            [sourcePath, replacement]() {
                writeFileBytes(
                    sourcePath,
                    replacement);
            });
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        0.0);
    QCOMPARE(
        readFileBytes(sourcePath),
        replacement);
}

void
TestRideFileCacheRefresh::
failedReadConsumesSourceMutationHook()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(
            QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(
            QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(
            QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString sourcePath =
        QDir(completedRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.fit"));
    const QString missingPath =
        QDir(completedRoot).filePath(
            QStringLiteral(
                "2026_07_28_12_00_00.fit"));
    const QString cachePath =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    writeFileBytes(
        sourcePath, sourceBytes);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        sourceBytes);

    bool hookRan = false;
    RideFileCache::
        setSourceBoundReadHookForTest(
            [&hookRan]() {
                hookRan = true;
            });
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            missingPath,
            RideFile::watts,
            1),
        0.0);
    QVERIFY(!hookRan);

    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        210.0);
    QVERIFY(!hookRan);
}

void
TestRideFileCacheRefresh::
sourceFingerprintReadsAreNotAmplified()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(
            QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(
            QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(
            QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString sourcePath =
        QDir(completedRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.fit"));
    const QString cachePath =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    writeFileBytes(
        sourcePath, sourceBytes);

    RideFileCache::
        resetSourceFingerprintReadCountForTest();
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        0.0);
    QCOMPARE(
        RideFileCache::
            sourceFingerprintReadCountForTest(),
        0);

    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        sourceBytes);
    RideFileCache::
        resetSourceFingerprintReadCountForTest();
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        210.0);
    QCOMPARE(
        RideFileCache::
            sourceFingerprintReadCountForTest(),
        1);
}

void
TestRideFileCacheRefresh::
corruptPayloadRejectsFastReaders()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(
            QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(
            QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(
            QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString sourcePath =
        QDir(completedRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.fit"));
    const QString cachePath =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    writeFileBytes(
        sourcePath, sourceBytes);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        sourceBytes);
    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        210.0);

    QByteArray cacheBytes =
        readFileBytes(cachePath);
    const qsizetype valueOffset =
        RideFileCacheIntegrity::
            CachePreambleBytes
        + static_cast<qsizetype>(
            sizeof(float));
    QVERIFY(valueOffset < cacheBytes.size());
    cacheBytes[valueOffset] ^= 0x01;
    writeFileBytes(
        cachePath, cacheBytes);

    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        0.0);
    QCOMPARE(
        RideFileCache::tizForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        0);
}

void
TestRideFileCacheRefresh::
sourceFingerprintMismatchRejectsFastReaders()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString cacheRoot =
        directory.filePath(
            QStringLiteral("cache"));
    const QString completedRoot =
        directory.filePath(
            QStringLiteral("activities"));
    const QString plannedRoot =
        directory.filePath(
            QStringLiteral("planned"));
    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(completedRoot));
    QVERIFY(QDir().mkpath(plannedRoot));
    const QString sourcePath =
        QDir(completedRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.fit"));
    const QString cachePath =
        QDir(cacheRoot).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    writeFileBytes(
        sourcePath, sourceBytes);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        sourceBytes);

    QByteArray cacheBytes =
        readFileBytes(cachePath);
    const qsizetype sourceDigestOffset =
        sizeof(RideFileCacheHeader)
        + sizeof(qint64);
    QVERIFY(
        sourceDigestOffset
        < cacheBytes.size());
    cacheBytes[sourceDigestOffset] ^=
        0x01;
    resealCacheBytes(cacheBytes);
    writeFileBytes(
        cachePath, cacheBytes);

    QCOMPARE(
        RideFileCache::bestForActivityForTest(
            cacheRoot,
            completedRoot,
            plannedRoot,
            sourcePath,
            RideFile::watts,
            1),
        0.0);
}

void
TestRideFileCacheRefresh::
apiReadersRejectChangedSource()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString activityDir =
        directory.filePath(
            QStringLiteral("activities"));
    const QString cacheDir =
        directory.filePath(
            QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(activityDir));
    QVERIFY(QDir().mkpath(cacheDir));
    const QString sourcePath =
        QDir(activityDir).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.fit"));
    const QString cachePath =
        QDir(cacheDir).filePath(
            QStringLiteral(
                "2026_07_29_12_00_00.cpx"));
    const QByteArray original =
        QByteArrayLiteral("source-a");
    const QByteArray replacement =
        QByteArrayLiteral("source-b");
    writeFileBytes(sourcePath, original);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        original);

    const QVector<float> single =
        RideFileCache::meanMaxFor(
            sourcePath,
            cachePath,
            RideFile::watts);
    QCOMPARE(single.size(), 2);
    QCOMPARE(single.at(1), 210.0f);
    const QVector<float> range =
        RideFileCache::meanMaxFor(
            activityDir,
            cacheDir,
            RideFile::watts,
            QDate(2026, 7, 29),
            QDate(2026, 7, 29));
    QCOMPARE(range.size(), 2);
    QCOMPARE(range.at(1), 210.0f);

    writeFileBytes(
        sourcePath, replacement);
    makeSourceOlderThanCache(
        sourcePath, cachePath);
    QVERIFY(
        RideFileCache::meanMaxFor(
            sourcePath,
            cachePath,
            RideFile::watts)
            .isEmpty());
    QVERIFY(
        RideFileCache::meanMaxFor(
            activityDir,
            cacheDir,
            RideFile::watts,
            QDate(2026, 7, 29),
            QDate(2026, 7, 29))
            .isEmpty());
}

void
TestRideFileCacheRefresh::
batchReadersRejectChangedSource()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.fit"));
    const QString cachePath =
        directory.filePath(
            QStringLiteral("cache/source.cpx"));
    const QByteArray original =
        QByteArrayLiteral("source-a");
    const QByteArray replacement =
        QByteArrayLiteral("source-b");
    writeFileBytes(sourcePath, original);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        original);
    const QVector<
        QPair<RideFile::SeriesType, int>>
        requests = {
            {RideFile::watts, 1}
        };
    QVector<double> values;
    QVERIFY(
        RideFileCache::
            readBestRowForSourceForTest(
                sourcePath,
                cachePath,
                requests,
                values));
    QCOMPARE(values, QVector<double>({210.0}));

    writeFileBytes(
        sourcePath, replacement);
    values = {999.0};
    QVERIFY(
        !RideFileCache::
            readBestRowForSourceForTest(
                sourcePath,
                cachePath,
                requests,
                values));
    QVERIFY(values.isEmpty());
}

void
TestRideFileCacheRefresh::
aggregateBindingsRejectChangedSource()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.fit"));
    writeFileBytes(
        sourcePath,
        QByteArrayLiteral("source-a"));
    RideFileCRC::ContentFingerprint
        fingerprint;
    QVERIFY(
        RideFileCRC::computeFileFingerprint(
            sourcePath, fingerprint));
    const QVector<
        QPair<
            QString,
            RideFileCRC::ContentFingerprint>>
        bindings = {
            {sourcePath, fingerprint}
        };
    QVERIFY(
        RideFileCache::
            sourceBindingsAreCurrentForTest(
                bindings));

    writeFileBytes(
        sourcePath,
        QByteArrayLiteral("source-b"));
    QVERIFY(
        !RideFileCache::
            sourceBindingsAreCurrentForTest(
                bindings));
}

void
TestRideFileCacheRefresh::factoryCapturesSourceProvenance()
{
    registerProvenanceTestReader();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral(
                "2026_07_29_12_34_56.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-a")),
        qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));

    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    const QString openedPath =
        provenanceTestReader().openedPathForTest();
    QVERIFY(openedPath != sourcePath);
    QCOMPARE(
        QFileInfo(
            openedPath).completeSuffix(),
        QStringLiteral("provenance"));
    QCOMPARE(
        ride->startTime(),
        QDateTime(
            QDate(2026, 7, 29),
            QTime(12, 34, 56)));
    QVERIFY(ride->sourceProvenanceMatchesForTest(sourcePath));
}

void
TestRideFileCacheRefresh::
factoryLeavesUnauditedReaderUnprovenanced()
{
    static UnauditedTestReader reader;
    RideFileFactory::instance().registerReader(
        QStringLiteral("unaudited"),
        QStringLiteral("unaudited test"),
        &reader);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.unaudited"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(QByteArrayLiteral("source")), qint64(6));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));

    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(reader.openedPath, sourcePath);
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
    QVERIFY(ride->bindSourceProvenanceForTest(sourcePath));
    ride->rebindSourceProvenanceForTest(sourcePath);
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
}

void
TestRideFileCacheRefresh::
factoryLeavesPathDependentReaderUnprovenanced()
{
    static PathDependentTestReader reader;
    RideFileFactory::instance().registerReader(
        QStringLiteral("dependent"),
        QStringLiteral("path-dependent test"),
        &reader);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.dependent"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(QByteArrayLiteral("source")), qint64(6));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));

    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(reader.openedPath, sourcePath);
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
    QVERIFY(ride->bindSourceProvenanceForTest(sourcePath));
    ride->rebindSourceProvenanceForTest(sourcePath);
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
}

void
TestRideFileCacheRefresh::factoryRejectsParserMutatedStage()
{
    static MutatingTestReader reader;
    RideFileFactory::instance().registerReader(
        QStringLiteral("mutating"),
        QStringLiteral("mutating test"),
        &reader);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.mutating"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(sourceBytes), qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));

    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), sourceBytes);
}

void
TestRideFileCacheRefresh::unprovenancedRideSkipsPersistence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.fit"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-a")),
        qint64(8));
    source.close();

    RideFile ride;
    RideFileCache cache(
        &ride,
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    int reportCalls = 0;

    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *) {
            ++writeCalls;
            return false;
        },
        [&](const QString &, const QString &) {
            ++reportCalls;
        }));

    QCOMPARE(writeCalls, 0);
    QCOMPARE(reportCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::
savedRideRebindsAndPersistsAtomically()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral(
                "2026_07_29_12_34_56.provenance"));
    const QByteArray sourceBytes =
        QByteArrayLiteral("source-a");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(sourceBytes), qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(ride->sourceProvenanceMatchesForTest(sourcePath));

    ride->setStartTime(ride->startTime());
    QVERIFY(!ride->sourceProvenanceMatchesForTest(sourcePath));
    ride->rebindSourceProvenanceForTest(sourcePath);
    QVERIFY(ride->sourceProvenanceMatchesForTest(sourcePath));

    const QString cachePath =
        directory.filePath(
            QStringLiteral("cache/source.cpx"));
    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    int reportCalls = 0;

    QVERIFY(cache.refreshCacheWithValidatorForTest(
        sourcePath,
        cachePath,
        [&](const QString &path,
            const RideFileCacheIntegrity::CacheWriteOperation &write,
            const RideFileCacheIntegrity::
                CachePreCommitValidator &validate,
            QString *error) {
            ++writeCalls;
            return RideFileCacheIntegrity::writeCacheAtomically(
                path, write, validate, error);
        },
        [&](const QString &, const QString &) {
            ++reportCalls;
        }));

    QCOMPARE(writeCalls, 1);
    QCOMPARE(reportCalls, 0);
    QVERIFY(!cache.incomplete);

    QFile persisted(cachePath);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    RideFileCacheIntegrity::CacheData data;
    QString readError;
    QVERIFY2(
        RideFileCacheIntegrity::readCache(
            persisted, data, &readError),
        qPrintable(readError));
    QVERIFY(data.complete);
    QCOMPARE(
        data.header.crc,
        static_cast<unsigned int>(
            qChecksum(QByteArrayView(sourceBytes))));
    QCOMPARE(
        data.sourceFingerprint.byteSize,
        static_cast<qint64>(
            sourceBytes.size()));
    QCOMPARE(
        data.sourceFingerprint.sha256,
        QCryptographicHash::hash(
            sourceBytes,
            QCryptographicHash::Sha256));
    QCOMPARE(
        data.sourceFingerprint.legacyCrc16,
        qChecksum(
            QByteArrayView(sourceBytes)));
}

void
TestRideFileCacheRefresh::
matchingSourceProvenanceAllowsPersistenceAttempt()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-a")),
        qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    int reportCalls = 0;

    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *error) {
            ++writeCalls;
            if (error)
                *error = QStringLiteral("injected write failure");
            return false;
        },
        [&](const QString &, const QString &) {
            ++reportCalls;
        }));

    QCOMPARE(writeCalls, 1);
    QCOMPARE(reportCalls, 1);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::sourceChangeInvalidatesPersistence()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-a")),
        qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-b")),
        qint64(8));
    source.close();

    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;

    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *) {
            ++writeCalls;
            return false;
        },
        {}));

    QCOMPARE(writeCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::rideMutationInvalidatesPersistence()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("source-a")),
        qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    ride->setStartTime(
        QDateTime::fromSecsSinceEpoch(1));

    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;

    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *) {
            ++writeCalls;
            return false;
        },
        {}));

    QCOMPARE(writeCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::directPointMutationSkipsPersistence()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(QByteArrayLiteral("A")), qint64(1));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(ride->dataPoints().size(), 1);
    ride->dataPoints().constFirst()->watts += 100.0;

    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *) {
            ++writeCalls;
            return false;
        },
        {}));

    QCOMPARE(writeCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::
sourceChangeBeforeCommitSkipsPersistence()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(
            QStringLiteral("source.provenance"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(QByteArrayLiteral("source-a")), qint64(8));
    source.close();

    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    int writeCalls = 0;
    int validationCalls = 0;
    int reportCalls = 0;

    QVERIFY(!cache.refreshCacheWithValidatorForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/source.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &write,
            const RideFileCacheIntegrity::CachePreCommitValidator &validate,
            QString *error) {
            ++writeCalls;
            QByteArray bytes;
            QBuffer output(&bytes);
            if (!output.open(QIODevice::WriteOnly)
                || !write(output, error)) {
                return false;
            }
            QFile changed(sourcePath);
            if (!changed.open(
                    QIODevice::WriteOnly
                    | QIODevice::Truncate)
                || changed.write(
                    QByteArrayLiteral("source-b"))
                    != qint64(8)) {
                return false;
            }
            changed.close();
            ++validationCalls;
            return validate(error);
        },
        [&](const QString &, const QString &) {
            ++reportCalls;
        }));

    QCOMPARE(writeCalls, 1);
    QCOMPARE(validationCalls, 1);
    QCOMPARE(reportCalls, 0);
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::concurrentPersistenceFailuresKeepComputedResults()
{
    constexpr int WorkerCount = 4;
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVector<QString> sourcePaths;
    QVector<QString> cachePaths;
    for (int index = 0; index < WorkerCount; ++index) {
        const QString sourcePath =
            directory.filePath(
                QStringLiteral(
                    "source-%1.provenance").arg(index));
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
    std::atomic<int> provenanceFailures {0};
    std::atomic<bool> cancel {false};
    std::mutex readerGateMutex;
    std::condition_variable readerGateChanged;
    int readersReady = 0;
    bool releaseReaders = false;
    bool readerGateTimedOut = false;
    std::mutex writerGateMutex;
    std::condition_variable writerGateChanged;
    int writersReady = 0;
    bool releaseWriters = false;
    bool writerGateTimedOut = false;
    std::vector<std::thread> workers;
    workers.reserve(WorkerCount);
    ThreadJoiner joinWorkers(workers);
    for (int index = 0; index < WorkerCount; ++index) {
        workers.emplace_back([&, index]() {
            QFile source(sourcePaths.at(index));
            QStringList errors;
            std::unique_ptr<RideFile> ride(
                RideFileFactory::instance().openRideFile(
                    nullptr, source, errors));
            const bool provenanceAvailable =
                ride
                && ride->sourceProvenanceMatchesForTest(
                    sourcePaths.at(index));
            if (!provenanceAvailable) {
                provenanceFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }
            {
                std::unique_lock<std::mutex> lock(
                    readerGateMutex);
                ++readersReady;
                readerGateChanged.notify_all();
                readerGateChanged.wait(
                    lock,
                    [&]() { return releaseReaders; });
            }
            if (cancel.load(std::memory_order_acquire)
                || !provenanceAvailable) {
                return;
            }
            RideFileCache cache(
                ride.get(),
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
    {
        std::unique_lock<std::mutex> lock(
            readerGateMutex);
        if (!readerGateChanged.wait_for(
                lock,
                std::chrono::seconds(5),
                [&]() {
                    return readersReady == WorkerCount;
                })) {
            readerGateTimedOut = true;
        }
        cancel.store(
            readerGateTimedOut
                || provenanceFailures.load(
                       std::memory_order_acquire)
                    != 0,
            std::memory_order_release);
        releaseReaders = true;
    }
    readerGateChanged.notify_all();
    for (std::thread &worker : workers) {
        if (worker.joinable())
            worker.join();
    }

    QVERIFY(!readerGateTimedOut);
    QCOMPARE(provenanceFailures.load(), 0);
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
