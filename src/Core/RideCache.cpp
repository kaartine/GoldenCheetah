/*
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "RideCache.h"
#include "RideCacheAggregate.h"
#include "RideCacheBulkMerge.h"
#include "RideCacheSnapshot.h"

#include "Context.h"
#include "Athlete.h"
#include "RideFileCache.h"
#include "RideCacheModel.h"
#include "Specification.h"
#include "DataProcessor.h"
#include "Estimator.h"
#include "Colors.h"

#include "Route.h"

#include "Zones.h"
#include "HrZones.h"
#include "PaceZones.h"

#include "ErgFile.h"

#include "JsonRideFile.h" // for DATETIME_FORMAT

#ifdef SLOW_REFRESH
#include "unistd.h"
#endif

// we initialise the global user metrics
#include "RideMetric.h"
#include "UserMetricSettings.h"
#include "UserMetricParser.h"
#include "SpecialFields.h"
#include <QXmlInputSource>
#include <QXmlSimpleReader>

// for sorting
bool rideCacheLessThan(const RideItem *a, const RideItem *b) { return a->dateTime < b->dateTime; }

QStringList
RideCache::startupRideFiles(const QDir &directory) const
{
    return RideFileFactory::instance().listRideFiles(directory);
}

class RideCacheLoader : public QThread
{
public:
    explicit RideCacheLoader(RideCache *cache)
        : QThread(cache),
          cache(cache),
          activityDirectory(cache->directory),
          plannedDirectory(cache->plannedDirectory)
    {
    }

    void run() override
    {
        const QString activityPath =
            activityDirectory.canonicalPath();
        const QString plannedPath =
            plannedDirectory.canonicalPath();
        QVector<RideCacheStartup::IndexedFile> files =
            RideCacheStartup::buildIndex(
                cache->startupRideFiles(activityDirectory),
                activityPath,
                cache->startupRideFiles(plannedDirectory),
                plannedPath,
                [](const QString &name, QDateTime *dateTime) {
                    return RideFile::parseRideFileName(
                        name, dateTime);
                });

        cache->startupExpectedRideCount_.store(
            files.size(), std::memory_order_release);
        for (const RideCacheStartup::BatchRange &range :
             RideCacheStartup::batchRanges(
                 files.size(),
                 RideCacheStartup::BatchSize)) {
            if (isInterruptionRequested()) return;
            auto batch = std::make_shared<
                QVector<RideCacheStartup::IndexedFile>>(
                    files.mid(range.first, range.count));
            QMetaObject::invokeMethod(
                cache,
                [target = cache, batch]() {
                    if (!target->exiting) {
                        target->appendStartupFiles(batch);
                    }
                },
                Qt::QueuedConnection);
        }
        files.clear();
        files.squeeze();

        if (isInterruptionRequested()) return;

        QMetaObject::invokeMethod(
            cache,
            [target = cache]() {
                if (!target->exiting) {
                    target->startupIndexComplete();
                }
            },
            Qt::QueuedConnection);

        cache->load();
        if (isInterruptionRequested()) return;

        QMetaObject::invokeMethod(
            cache,
            [target = cache]() {
                if (!target->exiting) target->postLoad();
            },
            Qt::QueuedConnection);
    }

private:
    RideCache *cache;
    QDir activityDirectory;
    QDir plannedDirectory;
};

RideCache::RideCache(Context *context) : context(context)
{
    directory = context->athlete->home->activities();
    plannedDirectory = context->athlete->home->planned();

    progress_ = 100;
    exiting = false;
    estimator = new Estimator(context);

    // initial load of user defined metrics - do once we have an initial context
    // but before we refresh or check metrics for the first time
    if (UserMetricSchemaVersion == 0) {

        QString metrics = QString("%1/../usermetrics.xml").arg(context->athlete->home->root().absolutePath());
        if (QFile(metrics).exists()) {

            QFile metricfile(metrics);
            QXmlInputSource source(&metricfile);
            QXmlSimpleReader xmlReader;
            UserMetricParser handler;

            xmlReader.setContentHandler(&handler);
            xmlReader.setErrorHandler(&handler);

            // parse and get return values
            xmlReader.parse(source);
            _userMetrics = handler.getSettings();
            UserMetric::addCompatibility(_userMetrics);

            // reset schema version
            UserMetricSchemaVersion = RideMetric::userMetricFingerprint(_userMetrics);

            // now add initial metrics
            foreach(UserMetricSettings m, _userMetrics) {
                RideMetricFactory::instance().addMetric(UserMetric(context, m));
            }
        }

        // reset special fields to take into account user metrics
        SpecialFields::getInstance().reloadFields();
    }

    model_ = new RideCacheModel(context, this);
    first = true;
    connect(
        context, SIGNAL(refreshEnd()),
        this, SLOT(initEstimates()));
    connect(
        context, SIGNAL(configChanged(qint32)),
        this, SLOT(configChanged(qint32)));

    saveThread_ = new QThread(this);
    saveWorker_ = new QObject();
    saveWorker_->moveToThread(saveThread_);
    saveThread_->start();

    startupLoader_ = new RideCacheLoader(this);
    startupLoader_->start();
}

void
RideCache::appendStartupFiles(
    const std::shared_ptr<
        QVector<RideCacheStartup::IndexedFile>> &files)
{
    if (!files || files->isEmpty()) return;

    const int firstRow = rides_.size();
    model_->startInsert(
        firstRow, firstRow + files->size() - 1);
    for (const RideCacheStartup::IndexedFile &file : *files) {
        QDateTime dateTime = file.dateTime;
        auto *item = new RideItem(
            file.path, file.fileName, dateTime,
            context, file.planned);
        connect(
            item, SIGNAL(rideDataChanged()),
            this, SLOT(itemChanged()));
        connect(
            item, SIGNAL(rideMetadataChanged()),
            this, SLOT(itemChanged()));
        rides_.append(item);
        startupItemsByFile_.insert(item->fileName, item);
        startupRows_.insert(item, rides_.size() - 1);
    }
    model_->endInsert();
}

void
RideCache::startupIndexComplete()
{
    if (startupIndexReady_) return;
    startupIndexReady_ = true;
    emit loadComplete();
}

bool
RideCache::queueStartupSnapshots(
    const std::shared_ptr<RideCacheSnapshotBatch> &batch)
{
    if (!batch || batch->isEmpty()) return true;

    QThread *loaderThread = QThread::currentThread();
    while (!startupSnapshotSlots_.tryAcquire(1, 50)) {
        if (loaderThread->isInterruptionRequested()) return false;
    }

    const bool queued = QMetaObject::invokeMethod(
        this,
        [this, batch]() {
            if (!exiting) applyStartupSnapshots(batch);
            startupSnapshotSlots_.release();
        },
        Qt::QueuedConnection);
    if (!queued) {
        startupSnapshotSlots_.release();
        loaderThread->requestInterruption();
    }
    return queued;
}

RideItem *
RideCache::startupItemFor(
    const QString &fileName,
    const QDateTime &dateTime) const
{
    const QList<RideItem*> candidates =
        startupItemsByFile_.values(fileName);
    RideItem *match = nullptr;
    for (RideItem *item : candidates) {
        if (!item || item->dateTime != dateTime) continue;
        if (match) {
            // The persisted format cannot distinguish identical planned
            // and completed entries. Refresh both instead of guessing.
            return nullptr;
        }
        match = item;
    }
    return match;
}

void
RideCache::invalidateStartupSnapshots()
{
    if (startupLoadFinished_ || startupSnapshotsInvalidated_) return;
    startupSnapshotsInvalidated_ = true;
    startupItemsByFile_.clear();
    startupRows_.clear();
}


void
RideCache::applyStartupSnapshots(
    const std::shared_ptr<RideCacheSnapshotBatch> &batch)
{
    if (!batch || batch->isEmpty()) return;
    if (startupSnapshotsInvalidated_) return;

    QVector<int> changedRows;
    changedRows.reserve(batch->size());
    for (RideCacheItemSnapshot &snapshot : batch->snapshots()) {
        RideItem *item = startupItemFor(
            snapshot.fileName(), snapshot.dateTime());
        const int row = startupRows_.value(item, -1);
        if (item && row >= 0 && snapshot.applyTo(*item)) {
            changedRows.append(row);
        }
    }
    model_->rowsChanged(changedRows);
}

void
RideCache::postLoad()
{
    if (startupLoadFinished_) return;
    startupLoadFinished_ = true;
    startupItemsByFile_.clear();
    startupRows_.clear();
    emit startupLoadFinished();
    refresh();
}

struct comparerideitem { bool operator()(const RideItem *p1, const RideItem *p2) { return p1->dateTime < p2->dateTime; } };

int
RideCache::find(RideItem *dt)
{
    // use lower_bound to binary search
    QVector<RideItem*>::const_iterator i = std::lower_bound(rides_.begin(), rides_.end(), dt, comparerideitem());
    int index = i - rides_.begin();

    // did it find the right value?
    if (index < 0 || index >= rides_.count() || rides_.at(index)->dateTime != dt->dateTime) return -1;
    return index;
}

RideCache::~RideCache()
{
    exiting = true;

    if (startupLoader_ && startupLoader_->isRunning()) {
        startupLoader_->requestInterruption();
        startupLoader_->wait();
    }

    if (estimator) {
        estimator->stop();
        if (! estimator->wait(5000)) {
            qWarning() << "Estimator did not stop in time, forcing termination.";
            estimator->terminate();
            estimator->wait();
        }
        delete estimator;
        estimator = nullptr;
    }

    // cancel any refresh that may be running
    cancel();

    if (saveThread_) {
        saveThread_->quit();
        saveThread_->wait();
    }
    delete saveWorker_;
    saveWorker_ = nullptr;

    // Preserve the previous complete cache if startup was interrupted.
    if (startupLoadFinished_) save();
}

void
RideCache::garbageCollect()
{
    foreach(RideItem *item, delete_) {
        if (item) item->deleteLater();
    }
    delete_.clear();
}

void
RideCache::initEstimates()
{
    // kickoff first calculation
    if (first) {
        first = false;
        estimator->calculate();
    }
}

void
RideCache::configChanged(qint32 what)
{
    const RideCacheStartup::InvalidationPlan plan =
        RideCacheStartup::planInvalidation(what);

    if (plan.invalidateWbal) {
        for (RideItem *item : rides_) {
            if (item->isOpen()) item->ride()->wstale = true;
        }
    }

    if (plan.rebuildCalendarText) {
        for (RideItem *item : rides_) {
            item->metadata_.insert(
                QStringLiteral("Calendar Text"),
                GlobalContext::context()->rideMetadata->calendarText(item));
        }
    }

    if (plan.recolor) {
        for (RideItem *item : rides_) {
            item->color = GlobalContext::context()->colorEngine->colorFor(
                item->getText(
                    GlobalContext::context()->rideMetadata->getColorField(),
                    QString()));
        }
    }

    if (plan.refreshMetrics) refresh();
}

void
RideCache::itemChanged()
{
    // one of our kids changed, they grow up so fast.
    // NOTE ONLY CONNECT THIS TO RIDEITEMS !!!
    // BECAUSE IT IS ASSUMED BELOW THE SENDER IS A RIDEITEM
    RideItem *item = static_cast<RideItem*>(QObject::sender());

    // the model is particularly interested in ANY item that changes
    emit itemChanged(item);

    // current ride changed is more relevant for the charts lets notify
    // them the ride they're showing has changed
    if (item == context->currentRideItem()) {

        context->notifyRideChanged(item);
    }
}

// add a new ride
void
RideCache::addRide(QString name, bool dosignal, bool select, bool useTempActivities, bool planned)
{
    RideItem *prior = context->ride;

    // ignore malformed names
    QDateTime dt;
    if (!RideFile::parseRideFileName(name, &dt)) return;

    // new ride item
    RideItem *last;
    if (useTempActivities)
       last = new RideItem(context->athlete->home->tmpActivities().canonicalPath(), name, dt, context, false);
    else if (planned)
       last = new RideItem(plannedDirectory.canonicalPath(), name, dt, context, planned);
    else
       last = new RideItem(directory.canonicalPath(), name, dt, context, planned);

    connect(last, SIGNAL(rideDataChanged()), this, SLOT(itemChanged()));
    connect(last, SIGNAL(rideMetadataChanged()), this, SLOT(itemChanged()));

    // now add to the list, or replace if already there
    bool added = false;
    for (int index=0; index < rides_.count(); index++) {
        if (rides_[index]->fileName == last->fileName) {
            invalidateStartupSnapshots();
            rides_[index] = last;
            added = true;
            break;
        }
    }

    // add and sort, model needs to know !
    if (!added) {
        model_->beginReset();
        rides_ << last;
        std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
        model_->endReset();
    }

    // refresh metrics for *this ride only*
    last->refresh();

    if (dosignal) context->notifyRideAdded(last); // here so emitted BEFORE rideSelected is emitted!

    // free up memory from last one, which is no biggie when importing
    // a single ride, but means we don't exhaust memory when we import
    // hundreds/thousands of rides in a batch import.
    if (prior) prior->close();

    // notify everyone to select it
    if (select) {
        context->ride = last;
        context->notifyRideSelected(last);
    } else{
        // notify everyone to select the one we were already on
        context->notifyRideSelected(prior);
    }

    // model estimates (lazy refresh)
    estimator->refresh();
}

QVector<RideItem*>
RideCache::addRides(
    const QStringList &names,
    const QVector<RideFile*> &preparedRides,
    bool dosignal,
    bool select,
    bool useTempActivities,
    bool planned)
{
    RideItem *prior = context->ride;
    QVector<RideItem*> incoming;
    incoming.reserve(names.size());

    for (qsizetype index = 0; index < names.size(); ++index) {
        const QString &name = names[index];
        RideFile *prepared =
            index < preparedRides.size() ? preparedRides[index] : nullptr;

        QDateTime dt;
        if (!RideFile::parseRideFileName(name, &dt)) {
            delete prepared;
            continue;
        }

        QString path;
        bool itemPlanned = planned;
        if (useTempActivities) {
            path = context->athlete->home->tmpActivities().canonicalPath();
            itemPlanned = false;
        } else if (planned) {
            path = plannedDirectory.canonicalPath();
        } else {
            path = directory.canonicalPath();
        }

        RideItem *item = nullptr;
        if (prepared) {
            item = new RideItem(prepared, dt, context);
            item->path = path;
            item->fileName = name;
            item->planned = itemPlanned;
            item->isdirty = false;
        } else {
            item = new RideItem(
                path, name, dt, context, itemPlanned);
        }

        connect(item, SIGNAL(rideDataChanged()), this, SLOT(itemChanged()));
        connect(item, SIGNAL(rideMetadataChanged()), this, SLOT(itemChanged()));
        incoming.append(item);
    }

    for (qsizetype index = names.size();
         index < preparedRides.size(); ++index) {
        delete preparedRides[index];
    }
    if (incoming.isEmpty()) return incoming;

    const QVector<RideItem*> replaced =
        RideCacheBulkMerge::mergeItems(
            rides_,
            incoming,
            [](const RideItem *item) { return item->fileName; },
            rideCacheLessThan,
            [this]() { model_->beginReset(); },
            [this]() { model_->endReset(); });
    Q_UNUSED(replaced);

    for (RideItem *item : incoming) {
        item->refresh();
        item->close();
        if (dosignal) {
            context->notifyRideAdded(item);
        }
    }

    if (prior) prior->close();

    if (select) {
        context->ride = incoming.constLast();
        context->notifyRideSelected(context->ride);
    } else {
        context->notifyRideSelected(prior);
    }

    estimator->refresh();
    return incoming;
}

bool
RideCache::removeRides
(const QStringList &filenamesToDelete, bool triggerRefresh)
{
    if (filenamesToDelete.isEmpty()) {
        return false;
    }
    cancel();

    bool anyDeleted = false;
    for (const QString &filenameToDelete : filenamesToDelete) {
        if (filenameToDelete.isEmpty()) {
            continue;
        }

        RideItem *todelete = nullptr;
        RideItem *select = nullptr;
        int index = 0;

        for (index = 0; index < rides_.count(); index++) {
            RideItem *rideI = rides_[index];
            if (rideI->fileName == filenameToDelete) {
                todelete = rideI;
                if (context->ride == todelete) {
                    if (rides_.count() - index > 1) {
                        select = rides_[index + 1];
                    } else if (index > 0) {
                        select = rides_[index - 1];
                    }
                }
                break;
            }
        }

        if (! todelete) {
            qDebug() << "ERROR: delete not found:" << filenameToDelete;
            continue;
        }

        if (todelete->hasLinkedActivity()) {
            RideItem *linkedItem = getLinkedActivity(todelete);
            if (linkedItem) {
                linkedItem->clearLinkedFileName();
                QString error;
                saveActivity(linkedItem, error);
            }
        }

        DataProcessorFactory::instance().autoProcess(todelete->ride(), "Save", "DELETE");

        model_->startRemove(index);
        rides_.remove(index, 1);
        delete_ << todelete;
        model_->endRemove(index);

        QFile file((todelete->planned ? plannedDirectory : directory).canonicalPath() + "/" + filenameToDelete);
        QString strNewName = filenameToDelete + ".bak";
        QFile::remove(context->athlete->home->fileBackup().canonicalPath() + "/" + strNewName);
        if (! file.rename(context->athlete->home->fileBackup().canonicalPath() + "/" + strNewName)) {
            QMessageBox::critical(NULL, "Rename Error", tr("Can't rename %1 to %2 in %3")
                                                          .arg(filenameToDelete)
                                                          .arg(strNewName)
                                                          .arg(context->athlete->home->fileBackup().canonicalPath()));
        }

        QStringList extras;
        extras << "notes" << "cpi" << "cpx";
        for (const QString &extension : extras) {
            QString deleteMe = QFileInfo(filenameToDelete).baseName() + "." + extension;
            QFile::remove(context->athlete->home->cache().canonicalPath() + "/" + deleteMe);
        }

        if (select) {
            context->mainWindow->setUpdatesEnabled(false);
            context->ride = select;
            context->notifyRideDeleted(todelete);
            context->mainWindow->setUpdatesEnabled(true);
            QApplication::processEvents();
            context->notifyRideSelected(select);
        } else {
            context->notifyRideSelected(context->ride);
        }

        anyDeleted = true;
    }

    if (anyDeleted && triggerRefresh) {
        refresh();
        estimator->refresh();
    }
    return anyDeleted;
}


// NOTE:
// We use a bison parser to reduce memory
// overhead and (believe it or not) simplicity
// RideCache::load() and save() -- see RideDB.y

// export metrics to csv, for users to play with R, Matlab, Excel etc
void
RideCache::writeAsCSV(QString filename)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QVector<const RideMetric *> indexed(factory.metricCount());

    // get metrics indexed in same order as the array
    foreach(QString name, factory.allMetrics()) {

        const RideMetric *m = factory.rideMetric(name);
        indexed[m->index()] = m;
    }

    // open file.. truncate if exists already
    QFile file(filename);
    if (!file.open(QFile::WriteOnly)) {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText(tr("Problem Saving Ride Cache"));
        msgBox.setInformativeText(tr("File: %1 cannot be opened for 'Writing'. Please check file properties.").arg(filename));
        msgBox.exec();
        return;
    };
    file.resize(0);
    QTextStream out(&file);

    // write headings
    out<<"date, time, filename";
    foreach(const RideMetric *m, indexed) {
        if (m->name().startsWith("BikeScore"))
            out <<", BikeScore";
        else
            out <<", " <<m->name();
    }
    out<<"\n";

    // write values
    foreach(RideItem *item, rides()) {

        // date, time, filename
        out << item->dateTime.date().toString(Qt::ISODate);
        out << "," << item->dateTime.time().toString("hh:mm:ss");
        out << "," << item->fileName;

        // values
        foreach(double value, item->metrics()) {
            out << "," << QString("%1").arg(value, 'f').simplified();
        }

        out<<"\n";
    }
    file.close();
}

int
RideCache::nextRefresh(quint64 generation)
{
    int returning = -1;
    int completed = 0;
    bool reportProgress = false;
    {
        QMutexLocker locker(&updateMutex);
        if (updates >= 0
            && refreshGeneration_.accepts(generation)
            && updates < reverse_.count()) {
            returning = updates++;
            completed = updates;
            const int step = qMax(1, reverse_.count() / 10);
            reportProgress = completed == reverse_.count()
                || completed % step == 0;
        }
    }

    if (reportProgress) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, completed]() {
                bool accepted = false;
                {
                    QMutexLocker locker(&updateMutex);
                    accepted = refreshGeneration_.accepts(generation);
                }
                if (accepted) progressing(completed);
            },
            Qt::QueuedConnection);
    }
    return returning;
}


RideCacheRefreshThread::RideCacheRefreshThread(
    RideCache *cache, quint64 generation)
    : cache(cache), generation(generation)
{
    QPointer<RideCacheRefreshThread> weakSelf(this);
    connect(
        this, &QThread::finished, cache,
        [weakSelf, c = QPointer<RideCache>(cache), generation]() {
            if (weakSelf && c) {
                c->threadCompleted(weakSelf.data(), generation);
            }
        },
        Qt::QueuedConnection);
}

void
RideCache::cleanupThread(RideCacheRefreshThread *thread)
{
    Q_UNUSED(thread);
}

void
RideCache::threadCompleted(
    RideCacheRefreshThread *thread, quint64 generation)
{
    bool isLast = false;
    bool cancelled = false;
    bool restart = false;
    bool changed = false;
    bool notifyEnd = false;
    {
        QMutexLocker locker(&updateMutex);
        if (!refreshThreads.removeOne(thread)) return;
        isLast = refreshThreads.isEmpty();
        cancelled = isCancelled;
        if (isLast) {
            restart = refreshGeneration_.finish(generation);
            changed = refreshChanged_;
            if (!restart) {
                notifyEnd = refreshNotificationActive_;
                refreshNotificationActive_ = false;
            }
        }
    }

    thread->wait();
    delete thread;

    if (!isLast || cancelled || exiting) return;
    if (restart) {
        startLatestRefresh();
        return;
    }

    if (changed) {
        RideItem *current =
            const_cast<RideItem*>(context->currentRideItem());
        if (current) context->notifyRideChanged(current);
    }

    if (notifyEnd) context->notifyRefreshEnd();
    garbageCollect();
    if (changed && saveWorker_) {
        QMetaObject::invokeMethod(
            saveWorker_, [this]() { save(); }, Qt::QueuedConnection);
    }
}

void
RideCache::progressing(int value)
{
    const int total = reverse_.count();
    if (total <= 0) return;

    value = qBound(0, value, total);
    const double nextProgress =
        100.0 * (double(value) / double(total));
    if (nextProgress <= progress_) return;

    progress_ = nextProgress;
    if (value > 0) {
        context->notifyRefreshUpdate(
            reverse_.at(value - 1)->dateTime.date());
    }
}

// cancel the refresh map, we're about to exit !
void
RideCache::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());

    QVector<RideCacheRefreshThread*> current;
    {
        QMutexLocker locker(&updateMutex);
        current = refreshThreads;
        refreshThreads.clear();
        updates = -1;
        isCancelled = true;
        refreshGeneration_.cancel();
    }

    for (RideCacheRefreshThread *worker : current) {
        disconnect(worker, &QThread::finished, nullptr, nullptr);
        worker->requestInterruption();
    }
    for (RideCacheRefreshThread *worker : current) {
        worker->wait();
        delete worker;
    }

    {
        QMutexLocker locker(&updateMutex);
        reverse_.clear();
        refreshChanged_ = false;
        refreshNotificationActive_ = false;
        isCancelled = false;
    }
}

// check if we need to refresh the metrics then start the thread if needed
void
RideCache::refresh()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, &RideCache::refresh, Qt::QueuedConnection);
        return;
    }

    bool active = false;
    {
        QMutexLocker locker(&updateMutex);
        if (exiting || isCancelled) return;
        refreshGeneration_.request();
        active = refreshGeneration_.hasActive();
        if (!active) refreshChanged_ = false;
    }

    if (active) interruptActiveRefresh();
    else startLatestRefresh();
}

void
RideCache::interruptActiveRefresh()
{
    QVector<RideCacheRefreshThread*> current;
    {
        QMutexLocker locker(&updateMutex);
        updates = -1;
        current = refreshThreads;
    }
    for (RideCacheRefreshThread *worker : current) {
        worker->requestInterruption();
    }
}

void
RideCache::startLatestRefresh()
{
    Q_ASSERT(QThread::currentThread() == thread());

    quint64 generation = 0;
    int workerCount = 0;
    bool empty = false;
    bool notifyStart = false;
    {
        QMutexLocker locker(&updateMutex);
        if (exiting || isCancelled
            || refreshGeneration_.hasActive()
            || !refreshGeneration_.hasPending()) {
            return;
        }

        generation = refreshGeneration_.beginLatest();
        reverse_ = rides_;
        std::reverse(reverse_.begin(), reverse_.end());
        updates = 0;
        progress_ = 0;

        empty = reverse_.isEmpty();
        if (empty) {
            refreshGeneration_.finish(generation);
        } else {
            const int capacity = qMax(
                1,
                QThreadPool::globalInstance()->maxThreadCount() / 2);
            workerCount = qMin(capacity, reverse_.count());
            notifyStart = !refreshNotificationActive_;
            refreshNotificationActive_ = true;
        }
    }

    if (empty) {
        QTimer::singleShot(
            5000, this, [this, generation]() {
                bool current = false;
                {
                    QMutexLocker locker(&updateMutex);
                    current = !exiting && !isCancelled
                        && refreshGeneration_.requested() == generation
                        && !refreshGeneration_.hasActive()
                        && !refreshGeneration_.hasPending();
                }
                if (current) context->notifyRefreshEnd();
            });
        return;
    }

    QVector<RideCacheRefreshThread*> workers;
    workers.reserve(workerCount);
    for (int index = 0; index < workerCount; ++index) {
        workers.append(
            new RideCacheRefreshThread(this, generation));
    }
    {
        QMutexLocker locker(&updateMutex);
        refreshThreads += workers;
    }
    for (RideCacheRefreshThread *worker : workers) worker->start();
    if (notifyStart) context->notifyRefreshStart();
}

namespace {

RideCacheAggregate::MetricType aggregateMetricType(
    const RideMetric *metric)
{
    switch (metric->type()) {
    case RideMetric::Total:
        return RideCacheAggregate::MetricType::Total;
    case RideMetric::Peak:
        return RideCacheAggregate::MetricType::Peak;
    case RideMetric::Low:
        return RideCacheAggregate::MetricType::Low;
    case RideMetric::RunningTotal:
        return RideCacheAggregate::MetricType::RunningTotal;
    case RideMetric::MeanSquareRoot:
        return RideCacheAggregate::MetricType::MeanSquareRoot;
    case RideMetric::Average:
    case RideMetric::StdDev:
    default:
        return RideCacheAggregate::MetricType::Average;
    }
}

} // namespace

QVector<QStringList>
RideCache::getAggregates(
    const QStringList &names,
    const QVector<Specification> &specifications,
    bool useMetricUnits,
    bool nofmt)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QVector<const RideMetric*> metrics;
    QVector<RideCacheAggregate::MetricDefinition> definitions;
    metrics.reserve(names.size());
    definitions.reserve(names.size());

    for (const QString &name : names) {
        const RideMetric *metric = factory.rideMetric(name);
        RideCacheAggregate::MetricDefinition definition;
        definition.enabled = metric != nullptr;
        if (metric) {
            definition.type = aggregateMetricType(metric);
            definition.aggregateZero = metric->aggregateZero();
            definition.divideByCount =
                metric->type() == RideMetric::Average;
            definition.excludesValue =
                metric->symbol() == QStringLiteral("average_temp");
            definition.excludedValue = RideFile::NA;
        } else {
            qDebug() << "unknown metric:" << name;
        }
        metrics.append(metric);
        definitions.append(definition);
    }

    const RideCacheAggregate::BatchResult batch =
        RideCacheAggregate::aggregate(
            rides_,
            specifications,
            definitions,
            [](const Specification &specification, RideItem *item) {
                return specification.pass(item);
            },
            [&](qsizetype metric, RideItem *item) {
                return item->getForSymbol(names.at(metric));
            },
            [&](qsizetype metric, RideItem *item) {
                return item->getCountForSymbol(names.at(metric));
            });

    QVector<QStringList> results(specifications.size());
    for (qsizetype specification = 0;
         specification < specifications.size();
         ++specification) {
        QStringList &values = results[specification];
        values.reserve(names.size());
        for (qsizetype metricIndex = 0;
             metricIndex < metrics.size();
             ++metricIndex) {
            const RideMetric *metric = metrics.at(metricIndex);
            if (!metric) {
                values.append(
                    QStringLiteral("%1 unknown").arg(names.at(metricIndex)));
                continue;
            }

            const double value = RideCacheAggregate::finalValue(
                batch.accumulators.at(specification).at(metricIndex),
                definitions.at(metricIndex));
            const_cast<RideMetric*>(metric)->setValue(value);

            QString formatted;
            if (metric->units(useMetricUnits) == QStringLiteral("seconds")
                || metric->units(useMetricUnits) == tr("seconds")) {
                formatted = nofmt
                    ? QStringLiteral("%1").arg(value)
                    : metric->toString(useMetricUnits);
            } else {
                formatted = metric->toString(useMetricUnits);
            }

            if ((metric->symbol() == QStringLiteral("average_temp")
                 || metric->symbol() == QStringLiteral("max_temp"))
                && formatted == QStringLiteral("0.0")) {
                formatted = QStringLiteral("-");
            }
            values.append(formatted);
        }
    }
    return results;
}

QString
RideCache::getAggregate(QString name, Specification spec, bool useMetricUnits, bool nofmt)
{
    const QVector<QStringList> results = getAggregates(
        QStringList{name},
        QVector<Specification>{spec},
        useMetricUnits,
        nofmt);
    return results.value(0).value(0);
}

bool rideCachesummaryBestGreaterThan(const AthleteBest &s1, const AthleteBest &s2)
{
     return s1.nvalue > s2.nvalue;
}

bool rideCachesummaryBestLowerThan(const AthleteBest &s1, const AthleteBest &s2)
{
     return s1.nvalue < s2.nvalue;
}

QList<AthleteBest>
RideCache::getBests(QString symbol, int n, Specification specification, bool useMetricUnits)
{
    QList<AthleteBest> results;

    // get the metric details, so we can convert etc
    const RideMetric *metric = RideMetricFactory::instance().rideMetric(symbol);
    if (!metric) return results;

    // loop through and aggregate
    foreach (RideItem *ride, rides_) {

        // skip filtered rides
        if (!specification.pass(ride)) continue;

        // get this value
        AthleteBest add;
        add.nvalue = ride->getForSymbol(symbol, true);
        add.date = ride->dateTime.date();

        const_cast<RideMetric*>(metric)->setValue(add.nvalue);
        add.value = metric->toString(useMetricUnits);

        // nil values are not needed
        if (add.nvalue < 0 || add.nvalue > 0) results << add;
    }

    // now sort
    std::stable_sort(results.begin(), results.end(), metric->isLowerBetter() ?
                                                rideCachesummaryBestLowerThan :
                                                rideCachesummaryBestGreaterThan);

    // truncate
    if (results.count() > n) results.erase(results.begin()+n,results.end());

    // return the array with the right number of entries in #1 - n order
    return results;
}

QList<QDateTime>
RideCache::getAllDates()
{
    QList<QDateTime> returning;
    foreach(RideItem *item, rides()) {
        returning << item->dateTime;
    }
    return returning;
}

QStringList
RideCache::getAllFilenames()
{
    QStringList returning;
    foreach(RideItem *item, rides()) {
        returning << item->fileName;
    }
    return returning;
}

RideItem *
RideCache::getRide(QString filename)
{
    foreach(RideItem *item, rides())
        if (item->fileName == filename)
            return item;
    return NULL;
}


RideItem*
RideCache::getRide
(const QString &filename, bool planned)
{
    for (RideItem *rideItem : rides()) {
        if (rideItem != nullptr && rideItem->planned == planned && rideItem->fileName == filename) {
            return rideItem;
        }
    }
    return nullptr;
}


RideItem *
RideCache::getRide(QDateTime dateTime)
{
    foreach(RideItem *item, rides())
        if (item->dateTime == dateTime)
            return item;
    return NULL;
}



QHash<QString,int>
RideCache::getRankedValues(QString field)
{
    QHash<QString, int> returning;
    foreach(RideItem *item, rides()) {
        QString value = item->metadata().value(field, "");
        if (value != "") {
            int count = returning.value(value,0);
            returning.insert(value,++count);
        }
    }
    return returning;
}

class OrderedList {
    public:
        OrderedList(QString string, int rank) : string(string), rank(rank) {}
        QString string;
        int rank;
};

bool rideCacheOrderListGreaterThan(const OrderedList a, const OrderedList b) { return a.rank > b.rank; }

QStringList
RideCache::getDistinctValues(QString field)
{
    QStringList returning;

    // ranked
    QHashIterator<QString,int> i(getRankedValues(field));
    QList<OrderedList> ranked;
    while(i.hasNext()) {
        i.next();
        ranked << OrderedList(i.key(), i.value());
    }

    // sort from big to small
    std::sort(ranked.begin(), ranked.end(), rideCacheOrderListGreaterThan);

    // extract ordered values
    foreach(OrderedList x, ranked)
        returning << x.string;

    return returning;
}

void
RideCache::getRideTypeCounts(Specification specification, int& nActivities,
                             int& nRides, int& nRuns, int& nSwims, QString& sport)
{
    nActivities = nRides = nRuns = nSwims = 0;
    sport = "";

    // loop through and aggregate
    foreach (RideItem *ride, rides_) {

        // skip filtered rides
        if (!specification.pass(ride)) continue;

        // sport is not empty only when all activities are from the same sport
        if (nActivities == 0) sport = ride->sport;
        else if (sport != ride-> sport) sport = "";

        nActivities++;
        if (ride->isSwim) nSwims++;
        else if (ride->isRun) nRuns++;
        else if (ride->isBike) nRides++;
    }
}

QVector<bool>
RideCache::areMetricsRelevantForRides(
    const QVector<Specification> &specifications,
    const QVector<const RideMetric*> &metrics,
    SportRestriction sport)
{
    return RideCacheAggregate::metricRelevance(
        rides_,
        specifications,
        metrics.size(),
        [](const Specification &specification, RideItem *ride) {
            return specification.pass(ride);
        },
        [&](qsizetype metricIndex, RideItem *ride) {
            if ((sport == OnlyRides) && !ride->isBike) return false;
            if ((sport == OnlyRuns) && !ride->isRun) return false;
            if ((sport == OnlySwims) && !ride->isSwim) return false;
            if ((sport == OnlyXtrains) && !ride->isXtrain) return false;

            const RideMetric *metric = metrics.at(metricIndex);
            return metric && metric->isRelevantForRide(ride);
        });
}

bool
RideCache::isMetricRelevantForRides(
    Specification specification,
    const RideMetric *metric,
    SportRestriction sport)
{
    return areMetricsRelevantForRides(
        QVector<Specification>{specification},
        QVector<const RideMetric*>{metric},
        sport).value(0);
}


RideCache::OperationPreCheck
RideCache::checkLinkActivities
(RideItem *item1, RideItem *item2)
{
    OperationPreCheck check;

    if (! isValidLink(item1, item2, check.blockingReason)) {
        check.canProceed = false;
        return check;
    }
    if (item1->hasLinkedActivity()) {
        check.canProceed = false;
        check.blockingReason = tr("%1 is already linked to %2").arg(item1->fileName).arg(item1->getLinkedFileName());
        return check;
    }
    if (item2->hasLinkedActivity()) {
        check.canProceed = false;
        check.blockingReason = tr("%1 is already linked to %2").arg(item2->fileName).arg(item2->getLinkedFileName());
        return check;
    }

    check.affectedItems << item1 << item2;
    if (item1->isDirty()) {
        check.dirtyItems << item1;
    }
    if (item2->isDirty()) {
        check.dirtyItems << item2;
    }
    if (! check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *item : check.dirtyItems) {
            dirtyNames << item->fileName;
        }
        check.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Linking will modify both activities. You must save or discard changes first.")
            .arg(dirtyNames.join("\n"));
    }

    return check;
}


RideCache::OperationResult
RideCache::linkActivities
(RideItem *item1, RideItem *item2)
{
    OperationResult result;

    item1->setLinkedFileName(item2->fileName);
    item2->setLinkedFileName(item1->fileName);

    result.success = true;
    result.affectedCount = 2;

    emit itemChanged(item1);
    emit itemChanged(item2);

    return result;
}


RideCache::OperationPreCheck
RideCache::checkUnlinkActivity
(RideItem *item)
{
    OperationPreCheck check;

    if (! item) {
        check.canProceed = false;
        check.blockingReason = tr("No activity given");
        return check;
    }
    QString linkedFileName = item->getLinkedFileName();
    if (linkedFileName.isEmpty()) {
        check.canProceed = false;
        check.blockingReason = tr("Activity is not linked");
        return check;
    }
    RideItem *linkedItem = getLinkedActivity(item);
    if (! linkedItem) {
        check.canProceed = false;
        check.blockingReason = tr("Linked activity not found: %1").arg(linkedFileName);
        return check;
    }

    check.affectedItems << item << linkedItem;
    if (item->isDirty()) {
        check.dirtyItems << item;
    }
    if (linkedItem->isDirty()) {
        check.dirtyItems << linkedItem;
    }
    if (! check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *item : check.dirtyItems) {
            dirtyNames << item->fileName;
        }
        check.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Unlinking will modify both activities. You must save or discard changes first.")
            .arg(dirtyNames.join("\n"));
    }

    return check;
}


RideCache::OperationResult
RideCache::unlinkActivity
(RideItem *item)
{
    OperationResult result;

    RideItem *linkedItem = getLinkedActivity(item);

    linkedItem->clearLinkedFileName();
    item->clearLinkedFileName();

    result.success = true;
    result.affectedCount = 2;

    emit itemChanged(item);
    emit itemChanged(linkedItem);

    return result;
}


RideCache::OperationPreCheck
RideCache::checkUnlinkActivities
(const QList<RideItem*> &items)
{
    OperationPreCheck batchCheck;

    if (items.isEmpty()) {
        batchCheck.canProceed = false;
        batchCheck.blockingReason = tr("No activities given");
        return batchCheck;
    }

    QSet<RideItem*> processedItems;
    for (RideItem *item : items) {
        if (! item || processedItems.contains(item)) {
            continue;
        }
        OperationPreCheck itemCheck = checkUnlinkActivity(item);
        if (! itemCheck.canProceed) {
            continue;
        }
        batchCheck.affectedItems.append(itemCheck.affectedItems);
        batchCheck.dirtyItems.append(itemCheck.dirtyItems);
        for (RideItem *affectedItem : itemCheck.affectedItems) {
            processedItems.insert(affectedItem);
        }
    }
    if (batchCheck.affectedItems.isEmpty()) {
        batchCheck.canProceed = false;
        batchCheck.blockingReason = tr("No valid linked activities to unlink");
        return batchCheck;
    }
    if (! batchCheck.dirtyItems.isEmpty()) {
        batchCheck.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *item : batchCheck.dirtyItems) {
            dirtyNames << item->fileName;
        }
        batchCheck.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Unlinking will modify these activities. You must save or discard changes first.")
            .arg(dirtyNames.join("\n"));
    }

    return batchCheck;
}


RideCache::OperationResult
RideCache::unlinkActivities
(const QList<RideItem*> &items)
{
    OperationResult batchResult;
    QSet<RideItem*> processedItems;

    for (RideItem *item : items) {
        if (! item || processedItems.contains(item)) {
            continue;
        }
        RideItem *linkedItem = getLinkedActivity(item);
        if (! linkedItem) {
            continue;
        }
        if (processedItems.contains(linkedItem)) {
            continue;
        }
        OperationResult itemResult = unlinkActivity(item);
        if (itemResult.success) {
            batchResult.affectedCount += itemResult.affectedCount;
            processedItems.insert(item);
            processedItems.insert(linkedItem);
        }
    }
    batchResult.success = (batchResult.affectedCount > 0);
    return batchResult;
}


RideCache::OperationPreCheck
RideCache::checkMoveActivity
(RideItem *item, const QDateTime &newDateTime)
{
    OperationPreCheck check;

    if (! item) {
        check.canProceed = false;
        check.blockingReason = tr("No activity given");
        return check;
    }
    if (! newDateTime.isValid()) {
        check.canProceed = false;
        check.blockingReason = tr("Invalid date/time specified");
        return check;
    }

    QFileInfo oldInfo(item->fileName);
    QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();
    QString newPath = (item->planned ? plannedDirectory : directory).canonicalPath() + "/" + newFileName;
    if (QFile::exists(newPath)) {
        check.canProceed = false;
        check.blockingReason = tr("Target file already exists: %1").arg(newFileName);
        return check;
    }
    check.affectedItems << item;
    if (item->isDirty()) {
        check.dirtyItems << item;
    }

    RideItem *linkedItem = getLinkedActivity(item);
    if (linkedItem) {
        check.affectedItems << linkedItem;
        if (linkedItem->isDirty()) {
            check.dirtyItems << linkedItem;
        }
    }
    if (! check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;
        QStringList dirtyNames;
        for (RideItem *dirtyItem : check.dirtyItems) {
            dirtyNames << dirtyItem->fileName;
        }
        check.warningMessage = tr(
            "The following activities have unsaved changes:\n%1\n\n"
            "Moving will update the link reference. You must save or discard changes first.")
            .arg(dirtyNames.join("\n"));
    }
    return check;
}


RideCache::OperationResult
RideCache::moveActivity
(RideItem *item, const QDateTime &newDateTime)
{
    OperationResult result;

    QString oldFileName = item->fileName;
    QDateTime oldDateTime = item->dateTime;

    QFileInfo oldInfo(oldFileName);
    QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();

    RideFile *ride = item->ride(true);
    if (! ride) {
        result.error = tr("Failed to open activity file");
        return result;
    }

    QDate originalDate = QDate::fromString(ride->getTag("Original Date", ""), "yyyy/MM/dd");
    if (! originalDate.isValid()) {
        ride->setTag("Original Date", oldDateTime.date().toString("yyyy/MM/dd"));
    }
    item->setStartTime(newDateTime);
    ride->setTag("Year", newDateTime.toString("yyyy"));
    ride->setTag("Month", newDateTime.toString("MMMM"));
    ride->setTag("Weekday", newDateTime.toString("ddd"));
    ride->setTag("Filename", newFileName);
    item->metadata_.insert("Calendar Text", GlobalContext::context()->rideMetadata->calendarText(item));

    QString renameError;
    if (! renameRideFiles(oldFileName, newFileName, item->planned, renameError)) {
        item->dateTime = oldDateTime;
        item->fileName = oldFileName;
        result.error = tr("Failed to rename files: %1").arg(renameError);
        item->close();
        return result;
    }

    QString newPath = (item->planned ? plannedDirectory : directory).canonicalPath() + "/" + newFileName;
    QFile outFile(newPath);
    if (! RideFileFactory::instance().writeRideFile(context, ride, outFile, QFileInfo(newFileName).suffix())) {
        renameRideFiles(newFileName, oldFileName, item->planned, renameError);
        item->dateTime = oldDateTime;
        item->fileName = oldFileName;
        result.error = tr("Failed to save activity file after rename");
        item->close();
        return result;
    }
    item->close();

    int index = rides_.indexOf(item);
    if (index >= 0) {
        model_->startRemove(index);
        rides_.remove(index, 1);
        model_->endRemove(index);
    }

    item->setFileName((item->planned ? plannedDirectory : directory).canonicalPath(), newFileName);

    model_->beginReset();
    rides_ << item;
    std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
    model_->endReset();

    item->isstale = true;

    RideItem *linkedItem = getLinkedActivity(item);
    if (linkedItem) {
        linkedItem->setLinkedFileName(newFileName);
        emit itemChanged(linkedItem);
        result.affectedCount = 2;
    } else {
        result.affectedCount = 1;
    }

    if (item->planned) {
        updateFromWorkout(item, false);
    }

    item->refresh();
    context->notifyRideChanged(item);
    if (context->ride == item) {
        context->notifyRideSelected(item);
    }
    refresh();
    estimator->refresh();

    result.success = true;

    return result;
}


RideCache::OperationPreCheck
RideCache::checkCopyPlannedActivity
(RideItem *sourceItem, const QDate &newDate, QTime newTime)
{
    OperationPreCheck check;

    if (! sourceItem) {
        check.canProceed = false;
        check.blockingReason = tr("No activity given");
        return check;
    }
    if (! newDate.isValid()) {
        check.canProceed = false;
        check.blockingReason = tr("Invalid date specified");
        return check;
    }
    QTime time(sourceItem->dateTime.time());
    if (newTime.isValid()) {
        time = newTime;
    }

    QDateTime newDateTime(newDate, time);
    QFileInfo oldInfo(sourceItem->fileName);
    QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();
    QString newPath = plannedDirectory.canonicalPath() + "/" + newFileName;
    if (QFile::exists(newPath)) {
        check.canProceed = false;
        check.blockingReason = tr("Target file already exists: %1").arg(newFileName);
        return check;
    }

    return check;
}


RideCache::OperationResult
RideCache::copyPlannedActivity
(RideItem *sourceItem, const QDate &newDate, QTime newTime)
{
    OperationResult result;

    QString error;
    QTime time(sourceItem->dateTime.time());
    if (newTime.isValid()) {
        time = newTime;
    }
    RideItem *newItem = copyPlannedRideFile(sourceItem, newDate, time, error);

    if (! newItem) {
        result.error = error;
        return result;
    }

    model_->beginReset();
    rides_ << newItem;
    std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
    model_->endReset();

    refresh();
    estimator->refresh();

    result.success = true;
    result.affectedCount = 1;

    return result;
}


RideCache::OperationPreCheck
RideCache::checkCopyPlannedActivities
(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets)
{
    OperationPreCheck check;

    if (sourceItemsAndTargets.isEmpty()) {
        check.canProceed = false;
        check.blockingReason = tr("No items specified");
        return check;
    }

    for (const std::pair<RideItem*, QDate> &pair : sourceItemsAndTargets) {
        RideItem *sourceItem = pair.first;
        QDate targetDate = pair.second;

        if (! sourceItem) {
            check.canProceed = false;
            check.blockingReason = tr("Invalid source item");
            return check;
        }
        if (! sourceItem->planned) {
            check.canProceed = false;
            check.blockingReason = tr("Source item is not a planned activity: %1").arg(sourceItem->fileName);
            return check;
        }
        if (! targetDate.isValid()) {
            check.canProceed = false;
            check.blockingReason = tr("Invalid target date for: %1").arg(sourceItem->fileName);
            return check;
        }

        QDateTime newDateTime(targetDate, sourceItem->dateTime.time());
        QFileInfo oldInfo(sourceItem->fileName);
        QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();
        QString newPath = plannedDirectory.canonicalPath() + "/" + newFileName;

        if (QFile::exists(newPath)) {
            check.canProceed = false;
            check.blockingReason = tr("Target file already exists: %1").arg(newFileName);
            return check;
        }
    }

    return check;
}


RideCache::OperationResult
RideCache::copyPlannedActivities
(const QList<std::pair<RideItem*, QDate>> &sourceItemsAndTargets)
{
    OperationResult result;

    if (sourceItemsAndTargets.isEmpty()) {
        result.error = tr("No files specified");
        return result;
    }

    QList<RideItem*> newItems;
    QStringList failedFiles;
    for (const std::pair<RideItem*, QDate> &pair : sourceItemsAndTargets) {
        QString error;
        RideItem *newItem = copyPlannedRideFile(pair.first, pair.second, QTime(), error);
        if (newItem) {
            newItems << newItem;
        } else {
            failedFiles << pair.first->fileName;
        }
    }

    if (! newItems.isEmpty()) {
        model_->beginReset();
        rides_ << newItems;
        std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
        model_->endReset();
        refresh();
        estimator->refresh();
    }
    if (! failedFiles.isEmpty()) {
        result.error = tr("Failed to copy %1 of %2 activities: %3")
                         .arg(failedFiles.count())
                         .arg(sourceItemsAndTargets.count())
                         .arg(failedFiles.join(", "));
    }

    result.success = !newItems.isEmpty();
    result.affectedCount = newItems.count();

    return result;
}


RideCache::OperationPreCheck
RideCache::checkShiftPlannedActivities
(const QDate &fromDate, int dayOffset)
{
    OperationPreCheck check;

    if (! fromDate.isValid()) {
        check.canProceed = false;
        check.blockingReason = tr("Invalid from date specified");
        return check;
    }
    if (dayOffset == 0) {
        check.canProceed = true;
        return check;
    }

    QList<RideItem*> itemsToShift;
    for (RideItem *item : rides_) {
        if (item->planned && item->dateTime.date() >= fromDate) {
            itemsToShift.append(item);
            check.affectedItems << item;
        }
    }
    if (itemsToShift.isEmpty()) {
        check.canProceed = true;
        return check;
    }

    for (RideItem *item : itemsToShift) {
        RideItem *linkedItem = getLinkedActivity(item);
        if (linkedItem && ! linkedItem->planned) {
            check.affectedItems << linkedItem;
        }
    }
    for (RideItem *item : check.affectedItems) {
        if (item->isDirty()) {
            check.dirtyItems << item;
        }
    }

    if (! check.dirtyItems.isEmpty()) {
        check.requiresUserDecision = true;

        QStringList plannedDirty;
        QStringList actualDirty;
        for (RideItem *item : check.dirtyItems) {
            if (item->planned) {
                plannedDirty << item->fileName;
            } else {
                actualDirty << item->fileName;
            }
        }
        QString msg = tr("This operation will shift %1 planned activities.\n\n").arg(itemsToShift.count());
        if (! plannedDirty.isEmpty()) {
            msg += tr("Planned activities with unsaved changes:\n%1\n\n").arg(plannedDirty.join("\n"));
        }
        if (! actualDirty.isEmpty()) {
            msg += tr("Linked actual activities with unsaved changes:\n%1\n\n").arg(actualDirty.join("\n"));
        }

        msg += tr("All affected activities must be saved or changes discarded before shifting.");
        check.warningMessage = msg;
    }

    return check;
}


RideCache::OperationResult
RideCache::shiftPlannedActivities
(const QDate &fromDate, int dayOffset)
{
    OperationResult result;

    if (dayOffset == 0) {
        result.success = true;
        result.affectedCount = 0;
        return result;
    }
    QList<RideItem*> itemsToShift;
    for (RideItem *item : rides_) {
        if (item->planned && item->dateTime.date() >= fromDate) {
            itemsToShift.append(item);
        }
    }
    if (itemsToShift.isEmpty()) {
        result.success = true;
        result.affectedCount = 0;
        return result;
    }

    // prevent shifting any activity to before fromDate
    int effectiveOffset = dayOffset;
    if (dayOffset < 0) {
        QDate earliestDate = itemsToShift[0]->dateTime.date();
        for (RideItem *item : itemsToShift) {
            if (item->dateTime.date() < earliestDate) {
                earliestDate = item->dateTime.date();
            }
        }
        int maxBackwardShift = fromDate.daysTo(earliestDate);
        if (-dayOffset > maxBackwardShift) {
            effectiveOffset = -maxBackwardShift;
        }
        if (effectiveOffset == 0) {
            result.success = true;
            result.affectedCount = 0;
            return result;
        }
    }

    // avoid filename collisions: copy forward / backward, depending on offset
    if (effectiveOffset > 0) {
        std::sort(itemsToShift.begin(), itemsToShift.end(), [](RideItem *a, RideItem *b) { return a->dateTime > b->dateTime; });
    } else {
        std::sort(itemsToShift.begin(), itemsToShift.end(), [](RideItem *a, RideItem *b) { return a->dateTime < b->dateTime; });
    }

    QStringList failedFiles;
    int successCount = 0;
    for (RideItem *item : itemsToShift) {
        QString oldFileName = item->fileName;
        QDate newDate = item->dateTime.date().addDays(effectiveOffset);
        QDateTime newDateTime(newDate, item->dateTime.time());

        QFileInfo oldInfo(oldFileName);
        QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();

        RideFile *ride = item->ride(true);
        if (! ride) {
            failedFiles << oldFileName;
            continue;
        }

        QDate originalDate = QDate::fromString(ride->getTag("Original Date", ""), "yyyy/MM/dd");
        if (! originalDate.isValid()) {
            ride->setTag("Original Date", item->dateTime.date().toString("yyyy/MM/dd"));
        }
        item->setStartTime(newDateTime);
        ride->setTag("Year", newDateTime.toString("yyyy"));
        ride->setTag("Month", newDateTime.toString("MMMM"));
        ride->setTag("Weekday", newDateTime.toString("ddd"));
        ride->setTag("Filename", newFileName);
        item->metadata_.insert("Calendar Text", GlobalContext::context()->rideMetadata->calendarText(item));

        QString renameError;
        if (! renameRideFiles(oldFileName, newFileName, true, renameError)) {
            failedFiles << oldFileName;
            item->close();
            continue;
        }

        QString newPath = plannedDirectory.canonicalPath() + "/" + newFileName;
        QFile outFile(newPath);
        if (! RideFileFactory::instance().writeRideFile(context, ride, outFile, QFileInfo(newFileName).suffix())) {
            renameRideFiles(newFileName, oldFileName, true, renameError);
            failedFiles << oldFileName;
            item->close();
            continue;
        }
        item->close();
        item->setFileName(plannedDirectory.canonicalPath(), newFileName);
        updateFromWorkout(item, true);
        item->isstale = true;

        RideItem *linkedItem = getLinkedActivity(item);
        if (linkedItem) {
            linkedItem->setLinkedFileName(item->fileName);
            emit itemChanged(linkedItem);
        }

        successCount++;
    }

    if (successCount > 0) {
        model_->beginReset();
        std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
        model_->endReset();

        refresh();
        estimator->refresh();
    }

    if (! failedFiles.isEmpty()) {
        result.error = tr("Failed to shift %1 of %2 activities: %3")
                         .arg(failedFiles.count())
                         .arg(itemsToShift.count())
                         .arg(failedFiles.join(", "));
    }

    result.success = true;
    result.affectedCount = successCount;

    return result;
}


bool
RideCache::saveActivity
(RideItem *item, QString &error)
{
    return RideCache::saveActivity(
        context, item, error,
        [](Context *saveContext, RideItem *saveItem, QString *saveError) {
            return MainWindow::saveSilent(
                saveContext, saveItem, saveError);
        },
        [this](RideItem *savedItem) { emit itemSaved(savedItem); });
}


bool
RideCache::saveActivity
(Context *context, RideItem *item, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved)
{
    error.clear();
    if (!item) {
        error = QObject::tr("No activity given");
        return false;
    }
    if (!item->isDirty()) {
        return true;
    }
    if (!save) {
        error = QObject::tr("No activity save operation available");
        return false;
    }
    if (!save(context, item, &error)) {
        if (error.isEmpty()) {
            error = QObject::tr("The activity could not be saved");
        }
        return false;
    }
    if (notifySaved) {
        notifySaved(item);
    }
    return true;
}


bool
RideCache::saveActivities
(QList<RideItem*> items, QString &error)
{
    return RideCache::saveActivities(
        context, items, error,
        [](Context *saveContext, RideItem *saveItem, QString *saveError) {
            return MainWindow::saveSilent(
                saveContext, saveItem, saveError);
        },
        [this](RideItem *savedItem) { emit itemSaved(savedItem); });
}


bool
RideCache::saveActivities
(Context *context, const QList<RideItem *> &items, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved)
{
    error.clear();
    QStringList failed;

    for (RideItem *item : items) {
        QString itemError;
        if (!RideCache::saveActivity(
                context, item, itemError, save, notifySaved)) {
            const QString fileName =
                item ? item->fileName : QObject::tr("<unknown activity>");
            if (itemError.isEmpty()) {
                failed << fileName;
            } else {
                failed << QStringLiteral("%1 (%2)").arg(fileName, itemError);
            }
        }
    }
    if (!failed.isEmpty()) {
        error = QObject::tr("Failed to save: %1")
                    .arg(failed.join(QStringLiteral(", ")));
        return false;
    }

    return true;
}


bool
RideCache::renameRideFiles
(const QString &oldFileName, const QString &newFileName, bool isPlanned, QString &error)
{
    QFileInfo oldInfo(oldFileName);
    QFileInfo newInfo(newFileName);

    QDir activeDir = isPlanned ? plannedDirectory : directory;

    QString oldPath = activeDir.canonicalPath() + "/" + oldFileName;
    QString newPath = activeDir.canonicalPath() + "/" + newFileName;

    if (! QFile::rename(oldPath, newPath)) {
        error = tr("Failed to rename activity file from %1 to %2").arg(oldFileName).arg(newFileName);
        return false;
    }

    QStringList extensions;
    extensions << "notes" << "cpi" << "cpx";
    for (const QString &ext : extensions) {
        QString oldExtPath = context->athlete->home->cache().canonicalPath() + "/" + oldInfo.baseName() + "." + ext;
        QString newExtPath = context->athlete->home->cache().canonicalPath() + "/" + newInfo.baseName() + "." + ext;
        if (QFile::exists(oldExtPath)) {
            QFile::rename(oldExtPath, newExtPath);
        }
    }

    return true;
}


RideItem*
RideCache::getLinkedActivity
(RideItem *item)
{
    if (! item) {
        return nullptr;
    }
    QString linkedFileName = item->getLinkedFileName();
    if (linkedFileName.isEmpty()) {
        return nullptr;
    }
    return getRide(linkedFileName, ! item->planned);
}


RideItem*
RideCache::findSuggestion
(RideItem *rideItem)
{
    RideItem *closest = nullptr;
    for (RideItem *o: this->context->athlete->rideCache->rides()) {
        if (   o != nullptr
            && o->planned == ! rideItem->planned
            && o->dateTime.date() == rideItem->dateTime.date()
            && o->sport == rideItem->sport) {
            if (closest == nullptr) {
                closest = o;
            } else if (std::abs(rideItem->dateTime.time().secsTo(o->dateTime.time())) < std::abs(rideItem->dateTime.time().secsTo(closest->dateTime.time()))) {
                closest = o;
            }
        }
        if (o->dateTime.date() > rideItem->dateTime.date()) {
            break;
        }
    }
    return closest;
}


bool
RideCache::updateFromWorkout
(RideItem *item, bool autoSave)
{
    if (item == nullptr || ! item->planned) {
        return false;
    }
    QString workoutFilename = item->getText("WorkoutFilename", item->ride()->getTag("WorkoutFilename", "")).trimmed();
    if (workoutFilename.isEmpty()) {
        return false;
    }
    ErgFile ergFile(workoutFilename, ErgFileFormat::unknown, context, item->dateTime.date());
    if (! ergFile.hasRelativeWatts()) {
        return false;
    }
    bool changed = false;
    for (const QString &name : item->overrides_) {
        int value = static_cast<int>(item->getForSymbol(name));
        // Operate only on the values overridden by ManualActivityWizard
        if (name == "average_power") {
            if (value != std::round(ergFile.AP())) {
                QMap<QString, QString> values;
                values.insert("value", QString::number(std::round(ergFile.AP())));
                item->ride()->metricOverrides.insert(name, values);
                changed = true;
            }
        } else if (name == "coggan_np") {
            if (value != std::round(ergFile.IsoPower())) {
                QMap<QString, QString> values;
                values.insert("value", QString::number(std::round(ergFile.IsoPower())));
                item->ride()->metricOverrides.insert(name, values);
                changed = true;
            }
        } else if (name == "coggan_tss") {
            if (value != std::round(ergFile.bikeStress())) {
                QMap<QString, QString> values;
                values.insert("value", QString::number(std::round(ergFile.bikeStress())));
                item->ride()->metricOverrides.insert(name, values);
                changed = true;
            }
        } else if (name == "skiba_bike_score") {
            if (value != std::round(ergFile.BS())) {
                QMap<QString, QString> values;
                values.insert("value", QString::number(std::round(ergFile.BS())));
                item->ride()->metricOverrides.insert(name, values);
                changed = true;
            }
        } else if (name == "skiba_xpower") {
            if (value != std::round(ergFile.XP())) {
                QMap<QString, QString> values;
                values.insert("value", QString::number(std::round(ergFile.XP())));
                item->ride()->metricOverrides.insert(name, values);
                changed = true;
            }
        }
    }
    if (changed) {
        item->setDirty(true);
        item->isstale = true;
        if (autoSave) {
            QString error;
            saveActivity(item, error);
        }
    }
    return changed;
}


bool
RideCache::updateFromWorkoutAfter
(const QDate &when, bool autoSave)
{
    cancel();

    QList<RideItem*> changedItems;
    for (RideItem *item : rides()) {
        if (   item
            && item->planned
            && item->dateTime.date() >= when) {
            if (updateFromWorkout(item, false)) {
                changedItems << item;
            }
        }
    }

    if (! changedItems.isEmpty()) {
        if (autoSave) {
            QString error;
            saveActivities(changedItems, error);
        }
        refresh();
        estimator->refresh();
    }
    return ! changedItems.isEmpty();
}


bool
RideCache::isValidLink
(RideItem *item1, RideItem *item2, QString &error)
{
    error = "";
    if (! item1 || ! item2) {
        error = tr("Invalid activities for linking");
        return false;
    }
    if (item1 == item2) {
        error = tr("Can't link to self");
        return false;
    }
    if (item1->planned == item2->planned) {
        error = tr("Cannot link two activities of the same type. One must be planned, one actual.");
        return false;
    }
    return true;
}


RideItem*
RideCache::copyPlannedRideFile
(RideItem *sourceItem, const QDate &newDate, const QTime &newTime, QString &error)
{
    QDateTime newDateTime(newDate, newTime);
    QFileInfo oldInfo(sourceItem->fileName);
    QString newFileName = newDateTime.toString("yyyy_MM_dd_HH_mm_ss") + "." + oldInfo.suffix();
    QString newPath = plannedDirectory.canonicalPath() + "/" + newFileName;
    QString sourcePath = plannedDirectory.canonicalPath() + "/" + sourceItem->fileName;

    if (! QFile::copy(sourcePath, newPath)) {
        error = tr("Failed to copy file");
        return nullptr;
    }

    QFile file(newPath);
    QStringList errors;
    RideFile *newRide = RideFileFactory::instance().openRideFile(context, file, errors);
    if (! newRide) {
        QFile::remove(newPath);
        error = tr("Failed to open copied file");
        return nullptr;
    }

    newRide->setStartTime(QDateTime(newDate, sourceItem->dateTime.time()));
    newRide->setTag("Year", newDateTime.toString("yyyy"));
    newRide->setTag("Month", newDateTime.toString("MMMM"));
    newRide->setTag("Weekday", newDateTime.toString("ddd"));
    newRide->setTag("Original Date", newDateTime.date().toString("yyyy/MM/dd"));

    if (! newRide->getTag("Linked Filename", "").isEmpty()) {
        newRide->removeTag("Linked Filename");
    }

    QFile outFile(newPath);
    if (! RideFileFactory::instance().writeRideFile(context, newRide, outFile, oldInfo.suffix())) {
        error = tr("Failed to write modified file");
        delete newRide;
        QFile::remove(newPath);
        return nullptr;
    }
    delete newRide;

    RideItem *newItem = new RideItem(plannedDirectory.canonicalPath(), newFileName, newDateTime, context, true);
    updateFromWorkout(newItem, true);
    newItem->isstale = true;

    return newItem;
}


// refresh metrics
void RideCacheRefreshThread::run()
{
    while (!isInterruptionRequested()) {
        RideCache *target = cache.data();
        if (!target) return;

        const int index = target->nextRefresh(generation);
        if (index < 0 || isInterruptionRequested()) return;

        RideItem *item = nullptr;
        {
            QMutexLocker locker(&target->updateMutex);
            if (target->refreshGeneration_.accepts(generation)
                && index < target->reverse_.count()) {
                item = target->reverse_.at(index);
            }
        }

        if (item && item->checkStale()) {
            item->refresh();
            QMutexLocker locker(&target->updateMutex);
            target->refreshChanged_ = true;
        }
    }
}
