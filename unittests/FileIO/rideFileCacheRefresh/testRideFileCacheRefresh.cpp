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
#include "RideItemRefreshResult.h"
#include "SessionServices.h"
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
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <io.h>
#include <qt_windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#endif

class RideFileDetachedCopyTestAccess
{
public:
    static void populatePrivateState(RideFile &ride)
    {
        ride.weight_ = 73.5;
        ride.windSpeed_ = 8.5;
        ride.windHeading_ = 225.0;
        ride.minPoint->watts = 100.0;
        ride.maxPoint->watts = 500.0;
        ride.avgPoint->watts = 250.0;
        ride.totalPoint->watts = 750.0;
        ride.totalCount = 3.0;
        ride.totalTemp = 42.0;
    }

    static void verifyPrivateState(
        const RideFile &copy,
        const RideFile &source)
    {
        QCOMPARE(copy.weight_, source.weight_);
        QCOMPARE(copy.windSpeed_, source.windSpeed_);
        QCOMPARE(copy.windHeading_, source.windHeading_);
        QCOMPARE(copy.minPoint->watts, source.minPoint->watts);
        QCOMPARE(copy.maxPoint->watts, source.maxPoint->watts);
        QCOMPARE(copy.avgPoint->watts, source.avgPoint->watts);
        QCOMPARE(copy.totalPoint->watts, source.totalPoint->watts);
        QCOMPARE(copy.totalCount, source.totalCount);
        QCOMPARE(copy.totalTemp, source.totalTemp);
        QVERIFY(copy.minPoint != source.minPoint);
        QVERIFY(copy.maxPoint != source.maxPoint);
        QVERIFY(copy.avgPoint != source.avgPoint);
        QVERIFY(copy.totalPoint != source.totalPoint);
    }
};

namespace {

constexpr int FixedZoneFloatCount =
    10 + 4 + 10 + 4 + 10 + 4 + 4;

QByteArray analysisFingerprint(const QByteArray &label)
{
    return QCryptographicHash::hash(
        label, QCryptographicHash::Sha256);
}

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
        if (contents.startsWith(QByteArrayLiteral("large:"))) {
            bool validCount = false;
            const int pointCount =
                contents.mid(6).toInt(&validCount);
            if (!validCount || pointCount <= 0) {
                errors.append(QStringLiteral("Invalid large test ride"));
                delete ride;
                return nullptr;
            }
            for (int index = 0; index < pointCount; ++index) {
                RideFilePoint point;
                point.secs = index + 1.0;
                point.watts = 200.0 + (index % 100);
                ride->appendPoint(point);
            }
        } else if (!contents.isEmpty()) {
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

class CountingWriteDevice final : public QIODevice
{
public:
    CountingWriteDevice()
    {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }

    qint64 totalBytes() const { return totalBytes_; }
    qint64 largestWrite() const { return largestWrite_; }

protected:
    qint64 readData(char *, qint64) override { return -1; }

    qint64 writeData(const char *, qint64 size) override
    {
        if (size < 0)
            return -1;
        totalBytes_ += size;
        largestWrite_ = std::max(largestWrite_, size);
        return size;
    }

private:
    qint64 totalBytes_ = 0;
    qint64 largestWrite_ = 0;
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
    double weight = 0.0,
    const QByteArray &analysis =
        analysisFingerprint(QByteArrayLiteral("analysis-v1")))
{
    QVERIFY(QDir().mkpath(
        QFileInfo(path).absolutePath()));
    RideFileCacheHeader header {};
    header.version = RideFileCacheVersion;
    header.crc = qChecksum(
        QByteArrayView(sourceBytes));
    header.WEIGHT = weight;
    QVERIFY(RideFileCacheIntegrity::setAnalysisFingerprint(
        header, analysis));
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

class RecordingPersistenceService final
    : public AthletePersistenceService
{
public:
    void reportCacheWriteFailure(
        const QString &cachePath,
        const QString &detail) override
    {
        ++reportCount;
        reportedPath = cachePath;
        reportedDetail = detail;
    }

    int reportCount = 0;
    QString reportedPath;
    QString reportedDetail;
};

} // namespace

class TestRideFileCacheRefresh : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void immutableStaleInputsFailClosedAndAcceptCurrentCache();
    void refreshRestoresFixedZoneStorage();
    void detachedRideCopyIsCompleteAndIndependent();
    void nonPersistentRefreshHasExplicitOutcome();
    void artifactPreparationFailurePreservesComputedOutcome();
    void persistentRefreshOutcomeMatrix();
    void detachedRefreshIdentityRejectsEveryChangedDimension();
    void repeatedRefreshClearsZoneValues();
    void temporaryActivityComputesWithoutPersistentCache();
    void standalonePowerActivityComputesWithoutContext();
    void standaloneWPrimeWithoutZonesStaysEmpty();
    void plannedAndCompletedActivitiesUseSeparateCaches();
    void rankUsesDescendingInsertionSemantics_data();
    void rankUsesDescendingInsertionSemantics();
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
    void aggregateBindingsRejectMixedSourceGeneration();
    void changedAnalysisInputsRejectDependentFastPaths();
    void aggregateBindingsRejectChangedAnalysisInputs();
    void factoryCapturesSourceProvenance();
    void factoryLeavesUnauditedReaderUnprovenanced();
    void factoryLeavesPathDependentReaderUnprovenanced();
    void factoryRejectsParserMutatedStage();
    void unprovenancedRideSkipsPersistence();
    void savedRideRebindsAndPersistsAtomically();
    void matchingSourceProvenanceAllowsPersistenceAttempt();
    void injectedPersistenceServiceReceivesWriteFailure();
    void omittedPersistenceServiceFallsBackToContext();
    void sourceChangeInvalidatesPersistence();
    void rideMutationInvalidatesPersistence();
    void directPointMutationSkipsPersistence();
    void sourceChangeBeforeCommitSkipsPersistence();
    void verifiedRefreshStreamsPayloadInBoundedWrites();
    void preparedCommitOutlivesCacheAndPublishesExplicitly();
    void preparedArtifactIgnoresLiveCacheMutation();
    void preparedArtifactPinsBoundsAndCleansUp();
    void concurrentPersistenceFailuresKeepComputedResults();
};

void TestRideFileCacheRefresh::cleanup()
{
    RideFileCache::setContextPersistenceFallbackHookForTest({});
}

void TestRideFileCacheRefresh::
immutableStaleInputsFailClosedAndAcceptCurrentCache()
{
    RideFileCacheStaleInputs inputs;
    QVERIFY(RideFileCache::checkStale(inputs));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("activity.fit"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/activity.cpx"));
    const QByteArray sourceBytes = QByteArrayLiteral("source-generation");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
    source.close();

    constexpr double Weight = 72.5;
    const QByteArray analysis = analysisFingerprint(
        QByteArrayLiteral("immutable-analysis"));
    writeCacheFixture(
        cachePath, 100.0f, 30.0f,
        sourceBytes, Weight, analysis);
    makeSourceOlderThanCache(sourcePath, cachePath);

    inputs.storagePathsComplete = true;
    inputs.sourcePath = sourcePath;
    inputs.cachePath = cachePath;
    inputs.weight = Weight;
    inputs.analysisFingerprint = analysis;
    QVERIFY(!RideFileCache::checkStale(inputs));

    RideFileCacheStaleInputs invalid = inputs;
    invalid.storagePathsComplete = false;
    QVERIFY(RideFileCache::checkStale(invalid));
    invalid = inputs;
    invalid.sourcePath.clear();
    QVERIFY(RideFileCache::checkStale(invalid));
    invalid = inputs;
    invalid.cachePath.clear();
    QVERIFY(RideFileCache::checkStale(invalid));
    invalid = inputs;
    invalid.weight = 0.0;
    QVERIFY(RideFileCache::checkStale(invalid));
    invalid = inputs;
    invalid.weight = std::numeric_limits<double>::infinity();
    QVERIFY(RideFileCache::checkStale(invalid));
    invalid = inputs;
    invalid.analysisFingerprint.chop(1);
    QVERIFY(RideFileCache::checkStale(invalid));
}

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

void TestRideFileCacheRefresh::detachedRideCopyIsCompleteAndIndependent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.fit"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-generation"));

    RideFile ride;
    ride.setId(QStringLiteral("ride-id"));
    const QDateTime startTime(
        QDate(2026, 9, 19), QTime(8, 15), QTimeZone::UTC);
    ride.setStartTime(startTime);
    ride.setRecIntSecs(1.0);
    ride.setFileFormat(QStringLiteral("FIT"));
    ride.setTag(QStringLiteral("Sport"), QStringLiteral("Bike"));
    ride.setDataPresent(RideFile::watts, true);
    ride.metricOverrides[QStringLiteral("workout_time")].insert(
        QStringLiteral("value"), QStringLiteral("42"));
    RideFilePoint point;
    point.secs = 1.0;
    point.watts = 250.0;
    ride.appendPoint(point);
    RideFilePoint reference;
    reference.secs = 0.5;
    reference.watts = 175.0;
    ride.appendReference(reference);
    ride.newInterval(
        QStringLiteral("Lap"), 0.0, 1.0, Qt::red, true);
    ride.addCalibration(0.25, 123, QStringLiteral("Zero offset"));
    CIQinfo ciq(QStringLiteral("app-id"), 42, 7);
    ride.addCIQ(ciq);
    auto series = new XDataSeries;
    series->name = QStringLiteral("EXTRA");
    series->valuename = {QStringLiteral("value")};
    series->datapoints.append(new XDataPoint);
    ride.addXData(QStringLiteral("EXTRA"), series);
    RideFileDetachedCopyTestAccess::populatePrivateState(ride);
    QVERIFY(ride.bindSourceProvenanceForTest(sourcePath));

    std::unique_ptr<RideFile> copy = ride.detachedCopy();
    QVERIFY(copy);
    QVERIFY(copy.get() != &ride);
    QCOMPARE(copy->id(), ride.id());
    QCOMPARE(copy->startTime(), startTime);
    QCOMPARE(copy->recIntSecs(), 1.0);
    QCOMPARE(copy->fileFormat(), QStringLiteral("FIT"));
    QCOMPARE(copy->tags(), ride.tags());
    QCOMPARE(copy->metricOverrides, ride.metricOverrides);
    QVERIFY(copy->isDataPresent(RideFile::watts));
    QCOMPARE(copy->ciqinfo().size(), 1);
    QCOMPARE(copy->ciqinfo().constFirst().appid, QStringLiteral("app-id"));
    QCOMPARE(copy->ciqinfo().constFirst().devid, 42);
    QCOMPARE(copy->ciqinfo().constFirst().ver, 7);
    QCOMPARE(copy->dataPoints().size(), 1);
    QVERIFY(copy->dataPoints().constFirst()
            != ride.dataPoints().constFirst());
    QCOMPARE(copy->dataPoints().constFirst()->watts, 250.0);
    QCOMPARE(copy->referencePoints().size(), 1);
    QVERIFY(copy->referencePoints().constFirst()
            != ride.referencePoints().constFirst());
    QCOMPARE(copy->referencePoints().constFirst()->watts, 175.0);
    QCOMPARE(copy->intervals().size(), 1);
    QVERIFY(copy->intervals().constFirst()
            != ride.intervals().constFirst());
    QCOMPARE(copy->calibrations().size(), 1);
    QVERIFY(copy->calibrations().constFirst()
            != ride.calibrations().constFirst());
    QCOMPARE(copy->calibrations().constFirst()->value, 123);
    QVERIFY(copy->xdata(QStringLiteral("EXTRA"))
            != ride.xdata(QStringLiteral("EXTRA")));
    QVERIFY(copy->sourceProvenanceMatchesForTest(sourcePath));
    RideFileDetachedCopyTestAccess::verifyPrivateState(*copy, ride);

    copy->setTag(QStringLiteral("Sport"), QStringLiteral("Run"));
    copy->setPointValue(0, RideFile::watts, 999.0);
    copy->referencePoints().constFirst()->watts = 888.0;
    copy->intervals().constFirst()->name = QStringLiteral("Changed");
    copy->calibrations().constFirst()->value = 999;
    copy->xdata(QStringLiteral("EXTRA"))->datapoints.constFirst()->number[0]
        = 7.0;
    QCOMPARE(ride.getTag(QStringLiteral("Sport"), {}),
             QStringLiteral("Bike"));
    QCOMPARE(ride.dataPoints().constFirst()->watts, 250.0);
    QCOMPARE(ride.referencePoints().constFirst()->watts, 175.0);
    QCOMPARE(ride.intervals().constFirst()->name, QStringLiteral("Lap"));
    QCOMPARE(ride.calibrations().constFirst()->value, 123);
    QCOMPARE(ride.xdata(QStringLiteral("EXTRA"))
                 ->datapoints.constFirst()->number[0],
             0.0);
}

void TestRideFileCacheRefresh::nonPersistentRefreshHasExplicitOutcome()
{
    RideFile ride;
    ride.setRecIntSecs(1.0);
    RideFilePoint point;
    point.secs = 1.0;
    point.watts = 200.0;
    ride.appendPoint(point);

    RideFileCache cache(
        &ride, RideFileCache::NoPersistentTargetForTest {});
    RideFileCache::PreparedRefresh prepared =
        cache.preparePersistentRefresh(&ride, false);

    QCOMPARE(
        prepared.outcome,
        RideFileCache::PreparedRefresh::Outcome::
            ValidWithoutPersistence);
    QVERIFY(!prepared.commit);
    QVERIFY(prepared.failurePath.isEmpty());
    QVERIFY(prepared.failureDetail.isEmpty());
    QVERIFY(!cache.incomplete);
}

void TestRideFileCacheRefresh::
artifactPreparationFailurePreservesComputedOutcome()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("source.provenance"));
    const QString cachePath = directory.filePath(
        QStringLiteral("cache/source.cpx"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));

    RideFileCache cache(
        ride.get(), RideFileCache::SkipInitialComputeForTest {});
    RideFileCache::PreparedRefresh prepared =
        cache.preparePersistentRefreshForTest(
            sourcePath, cachePath, ride.get(), true);

    QCOMPARE(
        prepared.outcome,
        RideFileCache::PreparedRefresh::Outcome::
            PersistencePreparationFailed);
    QVERIFY(!prepared.commit);
    QCOMPARE(prepared.failurePath, cachePath);
    QVERIFY(!prepared.failureDetail.isEmpty());
    QVERIFY(!cache.incomplete);
    QVERIFY(!QFileInfo::exists(cachePath));
}

void TestRideFileCacheRefresh::persistentRefreshOutcomeMatrix()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("source.provenance"));
    const QString preparedPath = directory.filePath(
        QStringLiteral("cache/prepared.cpx"));
    const QString currentPath = directory.filePath(
        QStringLiteral("cache/current.cpx"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));

    RideFileCache preparedCache(
        ride.get(), RideFileCache::SkipInitialComputeForTest {});
    RideFileCache::PreparedRefresh prepared =
        preparedCache.preparePersistentRefreshForTest(
            sourcePath, preparedPath, ride.get(), false);
    QCOMPARE(
        prepared.outcome,
        RideFileCache::PreparedRefresh::Outcome::Prepared);
    QVERIFY(prepared.commit);
    QVERIFY(!QFileInfo::exists(preparedPath));

    RideFileCache currentCache(
        ride.get(), RideFileCache::SkipInitialComputeForTest {});
    RideFileCache::PreparedRefresh current =
        currentCache.preparePersistentRefreshForTest(
            sourcePath, currentPath, ride.get(), false, true);
    QCOMPARE(
        current.outcome,
        RideFileCache::PreparedRefresh::Outcome::Current);
    QVERIFY(!current.commit);
    QVERIFY(!QFileInfo::exists(currentPath));

    RideFile unprovenanced;
    unprovenanced.setRecIntSecs(1.0);
    RideFilePoint point;
    point.secs = 1.0;
    point.watts = 200.0;
    unprovenanced.appendPoint(point);
    RideFileCache invalidCache(
        &unprovenanced,
        RideFileCache::SkipInitialComputeForTest {});
    RideFileCache::PreparedRefresh invalid =
        invalidCache.preparePersistentRefreshForTest(
            sourcePath,
            directory.filePath(QStringLiteral("cache/invalid.cpx")),
            &unprovenanced,
            false);
    QCOMPARE(
        invalid.outcome,
        RideFileCache::PreparedRefresh::Outcome::Invalid);
    QVERIFY(!invalid.commit);
}

void TestRideFileCacheRefresh::
detachedRefreshIdentityRejectsEveryChangedDimension()
{
    RideFile openRide;
    RideFile otherRide;
    const QDateTime dateTime(
        QDate(2026, 9, 19), QTime(12, 30), QTimeZone::UTC);
    RideItemRefreshGate result;
    result.expected = {
        QStringLiteral("/activities"),
        QStringLiteral("activity.fit"),
        dateTime,
        true,
        &openRide};
    result.sourcePath = QStringLiteral("/activities/activity.fit");
    result.sourceFingerprint.byteSize = 8;
    result.sourceFingerprint.sha256 = QByteArray(32, 'a');
    result.sourceFingerprint.legacyCrc16 = 17;

    RideItemRefreshIdentity current = result.expected;
    auto fingerprint = result.sourceFingerprint;
    const auto accepts = [&]() {
        return result.accepts(
            current, QStringLiteral("/activities/activity.fit"),
            fingerprint);
    };
    QVERIFY(accepts());

    current.path = QStringLiteral("/planned");
    QVERIFY(!accepts());
    current = result.expected;
    current.fileName = QStringLiteral("other.fit");
    QVERIFY(!accepts());
    current = result.expected;
    current.dateTime = dateTime.addSecs(1);
    QVERIFY(!accepts());
    current = result.expected;
    current.open = false;
    QVERIFY(!accepts());
    current = result.expected;
    current.openRide = &otherRide;
    QVERIFY(!accepts());
    current = result.expected;
    fingerprint.sha256[0] = 'b';
    QVERIFY(!accepts());
    fingerprint = result.sourceFingerprint;
    ++fingerprint.byteSize;
    QVERIFY(!accepts());
    fingerprint = result.sourceFingerprint;
    ++fingerprint.legacyCrc16;
    QVERIFY(!accepts());
    fingerprint = result.sourceFingerprint;
    QVERIFY(!result.accepts(
        current, QStringLiteral("/planned/activity.fit"), fingerprint));
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
TestRideFileCacheRefresh::rankUsesDescendingInsertionSemantics_data()
{
    QTest::addColumn<double>("candidate");
    QTest::addColumn<int>("expectedRank");

    QTest::newRow("above-top") << 350.0 << 1;
    QTest::newRow("tie-at-top") << 300.0 << 1;
    QTest::newRow("between-top-and-middle") << 250.0 << 2;
    QTest::newRow("tie-in-middle") << 200.0 << 2;
    QTest::newRow("between-middle-and-bottom") << 150.0 << 4;
    QTest::newRow("below-bottom") << 50.0 << 5;
}

void
TestRideFileCacheRefresh::rankUsesDescendingInsertionSemantics()
{
    QFETCH(double, candidate);
    QFETCH(int, expectedRank);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVector<QPair<QString, QString>> cacheRows;
    const auto addRow = [&](const QString &name,
                            float best,
                            bool valid) {
        const QString sourcePath =
            directory.filePath(name + QStringLiteral(".fit"));
        const QString cachePath =
            directory.filePath(
                QStringLiteral("cache/")
                + name
                + QStringLiteral(".cpx"));
        const QByteArray sourceBytes =
            name.toUtf8() + QByteArrayLiteral("-source");
        writeFileBytes(sourcePath, sourceBytes);
        writeCacheFixture(
            cachePath,
            best,
            0.0f,
            valid
                ? sourceBytes
                : QByteArrayLiteral("stale-source"));
        cacheRows.append({sourcePath, cachePath});
    };

    addRow(QStringLiteral("top"), 300.0f, true);
    addRow(QStringLiteral("middle-a"), 200.0f, true);
    addRow(QStringLiteral("middle-b"), 200.0f, true);
    addRow(QStringLiteral("bottom"), 100.0f, true);
    addRow(QStringLiteral("rejected"), 1000.0f, false);

    int of = -1;
    const int rank =
        RideFileCache::rankCacheRowsForTest(
            cacheRows,
            RideFile::watts,
            1,
            candidate,
            of);

    // "of" counts accepted rows. Rank is their one-based descending
    // insertion position, so a new last place can be "of + 1".
    QCOMPARE(of, 4);
    QCOMPARE(rank, expectedRank);
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
    const QByteArray analysis = analysisFingerprint(
        QByteArrayLiteral("analysis-v1"));
    const QVector<QPair<QByteArray, QByteArray>>
        analysisBindings = {
            {analysis, analysis}
        };
    QVERIFY(
        RideFileCache::
            aggregateBindingsAreCurrentForTest(
                bindings, analysisBindings));

    writeFileBytes(
        sourcePath,
        QByteArrayLiteral("source-b"));
    QVERIFY(
        !RideFileCache::
            aggregateBindingsAreCurrentForTest(
                bindings, analysisBindings));
}

void
TestRideFileCacheRefresh::
aggregateBindingsRejectMixedSourceGeneration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVector<
        QPair<
            QString,
            RideFileCRC::ContentFingerprint>>
        sourceBindings;
    const QByteArray analysis = analysisFingerprint(
        QByteArrayLiteral("analysis-v1"));
    QVector<QPair<QByteArray, QByteArray>>
        analysisBindings;
    for (int index = 0; index < 2; ++index) {
        const QString sourcePath = directory.filePath(
            QStringLiteral("source-%1.fit").arg(index));
        writeFileBytes(
            sourcePath,
            QByteArrayLiteral("original-")
                + QByteArray::number(index));
        RideFileCRC::ContentFingerprint fingerprint;
        QVERIFY(RideFileCRC::computeFileFingerprint(
            sourcePath, fingerprint));
        sourceBindings.append({sourcePath, fingerprint});
        analysisBindings.append({analysis, analysis});
    }

    RideFileCache::setAggregateBindingReadHookForTest(
        [sourcePath = sourceBindings.constFirst().first]() {
            writeFileBytes(
                sourcePath,
                QByteArrayLiteral("changed-0"));
        });
    QVERIFY(!RideFileCache::aggregateBindingsAreCurrentForTest(
        sourceBindings, analysisBindings));
}

void
TestRideFileCacheRefresh::
changedAnalysisInputsRejectDependentFastPaths()
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
    const QString sourcePath = QDir(completedRoot).filePath(
        QStringLiteral("2026_07_29_12_00_00.fit"));
    const QString cachePath = QDir(cacheRoot).filePath(
        QStringLiteral("2026_07_29_12_00_00.cpx"));
    const QByteArray sourceBytes = QByteArrayLiteral("source-a");
    const QByteArray current = analysisFingerprint(
        QByteArrayLiteral("weight=75;zones=1;settings=1"));
    const QVector<QByteArray> stale = {
        analysisFingerprint(
            QByteArrayLiteral("weight=76;zones=1;settings=1")),
        analysisFingerprint(
            QByteArrayLiteral("weight=75;zones=2;settings=1")),
        analysisFingerprint(
            QByteArrayLiteral("weight=75;zones=1;settings=2"))
    };
    constexpr double Weight = 75.0;
    writeFileBytes(sourcePath, sourceBytes);
    writeCacheFixture(
        cachePath,
        210.0f,
        11.0f,
        sourceBytes,
        Weight,
        current);
    const QVector<QPair<RideFile::SeriesType, int>> requests = {
        {RideFile::watts, 1}
    };

    QVERIFY(RideFileCache::cacheIsCurrentForSourceWithAnalysisForTest(
        sourcePath, cachePath, Weight, current));
    QCOMPARE(
        RideFileCache::bestForActivityWithAnalysisForTest(
            cacheRoot, completedRoot, plannedRoot,
            sourcePath, RideFile::watts, 1, current),
        210.0);
    QCOMPARE(
        RideFileCache::tizForActivityWithAnalysisForTest(
            cacheRoot, completedRoot, plannedRoot,
            sourcePath, RideFile::watts, 1, current),
        11);
    QVector<double> values;
    QVERIFY(RideFileCache::readBestRowForSourceWithAnalysisForTest(
        sourcePath, cachePath, current, requests, values));
    QCOMPARE(values, QVector<double>({210.0}));

    for (const QByteArray &changed : stale) {
        QVERIFY(!RideFileCache::cacheIsCurrentForSourceWithAnalysisForTest(
            sourcePath, cachePath, Weight, changed));
        QCOMPARE(
            RideFileCache::bestForActivityWithAnalysisForTest(
                cacheRoot, completedRoot, plannedRoot,
                sourcePath, RideFile::watts, 1, changed),
            0.0);
        QCOMPARE(
            RideFileCache::tizForActivityWithAnalysisForTest(
                cacheRoot, completedRoot, plannedRoot,
                sourcePath, RideFile::watts, 1, changed),
            0);
        values = {999.0};
        QVERIFY(!RideFileCache::readBestRowForSourceWithAnalysisForTest(
            sourcePath, cachePath, changed, requests, values));
        QVERIFY(values.isEmpty());
    }
}

void
TestRideFileCacheRefresh::
aggregateBindingsRejectChangedAnalysisInputs()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray current = analysisFingerprint(
        QByteArrayLiteral("analysis-v1"));
    const QByteArray changed = analysisFingerprint(
        QByteArrayLiteral("analysis-v2"));
    QVector<
        QPair<
            QString,
            RideFileCRC::ContentFingerprint>>
        sourceBindings;
    for (int index = 0; index < 2; ++index) {
        const QString sourcePath = directory.filePath(
            QStringLiteral("source-%1.fit").arg(index));
        writeFileBytes(
            sourcePath,
            QByteArrayLiteral("source-")
                + QByteArray::number(index));
        RideFileCRC::ContentFingerprint fingerprint;
        QVERIFY(RideFileCRC::computeFileFingerprint(
            sourcePath, fingerprint));
        sourceBindings.append({sourcePath, fingerprint});
    }

    QVERIFY(RideFileCache::aggregateBindingsAreCurrentForTest(
        sourceBindings, {
        {current, current},
        {current, current}
    }));
    QVERIFY(!RideFileCache::aggregateBindingsAreCurrentForTest(
        sourceBindings, {
        {current, current},
        {current, changed}
    }));
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
    QCOMPARE(
        data.analysisFingerprint.size(),
        RideFileCRC::Sha256Size);
    QVERIFY(data.analysisFingerprint
        != QByteArray(RideFileCRC::Sha256Size, '\0'));
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
TestRideFileCacheRefresh::
injectedPersistenceServiceReceivesWriteFailure()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.provenance"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/source.cpx"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    RecordingPersistenceService persistence;
    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {},
        &persistence);

    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        cachePath,
        [](const QString &,
           const RideFileCacheIntegrity::CacheWriteOperation &,
           QString *error) {
            if (error)
                *error = QStringLiteral("injected write failure");
            return false;
        },
        {}));

    QCOMPARE(persistence.reportCount, 1);
    QCOMPARE(persistence.reportedPath, cachePath);
    QCOMPARE(
        persistence.reportedDetail,
        QStringLiteral("injected write failure"));
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::
omittedPersistenceServiceFallsBackToContext()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        directory.filePath(QStringLiteral("source.provenance"));
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/source.cpx"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));

    Context *const expectedContext =
        reinterpret_cast<Context *>(quintptr(0x1234));
    Context *reportedContext = nullptr;
    QString reportedPath;
    QString reportedDetail;
    int reportCount = 0;
    RideFileCache::setContextPersistenceFallbackHookForTest(
        [&](Context *context,
            const QString &path,
            const QString &detail) {
            ++reportCount;
            reportedContext = context;
            reportedPath = path;
            reportedDetail = detail;
        });

    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    QVERIFY(!cache.refreshCacheForTest(
        sourcePath,
        cachePath,
        [&cache, expectedContext](
            const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &,
            QString *error) {
            cache.setContextForTest(expectedContext);
            if (error)
                *error = QStringLiteral("injected write failure");
            return false;
        },
        {}));

    QCOMPARE(reportCount, 1);
    QCOMPARE(reportedContext, expectedContext);
    QCOMPARE(reportedPath, cachePath);
    QCOMPARE(
        reportedDetail,
        QStringLiteral("injected write failure"));
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
TestRideFileCacheRefresh::verifiedRefreshStreamsPayloadInBoundedWrites()
{
    constexpr qint64 WriteChunkLimit =
        RideFileCacheIntegrity::CacheWriteChunkBytes;
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("large.provenance"));
    writeFileBytes(
        sourcePath,
        QByteArrayLiteral("large:4096"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(ride->dataPoints().size(), 4096);

    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    qint64 totalBytes = 0;
    qint64 largestWrite = 0;
    QVERIFY(cache.refreshCacheWithValidatorForTest(
        sourcePath,
        directory.filePath(QStringLiteral("cache/large.cpx")),
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &write,
            const RideFileCacheIntegrity::CachePreCommitValidator &validate,
            QString *error) {
            CountingWriteDevice output;
            if (!write(output, error))
                return false;
            totalBytes = output.totalBytes();
            largestWrite = output.largestWrite();
            return validate(error);
        },
        {}));

    QVERIFY(totalBytes > WriteChunkLimit);
    QVERIFY2(
        largestWrite <= WriteChunkLimit,
        qPrintable(QStringLiteral(
            "largest CPX write was %1 bytes").arg(largestWrite)));
}

void
TestRideFileCacheRefresh::
preparedCommitOutlivesCacheAndPublishesExplicitly()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("source.provenance"));
    const QString firstCachePath = directory.filePath(
        QStringLiteral("cache/first.cpx"));
    const QString rejectedCachePath = directory.filePath(
        QStringLiteral("cache/rejected.cpx"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));

    std::unique_ptr<RideFileCache::PreparedCacheCommit> prepared;
    {
        RideFileCache cache(
            ride.get(),
            RideFileCache::SkipInitialComputeForTest {});
        prepared = cache.preparePersistentCommitForTest(
            sourcePath, firstCachePath);
        QVERIFY(prepared);
        QVERIFY(!QFileInfo::exists(firstCachePath));
    }

    QByteArray publishedBytes;
    int publishCalls = 0;
    bool publishPathMatched = false;
    QCOMPARE(
        RideFileCache::publishPreparedCommitForTest(
            std::move(prepared),
            nullptr,
            nullptr,
            [&](const QString &path,
                const RideFileCacheIntegrity::CacheWriteOperation &write,
                const RideFileCacheIntegrity::CachePreCommitValidator &validate,
                QString *error) {
                ++publishCalls;
                publishPathMatched = path == firstCachePath;
                QBuffer output(&publishedBytes);
                if (!output.open(QIODevice::WriteOnly)
                    || !write(output, error)) {
                    return false;
                }
                return validate(error);
            }),
        RideFileCache::PreparedCommitOutcome::Persisted);
    QCOMPARE(publishCalls, 1);
    QVERIFY(publishPathMatched);
    QVERIFY(!publishedBytes.isEmpty());
    QVERIFY(!QFileInfo::exists(firstCachePath));

    std::unique_ptr<RideFileCache::PreparedCacheCommit> rejected;
    {
        RideFileCache cache(
            ride.get(),
            RideFileCache::SkipInitialComputeForTest {});
        rejected = cache.preparePersistentCommitForTest(
            sourcePath, rejectedCachePath);
        QVERIFY(rejected);
        QVERIFY(!QFileInfo::exists(rejectedCachePath));
    }

    writeFileBytes(sourcePath, QByteArrayLiteral("source-b"));
    QCOMPARE(
        RideFileCache::publishPreparedCommitForTest(
            std::move(rejected),
            nullptr,
            nullptr,
            [](const QString &,
               const RideFileCacheIntegrity::CacheWriteOperation &write,
               const RideFileCacheIntegrity::CachePreCommitValidator &validate,
               QString *error) {
                QBuffer output;
                if (!output.open(QIODevice::WriteOnly)
                    || !write(output, error)) {
                    return false;
                }
                return validate(error);
            }),
        RideFileCache::PreparedCommitOutcome::SourceRejected);
    QVERIFY(!QFileInfo::exists(rejectedCachePath));
}

void
TestRideFileCacheRefresh::
preparedArtifactIgnoresLiveCacheMutation()
{
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("source.provenance"));
    writeFileBytes(sourcePath, QByteArrayLiteral("source-a"));

    QFile source(sourcePath);
    QStringList errors;
    std::unique_ptr<RideFile> ride(
        RideFileFactory::instance().openRideFile(
            nullptr, source, errors));
    QVERIFY2(ride, qPrintable(errors.join(QLatin1Char('\n'))));
    RideFileCache cache(
        ride.get(),
        RideFileCache::SkipInitialComputeForTest {});
    const QString cachePath =
        directory.filePath(QStringLiteral("cache/source.cpx"));
    int writeCalls = 0;
    int validationCalls = 0;
    float preparedZoneValue = 0.0f;
    float mutatedZoneValue = 0.0f;
    qintptr artifactDescriptor = -1;
#ifdef Q_OS_WIN
    quintptr nativeArtifactHandle = 0;
#endif
    bool artifactIsNonInheritable = false;
    QString artifactPath;
    QString artifactDirectoryPath;
    QVERIFY(cache.refreshCacheWithValidatorForTest(
        sourcePath,
        cachePath,
        [&](const QString &,
            const RideFileCacheIntegrity::CacheWriteOperation &write,
            const RideFileCacheIntegrity::CachePreCommitValidator &validate,
            QString *error) {
            ++writeCalls;
            if (QFileInfo::exists(cachePath))
                return false;
            const float beforeMutation =
                cache.wattsZoneArray().at(0);
            cache.wattsZoneArray()[0] += 1.0f;
            mutatedZoneValue = cache.wattsZoneArray().at(0);
            QByteArray bytes;
            QBuffer output(&bytes);
            if (!output.open(QIODevice::WriteOnly)
                || !write(output, error)) {
                return false;
            }
            QBuffer input(&bytes);
            if (!input.open(QIODevice::ReadOnly))
                return false;
            RideFileCacheIntegrity::CacheData data;
            if (!RideFileCacheIntegrity::readCache(
                    input, data, error)) {
                return false;
            }
            preparedZoneValue = data.zones.at(
                RideFileCacheIntegrity::WattsTimeInZone).at(0);
            if (preparedZoneValue != beforeMutation)
                return false;
            ++validationCalls;
            return validate(error);
        },
        {},
        [&](qintptr descriptor,
            const QString &path,
            const QString &directoryPath,
            qint64) {
            artifactDescriptor = descriptor;
#ifdef Q_OS_WIN
            const intptr_t native = ::_get_osfhandle(
                static_cast<int>(descriptor));
            if (native != -1) {
                nativeArtifactHandle = static_cast<quintptr>(native);
                DWORD flags = 0;
                artifactIsNonInheritable =
                    GetHandleInformation(
                        reinterpret_cast<HANDLE>(
                            nativeArtifactHandle),
                        &flags)
                    && !(flags & HANDLE_FLAG_INHERIT);
            }
#else
            const int flags = ::fcntl(
                static_cast<int>(descriptor), F_GETFD);
            artifactIsNonInheritable =
                flags >= 0 && (flags & FD_CLOEXEC);
#endif
            artifactPath = path;
            artifactDirectoryPath = directoryPath;
        }));
    QCOMPARE(writeCalls, 1);
    QCOMPARE(validationCalls, 1);
    QCOMPARE(mutatedZoneValue, preparedZoneValue + 1.0f);
    QVERIFY(artifactIsNonInheritable);
    QVERIFY(!QFileInfo::exists(cachePath));
    if (!artifactPath.isEmpty())
        QVERIFY(!QFileInfo::exists(artifactPath));
    if (!artifactDirectoryPath.isEmpty())
        QVERIFY(!QFileInfo::exists(artifactDirectoryPath));
#ifdef Q_OS_WIN
    DWORD closedHandleFlags = 0;
    SetLastError(ERROR_SUCCESS);
    QVERIFY(!GetHandleInformation(
        reinterpret_cast<HANDLE>(nativeArtifactHandle),
        &closedHandleFlags));
    QCOMPARE(GetLastError(), DWORD(ERROR_INVALID_HANDLE));
#else
    errno = 0;
    QCOMPARE(::fcntl(
                 static_cast<int>(artifactDescriptor),
                 F_GETFD),
             -1);
    QCOMPARE(errno, EBADF);
    QVERIFY(artifactPath.isEmpty());
#endif
    QVERIFY(!cache.incomplete);
}

void
TestRideFileCacheRefresh::
preparedArtifactPinsBoundsAndCleansUp()
{
    enum Mutation { Corrupt, Append };
    registerProvenanceTestReader();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(
        QStringLiteral("source.provenance"));
    writeFileBytes(sourcePath, QByteArrayLiteral("large:4096"));

    for (const Mutation mutation : {Corrupt, Append}) {
        QFile source(sourcePath);
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
        bool mutationSucceeded = false;
        qintptr artifactDescriptor = -1;
#ifdef Q_OS_WIN
        quintptr nativeArtifactHandle = 0;
#endif
        bool artifactIsNonInheritable = false;
        QString artifactPath;
        QString artifactDirectoryPath;
        qint64 expectedBytes = 0;
        qint64 copiedBytes = 0;
        qint64 largestWrite = 0;
        const QString cachePath = directory.filePath(
            mutation == Corrupt
                ? QStringLiteral("cache/corrupt.cpx")
                : QStringLiteral("cache/append.cpx"));

        QVERIFY(!cache.refreshCacheWithValidatorForTest(
            sourcePath,
            cachePath,
            [&](const QString &,
                const RideFileCacheIntegrity::CacheWriteOperation &write,
                const RideFileCacheIntegrity::CachePreCommitValidator &,
                QString *error) {
                ++writeCalls;
                CountingWriteDevice output;
                const bool result = write(output, error);
                copiedBytes = output.totalBytes();
                largestWrite = output.largestWrite();
                return result;
            },
            [&](const QString &, const QString &) {
                ++reportCalls;
            },
            [&](qintptr descriptor,
                const QString &path,
                const QString &directoryPath,
                qint64 byteCount) {
                artifactDescriptor = descriptor;
#ifdef Q_OS_WIN
                const intptr_t native = ::_get_osfhandle(
                    static_cast<int>(descriptor));
                if (native != -1) {
                    nativeArtifactHandle =
                        static_cast<quintptr>(native);
                    DWORD flags = 0;
                    artifactIsNonInheritable =
                        GetHandleInformation(
                            reinterpret_cast<HANDLE>(
                                nativeArtifactHandle),
                            &flags)
                        && !(flags & HANDLE_FLAG_INHERIT);
                }
#else
                const int flags = ::fcntl(
                    static_cast<int>(descriptor), F_GETFD);
                artifactIsNonInheritable =
                    flags >= 0 && (flags & FD_CLOEXEC);
#endif
                artifactPath = path;
                artifactDirectoryPath = directoryPath;
                expectedBytes = byteCount;
                QFile pinned;
                if (!pinned.open(
                        descriptor,
                        QIODevice::ReadWrite,
                        QFileDevice::DontCloseHandle)) {
                    return;
                }
                if (mutation == Corrupt) {
                    if (!pinned.seek(byteCount / 2))
                        return;
                    QByteArray byte = pinned.read(1);
                    if (byte.size() != 1)
                        return;
                    byte[0] = static_cast<char>(byte[0] ^ 0x5a);
                    if (!pinned.seek(byteCount / 2)
                        || pinned.write(byte) != 1) {
                        return;
                    }
                } else {
                    const QByteArray appended(
                        2 * RideFileCacheIntegrity::CacheWriteChunkBytes,
                        'x');
                    if (!pinned.seek(byteCount)
                        || pinned.write(appended)
                            != appended.size()) {
                        return;
                    }
                }
                mutationSucceeded = pinned.flush();
            }));

        QVERIFY(mutationSucceeded);
        QVERIFY(artifactIsNonInheritable);
        QCOMPARE(writeCalls, 1);
        QCOMPARE(reportCalls, 1);
        QVERIFY(expectedBytes > 0);
        QCOMPARE(copiedBytes, expectedBytes);
        QVERIFY(largestWrite
                <= RideFileCacheIntegrity::CacheWriteChunkBytes);
        QVERIFY(!QFileInfo::exists(cachePath));
        if (!artifactPath.isEmpty())
            QVERIFY(!QFileInfo::exists(artifactPath));
        if (!artifactDirectoryPath.isEmpty())
            QVERIFY(!QFileInfo::exists(artifactDirectoryPath));
#ifdef Q_OS_WIN
        DWORD closedHandleFlags = 0;
        SetLastError(ERROR_SUCCESS);
        QVERIFY(!GetHandleInformation(
            reinterpret_cast<HANDLE>(nativeArtifactHandle),
            &closedHandleFlags));
        QCOMPARE(GetLastError(), DWORD(ERROR_INVALID_HANDLE));
#else
        errno = 0;
        QCOMPARE(::fcntl(
                     static_cast<int>(artifactDescriptor),
                     F_GETFD),
                 -1);
        QCOMPARE(errno, EBADF);
        QVERIFY(artifactPath.isEmpty());
#endif
    }
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
