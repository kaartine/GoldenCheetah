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
    void standalonePowerActivityComputesWithoutContext();
    void standaloneWPrimeWithoutZonesStaysEmpty();
    void plannedAndCompletedActivitiesUseSeparateCaches();
    void batchReadDiscardsRowAfterMidReadFailure();
    void crcReadFailureSkipsPersistence();
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
