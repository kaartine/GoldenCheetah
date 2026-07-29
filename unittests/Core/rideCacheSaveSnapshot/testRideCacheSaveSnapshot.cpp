#include <QtTest>

#include "RideCacheBackgroundSaver.h"
#include "RideCacheSaveCapture.h"
#include "RideCacheSaveSnapshot.h"
#include "RideCacheStartup.h"
#include "IntervalItem.h"
#include "RideItem.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

RideCacheSave::MetricValues metricValues(
    const QVector<double> &values,
    const QVector<double> &counts)
{
    RideCacheSave::MetricValues result;
    result.values = values;
    result.counts = counts;
    return result;
}

RideCacheSave::Snapshot richSnapshot()
{
    using namespace RideCacheSave;

    Snapshot snapshot;
    snapshot.version = QStringLiteral("2.0");
    snapshot.metrics = {
        {QStringLiteral("average_power"), 0, false},
        {QStringLiteral("zero_metric"), 1, true},
        {QStringLiteral("variability"), 2, false}
    };

    Ride ride;
    ride.dateTime = QDateTime(
        QDate(2026, 7, 28), QTime(18, 42, 5), QTimeZone::UTC);
    ride.fileName = QStringLiteral("2026_07_28_18_42_05.json");
    ride.fingerprint = 101;
    ride.crc = 202;
    ride.metadataCrc = 303;
    ride.timestamp = 404;
    ride.databaseVersion = 7;
    ride.userDatabaseVersion = 8;
    ride.color = QStringLiteral("#123456");
    ride.present = QStringLiteral("P H");
    ride.sport = QStringLiteral("Bike");
    ride.aero = true;
    ride.weight = 79.5;
    ride.zoneRange = 2;
    ride.hrZoneRange = 3;
    ride.paceZoneRange = 4;
    ride.overrides = {
        QStringLiteral("average_power"),
        QStringLiteral("zero_metric")
    };
    ride.samples = true;
    ride.metricValues = metricValues(
        {245.25, 0.0, 1.07},
        {0.0, 2.0, 3600.0});
    ride.metricValues.stdMeans.insert(2, 1.05);
    ride.metricValues.stdVariances.insert(2, 0.02);
    ride.metadata.insert(
        QStringLiteral("Notes"),
        QStringLiteral("line 1\n\"quoted\"/path"));
    ride.xdata.insert(
        QStringLiteral("EXTRA"),
        {QStringLiteral("left"), QStringLiteral("right/value")});

    Interval interval;
    interval.name = QStringLiteral("Threshold \"set\"");
    interval.start = 60.5;
    interval.stop = 360.25;
    interval.startKm = 1.25;
    interval.stopKm = 12.75;
    interval.type = 3;
    interval.test = true;
    interval.color = QStringLiteral("#abcdef");
    interval.route = QStringLiteral(
        "12345678-1234-5678-9abc-def012345678");
    interval.sequence = 9;
    interval.metricValues = metricValues(
        {280.0, 0.0, 1.11},
        {0.0, 2.0, 300.0});
    interval.metricValues.stdMeans.insert(2, 1.08);
    interval.metricValues.stdVariances.insert(2, 0.03);
    ride.intervals.append(interval);

    snapshot.rides.append(ride);
    return snapshot;
}

QJsonDocument parseDocument(const QByteArray &bytes)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning().noquote() << error.errorString() << bytes;
    }
    return document;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size()
        && file.flush();
}

QByteArray serialize(
    const RideCacheSave::Snapshot &snapshot,
    QString *errorOut = nullptr)
{
    QByteArray bytes;
    QString error;
    const bool success =
        RideCacheSave::serialize(snapshot, bytes, error);
    if (errorOut) *errorOut = error;
    if (!success) return {};
    return bytes;
}

} // namespace

class TestRideCacheSaveSnapshot : public QObject
{
    Q_OBJECT

private slots:
    void activeRefreshDefersUniqueTargets();
    void shutdownTargetPolicyPreservesCustomExports();
    void synchronousSaveWaitsForRefreshWorkers();
    void stalledRefreshWorkerTimesOut();
    void synchronousSaveRequiresLatestRefreshGeneration();
    void synchronousSaveSettlesLatestRefreshGeneration();
    void supersededRefreshResultRemainsStale();
    void capturedRideItemsCanBeDestroyedBeforeWorkerSave();
    void captureSkipsIncompleteAndDiscardedRides();
    void backgroundSaverPublishesSnapshotsInFifoOrder();
    void blockingSaveWaitsForQueuedSnapshots();
    void backgroundSaverWaitTimeoutRetainsQueuedFailure();
    void drainReportsQueuedWriteFailure();
    void stopDrainsQueuedSnapshots();
    void completeSnapshotSerializesAsValidJson();
    void legacyMetricFilteringIsPreserved();
    void snapshotDoesNotObserveLaterSourceMutations();
    void concurrentSerializationIsDeterministic();
    void nonFiniteIntervalDoesNotCreateMetricObject();
    void writePublishesSerializedSnapshot();
    void failedSerializationPreservesPreviousFile();
    void missingTargetPathFails();
    void invalidMetricSchemaFailsClosed_data();
    void invalidMetricSchemaFailsClosed();
};

void
TestRideCacheSaveSnapshot::activeRefreshDefersUniqueTargets()
{
    QStringList pending;

    QVERIFY(!RideCacheSave::deferTarget(
        pending, false, QStringLiteral("/cache/first.json")));
    QVERIFY(pending.isEmpty());

    QVERIFY(RideCacheSave::deferTarget(
        pending, true, QStringLiteral("/cache/first.json")));
    QVERIFY(RideCacheSave::deferTarget(
        pending, true, QStringLiteral("/cache/second.json")));
    QVERIFY(RideCacheSave::deferTarget(
        pending, true, QStringLiteral("/cache/first.json")));
    QCOMPARE(
        pending,
        QStringList({
            QStringLiteral("/cache/first.json"),
            QStringLiteral("/cache/second.json")
        }));

    QCOMPARE(
        RideCacheSave::takeDeferredTargets(pending),
        QStringList({
            QStringLiteral("/cache/first.json"),
            QStringLiteral("/cache/second.json")
        }));
    QVERIFY(pending.isEmpty());
}

void
TestRideCacheSaveSnapshot::
shutdownTargetPolicyPreservesCustomExports()
{
    const QString defaultTarget =
        QStringLiteral("/cache/rideDB.json");
    const QString customTarget =
        QStringLiteral("/exports/rideDB-copy.json");
    const QStringList allTargets({defaultTarget, customTarget});

    QStringList pending = allTargets;
    QCOMPARE(
        RideCacheSave::takeDeferredTargetsForCancellation(
            pending,
            defaultTarget,
            true,
            false),
        QStringList({customTarget}));
    QVERIFY(pending.isEmpty());

    pending = allTargets;
    QCOMPARE(
        RideCacheSave::takeDeferredTargetsForCancellation(
            pending,
            defaultTarget,
            true,
            true),
        allTargets);

    pending = allTargets;
    QCOMPARE(
        RideCacheSave::takeDeferredTargetsForCancellation(
            pending,
            defaultTarget,
            false,
            false),
        allTargets);
}

void
TestRideCacheSaveSnapshot::
synchronousSaveWaitsForRefreshWorkers()
{
    std::atomic<bool> completed{false};
    std::unique_ptr<QThread> worker(QThread::create([&completed]() {
        QThread::msleep(50);
        completed.store(true, std::memory_order_release);
    }));
    worker->start();

    QString error;
    QVERIFY(RideCacheSave::waitForRefreshWorkers(
        {worker.get()}, error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(completed.load(std::memory_order_acquire));

    QVERIFY(!RideCacheSave::waitForRefreshWorkers(
        {QThread::currentThread()}, error));
    QVERIFY(!error.isEmpty());
}

void
TestRideCacheSaveSnapshot::stalledRefreshWorkerTimesOut()
{
    std::unique_ptr<QThread> worker(QThread::create([]() {
        QThread::msleep(100);
    }));
    worker->start();

    QString error;
    QVERIFY(!RideCacheSave::waitForRefreshWorkers(
        {worker.get()}, error, 5));
    QVERIFY(!error.isEmpty());
    QVERIFY(worker->wait());
}

void
TestRideCacheSaveSnapshot::
synchronousSaveRequiresLatestRefreshGeneration()
{
    using Action = RideCacheSave::RefreshBarrierAction;

    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            2, true, true, false),
        Action::WaitForWorkers);
    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            0, false, true, false),
        Action::StartPendingRefresh);
    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            0, false, true, true),
        Action::Capture);
    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            1, false, false, false),
        Action::WaitForWorkers);
    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            0, false, false, false),
        Action::Capture);
    QCOMPARE(
        RideCacheSave::refreshBarrierAction(
            0, true, false, false),
        Action::Invalid);

    int completedWorkers = 0;
    int pendingStarts = 0;
    QString error;
    QVERIFY(RideCacheSave::settleRefreshBarrier(
        []() {
            return RideCacheSave::RefreshBarrierState{};
        },
        [&](QThread*) {
            ++completedWorkers;
        },
        [&]() {
            ++pendingStarts;
        },
        error,
        0));
    QVERIFY(error.isEmpty());
    QCOMPARE(completedWorkers, 0);
    QCOMPARE(pendingStarts, 0);
}

void
TestRideCacheSaveSnapshot::
synchronousSaveSettlesLatestRefreshGeneration()
{
    QVector<QThread*> workers;
    std::atomic<int> firstGenerationRuns{0};
    std::atomic<int> latestGenerationRuns{0};
    bool active = true;
    bool pending = true;
    int pendingStarts = 0;
    int completedWorkers = 0;
    int liveQueuedCallbacks = 0;
    bool completionWaitsSucceeded = true;
    QObject receiver;

    const auto startWorker =
        [&](std::atomic<int> &runCount) {
            runCount.fetch_add(1, std::memory_order_relaxed);
            QThread *worker = new QThread;
            QPointer<QThread> weakWorker(worker);
            QMetaObject::invokeMethod(
                &receiver,
                [weakWorker, &liveQueuedCallbacks]() {
                    if (weakWorker) ++liveQueuedCallbacks;
                },
                Qt::QueuedConnection);
            workers.append(worker);
        };

    startWorker(firstGenerationRuns);
    startWorker(firstGenerationRuns);

    QString error;
    const bool settled = RideCacheSave::settleRefreshBarrier(
        [&]() {
            return RideCacheSave::RefreshBarrierState{
                workers, active, pending, false
            };
        },
        [&](QThread *worker) {
            workers.removeOne(worker);
            completionWaitsSucceeded =
                worker->wait() && completionWaitsSucceeded;
            delete worker;
            ++completedWorkers;
            if (workers.isEmpty()) active = false;
        },
        [&]() {
            ++pendingStarts;
            pending = false;
            active = true;
            startWorker(latestGenerationRuns);
        },
        error,
        1000);

    for (QThread *worker : std::as_const(workers)) {
        worker->wait();
        delete worker;
    }
    workers.clear();
    QCoreApplication::processEvents();

    QVERIFY2(settled, qPrintable(error));
    QCOMPARE(firstGenerationRuns.load(), 2);
    QCOMPARE(latestGenerationRuns.load(), 1);
    QCOMPARE(pendingStarts, 1);
    QCOMPARE(completedWorkers, 3);
    QCOMPARE(liveQueuedCallbacks, 0);
    QVERIFY(completionWaitsSucceeded);
}

void
TestRideCacheSaveSnapshot::
supersededRefreshResultRemainsStale()
{
    const auto current =
        RideCacheStartup::refreshResultDisposition(true);
    QVERIFY(!current.keepStale);
    QVERIFY(current.markCacheChanged);

    const auto superseded =
        RideCacheStartup::refreshResultDisposition(false);
    QVERIFY(superseded.keepStale);
    QVERIFY(!superseded.markCacheChanged);
}

void
TestRideCacheSaveSnapshot::
capturedRideItemsCanBeDestroyedBeforeWorkerSave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("rideDB.json"));
    RideCacheSave::Snapshot snapshot;

    {
        RideItem item;
        item.dateTime = QDateTime(
            QDate(2026, 7, 28),
            QTime(19, 15),
            QTimeZone::UTC);
        item.fileName = QStringLiteral("captured.json");
        item.isstale = false;
        item.fingerprint = 11;
        item.crc = 12;
        item.metacrc = 13;
        item.timestamp = 14;
        item.dbversion = 15;
        item.udbversion = 16;
        item.color = QColor(QStringLiteral("#123abc"));
        item.present = QStringLiteral("P H S");
        item.sport = QStringLiteral("Bike");
        item.isAero = true;
        item.weight = 78.25;
        item.zoneRange = 2;
        item.hrZoneRange = 3;
        item.paceZoneRange = 4;
        item.overrides_ = {QStringLiteral("capture_metric")};
        item.samples = true;
        item.metrics() = {321.0};
        item.counts() = {9.0};
        item.stdmeans().insert(0, 300.0);
        item.stdvariances().insert(0, 4.0);
        item.metadata().insert(
            QStringLiteral("snapshot_revision"),
            QStringLiteral("before"));
        item.xdata().insert(
            QStringLiteral("EXTRA"),
            {QStringLiteral("captured")});

        auto *interval = new IntervalItem();
        interval->name = QStringLiteral("Captured interval");
        interval->start = 10;
        interval->stop = 20;
        interval->startKM = 1.5;
        interval->stopKM = 2.5;
        interval->type = RideFileInterval::ROUTE;
        interval->test = true;
        interval->color = QColor(QStringLiteral("#abcdef"));
        interval->route = QUuid(
            QStringLiteral(
                "12345678-1234-5678-9abc-def012345678"));
        interval->displaySequence = 7;
        interval->metrics() = {333.0};
        interval->counts() = {5.0};
        interval->stdmeans().insert(0, 320.0);
        interval->stdvariances().insert(0, 6.0);
        item.intervals().append(interval);

        snapshot = RideCacheSave::capture(
            QStringLiteral("2.0"),
            path,
            {{QStringLiteral("capture_metric"), 0, false}},
            {&item});

        QCOMPARE(snapshot.rides.size(), 1);
        const RideCacheSave::Ride &captured = snapshot.rides.first();
        QCOMPARE(captured.dateTime, item.dateTime);
        QCOMPARE(captured.fileName, item.fileName);
        QCOMPARE(captured.fingerprint, quint64(11));
        QCOMPARE(captured.crc, quint64(12));
        QCOMPARE(captured.metadataCrc, quint64(13));
        QCOMPARE(captured.timestamp, quint64(14));
        QCOMPARE(captured.databaseVersion, 15);
        QCOMPARE(captured.userDatabaseVersion, 16);
        QCOMPARE(captured.color, QStringLiteral("#123abc"));
        QCOMPARE(captured.present, QStringLiteral("P H S"));
        QCOMPARE(captured.sport, QStringLiteral("Bike"));
        QVERIFY(captured.aero);
        QCOMPARE(captured.weight, 78.25);
        QCOMPARE(captured.zoneRange, 2);
        QCOMPARE(captured.hrZoneRange, 3);
        QCOMPARE(captured.paceZoneRange, 4);
        QCOMPARE(
            captured.overrides,
            QStringList({QStringLiteral("capture_metric")}));
        QVERIFY(captured.samples);
        QCOMPARE(captured.metricValues.values, QVector<double>({321.0}));
        QCOMPARE(captured.metricValues.counts, QVector<double>({9.0}));
        QCOMPARE(captured.metricValues.stdMeans.value(0), 300.0);
        QCOMPARE(captured.metricValues.stdVariances.value(0), 4.0);
        QCOMPARE(
            captured.metadata.value(
                QStringLiteral("snapshot_revision")),
            QStringLiteral("before"));
        QCOMPARE(
            captured.xdata.value(QStringLiteral("EXTRA")),
            QStringList({QStringLiteral("captured")}));
        QCOMPARE(captured.intervals.size(), 1);
        const RideCacheSave::Interval &capturedInterval =
            captured.intervals.first();
        QCOMPARE(
            capturedInterval.name,
            QStringLiteral("Captured interval"));
        QCOMPARE(capturedInterval.start, 10.0);
        QCOMPARE(capturedInterval.stop, 20.0);
        QCOMPARE(capturedInterval.startKm, 1.5);
        QCOMPARE(capturedInterval.stopKm, 2.5);
        QCOMPARE(
            capturedInterval.type,
            static_cast<int>(RideFileInterval::ROUTE));
        QVERIFY(capturedInterval.test);
        QCOMPARE(capturedInterval.color, QStringLiteral("#abcdef"));
        QCOMPARE(
            capturedInterval.route,
            QStringLiteral(
                "{12345678-1234-5678-9abc-def012345678}"));
        QCOMPARE(capturedInterval.sequence, 7);
        QCOMPARE(
            capturedInterval.metricValues.values,
            QVector<double>({333.0}));
        QCOMPARE(
            capturedInterval.metricValues.counts,
            QVector<double>({5.0}));
        QCOMPARE(
            capturedInterval.metricValues.stdMeans.value(0),
            320.0);
        QCOMPARE(
            capturedInterval.metricValues.stdVariances.value(0),
            6.0);

        item.fileName = QStringLiteral("mutated.json");
        item.metrics()[0] = 999.0;
        item.metadata()[QStringLiteral("snapshot_revision")] =
            QStringLiteral("after");
        interval->name = QStringLiteral("Mutated interval");
        delete interval;
        item.clearIntervals();
    }

    RideCacheBackgroundSaver saver;
    QVERIFY(saver.enqueue(
        std::make_shared<const RideCacheSave::Snapshot>(
            std::move(snapshot))));
    QString error;
    QVERIFY2(saver.drain(&error), qPrintable(error));

    const QJsonObject ride =
        parseDocument(readFile(path))
            .object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .first()
            .toObject();
    QCOMPARE(
        ride.value(QStringLiteral("filename")).toString(),
        QStringLiteral("captured.json"));
    const QJsonArray capturedMetric =
        ride.value(QStringLiteral("METRICS"))
            .toObject()
            .value(QStringLiteral("capture_metric"))
            .toArray();
    QCOMPARE(capturedMetric.size(), 4);
    QCOMPARE(capturedMetric.at(0).toString(), QStringLiteral("321.00000"));
    QCOMPARE(capturedMetric.at(1).toString(), QStringLiteral("9.00000"));
    QCOMPARE(capturedMetric.at(2).toString(), QStringLiteral("300.00000"));
    QCOMPARE(capturedMetric.at(3).toString(), QStringLiteral("4.00000"));
    QVERIFY(
        ride.value(QStringLiteral("TAGS"))
            .toObject()
            .value(QStringLiteral("snapshot_revision"))
            .toString()
            .startsWith(QStringLiteral("before")));
    QVERIFY(
        ride.value(QStringLiteral("INTERVALS"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("name"))
            .toString()
            .startsWith(QStringLiteral("Captured interval")));
}

void
TestRideCacheSaveSnapshot::captureSkipsIncompleteAndDiscardedRides()
{
    RideItem incomplete;
    incomplete.fileName = QStringLiteral("incomplete.json");

    RideItem discarded;
    discarded.fileName = QStringLiteral("discarded.json");
    discarded.metrics() = {1.0};
    discarded.counts() = {1.0};
    discarded.skipsave = true;

    RideItem stale;
    stale.fileName = QStringLiteral("stale.json");
    stale.metrics() = {1.5};
    stale.counts() = {1.0};
    stale.isstale = true;

    RideItem retained;
    retained.fileName = QStringLiteral("retained.json");
    retained.isstale = false;
    retained.metrics() = {2.0};
    retained.counts() = {1.0};

    const RideCacheSave::Snapshot snapshot =
        RideCacheSave::capture(
            QStringLiteral("2.0"),
            QStringLiteral("/cache/rideDB.json"),
            {{QStringLiteral("capture_metric"), 0, false}},
            {&incomplete, &discarded, &stale, &retained});
    QCOMPARE(snapshot.rides.size(), 1);
    QCOMPARE(
        snapshot.rides.first().fileName,
        QStringLiteral("retained.json"));
}

void
TestRideCacheSaveSnapshot::backgroundSaverPublishesSnapshotsInFifoOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("rideDB.json"));

    RideCacheSave::Snapshot first = richSnapshot();
    first.targetPath = path;
    first.rides[0].fileName = QStringLiteral("first.json");
    RideCacheSave::Snapshot second = first;
    second.rides[0].fileName = QStringLiteral("second.json");
    const auto firstSnapshot =
        std::make_shared<const RideCacheSave::Snapshot>(first);
    const auto secondSnapshot =
        std::make_shared<const RideCacheSave::Snapshot>(second);

    RideCacheBackgroundSaver saver;
    QVERIFY(saver.enqueue(firstSnapshot));
    QVERIFY(saver.enqueue(secondSnapshot));

    first.rides[0].fileName = QStringLiteral("mutated.json");
    first.rides.clear();
    second.rides.clear();

    QString error;
    QVERIFY2(saver.drain(&error), qPrintable(error));
    const QJsonDocument document = parseDocument(readFile(path));
    QVERIFY(document.isObject());
    QCOMPARE(
        document.object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("filename"))
            .toString(),
        QStringLiteral("second.json"));
}

void
TestRideCacheSaveSnapshot::blockingSaveWaitsForQueuedSnapshots()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("rideDB.json"));

    RideCacheSave::Snapshot queued = richSnapshot();
    queued.targetPath = path;
    queued.rides[0].fileName = QStringLiteral("queued.json");
    RideCacheSave::Snapshot blocking = queued;
    blocking.rides[0].fileName = QStringLiteral("blocking.json");

    RideCacheBackgroundSaver saver;
    QVERIFY(saver.enqueue(
        std::make_shared<const RideCacheSave::Snapshot>(queued)));
    QString error;
    QVERIFY2(
        saver.writeAndWait(
            std::make_shared<const RideCacheSave::Snapshot>(blocking),
            error),
        qPrintable(error));

    const QJsonDocument document = parseDocument(readFile(path));
    QCOMPARE(
        document.object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("filename"))
            .toString(),
        QStringLiteral("blocking.json"));
}

void
TestRideCacheSaveSnapshot::
backgroundSaverWaitTimeoutRetainsQueuedFailure()
{
    std::mutex gateMutex;
    std::condition_variable gate;
    bool released = false;
    int writes = 0;

    RideCacheBackgroundSaver saver(
        [&](const RideCacheSave::Snapshot&, QString &error) {
            std::unique_lock<std::mutex> lock(gateMutex);
            gate.wait(lock, [&released]() {
                return released;
            });
            ++writes;
            error = QStringLiteral("deferred write failed");
            return false;
        });

    const auto snapshot =
        std::make_shared<const RideCacheSave::Snapshot>(
            richSnapshot());
    QString error;
    const bool timedOut =
        !saver.writeAndWait(snapshot, error, 5);
    const QString timeoutError = error;

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
    }
    gate.notify_one();

    QString deferredError;
    const bool deferredFailure =
        !saver.drain(&deferredError);

    QVERIFY(timedOut);
    QVERIFY(timeoutError.contains(QStringLiteral("Timed out")));
    QVERIFY(deferredFailure);
    QCOMPARE(writes, 1);
    QVERIFY(deferredError.contains(
        QStringLiteral("deferred write failed")));
}

void
TestRideCacheSaveSnapshot::drainReportsQueuedWriteFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RideCacheSave::Snapshot invalid = richSnapshot();
    invalid.targetPath =
        directory.filePath(QStringLiteral("rideDB.json"));
    invalid.metrics[0].index = -1;

    RideCacheBackgroundSaver saver;
    QTest::ignoreMessage(
        QtWarningMsg,
        "Cannot save ride cache: "
        "Ride cache metric schema contains a negative index");
    QVERIFY(saver.enqueue(
        std::make_shared<const RideCacheSave::Snapshot>(invalid)));

    QString error;
    QVERIFY(!saver.drain(&error));
    QVERIFY(error.contains(QStringLiteral("negative index")));

    error = QStringLiteral("stale");
    QVERIFY(saver.drain(&error));
    QVERIFY(error.isEmpty());
}

void
TestRideCacheSaveSnapshot::stopDrainsQueuedSnapshots()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RideCacheSave::Snapshot snapshot = richSnapshot();
    snapshot.targetPath =
        directory.filePath(QStringLiteral("rideDB.json"));

    RideCacheBackgroundSaver saver;
    QVERIFY(saver.enqueue(
        std::make_shared<const RideCacheSave::Snapshot>(snapshot)));
    saver.stop();

    QVERIFY(parseDocument(readFile(snapshot.targetPath)).isObject());
    QVERIFY(!saver.isRunning());
    QVERIFY(!saver.enqueue(
        std::make_shared<const RideCacheSave::Snapshot>(snapshot)));
}

void
TestRideCacheSaveSnapshot::completeSnapshotSerializesAsValidJson()
{
    const QByteArray bytes = serialize(richSnapshot());
    QVERIFY(!bytes.isEmpty());
    QVERIFY(bytes.startsWith("\xef\xbb\xbf"));

    const QJsonDocument document = parseDocument(bytes);
    QVERIFY(document.isObject());
    const QJsonObject root = document.object();
    QCOMPARE(root.value(QStringLiteral("VERSION")).toString(),
             QStringLiteral("2.0"));

    const QJsonArray rides =
        root.value(QStringLiteral("RIDES")).toArray();
    QCOMPARE(rides.size(), 1);
    const QJsonObject ride = rides.first().toObject();
    QCOMPARE(
        ride.value(QStringLiteral("date")).toString(),
        QStringLiteral("2026/07/28 18:42:05 UTC"));
    QCOMPARE(
        ride.value(QStringLiteral("filename")).toString(),
        QStringLiteral("2026_07_28_18_42_05.json"));
    QCOMPARE(ride.value(QStringLiteral("fingerprint")).toString(),
             QStringLiteral("101"));
    QCOMPARE(ride.value(QStringLiteral("crc")).toString(),
             QStringLiteral("202"));
    QCOMPARE(ride.value(QStringLiteral("metacrc")).toString(),
             QStringLiteral("303"));
    QCOMPARE(ride.value(QStringLiteral("timestamp")).toString(),
             QStringLiteral("404"));
    QCOMPARE(ride.value(QStringLiteral("dbversion")).toString(),
             QStringLiteral("7"));
    QCOMPARE(ride.value(QStringLiteral("udbversion")).toString(),
             QStringLiteral("8"));
    QCOMPARE(ride.value(QStringLiteral("color")).toString(),
             QStringLiteral("#123456"));
    QCOMPARE(ride.value(QStringLiteral("present")).toString(),
             QStringLiteral("P H"));
    QCOMPARE(ride.value(QStringLiteral("sport")).toString(),
             QStringLiteral("Bike"));
    QCOMPARE(
        ride.value(QStringLiteral("weight")).toString(),
        QStringLiteral("79.5"));
    QCOMPARE(
        ride.value(QStringLiteral("aero")).toString(),
        QStringLiteral("1"));
    QCOMPARE(ride.value(QStringLiteral("zonerange")).toString(),
             QStringLiteral("2"));
    QCOMPARE(ride.value(QStringLiteral("hrzonerange")).toString(),
             QStringLiteral("3"));
    QCOMPARE(ride.value(QStringLiteral("pacezonerange")).toString(),
             QStringLiteral("4"));
    QCOMPARE(
        ride.value(QStringLiteral("overrides")).toString(),
        QStringLiteral("average_power,zero_metric"));
    QCOMPARE(ride.value(QStringLiteral("samples")).toString(),
             QStringLiteral("1"));

    const QJsonObject metrics =
        ride.value(QStringLiteral("METRICS")).toObject();
    QCOMPARE(
        metrics.value(QStringLiteral("average_power")).toString(),
        QStringLiteral("245.25000"));
    QCOMPARE(
        metrics.value(QStringLiteral("zero_metric")).toArray().size(),
        2);
    const QJsonArray variability =
        metrics.value(QStringLiteral("variability")).toArray();
    QCOMPARE(variability.size(), 4);
    QCOMPARE(variability.at(0).toString(), QStringLiteral("1.07000"));
    QCOMPARE(variability.at(1).toString(), QStringLiteral("3600.00000"));
    QCOMPARE(variability.at(2).toString(), QStringLiteral("1.05000"));
    QCOMPARE(variability.at(3).toString(), QStringLiteral("0.02000"));

    const QString notes =
        ride.value(QStringLiteral("TAGS")).toObject()
            .value(QStringLiteral("Notes")).toString();
    QVERIFY(notes.startsWith(QStringLiteral("line 1\n\"quoted\"/path")));

    const QJsonArray xdata =
        ride.value(QStringLiteral("XDATA")).toObject()
            .value(QStringLiteral("EXTRA")).toArray();
    QCOMPARE(xdata.size(), 2);
    QVERIFY(xdata.at(1).toString().startsWith(
        QStringLiteral("right/value")));

    const QJsonArray intervals =
        ride.value(QStringLiteral("INTERVALS")).toArray();
    QCOMPARE(intervals.size(), 1);
    const QJsonObject interval = intervals.first().toObject();
    QVERIFY(interval.value(QStringLiteral("name")).toString().startsWith(
        QStringLiteral("Threshold \"set\"")));
    QCOMPARE(interval.value(QStringLiteral("start")).toString(),
             QStringLiteral("60.5"));
    QCOMPARE(interval.value(QStringLiteral("stop")).toString(),
             QStringLiteral("360.25"));
    QCOMPARE(interval.value(QStringLiteral("startKM")).toString(),
             QStringLiteral("1.25"));
    QCOMPARE(interval.value(QStringLiteral("stopKM")).toString(),
             QStringLiteral("12.75"));
    QCOMPARE(interval.value(QStringLiteral("type")).toString(),
             QStringLiteral("3"));
    QCOMPARE(interval.value(QStringLiteral("test")).toString(),
             QStringLiteral("true"));
    QCOMPARE(interval.value(QStringLiteral("color")).toString(),
             QStringLiteral("#abcdef"));
    QCOMPARE(
        interval.value(QStringLiteral("route")).toString(),
        QStringLiteral("12345678-1234-5678-9abc-def012345678"));
    QCOMPARE(
        interval.value(QStringLiteral("seq")).toString(),
        QStringLiteral("9"));
    QCOMPARE(
        interval.value(QStringLiteral("METRICS")).toObject()
            .value(QStringLiteral("variability")).toArray().size(),
        4);
}

void
TestRideCacheSaveSnapshot::legacyMetricFilteringIsPreserved()
{
    RideCacheSave::Snapshot snapshot;
    snapshot.version = QStringLiteral("2.0");
    snapshot.metrics = {
        {QStringLiteral("positive"), 0, false},
        {QStringLiteral("negative"), 1, false},
        {QStringLiteral("plain_zero"), 2, false},
        {QStringLiteral("aggregate_zero"), 3, true},
        {QStringLiteral("not_a_number"), 4, false},
        {QStringLiteral("infinite"), 5, false}
    };

    RideCacheSave::Ride ride;
    ride.metricValues = metricValues(
        {
            2.0,
            -2.0,
            0.0,
            0.0,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity()
        },
        {0.0, 0.0, 10.0, 2.0, 0.0, 0.0});

    RideCacheSave::Interval interval;
    interval.metricValues = metricValues(
        {0.0, -3.0, 0.0, 0.0, 0.0, 0.0},
        {0.0, 4.0, 0.0, 7.0, 0.0, 0.0});
    ride.intervals.append(interval);
    snapshot.rides.append(ride);

    const QJsonObject serializedRide =
        parseDocument(serialize(snapshot))
            .object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .first()
            .toObject();
    const QJsonObject metrics =
        serializedRide.value(QStringLiteral("METRICS")).toObject();
    QVERIFY(metrics.contains(QStringLiteral("positive")));
    QVERIFY(metrics.contains(QStringLiteral("negative")));
    QVERIFY(metrics.contains(QStringLiteral("aggregate_zero")));
    QVERIFY(!metrics.contains(QStringLiteral("plain_zero")));
    QVERIFY(!metrics.contains(QStringLiteral("not_a_number")));
    QVERIFY(!metrics.contains(QStringLiteral("infinite")));

    const QJsonObject intervalMetrics =
        serializedRide.value(QStringLiteral("INTERVALS"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("METRICS"))
            .toObject();
    QVERIFY(!intervalMetrics.contains(QStringLiteral("positive")));
    QVERIFY(intervalMetrics.contains(QStringLiteral("negative")));
    QVERIFY(intervalMetrics.contains(QStringLiteral("aggregate_zero")));
    QVERIFY(!intervalMetrics.contains(QStringLiteral("plain_zero")));
    QVERIFY(!intervalMetrics.contains(QStringLiteral("not_a_number")));
    QVERIFY(!intervalMetrics.contains(QStringLiteral("infinite")));
}

void
TestRideCacheSaveSnapshot::snapshotDoesNotObserveLaterSourceMutations()
{
    RideCacheSave::Snapshot source = richSnapshot();
    const RideCacheSave::Snapshot snapshot = source;
    const QByteArray expected = serialize(snapshot);
    QVERIFY(!expected.isEmpty());

    source.metrics[0].name = QStringLiteral("mutated_metric");
    source.rides[0].fileName = QStringLiteral("mutated.json");
    source.rides[0].metricValues.values[0] = 999.0;
    source.rides[0].metadata[QStringLiteral("Notes")] =
        QStringLiteral("mutated");
    source.rides[0].xdata[QStringLiteral("EXTRA")][0] =
        QStringLiteral("mutated");
    source.rides[0].intervals[0].name =
        QStringLiteral("mutated interval");
    source.rides.clear();

    QCOMPARE(serialize(snapshot), expected);
}

void
TestRideCacheSaveSnapshot::concurrentSerializationIsDeterministic()
{
    constexpr int ThreadCount = 4;
    constexpr int Iterations = 100;

    const auto snapshot =
        std::make_shared<const RideCacheSave::Snapshot>(richSnapshot());
    const QByteArray expected = serialize(*snapshot);
    QVERIFY(!expected.isEmpty());
    const QByteArray expectedHash =
        QCryptographicHash::hash(expected, QCryptographicHash::Sha256);

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(ThreadCount);
    for (int thread = 0; thread < ThreadCount; ++thread) {
        workers.emplace_back([&, snapshot]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < Iterations; ++iteration) {
                QString error;
                const QByteArray bytes = serialize(*snapshot, &error);
                if (!error.isEmpty()
                    || QCryptographicHash::hash(
                           bytes, QCryptographicHash::Sha256)
                        != expectedHash) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers) worker.join();
    QVERIFY(!failed.load(std::memory_order_acquire));
}

void
TestRideCacheSaveSnapshot::nonFiniteIntervalDoesNotCreateMetricObject()
{
    RideCacheSave::Snapshot snapshot = richSnapshot();
    RideCacheSave::Interval &interval =
        snapshot.rides[0].intervals[0];
    interval.metricValues.values.fill(0.0);
    interval.metricValues.values[0] =
        std::numeric_limits<double>::quiet_NaN();
    interval.metricValues.stdMeans.clear();
    interval.metricValues.stdVariances.clear();

    const QJsonDocument document =
        parseDocument(serialize(snapshot));
    QVERIFY(document.isObject());
    const QJsonObject serializedInterval =
        document.object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("INTERVALS"))
            .toArray()
            .first()
            .toObject();
    QVERIFY(!serializedInterval.contains(
        QStringLiteral("METRICS")));
}

void
TestRideCacheSaveSnapshot::writePublishesSerializedSnapshot()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RideCacheSave::Snapshot snapshot = richSnapshot();
    snapshot.targetPath =
        directory.filePath(QStringLiteral("rideDB.json"));
    QString error;
    QVERIFY2(
        RideCacheSave::write(snapshot, error),
        qPrintable(error));
    QVERIFY(error.isEmpty());

    const QJsonDocument document =
        parseDocument(readFile(snapshot.targetPath));
    QVERIFY(document.isObject());
    QCOMPARE(
        document.object()
            .value(QStringLiteral("RIDES"))
            .toArray()
            .size(),
        1);
}

void
TestRideCacheSaveSnapshot::failedSerializationPreservesPreviousFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RideCacheSave::Snapshot snapshot = richSnapshot();
    snapshot.targetPath =
        directory.filePath(QStringLiteral("rideDB.json"));
    const QByteArray previous(
        "{\"VERSION\":\"previous\",\"RIDES\":[]}\n");
    QVERIFY(writeFile(snapshot.targetPath, previous));
    snapshot.rides[0].metricValues.values.clear();

    QString error;
    QVERIFY(!RideCacheSave::write(snapshot, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(snapshot.targetPath), previous);
}

void
TestRideCacheSaveSnapshot::missingTargetPathFails()
{
    RideCacheSave::Snapshot snapshot = richSnapshot();
    QString error;

    QVERIFY(!RideCacheSave::write(snapshot, error));
    QVERIFY(!error.isEmpty());
}

void
TestRideCacheSaveSnapshot::invalidMetricSchemaFailsClosed_data()
{
    QTest::addColumn<int>("failure");

    QTest::newRow("negative index") << 0;
    QTest::newRow("ride values too short") << 1;
    QTest::newRow("ride counts too short") << 2;
    QTest::newRow("interval values too short") << 3;
    QTest::newRow("interval counts too short") << 4;
}

void
TestRideCacheSaveSnapshot::invalidMetricSchemaFailsClosed()
{
    QFETCH(int, failure);
    RideCacheSave::Snapshot snapshot = richSnapshot();

    switch (failure) {
    case 0:
        snapshot.metrics[0].index = -1;
        break;
    case 1:
        snapshot.rides[0].metricValues.values.removeLast();
        break;
    case 2:
        snapshot.rides[0].metricValues.counts.removeLast();
        break;
    case 3:
        snapshot.rides[0].intervals[0]
            .metricValues.values.removeLast();
        break;
    case 4:
        snapshot.rides[0].intervals[0]
            .metricValues.counts.removeLast();
        break;
    default:
        QFAIL("Unexpected data row");
    }

    QByteArray bytes("sentinel");
    QString error;
    QVERIFY(!RideCacheSave::serialize(snapshot, bytes, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(bytes, QByteArray("sentinel"));
}

QTEST_GUILESS_MAIN(TestRideCacheSaveSnapshot)
#include "testRideCacheSaveSnapshot.moc"
