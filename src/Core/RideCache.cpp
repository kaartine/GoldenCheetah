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
#include "RideCacheBackgroundSaver.h"
#include "RideCacheBulkMerge.h"
#include "RideCacheCallbackGuard.h"
#include "RideCacheSaveCapture.h"
#include "RideCacheSaveSnapshot.h"
#include "RideCacheSnapshot.h"
#include "SaveDialogs.h"
#include "LinkedActivityRemovalJournal.h"
#include "LinkedActivitySaveJournal.h"
#include "PlanReplacementJournal.h"
#include "PlannedActivityFileStager.h"

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
#include <QPointer>
#include <QTemporaryFile>

#include <exception>

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

    if (!LinkedActivityRemoval::Journal::reconcileAll(
            context->athlete->home->root().absolutePath(),
            startupRecoveryError_)) {
        qCritical().noquote()
            << "Activity recovery must be completed before loading:"
            << startupRecoveryError_;
        return;
    }
    if (!LinkedActivitySave::Journal::reconcileAll(
            context->athlete->home->root().absolutePath(),
            startupRecoveryError_)) {
        qCritical().noquote()
            << "Activity recovery must be completed before loading:"
            << startupRecoveryError_;
        return;
    }
    if (!PlanReplacement::Journal::reconcileAll(
            context->athlete->home->root().absolutePath(),
            startupRecoveryError_)) {
        qCritical().noquote()
            << "Activity recovery must be completed before loading:"
            << startupRecoveryError_;
        return;
    }

    backgroundSaver_ =
        std::make_shared<RideCacheBackgroundSaver>();

    startupLoader_ = new RideCacheLoader(this);
    startupLoader_->start();
}

void
RideCache::queueBackgroundSave(const QString &requestedTargetPath)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!backgroundSaver_) return;

    const QString targetPath = requestedTargetPath.isEmpty()
        ? context->athlete->home->cache().filePath(
              QStringLiteral("rideDB.json"))
        : requestedTargetPath;
    {
        QMutexLocker locker(&updateMutex);
        if (RideCacheSave::deferTarget(
                pendingSaveTargets_,
                !refreshThreads.isEmpty()
                    || refreshGeneration_.hasActive()
                    || saveSnapshotBoundary_,
                targetPath)) {
            return;
        }
    }

    const std::shared_ptr<const RideCacheSave::Snapshot> snapshot =
        captureSaveSnapshot(targetPath);
    enqueueSaveSnapshot(snapshot);
}

bool
RideCache::settleRefreshForSave(QString &error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return RideCacheSave::settleRefreshBarrier(
        [this]() {
            RideCacheSave::RefreshBarrierState state;
            QMutexLocker locker(&updateMutex);
            state.workers.reserve(refreshThreads.size());
            for (RideCacheRefreshThread *worker : refreshThreads) {
                state.workers.append(worker);
            }
            state.hasActiveGeneration =
                refreshGeneration_.hasActive();
            state.hasPendingGeneration =
                refreshGeneration_.hasPending();
            state.stableCaptureBoundary =
                saveSnapshotBoundary_;
            return state;
        },
        [this](QThread *thread) {
            auto *worker =
                static_cast<RideCacheRefreshThread*>(thread);
            threadCompleted(worker, worker->generation);
        },
        [this]() {
            startLatestRefresh();
        },
        error);
}

bool
RideCache::settleForOpenDataSnapshot(QString &error)
{
    error.clear();
    if (QThread::currentThread() != thread()) {
        error = QStringLiteral(
            "OpenData snapshots must be settled on the owner thread");
        return false;
    }
    return settleRefreshForSave(error);
}

bool
RideCache::enqueueSaveSnapshot(
    const std::shared_ptr<const RideCacheSave::Snapshot> &snapshot)
{
    if (!backgroundSaver_ || !snapshot) return false;
    const bool queued = backgroundSaver_->enqueue(snapshot);
    if (!queued) {
        qWarning() << "Cannot queue ride cache snapshot save";
    }
    return queued;
}

void
RideCache::appendStartupFiles(
    const std::shared_ptr<
        QVector<RideCacheStartup::IndexedFile>> &files)
{
    if (!files || files->isEmpty()) return;

    const int firstRow = rides_.size();
    if (!model_->startInsert(
            firstRow, firstRow + files->size() - 1)) {
        const QPointer<RideCache> guardedThis(this);
        QMetaObject::invokeMethod(
            this,
            [guardedThis, files] {
                if (guardedThis)
                    guardedThis->appendStartupFiles(files);
            },
            Qt::QueuedConnection);
        return;
    }
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
    if (!dt || deletelist.contains(dt)) return -1;

    if (!deletelist.isEmpty()) {
        for (qsizetype index = 0; index < rides_.size(); ++index) {
            RideItem *item = rides_.at(index);
            if (item && !deletelist.contains(item)
                && item->dateTime == dt->dateTime) {
                return index;
            }
        }
        return -1;
    }

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

    if (backgroundSaver_) {
        QString error;
        if (backgroundSaver_->isRunning()
            && !backgroundSaver_->drain(&error)) {
            qWarning().noquote() << error;
        }
        backgroundSaver_->stop();
        backgroundSaver_.reset();
    }

    // Preserve the previous complete cache if startup was interrupted.
    if (startupLoadFinished_) save();
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
        for (RideItem *item : rides()) {
            if (item->isOpen()) item->ride()->wstale = true;
        }
    }

    if (plan.rebuildCalendarText) {
        for (RideItem *item : rides()) {
            item->metadata_.insert(
                QStringLiteral("Calendar Text"),
                GlobalContext::context()->rideMetadata->calendarText(item));
        }
    }

    if (plan.recolor) {
        for (RideItem *item : rides()) {
            item->color = GlobalContext::context()->colorEngine->colorFor(
                item->getText(
                    GlobalContext::context()->rideMetadata->getColorField(),
                    QString()));
        }
    }

    if (plan.refreshMetrics) refresh();
}

bool
RideCache::activityMutationIsBlocked() const
{
    return QThread::currentThread() != thread()
        || (removalInProgress_ && *removalInProgress_)
        || (model_ && !model_->cacheMutationAllowed());
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
    if (activityMutationIsBlocked()) {
        qWarning()
            << "Cannot add an activity while another activity operation is in progress";
        return;
    }
    QPointer<RideItem> prior(context->ride);

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
        RideItem *const current = rides_.at(index);
        if (current && !deletelist.contains(current)
            && current->fileName == last->fileName) {
            invalidateStartupSnapshots();
            rides_[index] = last;
            added = true;
            break;
        }
    }

    // add and sort, model needs to know !
    if (!added) {
        if (!model_->beginReset()) {
            delete last;
            return;
        }
        rides_ << last;
        std::sort(rides_.begin(), rides_.end(), rideCacheLessThan);
        model_->endReset();
    }

    // refresh metrics for *this ride only*
    last->refresh();

    const RideCacheCallbackGuard callbackGuard(
        this, context, last, estimator);
    if (dosignal) {
        context->notifyRideAdded(last); // emitted before rideSelected
        if (!callbackGuard.allAlive()) return;
    }

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
        context->notifyRideSelected(prior.data());
    }

    if (!callbackGuard.allAlive()) return;

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
    if (activityMutationIsBlocked()) {
        qWarning()
            << "Cannot add activities while another activity operation is in progress";
        qDeleteAll(preparedRides);
        return {};
    }
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

    bool mergeStarted = false;
    const QVector<RideItem*> replaced =
        RideCacheBulkMerge::mergeItems(
            rides_,
            incoming,
            [](const RideItem *item) { return item->fileName; },
            rideCacheLessThan,
            [this, &mergeStarted]() {
                mergeStarted = model_->beginReset();
                return mergeStarted;
            },
            [this]() { model_->endReset(); });
    if (!mergeStarted) {
        qDeleteAll(incoming);
        return {};
    }
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
    QStringList pendingSaveTargets;
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
                if (!cancelled && !exiting) {
                    saveSnapshotBoundary_ = true;
                }
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

    garbageCollect();
    if (changed) {
        RideItem *current =
            const_cast<RideItem*>(context->currentRideItem());
        if (current) context->notifyRideChanged(current);
    }

    if (notifyEnd) context->notifyRefreshEnd();

    {
        QMutexLocker locker(&updateMutex);
        pendingSaveTargets =
            RideCacheSave::takeDeferredTargets(
                pendingSaveTargets_);
    }
    const QString defaultTarget =
        context->athlete->home->cache().filePath(
            QStringLiteral("rideDB.json"));
    if (changed
        && !pendingSaveTargets.contains(defaultTarget)) {
        pendingSaveTargets.append(defaultTarget);
    }
    for (const QString &targetPath : pendingSaveTargets) {
        enqueueSaveSnapshot(captureSaveSnapshot(targetPath));
    }

    bool startDeferredRefresh = false;
    {
        QMutexLocker locker(&updateMutex);
        saveSnapshotBoundary_ = false;
        startDeferredRefresh =
            refreshGeneration_.hasPending()
            && !refreshGeneration_.hasActive();
    }
    if (startDeferredRefresh) startLatestRefresh();
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
    QStringList pendingSaveTargets;
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

    const QString defaultSaveTarget =
        context->athlete->home->cache().filePath(
            QStringLiteral("rideDB.json"));
    {
        QMutexLocker locker(&updateMutex);
        reverse_.clear();
        refreshChanged_ = false;
        refreshNotificationActive_ = false;
        isCancelled = false;
        pendingSaveTargets =
            RideCacheSave::takeDeferredTargetsForCancellation(
                pendingSaveTargets_,
                defaultSaveTarget,
                exiting,
                startupLoadFinished_);
    }
    for (const QString &targetPath : pendingSaveTargets) {
        queueBackgroundSave(targetPath);
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

    if (replacementRefreshBlocked_
        && *replacementRefreshBlocked_)
        return;

    if (removalInProgress_ && *removalInProgress_) {
        removalRefreshPending_ = true;
        return;
    }

    bool active = false;
    bool deferStart = false;
    {
        QMutexLocker locker(&updateMutex);
        if (exiting || isCancelled) return;
        refreshGeneration_.request();
        active = refreshGeneration_.hasActive();
        deferStart = saveSnapshotBoundary_;
        if (!active) refreshChanged_ = false;
    }

    if (deferStart) return;
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
        reverse_ = rides();
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
            rides(),
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
    foreach (RideItem *ride, rides()) {

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
        if (!item || deletelist.contains(item)) continue;
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
    foreach (RideItem *ride, rides()) {

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
        rides(),
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
RideCache::checkUnlinkActivity
(RideItem *item)
{
    OperationPreCheck check;

    if (!ownsLiveRide(item)) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The activity is no longer in the activity list");
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
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(item)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }

    RideItem *linkedItem = getLinkedActivity(item);
    if (!linkedItem) {
        result.error = tr("Linked activity not found");
        return result;
    }

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
    if (activityMutationIsBlocked()) {
        batchResult.error = tr(
            "Another activity operation is already in progress");
        return batchResult;
    }
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

    if (!ownsLiveRide(item)) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The activity is no longer in the activity list");
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
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(item)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }

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
        if (!model_->startRemove(index)) {
            result.error = tr(
                "The activity list changed while the renamed activity was being published");
            return result;
        }
        rides_.remove(index, 1);
        model_->endRemove(index);
    }

    item->setFileName((item->planned ? plannedDirectory : directory).canonicalPath(), newFileName);

    if (!model_->beginReset()) {
        result.error = tr(
            "The activity list changed while the renamed activity was being reordered");
        return result;
    }
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

    if (!ownsLiveRide(sourceItem)) {
        check.canProceed = false;
        check.blockingReason = tr(
            "The activity is no longer in the activity list");
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

    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (!ownsLiveRide(sourceItem)) {
        result.error = tr(
            "The activity is no longer in the activity list");
        return result;
    }

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

    if (!model_->beginReset()) {
        delete newItem;
        result.error = tr(
            "The activity list changed while the copied activity was being published");
        return result;
    }
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

        if (!ownsLiveRide(sourceItem)) {
            check.canProceed = false;
            check.blockingReason = tr(
                "A source activity is no longer in the activity list");
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

    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }
    if (sourceItemsAndTargets.isEmpty()) {
        result.error = tr("No files specified");
        return result;
    }
    for (const std::pair<RideItem*, QDate> &pair :
         sourceItemsAndTargets) {
        if (!ownsLiveRide(pair.first)) {
            result.error = tr(
                "A source activity is no longer in the activity list");
            return result;
        }
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
        if (!model_->beginReset()) {
            qDeleteAll(newItems);
            result.error = tr(
                "The activity list changed while copied activities were being published");
            return result;
        }
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
    for (RideItem *item : rides()) {
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
    if (activityMutationIsBlocked()) {
        result.error = tr(
            "Another activity operation is already in progress");
        return result;
    }

    if (dayOffset == 0) {
        result.success = true;
        result.affectedCount = 0;
        return result;
    }
    QList<RideItem*> itemsToShift;
    for (RideItem *item : rides()) {
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
        if (!model_->beginReset()) {
            result.error = tr(
                "The activity list changed while shifted activities were being reordered");
            return result;
        }
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
    if (!item || deletelist.contains(item)) {
        error = QObject::tr(
            "The activity is no longer in the activity list");
        return false;
    }
    const QPointer<RideCache> guardedCache(this);
    return RideCache::saveActivity(
        context, item, error,
        [](Context *saveContext, RideItem *saveItem, QString *saveError) {
            return MainWindow::saveSilent(
                saveContext, saveItem, saveError);
        },
        [guardedCache](RideItem *savedItem) {
            if (guardedCache)
                emit guardedCache->itemSaved(savedItem);
        },
        guardedCache.data());
}


bool
RideCache::saveActivity
(RideItem *item, QString &error,
 const AtomicFileWriterFactory &writerFactory)
{
    if (!item || deletelist.contains(item)) {
        error = QObject::tr(
            "The activity is no longer in the activity list");
        return false;
    }
    ActivitySaveOperations operations;
    operations.writerFactory = writerFactory;
    operations.stage = [](RideFile *ride, QString &stageError) {
        try {
            DataProcessorFactory::instance().autoProcess(
                ride,
                QStringLiteral("Save"),
                QStringLiteral("UPDATE"));
        } catch (const QString &detail) {
            stageError = detail;
            return false;
        } catch (const std::exception &exception) {
            stageError = QString::fromLocal8Bit(
                exception.what());
            return false;
        } catch (...) {
            stageError = QObject::tr(
                "An activity processor failed");
            return false;
        }
        return true;
    };

    const QPointer<RideCache> guardedCache(this);
    return RideCache::saveActivity(
        context, item, error,
        [operations](Context *saveContext,
                     RideItem *saveItem,
                     QString *saveError) {
            return MainWindow::saveSilent(
                saveContext, saveItem, saveError,
                &operations);
        },
        [guardedCache](RideItem *savedItem) {
            if (guardedCache)
                emit guardedCache->itemSaved(savedItem);
        },
        guardedCache.data());
}


bool
RideCache::saveActivity
(Context *context, RideItem *item, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved,
 QObject *operationOwner)
{
    error.clear();
    const bool contextRequired = context != nullptr;
    const QPointer<Context> guardedContext(context);
    const bool ownerRequired = operationOwner != nullptr;
    const QPointer<QObject> guardedOwner(operationOwner);
    QPointer<RideItem> guardedItem(item);
    const auto operationIsAvailable = [&] {
        return (!contextRequired || guardedContext)
            && (!ownerRequired || guardedOwner);
    };
    const auto rejectUnavailableOperation = [&] {
        if (operationIsAvailable()) return false;
        if (error.isEmpty()) {
            error = QObject::tr(
                "The activity collection disappeared while saving");
        }
        return true;
    };

    if (rejectUnavailableOperation()) return false;
    if (!guardedItem) {
        error = QObject::tr("No activity given");
        return false;
    }
    if (!guardedItem->isDirty()) {
        return true;
    }
    if (!save) {
        error = QObject::tr("No activity save operation available");
        return false;
    }
    const bool saved = save(
        guardedContext.data(), guardedItem.data(), &error);
    if (rejectUnavailableOperation()) return false;
    if (!guardedItem) {
        error = QObject::tr(
            "The activity disappeared while it was being saved");
        return false;
    }
    if (!saved) {
        if (error.isEmpty()) {
            error = QObject::tr("The activity could not be saved");
        }
        return false;
    }
    if (notifySaved) {
        const QString savedFileName = guardedItem->fileName;
        const QString savedPath = guardedItem->path;
        const bool savedPlanned = guardedItem->planned;
        notifySaved(guardedItem.data());
        if (rejectUnavailableOperation()) return false;
        if (!guardedItem) {
            error = QObject::tr(
                "The activity disappeared while announcing its save");
            return false;
        }
        if (guardedItem->fileName != savedFileName
            || guardedItem->path != savedPath
            || guardedItem->planned != savedPlanned) {
            error = QObject::tr(
                "The activity identity changed while announcing its save");
            return false;
        }
    }
    return true;
}


bool
RideCache::saveActivities
(QList<RideItem*> items, QString &error)
{
    for (RideItem *item : std::as_const(items)) {
        if (!item || deletelist.contains(item)) {
            error = QObject::tr(
                "An activity is no longer in the activity list");
            return false;
        }
    }
    const QPointer<RideCache> guardedCache(this);
    const LinkedActivitySaveRequirement requirement =
        linkedActivitySaveRequirement(items, error);
    if (requirement == LinkedActivitySaveRequirement::Invalid) {
        return false;
    }
    if (requirement == LinkedActivitySaveRequirement::Required) {
        QList<QPointer<RideItem>> savedItems;
        QSet<RideItem *> seen;
        for (RideItem *item : std::as_const(items)) {
            if (!item || seen.contains(item) || !item->isDirty()) continue;
            seen.insert(item);
            savedItems.append(QPointer<RideItem>(item));
        }
        if (!MainWindow::saveLinkedActivitiesTransaction(
                context,
                context->athlete->home->root().absolutePath(),
                items,
                error,
                ActivitySaveOperationsProvider())) {
            return false;
        }
        for (const QPointer<RideItem> &saved : std::as_const(savedItems)) {
            if (!guardedCache) {
                error = QObject::tr(
                    "The activity collection disappeared while announcing a linked save");
                return false;
            }
            if (!saved) continue;
            const QString fileName = saved->fileName;
            const QString path = saved->path;
            const bool planned = saved->planned;
            QMetaObject::invokeMethod(
                guardedCache.data(), "itemSaved",
                Qt::DirectConnection,
                Q_ARG(RideItem *, saved.data()));
            if (!guardedCache) {
                error = QObject::tr(
                    "The activity collection disappeared while announcing a linked save");
                return false;
            }
            if (!saved) {
                error = QObject::tr(
                    "A linked activity disappeared while announcing its save");
                return false;
            }
            if (saved
                && (saved->fileName != fileName
                    || saved->path != path
                    || saved->planned != planned)) {
                error = QObject::tr(
                    "A linked activity identity changed while announcing its save");
                return false;
            }
        }
        return true;
    }
    return RideCache::saveActivities(
        context, items, error,
        [](Context *saveContext, RideItem *saveItem, QString *saveError) {
            return MainWindow::saveSilent(
                saveContext, saveItem, saveError);
        },
        [guardedCache](RideItem *savedItem) {
            if (guardedCache)
                emit guardedCache->itemSaved(savedItem);
        },
        guardedCache.data());
}


bool
RideCache::saveActivities
(Context *context, const QList<RideItem *> &items, QString &error,
 const SaveActivityFunction &save,
 const ActivitySavedFunction &notifySaved,
 QObject *operationOwner)
{
    error.clear();
    QStringList failed;
    const bool contextRequired = context != nullptr;
    const QPointer<Context> guardedContext(context);
    const bool ownerRequired = operationOwner != nullptr;
    const QPointer<QObject> guardedOwner(operationOwner);
    const auto operationIsAvailable = [&] {
        return (!contextRequired || guardedContext)
            && (!ownerRequired || guardedOwner);
    };
    const auto unavailableFailure = [] {
        return QObject::tr(
            "<activity collection> (collection disappeared while saving)");
    };

    if (!operationIsAvailable()) {
        error = QObject::tr(
            "The activity collection is no longer available");
        return false;
    }

    struct SaveRequest
    {
        QPointer<RideItem> item;
        QString fileName;
        QString path;
        bool planned = false;
    };
    QList<SaveRequest> requests;
    requests.reserve(items.size());
    QSet<RideItem*> requestedItems;
    for (RideItem *item : items) {
        if (!item) {
            requests.append(SaveRequest{});
            continue;
        }
        if (requestedItems.contains(item)) continue;
        requestedItems.insert(item);
        requests.append({
            QPointer<RideItem>(item),
            item->fileName,
            item->path,
            item->planned});
    }

    for (const SaveRequest &request :
         std::as_const(requests)) {
        if (!operationIsAvailable()) {
            failed << unavailableFailure();
            break;
        }
        RideItem *const item =
            request.item.data();
        if (!item
            || item->fileName != request.fileName
            || item->path != request.path
            || item->planned != request.planned) {
            failed << (request.fileName.isEmpty()
                ? QObject::tr("<unknown activity>")
                : QStringLiteral("%1 (%2)")
                    .arg(
                        request.fileName,
                        QObject::tr(
                            "activity changed before it could be saved")));
            continue;
        }
        QString itemError;
        if (!RideCache::saveActivity(
                guardedContext.data(), item, itemError,
                save, notifySaved, guardedOwner.data())) {
            const QString fileName =
                request.fileName.isEmpty()
                ? QObject::tr("<unknown activity>")
                : request.fileName;
            if (itemError.isEmpty()) {
                failed << fileName;
            } else {
                failed << QStringLiteral("%1 (%2)").arg(fileName, itemError);
            }
            if (!operationIsAvailable()) break;
        }
    }
    if (!failed.isEmpty()) {
        error = QObject::tr("Failed to save: %1")
                    .arg(failed.join(QStringLiteral(", ")));
        return false;
    }

    return true;
}


RideItem*
RideCache::getLinkedActivity
(RideItem *item)
{
    if (!ownsLiveRide(item)) {
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
    if (!ownsLiveRide(rideItem)) return nullptr;

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
    if (!item || deletelist.contains(item) || !item->planned) {
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


bool
RideCache::stagePlannedActivityCopy
(const QString &sourcePath,
 const QString &sourceFileName,
 const QDateTime &targetDateTime,
 const QString &stagingPath,
 QString &error)
{
    const QPointer<RideCache> guardedCache(this);
    const QPointer<Context> guardedContext(context);
    return PlannedActivityFile::stageCopy(
        guardedContext.data(), sourcePath,
        sourceFileName, targetDateTime,
        stagingPath,
        [guardedCache, guardedContext]
        (RideFile *ride, const QDateTime &when,
         QString &transformError) {
            if (!guardedCache || !guardedContext
                || guardedCache->context
                    != guardedContext.data()
                || !ride) {
                transformError = QObject::tr(
                    "The planned activity collection disappeared during staging");
                return false;
            }

            const QString workoutFilename = ride->getTag(
                QStringLiteral("WorkoutFilename"),
                QString()).trimmed();
            if (workoutFilename.isEmpty()) return true;

            ErgFile ergFile(
                workoutFilename, ErgFileFormat::unknown,
                guardedContext.data(), when.date());
            if (!guardedCache || !guardedContext
                || guardedCache->context
                    != guardedContext.data()) {
                transformError = QObject::tr(
                    "The planned activity collection disappeared while its workout was being evaluated");
                return false;
            }
            if (!ergFile.hasRelativeWatts()) return true;

            const QList<std::pair<QString, double>> metrics {
                {QStringLiteral("average_power"), ergFile.AP()},
                {QStringLiteral("coggan_np"), ergFile.IsoPower()},
                {QStringLiteral("coggan_tss"), ergFile.bikeStress()},
                {QStringLiteral("skiba_bike_score"), ergFile.BS()},
                {QStringLiteral("skiba_xpower"), ergFile.XP()}
            };
            for (const auto &metric : metrics) {
                auto override =
                    ride->metricOverrides.find(metric.first);
                if (override == ride->metricOverrides.end())
                    continue;

                const int targetValue = static_cast<int>(
                    std::round(metric.second));
                bool currentIsNumber = false;
                const double currentValue =
                    override.value()
                        .value(QStringLiteral("value"))
                        .toDouble(&currentIsNumber);
                if (!currentIsNumber
                    || static_cast<int>(std::round(currentValue))
                        != targetValue) {
                    override.value().insert(
                        QStringLiteral("value"),
                        QString::number(targetValue));
                }
            }
            return true;
        },
        error);
}

bool
RideCache::validatePlannedActivityStage
(const QString &stagingPath,
 const QString &targetFileName,
 QString &error) const
{
    error.clear();
    QDateTime expectedDateTime;
    if (!RideFile::parseRideFileName(
            targetFileName, &expectedDateTime)
        || !context || !context->athlete
        || !context->athlete->home) {
        error = tr(
            "A staged planned activity has an invalid target identity: %1")
                        .arg(targetFileName);
        return false;
    }

    const QFileInfo stagingInfo(stagingPath);
    QFile staged(stagingPath);
    if (stagingInfo.isSymLink() || !stagingInfo.isFile()
        || !staged.open(QIODevice::ReadOnly)) {
        error = tr(
            "A staged planned activity cannot be read: %1")
                        .arg(targetFileName);
        return false;
    }

    QString validationTemplate =
        context->athlete->home->temp().filePath(
            QStringLiteral("gc-plan-validation-XXXXXX"));
    const QString suffix =
        QFileInfo(targetFileName).completeSuffix();
    if (!suffix.isEmpty())
        validationTemplate += QLatin1Char('.') + suffix;
    QTemporaryFile validationFile(validationTemplate);
    if (!validationFile.open()) {
        error = tr(
            "Cannot create a temporary planned activity validation file");
        return false;
    }

    while (!staged.atEnd()) {
        const QByteArray bytes = staged.read(1024 * 1024);
        if (bytes.isEmpty()) {
            if (staged.error() != QFileDevice::NoError) {
                error = staged.errorString();
                return false;
            }
            break;
        }
        if (validationFile.write(bytes) != bytes.size()) {
            error = validationFile.errorString();
            return false;
        }
    }
    if (!validationFile.flush()) {
        error = validationFile.errorString();
        return false;
    }
    validationFile.close();

    QFile candidate(validationFile.fileName());
    QStringList parseErrors;
    RideFile *ride = RideFileFactory::instance().openRideFile(
        context, candidate, parseErrors);
    if (!ride) {
        error = parseErrors.isEmpty()
            ? tr("A staged planned activity is not readable: %1")
                  .arg(targetFileName)
            : parseErrors.join(QStringLiteral("; "));
        return false;
    }

    const QDateTime actualDateTime = ride->startTime();
    delete ride;
    if (actualDateTime.date() != expectedDateTime.date()
        || actualDateTime.time().hour()
            != expectedDateTime.time().hour()
        || actualDateTime.time().minute()
            != expectedDateTime.time().minute()
        || actualDateTime.time().second()
            != expectedDateTime.time().second()) {
        error = tr(
            "A staged planned activity does not match its target date: %1")
                        .arg(targetFileName);
        return false;
    }
    return true;
}

RideFile *
RideCache::openPlannedActivityForDeleteProcessor
(const QString &sourcePath,
 QString &error) const
{
    error.clear();
    const QFileInfo sourceInfo(sourcePath);
    QDateTime expectedDateTime;
    if (sourceInfo.isSymLink() || !sourceInfo.isFile()
        || !RideFile::parseRideFileName(
            sourceInfo.fileName(), &expectedDateTime)) {
        error = tr(
            "A planned activity cannot be opened for delete processing: %1")
                        .arg(sourceInfo.fileName());
        return nullptr;
    }

    QFile source(sourcePath);
    QStringList parseErrors;
    RideFile *ride = RideFileFactory::instance().openRideFile(
        context, source, parseErrors);
    if (!ride) {
        error = parseErrors.isEmpty()
            ? tr("A planned activity is not readable: %1")
                  .arg(sourceInfo.fileName())
            : parseErrors.join(QStringLiteral("; "));
        return nullptr;
    }

    const QDateTime actualDateTime = ride->startTime();
    if (actualDateTime.date() != expectedDateTime.date()
        || actualDateTime.time().hour()
            != expectedDateTime.time().hour()
        || actualDateTime.time().minute()
            != expectedDateTime.time().minute()
        || actualDateTime.time().second()
            != expectedDateTime.time().second()) {
        error = tr(
            "A planned activity does not match its filename date: %1")
                        .arg(sourceInfo.fileName());
        delete ride;
        return nullptr;
    }
    return ride;
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
            const auto disposition =
                RideCacheStartup::refreshResultDisposition(
                    target->refreshGeneration_.accepts(
                        generation));
            item->isstale = disposition.keepStale;
            if (disposition.markCacheChanged) {
                target->refreshChanged_ = true;
            }
        }
    }
}
